#include "../ClipGeometry.h"
#include "../EffectParamSchema.h"
#include "../EffectPreset.h"
#include "../MaskSystem.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoEffect.h"
#include "../VideoPlayer.h"

#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace {

bool imagesByteIdentical(const QImage &a, const QImage &b)
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

QImage makePattern(int width, int height)
{
    QImage image(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y,
                                QColor((x * 31 + y * 7) & 255,
                                       (x * 11 + y * 29) & 255,
                                       (x * 19 + y * 13) & 255,
                                       255));
        }
    }
    return image;
}

double redVariance(const QImage &image)
{
    if (image.isNull())
        return 0.0;
    const int sampleCount = image.width() * image.height();
    double sum = 0.0;
    double sumSquares = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double value = image.pixelColor(x, y).red();
            sum += value;
            sumSquares += value * value;
        }
    }
    const double mean = sum / sampleCount;
    return sumSquares / sampleCount - mean * mean;
}

double meanAbsoluteError(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return std::numeric_limits<double>::infinity();

    const QImage lhs = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage rhs = b.convertToFormat(QImage::Format_RGBA8888);
    quint64 totalError = 0;
    for (int y = 0; y < lhs.height(); ++y) {
        const uchar *left = lhs.constScanLine(y);
        const uchar *right = rhs.constScanLine(y);
        for (int x = 0; x < lhs.width() * 4; ++x)
            totalError += static_cast<quint64>(std::abs(int(left[x]) - int(right[x])));
    }
    return static_cast<double>(totalError)
        / static_cast<double>(lhs.width() * lhs.height() * 4);
}

QImage sourceOver(const QImage &lower, const QImage &upper)
{
    QImage out = lower.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&out);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(0, 0, upper);
    painter.end();
    return out;
}

bool schemaNamesEqual(VideoEffectType type, const QStringList &expected)
{
    const QVector<effectctrl::ParamDef> schema = effectctrl::paramSchemaFor(type);
    if (schema.size() != expected.size())
        return false;
    for (int i = 0; i < schema.size(); ++i) {
        if (schema[i].name != expected[i])
            return false;
    }
    return true;
}

bool containsType(VideoEffectType type)
{
    const QVector<VideoEffectType> types = VideoEffect::allTypes();
    return std::find(types.cbegin(), types.cend(), type) != types.cend();
}

void reportGate(const char *gate, const char *description, bool ok,
                int &passed, int &failed)
{
    std::cerr << (ok ? "PASS " : "FAIL ") << gate << ' ' << description << '\n';
    if (ok)
        ++passed;
    else
        ++failed;
}

} // namespace

