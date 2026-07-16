#include "../ProjectFile.h"
#include "../MaskSystem.h"
#include "../PlanarTracker.h"
#include "../Timeline.h"
#include "../mask/ClipMask.h"

#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QSize>

#include <cmath>
#include <cstdio>

namespace clipmask {

ClipMaskStack applyPlanarCornerPinToMaskPath(const ClipMaskStack &stack,
                                             const planar::CornerSet &referenceCorners,
                                             const planar::CornerSet &trackedCorners);

QMap<int, ClipMaskStack> bakePlanarTrackToMaskPathKeyframes(
    const ClipMaskStack &stack,
    const planar::CornerSet &referenceCorners,
    const QList<planar::Frame> &frames);

QMap<int, ClipMaskStack> bakePlanarTrackToMaskPathKeyframes(
    const ClipMaskStack &stack,
    const planartrack::PlanarTrack &track);

} // namespace clipmask

namespace {

clipmask::BezierPoint bezierPoint(double x, double y,
                                  double inX = 0.0, double inY = 0.0,
                                  double outX = 0.0, double outY = 0.0)
{
    clipmask::BezierPoint point;
    point.vertex = QPointF(x, y);
    point.inTangent = QPointF(inX, inY);
    point.outTangent = QPointF(outX, outY);
    return point;
}

ClipInfo makeClip()
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("mask-1-source.mov");
    clip.displayName = QStringLiteral("mask-1-source");
    clip.duration = 4.0;
    clip.inPoint = 0.0;
    clip.outPoint = 4.0;
    return clip;
}

ProjectData projectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    return data;
}

clipmask::ClipMaskStack makeModelStack()
{
    clipmask::BezierMask add;
    add.name = QStringLiteral("hero-add");
    add.mode = clipmask::CombineMode::Add;
    add.closed = true;
    add.feather = 6.5;
    add.expansion = 2.0;
    add.inverted = false;
    add.points = {
        bezierPoint(0.10, 0.20, 0.0, 0.0, 0.08, -0.03),
        bezierPoint(0.80, 0.18, -0.06, 0.02, 0.02, 0.08),
        bezierPoint(0.74, 0.76, 0.06, -0.01, -0.08, 0.04),
        bezierPoint(0.18, 0.82, 0.07, 0.03, 0.0, -0.09)
    };

    clipmask::BezierMask subtract;
    subtract.name = QStringLiteral("eye-subtract");
    subtract.mode = clipmask::CombineMode::Subtract;
    subtract.closed = true;
    subtract.feather = 1.25;
    subtract.expansion = -1.5;
    subtract.inverted = true;
    subtract.opacity = 0.75;
    subtract.points = {
        bezierPoint(0.30, 0.32),
        bezierPoint(0.50, 0.30),
        bezierPoint(0.48, 0.46),
        bezierPoint(0.32, 0.48)
    };

    clipmask::BezierMask intersect;
    intersect.name = QStringLiteral("limit-intersect");
    intersect.mode = clipmask::CombineMode::Intersect;
    intersect.closed = true;
    intersect.feather = 0.0;
    intersect.expansion = 0.0;
    intersect.points = {
        bezierPoint(0.05, 0.05),
        bezierPoint(0.95, 0.05),
        bezierPoint(0.95, 0.95),
        bezierPoint(0.05, 0.95)
    };

    clipmask::ClipMaskStack stack;
    stack.masks = {add, subtract, intersect};
    return stack;
}

Mask makePathMask(const QString &name, MaskMode mode, bool inverted)
{
    Mask mask;
    mask.shape = MaskShape::Path;
    mask.mode = mode;
    mask.name = name;
    mask.inverted = inverted;
    mask.feather.amount = inverted ? 2.0 : 4.0;
    mask.expansion = inverted ? -3.0 : 5.0;
    mask.opacity = inverted ? 0.8 : 1.0;
    mask.points = {
        QPointF(10.0, 12.0),
        QPointF(80.0, 15.0),
        QPointF(72.0, 64.0),
        QPointF(14.0, 70.0)
    };
    return mask;
}

bool sameMaskFields(const Mask &a, const Mask &b)
{
    return a.shape == b.shape
        && a.mode == b.mode
        && a.name == b.name
        && a.inverted == b.inverted
        && a.feather.amount == b.feather.amount
        && a.expansion == b.expansion
        && a.opacity == b.opacity
        && a.points == b.points;
}

