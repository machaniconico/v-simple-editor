#include "../Camera3D.h"
#include "../ClipGeometry.h"
#include "../ProjectFile.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "SelftestRegistry.h"

#include <QJsonDocument>
#include <QLineF>
#include <QMatrix4x4>
#include <QPainter>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

// GLPreview.cpp (US-1509): the layer3D GLPreview's projection uses for the clip active at a time.
Layer3DTransform previewLayer3DForSelftest(const Timeline *timeline, double timelineSec);

namespace {
bool sameBytes(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()
        || a.format() != b.format() || a.bytesPerLine() != b.bytesPerLine())
        return false;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<size_t>(a.bytesPerLine())) != 0)
            return false;
    return true;
}

double mse(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return std::numeric_limits<double>::infinity();
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0;
    for (int y = 0; y < aa.height(); ++y)
        for (int x = 0; x < aa.width() * 4; ++x) {
            const double d = double(aa.constScanLine(y)[x]) - bb.constScanLine(y)[x];
            sum += d * d;
        }
    return sum / (aa.width() * aa.height() * 4.0);
}

QPointF centroid(const QPolygonF &quad)
{
    QPointF sum;
    for (const QPointF &p : quad)
        sum += p;
    return quad.isEmpty() ? sum : sum / quad.size();
}

ClipInfo fixtureClip(const QColor &color, const QSize &canvas)
{
    Shape rectangle;
    rectangle.type = ShapeType::Rectangle;
    rectangle.position = QPointF(canvas.width() / 2.0, canvas.height() / 2.0);
    rectangle.properties.size = QSizeF(canvas.width() * 0.8, canvas.height() * 0.8);
    rectangle.fill.color = color;
    rectangle.stroke.enabled = false;
    ClipInfo clip;
    clip.displayName = QStringLiteral("カメラ射影テスト");
    clip.duration = 2.0;
    clip.outPoint = 2.0;
    clip.shapes = {rectangle};
    return clip;
}
}

