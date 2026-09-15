#include "../Timeline.h"
#include "../UndoManager.h"

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
        gate(3, ok);
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
        gate(4, ok);
    }
    std::fprintf(stderr, "[timeline-ergo] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