clipmask::ClipMaskStack makePlanarBakeStack()
{
    clipmask::BezierMask mask;
    mask.name = QStringLiteral("planar-follow");
    mask.mode = clipmask::CombineMode::Add;
    mask.closed = true;
    mask.points = {
        bezierPoint(28.0, 30.0),
        bezierPoint(36.0, 30.0),
        bezierPoint(36.0, 38.0),
        bezierPoint(28.0, 38.0)
    };

    clipmask::ClipMaskStack stack;
    stack.masks = {mask};
    return stack;
}

QImage makePlanarTrackingFrame(const QPoint &shift)
{
    QImage frame(88, 72, QImage::Format_ARGB32);
    frame.fill(Qt::black);

    const QPoint origin(24 + shift.x(), 24 + shift.y());
    for (int y = 0; y < 18; ++y) {
        for (int x = 0; x < 24; ++x) {
            const int value = 35 + ((x * 19 + y * 31) % 205);
            frame.setPixel(origin.x() + x, origin.y() + y,
                           qRgb(value, value, value));
        }
    }

    QPainter painter(&frame);
    painter.setPen(QPen(Qt::white, 1));
    painter.drawRect(QRect(origin, QSize(23, 17)));
    painter.end();
    return frame;
}

int matteValueAt(const clipmask::ClipMaskStack &stack, const QPoint &point)
{
    const MaskSystem system = clipmask::toMaskSystem(stack);
    const QImage matte = MaskSystem::generateMaskImage(system.masks(), QSize(88, 72));
    if (matte.isNull() || !QRect(QPoint(0, 0), matte.size()).contains(point))
        return 0;
    return qGray(matte.pixel(point));
}

bool pointClose(const QPointF &a, const QPointF &b, double tolerance = 1.25)
{
    return std::abs(a.x() - b.x()) <= tolerance
        && std::abs(a.y() - b.y()) <= tolerance;
}

} // namespace

int runClipMaskSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok) {
        std::printf("[clip-mask] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", gate, name);
        ok ? ++passed : ++failed;
    };

    const clipmask::ClipMaskStack empty;
    check(1, "default stack serializes to no object fields",
          empty.isDefault() && clipmask::toJson(empty).isEmpty());

    const clipmask::ClipMaskStack model = makeModelStack();
    const QJsonObject modelJson = clipmask::toJson(model);
    const clipmask::ClipMaskStack modelLoaded = clipmask::fromJson(modelJson);
    check(2, "bezier model JSON round-trip preserves closed paths and controls",
          modelLoaded == model
              && modelLoaded.masks.size() == 3
              && modelLoaded.masks[0].closed
              && modelLoaded.masks[1].inverted);

    check(3, "add/subtract/intersect mode token inverse",
          clipmask::combineModeFromToken(clipmask::combineModeToken(clipmask::CombineMode::Add))
              == clipmask::CombineMode::Add
          && clipmask::combineModeFromToken(clipmask::combineModeToken(clipmask::CombineMode::Subtract))
              == clipmask::CombineMode::Subtract
          && clipmask::combineModeFromToken(clipmask::combineModeToken(clipmask::CombineMode::Intersect))
              == clipmask::CombineMode::Intersect);

    ClipInfo plain = makeClip();
    const QString plainJson = ProjectFile::toJsonString(projectWithClip(plain));
    ProjectData plainLoaded;
    const bool plainLoadOk = ProjectFile::fromJsonString(plainJson, plainLoaded);
    const QString plainResaved = ProjectFile::toJsonString(plainLoaded);
    check(4, "legacy/default project JSON omits clipMasks and round-trips byte-identical",
          !plainJson.contains(QStringLiteral("\"clipMasks\""))
              && plainLoadOk
              && plainJson == plainResaved);

    ClipInfo masked = makeClip();
    masked.maskSystem.addMask(makePathMask(QStringLiteral("add-path"), MaskMode::Add, false));
    masked.maskSystem.addMask(makePathMask(QStringLiteral("subtract-path"), MaskMode::Subtract, true));

    const QString maskedJson = ProjectFile::toJsonString(projectWithClip(masked));
    ProjectData maskedLoaded;
    const bool maskedLoadOk = ProjectFile::fromJsonString(maskedJson, maskedLoaded);
    const bool loadedShapeOk = maskedLoadOk
        && maskedLoaded.videoTracks.size() == 1
        && maskedLoaded.videoTracks[0].size() == 1
        && maskedLoaded.videoTracks[0][0].maskSystem.masks().size() == 2;
    const QVector<Mask> loadedMasks = loadedShapeOk
        ? maskedLoaded.videoTracks[0][0].maskSystem.masks()
        : QVector<Mask>();
    check(5, "ProjectFile writes clipMasks only when masks exist",
          maskedJson.contains(QStringLiteral("\"clipMasks\""))
              && maskedJson.contains(QStringLiteral("\"mode\":\"subtract\""))
              && loadedShapeOk);

    const bool projectRoundtripOk = loadedShapeOk
        && sameMaskFields(masked.maskSystem.masks()[0], loadedMasks[0])
        && sameMaskFields(masked.maskSystem.masks()[1], loadedMasks[1]);
    check(6, "ProjectFile mask fields round-trip through ClipInfo maskSystem",
          projectRoundtripOk);

    const clipmask::ClipMaskStack bakeStack = makePlanarBakeStack();
    const planar::CornerSet referenceCorners =
        planar::CornerSet::rectangle(QRectF(24.0, 24.0, 24.0, 18.0));
    planar::TrackParams params;
    params.searchRadiusPx = 10.0;
    params.patchSizePx = 12.0;
    params.dampingFactor = 1.0;
    planar::Tracker tracker;
    tracker.setParams(params);
    const QList<QImage> frames = {
        makePlanarTrackingFrame(QPoint(0, 0)),
        makePlanarTrackingFrame(QPoint(6, 4)),
        makePlanarTrackingFrame(QPoint(12, 8))
    };
    const QList<planar::Frame> trackFrames =
        tracker.trackSequence(frames, referenceCorners, 33);
    const bool trackerMovedOk = trackFrames.size() == 3
        && pointClose(trackFrames.last().corners.tl, QPointF(36.0, 32.0), 1.5)
        && pointClose(trackFrames.last().corners.br, QPointF(60.0, 50.0), 1.5);
    check(7, "synthetic planar tracker follows translated feature",
          trackerMovedOk);

    const QMap<int, clipmask::ClipMaskStack> bakedKeyframes =
        clipmask::bakePlanarTrackToMaskPathKeyframes(bakeStack,
                                                     referenceCorners,
                                                     trackFrames);
    const clipmask::ClipMaskStack bakedLast = bakedKeyframes.value(2);
    const bool maskFollowsTrack = trackerMovedOk
        && bakedKeyframes.size() == 3
        && matteValueAt(bakedLast, QPoint(44, 42)) > 220
        && matteValueAt(bakedLast, QPoint(32, 34)) < 20;
    check(8, "planar track bakes mask path keyframes that follow the feature",
          maskFollowsTrack);

    const planar::CornerSet pinnedCorners = planar::CornerSet::rectangle(
        QRectF(36.0, 30.0, 30.0, 24.0));
    const clipmask::ClipMaskStack cornerPinned =
        clipmask::applyPlanarCornerPinToMaskPath(bakeStack,
                                                 referenceCorners,
                                                 pinnedCorners);
    const bool cornerPinOk = !cornerPinned.masks.isEmpty()
        && cornerPinned.masks.first().points.size() == 4
        && pointClose(cornerPinned.masks.first().points[0].vertex,
                      QPointF(41.0, 38.0), 0.25)
        && pointClose(cornerPinned.masks.first().points[2].vertex,
                      QPointF(51.0, 48.6666666667), 0.25);
    check(9, "corner-pin conversion transforms mask vertices per keyframe",
          cornerPinOk);

    planartrack::PlanarTrack persistedTrack;
    persistedTrack.refFrameIndex = 0;
    planartrack::FrameResult persistedFrame;
    persistedFrame.frameIndex = 3;
    persistedFrame.H = {1.0, 0.0, 5.0,
                        0.0, 1.0, -2.0,
                        0.0, 0.0, 1.0};
    persistedTrack.frames.push_back(persistedFrame);
    const QMap<int, clipmask::ClipMaskStack> persistedBaked =
        clipmask::bakePlanarTrackToMaskPathKeyframes(bakeStack,
                                                     persistedTrack);
    const bool persistedOk = persistedBaked.contains(0)
        && persistedBaked.contains(3)
        && pointClose(persistedBaked.value(3).masks.first().points.first().vertex,
                      QPointF(33.0, 28.0), 0.01);
    check(10, "persisted planar homography results bake to mask path keyframes",
          persistedOk);

    std::printf("[clip-mask] summary: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
