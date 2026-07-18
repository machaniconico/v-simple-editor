#include <QDebug>
#include <QImage>
#include <QString>

#include <cstdint>
#include <vector>

#include "../AutoMatte.h"

// AutoMatte (AM-1) の純粋エンジン単体テスト (AM-2)。
// QApplication 不要 (QImage は QApp 無しで利用可、namespace automatte は純粋関数)。
// 小さな合成 RGBA 画像を手で組み立てて differenceMatte / autoSegment /
// モルフォロジ / feather / refine / composite / applyMatteAsAlpha と境界堅牢性を検証する。
// 流儀は command_search_selftest.cpp に倣う ([auto-matte] プレフィックス, PASS/FAIL 集計)。

namespace {

// 単色 RGBA8 バッファ (長さ w*h*4) を作る。
std::vector<uint8_t> solidRgba(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = r;
        buf[i + 1] = g;
        buf[i + 2] = b;
        buf[i + 3] = a;
    }
    return buf;
}

// 1 ピクセルの RGB を上書き (RGBA8 バッファ, alpha は 255 のまま)。
void setRgb(std::vector<uint8_t>& buf, int w, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
    buf[idx + 0] = r;
    buf[idx + 1] = g;
    buf[idx + 2] = b;
    buf[idx + 3] = 255;
}

// マットの前景画素数 (>0 を前景とみなす)。
int foregroundCount(const std::vector<uint8_t>& matte)
{
    int n = 0;
    for (uint8_t v : matte) {
        if (v > 0) {
            ++n;
        }
    }
    return n;
}

} // namespace

