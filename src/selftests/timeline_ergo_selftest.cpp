#include "../Timeline.h"
#include "../UndoManager.h"
#include "../Timecode.h"
#include <QApplication>
#include <QScrollBar>

#include <QSignalBlocker>

#include <cmath>
#include <cstdio>

namespace {
ClipInfo clip(double duration, double gap = 0.0, int group = 0)
{
    ClipInfo result;
    result.filePath = QStringLiteral("/selftests/timeline-ergo.mov");
    result.displayName = QStringLiteral("操作テスト");
    result.duration = duration;
    result.outPoint = duration;
    result.leadInSec = gap;
    result.linkGroup = group;
    return result;
}

void baseline(Timeline &timeline)
{
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("Baseline"));
}

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-6;
}

bool sameClipPayload(const ClipInfo &a, const ClipInfo &b)
{
    // Duplication changes placement and link identity, so omit leadInSec/linkGroup.
    return a.filePath == b.filePath && a.displayName == b.displayName
        && near(a.duration, b.duration) && near(a.inPoint, b.inPoint)
        && near(a.outPoint, b.outPoint) && near(a.speed, b.speed)
        && near(a.volume, b.volume) && near(a.pan, b.pan)
        && a.reversed == b.reversed && a.audioChannelMode == b.audioChannelMode
        && near(a.opacity, b.opacity) && near(a.videoScale, b.videoScale)
        && near(a.videoDx, b.videoDx) && near(a.videoDy, b.videoDy)
        && near(a.rotation2DDegrees, b.rotation2DDegrees)
        && a.visible == b.visible && a.effects.size() == b.effects.size();
}

bool sameTracks(const QVector<QVector<ClipInfo>> &a, const QVector<QVector<ClipInfo>> &b)
{
    if (a.size() != b.size()) return false;
    for (int t = 0; t < a.size(); ++t) {
        if (a[t].size() != b[t].size()) return false;
        for (int c = 0; c < a[t].size(); ++c) {
            const auto &x = a[t][c];
            const auto &y = b[t][c];
            if (x.filePath != y.filePath || !near(x.duration, y.duration)
                || !near(x.inPoint, y.inPoint) || !near(x.outPoint, y.outPoint)
                || !near(x.leadInSec, y.leadInSec) || x.linkGroup != y.linkGroup)
                return false;
        }
    }
    return true;
}
} // namespace

