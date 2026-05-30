// trim_ops_selftest.cpp — TrimOps 純粋エンジン (TR-1) の headless 単体テスト。
// QApplication 不要 (needsQApplication=false)。
//
// 各トリム種別 (RippleIn / RippleOut / Roll / Slip / Slide) について、成功時の
// クリップ状態 (in/out/leadIn/実尺) と境界違反時の false + clips 不変を検証する。
// speed!=1.0 の source/timeline 換算も確認する。

#include <QDebug>
#include <QString>
#include <QVector>
#include <cmath>

#include "../TrimOps.h"
#include "../Timeline.h"   // ClipInfo

namespace {

using trimops::TrimType;

// 指定の絶対開始秒 (leadIn) と inPoint/outPoint/speed を持つクリップを作る。
ClipInfo makeClip(double leadInSec, double inPoint, double outPoint,
                  double duration, double speed, const QString &name)
{
    ClipInfo c;
    c.filePath = QStringLiteral("/clips/") + name + QStringLiteral(".mp4");
    c.displayName = name;
    c.duration = duration;
    c.inPoint = inPoint;
    c.outPoint = outPoint;
    c.leadInSec = leadInSec;
    c.speed = speed;
    return c;
}

bool approx(double a, double b) { return std::fabs(a - b) < 1e-4; }

// clips の絶対開始秒列 (TrimOps.h と同じ leadInSec モデル)。
QVector<double> starts(const QVector<ClipInfo> &clips)
{
    QVector<double> s;
    double cursor = 0.0;
    for (const ClipInfo &c : clips) {
        const double st = cursor + c.leadInSec;
        s.append(st);
        cursor = st + c.effectiveDuration();
    }
    return s;
}

} // namespace

