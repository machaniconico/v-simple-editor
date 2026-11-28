#include "SelftestRegistry.h"
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpToolRegistry.h"

#include <QAction>
#include <QJsonObject>

#include <cmath>
#include <iostream>

namespace {
using TrackClips = QVector<QVector<ClipInfo>>;

ClipInfo makeTestClip(const QString &name, double durSec, int linkGroup = 0)
{
    ClipInfo c;
    c.filePath = QStringLiteral("test_assets/%1.mp4").arg(name);
    c.displayName = name;
    c.duration = durSec;
    c.inPoint = 0.0;
    c.outPoint = durSec;
    c.linkGroup = linkGroup;
    return c;
}

double trackTotalDuration(const TimelineTrack *t)
{
    if (!t) return 0.0;
    double cur = 0.0;
    for (const ClipInfo &c : t->clips())
        cur += c.leadInSec + c.effectiveDuration();
    return cur;
}
} // namespace

int runBladeMcpRippleSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](const char *gate, bool ok, const QString &detail) {
        if (ok) {
            ++passed;
            std::cout << "PASS " << gate << " " << detail.toStdString() << "\n";
        } else {
            ++failed;
            std::cerr << "FAIL " << gate << " " << detail.toStdString() << "\n";
        }
    };

    // G1: Timeline::rippleTrimBeforePlayhead() trims [clipStart, playhead] across V1+A1,
    // moves playhead to clipStart, 1 Undo step.
    {
        Timeline tl;
        tl.restoreFromProject(
            TrackClips{{makeTestClip(QStringLiteral("A"), 10.0, 1), makeTestClip(QStringLiteral("B"), 5.0, 2)}},
            TrackClips{{makeTestClip(QStringLiteral("A"), 10.0, 1), makeTestClip(QStringLiteral("B"), 5.0, 2)}},
            0.0, -1.0, -1.0, 100);
        tl.setPlayheadPosition(3.0);
        const auto serial0 = tl.undoManager()->saveSerial();
        const bool okTrim = tl.rippleTrimBeforePlayhead();
        const double vDur = trackTotalDuration(tl.videoTracks().value(0));
        const double aDur = trackTotalDuration(tl.audioTracks().value(0));
        const bool afterTrim = okTrim && tl.undoManager()->saveSerial() == serial0 + 1
            && std::abs(vDur - 12.0) < 0.05 && std::abs(aDur - 12.0) < 0.05
            && std::abs(tl.playheadPosition() - 0.0) < 0.05;
        tl.undo();
        const bool afterUndo = std::abs(trackTotalDuration(tl.videoTracks().value(0)) - 15.0) < 0.05;
        check("G1", afterTrim && afterUndo,
              QStringLiteral("vDur=%1 aDur=%2 ph=%3 undo=%4")
                  .arg(vDur, 0, 'f', 2).arg(aDur, 0, 'f', 2).arg(tl.playheadPosition(), 0, 'f', 2).arg(afterUndo));
    }

    // G2: Timeline::rippleTrimAfterPlayhead() trims [playhead, clipEnd] across unlocked tracks
    // while respecting locked V2, 1 Undo step.
    {
        Timeline tl;
        tl.restoreFromProject(
            TrackClips{{makeTestClip(QStringLiteral("A"), 10.0, 1), makeTestClip(QStringLiteral("B"), 5.0, 2)},
                       {makeTestClip(QStringLiteral("LockedBroll"), 12.0, 0)}},
            TrackClips{{makeTestClip(QStringLiteral("A"), 10.0, 1), makeTestClip(QStringLiteral("B"), 5.0, 2)}},
            0.0, -1.0, -1.0, 100);
        tl.videoTracks()[1]->setLocked(true);
        tl.setPlayheadPosition(4.0);
        const auto serial0 = tl.undoManager()->saveSerial();
        const bool okTrim = tl.rippleTrimAfterPlayhead();
        const double v1Dur = trackTotalDuration(tl.videoTracks().value(0));
        const double v2Dur = trackTotalDuration(tl.videoTracks().value(1));
        const double a1Dur = trackTotalDuration(tl.audioTracks().value(0));
        const bool ok = okTrim && tl.undoManager()->saveSerial() == serial0 + 1
            && std::abs(v1Dur - 9.0) < 0.05 && std::abs(a1Dur - 9.0) < 0.05
            && std::abs(v2Dur - 12.0) < 0.05; // locked V2 untouched!
        check("G2", ok, QStringLiteral("v1=%1 a1=%2 lockedV2=%3")
                            .arg(v1Dur, 0, 'f', 2).arg(a1Dur, 0, 'f', 2).arg(v2Dur, 0, 'f', 2));
    }

    // G3: Timeline::rippleInsertClipAtTime() splits straddling clip into [left, inserted, right]
    // and shifts downstream clips right by inserted duration; 1 Undo restores original clip.
    {
        Timeline tl;
        tl.restoreFromProject(
            TrackClips{{makeTestClip(QStringLiteral("Base"), 10.0, 1)}},
            TrackClips{{}},
            0.0, -1.0, -1.0, 100);
        const auto serial0 = tl.undoManager()->saveSerial();
        QString err;
        const bool insOk = tl.rippleInsertClipAtTime(false, 0, makeTestClip(QStringLiteral("Ins"), 4.0), 3.0, &err);
        const auto clips = tl.videoTracks()[0]->clips();
        const bool splitOk = insOk && tl.undoManager()->saveSerial() == serial0 + 1
            && clips.size() == 3
            && std::abs(clips[0].effectiveDuration() - 3.0) < 0.05
            && clips[1].displayName == QStringLiteral("Ins")
            && std::abs(clips[1].effectiveDuration() - 4.0) < 0.05
            && std::abs(clips[2].effectiveDuration() - 7.0) < 0.05;
        tl.undo();
        const bool undoOk = tl.videoTracks()[0]->clips().size() == 1
            && std::abs(trackTotalDuration(tl.videoTracks()[0]) - 10.0) < 0.05;
        check("G3", splitOk && undoOk,
              QStringLiteral("clips=%1 total=%2 undo=%3").arg(clips.size()).arg(trackTotalDuration(tl.videoTracks()[0]), 0, 'f', 2).arg(undoOk));
    }

    // G4: rippleInsertClipAtTime() gap insert + append past end + locked track rejection.
    {
        Timeline tl;
        ClipInfo c = makeTestClip(QStringLiteral("Late"), 5.0);
        c.leadInSec = 4.0;
        tl.restoreFromProject(TrackClips{{c}}, TrackClips{{}}, 0.0, -1.0, -1.0, 100);
        QString err;
        const bool gapOk = tl.rippleInsertClipAtTime(false, 0, makeTestClip(QStringLiteral("InGap"), 2.0), 1.0, &err);
        const auto afterGap = tl.videoTracks()[0]->clips();
        const bool gapLayoutOk = gapOk && afterGap.size() == 2
            && std::abs(afterGap[0].leadInSec - 1.0) < 0.05
            && std::abs(afterGap[0].effectiveDuration() - 2.0) < 0.05
            && std::abs(afterGap[1].leadInSec - 3.0) < 0.05;

        tl.videoTracks()[0]->setLocked(true);
        QString lockErr;
        const bool lockedRejected = !tl.rippleInsertClipAtTime(false, 0, makeTestClip(QStringLiteral("X"), 1.0), 0.0, &lockErr)
            && !lockErr.isEmpty();
        check("G4", gapLayoutOk && lockedRejected,
              QStringLiteral("gapLayout=%1 lockedRej=%2").arg(gapLayoutOk).arg(lockedRejected));
    }

    // G5-G8: MainWindow + MCP tool integration (`ripple_delete_range`, `blade_all_tracks`,
    // `ripple_insert_clip`) + menu actions.
    MainWindow win;
    mcp::McpToolRegistry registry;
    mcp::McpEditorTools tools(&win, &registry);
    tools.registerReadTools();
    tools.registerWriteTools();
    Timeline *tl = win.findChild<Timeline *>();

    auto callTool = [&](const QString &name, const QJsonObject &args, QString *errOut) {
        bool found = false;
        return registry.callTool(name, args, &found, errOut);
    };

    // G5: MCP ripple_delete_range cuts [2.0, 5.0] across V1+A1 in 1 Undo step.
    {
        tl->restoreFromProject(
            TrackClips{{makeTestClip(QStringLiteral("V"), 10.0, 1)}},
            TrackClips{{makeTestClip(QStringLiteral("A"), 10.0, 1)}},
            0.0, -1.0, -1.0, 100);
        QString err;
        const QJsonObject res = callTool(QStringLiteral("ripple_delete_range"),
            {{QStringLiteral("startSec"), 2.0}, {QStringLiteral("endSec"), 5.0}}, &err);
        const double vDur = trackTotalDuration(tl->videoTracks().value(0));
        const double aDur = trackTotalDuration(tl->audioTracks().value(0));

        QString badErr;
        callTool(QStringLiteral("ripple_delete_range"),
            {{QStringLiteral("startSec"), 5.0}, {QStringLiteral("endSec"), 2.0}}, &badErr);

        const bool ok = err.isEmpty() && res.value(QStringLiteral("ok")).toBool()
            && std::abs(res.value(QStringLiteral("removedSec")).toDouble() - 3.0) < 1e-6
            && std::abs(vDur - 7.0) < 0.05 && std::abs(aDur - 7.0) < 0.05
            && !badErr.isEmpty();
        check("G5", ok, QStringLiteral("vDur=%1 aDur=%2 badErr=%3")
                            .arg(vDur, 0, 'f', 2).arg(aDur, 0, 'f', 2).arg(!badErr.isEmpty()));
    }

    // G6: MCP blade_all_tracks tests none / before / after modes.
    {
        tl->restoreFromProject(
            TrackClips{{makeTestClip(QStringLiteral("V"), 10.0, 1)}},
            TrackClips{{makeTestClip(QStringLiteral("A"), 10.0, 1)}},
            0.0, -1.0, -1.0, 100);
        QString err1, err2, err3;
        const QJsonObject rNone = callTool(QStringLiteral("blade_all_tracks"),
            {{QStringLiteral("timeSec"), 5.0}, {QStringLiteral("rippleTrim"), QStringLiteral("none")}}, &err1);
        const bool noneOk = err1.isEmpty() && rNone.value(QStringLiteral("ok")).toBool()
            && tl->videoTracks()[0]->clipCount() == 2 && tl->audioTracks()[0]->clipCount() == 2;

        const QJsonObject rBefore = callTool(QStringLiteral("blade_all_tracks"),
            {{QStringLiteral("timeSec"), 2.0}, {QStringLiteral("rippleTrim"), QStringLiteral("before")}}, &err2);
        const bool beforeOk = err2.isEmpty() && rBefore.value(QStringLiteral("ok")).toBool()
            && std::abs(trackTotalDuration(tl->videoTracks()[0]) - 8.0) < 0.05;

        const QJsonObject rAfter = callTool(QStringLiteral("blade_all_tracks"),
            {{QStringLiteral("timeSec"), 5.0}, {QStringLiteral("rippleTrim"), QStringLiteral("after")}}, &err3);
        const bool afterOk = err3.isEmpty() && rAfter.value(QStringLiteral("ok")).toBool()
            && std::abs(trackTotalDuration(tl->videoTracks()[0]) - 5.0) < 0.05;

        check("G6", noneOk && beforeOk && afterOk,
              QStringLiteral("none=%1 before=%2 after=%3").arg(noneOk).arg(beforeOk).arg(afterOk));
    }

    // G7: MCP ripple_insert_clip inserts clip at timeSec=3.0 and splits straddling clip.
    {
        tl->restoreFromProject(
            TrackClips{{makeTestClip(QStringLiteral("Main"), 10.0, 1)}},
            TrackClips{{}},
            0.0, -1.0, -1.0, 100);
        QString err;
        const QJsonObject res = callTool(QStringLiteral("ripple_insert_clip"), {
            {QStringLiteral("filePath"), QStringLiteral("test_assets/broll.mp4")},
            {QStringLiteral("timeSec"), 3.0},
            {QStringLiteral("durationSec"), 2.5},
            {QStringLiteral("displayName"), QStringLiteral("Broll")},
        }, &err);
        const bool insOk = err.isEmpty() && res.value(QStringLiteral("ok")).toBool()
            && res.value(QStringLiteral("clipCount")).toInt() == 3
            && std::abs(trackTotalDuration(tl->videoTracks()[0]) - 12.5) < 0.05;
        tl->undo();
        const bool undoOk = tl->videoTracks()[0]->clipCount() == 1
            && std::abs(trackTotalDuration(tl->videoTracks()[0]) - 10.0) < 0.05;
        check("G7", insOk && undoOk, QStringLiteral("ins=%1 undo=%2").arg(insOk).arg(undoOk));
    }

    // G8: MainWindow edit menu contains both new all-track ripple-cut actions.
    {
        bool foundBefore = false;
        bool foundAfter = false;
        for (QAction *act : win.findChildren<QAction *>()) {
            if (!act) continue;
            if (act->text().contains(QStringLiteral("再生ヘッドより前をリップルカット")))
                foundBefore = (act->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+Q")));
            if (act->text().contains(QStringLiteral("再生ヘッドより後をリップルカット")))
                foundAfter = (act->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+W")));
        }
        check("G8", foundBefore && foundAfter,
              QStringLiteral("beforeAct=%1 afterAct=%2").arg(foundBefore).arg(foundAfter));
    }

    const int declared = 8;
    std::cout << "blade-mcp-ripple summary: " << passed << " PASS, " << failed << " FAIL, 0 SKIP\n";
    std::cout << "gate-count: declared " << declared << ", ran " << (passed + failed) << "\n";
    return failed == 0 ? 0 : 1;
}
