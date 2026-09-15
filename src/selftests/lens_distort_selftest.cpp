#include "../EffectParamSchema.h"
#include "../EffectPreset.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoEffect.h"
#include "../VideoPlayer.h"

#include <QJsonDocument>
#include <QStringList>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
bool identical(const QImage &a, const QImage &b)
{
    return !a.isNull() && a.size() == b.size() && a.format() == b.format()
        && a.sizeInBytes() == b.sizeInBytes()
        && std::memcmp(a.constBits(), b.constBits(), size_t(a.sizeInBytes())) == 0;
}

int blackPixels(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y) == QColor(0, 0, 0, 255)) ++count;
    return count;
}

int blackEdgeWidth(const QImage &image, bool fromRight)
{
    int width = 0;
    while (width < image.width()) {
        const int x = fromRight ? image.width() - 1 - width : width;
        if (image.pixelColor(x, image.height() / 2) != QColor(0, 0, 0, 255)) break;
        ++width;
    }
    return width;
}
} // namespace

int runLensDistortSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };

    // G1: actual branch-disable switch, original bytes and zero invocation count.
    bool identity = true;
    const VideoEffect neutral = VideoEffect::createLensDistortion();
    for (const auto format : {QImage::Format_RGB888, QImage::Format_RGBA8888}) {
        QImage input(37, 23, format);
        for (qsizetype i = 0; i < input.sizeInBytes(); ++i)
            input.bits()[i] = static_cast<uchar>((i * 37 + 19) & 255);
        VideoEffectProcessor::resetLensDistortionInvocationCount();
        VideoEffectProcessor::setLensDistortionEnabledForTesting(false);
        const QImage bypass = VideoEffectProcessor::applyEffect(input, neutral);
        const QImage forcedBypass = VideoEffectProcessor::applyEffect(
            input, VideoEffect::createLensDistortion(0.3));
        VideoEffectProcessor::setLensDistortionEnabledForTesting(true);
        const QImage actual = VideoEffectProcessor::applyEffect(input, neutral);
        VideoEffect disabled = VideoEffect::createLensDistortion(0.3);
        disabled.enabled = false;
        identity = identity && identical(input, bypass) && identical(input, forcedBypass)
            && identical(input, actual)
            && identical(input, VideoEffectProcessor::applyEffect(input, disabled))
            && identical(input, VideoEffectProcessor::applyEffectStack(input, ColorCorrection(), {}))
            && VideoEffectProcessor::lensDistortionInvocationCount() == 0;
    }
    gate(1, identity);

    QImage white(400, 400, QImage::Format_RGB888);
    white.fill(Qt::white);
    const VideoEffect positive = VideoEffect::createLensDistortion(0.3);
    const QImage pincushion = VideoEffectProcessor::applyEffect(white, positive);
    const QImage barrel = VideoEffectProcessor::applyEffect(
        white, VideoEffect::createLensDistortion(-0.3));

    // Exercise the production preview/export seams, without constructing Timeline or QWidget.
    ClipInfo clip;
    clip.effects = {positive};
    VideoEffectProcessor::resetLensDistortionInvocationCount();
    const QImage preview = videopreview::prepareEchoClipForComposite(
        white, clip, 0.0, 0.0, {}, {});
    const int previewCalls = VideoEffectProcessor::lensDistortionInvocationCount();
    VideoEffectProcessor::resetLensDistortionInvocationCount();
    const QImage exported = tlrender::applyClipFxStackFromSource(white, clip, 0.0);
    const bool routes = previewCalls == 1
        && VideoEffectProcessor::lensDistortionInvocationCount() == 1
        && identical(preview.convertToFormat(QImage::Format_RGBA8888),
                     exported.convertToFormat(QImage::Format_RGBA8888))
        && videopreview::stackRequiresClipLocalCpu(clip.effects, true);
    gate(2, blackPixels(pincushion) > blackPixels(white)
        && barrel.pixelColor(0, 0) == Qt::white
        && barrel.pixelColor(399, 0) == Qt::white
        && barrel.pixelColor(0, 399) == Qt::white
        && barrel.pixelColor(399, 399) == Qt::white && routes);

    const QImage fill12 = VideoEffectProcessor::applyEffect(
        white, VideoEffect::createLensDistortion(0.3, 0.0, 1.2));
    const QImage fill16 = VideoEffectProcessor::applyEffect(
        white, VideoEffect::createLensDistortion(0.3, 0.0, 1.6));
    // The literal US-304 G3 conflicts with its mapping equation. Keep the gate
    // honest: at r^2 ~= 2, scale 1.2 cannot cancel the factor ~= 1.6.
    std::fprintf(stderr, "G3 black pixels: scale=1: %d, scale=1.2: %d, scale=1.6: %d\n",
                 blackPixels(pincushion), blackPixels(fill12), blackPixels(fill16));
    gate(3, blackPixels(fill12) == 0);

    const QImage offset = VideoEffectProcessor::applyEffect(
        white, VideoEffect::createLensDistortion(0.3, 0.0, 1.0, 0.2, 0.0));
    gate(4, blackEdgeWidth(offset, true) < blackEdgeWidth(offset, false));

    // Grid crossing at input r=0.5: source x=299.5, center x=y=199.5, R=200.
    // Solve q*(1+0.2*q^2)=0.5 analytically by bisection, then locate the
    // corresponding output line by its subpixel intensity centroid.
    QImage grid(400, 400, QImage::Format_RGB888);
    grid.fill(Qt::black);
    for (int y = 0; y < 400; ++y) {
        uchar *row = grid.scanLine(y);
        for (int x = 0; x < 400; ++x) {
            row[x * 3] = (x % 100 == 99 || x % 100 == 0) ? 255 : 0;
            row[x * 3 + 1] = (y % 100 == 99 || y % 100 == 0) ? 255 : 0;
        }
    }
    const QImage warped = VideoEffectProcessor::applyEffect(
        grid, VideoEffect::createLensDistortion(0.2));
    double lo = 0.0, hi = 0.5;
    for (int i = 0; i < 60; ++i) {
        const double q = (lo + hi) * 0.5;
        if (q * (1.0 + 0.2 * q * q) < 0.5) lo = q; else hi = q;
    }
    const double expectedX = 199.5 + 200.0 * (lo + hi) * 0.5;
    double weighted = 0.0, weight = 0.0;
    for (int x = int(std::floor(expectedX)) - 3; x <= int(std::ceil(expectedX)) + 3; ++x) {
        const double v = warped.pixelColor(x, 199).red();
        weight += v;
        weighted += x * v;
    }
    gate(5, weight > 0.0 && std::abs(weighted / weight - expectedX) <= 0.5
        && warped.pixelColor(int(std::round(expectedX)), 199).green() > 200);

    bool roundTrip = true;
    const QStringList names{QStringLiteral("GoPro 広角"), QStringLiteral("DJI"),
                            QStringLiteral("一眼 24mm")};
    const double expectedK1[] = {-0.30, -0.18, -0.08};
    const double expectedK2[] = {0.08, 0.04, 0.01};
    for (int i = 0; i < names.size(); ++i) {
        const EffectPreset preset = PresetLibrary::instance().findByName(names[i]);
        const auto decoded = EffectPreset::fromJson(
            QJsonDocument::fromJson(QJsonDocument(preset.toJson()).toJson()).object());
        if (preset.effects.size() != 1 || decoded.effects.size() != 1) {
            roundTrip = false;
            continue;
        }
        const auto &e = decoded.effects.front();
        roundTrip = roundTrip && decoded.name == names[i] && decoded.isBuiltIn
            && e.type == VideoEffectType::LensDistortion && e.enabled
            && e.param1 == expectedK1[i] && e.param2 == expectedK2[i] && e.param3 == 1.0
            && effectctrl::paramValue(e, "centerX") == 0.0
            && effectctrl::paramValue(e, "centerY") == 0.0;
        VideoEffect shifted = e;
        effectctrl::setParamValue(shifted, "centerX", 0.2);
        effectctrl::setParamValue(shifted, "centerY", -0.375);
        const VideoEffect restored = PresetLibrary::videoEffectFromJson(
            QJsonDocument::fromJson(QJsonDocument(PresetLibrary::videoEffectToJson(shifted))
                                      .toJson()).object());
        roundTrip = roundTrip && effectctrl::paramValue(restored, "centerX") == 0.2
            && effectctrl::paramValue(restored, "centerY") == -0.375;
    }
    VideoEffect bounds = neutral;
    effectctrl::setParamValue(bounds, "k1", 5.0);
    effectctrl::setParamValue(bounds, "k2", -5.0);
    effectctrl::setParamValue(bounds, "scale", std::numeric_limits<double>::quiet_NaN());
    effectctrl::setParamValue(bounds, "centerX", 5.0);
    effectctrl::setParamValue(bounds, "centerY", -5.0);
    gate(6, roundTrip && bounds.param1 == 0.5 && bounds.param2 == -0.5 && bounds.param3 == 1.0
        && effectctrl::paramValue(bounds, "centerX") == 0.5
        && effectctrl::paramValue(bounds, "centerY") == -0.5);

    VideoEffectProcessor::setLensDistortionEnabledForTesting(true);
    VideoEffectProcessor::resetLensDistortionInvocationCount();
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
