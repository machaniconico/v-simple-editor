#include "../Timeline.h"
#include "../UndoManager.h"
#include "../ClipGeometry.h"
#include "../Keyframe.h"
#include "../TimelineFrameRenderer.h"
#include "../TrackMatteKey.h"
#include "../clipanim/ClipAnim.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"
#include <QColor>
#include <QImage>
#include <QPair>
#include <QRectF>
#include <QStringList>
#include <QTemporaryDir>
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

constexpr int kRenderW = 400;
constexpr int kRenderH = 200;
constexpr int kClipFps = 10;
constexpr double kClipSeconds = 3.0;

// mpeg4 material through FrameEncoder (common rule 3d), opaque everywhere.
bool writeSyntheticClip(const QString &path)
{
    libavcore::EncodeRequest req;
    req.width = 40;
    req.height = 24;
    req.fps = kClipFps;
    req.fpsNum = kClipFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 600000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(req)) return false;
    for (int i = 0; i < int(kClipSeconds * kClipFps); ++i) {
        QImage frame(req.width, req.height, QImage::Format_RGB888);
        frame.fill(QColor(200, (i * 7) & 255, 60));
        if (!encoder.pushFrame(frame, i)) return false;
    }
    return !encoder.finalize();
}
ClipInfo mediaClip(const QString &path, double scale, double dx, double dy)
{
    ClipInfo clip = makeClip(scale, dx, dy);
    clip.filePath = path;
    clip.duration = clip.outPoint = kClipSeconds;
    return clip;
}
ClipInfo makeNullObject(double dx = 0.0)
{
    ClipInfo clip = makeClip(1.0, dx, 0.0);
    clip.filePath = clipgeom::nullObjectFilePath();
    clip.duration = clip.outPoint = kClipSeconds;
    clip.opacity = 0.0;
    clip.visible = false;
    return clip;
}
// V1 carries a Null Object so the frame is not built on an empty V1 (rule 3c).
void setupRender(Timeline &timeline, const QVector<ClipInfo> &upper, double playhead)
{
    QVector<QVector<ClipInfo>> video{{makeNullObject()}};
    for (const auto &clip : upper) video.append(QVector<ClipInfo>{clip});
    timeline.restoreFromProject(video, QVector<QVector<ClipInfo>>{}, playhead, -1.0, -1.0, 100);
    timeline.setProjectOutputConfig(kRenderW, kRenderH, true);
    timeline.setLinkedSelectionEnabled(false);
    timeline.clearSelection();
    for (auto *track : timeline.videoTracks())
        if (track->clipCount()) track->toggleClipSelection(0);
}
void finishSetup(Timeline &timeline)
{
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("基準"));
}
// Horizontal extent of non-transparent pixels in rows [y0, y1); {-1, -1} if none.
QPair<int, int> opaqueColumns(const QImage &frame, int y0, int y1)
{
    int minX = -1, maxX = -1;
    const QImage image = frame.convertToFormat(QImage::Format_ARGB32);
    for (int y = qMax(0, y0); y < qMin(image.height(), y1); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(row[x]) == 0) continue;
            if (minX < 0 || x < minX) minX = x;
            if (x > maxX) maxX = x;
        }
    }
    return {minX, maxX};
}
QImage renderAt(const Timeline &timeline, double seconds)
{
    return tlrender::renderFrameAt(&timeline, qint64(seconds * 1000000.0),
                                   QSize(kRenderW, kRenderH));
}
// Rendered center (center=true) or right edge of an opaqueColumns() span.
double edgeBefore(const QPair<int, int> &span, bool center)
{
    return center ? (span.first + span.second + 1) * 0.5 : span.second + 1.0;
}
const KeyframeTrack *positionX(const ClipInfo &clip)
{
    return clip.keyframes.track(QStringLiteral("motion.position.x"));
}
double keyAt(const KeyframeTrack *track, double time)
{
    for (const auto &key : track->keyframes())
        if (std::abs(key.time - time) < 0.001) return key.value;
    return std::nan("");
}
}