int runCamera3DSelftest()
{
    int passed = 0, failed = 0, skipped = 0;
    auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    // Restore the calling thread's switch even if a later gate is changed to exit early.
    struct RestoreSwitch {
        ~RestoreSwitch() { Camera3D::setTrueProjectionEnabledForTest(true); }
    } restoreSwitch;

    const QSize canvas(64, 48);
    Timeline timeline;
    while (timeline.videoTracks().size() < 2)
        timeline.addVideoTrack();
    ClipInfo front = fixtureClip(QColor(220, 80, 40), canvas);
    front.videoScale = 0.7;
    front.videoDx = 0.08;
    front.videoDy = -0.05;
    front.opacity = 0.8;
    front.is3DLayer = true;
    front.layer3D.rotationX = 20.0;
    ClipInfo back = fixtureClip(QColor(40, 100, 210), canvas);
    auto setFixture = [&](const ClipInfo &a, const ClipInfo &b) {
        timeline.videoTracks()[0]->setClips({a});
        timeline.videoTracks()[1]->setClips({b});
    };
    setFixture(front, back);
    Camera3DState camera;
    timeline.setProjectCamera(camera);
    Camera3D::setTrueProjectionEnabledForTest(false);
    const QImage bypass = tlrender::renderFrameAt(&timeline, 0, canvas);
    Camera3D::setTrueProjectionEnabledForTest(true);
    const QImage off = tlrender::renderFrameAt(&timeline, 0, canvas);
    const int offCalls = Camera3D::trueProjectionCallCountForTest();
    ClipInfo plain = front;
    plain.is3DLayer = false;
    plain.layer3D.reset();
    setFixture(plain, back);
    const QImage plainFrame = tlrender::renderFrameAt(&timeline, 0, canvas);
    gate(1, sameBytes(off, bypass) && sameBytes(off, plainFrame)
            && offCalls == 0 && Camera3D::trueProjectionCallCountForTest() == 0);

    camera.trueProjection = true;
    bool legacyScale = true;
    for (double z : {0.0, 40.0}) {
        Layer3DTransform layer;
        layer.positionZ = z;
        const QPolygonF quad = Camera3D::projectLayerQuad(layer, camera, canvas);
        const double scale = camera.fov / (camera.fov + z);
        const QPointF center(canvas.width() / 2.0, canvas.height() / 2.0);
        const QPolygonF corners{QPointF(0, 0), QPointF(canvas.width(), 0),
            QPointF(canvas.width(), canvas.height()), QPointF(0, canvas.height())};
        legacyScale = legacyScale && quad.size() == 4;
        for (int i = 0; i < quad.size(); ++i)
            legacyScale = legacyScale
                && QLineF(quad[i], center + (corners[i] - center) * scale).length() < 0.5;
    }
    gate(2, legacyScale);

    // A near layer, in front of the camera's lookAt origin, follows its pan.
    Layer3DTransform nearLayer;
    nearLayer.positionZ = -20;
    const QSize smallCanvas(16, 12);
    const QPointF beforePan = centroid(Camera3D::projectLayerQuad(nearLayer, camera, smallCanvas));
    Camera3DState panned = camera;
    panned.target.setX(0.1f);
    const QPointF afterPan = centroid(Camera3D::projectLayerQuad(nearLayer, panned, smallCanvas));
    gate(3, afterPan.x() < beforePan.x() - 0.1);

    Camera3DState rolled = camera;
    rolled.roll = 90;
    const QPolygonF quad = Camera3D::projectLayerQuad({}, camera, canvas);
    const QPolygonF rollQuad = Camera3D::projectLayerQuad({}, rolled, canvas);
    const QPointF center(canvas.width() / 2.0, canvas.height() / 2.0);
    bool rollOk = quad.size() == 4 && rollQuad.size() == 4;
    for (int i = 0; i < quad.size(); ++i) {
        const QPointF p = quad[i] - center;
        rollOk = rollOk && QLineF(rollQuad[i], center + QPointF(p.y(), -p.x())).length() < 0.5;
    }
    gate(4, rollOk && rollQuad[1].x() < center.x() && rollQuad[1].y() < center.y());

    Layer3DTransform rotated;
    rotated.rotationY = 60;
    const QPolygonF perspective = Camera3D::projectLayerQuad(rotated, camera, canvas);
    const double left = QLineF(perspective[0], perspective[3]).length();
    const double right = QLineF(perspective[1], perspective[2]).length();
    gate(5, std::abs(left - right) > 0.05 * std::max(left, right));

    Camera3D savedCamera;
    ProjectData data;
    data.projectCamera = savedCamera.toJson();
    const QString defaultJson = ProjectFile::toJsonString(data);
    ProjectData restored;
    bool jsonOk = !defaultJson.contains(QStringLiteral("trueProjection"))
        && Camera3DState{}.isDefault() && !camera.isDefault()
        && ProjectFile::fromJsonString(defaultJson, restored);
    QJsonObject explicitOff = data.projectCamera.value(QStringLiteral("cameraState")).toObject();
    explicitOff[QStringLiteral("trueProjection")] = false;
    data.projectCamera[QStringLiteral("cameraState")] = explicitOff;
    jsonOk = jsonOk && ProjectFile::toJsonString(data) == defaultJson;
    Camera3D loaded;
    loaded.fromJson(restored.projectCamera);
    jsonOk = jsonOk && !loaded.camera().trueProjection;
    savedCamera.setCamera(camera);
    savedCamera.setCameraKeyframe(0.0, camera);
    jsonOk = jsonOk && savedCamera.getCameraAt(0.0).trueProjection;
    data.projectCamera = savedCamera.toJson();
    const QString enabledJson = ProjectFile::toJsonString(data);
    jsonOk = jsonOk && enabledJson.contains(QStringLiteral("trueProjection"))
        && ProjectFile::fromJsonString(enabledJson, restored);
    loaded.fromJson(restored.projectCamera);
    jsonOk = jsonOk && loaded.camera().trueProjection && !loaded.camera().isDefault();
    // Exercise both ProjectFile serialization entrypoints, including default omission.
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("camera.veditor"));
    jsonOk = jsonOk && dir.isValid() && ProjectFile::save(path, data)
        && ProjectFile::load(path, restored);
    loaded.fromJson(restored.projectCamera);
    jsonOk = jsonOk && loaded.camera().trueProjection;
    savedCamera.setCamera(Camera3DState{});
    data.projectCamera = savedCamera.toJson();
    jsonOk = jsonOk && ProjectFile::save(path, data) && ProjectFile::load(path, restored)
        && !restored.projectCamera.value(QStringLiteral("cameraState")).toObject()
                .contains(QStringLiteral("trueProjection"));
    gate(6, jsonOk);

    front.layer3D.rotationY = 25;
    front.layer3D.positionZ = 12;
    front.rotation2DDegrees = 17;
    back.is3DLayer = true; // Default transform still receives camera pan and roll.
    camera.target.setX(0.1f);
    camera.roll = 12;
    setFixture(front, back);
    timeline.setProjectCamera(camera);
    Camera3D::setTrueProjectionEnabledForTest(true);
    const QImage exported = tlrender::renderFrameAt(&timeline, 0, canvas);
    const int exportCalls = Camera3D::trueProjectionCallCountForTest();
    QImage reference(canvas, QImage::Format_ARGB32_Premultiplied);
    reference.fill(Qt::transparent);
    {
        QPainter painter(&reference);
        // Existing V1-wins contract: back track first, V1 last.
        for (const ClipInfo &clip : {back, front}) {
            const QImage source = ShapeLayer::renderShapesToImage(clip.shapes, canvas);
            const QImage projected = Camera3D::applyTrueProjection(source, clip.layer3D, camera, canvas);
            const QImage placed = clipgeom::renderLayer(projected,
                {clip.videoScale, clip.videoDx, clip.videoDy, clip.rotation2DDegrees}, canvas, true);
            painter.setOpacity(clip.opacity);
            painter.drawImage(0, 0, placed);
        }
    }
    Camera3D::setTrueProjectionEnabledForTest(false);
    const QImage enabledButBypassed = tlrender::renderFrameAt(&timeline, 0, canvas);
    bool exportOk = exportCalls == 2 && mse(exported, reference) < 1.0
            && mse(exported, enabledButBypassed) > 1.0
            && Camera3D::trueProjectionCallCountForTest() == 0;
    // A non-default transform opts a layer in even without the explicit flag.
    // Conversely, an ordinary 2D layer must remain untouched with the camera ON.
    front.is3DLayer = false;
    setFixture(front, back);
    Camera3D::setTrueProjectionEnabledForTest(true);
    const QImage implicit3D = tlrender::renderFrameAt(&timeline, 0, canvas);
    exportOk = exportOk && sameBytes(exported, implicit3D)
        && Camera3D::trueProjectionCallCountForTest() == 2;
    plain = front;
    plain.layer3D.reset();
    back.is3DLayer = false;
    setFixture(plain, back);
    Camera3D::setTrueProjectionEnabledForTest(true);
    const QImage ordinary2D = tlrender::renderFrameAt(&timeline, 0, canvas);
    exportOk = exportOk && Camera3D::trueProjectionCallCountForTest() == 0;
    camera.trueProjection = false;
    timeline.setProjectCamera(camera);
    exportOk = exportOk && sameBytes(ordinary2D,
        tlrender::renderFrameAt(&timeline, 0, canvas));
    gate(7, exportOk);

    // Both sides of a real borrowed overlap must pass through projection.
    // Keep the outgoing layer unrotated so the image difference specifically
    // exercises the incoming layer's projection before transition placement.
    Timeline overlapTimeline;
    ClipInfo outgoing = fixtureClip(QColor(220, 80, 40), canvas);
    ClipInfo incoming = fixtureClip(QColor(40, 100, 210), canvas);
    outgoing.duration = incoming.duration = 4.0;
    outgoing.inPoint = incoming.inPoint = 1.0;
    outgoing.outPoint = incoming.outPoint = 3.0;
    outgoing.is3DLayer = incoming.is3DLayer = true;
    incoming.layer3D.rotationY = 60.0;
    outgoing.trailOut.type = incoming.leadIn.type = TransitionType::CrossDissolve;
    outgoing.trailOut.duration = incoming.leadIn.duration = 1.0;
    outgoing.trailOut.alignment = incoming.leadIn.alignment = TransitionAlignment::Center;
    overlapTimeline.videoTracks()[0]->setClips({outgoing, incoming});
    const auto overlapIntervals = overlapTimeline.videoOverlapIntervals();
    bool overlapOk = !overlapIntervals.isEmpty() && overlapIntervals[0].size() == 2;
    if (overlapOk) {
        const auto &a = overlapIntervals[0][0];
        const auto &b = overlapIntervals[0][1];
        overlapOk = b.timelineStart < a.timelineEnd
            && std::abs((a.timelineEnd - b.timelineStart) - 1.0) < 1e-9;
        const qint64 midpointUsec = qRound64(
            (b.timelineStart + a.timelineEnd) * 0.5 * 1000000.0);
        Camera3DState overlapCamera;
        overlapCamera.trueProjection = true;
        overlapTimeline.setProjectCamera(overlapCamera);
        Camera3D::setTrueProjectionEnabledForTest(true);
        const QImage projectedOverlap = tlrender::renderFrameAt(
            &overlapTimeline, midpointUsec, canvas);
        const int overlapCalls = Camera3D::trueProjectionCallCountForTest();
        Camera3D::setTrueProjectionEnabledForTest(false);
        const QImage bypassedOverlap = tlrender::renderFrameAt(
            &overlapTimeline, midpointUsec, canvas);
        overlapOk = overlapOk && !projectedOverlap.isNull() && !bypassedOverlap.isNull()
            && overlapCalls == 2 && mse(projectedOverlap, bypassedOverlap) > 1.0
            && Camera3D::trueProjectionCallCountForTest() == 0;

        overlapCamera.trueProjection = false;
        overlapTimeline.setProjectCamera(overlapCamera);
        Camera3D::setTrueProjectionEnabledForTest(true);
        const QImage offOverlap = tlrender::renderFrameAt(
            &overlapTimeline, midpointUsec, canvas);
        overlapOk = overlapOk && Camera3D::trueProjectionCallCountForTest() == 0
            && sameBytes(offOverlap, bypassedOverlap);
        outgoing.is3DLayer = incoming.is3DLayer = false;
        outgoing.layer3D.reset();
        incoming.layer3D.reset();
        overlapTimeline.videoTracks()[0]->setClips({outgoing, incoming});
        const QImage plainOverlap = tlrender::renderFrameAt(
            &overlapTimeline, midpointUsec, canvas);
        overlapOk = overlapOk && sameBytes(offOverlap, plainOverlap)
            && Camera3D::trueProjectionCallCountForTest() == 0;
    }
    gate(8, overlapOk);

    // A static source spanning both sample times isolates camera animation.
    Timeline animatedTimeline;
    ClipInfo animatedClip = fixtureClip(QColor(220, 80, 40), canvas);
    animatedClip.duration = animatedClip.outPoint = 3.0;
    animatedClip.is3DLayer = true;
    animatedTimeline.videoTracks()[0]->setClips({animatedClip});
    Camera3DState animatedState;
    animatedState.trueProjection = true;
    Camera3D animatedCamera{animatedState};
    animatedCamera.track(Camera3DProperty::PositionX)->addKeyframe(0.0, 0.0);
    animatedCamera.track(Camera3DProperty::PositionX)->addKeyframe(2.0, 200.0);
    animatedTimeline.setProjectCamera(animatedCamera);
    Camera3D::setTrueProjectionEnabledForTest(true);
    const QImage atZero = tlrender::renderFrameAt(&animatedTimeline, 0, canvas);
    const QImage atTwo = tlrender::renderFrameAt(&animatedTimeline, 2000000, canvas);
    const QImage animatedSource = ShapeLayer::renderShapesToImage(animatedClip.shapes, canvas);
    const QImage atTwoReference = Camera3D::applyTrueProjection(
        animatedSource, animatedClip.layer3D, animatedCamera.getCameraAt(2.0), canvas);
    bool animationOk = !sameBytes(atZero, atTwo) && mse(atTwo, atTwoReference) < 1.0;
    animatedTimeline.setProjectCamera(Camera3D{animatedState});
    animationOk = animationOk && sameBytes(
        tlrender::renderFrameAt(&animatedTimeline, 0, canvas),
        tlrender::renderFrameAt(&animatedTimeline, 2000000, canvas));

    // OFF must bypass projection even when keyframe tracks are present.
    animatedState.trueProjection = false;
    animatedCamera.setCamera(animatedState);
    animatedTimeline.setProjectCamera(animatedCamera);
    Camera3D::setTrueProjectionEnabledForTest(false);
    const QImage animatedBypass = tlrender::renderFrameAt(&animatedTimeline, 2000000, canvas);
    Camera3D::setTrueProjectionEnabledForTest(true);
    const QImage animatedOff = tlrender::renderFrameAt(&animatedTimeline, 2000000, canvas);
    animationOk = animationOk && sameBytes(animatedOff, animatedBypass)
        && Camera3D::trueProjectionCallCountForTest() == 0;
    animatedTimeline.setProjectCamera(animatedState);
    animationOk = animationOk && sameBytes(animatedOff,
        tlrender::renderFrameAt(&animatedTimeline, 2000000, canvas));
    gate(9, animationOk);

    // G10 (US-1509): autoOrientToCamera orients a 3D layer towards a trueProjection camera.
    // Camera (0,0,-500), target = position + (0,0,-1) (the default target (0,0,-1) would make
    // forward +z and mirror x). Detects: an effective layer3D that ignores the flag (the
    // mutation build that never reads it renders ON == OFF), a lighting / GLPreview / overlay /
    // V1 site that still reads clip.layer3D (ON != the baked-helper reference), a flag on a
    // non-3D layer, a lost JSON key, an undo count != 1.
    {
        Camera3DState orientCam;
        orientCam.position = QVector3D(0.0f, 0.0f, -500.0f);
        orientCam.target = orientCam.position + QVector3D(0.0f, 0.0f, -1.0f);
        orientCam.fov = 500.0;
        orientCam.trueProjection = true;
        ClipInfo tilted = fixtureClip(QColor(220, 80, 40), canvas);
        tilted.is3DLayer = true;
        tilted.layer3D.rotationY = 60.0;
        tilted.layer3D.rotationZ = 15.0;
        tilted.autoOrientToCamera = true;
        ClipInfo offClip = tilted;
        offClip.autoOrientToCamera = false;

        // (1) helper values. The +x sign is the Step 0 print: rotationY positive.
        const Layer3DTransform atCentre = camera3d::effectiveLayer3DAt(tilted, QPointF(0, 0), orientCam);
        const Layer3DTransform atPlusX = camera3d::effectiveLayer3DAt(tilted, QPointF(150, 0), orientCam);
        const Layer3DTransform atPlusY = camera3d::effectiveLayer3DAt(tilted, QPointF(0, 100), orientCam);
        // Independent normal check: model order of projectLayerQuad (Rz, Ry, Rx), the mapped
        // (0,0,1) must point at the camera (+z side) from the layer's position.
        const auto normalDot = [&](const Layer3DTransform &l, QPointF px) {
            QMatrix4x4 m;
            m.rotate(static_cast<float>(l.rotationZ), 0, 0, 1);
            m.rotate(static_cast<float>(l.rotationY), 0, 1, 0);
            m.rotate(static_cast<float>(l.rotationX), 1, 0, 0);
            const QVector3D n = m.mapVector(QVector3D(0, 0, 1)).normalized();
            QVector3D d = orientCam.position - QVector3D(static_cast<float>(px.x()),
                                                         static_cast<float>(px.y()),
                                                         static_cast<float>(l.positionZ));
            d.normalize();
            if (d.z() < 0.0f)
                d = -d;
            return static_cast<double>(QVector3D::dotProduct(n, d));
        };
        ClipInfo rz90 = tilted;
        rz90.layer3D.rotationZ = 90.0;
        ClipInfo rz0 = tilted;
        rz0.layer3D.rotationZ = 0.0;
        const Layer3DTransform atBoth = camera3d::effectiveLayer3DAt(tilted, QPointF(150, 100), orientCam);
        const Layer3DTransform atRz90 = camera3d::effectiveLayer3DAt(rz90, QPointF(150, 0), orientCam);
        const Layer3DTransform atRz0 = camera3d::effectiveLayer3DAt(rz0, QPointF(150, 0), orientCam);
        const double dotCentre = normalDot(atCentre, QPointF(0, 0));
        const double dotPlusX = normalDot(atPlusX, QPointF(150, 0));
        const double dotPlusY = normalDot(atPlusY, QPointF(0, 100));
        const double dotBoth = normalDot(atBoth, QPointF(150, 100));
        const double dotRz90 = normalDot(atRz90, QPointF(150, 0));
        // Negative control: the round-1 solution ignored rotationZ; it must fail the same check.
        Layer3DTransform uncomp = rz90.layer3D;
        uncomp.rotationX = 0.0;
        uncomp.rotationY = std::atan2(150.0, 500.0) * 180.0 / M_PI;
        const double dotUncomp = normalDot(uncomp, QPointF(150, 0));
        const double kNormalMin = 1.0 - 1e-6;
        const QPolygonF quadPlusX = Camera3D::projectLayerQuad(atPlusX, orientCam, canvas);
        std::fprintf(stderr, "camera3d G10 helper: centre rx=%.6f ry=%.6f | +x(150) rx=%.6f ry=%.6f"
                     " | +y(100) rx=%.6f ry=%.6f | rz kept %.1f | rz0 +x ry=%.6f | rz90 +x rx=%.6f ry=%.6f"
                     " | normal dots centre/+x/+y/both/rz90 %.9f/%.9f/%.9f/%.9f/%.9f uncompensated %.6f"
                     " | +x quad left/right edge %.3f/%.3f\n",
                     atCentre.rotationX, atCentre.rotationY, atPlusX.rotationX, atPlusX.rotationY,
                     atPlusY.rotationX, atPlusY.rotationY, atPlusX.rotationZ, atRz0.rotationY,
                     atRz90.rotationX, atRz90.rotationY, dotCentre, dotPlusX, dotPlusY, dotBoth, dotRz90,
                     dotUncomp,
                     quadPlusX.size() == 4 ? QLineF(quadPlusX[0], quadPlusX[3]).length() : -1.0,
                     quadPlusX.size() == 4 ? QLineF(quadPlusX[1], quadPlusX[2]).length() : -1.0);
        const auto sameLayer = [](const Layer3DTransform &a, const Layer3DTransform &b) {
            return a.positionZ == b.positionZ && a.rotationX == b.rotationX
                && a.rotationY == b.rotationY && a.rotationZ == b.rotationZ;
        };
        ClipInfo flat = tilted;
        flat.is3DLayer = false;
        Camera3DState flatCam = orientCam;
        flatCam.trueProjection = false;
        const bool helperOk = std::abs(atCentre.rotationX) < 1e-6 && std::abs(atCentre.rotationY) < 1e-6
            && atRz0.rotationY > 10.0 && std::abs(atRz0.rotationY - std::atan2(150.0, 500.0) * 180.0 / M_PI) < 1e-6
            && dotCentre > kNormalMin && dotPlusX > kNormalMin && dotPlusY > kNormalMin
            && dotBoth > kNormalMin && dotRz90 > kNormalMin && dotUncomp < kNormalMin
            && atPlusX.rotationZ == 15.0 && atRz90.rotationZ == 90.0
            && sameLayer(camera3d::effectiveLayer3DAt(offClip, QPointF(150, 0), orientCam), offClip.layer3D)
            && sameLayer(camera3d::effectiveLayer3DAt(flat, QPointF(150, 0), orientCam), flat.layer3D)
            && sameLayer(camera3d::effectiveLayer3DAt(tilted, QPointF(150, 0), flatCam), tilted.layer3D);

        // (2) renderFrameAt, upper track (overlay path): ON differs from OFF, and equals OFF with
        // the helper's rotations baked in (a clip.layer3D read anywhere would not).
        Timeline orientTl;
        while (orientTl.videoTracks().size() < 2)
            orientTl.addVideoTrack();
        orientTl.setProjectOutputConfig(canvas.width(), canvas.height(), true, 30.0);
        orientTl.setProjectCamera(orientCam);
        Camera3D::setTrueProjectionEnabledForTest(true);
        ClipInfo backdrop = fixtureClip(QColor(40, 100, 210), canvas);
        const double dx = 0.3;
        const QPointF px(dx * canvas.width(), 0.0);
        const auto baked = [&](ClipInfo clip, const Camera3DState &cam) {
            clip.layer3D = camera3d::effectiveLayer3DAt(clip, px, cam);
            clip.autoOrientToCamera = false;
            return clip;
        };
        const auto place = [&](ClipInfo clip) { clip.videoDx = dx; return clip; };
        const auto renderPair = [&](const ClipInfo &v1, const ClipInfo &upper) {
            orientTl.videoTracks()[0]->setClips({v1});
            orientTl.videoTracks()[1]->setClips({upper});
            return tlrender::renderFrameAt(&orientTl, 0, canvas);
        };
        const QImage upperOn = renderPair(backdrop, place(tilted));
        const QImage upperOff = renderPair(backdrop, place(offClip));
        const QImage upperBaked = renderPair(backdrop, place(baked(tilted, orientCam)));
        const double upperMse = mse(upperOn, upperOff);
        const bool upperOk = upperMse > 1.0 && sameBytes(upperOn, upperBaked);
        // (3) v1Contained path: the 3D layer is V1.
        const QImage v1On = renderPair(place(tilted), backdrop);
        const QImage v1Off = renderPair(place(offClip), backdrop);
        const QImage v1Baked = renderPair(place(baked(tilted, orientCam)), backdrop);
        const double v1Mse = mse(v1On, v1Off);
        const bool v1Ok = v1Mse > 1.0 && sameBytes(v1On, v1Baked);
        // (4) a non-3D layer with the flag set renders as with it cleared.
        const QImage flatOn = renderPair(backdrop, place(flat));
        ClipInfo flatOff = flat;
        flatOff.autoOrientToCamera = false;
        const bool flatOk = sameBytes(flatOn, renderPair(backdrop, place(flatOff)));
        // (5) lighting normals come from the helper's value (ON-lit == baked-lit), and the
        // lighting is real and normal-sensitive (sanity bands).
        Light3DState parallel;
        parallel.type = LightType::Parallel;
        parallel.position = QVector3D(0.0f, 0.0f, 500.0f);
        parallel.target = QVector3D(0.0f, 0.0f, 0.0f);
        orientTl.setProjectLights({Light3D(parallel)});
        ClipInfo litOn = place(tilted), litOff = place(offClip);
        for (ClipInfo *lit : {&litOn, &litOff}) {
            lit->material.ambientCoeff = 0.0;
            lit->material.diffuseCoeff = 1.0;
        }
        const QImage lightOn = renderPair(litOn, backdrop);
        const QImage lightBaked = renderPair(baked(litOn, orientCam), backdrop);
        const QImage lightOff = renderPair(litOff, backdrop);
        const double normalOn = light3d::layerNormal(camera3d::effectiveLayer3DAt(litOn, px, orientCam)).z();
        const double normalOff = light3d::layerNormal(litOn.layer3D).z();
        orientTl.setProjectLights({});
        const double lightMse = mse(lightOn, v1On);
        const bool lightOk = sameBytes(lightOn, lightBaked) && lightMse > 0.5 && mse(lightOn, lightOff) > 1.0
            && normalOn > normalOff + 0.2;
        // (6) GLPreview's projection input equals the helper's value, and projecting with it
        // by hand reproduces the exported V1 layer (the gate-7 reference).
        orientTl.videoTracks()[0]->setClips({place(tilted)});
        orientTl.videoTracks()[1]->setClips({});
        const Layer3DTransform glLayer = previewLayer3DForSelftest(&orientTl, 0.0);
        const Layer3DTransform wantLayer = camera3d::effectiveLayer3DAt(place(tilted), px, orientCam);
        const QImage exportedV1 = tlrender::renderFrameAt(&orientTl, 0, canvas);
        const QImage glReference = clipgeom::renderLayer(
            Camera3D::applyTrueProjection(
                ShapeLayer::renderShapesToImage(tilted.shapes, canvas), glLayer, orientCam, canvas),
            {tilted.videoScale, dx, tilted.videoDy, tilted.rotation2DDegrees}, canvas, true);
        const double glMse = mse(exportedV1, glReference);
        std::fprintf(stderr, "camera3d G10 gl: ry=%.6f (want %.6f) mse(export, projection with GL input)=%.4f\n",
                     glLayer.rotationY, wantLayer.rotationY, glMse);
        const bool glOk = sameLayer(glLayer, wantLayer) && !sameLayer(glLayer, tilted.layer3D) && glMse < 1.0;
        // (7) JSON round trip (the key is written only when set) and the setter's undo point.
        const auto roundTrip = [](const ClipInfo &clip, QString *json) {
            ProjectData project;
            project.videoTracks = QVector<QVector<ClipInfo>>{{clip}};
            *json = ProjectFile::toJsonString(project);
            ProjectData back;
            return ProjectFile::fromJsonString(*json, back) && !back.videoTracks.isEmpty()
                    && !back.videoTracks[0].isEmpty()
                ? back.videoTracks[0][0] : ClipInfo{};
        };
        QString onText, offText;
        const ClipInfo onBack = roundTrip(tilted, &onText);
        const ClipInfo offBack = roundTrip(offClip, &offText);
        const QString key = QStringLiteral("autoOrientToCamera");
        const bool jsonOk = onText.contains(key) && onBack.autoOrientToCamera && onBack.is3DLayer
            && !offText.contains(key) && !offBack.autoOrientToCamera;
        Timeline undoTl;
        undoTl.restoreFromProject(QVector<QVector<ClipInfo>>{{offClip}}, QVector<QVector<ClipInfo>>{{}},
                                  0.0, -1.0, -1.0, 100);
        undoTl.undoManager()->clear();
        undoTl.saveUndoState(QStringLiteral("Initial"));
        const int depth0 = undoTl.undoManager()->currentIndex();
        const bool setOn = undoTl.setClipAutoOrientToCamera(0, 0, true);
        const int depth1 = undoTl.undoManager()->currentIndex();
        const bool isOn = undoTl.videoTracks()[0]->clips().at(0).autoOrientToCamera;
        const bool setAgain = undoTl.setClipAutoOrientToCamera(0, 0, true);
        const int depth2 = undoTl.undoManager()->currentIndex();
        undoTl.undo();
        const bool undone = !undoTl.videoTracks()[0]->clips().at(0).autoOrientToCamera;
        const bool undoOk = setOn && isOn && depth1 == depth0 + 1 && setAgain && depth2 == depth1 && undone;
        std::fprintf(stderr, "camera3d G10: helper=%d upper(mse %.2f)=%d v1(mse %.2f)=%d flat=%d "
                     "light(mse %.2f, nz %.3f vs %.3f)=%d gl=%d json=%d undo=%d\n",
                     helperOk, upperMse, upperOk, v1Mse, v1Ok, flatOk, lightMse, normalOn, normalOff,
                     lightOk, glOk, jsonOk, undoOk);
        gate(10, helperOk && upperOk && v1Ok && flatOk && lightOk && glOk && jsonOk && undoOk);
    }
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    // The registry checks the declared "(N gates)" against this count.
    selftests::reportTally(passed, failed, skipped);
    return failed;
}