int runAutoMatteSelftest()
{
    qInfo().noquote() << "[auto-matte] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[auto-matte] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[auto-matte] FAIL" << name << ":" << msg; };

    using namespace automatte;

    const int w = 8, h = 8;

    // G1: differenceMatte — 同一なら全背景、明確に異なる領域は前景。
    {
        std::vector<uint8_t> plate = solidRgba(w, h, 10, 20, 30);
        std::vector<uint8_t> fgSame = plate; // 完全一致
        const std::vector<uint8_t> matteSame = differenceMatte(fgSame, plate, w, h, 0.25);
        const bool sameOk = matteSame.size() == static_cast<size_t>(w) * h
            && foregroundCount(matteSame) == 0;

        std::vector<uint8_t> fgDiff = plate;
        // 中央 2x2 を大きく異なる色 (白) にする。
        setRgb(fgDiff, w, 3, 3, 255, 255, 255);
        setRgb(fgDiff, w, 4, 3, 255, 255, 255);
        setRgb(fgDiff, w, 3, 4, 255, 255, 255);
        setRgb(fgDiff, w, 4, 4, 255, 255, 255);
        const std::vector<uint8_t> matteDiff = differenceMatte(fgDiff, plate, w, h, 0.25);
        const bool diffOk = matteDiff.size() == static_cast<size_t>(w) * h
            && matteDiff[(3 * w + 3)] == 255
            && matteDiff[(4 * w + 4)] == 255
            && matteDiff[0] == 0; // 角 (未変更) は背景

        if (sameOk && diffOk) {
            pass("G1 differenceMatte: identical->all bg, distinct region->fg");
        } else {
            fail("G1 differenceMatte", QStringLiteral("sameFg=%1 diffFgCenter=%2 corner=%3")
                    .arg(foregroundCount(matteSame))
                    .arg(matteDiff.empty() ? -1 : matteDiff[(3 * w + 3)])
                    .arg(matteDiff.empty() ? -1 : matteDiff[0]));
        }
    }

    // G2: differenceMatte threshold — しきい値で切り分けが変わる。
    {
        std::vector<uint8_t> plate = solidRgba(w, h, 0, 0, 0);
        std::vector<uint8_t> fg = plate;
        // 中央 1 画素を中程度のグレー (128) にする → 正規化距離はおよそ 0.29。
        setRgb(fg, w, 4, 4, 128, 128, 128);
        const std::vector<uint8_t> matteLow = differenceMatte(fg, plate, w, h, 0.10);  // 低しきい値 → 前景
        const std::vector<uint8_t> matteHigh = differenceMatte(fg, plate, w, h, 0.90); // 高しきい値 → 背景
        const bool ok = !matteLow.empty() && !matteHigh.empty()
            && matteLow[(4 * w + 4)] == 255
            && matteHigh[(4 * w + 4)] == 0;
        if (ok) {
            pass("G2 differenceMatte: threshold flips fg/bg classification");
        } else {
            fail("G2 differenceMatte threshold", QStringLiteral("low=%1 high=%2")
                    .arg(matteLow.empty() ? -1 : matteLow[(4 * w + 4)])
                    .arg(matteHigh.empty() ? -1 : matteHigh[(4 * w + 4)]));
        }
    }

    // G3: autoSegment — 四隅が背景色・中央が別色なら中央前景 / 四隅背景。
    {
        std::vector<uint8_t> img = solidRgba(w, h, 0, 0, 0); // 背景は黒 (四隅もこの色)
        // 中央 4x4 を白い被写体にする。
        for (int y = 2; y < 6; ++y) {
            for (int x = 2; x < 6; ++x) {
                setRgb(img, w, x, y, 255, 255, 255);
            }
        }
        const std::vector<uint8_t> matte = autoSegment(img, w, h, 0.25);
        const std::vector<uint8_t> uniform = autoSegment(solidRgba(w, h, 12, 34, 56), w, h, 0.0);
        const bool ok = matte.size() == static_cast<size_t>(w) * h
            && matte[(4 * w + 4)] == 255            // 中央 = 前景
            && matte[0] == 0                        // 左上角 = 背景
            && matte[(0 * w + (w - 1))] == 0        // 右上角 = 背景
            && matte[((h - 1) * w + 0)] == 0        // 左下角 = 背景
            && matte[((h - 1) * w + (w - 1))] == 0  // 右下角 = 背景
            && uniform.size() == static_cast<size_t>(w) * h
            && foregroundCount(uniform) == 0;       // 中央プライアだけで背景を前景化しない
        if (ok) {
            pass("G3 autoSegment: center fg, four corners bg");
        } else {
            fail("G3 autoSegment", QStringLiteral("center=%1 corners=%2,%3,%4,%5 uniformFg=%6")
                    .arg(matte.empty() ? -1 : matte[(4 * w + 4)])
                    .arg(matte.empty() ? -1 : matte[0])
                    .arg(matte.empty() ? -1 : matte[(w - 1)])
                    .arg(matte.empty() ? -1 : matte[((h - 1) * w)])
                    .arg(matte.empty() ? -1 : matte[((h - 1) * w + (w - 1))])
                    .arg(uniform.empty() ? -1 : foregroundCount(uniform)));
        }
    }

    // 以降のモルフォロジ用に、中央 4x4 が前景の二値マットを用意する。
    auto makeBlockMatte = [&]() {
        std::vector<uint8_t> m(static_cast<size_t>(w) * h, 0);
        for (int y = 2; y < 6; ++y) {
            for (int x = 2; x < 6; ++x) {
                m[static_cast<size_t>(y) * w + x] = 255;
            }
        }
        return m;
    };

    // G4: erodeMatte — 前景塊が縮む (前景画素数が減る)。
    {
        const std::vector<uint8_t> block = makeBlockMatte();
        const std::vector<uint8_t> eroded = erodeMatte(block, w, h, 1);
        const bool ok = eroded.size() == static_cast<size_t>(w) * h
            && foregroundCount(eroded) < foregroundCount(block)
            && foregroundCount(eroded) > 0; // 完全消滅はしない (4x4 を radius1 で侵食 → 2x2)
        if (ok) {
            pass("G4 erodeMatte: foreground block shrinks");
        } else {
            fail("G4 erodeMatte", QStringLiteral("before=%1 after=%2")
                    .arg(foregroundCount(block))
                    .arg(eroded.empty() ? -1 : foregroundCount(eroded)));
        }
    }

    // G5: dilateMatte — 前景塊が広がる (前景画素数が増える)。
    {
        const std::vector<uint8_t> block = makeBlockMatte();
        const std::vector<uint8_t> dilated = dilateMatte(block, w, h, 1);
        const bool ok = dilated.size() == static_cast<size_t>(w) * h
            && foregroundCount(dilated) > foregroundCount(block);
        if (ok) {
            pass("G5 dilateMatte: foreground block grows");
        } else {
            fail("G5 dilateMatte", QStringLiteral("before=%1 after=%2")
                    .arg(foregroundCount(block))
                    .arg(dilated.empty() ? -1 : foregroundCount(dilated)));
        }
    }

    // G6: featherMatte — 鋭い境界が 0/255 以外の中間値を持つソフト境界になる。
    {
        const std::vector<uint8_t> block = makeBlockMatte();
        const std::vector<uint8_t> feathered = featherMatte(block, w, h, 1);
        bool hasMid = false;
        for (uint8_t v : feathered) {
            if (v > 0 && v < 255) {
                hasMid = true;
                break;
            }
        }
        const bool ok = feathered.size() == static_cast<size_t>(w) * h && hasMid;
        if (ok) {
            pass("G6 featherMatte: hard edge becomes soft (mid-alpha) values");
        } else {
            fail("G6 featherMatte", QStringLiteral("size=%1 hasMid=%2")
                    .arg(feathered.size())
                    .arg(hasMid ? 1 : 0));
        }
    }

    // G7: refineMatte — erode->dilate->feather の合成が妥当 (サイズ w*h、値域 0..255)。
    {
        const std::vector<uint8_t> block = makeBlockMatte();
        MatteParams params;
        params.erode = 1;
        params.dilate = 1;
        params.featherRadius = 1;
        const std::vector<uint8_t> refined = refineMatte(block, w, h, params);
        // uint8_t は本質的に 0..255。出力長と「何らかの前景が残る」ことを検証する。
        const bool ok = refined.size() == static_cast<size_t>(w) * h
            && foregroundCount(refined) > 0;
        if (ok) {
            pass("G7 refineMatte: erode->dilate->feather yields valid w*h matte");
        } else {
            fail("G7 refineMatte", QStringLiteral("size=%1 fg=%2")
                    .arg(refined.size())
                    .arg(refined.empty() ? -1 : foregroundCount(refined)));
        }
    }

    // G8: composite — matte=255 は fg 色、matte=0 は bg 色、中間は線形補間。
    {
        std::vector<uint8_t> fg = solidRgba(w, h, 200, 0, 0); // 赤
        std::vector<uint8_t> bg = solidRgba(w, h, 0, 0, 100);  // 青
        std::vector<uint8_t> matte(static_cast<size_t>(w) * h, 0);
        matte[0] = 255;        // 画素0: 完全前景
        matte[1] = 0;          // 画素1: 完全背景
        matte[2] = 128;        // 画素2: 中間 (約半々)
        const std::vector<uint8_t> out = composite(fg, bg, matte, w, h);
        const bool sizeOk = out.size() == static_cast<size_t>(w) * h * 4;
        // 画素0 = fg (赤)。
        const bool p0 = sizeOk && out[0] == 200 && out[1] == 0 && out[2] == 0 && out[3] == 255;
        // 画素1 = bg (青)。
        const bool p1 = sizeOk && out[4] == 0 && out[5] == 0 && out[6] == 100 && out[7] == 255;
        // 画素2 = 中間。R は fg(200) と bg(0) の間、B は bg(100) と fg(0) の間。
        const bool p2 = sizeOk
            && out[8] > 0 && out[8] < 200      // R: 中間
            && out[10] > 0 && out[10] < 100;   // B: 中間
        if (p0 && p1 && p2) {
            pass("G8 composite: matte 255->fg, 0->bg, mid->linear blend");
        } else {
            fail("G8 composite", QStringLiteral("p0=%1 p1=%2 p2(R=%3,B=%4)")
                    .arg(p0 ? 1 : 0)
                    .arg(p1 ? 1 : 0)
                    .arg(sizeOk ? out[8] : -1)
                    .arg(sizeOk ? out[10] : -1));
        }
    }

    // G9: applyMatteAsAlpha — 出力 A チャンネルが matte と一致、RGB 保持。
    {
        std::vector<uint8_t> rgba = solidRgba(w, h, 50, 100, 150, 255);
        std::vector<uint8_t> matte(static_cast<size_t>(w) * h, 0);
        matte[0] = 255;
        matte[1] = 77;
        matte[2] = 0;
        const std::vector<uint8_t> out = applyMatteAsAlpha(rgba, matte, w, h);
        const bool sizeOk = out.size() == static_cast<size_t>(w) * h * 4;
        const bool rgbKept = sizeOk
            && out[0] == 50 && out[1] == 100 && out[2] == 150
            && out[4] == 50 && out[5] == 100 && out[6] == 150;
        const bool alphaOk = sizeOk
            && out[3] == 255   // 画素0 alpha
            && out[7] == 77    // 画素1 alpha
            && out[11] == 0;   // 画素2 alpha
        if (rgbKept && alphaOk) {
            pass("G9 applyMatteAsAlpha: A replaced by matte, RGB preserved");
        } else {
            fail("G9 applyMatteAsAlpha", QStringLiteral("rgbKept=%1 a0=%2 a1=%3 a2=%4")
                    .arg(rgbKept ? 1 : 0)
                    .arg(sizeOk ? out[3] : -1)
                    .arg(sizeOk ? out[7] : -1)
                    .arg(sizeOk ? out[11] : -1));
        }
    }

    // G10: 境界 / 堅牢 — 空入力・サイズ不整合で安全 (クラッシュせず空 or 入力返し)。
    {
        const std::vector<uint8_t> empty;
        // differenceMatte: 空入力 → 空。
        const bool d1 = differenceMatte(empty, empty, w, h, 0.25).empty();
        // differenceMatte: サイズ不整合 (fg と plate の長さが違う) → 空。
        std::vector<uint8_t> fgFull = solidRgba(w, h, 1, 2, 3);
        std::vector<uint8_t> plateShort(4, 0);
        const bool d2 = differenceMatte(fgFull, plateShort, w, h, 0.25).empty();
        // autoSegment: 空入力 → 空。
        const bool d3 = autoSegment(empty, w, h, 0.25).empty();
        // erode/dilate: 空マット → 空。
        const bool d4 = erodeMatte(empty, w, h, 1).empty();
        const bool d5 = dilateMatte(empty, w, h, 1).empty();
        // composite: fg と bg のサイズ不整合 → 空。
        std::vector<uint8_t> bgShort(4, 0);
        std::vector<uint8_t> matteFull(static_cast<size_t>(w) * h, 255);
        const bool d6 = composite(fgFull, bgShort, matteFull, w, h).empty();
        // applyMatteAsAlpha: 不正入力 → 入力そのまま返し (空でない fgFull が返る)。
        std::vector<uint8_t> matteShort(2, 255);
        const std::vector<uint8_t> appliedBad = applyMatteAsAlpha(fgFull, matteShort, w, h);
        const bool d7 = appliedBad.size() == fgFull.size();
        if (d1 && d2 && d3 && d4 && d5 && d6 && d7) {
            pass("G10 robustness: empty/size-mismatch handled safely");
        } else {
            fail("G10 robustness", QStringLiteral("d1=%1 d2=%2 d3=%3 d4=%4 d5=%5 d6=%6 d7=%7")
                    .arg(d1).arg(d2).arg(d3).arg(d4).arg(d5).arg(d6).arg(d7));
        }
    }

    // G11: radius<=0 は入力をそのまま返す (erode/dilate/feather のドキュメント契約)。
    {
        const std::vector<uint8_t> block = makeBlockMatte();
        const bool e0 = erodeMatte(block, w, h, 0) == block;
        const bool d0 = dilateMatte(block, w, h, 0) == block;
        const bool f0 = featherMatte(block, w, h, 0) == block;
        if (e0 && d0 && f0) {
            pass("G11 radius<=0 returns input unchanged");
        } else {
            fail("G11 radius<=0 identity", QStringLiteral("erode=%1 dilate=%2 feather=%3")
                    .arg(e0).arg(d0).arg(f0));
        }
    }

    // G12: QImage 往復変換 (rgbaToQImage / qimageToRgba) が値を保持する。
    //      QApplication 不要 (QImage は QApp 無しで構築可能)。
    {
        std::vector<uint8_t> rgba = solidRgba(w, h, 12, 34, 56, 200);
        setRgb(rgba, w, 1, 1, 210, 110, 10); // alpha は setRgb で 255 になる
        const QImage img = rgbaToQImage(rgba, w, h);
        const bool imgOk = !img.isNull()
            && img.width() == w && img.height() == h;
        const std::vector<uint8_t> back = qimageToRgba(img);
        const bool roundTrip = imgOk && back.size() == rgba.size() && back == rgba;
        if (roundTrip) {
            pass("G12 rgbaToQImage/qimageToRgba round-trip preserves pixels");
        } else {
            fail("G12 QImage round-trip", QStringLiteral("imgNull=%1 backSize=%2 expectSize=%3 equal=%4")
                    .arg(img.isNull() ? 1 : 0)
                    .arg(back.size())
                    .arg(rgba.size())
                    .arg((back == rgba) ? 1 : 0));
        }
    }

    qInfo().noquote().nospace() << "[auto-matte] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
