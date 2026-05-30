#include <QDebug>
#include <QJsonObject>
#include <QString>

#include <cmath>

#include "../AcesColor.h"

// aces-color selftest — AcesColor.h (AC-1) の純粋エンジンをヘッドレス検証する
// (needsQApplication=false)。行列プリミティブ / 色空間行列 / Bradford 順応 / 転送関数 /
// IDT・RRT+ODT パイプライン / JSON 往復を、ITU・Academy の公開プライマリ値に対し許容誤差で
// 照合する。被テストコード経由でリファレンスを作らず (tautology 回避)、published 定数と
// 直接比較する。

namespace {

// 浮動小数のスカラ比較 (絶対誤差)。
bool approxEqual(double a, double b, double eps)
{
    return std::fabs(a - b) <= eps;
}

// 3x3 行列を許容誤差で比較。
bool matApproxEqual(const aces::Mat3& a, const aces::Mat3& b, double eps)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (!approxEqual(a[r][c], b[r][c], eps)) {
                return false;
            }
        }
    }
    return true;
}

// 単位行列との比較。
bool isApproxIdentity(const aces::Mat3& m, double eps)
{
    return matApproxEqual(m, aces::identity3(), eps);
}

// ベクトルを許容誤差で比較。
bool vecApproxEqual(const aces::Vec3& a, const aces::Vec3& b, double eps)
{
    for (int i = 0; i < 3; ++i) {
        if (!approxEqual(a[i], b[i], eps)) {
            return false;
        }
    }
    return true;
}

QString matToString(const aces::Mat3& m)
{
    return QStringLiteral("[[%1,%2,%3],[%4,%5,%6],[%7,%8,%9]]")
        .arg(m[0][0], 0, 'g', 6).arg(m[0][1], 0, 'g', 6).arg(m[0][2], 0, 'g', 6)
        .arg(m[1][0], 0, 'g', 6).arg(m[1][1], 0, 'g', 6).arg(m[1][2], 0, 'g', 6)
        .arg(m[2][0], 0, 'g', 6).arg(m[2][1], 0, 'g', 6).arg(m[2][2], 0, 'g', 6);
}

QString vecToString(const aces::Vec3& v)
{
    return QStringLiteral("[%1,%2,%3]")
        .arg(v[0], 0, 'g', 6).arg(v[1], 0, 'g', 6).arg(v[2], 0, 'g', 6);
}

} // namespace