int runTimelineErgoSelftest()
{
    int passed = 0;
    int failed = 0;
    auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "[timeline-ergo] %s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };

    {
        Timeline timeline;
        const QVector<ClipInfo> clips{clip(2.0, 0.0, 7), clip(2.0), clip(2.0, 1.0)};
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{clips, clips, clips},
                                    QVector<QVector<ClipInfo>>{clips, clips, clips},
                                    2.0, -1.0, -1.0, 100);
        timeline.videoTracks()[1]->setLocked(true);
        timeline.audioTracks()[1]->setLocked(true);
        timeline.videoTracks()[2]->setHidden(true);
        timeline.audioTracks()[2]->setHidden(true);
        // Existing linked selection must not leak into excluded tracks.
        timeline.videoTracks()[1]->setSelectedClip(0);
        baseline(timeline);
        timeline.selectAllClips();
        const auto excludedEmpty = [&]() {
            return timeline.videoTracks()[1]->selectedClips().isEmpty()
                && timeline.audioTracks()[1]->selectedClips().isEmpty()
                && timeline.videoTracks()[2]->selectedClips().isEmpty()
                && timeline.audioTracks()[2]->selectedClips().isEmpty();
        };
        gate(1, timeline.videoTracks()[0]->selectedClips() == QList<int>({0, 1, 2})
                && timeline.audioTracks()[0]->selectedClips() == QList<int>({0, 1, 2})
                && excludedEmpty() && !timeline.canUndo());

        timeline.selectClipsFromPlayhead(true);
        bool ok = timeline.videoTracks()[0]->selectedClips() == QList<int>({1, 2})
            && timeline.audioTracks()[0]->selectedClips() == QList<int>({1, 2})
            && excludedEmpty();
        timeline.selectClipsFromPlayhead(false);
        ok = ok && timeline.videoTracks()[0]->selectedClips() == QList<int>({0})
            && timeline.audioTracks()[0]->selectedClips() == QList<int>({0})
            && excludedEmpty();
        timeline.setPlayheadPosition(3.0); // The straddling clip belongs to neither side.
        timeline.selectClipsFromPlayhead(true);
        ok = ok && timeline.videoTracks()[0]->selectedClips() == QList<int>({2});
        timeline.selectClipsFromPlayhead(false);
        ok = ok && timeline.videoTracks()[0]->selectedClips() == QList<int>({0});
        timeline.setPlayheadPosition(4.5); // Gap between the second and third clips.
        timeline.selectClipsFromPlayhead(true);
        ok = ok && timeline.videoTracks()[0]->selectedClips() == QList<int>({2});
        timeline.selectClipsFromPlayhead(false);
        ok = ok && timeline.videoTracks()[0]->selectedClips() == QList<int>({0, 1});
        gate(2, ok && !timeline.canUndo());
    }

    bool bladeOk = false;
    {
        Timeline timeline;
        const QVector<ClipInfo> clips{clip(4.0, 1.0, 1), clip(2.0, 1.0)};
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{clips, clips, clips},
                                    QVector<QVector<ClipInfo>>{clips, clips},
                                    3.0, -1.0, -1.0, 100);
        timeline.videoTracks()[1]->setLocked(true);
        timeline.audioTracks()[1]->setLocked(true);
        timeline.videoTracks()[2]->setHidden(true); // Hidden, unlocked tracks are bladed.
        timeline.clearSelection();
        baseline(timeline);
        const TimelineState before = timeline.currentState();
        const int undoIndex = timeline.undoManager()->currentIndex();
        bool ok = !timeline.hasAnySelection();
        timeline.bladeAllTracksAtPlayhead();
        const auto v = timeline.videoTracks()[0]->clips();
        const auto a = timeline.audioTracks()[0]->clips();
        ok = ok && v.size() == 3 && a.size() == 3
            && timeline.videoTracks()[1]->clipCount() == 2
            && timeline.audioTracks()[1]->clipCount() == 2
            && timeline.videoTracks()[2]->clipCount() == 3
            && timeline.undoManager()->currentIndex() == undoIndex + 1;
        if (v.size() == 3 && a.size() == 3) {
            ok = ok && near(v[0].effectiveDuration(), 2.0) && near(v[1].effectiveDuration(), 2.0)
                && near(a[0].effectiveDuration(), 2.0) && near(a[1].effectiveDuration(), 2.0)
                && near(v[0].leadInSec, 1.0) && near(v[1].leadInSec, 0.0)
                && near(v[2].leadInSec, 1.0) && v[0].linkGroup == 1
                && v[1].linkGroup > 0 && v[1].linkGroup != 1
                && v[1].linkGroup == a[1].linkGroup;
        }
        timeline.bladeAllTracksAtPlayhead(); // New cut boundary is a no-op.
        ok = ok && timeline.undoManager()->currentIndex() == undoIndex + 1;
        timeline.undo();
        const TimelineState restored = timeline.currentState();
        ok = ok && sameTracks(before.videoTracks, restored.videoTracks)
            && sameTracks(before.audioTracks, restored.audioTracks) && !timeline.canUndo();
        int noOpMessages = 0;
        QObject::connect(&timeline, &Timeline::statusMessageRequested, &timeline,
                         [&](const QString &message, int) { if (!message.isEmpty()) ++noOpMessages; });
        for (double position : {1.0, 5.0, 5.5, 6.0, 8.0}) {
            timeline.setPlayheadPosition(position);
            timeline.bladeAllTracksAtPlayhead();
        }
        ok = ok && noOpMessages == 5 && !timeline.canUndo()
            && sameTracks(before.videoTracks, timeline.currentState().videoTracks)
            && sameTracks(before.audioTracks, timeline.currentState().audioTracks);
        bladeOk = ok;
    }
    {
        Timeline timeline;
        const QVector<ClipInfo> clips{clip(4.0), clip(2.0)};
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{clips},
                                    QVector<QVector<ClipInfo>>{},
                                    2.0, -1.0, -1.0, 100);
        timeline.videoTracks()[0]->setSelectedClip(1);
        baseline(timeline);
        const TimelineState before = timeline.currentState();
        const int undoIndex = timeline.undoManager()->currentIndex();
        int primaryNotifications = 0;
        int trackNotifications = 0;
        QObject::connect(&timeline, &Timeline::clipSelected, &timeline,
                         [&](int primary) { if (primary == 2) ++primaryNotifications; });
        QObject::connect(&timeline, &Timeline::clipSelectedOnTrack, &timeline,
                         [&](int trackIndex, int primary) {
                             if (trackIndex == 0 && primary == 2) ++trackNotifications;
                         });
        timeline.bladeAllTracksAtPlayhead();
        bool ok = primaryNotifications >= 1 && trackNotifications >= 1
            && timeline.videoTracks()[0]->selectedClips() == QList<int>({2})
            && timeline.videoTracks()[0]->clipCount() == 3
            && timeline.undoManager()->currentIndex() == undoIndex + 1;
        timeline.undo();
        const TimelineState restored = timeline.currentState();
        ok = ok && sameTracks(before.videoTracks, restored.videoTracks)
            && sameTracks(before.audioTracks, restored.audioTracks)
            && timeline.videoTracks()[0]->selectedClips() == QList<int>({1})
            && timeline.undoManager()->currentIndex() == undoIndex && !timeline.canUndo();
        gate(3, bladeOk && ok);
    }
    {
        Timeline timeline;
        // Common gaps [2,4), [6,8); only V2 is empty at [0.5,1).
        const QVector<ClipInfo> full{clip(2.0), clip(2.0, 2.0), clip(2.0, 2.0)};
        const QVector<ClipInfo> partial{clip(0.5), clip(1.0, 0.5), clip(2.0, 2.0), clip(2.0, 2.0)};
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{full, partial},
                                    QVector<QVector<ClipInfo>>{full, full},
                                    0.0, -1.0, -1.0, 100);
        timeline.audioTracks()[1]->setLocked(true);
        baseline(timeline);
        const auto before = timeline.currentState();
        const auto serial = timeline.undoManager()->saveSerial();
        timeline.closeAllGaps();
        bool ok = timeline.undoManager()->saveSerial() == serial + 1;
        for (auto *track : {timeline.videoTracks()[0], timeline.audioTracks()[0]}) {
            const auto &c = track->clips();
            ok = ok && c.size() == 3 && near(c[1].leadInSec, 0.0) && near(c[2].leadInSec, 0.0);
        }
        const auto &v2 = timeline.videoTracks()[1]->clips();
        ok = ok && v2.size() == 4 && near(v2[1].leadInSec, 0.5)
            && near(v2[2].leadInSec, 0.0) && near(v2[3].leadInSec, 0.0)
            && sameTracks({full}, {timeline.audioTracks()[1]->clips()});
        timeline.closeAllGaps();
        ok = ok && timeline.undoManager()->saveSerial() == serial + 1;
        timeline.undo();
        ok = ok && sameTracks(before.videoTracks, timeline.currentState().videoTracks)
            && sameTracks(before.audioTracks, timeline.currentState().audioTracks) && !timeline.canUndo();
        gate(4, ok);
    }
    {
        Timeline timeline;
        ClipInfo source = clip(2.0, 1.0, 7);
        source.opacity = 0.65;
        source.videoScale = 1.3;
        const QVector<ClipInfo> linked{source, clip(1.0, 4.0)};
        const QVector<ClipInfo> crowded{clip(2.0), clip(3.0)};
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{linked, crowded},
                                    QVector<QVector<ClipInfo>>{linked},
                                    0.0, -1.0, -1.0, 100);
        timeline.clearSelection();
        timeline.videoTracks()[0]->setSelectedClip(0);
        // Block cross-track selection clearing to explicitly exercise multi-selection.
        {
            const QSignalBlocker blocker(timeline.videoTracks()[1]);
            timeline.videoTracks()[1]->setSelectedClip(0);
        }
        baseline(timeline);
        const auto before = timeline.currentState();
        const auto serial = timeline.undoManager()->saveSerial();
        timeline.duplicateSelectedClips();
        const auto &v = timeline.videoTracks()[0]->clips();
        const auto &a = timeline.audioTracks()[0]->clips();
        const auto &v2 = timeline.videoTracks()[1]->clips();
        bool ok = v.size() == 3 && a.size() == 3 && v2.size() == 3
            && timeline.undoManager()->saveSerial() == serial + 1;
        if (v.size() == 3 && a.size() == 3 && v2.size() == 3) {
            ok = ok && near(v[1].leadInSec, 0.0) && near(v[2].leadInSec, 2.0)
                && v[1].linkGroup > 0 && v[1].linkGroup != 7 && v[1].linkGroup == a[1].linkGroup
                && sameClipPayload(source, v[1]) && sameClipPayload(source, a[1])
                && sameClipPayload(crowded[0], v2[2]) && near(v2[2].leadInSec, 0.0);
        }
        timeline.undo();
        ok = ok && sameTracks(before.videoTracks, timeline.currentState().videoTracks)
            && sameTracks(before.audioTracks, timeline.currentState().audioTracks) && !timeline.canUndo();
        gate(5, ok);
    }
    {
        bool ok = true;
        for (double fps : {24.0, 30.0, 60.0}) {
            for (int frames : {1, -1, 10}) {
                Timeline timeline;
                const QVector<ClipInfo> clips{clip(2.0, 1.0, 9), clip(2.0, 5.0)};
                timeline.restoreFromProject(QVector<QVector<ClipInfo>>{clips},
                                            QVector<QVector<ClipInfo>>{clips},
                                            0.0, -1.0, -1.0, 100);
                timeline.setNudgeFrameRate(fps);
                timeline.selectAllClips();
                baseline(timeline);
                const auto before = timeline.currentState();
                const auto serial = timeline.undoManager()->saveSerial();
                timeline.nudgeSelectedClips(frames);
                const auto &v = timeline.videoTracks()[0]->clips();
                const auto &a = timeline.audioTracks()[0]->clips();
                ok = ok && near(v[0].leadInSec, 1.0 + double(frames) / fps)
                    && near(v[1].leadInSec, 5.0) && near(a[0].leadInSec, v[0].leadInSec)
                    && near(a[1].leadInSec, 5.0)
                    && timeline.videoTracks()[0]->selectedClips().size() == 2
                    && timeline.undoManager()->saveSerial() == serial + 1;
                timeline.undo();
                ok = ok && sameTracks(before.videoTracks, timeline.currentState().videoTracks)
                    && sameTracks(before.audioTracks, timeline.currentState().audioTracks) && !timeline.canUndo();
            }
        }
        Timeline timeline;
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip(2.0, 0.01), clip(2.0, 0.01)}},
                                    QVector<QVector<ClipInfo>>{},
                                    0.0, -1.0, -1.0, 100);
        timeline.videoTracks()[0]->setSelectedClip(0);
        timeline.setNudgeFrameRate(30.0);
        baseline(timeline);
        const auto before = timeline.currentState();
        int messages = 0;
        QObject::connect(&timeline, &Timeline::statusMessageRequested, &timeline,
                         [&](const QString &, int) { ++messages; });
        timeline.nudgeSelectedClips(1); // Collision: rejected, candidate reported, no undo.
        ok = ok && messages == 1 && !timeline.canUndo()
            && sameTracks(before.videoTracks, timeline.currentState().videoTracks);
        timeline.nudgeSelectedClips(-1); // Clamp the requested negative start to zero.
        ok = ok && messages == 2 && near(timeline.videoTracks()[0]->clips()[0].leadInSec, 0.0)
            && timeline.canUndo();
        timeline.undo();
        ok = ok && !timeline.canUndo() && sameTracks(before.videoTracks, timeline.currentState().videoTracks);
        gate(6, ok);
    }
    {
        Timeline timeline;
        const QVector<ClipInfo> video{clip(0.5), clip(2.5, 0.25, 17), clip(1.0, 2.0)};
        const QVector<ClipInfo> audio{clip(0.5), clip(1.0, 1.5), clip(1.0, 3.0, 17)};
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{video, video, video},
                                    QVector<QVector<ClipInfo>>{audio}, 0.0, -1.0, -1.0, 100);
        timeline.videoTracks()[1]->setLocked(true);
        timeline.videoTracks()[2]->setHidden(true);
        baseline(timeline);
        const auto serial = timeline.undoManager()->saveSerial();
        timeline.selectClipsInRange(1.0, 3.0, 0, 3, false);
        bool ok = timeline.videoTracks()[0]->selectedClips() == QList<int>({1})
            && timeline.videoTracks()[1]->selectedClips().isEmpty()
            && timeline.videoTracks()[2]->selectedClips().isEmpty()
            && timeline.audioTracks()[0]->selectedClips() == QList<int>({1, 2});
        timeline.selectClipsInRange(5.0, 6.0, 0, 0, true);
        ok = ok && timeline.videoTracks()[0]->selectedClips() == QList<int>({1, 2})
            && timeline.audioTracks()[0]->selectedClips() == QList<int>({1, 2});
        timeline.selectClipsInRange(0.0, 0.5, 0, 0, false);
        ok = ok && timeline.videoTracks()[0]->selectedClips() == QList<int>({0})
            && timeline.audioTracks()[0]->selectedClips().isEmpty();
        gate(7, ok && timeline.undoManager()->saveSerial() == serial && !timeline.canUndo());
    }
    {
        Timeline timeline;
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip(10.0), clip(20.0, 40.0), clip(50.0)}},
                                    QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
        timeline.resize(1200, 400);
        timeline.show();
        QApplication::processEvents();
        auto *scroll = timeline.findChild<QScrollArea *>();
        auto *track = timeline.videoTracks()[0];
        timeline.setZoomLevel(1.0);
        timeline.zoomToFitSequence();
        bool ok = scroll && track->pixelsPerSecond() > 1.0
            && timeline.totalDuration() * track->pixelsPerSecond() <= scroll->viewport()->width()
            && scroll->horizontalScrollBar()->value() == 0;
        track->setSelectedClip(1);
        ok = timeline.zoomToSelection() && ok;
        QApplication::processEvents();
        if (scroll) {
            const int left = scroll->horizontalScrollBar()->value();
            ok = ok && 50.0 * track->pixelsPerSecond() >= left
                && 70.0 * track->pixelsPerSecond() <= left + scroll->viewport()->width()
                && left > 0;
        }
        timeline.clearSelection();
        gate(8, ok && !timeline.zoomToSelection());
    }
    {
        double result = 0.0;
        const auto parses = [&](const QString &text, double fps, double current, double expected) {
            return parseTimecodeInput(text, fps, current, &result) && near(result, expected);
        };
        bool ok = parses(QStringLiteral("00:01:30:15"), 30.0, 0.0, 90.5)
            && parses(QStringLiteral("1:30"), 30.0, 0.0, 90.0)
            && parses(QStringLiteral("12.5"), 30.0, 0.0, 12.5)
            && parses(QStringLiteral("+1:00"), 30.0, 12.0, 72.0)
            && parses(QStringLiteral("-10"), 30.0, 12.0, 2.0)
            && parses(QStringLiteral("00:01:30:15"), 29.97, 0.0, 90.0 + 15.0 / 29.97)
            && parses(QStringLiteral("01:30:15"), 30.0, 0.0, 90.5)
            && !parseTimecodeInput(QStringLiteral("abc"), 30.0, 0.0, &result);
        for (const QString &invalid : {QString(), QStringLiteral("1:60"), QStringLiteral("0:00:30"),
                                      QStringLiteral("1::2"), QStringLiteral("nan"), QStringLiteral("--10")})
            ok = ok && !parseTimecodeInput(invalid, 30.0, 0.0, &result);
        gate(9, ok && !parseTimecodeInput(QStringLiteral("1"), 0.0, 0.0, &result));
    }
    std::fprintf(stderr, "[timeline-ergo] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
