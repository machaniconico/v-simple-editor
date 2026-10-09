#include "../Camera3D.h"
#include "../Light3D.h"
#include "../Light3DDialog.h"
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TrackMatteBake.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QImage>
#include <QJsonObject>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QVector3D>

#include <cmath>
#include <cstring>
#include <iostream>

namespace {

constexpr double kTolerance = 1.0e-6;
constexpr double kPi = 3.14159265358979323846;

bool closeTo(double a, double b, double tolerance = kTolerance)
{
    return std::abs(a - b) <= tolerance;
}

bool sameImage(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()
        || a.bytesPerLine() != b.bytesPerLine()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<std::size_t>(a.bytesPerLine())) != 0) {
            return false;
        }
    }
    return true;
}

bool sameRgbaBytes(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.size() != bb.size())
        return false;
    for (int y = 0; y < aa.height(); ++y) {
        if (std::memcmp(aa.constScanLine(y), bb.constScanLine(y),
                        static_cast<std::size_t>(aa.width() * 4)) != 0) {
            return false;
        }
    }
    return true;
}

bool sameState(const Light3DState &a, const Light3DState &b)
{
    return a.type == b.type
        && a.name == b.name
        && a.enabled == b.enabled
        && a.position == b.position
        && a.target == b.target
        && a.color == b.color
        && closeTo(a.intensity, b.intensity)
        && a.falloffEnabled == b.falloffEnabled
        && closeTo(a.falloffRadius, b.falloffRadius)
        && closeTo(a.falloffDistance, b.falloffDistance)
        && closeTo(a.coneAngle, b.coneAngle)
        && closeTo(a.coneFeather, b.coneFeather);
}

bool sameMaterial(const LayerMaterial &a, const LayerMaterial &b)
{
    return a.acceptsLights == b.acceptsLights
        && closeTo(a.ambientCoeff, b.ambientCoeff)
        && closeTo(a.diffuseCoeff, b.diffuseCoeff)
        && closeTo(a.specularCoeff, b.specularCoeff)
        && closeTo(a.shininess, b.shininess);
}

QImage makeTestImage()
{
    QImage image(QSize(4, 1), QImage::Format_ARGB32);
    image.setPixel(0, 0, qRgba(20, 30, 40, 255));
    image.setPixel(1, 0, qRgba(40, 50, 60, 255));
    image.setPixel(2, 0, qRgba(60, 70, 80, 255));
    image.setPixel(3, 0, qRgba(80, 90, 100, 255));
    return image;
}

constexpr int kExportClipWidth = 64;
constexpr int kExportClipHeight = 16;
constexpr int kExportClipFps = 12;
constexpr int kExportClipFrames = 3;

QImage makeExportFrame(int frameIndex)
{
    QImage image(QSize(kExportClipWidth, kExportClipHeight), QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor(
                (60 + x * 2 + frameIndex * 3) & 255,
                (70 + y * 4 + frameIndex * 5) & 255,
                (80 + x + y * 3 + frameIndex * 7) & 255));
        }
    }
    return image;
}

bool writeExportClip(const QString &path, QString *error)
{
    libavcore::EncodeRequest request;
    request.width = kExportClipWidth;
    request.height = kExportClipHeight;
    request.fps = kExportClipFps;
    request.fpsNum = kExportClipFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 800000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (const auto openError = encoder.open(request)) {
        if (error)
            *error = QStringLiteral("FrameEncoder::open failed: ")
                + QString::fromStdString(*openError);
        return false;
    }
    for (int frameIndex = 0; frameIndex < kExportClipFrames; ++frameIndex) {
        if (!encoder.pushFrame(makeExportFrame(frameIndex), frameIndex)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrame failed at frame %1")
                    .arg(frameIndex);
            return false;
        }
    }
    if (const auto finalizeError = encoder.finalize()) {
        if (error)
            *error = QStringLiteral("FrameEncoder::finalize failed: ")
                + QString::fromStdString(*finalizeError);
        return false;
    }
    return true;
}

ClipInfo makeExportClip(const QString &path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("Light3D export fixture");
    clip.duration = static_cast<double>(kExportClipFrames) / kExportClipFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
}

