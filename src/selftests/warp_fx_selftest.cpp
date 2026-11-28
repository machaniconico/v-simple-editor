#include "../VideoEffect.h"
#include "../EffectParamSchema.h"
#include "../EffectPreset.h"
#include "../VideoPlayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
const std::array<VideoEffectType, 5> kTypes = {
    VideoEffectType::WarpWave, VideoEffectType::WarpRipple,
    VideoEffectType::WarpSpherize, VideoEffectType::WarpFisheye,
    VideoEffectType::WarpPinch
};

VideoEffect makeEffect(VideoEffectType type)
{
    VideoEffect effect;
    effect.type = type;
    for (const auto &def : effectctrl::paramSchemaFor(type))
        effectctrl::setParamValue(effect, def.name, def.defaultVal);
    return effect;
}

bool sameBits(const QImage &a, const QImage &b)
{
    return !a.isNull() && a.size() == b.size() && a.format() == b.format()
        && a.sizeInBytes() == b.sizeInBytes()
        && std::memcmp(a.constBits(), b.constBits(), size_t(a.sizeInBytes())) == 0;
}

QImage coordinateImage()
{
    QImage image(200, 200, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixel(x, y, qRgba(x, y, (x + y) % 256, 128 + (x % 128)));
    return image;
}

int brightestX(const QImage &image, int y, int first, int last)
{
    int best = first;
    for (int x = first + 1; x <= last; ++x)
        if (qRed(image.pixel(x, y)) > qRed(image.pixel(best, y))) best = x;
    return best;
}
}

int runWarpFxSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    const QImage input = coordinateImage();
    VideoEffectProcessor::setWarpEnabledForTesting(true);
    bool identity = true;
    for (auto type : kTypes) {
        auto effect = makeEffect(type);
        effect.param1 = 0.0;
        for (auto format : {QImage::Format_ARGB32, QImage::Format_RGB888,
                            QImage::Format_ARGB32_Premultiplied}) {
            const QImage source = input.convertToFormat(format);
            identity &= sameBits(source, VideoEffectProcessor::applyEffectStack(
                source, ColorCorrection(), {effect}));
        }
    }
    identity &= VideoEffectProcessor::warpInvocationCountForTesting() == 0;
    identity &= sameBits(input, VideoEffectProcessor::applyEffectStack(input, ColorCorrection(), {}));
    VideoEffectProcessor::setWarpEnabledForTesting(false);
    for (auto type : kTypes)
        identity &= sameBits(input, VideoEffectProcessor::applyEffect(input, makeEffect(type)));
    identity &= VideoEffectProcessor::warpInvocationCountForTesting() == 0;
    VideoEffectProcessor::setWarpEnabledForTesting(true);
    gate(1, identity);

    QImage line(200, 200, QImage::Format_ARGB32);
    line.fill(Qt::black);
    for (int y = 0; y < 200; ++y) line.setPixel(100, y, qRgb(255, 255, 255));
    auto wave = makeEffect(VideoEffectType::WarpWave);
    wave.param1 = 12.0;
    wave.param2 = 1.0;
    const QImage waved = VideoEffectProcessor::applyEffect(line, wave);
    bool waveOk = brightestX(waved, 0, 80, 120) == 100
        && brightestX(waved, 50, 80, 120) == 112
        && brightestX(waved, 150, 80, 120) == 88;
    wave.param3 = 0.25;
    waveOk &= brightestX(VideoEffectProcessor::applyEffect(line, wave), 0, 80, 120) == 112;
    gate(2, waveOk);

    auto ripple = makeEffect(VideoEffectType::WarpRipple);
    ripple.param3 = 0.25; // 50 pixels, frequency 2 cycles / 200 pixels.
    const QImage rippled = VideoEffectProcessor::applyEffect(input, ripple);
    bool rippleOk = true;
    for (const QPoint point : {QPoint(125, 100), QPoint(100, 125),
                               QPoint(75, 100), QPoint(100, 75)})
        rippleOk &= rippled.pixel(point) != input.pixel(point);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x)
            if ((x - 100) * (x - 100) + (y - 100) * (y - 100) >= 2500)
                rippleOk &= rippled.pixel(x, y) == input.pixel(x, y);
    gate(3, rippleOk);

    // Follow a bright feature initially at half-radius. This measures visible
    // displacement, whose sign is opposite to the inverse source lookup.
    QImage ring(200, 200, QImage::Format_ARGB32);
    ring.fill(Qt::black);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x)
            if (std::abs(std::hypot(x - 100.0, y - 100.0) - 50.0) < 1.0)
                ring.setPixel(x, y, qRgb(255, 255, 255));
    ring.setPixel(100, 100, qRgb(40, 70, 90));
    bool radialOk = true;
    for (auto type : {VideoEffectType::WarpSpherize, VideoEffectType::WarpFisheye,
                      VideoEffectType::WarpPinch}) {
        const QImage result = VideoEffectProcessor::applyEffect(ring, makeEffect(type));
        radialOk &= result.pixel(100, 100) == ring.pixel(100, 100);
        const int peak = brightestX(result, 100, 110, 195);
        radialOk &= qRed(result.pixel(peak, 100)) > 100;
        radialOk &= type == VideoEffectType::WarpPinch ? peak < 150 : peak > 150;
    }
    auto negativeSphere = makeEffect(VideoEffectType::WarpSpherize);
    negativeSphere.param1 = -0.5;
    radialOk &= brightestX(VideoEffectProcessor::applyEffect(ring, negativeSphere), 100, 110, 195) < 150;
    gate(4, radialOk);

    bool deterministic = true;
    for (auto type : kTypes) {
        const auto effect = makeEffect(type);
        deterministic &= sameBits(VideoEffectProcessor::applyEffect(input, effect),
                                  VideoEffectProcessor::applyEffect(input, effect));
    }
    gate(5, deterministic);

    EffectPreset preset;
    preset.name = QStringLiteral("ワープ検証");
    for (auto type : kTypes) {
        auto effect = makeEffect(type);
        // Exercise each named control and all persisted parameter slots.
        for (const auto &def : effectctrl::paramSchemaFor(type))
            effectctrl::setParamValue(effect, def.name, def.minVal + (def.maxVal - def.minVal) * 0.37);
        preset.effects.append(effect);
    }
    bool presetOk = true;
    for (bool namesOnly : {false, true}) {
        QJsonObject json = preset.toJson();
        if (namesOnly) {
            QJsonArray effects = json[QStringLiteral("effects")].toArray();
            for (int i = 0; i < effects.size(); ++i) {
                auto object = effects[i].toObject();
                object.remove(QStringLiteral("typeId"));
                effects[i] = object;
            }
            json[QStringLiteral("effects")] = effects;
        }
        const auto restored = EffectPreset::fromJson(json);
        presetOk &= restored.effects.size() == preset.effects.size();
        if (restored.effects.size() != preset.effects.size()) continue;
        for (int i = 0; i < preset.effects.size(); ++i) {
            const auto &before = preset.effects[i], &after = restored.effects[i];
            presetOk &= before.type == after.type && before.enabled == after.enabled
                && before.param1 == after.param1 && before.param2 == after.param2
                && before.param3 == after.param3;
            for (const auto &def : effectctrl::paramSchemaFor(before.type))
                presetOk &= std::abs(effectctrl::paramValue(after, def.name)
                    - (def.minVal + (def.maxVal - def.minVal) * 0.37)) < 1e-9;
        }
    }
    gate(6, presetOk);

    bool cpuOk = true;
    for (auto type : kTypes) {
        const auto effect = makeEffect(type);
        cpuOk &= videopreview::stackRequiresClipLocalCpu({effect}, true);
        ClipInfo clip;
        clip.effects = {effect};
        VideoEffectProcessor::setWarpEnabledForTesting(true);
        const QImage direct = VideoEffectProcessor::applyEffectStack(input, ColorCorrection(), {effect});
        // Shared production clip stage used by VideoPlayer and tlrender export.
        const QImage shared = tlrender::applyClipFxStackFromSource(input, clip, 0.0);
        const QImage preview = videopreview::prepareEchoClipForComposite(
            input, clip, 0.0, 0.0, {}, {});
        cpuOk &= VideoEffectProcessor::warpInvocationCountForTesting() == 3;
        cpuOk &= sameBits(direct.convertToFormat(QImage::Format_ARGB32),
                          shared.convertToFormat(QImage::Format_ARGB32));
        cpuOk &= sameBits(shared, preview);
    }
    gate(7, cpuOk);
    VideoEffectProcessor::setWarpEnabledForTesting(true);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