int runFxGrainEchoSelftest()
{
    int passed = 0;
    int failed = 0;
    const QImage pattern = makePattern(48, 32);

    const VideoEffect grain = VideoEffect::createFilmGrain(0.45, 2, 0.3, true);
    const QImage deterministicA = VideoEffectProcessor::applyEffect(pattern, grain);
    const QImage deterministicB = VideoEffectProcessor::applyEffect(pattern, grain);
    reportGate("G1", "FilmGrain deterministic output",
               imagesByteIdentical(deterministicA, deterministicB), passed, failed);

    const QImage grainOff = VideoEffectProcessor::applyEffect(
        pattern, VideoEffect::createFilmGrain(0.0, 4, 1.0, true));
    reportGate("G2", "FilmGrain amount=0 byte identity",
               imagesByteIdentical(pattern, grainOff), passed, failed);

    QImage flat(64, 64, QImage::Format_RGBA8888);
    flat.fill(QColor(128, 128, 128, 255));
    const double lowVariance = redVariance(VideoEffectProcessor::applyEffect(
        flat, VideoEffect::createFilmGrain(0.2, 1, 0.0, false)));
    const double mediumVariance = redVariance(VideoEffectProcessor::applyEffect(
        flat, VideoEffect::createFilmGrain(0.5, 1, 0.0, false)));
    const double highVariance = redVariance(VideoEffectProcessor::applyEffect(
        flat, VideoEffect::createFilmGrain(0.8, 1, 0.0, false)));
    reportGate("G3", "FilmGrain variance increases with amount",
               lowVariance < mediumVariance && mediumVariance < highVariance,
               passed, failed);

    const bool metadataPresent =
        containsType(VideoEffectType::FilmGrain)
        && containsType(VideoEffectType::Echo)
        && VideoEffect::typeName(VideoEffectType::FilmGrain) != QStringLiteral("Unknown")
        && VideoEffect::typeName(VideoEffectType::Echo) != QStringLiteral("Unknown")
        && schemaNamesEqual(VideoEffectType::FilmGrain,
                            { "amount", "size", "colorAmount", "seedPerFrame" })
        && schemaNamesEqual(VideoEffectType::Echo,
                            { "delaySec", "count", "decay", "blend" });
    reportGate("G4", "typeName/allTypes/schema contain FilmGrain and Echo",
               metadataPresent, passed, failed);

    const QImage echoNoOp = VideoEffectProcessor::applyEffect(
        pattern, VideoEffect::createEcho());
    reportGate("G5", "Echo single-frame applyEffect is a no-op",
               imagesByteIdentical(pattern, echoNoOp), passed, failed);

    const QImage emptyCompose = tlrender::composeEcho(
        pattern, QVector<QImage>(), 0.5, 2);
    reportGate("G6", "composeEcho count=0 byte identity",
               imagesByteIdentical(pattern, emptyCompose), passed, failed);

    QImage brighter(pattern.size(), QImage::Format_RGBA8888);
    brighter.fill(QColor(240, 220, 200, 255));
    const QImage zeroDecay = tlrender::composeEcho(
        pattern, QVector<QImage>{brighter}, 0.0, 2);
    reportGate("G7", "composeEcho decay=0 byte identity",
               imagesByteIdentical(pattern, zeroDecay), passed, failed);

    const QImage lightened = tlrender::composeEcho(
        pattern, QVector<QImage>{brighter}, 0.75, 2);
    bool lightenNeverDarkens = lightened.size() == pattern.size();
    for (int y = 0; lightenNeverDarkens && y < pattern.height(); ++y) {
        for (int x = 0; x < pattern.width(); ++x) {
            const QColor basePixel = pattern.pixelColor(x, y);
            const QColor outPixel = lightened.pixelColor(x, y);
            if (outPixel.red() < basePixel.red()
                || outPixel.green() < basePixel.green()
                || outPixel.blue() < basePixel.blue()) {
                lightenNeverDarkens = false;
                break;
            }
        }
    }
    reportGate("G8", "Lighten output channels are not below base",
               lightenNeverDarkens, passed, failed);

    EffectPreset preset;
    preset.name = QStringLiteral("fxgrain-echo-roundtrip");
    preset.effects = {
        VideoEffect::createFilmGrain(),
        VideoEffect::createEcho()
    };
    QJsonObject presetJson = preset.toJson();
    QJsonArray serializedEffects = presetJson.value("effects").toArray();
    const bool serializedTypeNames = serializedEffects.size() == 2
        && serializedEffects.at(0).toObject().value("type").toString()
            == QStringLiteral("FilmGrain")
        && serializedEffects.at(1).toObject().value("type").toString()
            == QStringLiteral("Echo");
    for (int i = 0; i < serializedEffects.size(); ++i) {
        QJsonObject effectJson = serializedEffects.at(i).toObject();
        effectJson.remove("typeId");
        effectJson.remove("typeName");
        serializedEffects[i] = effectJson;
    }
    presetJson["effects"] = serializedEffects;
    const EffectPreset restoredPreset = EffectPreset::fromJson(presetJson);
    const bool presetStringRoundTrip = serializedTypeNames
        && restoredPreset.effects.size() == 2
        && restoredPreset.effects.at(0).type == VideoEffectType::FilmGrain
        && restoredPreset.effects.at(1).type == VideoEffectType::Echo;
    reportGate("G9", "EffectPreset string round-trip preserves FilmGrain and Echo",
               presetStringRoundTrip, passed, failed);

    const QVector<effectctrl::ParamDef> echoSchema =
        effectctrl::paramSchemaFor(VideoEffectType::Echo);
    const auto blendDef = std::find_if(
        echoSchema.cbegin(), echoSchema.cend(),
        [](const effectctrl::ParamDef &def) { return def.name == "blend"; });
    bool blendModesValid = blendDef != echoSchema.cend()
        && blendDef->type == effectctrl::ParamType::Int
        && blendDef->minVal == 0.0
        && blendDef->maxVal == 3.0
        && blendDef->defaultVal == 2.0;

    QImage blendBase(1, 1, QImage::Format_RGBA8888);
    blendBase.fill(QColor(40, 80, 120, 255));
    QImage blendEcho(1, 1, QImage::Format_RGBA8888);
    blendEcho.fill(QColor(200, 100, 60, 255));
    const QVector<QColor> expectedBlendPixels = {
        QColor(240, 180, 180, 255),
        QColor(209, 149, 152, 255),
        QColor(200, 100, 120, 255),
        QColor(200, 100, 60, 255)
    };
    for (int blend = 0; blend <= 3 && blendModesValid; ++blend) {
        VideoEffect blendEffect = VideoEffect::createEcho(0.1, 1, 1.0, blend);
        effectctrl::setParamValue(blendEffect, QStringLiteral("blend"), blend);
        ClipInfo blendClip;
        blendClip.inPoint = 0.0;
        blendClip.outPoint = 2.0;
        blendClip.duration = 2.0;
        blendClip.effects = { blendEffect };
        const tlrender::EchoFrameProvider provider =
            [&blendEcho](double, double) { return blendEcho; };
        const QImage composed = tlrender::applyClipFxPackWithEcho(
            blendBase, blendClip, 1.0, 1.0, provider);
        blendModesValid = effectctrl::paramValue(
            blendEffect, QStringLiteral("blend")) == blend
            && composed.pixelColor(0, 0) == expectedBlendPixels.at(blend);
    }
    reportGate("G10", "Echo blend values 0..3 match schema and composition",
               blendModesValid, passed, failed);

    const auto delayDef = std::find_if(
        echoSchema.cbegin(), echoSchema.cend(),
        [](const effectctrl::ParamDef &def) { return def.name == "delaySec"; });
    ClipInfo adjustmentClip;
    adjustmentClip.isAdjustment = true;
    adjustmentClip.inPoint = 0.0;
    adjustmentClip.outPoint = 2.0;
    adjustmentClip.duration = 2.0;
    adjustmentClip.effects = { VideoEffect::createEcho(0.1, 1, 1.0, 2) };
    const tlrender::EchoFrameProvider brightProvider =
        [&brighter](double, double) { return brighter; };
    const QImage adjustmentResult = tlrender::applyClipFxPackWithEcho(
        pattern, adjustmentClip, 1.0, 1.0, brightProvider);
    const bool adjustmentEchoNoOp = imagesByteIdentical(pattern, adjustmentResult)
        && delayDef != echoSchema.cend()
        && delayDef->displayLabel.contains(
            QStringLiteral("調整レイヤーでは無効"));
    reportGate("G11", "adjustment-layer Echo is documented and remains a no-op",
               adjustmentEchoNoOp, passed, failed);

    // G12 uses the same clip-local Echo helper called by export and VideoPlayer,
    // then follows their real mask/transform/composite primitives. The upper
    // track has all three operations so a canvas-final Echo regression cannot
    // hide behind a single-layer identity case.
    const QSize paritySize(64, 48);
    QImage lowerTrack(paritySize, QImage::Format_RGBA8888);
    lowerTrack.fill(QColor(12, 40, 96, 255));
    QImage upperTrack(paritySize, QImage::Format_RGBA8888);
    upperTrack.fill(QColor(92, 24, 18, 255));
    QImage priorUpper(paritySize, QImage::Format_RGBA8888);
    priorUpper.fill(QColor(28, 184, 64, 255));

    ClipInfo upperClip;
    upperClip.inPoint = 0.0;
    upperClip.outPoint = 2.0;
    upperClip.duration = 2.0;
    upperClip.colorCorrection.brightness = 18.0;
    upperClip.colorCorrection.contrast = 12.0;
    upperClip.effects = { VideoEffect::createEcho(0.1, 1, 0.65, 2) };
    upperClip.videoScale = 1.0;
    upperClip.videoDx = 0.125;
    upperClip.videoDy = -0.125;
    Mask upperMask;
    upperMask.shape = MaskShape::Rectangle;
    upperMask.rect = QRectF(8.0, 6.0, 40.0, 30.0);
    upperClip.maskSystem.addMask(upperMask);

    int exportProviderCalls = 0;
    int previewProviderCalls = 0;
    const QImage exportFx = tlrender::applyClipFxStackWithEchoFromSource(
        upperTrack, upperClip, 1.0, 1.0,
        [&priorUpper, &upperClip, &exportProviderCalls](double,
                                                        double sampleLocalSec) {
            ++exportProviderCalls;
            return tlrender::prepareClipSourceForEcho(
                priorUpper, upperClip, sampleLocalSec);
        });
    const QVector<Mask> masks = upperClip.maskSystem.masks();
    const QImage previewMasked = videopreview::prepareEchoClipForComposite(
        upperTrack, upperClip, 1.0, 1.0,
        [&priorUpper, &upperClip, &previewProviderCalls](double,
                                                         double sampleLocalSec) {
            ++previewProviderCalls;
            return tlrender::prepareClipSourceForEcho(
                priorUpper, upperClip, sampleLocalSec);
        },
        masks);

    const QImage exportMasked = MaskSystem::applyMask(
        exportFx, MaskSystem::generateMaskImage(masks, exportFx.size()));
    const clipgeom::ClipTransform upperTransform{
        upperClip.videoScale, upperClip.videoDx, upperClip.videoDy,
        upperClip.rotation2DDegrees};
    const QImage exportPlaced = clipgeom::renderLayer(
        exportMasked, upperTransform, paritySize, /*smooth=*/true);
    const QImage previewPlaced = clipgeom::renderLayer(
        previewMasked, upperTransform, paritySize, /*smooth=*/false);
    const QImage exportComposite = sourceOver(lowerTrack, exportPlaced);
    const QImage previewComposite = sourceOver(lowerTrack, previewPlaced);
    const double parityMae = meanAbsoluteError(exportComposite, previewComposite);
    const bool gradeWasApplied = !imagesByteIdentical(
        upperTrack,
        tlrender::prepareClipSourceForEcho(upperTrack, upperClip, 1.0));

    ClipInfo reverseClip;
    reverseClip.inPoint = 2.0;
    reverseClip.outPoint = 6.0;
    reverseClip.duration = 6.0;
    reverseClip.reversed = true;
    reverseClip.effects = { VideoEffect::createEcho(0.5, 1, 1.0, 3) };
    double reverseSampleSource = -1.0;
    double reverseSampleLocal = -1.0;
    (void)tlrender::applyClipFxStackWithEchoFromSource(
        upperTrack, reverseClip, 1.0,
        reverseClip.sourceSecondAtLocalTime(1.0),
        [&priorUpper, &reverseSampleSource, &reverseSampleLocal](
            double sourceSec, double localSec) {
            reverseSampleSource = sourceSec;
            reverseSampleLocal = localSec;
            return priorUpper;
        });
    const bool reverseMappedToEarlierTimelineFrame =
        std::abs(reverseSampleLocal - 0.5) < 1e-9
        && std::abs(reverseSampleSource - 5.5) < 1e-9;

    ClipInfo animatedPrefixClip;
    animatedPrefixClip.inPoint = 0.0;
    animatedPrefixClip.outPoint = 2.0;
    animatedPrefixClip.duration = 2.0;
    animatedPrefixClip.effects = {
        VideoEffect::createBrightnessContrast(0.0, 0.0),
        VideoEffect::createEcho(0.5, 1, 1.0, 3)
    };
    KeyframeTrack brightnessTrack(
        QStringLiteral("effect.0.brightness"), 0.0);
    brightnessTrack.addKeyframe(0.0, 0.0);
    brightnessTrack.addKeyframe(1.0, 100.0);
    animatedPrefixClip.keyframes.addTrack(brightnessTrack);
    const QImage animatedPrefixResult =
        tlrender::applyClipFxStackWithEchoFromSource(
            upperTrack, animatedPrefixClip, 1.0, 1.0,
            [&priorUpper](double, double) { return priorUpper; });
    const QImage expectedAnimatedPrefix =
        VideoEffectProcessor::applyEffect(
            priorUpper,
            VideoEffect::createBrightnessContrast(50.0, 0.0))
            .convertToFormat(QImage::Format_RGBA8888);
    const bool prefixKeyframeUsesHistoryTime = imagesByteIdentical(
        animatedPrefixResult, expectedAnimatedPrefix);

    reportGate("G12", "two-track mask+transform+Echo preview/export MAE < 1.0",
               exportProviderCalls == 1 && previewProviderCalls == 1
                   && gradeWasApplied && parityMae < 1.0
                   && reverseMappedToEarlierTimelineFrame
                   && prefixKeyframeUsesHistoryTime,
               passed, failed);

    // G13 exercises VideoPlayer's production CPU-routing seam, not the Echo
    // helper alone. V1 has only FilmGrain while the sibling V2 has Echo: the
    // complete V1 stack must be baked on V1 exactly once and must not be
    // reapplied to the final two-track canvas.
    QImage grainTrack = makePattern(paritySize.width(), paritySize.height());
    QImage echoTrack(paritySize, QImage::Format_RGBA8888);
    QImage echoHistory(paritySize, QImage::Format_RGBA8888);
    for (int y = 0; y < paritySize.height(); ++y) {
        for (int x = 0; x < paritySize.width(); ++x) {
            echoTrack.setPixelColor(
                x, y, QColor(18 + (x * 2) % 90, 24 + (y * 3) % 110,
                             42 + (x + y) % 100, 255));
            echoHistory.setPixelColor(
                x, y, QColor(110 + (x * 3) % 120, 80 + (y * 5) % 150,
                             70 + (x + y * 2) % 150, 255));
        }
    }

    ClipInfo grainClip;
    grainClip.inPoint = 0.0;
    grainClip.outPoint = 2.0;
    grainClip.duration = 2.0;
    grainClip.opacity = 0.58;
    grainClip.effects = {
        VideoEffect::createFilmGrain(0.55, 1, 0.25, true)
    };
    ClipInfo echoClip;
    echoClip.inPoint = 0.0;
    echoClip.outPoint = 2.0;
    echoClip.duration = 2.0;
    echoClip.effects = {
        VideoEffect::createEcho(0.1, 2, 0.65, 2)
    };

    const auto grainProvider = [](double, double) { return QImage(); };
    const auto echoProvider =
        [&echoHistory, &echoClip](double, double sampleLocalSec) {
            return tlrender::prepareClipSourceForEcho(
                echoHistory, echoClip, sampleLocalSec);
        };
    const QImage playerCpuPreview = VideoPlayer::composeCpuPreviewForTest(
        grainTrack, grainClip, grainProvider,
        echoTrack, echoClip, echoProvider,
        1.0, 1.0, paritySize);

    const QImage rendererV1 = tlrender::applyClipFxStackFromSource(
        grainTrack, grainClip, 1.0);
    const QImage rendererV2 = tlrender::applyClipFxStackWithEchoFromSource(
        echoTrack, echoClip, 1.0, 1.0, echoProvider);
    QImage rendererOutput(
        paritySize, QImage::Format_ARGB32_Premultiplied);
    rendererOutput.fill(Qt::black);
    {
        QPainter painter(&rendererOutput);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setOpacity(1.0);
        painter.drawImage(0, 0, rendererV2);
        painter.setOpacity(grainClip.opacity);
        painter.drawImage(0, 0, rendererV1);
    }
    rendererOutput = rendererOutput.convertToFormat(QImage::Format_RGBA8888);

    const QImage canvasLeak = VideoEffectProcessor::applyEffect(
        rendererOutput, grainClip.effects.constFirst());
    const double playerRendererMae =
        meanAbsoluteError(playerCpuPreview, rendererOutput);
    const double leakedCanvasMae =
        meanAbsoluteError(canvasLeak, rendererOutput);
    const bool cpuRoutingRecognized =
        videopreview::stackRequiresClipLocalCpu(grainClip.effects)
        && videopreview::stackRequiresClipLocalCpu(echoClip.effects)
        && !videopreview::stackRequiresClipLocalCpu(
            QVector<VideoEffect>{VideoEffect::createInvert()});
    reportGate(
        "G13",
        "V1 FilmGrain + V2 Echo VideoPlayer CPU preview/renderer MAE < 1.0",
        cpuRoutingRecognized && playerRendererMae < 1.0
            && leakedCanvasMae >= 1.0,
        passed, failed);

    // G14 checks the stored premultiplied contribution. With equal alpha=128
    // on base/history and decay=0.5, Lighten keeps alpha=128 and the history
    // RGB contribution is multiplied by 0.5 exactly once.
    QImage translucentBase(1, 1, QImage::Format_RGBA8888);
    translucentBase.fill(QColor(40, 50, 60, 128));
    QImage translucentEcho(1, 1, QImage::Format_RGBA8888);
    translucentEcho.fill(QColor(200, 150, 100, 128));
    const QImage translucentLighten = tlrender::composeEcho(
        translucentBase, QVector<QImage>{translucentEcho}, 0.5, 2);
    const QImage basePremultiplied = translucentBase.convertToFormat(
        QImage::Format_ARGB32_Premultiplied);
    const QImage echoPremultiplied = translucentEcho.convertToFormat(
        QImage::Format_ARGB32_Premultiplied);
    const QImage outputPremultiplied = translucentLighten.convertToFormat(
        QImage::Format_ARGB32_Premultiplied);
    const QRgb basePixel =
        reinterpret_cast<const QRgb *>(basePremultiplied.constScanLine(0))[0];
    const QRgb echoPixel =
        reinterpret_cast<const QRgb *>(echoPremultiplied.constScanLine(0))[0];
    const QRgb outputPixel =
        reinterpret_cast<const QRgb *>(outputPremultiplied.constScanLine(0))[0];
    const auto expectedLightenChannel = [](int baseChannel, int echoChannel) {
        return qMax(baseChannel, qRound(echoChannel * 0.5));
    };
    const int oldDoubleDecayedRed = qRound(
        (40.0 + (200.0 - 40.0) * (128.0 / 255.0) * 0.5)
        * (128.0 / 255.0));
    const bool translucentLightenValid =
        qAlpha(outputPixel) == qAlpha(basePixel)
        && std::abs(qRed(outputPixel)
                    - expectedLightenChannel(qRed(basePixel), qRed(echoPixel))) <= 1
        && std::abs(qGreen(outputPixel)
                    - expectedLightenChannel(qGreen(basePixel), qGreen(echoPixel))) <= 1
        && std::abs(qBlue(outputPixel)
                    - expectedLightenChannel(qBlue(basePixel), qBlue(echoPixel))) <= 1
        && qRed(outputPixel) > oldDoubleDecayedRed;
    reportGate(
        "G14",
        "translucent Lighten preserves alpha and applies decay once",
        translucentLightenValid, passed, failed);

    std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    return failed;
}
