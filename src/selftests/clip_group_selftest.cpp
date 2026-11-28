#include "../Timeline.h"
#include "../UndoManager.h"
#include "../TrimOps.h"
#include <cmath>
#include <cstdio>

namespace {
ClipInfo makeClip(int group = 0)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("/selftests/clip-group.mp4");
    clip.duration = 10.0;
    clip.inPoint = 1.0;
    clip.outPoint = 5.0;
    clip.leadInSec = 2.0;
    clip.linkGroup = group;
    return clip;
}
void setup(Timeline &timeline)
{
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{makeClip()}, {makeClip()}},
        QVector<QVector<ClipInfo>>{{makeClip()}, {}}, 0.0, -1.0, -1.0, 100);
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("基準"));
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
}

int runClipGroupSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "[clip-group] %s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    const QVector<Timeline::GroupClipTarget> targets{{false, 0, 0}, {false, 1, 0}, {true, 0, 0}};
    Timeline timeline;
    setup(timeline);
    auto *v1 = timeline.videoTracks()[0];
    auto *v2 = timeline.videoTracks()[1];
    auto *a1 = timeline.audioTracks()[0];
    QString error;
    v1->setSelectedClip(0);
    v2->toggleClipSelection(0);
    a1->toggleClipSelection(0);
    bool ok = timeline.setClipGrouping(timeline.selectedGroupClipTargets(), true, &error);
    const int group = v1->clips()[0].linkGroup;
    timeline.clearSelection();
    v1->setSelectedClip(0);
    gate(1, ok && group > 0 && v2->clips()[0].linkGroup == group
        && a1->clips()[0].linkGroup == group && v2->isClipSelected(0) && a1->isClipSelected(0));

    // Exercise the production signal connections used by a horizontal drag.
    v1->linkedDragStarted(0);
    v1->linkedDragDelta(0, 0.75);
    ok = near(v1->clips()[0].leadInSec, 2.75)
        && near(v2->clips()[0].leadInSec, 2.75) && near(a1->clips()[0].leadInSec, 2.75);
    v1->linkedDragCancelled();
    ok = timeline.applyTrimActive(trimops::TrimType::Slip, 0.25, &error) && ok;
    ok = ok && near(v1->clips()[0].inPoint, 1.25) && near(v2->clips()[0].inPoint, 1.25)
        && near(a1->clips()[0].inPoint, 1.25);
    timeline.deleteSelectedClip();
    ok = ok && v1->clipCount() == 0 && v2->clipCount() == 0 && a1->clipCount() == 0;
    gate(2, ok);

    setup(timeline);
    timeline.setClipGrouping(targets, true, &error);
    ok = timeline.setClipGrouping(targets, false, &error);
    // restoreFromProject may rebuild track widgets; resolve them afresh.
    v1 = timeline.videoTracks()[0]; v2 = timeline.videoTracks()[1]; a1 = timeline.audioTracks()[0];
    timeline.clearSelection();
    v1->setSelectedClip(0);
    v1->linkedDragStarted(0);
    v1->linkedDragDelta(0, 0.5);
    gate(3, ok && v1->clips()[0].linkGroup == 0 && v2->clips()[0].linkGroup == 0
        && a1->clips()[0].linkGroup == 0 && !v2->isClipSelected(0) && !a1->isClipSelected(0)
        && near(v2->clips()[0].leadInSec, 2.0) && near(a1->clips()[0].leadInSec, 2.0));
    v1->linkedDragCancelled();

    setup(timeline);
    timeline.videoTracks()[0]->setClips({makeClip(), makeClip(77)});
    timeline.audioTracks()[0]->setClips({makeClip(), makeClip(77)});
    timeline.setClipParentEntries({{QStringLiteral("0:0"), QStringLiteral("1:0")}});
    TimelineTrackMatteEntry matte;
    matte.matteSourceClipId = QStringLiteral("1:0");
    timeline.setTrackMatteEntries({{QStringLiteral("0:0"), matte}});
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("関連データ基準"));
    const int before = timeline.undoManager()->currentIndex();
    ok = timeline.setClipGrouping(targets, true, &error);
    const int allocated = timeline.videoTracks()[0]->clips()[0].linkGroup;
    ok = ok && allocated != 77 && timeline.undoManager()->currentIndex() == before + 1;
    ok = ok && timeline.clipParentEntries().value(QStringLiteral("0:0")) == QStringLiteral("1:0")
        && timeline.trackMatteEntries().value(QStringLiteral("0:0")).matteSourceClipId == QStringLiteral("1:0");
    ok = timeline.setClipGrouping(targets, false, &error) && ok;
    ok = ok && timeline.clipParentEntries().value(QStringLiteral("0:0")) == QStringLiteral("1:0")
        && timeline.trackMatteEntries().value(QStringLiteral("0:0")).matteSourceClipId == QStringLiteral("1:0");
    ok = ok && timeline.undoManager()->currentIndex() == before + 2;
    timeline.undo();
    for (const auto &target : targets)
        ok = ok && timeline.trackAt(target.audio, target.trackIndex)->clips()[target.clipIndex].linkGroup == allocated;
    timeline.undo();
    for (const auto &target : targets)
        ok = ok && timeline.trackAt(target.audio, target.trackIndex)->clips()[target.clipIndex].linkGroup == 0;
    gate(4, ok && timeline.videoTracks()[0]->clips()[1].linkGroup == 77
        && timeline.audioTracks()[0]->clips()[1].linkGroup == 77 && !timeline.canUndo());

    timeline.clearSelection();
    timeline.setLinkedSelectionEnabled(false);
    timeline.videoTracks()[0]->setSelectedClip(0);
    timeline.videoTracks()[0]->toggleClipSelection(1);
    const auto serial = timeline.undoManager()->saveSerial();
    const auto selected = timeline.selectedGroupClipTargets();
    ok = !timeline.canGroupClips(selected, &error) && !error.isEmpty()
        && !timeline.setClipGrouping(selected, true, &error)
        && timeline.undoManager()->saveSerial() == serial;
    // Existing V1 -> V2 drop signals move the AV partner A1 -> A2.
    const ClipInfo avVideo = timeline.videoTracks()[0]->clips()[1];
    timeline.videoTracks()[0]->removeClipPreservingDownstream(1);
    timeline.videoTracks()[1]->insertClipPreservingDownstream(1, avVideo, 1.0);
    timeline.videoTracks()[1]->crossTrackDropped(1, 77, 3.0);
    ok = ok && timeline.audioTracks()[0]->clipCount() == 1
        && timeline.audioTracks()[1]->clipCount() == 1
        && timeline.audioTracks()[1]->clips()[0].linkGroup == 77
        && near(timeline.audioTracks()[1]->clips()[0].leadInSec, 3.0);
    timeline.setClipGrouping(targets, true, &error);
    timeline.clearSelection();
    timeline.videoTracks()[0]->setSelectedClip(0);
    ok = ok && !timeline.videoTracks()[1]->isClipSelected(0) && !timeline.audioTracks()[0]->isClipSelected(0);
    timeline.setLinkedSelectionEnabled(true);
    timeline.clearSelection();
    timeline.videoTracks()[0]->setSelectedClip(0);
    gate(5, ok && timeline.videoTracks()[1]->isClipSelected(0) && timeline.audioTracks()[0]->isClipSelected(0));
    std::fprintf(stderr, "[clip-group] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
