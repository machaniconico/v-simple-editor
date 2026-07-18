// three_point_edit_selftest.cpp — ThreePointEdit 純粋エンジン (SM-1) の
// headless 単体テスト。QApplication 不要 (needsQApplication=false)。

#include <QDebug>
#include <QString>
#include <QVector>
#include <cmath>

#include "../ThreePointEdit.h"
#include "../Timeline.h"   // ClipInfo

namespace {

using threepoint::SourceSelection;
using threepoint::OverwritePlan;

// 指定の絶対開始秒 (leadIn) と長さを持つ speed=1.0 クリップを作る。
// effectiveDuration() = (outPoint - inPoint) / speed = lengthSec。
ClipInfo makeClip(double leadInSec, double lengthSec, const QString &name)
{
    ClipInfo c;
    c.filePath = QStringLiteral("/clips/") + name + QStringLiteral(".mp4");
    c.displayName = name;
    c.duration = lengthSec;
    c.inPoint = 0.0;
    c.outPoint = lengthSec;
    c.leadInSec = leadInSec;
    c.speed = 1.0;
    return c;
}

bool approx(double a, double b) { return std::fabs(a - b) < 1e-4; }

} // namespace

int runThreePointEditSelftest()
{
    qInfo().noquote() << "[three-point-edit] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char *name) { ++passed; qInfo().noquote() << "[three-point-edit] PASS" << name; };
    auto fail = [&](const char *name, const QString &msg) {
        ++failed; qWarning().noquote() << "[three-point-edit] FAIL" << name << ":" << msg;
    };

    // G1: effectiveOutSec — sourceOutSec>0 はそれ、<=0 は durationSec
    {
        SourceSelection a; a.durationSec = 10.0; a.sourceOutSec = 4.0;
        SourceSelection b; b.durationSec = 10.0; b.sourceOutSec = 0.0;
        const bool ok = approx(threepoint::effectiveOutSec(a), 4.0)
                     && approx(threepoint::effectiveOutSec(b), 10.0);
        ok ? pass("G1 effectiveOutSec")
           : fail("G1 effectiveOutSec",
                  QStringLiteral("a=%1 b=%2").arg(threepoint::effectiveOutSec(a)).arg(threepoint::effectiveOutSec(b)));
    }

    // G2: selectionDurationSec — effectiveOut - sourceIn、負は 0 クランプ
    {
        SourceSelection a; a.durationSec = 10.0; a.sourceInSec = 2.0; a.sourceOutSec = 7.0;
        SourceSelection b; b.durationSec = 10.0; b.sourceInSec = 8.0; b.sourceOutSec = 3.0; // out<in
        const bool ok = approx(threepoint::selectionDurationSec(a), 5.0)
                     && approx(threepoint::selectionDurationSec(b), 0.0);
        ok ? pass("G2 selectionDurationSec")
           : fail("G2 selectionDurationSec",
                  QStringLiteral("a=%1 b=%2").arg(threepoint::selectionDurationSec(a)).arg(threepoint::selectionDurationSec(b)));
    }

    // G3: validate — 正常な選択は true、errorOut は空
    {
        SourceSelection s;
        s.filePath = QStringLiteral("/clips/ok.mp4");
        s.durationSec = 10.0; s.sourceInSec = 1.0; s.sourceOutSec = 6.0;
        QString err = QStringLiteral("dirty");
        const bool ok = threepoint::validate(s, &err) && err.isEmpty();
        ok ? pass("G3 validate accepts valid selection")
           : fail("G3 validate", QStringLiteral("err=%1").arg(err));
    }

    // G4: validate — 各種異常を reject し日本語理由を返す
    {
        QString err;
        SourceSelection empty; empty.durationSec = 5.0; empty.sourceOutSec = 3.0; // filePath 空
        const bool r1 = !threepoint::validate(empty, &err) && !err.isEmpty();

        SourceSelection badDur; badDur.filePath = QStringLiteral("x"); badDur.durationSec = 0.0;
        const bool r2 = !threepoint::validate(badDur, &err) && !err.isEmpty();

        SourceSelection inGeOut; inGeOut.filePath = QStringLiteral("x");
        inGeOut.durationSec = 10.0; inGeOut.sourceInSec = 6.0; inGeOut.sourceOutSec = 6.0; // in==out
        const bool r3 = !threepoint::validate(inGeOut, &err) && !err.isEmpty();

        SourceSelection outOver; outOver.filePath = QStringLiteral("x");
        outOver.durationSec = 5.0; outOver.sourceInSec = 0.0; outOver.sourceOutSec = 9.0; // out>dur
        const bool r4 = !threepoint::validate(outOver, &err) && !err.isEmpty();

        const bool ok = r1 && r2 && r3 && r4;
        ok ? pass("G4 validate rejects invalid selections")
           : fail("G4 validate",
                  QStringLiteral("r1=%1 r2=%2 r3=%3 r4=%4").arg(r1).arg(r2).arg(r3).arg(r4));
    }

    // G5: buildClipInfo — フィールド写像と speed=1.0
    {
        SourceSelection s;
        s.filePath = QStringLiteral("/clips/src.mp4");
        s.displayName = QStringLiteral("Src");
        s.durationSec = 12.0; s.sourceInSec = 2.0; s.sourceOutSec = 9.0;
        const ClipInfo c = threepoint::buildClipInfo(s);
        const bool ok = c.filePath == s.filePath
                     && c.displayName == s.displayName
                     && approx(c.duration, 12.0)
                     && approx(c.inPoint, 2.0)
                     && approx(c.outPoint, 9.0)
                     && approx(c.speed, 1.0)
                     && approx(c.effectiveDuration(), 7.0);
        ok ? pass("G5 buildClipInfo maps fields and sets speed=1.0")
           : fail("G5 buildClipInfo",
                  QStringLiteral("in=%1 out=%2 dur=%3 eff=%4")
                      .arg(c.inPoint).arg(c.outPoint).arg(c.duration).arg(c.effectiveDuration()));
    }

    // G6: clipStartSeconds — leadIn モデルの絶対開始秒
    {
        QVector<ClipInfo> clips;
        clips << makeClip(1.0, 3.0, QStringLiteral("a"))   // start 1, end 4
              << makeClip(0.0, 2.0, QStringLiteral("b"))   // start 4, end 6
              << makeClip(2.0, 4.0, QStringLiteral("c"));  // start 8, end 12
        const QVector<double> s = threepoint::clipStartSeconds(clips);
        const bool ok = s.size() == 3 && approx(s[0], 1.0) && approx(s[1], 4.0) && approx(s[2], 8.0);
        ok ? pass("G6 clipStartSeconds computes absolute starts")
           : fail("G6 clipStartSeconds",
                  QStringLiteral("s0=%1 s1=%2 s2=%3")
                      .arg(s.value(0)).arg(s.value(1)).arg(s.value(2)));
    }

    // G7: insertIndexForTime — 空トラックは index 0 / leadIn=T
    {
        QVector<ClipInfo> empty;
        double leadIn = -1.0;
        const int idx = threepoint::insertIndexForTime(empty, 5.0, &leadIn);
        const bool ok = idx == 0 && approx(leadIn, 5.0);
        ok ? pass("G7 insertIndexForTime empty track appends with gap")
           : fail("G7 insertIndexForTime", QStringLiteral("idx=%1 leadIn=%2").arg(idx).arg(leadIn));
    }

    // G8: insertIndexForTime — 既存クリップ間ギャップへの挿入
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 4.0, QStringLiteral("a"))   // [0,4)
              << makeClip(4.0, 2.0, QStringLiteral("b"));  // gap [4,8), clip [8,10)
        double leadIn = -1.0;
        // T=6 は a の後ろ (cursor=4)、b の手前ギャップ。挿入 index=1, leadIn=2。
        const int idx = threepoint::insertIndexForTime(clips, 6.0, &leadIn);
        const bool ok = idx == 1 && approx(leadIn, 2.0);
        ok ? pass("G8 insertIndexForTime inserts into mid gap")
           : fail("G8 insertIndexForTime", QStringLiteral("idx=%1 leadIn=%2").arg(idx).arg(leadIn));
    }

    // G9: planOverwrite — 空トラックへ T>=0 配置は末尾ギャップ追加
    {
        QVector<ClipInfo> empty;
        const OverwritePlan p = threepoint::planOverwrite(empty, 3.0, 5.0);
        const bool ok = p.valid
                     && p.splitHeadIndex == -1 && p.splitTailIndex == -1
                     && p.removeCount == 0
                     && p.insertIndex == 0
                     && approx(p.insertLeadInSec, 3.0);
        ok ? pass("G9 planOverwrite empty track = tail append with gap")
           : fail("G9 planOverwrite empty",
                  QStringLiteral("valid=%1 ins=%2 lead=%3").arg(p.valid).arg(p.insertIndex).arg(p.insertLeadInSec));
    }

    // G10: planOverwrite — 単一クリップの内部を完全に覆う (head/tail 両分割)。
    //      clip a = [0,10)。T=2, L=4 → [2,6) を上書き。
    //      a を 2 で head 分割 → [0,2)+[2,10)、後者を 6 で tail 分割。
    //      論理列: seg0[0,2] seg1[2,6] seg2[6,10]。中央 seg1 が削除対象。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 10.0, QStringLiteral("a"));
        const OverwritePlan p = threepoint::planOverwrite(clips, 2.0, 4.0);
        const bool ok = p.valid
                     && p.splitHeadIndex == 0 && approx(p.splitHeadLocalSec, 2.0)
                     && p.splitTailIndex == 0 && approx(p.splitTailLocalSec, 6.0)
                     && p.removeFromIndex == 1 && p.removeCount == 1
                     && p.insertIndex == 1
                     && approx(p.insertLeadInSec, 0.0);
        ok ? pass("G10 planOverwrite splits one clip head+tail, removes middle")
           : fail("G10 planOverwrite interior",
                  QStringLiteral("sh=%1@%2 st=%3@%4 rf=%5 rc=%6 ins=%7 lead=%8")
                      .arg(p.splitHeadIndex).arg(p.splitHeadLocalSec)
                      .arg(p.splitTailIndex).arg(p.splitTailLocalSec)
                      .arg(p.removeFromIndex).arg(p.removeCount)
                      .arg(p.insertIndex).arg(p.insertLeadInSec));
    }

    // G11: planOverwrite — 境界ちょうど配置は分割不要、跨いだクリップ群を削除。
    //      a=[0,4) b=[4,8) c=[8,12) (全 leadIn=0)。T=4, L=4 → [4,8) を上書き。
    //      b の境界ちょうどなので分割なし。b のみ削除、c の前に挿入。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 4.0, QStringLiteral("a"))
              << makeClip(0.0, 4.0, QStringLiteral("b"))
              << makeClip(0.0, 4.0, QStringLiteral("c"));
        const OverwritePlan p = threepoint::planOverwrite(clips, 4.0, 4.0);
        const bool ok = p.valid
                     && p.splitHeadIndex == -1 && p.splitTailIndex == -1
                     && p.removeFromIndex == 1 && p.removeCount == 1
                     && p.insertIndex == 1
                     && approx(p.insertLeadInSec, 0.0);
        ok ? pass("G11 planOverwrite boundary-aligned removes whole clip, no split")
           : fail("G11 planOverwrite boundary",
                  QStringLiteral("sh=%1 st=%2 rf=%3 rc=%4 ins=%5")
                      .arg(p.splitHeadIndex).arg(p.splitTailIndex)
                      .arg(p.removeFromIndex).arg(p.removeCount).arg(p.insertIndex));
    }

    // G12: planOverwrite — 末尾以降 (T>=全長) はギャップ付き末尾追加。
    //      a=[0,4)。T=6, L=2 → 既存に触れず insertIndex=1, leadIn=2。
    {
        QVector<ClipInfo> clips;
        clips << makeClip(0.0, 4.0, QStringLiteral("a"));
        const OverwritePlan p = threepoint::planOverwrite(clips, 6.0, 2.0);
        const bool ok = p.valid
                     && p.splitHeadIndex == -1 && p.splitTailIndex == -1
                     && p.removeCount == 0
                     && p.insertIndex == 1
                     && approx(p.insertLeadInSec, 2.0);
        ok ? pass("G12 planOverwrite past-end = tail append with gap")
           : fail("G12 planOverwrite past-end",
                  QStringLiteral("rc=%1 ins=%2 lead=%3").arg(p.removeCount).arg(p.insertIndex).arg(p.insertLeadInSec));
    }

    qInfo().noquote().nospace()
        << "[three-point-edit] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