bool setExportClip(Timeline &timeline, const ClipInfo &clip)
{
    if (timeline.videoTracks().isEmpty())
        timeline.addVideoTrack();
    TimelineTrack *track = timeline.videoTracks().value(0, nullptr);
    if (!track)
        return false;
    track->setClips(QVector<ClipInfo>{clip});
    return true;
}

double meanLuma(const QImage &image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return 0.0;
    double sum = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            sum += (static_cast<double>(qRed(pixel))
                    + static_cast<double>(qGreen(pixel))
                    + static_cast<double>(qBlue(pixel))) / 3.0;
        }
    }
    return sum / static_cast<double>(image.width() * image.height());
}

QImage exportComposite(const QVector<CompositeLayer> &layers,
                       const QImage &decoded,
                       const QSize &canvas,
                       const QVector<QPointF> &offsets,
                       const QVector<Light3DState> &lights,
                       const QVector3D &viewPos)
{
    QVector<QImage> decodedLayers{decoded};
    const QVector<QImage> litLayers = tlrender::detail::applyLightingToLayerImages(
        layers, decodedLayers, canvas, offsets, lights, viewPos);
    return trackmatte::composite(layers, litLayers, canvas);
}

QVector3D pointAtAngle(double degrees, double distance)
{
    const double radians = degrees * kPi / 180.0;
    return QVector3D(
        static_cast<float>(std::sin(radians) * distance),
        0.0f,
        static_cast<float>(100.0 - std::cos(radians) * distance));
}

} // namespace

