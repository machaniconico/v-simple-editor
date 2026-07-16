#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../mask/ClipMask.h"

#include <cstdio>

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

    std::printf("[clip-mask] summary: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
