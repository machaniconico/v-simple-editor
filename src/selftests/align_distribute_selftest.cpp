#include "../Timeline.h"
#include "../UndoManager.h"
#include "../ClipGeometry.h"
#include <QRectF>
#include <cmath>
#include <cstdio>

namespace {
using Arrangement = Timeline::LayerArrangement;
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }
ClipInfo makeClip(double scale, double dx, double dy, double rotation = 0.0)
{
    ClipInfo clip;
    // Geometry-only tests never decode media.
    clip.filePath = QStringLiteral("/selftests/align-distribute.mp4");
    clip.duration = clip.outPoint = 5.0;
    clip.videoScale = scale;
    clip.videoDx = dx;
    clip.videoDy = dy;
    clip.rotation2DDegrees = rotation;
    return clip;
}
void setup(Timeline &timeline, const QVector<ClipInfo> &clips)
{
    QVector<QVector<ClipInfo>> video;
    for (const auto &clip : clips) video.append(QVector<ClipInfo>{clip});
    timeline.restoreFromProject(video, QVector<QVector<ClipInfo>>{{makeClip(1, 0.2, 0.3)}},
                                0.0, -1.0, -1.0, 100);
    timeline.setProjectOutputConfig(1000, 500, true);
    timeline.setLinkedSelectionEnabled(false);
    timeline.clearSelection();
    for (auto *track : timeline.videoTracks())
        if (track->clipCount()) track->toggleClipSelection(0);
    timeline.audioTracks()[0]->toggleClipSelection(0);
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("基準"));
}
QRectF bounds(const ClipInfo &clip)
{
    return clipgeom::resolveTransform({clip.videoScale, clip.videoDx, clip.videoDy,
                                       clip.rotation2DDegrees}, QSize(1000, 500))
        .mapRect(QRectF(0, 0, 1000, 500));
}
}

int runAlignDistributeSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "[align-distribute] %s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    Timeline timeline;
    bool ok = true;
    // A single selection aligns to the canvas; widths include scale.
    for (const auto arrangement : {Arrangement::Left, Arrangement::HorizontalCenter, Arrangement::Right}) {
        setup(timeline, {makeClip(0.4, 0.17, -0.13)});
        ok = timeline.arrangeSelectedLayers(arrangement) && ok;
        const auto &clip = timeline.videoTracks()[0]->clips()[0];
        const double expected = arrangement == Arrangement::Left ? -0.3
                              : arrangement == Arrangement::Right ? 0.3 : 0.0;
        ok = near(clip.videoDx, expected) && near(clip.videoDy, -0.13) && ok;
    }
    // A quarter-turn makes the horizontal bounding width 200, not 400.
    setup(timeline, {makeClip(0.4, 0.1, 0.2, 90.0)});
    ok = timeline.arrangeSelectedLayers(Arrangement::Left) && ok;
    ok = near(timeline.videoTracks()[0]->clips()[0].videoDx, -0.4) && ok;
    gate(1, ok);

    ok = true;
    for (const auto arrangement : {Arrangement::Top, Arrangement::VerticalCenter, Arrangement::Bottom}) {
        setup(timeline, {makeClip(0.4, 0.17, -0.13)});
        ok = timeline.arrangeSelectedLayers(arrangement) && ok;
        const auto &clip = timeline.videoTracks()[0]->clips()[0];
        const double expected = arrangement == Arrangement::Top ? -0.3
                              : arrangement == Arrangement::Bottom ? 0.3 : 0.0;
        ok = near(clip.videoDy, expected) && near(clip.videoDx, 0.17) && ok;
    }
    setup(timeline, {makeClip(0.4, 0.1, 0.2, 90.0)});
    ok = timeline.arrangeSelectedLayers(Arrangement::Bottom) && ok;
    ok = near(timeline.videoTracks()[0]->clips()[0].videoDy, 0.1) && ok;
    gate(2, ok);

    ok = true;
    for (const auto arrangement : {Arrangement::DistributeHorizontal, Arrangement::DistributeVertical}) {
        // Deliberately not in spatial order, with unequal sizes.
        setup(timeline, {makeClip(0.1, 0.4, 0.4), makeClip(0.2, -0.05, -0.05),
                         makeClip(0.3, -0.4, -0.4)});
        const auto serial = timeline.undoManager()->saveSerial();
        ok = timeline.canArrangeSelectedLayers(arrangement)
            && timeline.arrangeSelectedLayers(arrangement) && ok;
        const auto &right = timeline.videoTracks()[0]->clips()[0];
        const auto &middle = timeline.videoTracks()[1]->clips()[0];
        const auto &left = timeline.videoTracks()[2]->clips()[0];
        const QRectF a = bounds(left), b = bounds(middle), c = bounds(right);
        const bool horizontal = arrangement == Arrangement::DistributeHorizontal;
        ok = (horizontal ? near(b.left() - a.right(), c.left() - b.right())
                         : near(b.top() - a.bottom(), c.top() - b.bottom())) && ok;
        ok = near(right.videoDx, 0.4) && near(right.videoDy, 0.4)
            && near(left.videoDx, -0.4) && near(left.videoDy, -0.4)
            && near(horizontal ? middle.videoDy : middle.videoDx, -0.05)
            && timeline.undoManager()->saveSerial() == serial + 1 && ok;
    }
    gate(3, ok);

    ok = true;
    for (int count = 0; count <= 2; ++count) {
        setup(timeline, {makeClip(0.2, 0.1, 0.2), makeClip(0.3, 0.2, 0.3)});
        timeline.clearSelection();
        for (int i = 0; i < count; ++i) timeline.videoTracks()[i]->toggleClipSelection(0);
        const auto serial = timeline.undoManager()->saveSerial();
        for (const auto arrangement : {Arrangement::DistributeHorizontal, Arrangement::DistributeVertical})
            ok = !timeline.canArrangeSelectedLayers(arrangement)
                && !timeline.arrangeSelectedLayers(arrangement) && ok;
        ok = timeline.undoManager()->saveSerial() == serial && ok;
    }
    ClipInfo nullClip = makeClip(0.3, 0.2, 0.3);
    nullClip.filePath = clipgeom::nullObjectFilePath();
    setup(timeline, {makeClip(0.2, 0.1, 0.2, 30), makeClip(0.3, 0.2, 0.3, -15), nullClip});
    ok = timeline.selectedArrangementTargets().size() == 2
        && !timeline.canArrangeSelectedLayers(Arrangement::DistributeHorizontal) && ok;
    const auto before = timeline.currentState();
    const auto serial = timeline.undoManager()->saveSerial();
    ok = timeline.arrangeSelectedLayers(Arrangement::Left) && ok;
    ok = timeline.undoManager()->saveSerial() == serial + 1 && ok;
    for (int i = 0; i < 2; ++i) {
        const auto &clip = timeline.videoTracks()[i]->clips()[0];
        ok = near(bounds(clip).left(), 0.0) && near(clip.videoScale, before.videoTracks[i][0].videoScale)
            && near(clip.rotation2DDegrees, before.videoTracks[i][0].rotation2DDegrees) && ok;
    }
    ok = near(timeline.videoTracks()[2]->clips()[0].videoDx, nullClip.videoDx)
        && near(timeline.audioTracks()[0]->clips()[0].videoDx, 0.2) && ok;
    timeline.undo();
    for (int i = 0; i < 3; ++i) {
        const auto &clip = timeline.videoTracks()[i]->clips()[0];
        const auto &original = before.videoTracks[i][0];
        ok = near(clip.videoDx, original.videoDx) && near(clip.videoDy, original.videoDy)
            && near(clip.videoScale, original.videoScale)
            && near(clip.rotation2DDegrees, original.rotation2DDegrees) && ok;
    }
    ok = !timeline.canUndo() && ok;
    // The same selection API must also retain multiple clips on one track.
    setup(timeline, {makeClip(0.2, 0.1, 0.2)});
    auto *track = timeline.videoTracks()[0];
    track->setClips({makeClip(0.2, 0.1, 0.2), makeClip(0.4, -0.1, -0.2)});
    timeline.clearSelection();
    track->toggleClipSelection(0);
    track->toggleClipSelection(1);
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("同一トラック基準"));
    const auto sameTrackSerial = timeline.undoManager()->saveSerial();
    ok = timeline.arrangeSelectedLayers(Arrangement::Right) && ok;
    ok = near(track->clips()[0].videoDx, 0.4) && near(track->clips()[1].videoDx, 0.3)
        && timeline.undoManager()->saveSerial() == sameTrackSerial + 1 && ok;
    timeline.undo();
    track = timeline.videoTracks()[0];
    ok = near(track->clips()[0].videoDx, 0.1) && near(track->clips()[1].videoDx, -0.1) && ok;
    gate(4, ok && !timeline.canUndo());
    std::fprintf(stderr, "[align-distribute] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