int runAlignDistributeSelftest()
{
    selftest::GateTally tally;
    const auto gate = [&](int n, bool ok) { tally.gate(n, ok); };
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

    QTemporaryDir dir;
    const QString media = dir.path() + QStringLiteral("/align-distribute.mp4");
    const bool haveMedia = dir.isValid() && writeSyntheticClip(media);
    std::fprintf(stderr, "[align-distribute] media=%d\n", haveMedia ? 1 : 0);

    ok = true;
    // G5: a child of a Null Object (videoDx=+0.2) and an unparented clip,
    // left-aligned, share the same left pixel column in the rendered frame.
    {
        Timeline parented;
        setupRender(parented, {makeNullObject(0.2), mediaClip(media, 0.3, 0.1, -0.25),
                               mediaClip(media, 0.3, -0.15, 0.25)}, 0.0);
        parented.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
        finishSetup(parented);
        const QPair<int, int> beforeChild = opaqueColumns(renderAt(parented, 0.0), 0, kRenderH / 2);
        const auto serial = parented.undoManager()->saveSerial();
        const bool arranged = haveMedia && parented.arrangeSelectedLayers(Arrangement::Left);
        const QImage frame = renderAt(parented, 0.0);
        const QPair<int, int> child = opaqueColumns(frame, 0, kRenderH / 2);
        const QPair<int, int> plain = opaqueColumns(frame, kRenderH / 2, kRenderH);
        const bool oneUndo = parented.undoManager()->saveSerial() == serial + 1;
        parented.undo();
        const bool undone = near(parented.videoTracks()[2]->clips()[0].videoDx, 0.1)
            && near(parented.videoTracks()[3]->clips()[0].videoDx, -0.15);
        // A child pushed past the left edge would also read 0, so its width must survive.
        const int widthBefore = beforeChild.second - beforeChild.first;
        const int widthAfter = child.second - child.first;
        std::fprintf(stderr, "[align-distribute] G5a arranged=%d childLeftBefore=%d childLeft=%d "
                     "plainLeft=%d childWidthBefore=%d childWidth=%d oneUndo=%d undone=%d\n",
                     arranged, beforeChild.first, child.first, plain.first, widthBefore,
                     widthAfter, oneUndo, undone);
        ok = arranged && child.first >= 0 && plain.first >= 0
            && std::abs(child.first - plain.first) <= 1 && plain.first <= 1
            && std::abs(widthAfter - widthBefore) <= 1
            && beforeChild.first > plain.first + 10 && oneUndo && undone && ok;
    }
    // G5b: a regular parent selected together with its child. The parent moves
    // first, so the child must be solved against the moved parent.
    // Bands (rows): parent 0..50, child 50..100, plain 150..200.
    {
        Timeline both;
        setupRender(both, {mediaClip(media, 0.25, 0.2, -0.375), mediaClip(media, 1.0, 0.4, 1.0),
                           mediaClip(media, 0.25, -0.1, 0.375)}, 0.0);
        both.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
        finishSetup(both);
        const QImage beforeFrame = renderAt(both, 0.0);
        const QPair<int, int> childBefore = opaqueColumns(beforeFrame, 60, 90);
        const bool arranged = haveMedia && both.selectedArrangementTargets().size() == 3
            && both.arrangeSelectedLayers(Arrangement::Left);
        const QImage frame = renderAt(both, 0.0);
        const QPair<int, int> parent = opaqueColumns(frame, 10, 40);
        const QPair<int, int> child = opaqueColumns(frame, 60, 90);
        const QPair<int, int> plain = opaqueColumns(frame, 160, 190);
        const int widthBefore = childBefore.second - childBefore.first;
        const int widthAfter = child.second - child.first;
        std::fprintf(stderr, "[align-distribute] G5b arranged=%d parentLeft=%d childLeftBefore=%d "
                     "childLeft=%d plainLeft=%d childWidthBefore=%d childWidth=%d\n", arranged,
                     parent.first, childBefore.first, child.first, plain.first, widthBefore,
                     widthAfter);
        ok = arranged && parent.first >= 0 && child.first >= 0 && plain.first >= 0
            && plain.first <= 1 && std::abs(parent.first - plain.first) <= 1
            && std::abs(child.first - plain.first) <= 1
            && std::abs(widthAfter - widthBefore) <= 1
            && childBefore.first > plain.first + 10 && ok;
    }
    // G5c: a hidden regular clip is not a parent layer in the renderer, so its
    // child renders (and must be aligned) without the parent transform.
    {
        ClipInfo hiddenParent = mediaClip(media, 0.5, 0.2, 0.0);
        hiddenParent.visible = false;
        Timeline hidden;
        setupRender(hidden, {hiddenParent, mediaClip(media, 0.3, 0.1, -0.25),
                             mediaClip(media, 0.3, -0.15, 0.25)}, 0.0);
        hidden.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
        hidden.videoTracks()[1]->toggleClipSelection(0);   // parent not selected
        finishSetup(hidden);
        const QImage beforeFrame = renderAt(hidden, 0.0);
        const QPair<int, int> childBefore = opaqueColumns(beforeFrame, 0, kRenderH / 2);
        const bool arranged = haveMedia && hidden.selectedArrangementTargets().size() == 2
            && hidden.arrangeSelectedLayers(Arrangement::Left);
        const QImage frame = renderAt(hidden, 0.0);
        const QPair<int, int> child = opaqueColumns(frame, 0, kRenderH / 2);
        const QPair<int, int> plain = opaqueColumns(frame, kRenderH / 2, kRenderH);
        const int widthBefore = childBefore.second - childBefore.first;
        const int widthAfter = child.second - child.first;
        std::fprintf(stderr, "[align-distribute] G5c arranged=%d childLeftBefore=%d childLeft=%d "
                     "plainLeft=%d childWidthBefore=%d childWidth=%d\n", arranged,
                     childBefore.first, child.first, plain.first, widthBefore, widthAfter);
        ok = arranged && child.first >= 0 && plain.first >= 0 && plain.first <= 1
            && std::abs(child.first - plain.first) <= 1
            && std::abs(widthAfter - widthBefore) <= 1
            && childBefore.first > plain.first + 10 && ok;
    }
    // G5d: a depth-2 chain (Null 0.15 <- Null 0.1 <- child) lines up with a plain clip.
    {
        Timeline chain;
        setupRender(chain, {makeNullObject(0.15), makeNullObject(0.1),
                            mediaClip(media, 0.3, 0.1, -0.25), mediaClip(media, 0.3, -0.15, 0.25)},
                    0.0);
        chain.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
        chain.setClipParent(trackMatteClipKey(3, 0), trackMatteClipKey(2, 0));
        finishSetup(chain);
        const QPair<int, int> childBefore = opaqueColumns(renderAt(chain, 0.0), 0, kRenderH / 2);
        const bool arranged = haveMedia && chain.selectedArrangementTargets().size() == 2
            && chain.arrangeSelectedLayers(Arrangement::Left);
        const QImage frame = renderAt(chain, 0.0);
        const QPair<int, int> child = opaqueColumns(frame, 0, kRenderH / 2);
        const QPair<int, int> plain = opaqueColumns(frame, kRenderH / 2, kRenderH);
        const int widthBefore = childBefore.second - childBefore.first;
        const int widthAfter = child.second - child.first;
        std::fprintf(stderr, "[align-distribute] G5d arranged=%d childLeftBefore=%d childLeft=%d "
                     "plainLeft=%d childWidthBefore=%d childWidth=%d\n", arranged,
                     childBefore.first, child.first, plain.first, widthBefore, widthAfter);
        ok = arranged && child.first >= 0 && plain.first >= 0 && plain.first <= 1
            && std::abs(child.first - plain.first) <= 1
            && std::abs(widthAfter - widthBefore) <= 1
            && childBefore.first > plain.first + 10 && ok;
    }
    // G5e: a scaled (0.5) and rotated (90 deg) Null parent. The child renders
    // 60x120 in rows 70..190; the plain clip sits in rows 0..50.
    {
        ClipInfo turned = makeNullObject(0.1);
        turned.videoScale = 0.5;
        turned.rotation2DDegrees = 90.0;
        Timeline rotated;
        setupRender(rotated, {turned, mediaClip(media, 0.6, 0.15, 0.0),
                              mediaClip(media, 0.25, -0.1, -0.375)}, 0.0);
        rotated.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
        finishSetup(rotated);
        const QImage beforeFrame = renderAt(rotated, 0.0);
        const QPair<int, int> childBefore = opaqueColumns(beforeFrame, 80, 180);
        const bool arranged = haveMedia && rotated.selectedArrangementTargets().size() == 2
            && rotated.arrangeSelectedLayers(Arrangement::Left);
        const QImage frame = renderAt(rotated, 0.0);
        const QPair<int, int> child = opaqueColumns(frame, 80, 180);
        const QPair<int, int> plain = opaqueColumns(frame, 10, 40);
        const int widthBefore = childBefore.second - childBefore.first;
        const int widthAfter = child.second - child.first;
        std::fprintf(stderr, "[align-distribute] G5e arranged=%d childLeftBefore=%d childLeft=%d "
                     "plainLeft=%d childWidthBefore=%d childWidth=%d\n", arranged,
                     childBefore.first, child.first, plain.first, widthBefore, widthAfter);
        ok = arranged && child.first >= 0 && plain.first >= 0 && plain.first <= 1
            && std::abs(child.first - plain.first) <= 1
            && std::abs(widthAfter - widthBefore) <= 1 && std::abs(widthBefore - 60) <= 2
            && childBefore.first > plain.first + 10 && ok;
    }
    // G5f: horizontal distribution with the middle clip parented to a scaled Null
    // (0.5, dx 0.1). Bands: a rows 0..40, b rows 80..120, c rows 160..200.
    {
        ClipInfo scaledNull = makeNullObject(0.1);
        scaledNull.videoScale = 0.5;
        Timeline spread;
        setupRender(spread, {scaledNull, mediaClip(media, 0.2, -0.35, -0.4),
                             mediaClip(media, 0.4, 0.2, 0.0), mediaClip(media, 0.2, 0.35, 0.4)},
                    0.0);
        spread.setClipParent(trackMatteClipKey(3, 0), trackMatteClipKey(1, 0));
        finishSetup(spread);
        const QPair<int, int> middleBefore = opaqueColumns(renderAt(spread, 0.0), 85, 115);
        const bool arranged = haveMedia && spread.selectedArrangementTargets().size() == 3
            && spread.arrangeSelectedLayers(Arrangement::DistributeHorizontal);
        const QImage frame = renderAt(spread, 0.0);
        const QPair<int, int> a = opaqueColumns(frame, 5, 35);
        const QPair<int, int> b = opaqueColumns(frame, 85, 115);
        const QPair<int, int> c = opaqueColumns(frame, 165, 195);
        const int gapAb = b.first - (a.second + 1);
        const int gapBc = c.first - (b.second + 1);
        std::fprintf(stderr, "[align-distribute] G5f arranged=%d a=[%d,%d] bBefore=[%d,%d] b=[%d,%d] "
                     "c=[%d,%d] gapAB=%d gapBC=%d\n", arranged, a.first, a.second,
                     middleBefore.first, middleBefore.second, b.first, b.second, c.first,
                     c.second, gapAb, gapBc);
        ok = arranged && a.first >= 0 && b.first >= 0 && c.first >= 0
            && std::abs(gapAb - gapBc) <= 1
            && std::abs((b.second - b.first) - (middleBefore.second - middleBefore.first)) <= 1
            && std::abs(middleBefore.first - b.first) > 10 && ok;
    }
    // G5g: a child of a zero-scale Null cannot be placed (no inverse). It stays
    // unchanged, the plain clip is still aligned in one undo entry, a status
    // message reports the count, and the call returns true (something was
    // written). With only the unresolvable child selected it returns false and
    // records no undo entry.
    {
        ClipInfo collapsed = makeNullObject(0.1);
        collapsed.videoScale = 0.0;
        Timeline stuck;
        setupRender(stuck, {collapsed, mediaClip(media, 0.3, 0.1, -0.25),
                            mediaClip(media, 0.3, -0.15, 0.25)}, 0.0);
        stuck.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
        finishSetup(stuck);
        QStringList messages;
        QObject::connect(&stuck, &Timeline::statusMessageRequested,
                         [&](const QString &message, int) { messages.append(message); });
        const auto serial = stuck.undoManager()->saveSerial();
        const bool result = stuck.arrangeSelectedLayers(Arrangement::Left);
        const ClipInfo &child = stuck.videoTracks()[2]->clips()[0];
        const ClipInfo &plain = stuck.videoTracks()[3]->clips()[0];
        const bool oneUndo = stuck.undoManager()->saveSerial() == serial + 1;
        std::fprintf(stderr, "[align-distribute] G5g result=%d childDx=%.6f plainDx=%.6f "
                     "oneUndo=%d messages=%d first=%s\n", result, child.videoDx, plain.videoDx,
                     oneUndo, int(messages.size()),
                     messages.isEmpty() ? "-" : messages.first().toUtf8().constData());
        ok = result && near(child.videoDx, 0.1) && near(plain.videoDx, -0.35) && oneUndo
            && messages.size() == 1 && messages.first().startsWith(QStringLiteral("1 ")) && ok;

        stuck.undo();
        stuck.clearSelection();
        stuck.videoTracks()[2]->toggleClipSelection(0);
        messages.clear();
        const auto aloneSerial = stuck.undoManager()->saveSerial();
        const bool aloneResult = stuck.arrangeSelectedLayers(Arrangement::Left);
        const bool noUndo = stuck.undoManager()->saveSerial() == aloneSerial;
        const double aloneDx = stuck.videoTracks()[2]->clips()[0].videoDx;
        std::fprintf(stderr, "[align-distribute] G5g alone result=%d childDx=%.6f noUndo=%d "
                     "messages=%d\n", aloneResult, aloneDx, noUndo, int(messages.size()));
        ok = !aloneResult && near(aloneDx, 0.1) && noUndo && messages.size() == 1 && ok;
    }
    // G5h: a child of a zero-scale Null already 0.4 px from the left edge is
    // within tolerance without any move: nothing is written, no undo entry, no
    // status message, and the call returns true.
    {
        ClipInfo collapsed = makeNullObject(-0.499);
        collapsed.videoScale = 0.0;
        Timeline still;
        setupRender(still, {collapsed, mediaClip(media, 0.3, 0.1, -0.25)}, 0.0);
        still.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
        still.clearSelection();
        still.videoTracks()[2]->toggleClipSelection(0);
        finishSetup(still);
        QStringList messages;
        QObject::connect(&still, &Timeline::statusMessageRequested,
                         [&](const QString &message, int) { messages.append(message); });
        const auto serial = still.undoManager()->saveSerial();
        const bool result = still.arrangeSelectedLayers(Arrangement::Left);
        const double childDx = still.videoTracks()[2]->clips()[0].videoDx;
        const bool noUndo = still.undoManager()->saveSerial() == serial;
        const ClipInfo &child = still.videoTracks()[2]->clips()[0];
        const QRectF childBounds = clipgeom::resolveTransform(
            clipgeom::composeParented({child.videoScale, child.videoDx, child.videoDy,
                                       child.rotation2DDegrees},
                                      {collapsed.videoScale, collapsed.videoDx,
                                       collapsed.videoDy, collapsed.rotation2DDegrees},
                                      QSize(kRenderW, kRenderH)),
            QSize(kRenderW, kRenderH)).mapRect(QRectF(0, 0, kRenderW, kRenderH));
        std::fprintf(stderr, "[align-distribute] G5h result=%d childDx=%.6f boundsLeft=%.6f "
                     "boundsWidth=%.6f noUndo=%d messages=%d first=%s\n", result, childDx,
                     childBounds.left(), childBounds.width(), noUndo, int(messages.size()),
                     messages.isEmpty() ? "-" : messages.first().toUtf8().constData());
        ok = result && near(childDx, 0.1) && noUndo && messages.isEmpty() && ok;
    }
    // G5i (r5 review): distribution where an endpoint is the child of the middle
    // clip. Child (scale 0.2, dx -1.0, rows 0..20) of a regular parent (scale 0.5,
    // dx 0.1, rows 50..150), plus a plain clip (scale 0.1, dx 0.4, rows 180..200):
    // [20,60] [140,340] [340,380]. The parent moves to [100,300]; the child is
    // compensated back to its world position, so both gaps are 40 px.
    {
        Timeline spread;
        setupRender(spread, {mediaClip(media, 0.5, 0.1, 0.0), mediaClip(media, 0.2, -1.0, -0.9),
                             mediaClip(media, 0.1, 0.4, 0.45)}, 0.0);
        spread.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
        finishSetup(spread);
        const QImage beforeFrame = renderAt(spread, 0.0);
        const QPair<int, int> childBefore = opaqueColumns(beforeFrame, 2, 18);
        const QPair<int, int> parentBefore = opaqueColumns(beforeFrame, 60, 140);
        const auto serial = spread.undoManager()->saveSerial();
        const bool arranged = haveMedia && spread.selectedArrangementTargets().size() == 3
            && spread.arrangeSelectedLayers(Arrangement::DistributeHorizontal);
        const QImage frame = renderAt(spread, 0.0);
        const QPair<int, int> child = opaqueColumns(frame, 2, 18);
        const QPair<int, int> parent = opaqueColumns(frame, 60, 140);
        const QPair<int, int> plain = opaqueColumns(frame, 182, 198);
        const int gapAb = parent.first - (child.second + 1);
        const int gapBc = plain.first - (parent.second + 1);
        const bool oneUndo = spread.undoManager()->saveSerial() == serial + 1;
        std::fprintf(stderr, "[align-distribute] G5i arranged=%d childBefore=[%d,%d] child=[%d,%d] "
                     "parentBefore=[%d,%d] parent=[%d,%d] plain=[%d,%d] gapAB=%d gapBC=%d "
                     "oneUndo=%d\n", arranged, childBefore.first, childBefore.second, child.first,
                     child.second, parentBefore.first, parentBefore.second, parent.first,
                     parent.second, plain.first, plain.second, gapAb, gapBc, oneUndo);
        ok = arranged && oneUndo && child.first >= 0 && parent.first >= 0 && plain.first >= 0
            && std::abs(child.first - childBefore.first) <= 1
            && std::abs(gapAb - gapBc) <= 1 && std::abs(gapAb - 40) <= 1
            && std::abs(parent.first - parentBefore.first) > 10 && ok;
    }
    gate(5, ok);

    ok = true;
    // G6: position keyframes 0s -0.3 (ease-in) / 2s +0.3; centering at 1s must
    // move the rendered layer to the canvas center and keep the outer keys.
    {
        ClipInfo animated = mediaClip(media, 0.3, 0.25, 0.0);
        KeyframeTrack track(QStringLiteral("motion.position.x"), 0.0);
        track.addKeyframe(0.0, -0.3, KeyframePoint::EaseIn);
        track.addKeyframe(2.0, 0.3);
        animated.keyframes.addTrack(track);
        Timeline keyed;
        setupRender(keyed, {animated}, 1.0);
        finishSetup(keyed);
        const QPair<int, int> before = opaqueColumns(renderAt(keyed, 1.0), 0, kRenderH);
        const auto serial = keyed.undoManager()->saveSerial();
        const bool arranged = haveMedia && keyed.arrangeSelectedLayers(Arrangement::HorizontalCenter);
        const QPair<int, int> after = opaqueColumns(renderAt(keyed, 1.0), 0, kRenderH);
        const double centerBefore = (before.first + before.second + 1) * 0.5;
        const double centerAfter = (after.first + after.second + 1) * 0.5;
        const ClipInfo &clip = keyed.videoTracks()[1]->clips()[0];
        const KeyframeTrack *keys = positionX(clip);
        const int count = keys ? keys->count() : 0;
        const double k0 = keys ? keyAt(keys, 0.0) : std::nan("");
        const double k1 = keys ? keyAt(keys, 1.0) : std::nan("");
        const double k2 = keys ? keyAt(keys, 2.0) : std::nan("");
        const bool keysKept = near(k0, -0.3) && near(k2, 0.3)
            && (count == 2 || (count == 3 && std::isfinite(k1)));
        const bool oneUndo = keyed.undoManager()->saveSerial() == serial + 1;
        keyed.undo();
        const KeyframeTrack *restored = positionX(keyed.videoTracks()[1]->clips()[0]);
        const bool undone = restored && restored->count() == 2;
        std::fprintf(stderr, "[align-distribute] G6 arranged=%d centerBefore=%.1f centerAfter=%.1f "
                     "canvasCenter=%.1f keys=%d k0=%.6f k1=%.6f k2=%.6f oneUndo=%d undone=%d\n",
                     arranged, centerBefore, centerAfter, kRenderW * 0.5, count, k0, k1, k2,
                     oneUndo, undone);
        ok = arranged && after.first >= 0 && std::abs(centerAfter - kRenderW * 0.5) <= 1.0
            && std::abs(centerBefore - kRenderW * 0.5) > 5.0 && keysKept && oneUndo && undone && ok;
    }
    // G6b: a Hold key at 0s (keys 0s -0.3 Hold / 2s +0.3). Centering at 1s adds a
    // key at 1s; the 1s frame must show it while [0s, 1s) still holds -0.3, and
    // after it the clip moves linearly from the new key to the 2s key (1.5s: 0.15).
    {
        ClipInfo held = mediaClip(media, 0.3, 0.0, 0.0);
        KeyframeTrack track(QStringLiteral("motion.position.x"), 0.0);
        track.addKeyframe(0.0, -0.3, KeyframePoint::Hold);
        track.addKeyframe(2.0, 0.3);
        held.keyframes.addTrack(track);
        Timeline keyed;
        setupRender(keyed, {held}, 1.0);
        finishSetup(keyed);
        const QPair<int, int> before = opaqueColumns(renderAt(keyed, 1.0), 0, kRenderH);
        const QPair<int, int> holdBefore = opaqueColumns(renderAt(keyed, 0.5), 0, kRenderH);
        const bool arranged = haveMedia && keyed.arrangeSelectedLayers(Arrangement::HorizontalCenter);
        const QPair<int, int> after = opaqueColumns(renderAt(keyed, 1.0), 0, kRenderH);
        const QPair<int, int> holdAfter = opaqueColumns(renderAt(keyed, 0.5), 0, kRenderH);
        const QPair<int, int> later = opaqueColumns(renderAt(keyed, 1.5), 0, kRenderH);
        const double centerBefore = (before.first + before.second + 1) * 0.5;
        const double centerAfter = (after.first + after.second + 1) * 0.5;
        const double centerLater = (later.first + later.second + 1) * 0.5;
        const double expectedLater = kRenderW * (0.5 + 0.15);
        const KeyframeTrack *keys = positionX(keyed.videoTracks()[1]->clips()[0]);
        const int count = keys ? keys->count() : 0;
        const double k1 = keys ? keyAt(keys, 1.0) : std::nan("");
        const double xLater = clipanim::effectivePositionAt(keyed.videoTracks()[1]->clips()[0], 1.5).x();
        std::fprintf(stderr, "[align-distribute] G6b arranged=%d centerBefore=%.1f centerAfter=%.1f "
                     "canvasCenter=%.1f holdLeftBefore=%d holdLeftAfter=%d keys=%d k1=%.6f "
                     "center1.5s=%.1f expected1.5s=%.1f x1.5s=%.6f\n",
                     arranged, centerBefore, centerAfter, kRenderW * 0.5, holdBefore.first,
                     holdAfter.first, count, k1, centerLater, expectedLater, xLater);
        ok = arranged && after.first >= 0 && std::abs(centerAfter - kRenderW * 0.5) <= 1.0
            && std::abs(centerBefore - kRenderW * 0.5) > 5.0 && holdBefore.first >= 0
            && holdAfter.first == holdBefore.first && count == 3 && near(k1, 0.0)
            && std::abs(centerLater - expectedLater) <= 1.0 && std::abs(xLater - 0.15) <= 1e-9
            && ok;
    }
    // G6c: Auto Orient. The new x key at 1s changes the path tangent, hence the
    // rotation and the rendered width; the left edge must still land on 0.
    // Case 1: x 0s -0.3 / 2s +0.3, y 0s -0.2 / 2s +0.2.
    // Case 2 (r3 review): x -0.35 / -0.25, y -0.05 / +0.05, starting ~16 px from
    // the edge, where fixed-step corrections oscillate around the target.
    // The geometric bound is checked too, so a layer clipped by the canvas edge
    // cannot pass on its rendered column alone.
    const double orientCases[2][4] = {{-0.3, 0.3, -0.2, 0.2}, {-0.35, -0.25, -0.05, 0.05}};
    for (int n = 0; n < 2; ++n) {
        const double *k = orientCases[n];
        ClipInfo oriented = mediaClip(media, 0.3, 0.0, 0.0);
        oriented.autoOrientEnabled = true;
        KeyframeTrack x(QStringLiteral("motion.position.x"), 0.0);
        x.addKeyframe(0.0, k[0]);
        x.addKeyframe(2.0, k[1]);
        KeyframeTrack y(QStringLiteral("motion.position.y"), 0.0);
        y.addKeyframe(0.0, k[2]);
        y.addKeyframe(2.0, k[3]);
        oriented.keyframes.addTrack(x);
        oriented.keyframes.addTrack(y);
        Timeline orient;
        setupRender(orient, {oriented}, 1.0);
        finishSetup(orient);
        const auto geometry = [&]() {
            return clipgeom::resolveTransform(
                clipanim::effectiveTransformAt(orient.videoTracks()[1]->clips()[0], 1.0),
                QSize(kRenderW, kRenderH)).mapRect(QRectF(0, 0, kRenderW, kRenderH));
        };
        const double rotBefore = clipanim::effectiveTransformAt(
            orient.videoTracks()[1]->clips()[0], 1.0).rotationDeg;
        const QRectF boundsBefore = geometry();
        const QPair<int, int> before = opaqueColumns(renderAt(orient, 1.0), 0, kRenderH);
        const auto serial = orient.undoManager()->saveSerial();
        const bool arranged = haveMedia && orient.arrangeSelectedLayers(Arrangement::Left);
        const QPair<int, int> after = opaqueColumns(renderAt(orient, 1.0), 0, kRenderH);
        const QRectF boundsAfter = geometry();
        const double rotAfter = clipanim::effectiveTransformAt(
            orient.videoTracks()[1]->clips()[0], 1.0).rotationDeg;
        const bool oneUndo = orient.undoManager()->saveSerial() == serial + 1;
        std::fprintf(stderr, "[align-distribute] G6c case=%d arranged=%d leftBefore=%d left=%d "
                     "widthBefore=%d width=%d boundsLeftBefore=%.3f boundsLeft=%.6f "
                     "boundsWidthBefore=%.3f boundsWidth=%.3f rotBefore=%.3f rotAfter=%.3f "
                     "oneUndo=%d\n", n + 1, arranged, before.first, after.first,
                     before.second - before.first, after.second - after.first,
                     boundsBefore.left(), boundsAfter.left(), boundsBefore.width(),
                     boundsAfter.width(), rotBefore, rotAfter, oneUndo);
        ok = arranged && oneUndo && after.first >= 0 && after.first <= 1
            && std::abs(boundsAfter.left()) <= 0.5 && boundsBefore.left() > 10.0
            && std::abs(rotAfter - rotBefore) > 1.0
            && std::abs((after.second - after.first) - boundsAfter.width()) <= 2.0 && ok;
    }
    // G6d: a key 0.5 ms after the playhead, behind a Hold segment (keys 0s -0.3
    // Hold / 1.0005s -0.1 / 2s +0.3). Centering at 1s must move that key to
    // exactly 1s; updating it in place would leave the 1s frame on the Hold value.
    {
        ClipInfo nearKey = mediaClip(media, 0.3, 0.0, 0.0);
        KeyframeTrack track(QStringLiteral("motion.position.x"), 0.0);
        track.addKeyframe(0.0, -0.3, KeyframePoint::Hold);
        track.addKeyframe(1.0005, -0.1);
        track.addKeyframe(2.0, 0.3);
        nearKey.keyframes.addTrack(track);
        Timeline keyed;
        setupRender(keyed, {nearKey}, 1.0);
        finishSetup(keyed);
        const QPair<int, int> before = opaqueColumns(renderAt(keyed, 1.0), 0, kRenderH);
        const bool arranged = haveMedia && keyed.arrangeSelectedLayers(Arrangement::HorizontalCenter);
        const QPair<int, int> after = opaqueColumns(renderAt(keyed, 1.0), 0, kRenderH);
        const double centerBefore = (before.first + before.second + 1) * 0.5;
        const double centerAfter = (after.first + after.second + 1) * 0.5;
        const KeyframeTrack *keys = positionX(keyed.videoTracks()[1]->clips()[0]);
        const int count = keys ? keys->count() : 0;
        const double t1 = count == 3 ? keys->keyframes()[1].time : std::nan("");
        const double v1 = count == 3 ? keys->keyframes()[1].value : std::nan("");
        std::fprintf(stderr, "[align-distribute] G6d arranged=%d centerBefore=%.1f centerAfter=%.1f "
                     "keys=%d key1Time=%.9f key1Value=%.6f\n", arranged, centerBefore,
                     centerAfter, count, t1, v1);
        ok = arranged && after.first >= 0 && std::abs(centerAfter - kRenderW * 0.5) <= 1.0
            && std::abs(centerBefore - kRenderW * 0.5) > 5.0 && count == 3 && t1 == 1.0
            && near(v1, 0.0) && ok;
    }
    // G6e: Hold with a spatial tangent (x/y keys at 0s Hold with tangents, 2s).
    // The spatial path evaluator must also return the new 1s key at 1s, while
    // [0s, 1s) still holds the 0s point.
    {
        ClipInfo spatial = mediaClip(media, 0.3, 0.0, 0.0);
        KeyframeTrack x(QStringLiteral("motion.position.x"), 0.0);
        x.addKeyframe(0.0, -0.3, KeyframePoint::Hold, 0.0, 0.0, 1.0, 1.0, true, 0.1, 0.05, 0.0, 0.0);
        x.addKeyframe(2.0, 0.3, KeyframePoint::Linear, 0.0, 0.0, 1.0, 1.0, true, 0.0, 0.0, -0.1, 0.05);
        KeyframeTrack y(QStringLiteral("motion.position.y"), 0.0);
        y.addKeyframe(0.0, 0.0, KeyframePoint::Hold, 0.0, 0.0, 1.0, 1.0, true, 0.1, 0.05, 0.0, 0.0);
        y.addKeyframe(2.0, 0.0, KeyframePoint::Linear, 0.0, 0.0, 1.0, 1.0, true, 0.0, 0.0, -0.1, 0.05);
        spatial.keyframes.addTrack(x);
        spatial.keyframes.addTrack(y);
        Timeline keyed;
        setupRender(keyed, {spatial}, 1.0);
        finishSetup(keyed);
        const QPair<int, int> before = opaqueColumns(renderAt(keyed, 1.0), 0, kRenderH);
        const QPair<int, int> holdBefore = opaqueColumns(renderAt(keyed, 0.5), 0, kRenderH);
        const bool arranged = haveMedia && keyed.arrangeSelectedLayers(Arrangement::HorizontalCenter);
        const QPair<int, int> after = opaqueColumns(renderAt(keyed, 1.0), 0, kRenderH);
        const QPair<int, int> holdAfter = opaqueColumns(renderAt(keyed, 0.5), 0, kRenderH);
        const double centerBefore = (before.first + before.second + 1) * 0.5;
        const double centerAfter = (after.first + after.second + 1) * 0.5;
        const ClipInfo &clip = keyed.videoTracks()[1]->clips()[0];
        const QPointF position = clipanim::effectivePositionAt(clip, 1.0);
        std::fprintf(stderr, "[align-distribute] G6e arranged=%d centerBefore=%.1f centerAfter=%.1f "
                     "holdLeftBefore=%d holdLeftAfter=%d position1s=(%.6f,%.6f) keys=%d\n",
                     arranged, centerBefore, centerAfter, holdBefore.first, holdAfter.first,
                     position.x(), position.y(), positionX(clip) ? positionX(clip)->count() : 0);
        ok = arranged && after.first >= 0 && std::abs(centerAfter - kRenderW * 0.5) <= 1.0
            && std::abs(centerBefore - kRenderW * 0.5) > 5.0 && holdBefore.first >= 0
            && holdAfter.first == holdBefore.first && std::abs(position.x()) <= 1e-6 && ok;
    }
    // G6f: Auto Orient with keys 0s (-0.1,-0.2) / 2s (0.5,0.2), center and right
    // alignment at 1s. The new key turns the layer and changes its width, so the
    // rendered center / right edge must be measured on the result (r4 review).
    // The right case uses x 0s -0.5 / 2s +0.1 so that moving right flattens the
    // path and the width changes there too.
    for (const auto arrangement : {Arrangement::HorizontalCenter, Arrangement::Right}) {
        const bool centerCase = arrangement == Arrangement::HorizontalCenter;
        ClipInfo oriented = mediaClip(media, 0.3, 0.0, 0.0);
        oriented.autoOrientEnabled = true;
        KeyframeTrack x(QStringLiteral("motion.position.x"), 0.0);
        x.addKeyframe(0.0, centerCase ? -0.1 : -0.5);
        x.addKeyframe(2.0, centerCase ? 0.5 : 0.1);
        KeyframeTrack y(QStringLiteral("motion.position.y"), 0.0);
        y.addKeyframe(0.0, -0.2);
        y.addKeyframe(2.0, 0.2);
        oriented.keyframes.addTrack(x);
        oriented.keyframes.addTrack(y);
        Timeline orient;
        setupRender(orient, {oriented}, 1.0);
        finishSetup(orient);
        const auto geometry = [&]() {
            return clipgeom::resolveTransform(
                clipanim::effectiveTransformAt(orient.videoTracks()[1]->clips()[0], 1.0),
                QSize(kRenderW, kRenderH)).mapRect(QRectF(0, 0, kRenderW, kRenderH));
        };
        const QRectF boundsBefore = geometry();
        const QPair<int, int> before = opaqueColumns(renderAt(orient, 1.0), 0, kRenderH);
        const bool center = arrangement == Arrangement::HorizontalCenter;
        const bool arranged = haveMedia && orient.arrangeSelectedLayers(arrangement);
        const QPair<int, int> after = opaqueColumns(renderAt(orient, 1.0), 0, kRenderH);
        const QRectF boundsAfter = geometry();
        const double renderedEdge = center ? (after.first + after.second + 1) * 0.5
                                           : after.second + 1.0;
        const double geometricEdge = center ? boundsAfter.center().x() : boundsAfter.right();
        const double goal = center ? kRenderW * 0.5 : kRenderW;
        std::fprintf(stderr, "[align-distribute] G6f %s arranged=%d widthBefore=%.3f width=%.3f "
                     "renderedEdge=%.1f geometricEdge=%.6f goal=%.1f left=%d right=%d\n",
                     center ? "center" : "right", arranged, boundsBefore.width(),
                     boundsAfter.width(), renderedEdge, geometricEdge, goal, after.first,
                     after.second);
        ok = arranged && after.first >= 0 && std::abs(renderedEdge - goal) <= 1.0
            && std::abs(geometricEdge - goal) <= 0.5
            && std::abs(boundsAfter.width() - boundsBefore.width()) > 2.0
            && std::abs((after.second - after.first + 1) - boundsAfter.width()) <= 2.0
            && std::abs(edgeBefore(before, center) - goal) > 5.0 && ok;
    }
    // G6g: horizontal distribution whose middle clip has Auto Orient (x 0s -0.3 /
    // 2s +0.3, y 0s -0.3 / 2s +0.3). Moving it changes its width, so the layout is
    // redone with the new width. Bands: a rows 0..40, b rows 45..155, c rows 160..200.
    {
        ClipInfo middle = mediaClip(media, 0.2, 0.0, 0.0);
        middle.autoOrientEnabled = true;
        KeyframeTrack x(QStringLiteral("motion.position.x"), 0.0);
        x.addKeyframe(0.0, -0.3);
        x.addKeyframe(2.0, 0.3);
        KeyframeTrack y(QStringLiteral("motion.position.y"), 0.0);
        y.addKeyframe(0.0, -0.3);
        y.addKeyframe(2.0, 0.3);
        middle.keyframes.addTrack(x);
        middle.keyframes.addTrack(y);
        Timeline spread;
        setupRender(spread, {mediaClip(media, 0.2, -0.4, -0.4), middle,
                             mediaClip(media, 0.2, 0.0, 0.4)}, 1.0);
        finishSetup(spread);
        const QPair<int, int> middleBefore = opaqueColumns(renderAt(spread, 1.0), 45, 155);
        const auto serial = spread.undoManager()->saveSerial();
        const bool arranged = haveMedia
            && spread.arrangeSelectedLayers(Arrangement::DistributeHorizontal);
        const QImage frame = renderAt(spread, 1.0);
        const QPair<int, int> a = opaqueColumns(frame, 5, 35);
        const QPair<int, int> b = opaqueColumns(frame, 45, 155);
        const QPair<int, int> c = opaqueColumns(frame, 165, 195);
        const int gapAb = b.first - (a.second + 1);
        const int gapBc = c.first - (b.second + 1);
        const int widthBefore = middleBefore.second - middleBefore.first;
        const int widthAfter = b.second - b.first;
        const bool oneUndo = spread.undoManager()->saveSerial() == serial + 1;
        std::fprintf(stderr, "[align-distribute] G6g arranged=%d a=[%d,%d] bBefore=[%d,%d] b=[%d,%d] "
                     "c=[%d,%d] gapAB=%d gapBC=%d widthBefore=%d width=%d oneUndo=%d\n", arranged,
                     a.first, a.second, middleBefore.first, middleBefore.second, b.first,
                     b.second, c.first, c.second, gapAb, gapBc, widthBefore, widthAfter, oneUndo);
        ok = arranged && oneUndo && a.first >= 0 && b.first >= 0 && c.first >= 0
            && std::abs(gapAb - gapBc) <= 1 && std::abs(widthAfter - widthBefore) > 3
            && std::abs(middleBefore.first - b.first) > 10 && ok;
    }
    gate(6, ok);

    // G7: unparented, keyframe-less clips keep the G1/G2 expected values.
    {
        ok = true;
        for (const auto arrangement : {Arrangement::Left, Arrangement::Bottom}) {
            setup(timeline, {makeClip(0.4, 0.17, -0.13), makeClip(0.4, 0.1, 0.2, 90.0)});
            const auto serial = timeline.undoManager()->saveSerial();
            ok = timeline.arrangeSelectedLayers(arrangement) && ok;
            const auto &a = timeline.videoTracks()[0]->clips()[0];
            const auto &b = timeline.videoTracks()[1]->clips()[0];
            const bool left = arrangement == Arrangement::Left;
            const bool values = left
                ? near(a.videoDx, -0.3) && near(a.videoDy, -0.13)
                    && near(b.videoDx, -0.4) && near(b.videoDy, 0.2)
                : near(a.videoDx, 0.17) && near(a.videoDy, 0.3)
                    && near(b.videoDx, 0.1) && near(b.videoDy, 0.1);
            const bool noKeys = !a.keyframes.hasAnyKeyframes() && !b.keyframes.hasAnyKeyframes();
            const bool oneUndo = timeline.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[align-distribute] G7 %s a=(%.9f,%.9f) b=(%.9f,%.9f) noKeys=%d "
                         "oneUndo=%d\n", left ? "left" : "bottom", a.videoDx, a.videoDy,
                         b.videoDx, b.videoDy, noKeys, oneUndo);
            ok = values && noKeys && oneUndo && ok;
        }
        gate(7, ok);
    }
    return tally.finish("[align-distribute]");
}
