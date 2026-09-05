#include "../Camera3D.h"
#include "../ClipGeometry.h"
#include "../ProjectFile.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"

#include <QJsonDocument>
#include <QLineF>
#include <QPainter>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

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
    int passed = 0, failed = 0;
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
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