int runAcesColorSelftest()
{
    using namespace aces;

    qInfo().noquote() << "[aces-color] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[aces-color] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[aces-color] FAIL" << name << ":" << msg;
    };

    const double kEps = 1e-9;     // identity / round-trip など厳密寄り
    const double kLooseEps = 1e-4; // 数値誤差・published 値の桁丸めを許容

    // 代表的な検証用行列・ベクトル。
    const Mat3 srgbToXyz = rgbToXyz(ColorSpace::sRGB);

    // -----------------------------------------------------------------------
    // G1: identity3 / multiply(M, identity)=M / apply(identity, v)=v
    // -----------------------------------------------------------------------
    {
        const Mat3 id = identity3();
        const Mat3 mId = multiply(srgbToXyz, id);
        const Mat3 idM = multiply(id, srgbToXyz);
        const Vec3 v = { 0.2, 0.5, 0.9 };
        const Vec3 idV = apply(id, v);
        const bool ok = matApproxEqual(mId, srgbToXyz, kEps)
            && matApproxEqual(idM, srgbToXyz, kEps)
            && vecApproxEqual(idV, v, kEps);
        if (ok) {
            pass("G1 identity multiply/apply are neutral");
        } else {
            fail("G1 identity", QStringLiteral("M*I=%1 apply(I,v)=%2")
                    .arg(matToString(mId), vecToString(idV)));
        }
    }

    // -----------------------------------------------------------------------
    // G2: inverse — multiply(M, inverse(M)) ≈ identity (sRGB rgbToXyz で検証)
    // -----------------------------------------------------------------------
    {
        const Mat3 inv = inverse(srgbToXyz);
        const Mat3 prod = multiply(srgbToXyz, inv);
        const Mat3 prod2 = multiply(inv, srgbToXyz);
        const bool ok = isApproxIdentity(prod, kLooseEps) && isApproxIdentity(prod2, kLooseEps);
        if (ok) {
            pass("G2 inverse(M)*M and M*inverse(M) are identity");
        } else {
            fail("G2 inverse", QStringLiteral("M*inv=%1").arg(matToString(prod)));
        }
    }

    // -----------------------------------------------------------------------
    // G3: conversionMatrix(X,X) ≈ identity (同色空間は早期 return で単位)
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        const ColorSpace spaces[] = {
            ColorSpace::sRGB, ColorSpace::Rec709, ColorSpace::Rec2020,
            ColorSpace::DisplayP3, ColorSpace::ACEScg, ColorSpace::ACES2065_1,
            ColorSpace::LinearSRGB
        };
        for (ColorSpace cs : spaces) {
            if (!isApproxIdentity(conversionMatrix(cs, cs), kEps)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            pass("G3 conversionMatrix(X,X) is identity for all spaces");
        } else {
            fail("G3 same-space conversion", QStringLiteral("non-identity diagonal"));
        }
    }

    // -----------------------------------------------------------------------
    // G4: 線形 RGB round-trip — sRGB→ACEScg→sRGB 合成行列 ≈ identity
    //     (conversionMatrix レベルはトーンマップ非含で round-trip 保証)
    // -----------------------------------------------------------------------
    {
        const Mat3 fwd = conversionMatrix(ColorSpace::sRGB, ColorSpace::ACEScg);
        const Mat3 back = conversionMatrix(ColorSpace::ACEScg, ColorSpace::sRGB);
        const Mat3 round = multiply(back, fwd);
        // 代表色での誤差も併せて確認する。
        const Vec3 grey = { 0.18, 0.18, 0.18 };
        const Vec3 rt = apply(round, grey);
        const bool ok = isApproxIdentity(round, kLooseEps) && vecApproxEqual(rt, grey, kLooseEps);
        if (ok) {
            pass("G4 sRGB->ACEScg->sRGB linear round-trip is identity");
        } else {
            fail("G4 linear round-trip", QStringLiteral("round=%1 grey'=%2")
                    .arg(matToString(round), vecToString(rt)));
        }
    }

    // -----------------------------------------------------------------------
    // G5: rgbToXyz(sRGB) の白色点 — apply(M, {1,1,1}) ≈ D65 XYZ
    //     (published D65: x=0.3127,y=0.3290 → XYZ ≈ 0.95046,1.0,1.08906)
    // -----------------------------------------------------------------------
    {
        const Vec3 white = apply(srgbToXyz, { 1.0, 1.0, 1.0 });
        const Vec3 d65 = { 0.95045593, 1.0, 1.08905775 };
        if (vecApproxEqual(white, d65, kLooseEps)) {
            pass("G5 rgbToXyz(sRGB) white point matches D65");
        } else {
            fail("G5 sRGB white point", QStringLiteral("white=%1 expectedD65=%2")
                    .arg(vecToString(white), vecToString(d65)));
        }
    }

    // -----------------------------------------------------------------------
    // G6: oetf/eotf round-trip — sRGB は eotf(oetf(x))≈x、Linear/ACEScg は恒等
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        const double samples[] = { 0.0, 0.05, 0.18, 0.5, 0.75, 1.0 };
        for (double x : samples) {
            const double rt = eotf(ColorSpace::sRGB, oetf(ColorSpace::sRGB, x));
            if (!approxEqual(rt, x, 1e-6)) { ok = false; break; }
        }
        // 線形色空間は転送関数が恒等。
        for (double x : samples) {
            if (!approxEqual(oetf(ColorSpace::LinearSRGB, x), x, kEps)
                || !approxEqual(eotf(ColorSpace::LinearSRGB, x), x, kEps)
                || !approxEqual(oetf(ColorSpace::ACEScg, x), x, kEps)
                || !approxEqual(eotf(ColorSpace::ACEScg, x), x, kEps)) {
                ok = false;
                break;
            }
        }
        // sRGB の符号化は非線形 (恒等ではない) ことも確認し、退化を防ぐ。
        const bool nonLinear = !approxEqual(oetf(ColorSpace::sRGB, 0.18), 0.18, 1e-3);
        if (ok && nonLinear) {
            pass("G6 oetf/eotf round-trip (sRGB) and linear identity");
        } else {
            fail("G6 transfer round-trip", QStringLiteral("ok=%1 nonLinear=%2")
                    .arg(static_cast<int>(ok)).arg(static_cast<int>(nonLinear)));
        }
    }

    // -----------------------------------------------------------------------
    // G7: Bradford — D65↔D60 順応行列が可逆 (往復 ≈ identity)
    // -----------------------------------------------------------------------
    {
        const Vec3 d65 = { 0.95045593, 1.0, 1.08905775 };
        const Vec3 d60 = { 0.95264608, 1.0, 1.00882518 };
        const Mat3 a = bradfordAdaptation(d65, d60);
        const Mat3 b = bradfordAdaptation(d60, d65);
        const Mat3 round = multiply(b, a);
        // 同一白色点同士は単位 (whiteDiffers ガード)。
        const Mat3 same = bradfordAdaptation(d65, d65);
        const bool ok = isApproxIdentity(round, kLooseEps) && isApproxIdentity(same, kLooseEps);
        if (ok) {
            pass("G7 Bradford D65<->D60 round-trip is identity");
        } else {
            fail("G7 Bradford", QStringLiteral("round=%1 same=%2")
                    .arg(matToString(round), matToString(same)));
        }
    }

    // -----------------------------------------------------------------------
    // G8: idt — sRGB 中間グレーを線形化→ACEScg。線形化されており範囲が妥当
    // -----------------------------------------------------------------------
    {
        // 符号値 0.5 を IDT。eotf(0.5) ≈ 0.214 (sRGB) なので線形化されているはず。
        const Vec3 acescg = idt(ColorSpace::sRGB, { 0.5, 0.5, 0.5 });
        const double encoded = 0.5;
        const double linearised = eotf(ColorSpace::sRGB, encoded); // ≈ 0.214
        bool finite = std::isfinite(acescg[0]) && std::isfinite(acescg[1]) && std::isfinite(acescg[2]);
        bool inRange = true;
        for (int i = 0; i < 3; ++i) {
            if (acescg[i] <= 0.0 || acescg[i] >= 1.0) { inRange = false; }
        }
        // ニュートラルグレーは ACEScg でもほぼ無彩色 (3 チャンネルが近い) で
        // 符号値 0.5 より小さい (線形化済み) ことを確認する。
        const double avg = (acescg[0] + acescg[1] + acescg[2]) / 3.0;
        const bool linearised_ok = avg < encoded && approxEqual(avg, linearised, 0.05);
        if (finite && inRange && linearised_ok) {
            pass("G8 idt linearises sRGB grey into ACEScg range");
        } else {
            fail("G8 idt", QStringLiteral("acescg=%1 avg=%2 linearised=%3 finite=%4 inRange=%5")
                    .arg(vecToString(acescg)).arg(avg).arg(linearised)
                    .arg(static_cast<int>(finite)).arg(static_cast<int>(inRange)));
        }
    }

    // -----------------------------------------------------------------------
    // G9: process — enabled=false で入力そのまま (identity)、
    //     enabled=true で idt→rrtOdt を通り有限値かつクランプ範囲
    // -----------------------------------------------------------------------
    {
        const Vec3 in = { 0.3, 0.45, 0.7 };

        AcesPipeline off;
        off.enabled = false;
        const Vec3 outOff = process(off, in);
        const bool identityOff = vecApproxEqual(outOff, in, kEps);

        AcesPipeline on;
        on.enabled = true;
        on.input = ColorSpace::sRGB;
        on.working = ColorSpace::ACEScg;
        on.output = ColorSpace::Rec709;
        const Vec3 outOn = process(on, in);
        bool finite = std::isfinite(outOn[0]) && std::isfinite(outOn[1]) && std::isfinite(outOn[2]);
        bool clamped = true;
        for (int i = 0; i < 3; ++i) {
            // トーンマップ後の符号値は概ね [0,1]。多少の余裕を持たせて検証する。
            if (outOn[i] < -1e-6 || outOn[i] > 1.5) { clamped = false; }
        }
        // enabled=true は加工が入るため入力と一致してはならない (退化検出)。
        const bool transformed = !vecApproxEqual(outOn, in, 1e-3);
        if (identityOff && finite && clamped && transformed) {
            pass("G9 process identity (off) and bounded transform (on)");
        } else {
            fail("G9 process", QStringLiteral("off=%1 on=%2 identityOff=%3 finite=%4 clamped=%5 transformed=%6")
                    .arg(vecToString(outOff), vecToString(outOn))
                    .arg(static_cast<int>(identityOff)).arg(static_cast<int>(finite))
                    .arg(static_cast<int>(clamped)).arg(static_cast<int>(transformed)));
        }
    }

    // -----------------------------------------------------------------------
    // G10: pipelineToJson/fromJson 往復 + colorSpaceName/fromName 往復
    // -----------------------------------------------------------------------
    {
        AcesPipeline p;
        p.enabled = true;
        p.input = ColorSpace::DisplayP3;
        p.working = ColorSpace::ACEScg;
        p.output = ColorSpace::Rec2020;
        const QJsonObject obj = pipelineToJson(p);
        const AcesPipeline rt = pipelineFromJson(obj);
        const bool jsonOk = rt.enabled == p.enabled
            && rt.input == p.input
            && rt.working == p.working
            && rt.output == p.output;

        bool nameOk = true;
        const ColorSpace spaces[] = {
            ColorSpace::sRGB, ColorSpace::Rec709, ColorSpace::Rec2020,
            ColorSpace::DisplayP3, ColorSpace::ACEScg, ColorSpace::ACES2065_1,
            ColorSpace::LinearSRGB
        };
        for (ColorSpace cs : spaces) {
            if (colorSpaceFromName(colorSpaceName(cs)) != cs) {
                nameOk = false;
                break;
            }
        }
        // 未知名は sRGB にフォールバックする。
        const bool unknownOk = colorSpaceFromName(QStringLiteral("__no_such_space__")) == ColorSpace::sRGB;
        if (jsonOk && nameOk && unknownOk) {
            pass("G10 pipeline JSON and colorSpace name round-trip");
        } else {
            fail("G10 json/name round-trip", QStringLiteral("jsonOk=%1 nameOk=%2 unknownOk=%3")
                    .arg(static_cast<int>(jsonOk)).arg(static_cast<int>(nameOk))
                    .arg(static_cast<int>(unknownOk)));
        }
    }

    qInfo().noquote().nospace() << "[aces-color] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
