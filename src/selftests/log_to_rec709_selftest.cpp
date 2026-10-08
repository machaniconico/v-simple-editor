#include "../EffectParamSchema.h"
#include "../EffectPreset.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoPlayer.h"
#include "../VideoEffect.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
bool sameBits(const QImage &a, const QImage &b)
{
    return !a.isNull() && a.size() == b.size() && a.format() == b.format()
        && a.sizeInBytes() == b.sizeInBytes()
        && std::memcmp(a.constBits(), b.constBits(), size_t(a.sizeInBytes())) == 0;
}
QImage gray(double code)
{
    QImage image(8, 4, QImage::Format_ARGB32);
    const int value = int(std::lround(code * 255.0));
    image.fill(qRgb(value, value, value));
    return image;
}
}

int runLogToRec709Selftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](const char *name, bool ok) {
        std::fprintf(stderr, "%s %s\n", ok ? "PASS" : "FAIL", name);
        ok ? ++passed : ++failed;
    };
    bool identity = true;
    for (auto format : {QImage::Format_RGB888, QImage::Format_ARGB32}) {
        QImage image(37, 11, format);
        for (qsizetype i = 0; i < image.sizeInBytes(); ++i)
            image.bits()[i] = uchar((i * 31 + 17) & 255);
        auto effect = VideoEffect::createLogToRec709();
        VideoEffectProcessor::setLogToRec709EnabledForTesting(true);
        identity &= sameBits(image, VideoEffectProcessor::applyEffectStack(image, {}, {}));
        effect.enabled = false;
        identity &= sameBits(image, VideoEffectProcessor::applyEffectStack(image, {}, {effect}));
        identity &= VideoEffectProcessor::logToRec709InvocationCountForTesting() == 0;
        VideoEffectProcessor::setLogToRec709EnabledForTesting(false);
        effect.enabled = true;
        identity &= sameBits(image, VideoEffectProcessor::applyEffectStack(image, {}, {effect}));
        identity &= VideoEffectProcessor::logToRec709InvocationCountForTesting() == 0;
    }
    VideoEffectProcessor::setLogToRec709EnabledForTesting(true);
    gate("G1", identity);

    const double codes[] = {0.4105, 0.3910, 0.4233, 470.0 / 1023.0};
    bool linear = true, gamma = true, oetf = true, exposure = true, monotonic = true;
    for (int camera = 0; camera < 4; ++camera) {
        const auto source = gray(codes[camera]);
        int base = 0;
        for (int output = 0; output < 3; ++output) {
            const auto effect = VideoEffect::createLogToRec709(camera, 0.0, output);
            const auto result = VideoEffectProcessor::applyEffect(source, effect);
            const QColor c = result.pixelColor(0, 0);
            const bool neutral = c.red() == c.green() && c.red() == c.blue();
            if (output == 0) gamma &= neutral && std::abs(c.red() - 125) <= 3;
            if (output == 2) oetf &= neutral && std::abs(c.red() - 104) <= 3;
            if (output == 1) {
                base = c.red();
                linear &= neutral && std::abs(base / 255.0 - 0.18) <= 0.01;
            }
            // Real preview and export entry points must each invoke the CPU kernel once.
            ClipInfo clip;
            clip.effects = {effect};
            VideoEffectProcessor::setLogToRec709EnabledForTesting(true);
            const auto preview = videopreview::prepareEchoClipForComposite(
                source, clip, 0.0, 0.0, {}, {});
            linear &= VideoEffectProcessor::logToRec709InvocationCountForTesting() == 1;
            VideoEffectProcessor::setLogToRec709EnabledForTesting(true);
            const auto exported = tlrender::applyClipFxStackFromSource(source, clip, 0.0);
            linear &= VideoEffectProcessor::logToRec709InvocationCountForTesting() == 1
                && videopreview::stackRequiresClipLocalCpu(clip.effects, true)
                && sameBits(preview.convertToFormat(QImage::Format_ARGB32), result)
                && sameBits(exported.convertToFormat(QImage::Format_ARGB32), result);
            QImage ramp(256, 1, QImage::Format_ARGB32);
            for (int x = 0; x < 256; ++x) ramp.setPixel(x, 0, qRgb(x, x, x));
            const auto converted = VideoEffectProcessor::applyEffect(ramp, effect);
            for (int x = 1; x < 256; ++x)
                monotonic &= qRed(converted.pixel(x, 0)) >= qRed(converted.pixel(x - 1, 0));
        }
        const auto raised = VideoEffectProcessor::applyEffect(
            source, VideoEffect::createLogToRec709(camera, 1.0, 1));
        exposure &= std::abs(qRed(raised.pixel(0, 0)) - 2 * base) <= 1;
    }
    gate("G2", linear);
    gate("G3", gamma);
    gate("G3b", oetf);
    gate("G4", exposure);
    gate("G5", monotonic);

    bool roundtrip = true;
    const QString names[] = {QStringLiteral("S-Log3 → Rec.709"),
        QStringLiteral("LogC3 → Rec.709"), QStringLiteral("V-Log → Rec.709")};
    for (int camera = 0; camera < 3; ++camera) {
        const auto preset = PresetLibrary::instance().findByName(names[camera]);
        const auto restored = EffectPreset::fromJson(preset.toJson());
        roundtrip &= preset.isBuiltIn && restored.name == names[camera]
            && restored.effects.size() == 1;
        if (restored.effects.size() != 1) continue;
        auto effect = restored.effects.front();
        roundtrip &= effect.type == VideoEffectType::LogToRec709
            && effect.param1 == camera && effect.param2 == 0.0 && effect.param3 == 0.0;
        effectctrl::setParamValue(effect, "input", camera + 1);
        effectctrl::setParamValue(effect, "exposure", -1.5);
        effectctrl::setParamValue(effect, "output", 2);
        const auto saved = PresetLibrary::videoEffectFromJson(PresetLibrary::videoEffectToJson(effect));
        roundtrip &= effectctrl::paramValue(saved, "input") == camera + 1
            && effectctrl::paramValue(saved, "exposure") == -1.5
            && effectctrl::paramValue(saved, "output") == 2;
    }
    gate("G6", roundtrip);

    // Independent expected basis vectors verify every matrix column, before clipping.
    const float expected[4][3][3] = {
        {{1.6269474f,-0.1785155f,-0.0444361f}, {-0.5401385f,1.4179409f,-0.1959718f}, {-0.0868088f,-0.2394254f,1.2404079f}},
        {{1.617523f,-0.070573f,-0.021102f}, {-0.537287f,1.334613f,-0.226954f}, {-0.080237f,-0.264040f,1.248056f}},
        {{1.806576f,-0.170090f,-0.025206f}, {-0.695697f,1.305955f,-0.154468f}, {-0.110879f,-0.135865f,1.179674f}},
        {{1,0,0}, {0,1,0}, {0,0,1}}
    };
    bool gamut = true;
    for (int camera = 0; camera < 4; ++camera) {
        for (int basis = 0; basis < 3; ++basis) {
            float r = basis == 0 ? 1.0f : 0.0f;
            float g = basis == 1 ? 1.0f : 0.0f;
            float b = basis == 2 ? 1.0f : 0.0f;
            VideoEffectProcessor::logToRec709Gamut(camera, r, g, b);
            gamut &= std::abs(r - expected[camera][basis][0]) <= 1e-4f
                && std::abs(g - expected[camera][basis][1]) <= 1e-4f
                && std::abs(b - expected[camera][basis][2]) <= 1e-4f;
        }
        float r = 1.0f, g = 1.0f, b = 1.0f;
        VideoEffectProcessor::logToRec709Gamut(camera, r, g, b);
        // AWG3's supplied first row sums to exactly 0.999999 in decimal.
        // Allow one float epsilon for its representation and dot-product rounding.
        const float rowTolerance = 1e-6f + std::numeric_limits<float>::epsilon();
        gamut &= std::abs(r - 1.0f) <= rowTolerance && std::abs(g - 1.0f) <= rowTolerance
            && std::abs(b - 1.0f) <= rowTolerance;
    }
    gate("G7", gamut);
    VideoEffectProcessor::setLogToRec709EnabledForTesting(true);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