int runLight3DSelftest()
{
    selftest::GateTally tally;

    const QImage input = makeTestImage();
    Layer3DTransform identity;
    LayerMaterial defaultMaterial;
    const QVector3D viewPos(0.0f, 0.0f, 1000.0f);

    const QImage noLights = light3d::applyLighting(
        input, {}, identity, QVector3D(), QSizeF(input.size()), viewPos,
        defaultMaterial);
    tally.gate(1,
        sameImage(input, noLights),
        "empty light list changed pixels");

    Light3DState disabledState;
    disabledState.enabled = false;
    const QImage disabled = light3d::applyLighting(
        input, {disabledState}, identity, QVector3D(), QSizeF(input.size()),
        viewPos, defaultMaterial);
    tally.gate(2,
        sameImage(input, disabled),
        "disabled light changed pixels");

    Light3DState ambient;
    ambient.type = LightType::Ambient;
    ambient.color = Qt::white;
    ambient.intensity = 1.0;
    LayerMaterial ambientMaterial;
    ambientMaterial.ambientCoeff = 1.0;
    ambientMaterial.diffuseCoeff = 0.0;
    ambientMaterial.specularCoeff = 0.0;
    const QImage ambientImage = light3d::applyLighting(
        input, {ambient}, identity, QVector3D(), QSizeF(input.size()), viewPos,
        ambientMaterial);
    double minGain = 100.0;
    double maxGain = 0.0;
    for (int x = 0; x < input.width(); ++x) {
        const QRgb before = input.pixel(x, 0);
        const QRgb after = ambientImage.pixel(x, 0);
        const double gain = static_cast<double>(qRed(after)) / qRed(before);
        minGain = qMin(minGain, gain);
        maxGain = qMax(maxGain, gain);
    }
    tally.gate(3,
        maxGain - minGain < 1.0 / 255.0,
        "ambient gain was not spatially uniform");

    Light3DState parallel;
    parallel.type = LightType::Parallel;
    parallel.position = QVector3D(0.0f, 0.0f, 500.0f);
    parallel.target = QVector3D(0.0f, 0.0f, 0.0f);
    parallel.color = Qt::white;
    LayerMaterial diffuseMaterial;
    diffuseMaterial.ambientCoeff = 0.0;
    diffuseMaterial.diffuseCoeff = 1.0;
    diffuseMaterial.specularCoeff = 0.0;
    const light3d::LightingResult front = light3d::computeLighting(
        {parallel}, QVector3D(), light3d::layerNormal(identity), viewPos,
        diffuseMaterial);
    Layer3DTransform sideTransform;
    sideTransform.rotationY = 90.0;
    const light3d::LightingResult side = light3d::computeLighting(
        {parallel}, QVector3D(), light3d::layerNormal(sideTransform), viewPos,
        diffuseMaterial);
    std::cerr << "light3d G4 front.r=" << front.r
              << " side.r=" << side.r << '\n';
    tally.gate(4,
        front.r > 1.9 && closeTo(side.r, 1.0),
        "parallel light did not follow the layer normal");

    Light3DState point;
    point.type = LightType::Point;
    point.position = QVector3D(0.0f, 0.0f, 100.0f);
    point.target = QVector3D();
    point.falloffEnabled = true;
    point.falloffRadius = 10.0;
    point.falloffDistance = 20.0;
    const light3d::LightingResult nearFalloff = light3d::computeLighting(
        {point}, QVector3D(0.0f, 0.0f, 90.0f),
        QVector3D(0.0f, 0.0f, 1.0f), viewPos, diffuseMaterial);
    const light3d::LightingResult farFalloff = light3d::computeLighting(
        {point}, QVector3D(), QVector3D(0.0f, 0.0f, 1.0f), viewPos,
        diffuseMaterial);
    std::cerr << "light3d G5 nearFalloff.r=" << nearFalloff.r
              << " farFalloff.r=" << farFalloff.r << '\n';
    tally.gate(5,
        closeTo(nearFalloff.r, 2.0) && closeTo(farFalloff.r, 1.0),
        "point falloff radius/distance boundaries were wrong");

    Light3DState spot;
    spot.type = LightType::Spot;
    spot.position = QVector3D(0.0f, 0.0f, 100.0f);
    spot.target = QVector3D();
    spot.coneAngle = 60.0;
    spot.coneFeather = 50.0;
    const light3d::LightingResult spotInner = light3d::computeLighting(
        {spot}, pointAtAngle(10.0, 100.0), QVector3D(0.0f, 0.0f, 1.0f),
        viewPos, diffuseMaterial);
    const light3d::LightingResult spotFeatherNear = light3d::computeLighting(
        {spot}, pointAtAngle(22.5, 100.0), QVector3D(0.0f, 0.0f, 1.0f),
        viewPos, diffuseMaterial);
    const light3d::LightingResult spotFeatherFar = light3d::computeLighting(
        {spot}, pointAtAngle(27.5, 100.0), QVector3D(0.0f, 0.0f, 1.0f),
        viewPos, diffuseMaterial);
    const light3d::LightingResult spotOutside = light3d::computeLighting(
        {spot}, pointAtAngle(31.0, 100.0), QVector3D(0.0f, 0.0f, 1.0f),
        viewPos, diffuseMaterial);
    std::cerr << "light3d G6 spotInner.r=" << spotInner.r
              << " spotFeatherNear.r=" << spotFeatherNear.r
              << " spotFeatherFar.r=" << spotFeatherFar.r
              << " spotOutside.r=" << spotOutside.r << '\n';
    tally.gate(6,
        spotInner.r > 1.9
                 && spotFeatherNear.r > spotFeatherFar.r
                 && spotFeatherFar.r > 1.0
                 && closeTo(spotOutside.r, 1.0),
        "spot cone feather was not bounded and monotonic");

    Light3DState red = parallel;
    red.color = QColor(255, 0, 0);
    const light3d::LightingResult redResult = light3d::computeLighting(
        {red}, QVector3D(), QVector3D(0.0f, 0.0f, 1.0f), viewPos,
        diffuseMaterial);
    std::cerr << "light3d G7 redResult.r=" << redResult.r
              << " g=" << redResult.g << " b=" << redResult.b << '\n';
    tally.gate(7,
        redResult.r > 1.0 && closeTo(redResult.g, 1.0)
                 && closeTo(redResult.b, 1.0),
        "colored light altered an unlit channel");

    Light3DState custom;
    custom.type = LightType::Spot;
    custom.name = QStringLiteral("Key Light");
    custom.enabled = false;
    custom.position = QVector3D(-12.5f, 8.25f, 420.0f);
    custom.target = QVector3D(2.0f, -3.0f, 0.0f);
    custom.color = QColor(10, 20, 30);
    custom.intensity = 1.75;
    custom.falloffEnabled = true;
    custom.falloffRadius = 123.0;
    custom.falloffDistance = 456.0;
    custom.coneAngle = 47.0;
    custom.coneFeather = 12.0;
    const Light3DState customRoundTrip = Light3DState::fromJson(custom.toJson());
    LayerMaterial customMaterial;
    customMaterial.acceptsLights = false;
    customMaterial.ambientCoeff = 0.25;
    customMaterial.diffuseCoeff = 0.75;
    customMaterial.specularCoeff = 0.4;
    customMaterial.shininess = 64.0;
    const LayerMaterial materialRoundTrip = LayerMaterial::fromJson(customMaterial.toJson());
    Light3D animated(custom);
    Light3DState keyframeStart = custom;
    keyframeStart.intensity = 0.5;
    Light3DState keyframeEnd = custom;
    keyframeEnd.intensity = 2.5;
    animated.setKeyframe(0.0, keyframeStart);
    animated.setKeyframe(1.0, keyframeEnd);
    const Light3D restoredAnimated = Light3D::fromJson(animated.toJson());
    const bool defaultFieldsOmitted = Light3DState{}.toJson().isEmpty()
        && LayerMaterial{}.toJson().isEmpty();
    const bool sameStateRoundTrip = sameState(custom, customRoundTrip);
    const bool sameMaterialRoundTrip =
        sameMaterial(customMaterial, materialRoundTrip);
    const bool animatedIntensityEq =
        closeTo(restoredAnimated.stateAt(0.5).intensity,
                animated.stateAt(0.5).intensity);
    std::cerr << "light3d G8 sameState=" << sameStateRoundTrip
              << " sameMaterial=" << sameMaterialRoundTrip
              << " defaultFieldsOmitted=" << defaultFieldsOmitted
              << " animatedIntensityEq=" << animatedIntensityEq << '\n';
    tally.gate(8,
        sameStateRoundTrip && sameMaterialRoundTrip && defaultFieldsOmitted
                 && animatedIntensityEq,
        "light/material JSON or keyframe round-trip failed");

    LayerMaterial specularMaterial;
    specularMaterial.ambientCoeff = 0.0;
    specularMaterial.diffuseCoeff = 0.0;
    specularMaterial.specularCoeff = 0.8;
    specularMaterial.shininess = 20.0;
    const light3d::LightingResult noSpec = light3d::computeLighting(
        {parallel}, QVector3D(), QVector3D(0.0f, 0.0f, 1.0f), viewPos,
        LayerMaterial{});
    const light3d::LightingResult spec = light3d::computeLighting(
        {parallel}, QVector3D(), QVector3D(0.0f, 0.0f, 1.0f),
        QVector3D(0.0f, 0.0f, 500.0f), specularMaterial);
    tally.gate(9,
        spec.r > noSpec.r,
        "Blinn-Phong highlight did not add energy");

    const QVector<Light3DState> deterministicLights{ambient, parallel, spot};
    const QImage first = light3d::applyLighting(
        input, deterministicLights, identity, QVector3D(), QSizeF(input.size()),
        viewPos, defaultMaterial);
    const QImage second = light3d::applyLighting(
        input, deterministicLights, identity, QVector3D(), QSizeF(input.size()),
        viewPos, defaultMaterial);
    tally.gate(10,
        sameImage(first, second),
        "repeated lighting was not bit deterministic");

    const QSize exportCanvas(kExportClipWidth, kExportClipHeight);
    QTemporaryDir exportTempDir;
    QString exportMediaError;
    const QString exportMediaPath = exportTempDir.filePath(
        QStringLiteral("light3d-export-fixture.mp4"));
    const bool exportMediaReady = exportTempDir.isValid()
        && writeExportClip(exportMediaPath, &exportMediaError);
    const QImage decodedExportFrame = exportMediaReady
        ? tlrender::detail::decodeClipFrameNativeForTest(exportMediaPath, 0.0)
        : QImage();
    CompositeLayer exportLayer;
    exportLayer.name = QStringLiteral("export-layer");
    exportLayer.visible = true;
    exportLayer.opacity = 1.0;
    exportLayer.material = defaultMaterial;
    exportLayer.zOrder = 0;
    const QVector<CompositeLayer> exportLayers{exportLayer};
    const QVector<QPointF> exportOffsets{QPointF()};
    tlrender::detail::setLightingSelftestObservation(true);
    const QImage preWiring = trackmatte::composite(
        exportLayers, QVector<QImage>{decodedExportFrame}, exportCanvas);
    const QImage postWiring = exportComposite(
        exportLayers, decodedExportFrame, exportCanvas, exportOffsets, {}, viewPos);
    Light3DState disabledExport;
    disabledExport.enabled = false;
    const QImage postDisabledWiring = exportComposite(
        exportLayers, decodedExportFrame, exportCanvas, exportOffsets,
        {disabledExport}, viewPos);
    Timeline noLightTimeline;
    const ClipInfo noLightClip = makeExportClip(exportMediaPath);
    const bool noLightTimelineReady = exportMediaReady
        && setExportClip(noLightTimeline, noLightClip);
    const QImage noLightExportFrame = noLightTimelineReady
        ? tlrender::renderFrameAt(&noLightTimeline, 0, exportCanvas)
        : QImage();
    tlrender::detail::resetLightingSelftestObservation();
    const QImage observedNoLightExportFrame = noLightTimelineReady
        ? tlrender::renderFrameAt(&noLightTimeline, 0, exportCanvas)
        : QImage();
    const bool noLightExportSeamCalled =
        tlrender::detail::lightingSelftestSeamWasCalled();
    tlrender::detail::setLightingSelftestObservation(false);
    const bool g11PreEqPost = sameImage(preWiring, postWiring);
    const bool g11PreEqDisabled = sameImage(preWiring, postDisabledWiring);
    const bool g11OffEqObserved =
        sameRgbaBytes(noLightExportFrame, observedNoLightExportFrame);
    const bool g11PreEqObserved =
        sameRgbaBytes(preWiring, observedNoLightExportFrame);
    std::cerr << "light3d G11 preEqPost=" << g11PreEqPost
              << " preEqDisabled=" << g11PreEqDisabled
              << " mediaReady=" << exportMediaReady
              << " decodedNonNull=" << !decodedExportFrame.isNull()
              << " timelineReady=" << noLightTimelineReady
              << " seamCalled=" << noLightExportSeamCalled
              << " offEqObserved=" << g11OffEqObserved
              << " preEqObserved=" << g11PreEqObserved << '\n';
    tally.gate(11,
        g11PreEqPost && g11PreEqDisabled && exportMediaReady
                 && !decodedExportFrame.isNull()
                 && noLightTimelineReady
                 && noLightExportSeamCalled
                 && g11OffEqObserved && g11PreEqObserved,
        "zero-light export changed the decoded frame");

    // G17: observation is an explicit setter, not an env latch fixed by the
    // first renderFrameAt call of the process. Detects a regression to a
    // latched flag: the OFF render below (the "earlier sweep entry") must not
    // pin the observation OFF for the later ON render.
    tlrender::detail::setLightingSelftestObservation(false);
    tlrender::detail::resetLightingSelftestObservation();
    const QImage offFrame = noLightTimelineReady
        ? tlrender::renderFrameAt(&noLightTimeline, 0, exportCanvas)
        : QImage();
    const bool offSeamCalled = tlrender::detail::lightingSelftestSeamWasCalled();
    tlrender::detail::setLightingSelftestObservation(true);
    tlrender::detail::resetLightingSelftestObservation();
    const QImage onFrame = noLightTimelineReady
        ? tlrender::renderFrameAt(&noLightTimeline, 0, exportCanvas)
        : QImage();
    const bool onSeamCalled = tlrender::detail::lightingSelftestSeamWasCalled();
    tlrender::detail::setLightingSelftestObservation(false);
    std::cerr << "light3d G17 ready=" << noLightTimelineReady
              << " offSeamCalled=" << offSeamCalled
              << " onSeamCalled=" << onSeamCalled
              << " framesEqual=" << sameRgbaBytes(offFrame, onFrame) << '\n';
    tally.gate(17,
        noLightTimelineReady && !offSeamCalled && onSeamCalled
            && sameRgbaBytes(offFrame, onFrame),
        "lighting seam observation did not follow the setter after an earlier render");

    Light3DState ambientExport;
    ambientExport.type = LightType::Ambient;
    ambientExport.intensity = 2.0;
    LayerMaterial ambientExportMaterial;
    ambientExportMaterial.ambientCoeff = 1.0;
    ambientExportMaterial.diffuseCoeff = 0.0;
    ambientExportMaterial.specularCoeff = 0.0;
    CompositeLayer ambientLayer = exportLayer;
    ambientLayer.material = ambientExportMaterial;
    Timeline ambientTimeline;
    ClipInfo ambientClip = noLightClip;
    ambientClip.material = ambientExportMaterial;
    const bool ambientTimelineReady = noLightTimelineReady
        && setExportClip(ambientTimeline, ambientClip);
    ambientTimeline.setProjectLights({Light3D(ambientExport)});
    ambientTimeline.setProjectLightViewPosition(viewPos);
    const QImage previewAmbient = light3d::applyLighting(
        decodedExportFrame, {ambientExport}, ambientLayer.layer3D,
        light3d::layerCenterWorld(QSizeF(exportCanvas)),
        QSizeF(exportCanvas), viewPos, ambientExportMaterial);
    const QImage exportAmbient = ambientTimelineReady
        ? tlrender::renderFrameAt(&ambientTimeline, 0, exportCanvas)
        : QImage();
    // This deliberately uses the production renderFrameAt seam rather than
    // only the helper: the pre-wiring implementation returned the unlit
    // decoded frame here, so the gate would fail before the export hookup.
    std::cerr << "light3d G12 ambientTimelineReady=" << ambientTimelineReady
              << " previewNull=" << previewAmbient.isNull()
              << " exportNull=" << exportAmbient.isNull()
              << " previewLuma="
              << (previewAmbient.isNull() ? -1.0 : meanLuma(previewAmbient))
              << " exportLuma="
              << (exportAmbient.isNull() ? -1.0 : meanLuma(exportAmbient))
              << '\n';
    tally.gate(12,
        ambientTimelineReady
                 && !previewAmbient.isNull()
                 && !exportAmbient.isNull()
                 && closeTo(meanLuma(previewAmbient), meanLuma(exportAmbient)),
        "ambient export brightness diverged from preview brightness");

    Light3DState pointExport;
    pointExport.type = LightType::Point;
    pointExport.position = QVector3D(32.0f, 8.0f, 30.0f);
    pointExport.falloffEnabled = true;
    pointExport.falloffRadius = 0.0;
    pointExport.falloffDistance = 100.0;
    LayerMaterial pointMaterial;
    pointMaterial.ambientCoeff = 0.0;
    pointMaterial.diffuseCoeff = 1.0;
    pointMaterial.specularCoeff = 0.0;
    Timeline pointLeftTimeline;
    Timeline pointRightTimeline;
    ClipInfo pointLeftClip = noLightClip;
    pointLeftClip.material = pointMaterial;
    pointLeftClip.videoDx = 0.0;
    ClipInfo pointRightClip = pointLeftClip;
    pointRightClip.videoDx = 0.5;
    const bool pointTimelinesReady = noLightTimelineReady
        && setExportClip(pointLeftTimeline, pointLeftClip)
        && setExportClip(pointRightTimeline, pointRightClip);
    pointLeftTimeline.setProjectLights({Light3D(pointExport)});
    pointRightTimeline.setProjectLights({Light3D(pointExport)});
    pointLeftTimeline.setProjectLightViewPosition(viewPos);
    pointRightTimeline.setProjectLightViewPosition(viewPos);
    const QImage pointLeft = pointTimelinesReady
        ? tlrender::renderFrameAt(&pointLeftTimeline, 0, exportCanvas)
        : QImage();
    const QImage pointRight = pointTimelinesReady
        ? tlrender::renderFrameAt(&pointRightTimeline, 0, exportCanvas)
        : QImage();
    std::cerr << "light3d G13 pointTimelinesReady=" << pointTimelinesReady
              << " leftNull=" << pointLeft.isNull()
              << " rightNull=" << pointRight.isNull()
              << " leftLuma="
              << (pointLeft.isNull() ? -1.0 : meanLuma(pointLeft))
              << " rightLuma="
              << (pointRight.isNull() ? -1.0 : meanLuma(pointRight)) << '\n';
    tally.gate(13,
        pointTimelinesReady
                 && !pointLeft.isNull()
                 && !pointRight.isNull()
                 && std::abs(meanLuma(pointLeft) - meanLuma(pointRight)) > 0.5,
        "point-light export brightness ignored layer position");

    Light3D keyedLight;
    Light3DState keyedStart = keyedLight.state();
    keyedStart.position.setX(10.0f);
    keyedStart.intensity = 2.0;
    keyedLight.setKeyframe(0.0, keyedStart);
    Light3DState keyedEdit = keyedStart;
    keyedEdit.position.setX(20.0f);
    keyedEdit.intensity = 3.0;
    keyedLight.setStateAt(0.0, keyedEdit);
    std::cerr << "light3d G14 intensity=" << keyedLight.stateAt(0.0).intensity
              << " position.x=" << keyedLight.stateAt(0.0).position.x()
              << '\n';
    tally.gate(14,
        closeTo(keyedLight.stateAt(0.0).intensity, 3.0)
                 && closeTo(keyedLight.stateAt(0.0).position.x(), 20.0),
        "editing a keyed light did not update the current key");

    Light3D dialogLight;
    int dialogChanges = 0;
    Light3DDialog dialog;
    QObject::connect(&dialog, &Light3DDialog::settingsChanged,
                     [&dialogChanges]() { ++dialogChanges; });
    dialog.setLights({dialogLight});
    dialogChanges = 0;
    QVector<Light3D> threeLights;
    threeLights.append(dialogLight);
    threeLights.append(Light3D());
    threeLights.append(Light3D());
    dialog.setLights(threeLights);
    tally.gate(15,
        dialogChanges == 0,
        std::string("dialogChanges=") + std::to_string(dialogChanges)
            + " (rebuilding the light list must not emit settingsChanged)");

    bool selectionGate = false;
    bool opened = false;
    bool dialogFound = false;
    bool diffuseFound = false;
    int selectedClipCount = -1;
    double firstDiffuseCoeff = -1.0;
    double secondDiffuseCoeff = -1.0;
    {
        MainWindow window;
        Timeline *timeline = window.findChild<Timeline *>();
        if (timeline && !timeline->videoTracks().isEmpty()
            && timeline->videoTracks().first()) {
            TimelineTrack *track = timeline->videoTracks().first();
            ClipInfo clipA;
            clipA.filePath = QStringLiteral("light3d-selection-a.mp4");
            clipA.duration = 2.0;
            clipA.displayName = QStringLiteral("A");
            clipA.material.diffuseCoeff = 0.10;
            ClipInfo clipB = clipA;
            clipB.filePath = QStringLiteral("light3d-selection-b.mp4");
            clipB.displayName = QStringLiteral("B");
            clipB.material.diffuseCoeff = 0.20;
            track->setClips({clipA, clipB});
            track->setSelectedClip(0);
            QCoreApplication::processEvents();

            opened = QMetaObject::invokeMethod(
                &window, "openLight3DDialog", Qt::DirectConnection);
            auto *lightDialog = window.findChild<Light3DDialog *>();
            dialogFound = lightDialog != nullptr;
            track->setSelectedClip(1);
            QCoreApplication::processEvents();
            auto *diffuse = lightDialog
                ? lightDialog->findChild<QDoubleSpinBox *>(
                      QStringLiteral("light3dDiffuseCoeff"))
                : nullptr;
            diffuseFound = diffuse != nullptr;
            const bool editorReady = opened && lightDialog && diffuse;
            if (editorReady) {
                diffuse->setValue(0.75);
                QCoreApplication::processEvents();
                const auto clips = track->clips();
                selectedClipCount = static_cast<int>(clips.size());
                if (selectedClipCount > 0) firstDiffuseCoeff = clips[0].material.diffuseCoeff;
                if (selectedClipCount > 1) secondDiffuseCoeff = clips[1].material.diffuseCoeff;
                selectionGate = clips.size() == 2
                    && closeTo(clips[0].material.diffuseCoeff, 0.10)
                    && closeTo(clips[1].material.diffuseCoeff, 0.75);
                lightDialog->close();
                QCoreApplication::processEvents();
            }
        }
    }
    tally.gate(16,
        selectionGate,
        QStringLiteral("opened=%1 dialog=%2 diffuse=%3 clips.size()=%4 diffuseCoeff[0]=%5 "
                       "diffuseCoeff[1]=%6 (material edit followed the dialog-open clip instead "
                       "of the current selection)")
            .arg(opened).arg(dialogFound).arg(diffuseFound).arg(selectedClipCount)
            .arg(firstDiffuseCoeff, 0, 'g', 17)
            .arg(secondDiffuseCoeff, 0, 'g', 17)
            .toStdString());

    return tally.finish("light3d");
}