int runTrimOpsSelftest()
{
    qInfo().noquote() << "[trim-ops] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char *name) { ++passed; qInfo().noquote() << "[trim-ops] PASS" << name; };
    auto fail = [&](const char *name, const QString &msg) {
        ++failed; qWarning().noquote() << "[trim-ops] FAIL" << name << ":" << msg;
    };

    // G1: RippleIn 正方向 — inPoint↑、leadIn 不変、実尺↓、下流は -delta シフト
    // (ギャップ無し ripple)。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 0.0, 10.0, 10.0, 1.0, "a")
              << makeClip(0.0, 0.0, 6.0, 6.0, 1.0, "b");
        const double startB0 = starts(clips)[1];
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::RippleIn, 2.0, &err);
        const double startB1 = starts(clips)[1];
        const bool good = ok && approx(clips[0].inPoint, 2.0)
                       && approx(clips[0].leadInSec, 0.0)
                       && approx(clips[0].effectiveDuration(), 8.0)
                       // 実尺が 2s 縮んだ分、下流クリップは -2s シフト (ギャップ無し)。
                       && approx(startB1 - startB0, -2.0);
        good ? pass("G1 RippleIn+") : fail("G1 RippleIn+",
              QStringLiteral("ok=%1 in=%2 lead=%3 dB=%4 err=%5")
                  .arg(ok).arg(clips[0].inPoint).arg(clips[0].leadInSec)
                  .arg(startB1 - startB0).arg(err));
    }

    // G2: RippleOut 正方向 — outPoint↑、実尺↑、下流 leadIn↑ で +delta シフト。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 0.0, 6.0, 10.0, 1.0, "a")
              << makeClip(0.0, 0.0, 5.0, 5.0, 1.0, "b");
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::RippleOut, 2.0, &err);
        const bool good = ok && approx(clips[0].outPoint, 8.0)
                       && approx(clips[0].effectiveDuration(), 8.0)
                       && approx(clips[1].leadInSec, 2.0);
        good ? pass("G2 RippleOut+") : fail("G2 RippleOut+",
              QStringLiteral("ok=%1 out=%2 leadNext=%3 err=%4")
                  .arg(ok).arg(clips[0].outPoint).arg(clips[1].leadInSec).arg(err));
    }

    // G3: RippleOut 境界違反 — 素材末尾 (duration) を超える delta は false + 不変。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 0.0, 9.0, 10.0, 1.0, "a")
              << makeClip(0.0, 0.0, 5.0, 5.0, 1.0, "b");
        const QVector<ClipInfo> before = clips;
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::RippleOut, 5.0, &err);
        const bool good = !ok && !err.isEmpty()
                       && approx(clips[0].outPoint, before[0].outPoint)
                       && approx(clips[1].leadInSec, before[1].leadInSec);
        good ? pass("G3 RippleOut bound") : fail("G3 RippleOut bound",
              QStringLiteral("ok=%1 out=%2 err=%3").arg(ok).arg(clips[0].outPoint).arg(err));
    }

    // G4: Roll — 編集点が +delta、a.outPoint↑/b.inPoint↑、総尺・下流位置不変。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 0.0, 5.0, 10.0, 1.0, "a")
              << makeClip(0.0, 2.0, 8.0, 10.0, 1.0, "b");
        const double totalBefore = clips[0].effectiveDuration() + clips[1].effectiveDuration();
        const double startBBefore = starts(clips)[1];
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::Roll, 1.5, &err);
        const double totalAfter = clips[0].effectiveDuration() + clips[1].effectiveDuration();
        const double startBAfter = starts(clips)[1];
        const bool good = ok && approx(clips[0].outPoint, 6.5)
                       && approx(clips[1].inPoint, 3.5)
                       && approx(totalBefore, totalAfter)
                       // 編集点 (= b の開始秒) が +1.5s 動く。
                       && approx(startBAfter - startBBefore, 1.5);
        good ? pass("G4 Roll") : fail("G4 Roll",
              QStringLiteral("ok=%1 outA=%2 inB=%3 dEdit=%4 err=%5")
                  .arg(ok).arg(clips[0].outPoint).arg(clips[1].inPoint)
                  .arg(startBAfter - startBBefore).arg(err));
    }

    // G5: Slip — in/out 同 delta、タイムライン位置・実尺不変。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(1.0, 2.0, 7.0, 10.0, 1.0, "a");
        const double startBefore = starts(clips)[0];
        const double durBefore = clips[0].effectiveDuration();
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::Slip, 1.0, &err);
        const double startAfter = starts(clips)[0];
        const bool good = ok && approx(clips[0].inPoint, 3.0)
                       && approx(clips[0].outPoint, 8.0)
                       && approx(startAfter, startBefore)
                       && approx(clips[0].effectiveDuration(), durBefore);
        good ? pass("G5 Slip") : fail("G5 Slip",
              QStringLiteral("ok=%1 in=%2 out=%3 dStart=%4 err=%5")
                  .arg(ok).arg(clips[0].inPoint).arg(clips[0].outPoint)
                  .arg(startAfter - startBefore).arg(err));
    }

    // G6: Slip 境界違反 — outPoint+delta が duration を超えると false + 不変。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 2.0, 9.0, 10.0, 1.0, "a");
        const QVector<ClipInfo> before = clips;
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::Slip, 2.0, &err);
        const bool good = !ok && !err.isEmpty()
                       && approx(clips[0].inPoint, before[0].inPoint)
                       && approx(clips[0].outPoint, before[0].outPoint);
        good ? pass("G6 Slip bound") : fail("G6 Slip bound",
              QStringLiteral("ok=%1 err=%2").arg(ok).arg(err));
    }

    // G7: Slide (中央クリップ) — clip[i] の source/実尺不変、prev.outPoint↑ で
    // 吸収、next.inPoint↑ で吸収、clip[i] の開始秒が +delta。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 0.0, 5.0, 10.0, 1.0, "p")
              << makeClip(0.0, 2.0, 6.0, 10.0, 1.0, "c")
              << makeClip(0.0, 1.0, 6.0, 10.0, 1.0, "n");
        const double inC = clips[1].inPoint, outC = clips[1].outPoint;
        const double durC = clips[1].effectiveDuration();
        const double startCBefore = starts(clips)[1];
        QString err;
        const bool ok = trimops::applyTrim(clips, 1, TrimType::Slide, 1.0, &err);
        const double startCAfter = starts(clips)[1];
        const bool good = ok
                       && approx(clips[1].inPoint, inC) && approx(clips[1].outPoint, outC)
                       && approx(clips[1].effectiveDuration(), durC)
                       && approx(clips[0].outPoint, 6.0)   // prev 吸収 +1
                       && approx(clips[2].inPoint, 2.0)     // next 吸収 +1
                       && approx(startCAfter - startCBefore, 1.0);
        good ? pass("G7 Slide mid") : fail("G7 Slide mid",
              QStringLiteral("ok=%1 inC=%2 outC=%3 outP=%4 inN=%5 dC=%6 err=%7")
                  .arg(ok).arg(clips[1].inPoint).arg(clips[1].outPoint)
                  .arg(clips[0].outPoint).arg(clips[2].inPoint)
                  .arg(startCAfter - startCBefore).arg(err));
    }

    // G8: Slide (先頭クリップ) — prev が無いので leadInSec で吸収。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(2.0, 0.0, 5.0, 10.0, 1.0, "c")
              << makeClip(0.0, 1.0, 6.0, 10.0, 1.0, "n");
        const double inC = clips[0].inPoint, outC = clips[0].outPoint;
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::Slide, -1.0, &err);
        const bool good = ok && approx(clips[0].inPoint, inC) && approx(clips[0].outPoint, outC)
                       && approx(clips[0].leadInSec, 1.0)   // 2.0 - 1.0
                       && approx(clips[1].inPoint, 0.0);     // next 吸収 -1
        good ? pass("G8 Slide head") : fail("G8 Slide head",
              QStringLiteral("ok=%1 lead=%2 inN=%3 err=%4")
                  .arg(ok).arg(clips[0].leadInSec).arg(clips[1].inPoint).arg(err));
    }

    // G9: speed=2.0 換算 — RippleOut で timeline delta 1.0s は source 2.0s 進む。
    // 実尺は (outEff-in)/speed なので 1.0s 増える。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 0.0, 4.0, 10.0, 2.0, "a")  // 実尺 (4-0)/2=2.0
              << makeClip(0.0, 0.0, 4.0, 4.0, 1.0, "b");
        const double durBefore = clips[0].effectiveDuration();
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::RippleOut, 1.0, &err);
        const bool good = ok && approx(clips[0].outPoint, 6.0)         // source +2.0
                       && approx(clips[0].effectiveDuration(), durBefore + 1.0) // timeline +1.0
                       && approx(clips[1].leadInSec, 1.0);
        good ? pass("G9 speed conv") : fail("G9 speed conv",
              QStringLiteral("ok=%1 out=%2 dur=%3 leadNext=%4 err=%5")
                  .arg(ok).arg(clips[0].outPoint).arg(clips[0].effectiveDuration())
                  .arg(clips[1].leadInSec).arg(err));
    }

    // G10: maxTrimDelta / minTrimDelta — RippleOut の上下限と境界一致確認。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 2.0, 6.0, 10.0, 1.0, "a")  // out headroom 4.0, in→out 4.0
              << makeClip(0.0, 0.0, 5.0, 5.0, 1.0, "b");
        const double mx = trimops::maxTrimDelta(clips, 0, TrimType::RippleOut); // duration-out=4.0
        const double mn = trimops::minTrimDelta(clips, 0, TrimType::RippleOut); // -(out-in)=-4.0 だが
                                                                                // leadNext=0 → -0 が支配
        // 下流 leadIn=0 なので負方向は 0 にクランプされる (下流が重なるため)。
        // 上限は素材 headroom 4.0。max は適用成功、max+ε は失敗を確認。
        QVector<ClipInfo> c2 = clips; QString e2;
        const bool okMax = trimops::applyTrim(c2, 0, TrimType::RippleOut, mx, &e2);
        QVector<ClipInfo> c3 = clips; QString e3;
        const bool okOver = trimops::applyTrim(c3, 0, TrimType::RippleOut, mx + 0.5, &e3);
        const bool good = approx(mx, 4.0) && approx(mn, 0.0) && okMax && !okOver;
        good ? pass("G10 bounds") : fail("G10 bounds",
              QStringLiteral("mx=%1 mn=%2 okMax=%3 okOver=%4").arg(mx).arg(mn).arg(okMax).arg(okOver));
    }

    // G11: 不正 index は false + clips 不変。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 0.0, 5.0, 10.0, 1.0, "a");
        const QVector<ClipInfo> before = clips;
        QString err;
        const bool ok = trimops::applyTrim(clips, 5, TrimType::Slip, 1.0, &err);
        const bool good = !ok && !err.isEmpty() && approx(clips[0].inPoint, before[0].inPoint);
        good ? pass("G11 bad index") : fail("G11 bad index",
              QStringLiteral("ok=%1 err=%2").arg(ok).arg(err));
    }

    // G12: Roll に次クリップが無い場合は false。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 0.0, 5.0, 10.0, 1.0, "a");
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::Roll, 1.0, &err);
        const bool good = !ok && !err.isEmpty();
        good ? pass("G12 Roll no next") : fail("G12 Roll no next",
              QStringLiteral("ok=%1 err=%2").arg(ok).arg(err));
    }

    // G13: Roll 境界違反 — 右クリップの素材先頭 (inPoint=0) を割る負 delta は
    //      false + clips 不変。b.inPoint=0.5 を -1.0 動かすと <0 になる。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 0.0, 5.0, 10.0, 1.0, "a")
              << makeClip(0.0, 0.5, 8.0, 10.0, 1.0, "b");
        const QVector<ClipInfo> before = clips;
        QString err;
        const bool ok = trimops::applyTrim(clips, 0, TrimType::Roll, -1.0, &err);
        const bool good = !ok && !err.isEmpty()
                       && approx(clips[0].outPoint, before[0].outPoint)
                       && approx(clips[1].inPoint, before[1].inPoint);
        good ? pass("G13 Roll bound") : fail("G13 Roll bound",
              QStringLiteral("ok=%1 outA=%2 inB=%3 err=%4")
                  .arg(ok).arg(clips[0].outPoint).arg(clips[1].inPoint).arg(err));
    }

    // G14: max/min Trim Delta の符号・大きさ — 各トリムで max>=0 / min<=0、かつ
    //      Slip/RippleIn の限界値が source headroom を speed で割った値に一致する。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 2.0, 6.0, 10.0, 2.0, "a")  // speed=2.0
              << makeClip(0.0, 1.0, 5.0, 5.0, 1.0, "b");
        const double slipMax = trimops::maxTrimDelta(clips, 0, TrimType::Slip);
        const double slipMin = trimops::minTrimDelta(clips, 0, TrimType::Slip);
        const double inMax   = trimops::maxTrimDelta(clips, 0, TrimType::RippleIn);
        const double inMin   = trimops::minTrimDelta(clips, 0, TrimType::RippleIn);
        const double rollMax = trimops::maxTrimDelta(clips, 0, TrimType::Roll);
        const double rollMin = trimops::minTrimDelta(clips, 0, TrimType::Roll);
        const bool good =
               // Slip: max=(duration-out)/speed=(10-6)/2=2.0, min=-in/speed=-2/2=-1.0
               approx(slipMax, 2.0) && approx(slipMin, -1.0)
               // RippleIn: max=(out-in)/speed=(6-2)/2=2.0, min=-in/speed=-1.0
            && approx(inMax, 2.0) && approx(inMin, -1.0)
               // 符号: max>=0, min<=0
            && rollMax >= -1e-9 && rollMin <= 1e-9;
        good ? pass("G14 max/min sign+magnitude") : fail("G14 max/min sign+magnitude",
              QStringLiteral("slipMax=%1 slipMin=%2 inMax=%3 inMin=%4 rollMax=%5 rollMin=%6")
                  .arg(slipMax).arg(slipMin).arg(inMax).arg(inMin).arg(rollMax).arg(rollMin));
    }

    // G15: max/min が applyTrim() の排他的な 0 長境界を返さないこと。
    //      返値そのものは成功し、少し越えた値は失敗する。
    {
        QString detail;
        bool good = true;

        auto require = [&](bool cond, const QString &msg) {
            if (!cond) {
                good = false;
                if (!detail.isEmpty()) detail += QStringLiteral("; ");
                detail += msg;
            }
        };

        {
            QVector<ClipInfo> clips;
            clips << makeClip(0.0, 0.0, 4.0, 10.0, 0.25, "ripple-in");
            const double mx = trimops::maxTrimDelta(clips, 0, TrimType::RippleIn);
            QVector<ClipInfo> at = clips, over = clips;
            QString e1, e2;
            require(trimops::applyTrim(at, 0, TrimType::RippleIn, mx, &e1),
                    QStringLiteral("RippleIn max failed mx=%1 err=%2").arg(mx).arg(e1));
            require(!trimops::applyTrim(over, 0, TrimType::RippleIn, mx + 0.01, &e2),
                    QStringLiteral("RippleIn over succeeded mx=%1").arg(mx));
        }
        {
            QVector<ClipInfo> clips;
            clips << makeClip(0.0, 2.0, 6.0, 10.0, 0.25, "ripple-out");
            const double mn = trimops::minTrimDelta(clips, 0, TrimType::RippleOut);
            QVector<ClipInfo> at = clips, over = clips;
            QString e1, e2;
            require(trimops::applyTrim(at, 0, TrimType::RippleOut, mn, &e1),
                    QStringLiteral("RippleOut min failed mn=%1 err=%2").arg(mn).arg(e1));
            require(!trimops::applyTrim(over, 0, TrimType::RippleOut, mn - 0.01, &e2),
                    QStringLiteral("RippleOut under succeeded mn=%1").arg(mn));
        }
        {
            QVector<ClipInfo> clips;
            clips << makeClip(0.0, 0.0, 4.0, 10.0, 1.0, "a")
                  << makeClip(0.0, 2.0, 4.0, 10.0, 1.0, "b");
            const double mx = trimops::maxTrimDelta(clips, 0, TrimType::Roll);
            QVector<ClipInfo> at = clips, over = clips;
            QString e1, e2;
            require(trimops::applyTrim(at, 0, TrimType::Roll, mx, &e1),
                    QStringLiteral("Roll max failed mx=%1 err=%2").arg(mx).arg(e1));
            require(!trimops::applyTrim(over, 0, TrimType::Roll, mx + 0.01, &e2),
                    QStringLiteral("Roll over succeeded mx=%1").arg(mx));
        }
        {
            QVector<ClipInfo> clips;
            clips << makeClip(0.0, 0.0, 4.0, 10.0, 1.0, "p")
                  << makeClip(0.0, 0.0, 4.0, 10.0, 1.0, "c")
                  << makeClip(0.0, 2.0, 4.0, 10.0, 1.0, "n");
            const double mx = trimops::maxTrimDelta(clips, 1, TrimType::Slide);
            QVector<ClipInfo> at = clips, over = clips;
            QString e1, e2;
            require(trimops::applyTrim(at, 1, TrimType::Slide, mx, &e1),
                    QStringLiteral("Slide max failed mx=%1 err=%2").arg(mx).arg(e1));
            require(!trimops::applyTrim(over, 1, TrimType::Slide, mx + 0.01, &e2),
                    QStringLiteral("Slide over succeeded mx=%1").arg(mx));
        }

        good ? pass("G15 max/min apply boundaries") : fail("G15 max/min apply boundaries", detail);
    }

    qInfo().noquote() << QStringLiteral("[trim-ops] selftest done: %1 passed, %2 failed")
                             .arg(passed).arg(failed);
    return failed == 0 ? 0 : 1;
}
