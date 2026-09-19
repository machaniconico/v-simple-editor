#include "../VideoEffect.h"
#include "../EffectParamSchema.h"
#include "../EffectPreset.h"
#include "../VideoPlayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"

#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
bool sameBits(const QImage &a, const QImage &b)
{
    return !a.isNull() && a.size() == b.size() && a.format() == b.format()
        && a.sizeInBytes() == b.sizeInBytes()
        && std::memcmp(a.constBits(), b.constBits(), a.sizeInBytes()) == 0;
}
double luma(QRgb p)
{
    return 0.2126 * qRed(p) + 0.7152 * qGreen(p) + 0.0722 * qBlue(p);
}
}

int runSafeLeaveColorSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    const auto safe = VideoEffect::createBroadcastSafe();
    const auto leave = VideoEffect::createLeaveColor();
    QImage patches(12, 4, QImage::Format_ARGB32);
    for (int y = 0; y < patches.height(); ++y)
        for (int x = 0; x < patches.width(); ++x)
            patches.setPixel(x, y, x < 4 ? qRgba(255, 0, 0, 73)
                : x < 8 ? qRgba(0, 255, 0, 129) : qRgba(0, 0, 255, 211));

    bool bypass = safe.enabled && leave.enabled;
    VideoEffectProcessor::setSafeLeaveColorEnabledForTesting(false);
    for (auto format : {QImage::Format_RGB888, QImage::Format_ARGB32}) {
        const auto input = patches.convertToFormat(format);
        for (const auto &effect : {safe, leave})
            bypass &= sameBits(input, VideoEffectProcessor::applyEffectStack(input, {}, {effect}));
        bypass &= sameBits(input, VideoEffectProcessor::applyEffectStack(input, {}, {safe, leave}));
    }
    bypass &= VideoEffectProcessor::safeLeaveColorInvocationCountForTesting() == 0;
    VideoEffectProcessor::setSafeLeaveColorEnabledForTesting(true);
    for (const auto &effect : {safe, leave})
        bypass &= !sameBits(patches, VideoEffectProcessor::applyEffect(patches, effect));
    bypass &= VideoEffectProcessor::safeLeaveColorInvocationCountForTesting() == 2;
    gate(1, bypass);

    QImage ramp(256, 2, QImage::Format_ARGB32);
    for (int y = 0; y < ramp.height(); ++y)
        for (int x = 0; x < ramp.width(); ++x) ramp.setPixel(x, y, qRgba(x, x, x, 91));
    const auto limited = VideoEffectProcessor::applyEffect(ramp, safe);
    bool legal = true;
    for (int x = 0; x < 256; ++x) {
        const auto p = limited.pixel(x, 0);
        legal &= std::abs(luma(p) - std::clamp(x, 16, 235)) < 1e-9 && qAlpha(p) == 91;
        if (x >= 16 && x <= 235) legal &= p == ramp.pixel(x, 0);
    }
    legal &= sameBits(ramp, VideoEffectProcessor::applyEffect(ramp, VideoEffect::createBroadcastSafe(1)));
    gate(2, legal);

    const auto chromaLimited = VideoEffectProcessor::applyEffect(patches, safe);
    bool chroma = true;
    for (int x = 0; x < patches.width(); ++x) {
        const auto p = chromaLimited.pixel(x, 0);
        const int cb = int(std::lround(128.0 + (qBlue(p) - luma(p)) / 1.8556));
        const int cr = int(std::lround(128.0 + (qRed(p) - luma(p)) / 1.5748));
        chroma &= cb >= 16 && cb <= 240 && cr >= 16 && cr <= 240
            && qAlpha(p) == qAlpha(patches.pixel(x, 0));
    }
    const auto neutral = VideoEffectProcessor::applyEffect(patches, VideoEffect::createBroadcastSafe(0, 0.0));
    for (int x = 0; x < patches.width(); ++x)
        chroma &= qRed(neutral.pixel(x, 0)) == qGreen(neutral.pixel(x, 0))
            && qGreen(neutral.pixel(x, 0)) == qBlue(neutral.pixel(x, 0));
    gate(3, chroma && chromaLimited.pixel(0, 0) != patches.pixel(0, 0));

    bool colors = true;
    for (double amount : {1.0, 0.5}) {
        const auto output = VideoEffectProcessor::applyEffect(patches,
            VideoEffect::createLeaveColor(Qt::red, 0.15, amount));
        for (int x = 0; x < patches.width(); ++x) {
            const auto p = output.pixel(x, 0);
            colors &= qAlpha(p) == qAlpha(patches.pixel(x, 0));
            if (x < 4) colors &= p == patches.pixel(x, 0);
            else colors &= std::abs(QColor::fromRgba(p).hsvSaturationF() - (1.0 - amount)) < 0.005;
        }
    }
    // Circular hue distance and inclusive 180-degree tolerance.
    QImage wrap(4, 1, QImage::Format_ARGB32);
    wrap.fill(QColor::fromHsv(359, 255, 255));
    colors &= sameBits(wrap, VideoEffectProcessor::applyEffect(wrap, leave));
    colors &= sameBits(patches, VideoEffectProcessor::applyEffect(patches,
        VideoEffect::createLeaveColor(Qt::red, 1.0)));
    gate(4, colors);

    EffectPreset preset;
    preset.effects = {VideoEffect::createBroadcastSafe(0, 0.37),
        VideoEffect::createLeaveColor(QColor(31, 157, 219), 0.27, 0.63)};
    const auto restored = EffectPreset::fromJson(QJsonDocument::fromJson(
        QJsonDocument(preset.toJson()).toJson(QJsonDocument::Compact)).object());
    bool roundTrip = restored.effects.size() == 2;
    for (int i = 0; roundTrip && i < 2; ++i) {
        const auto &a = preset.effects[i];
        const auto &b = restored.effects[i];
        roundTrip &= a.type == b.type && a.keyColor == b.keyColor
            && a.param1 == b.param1 && a.param2 == b.param2 && a.param3 == b.param3;
        roundTrip &= sameBits(VideoEffectProcessor::applyEffect(patches, a),
                              VideoEffectProcessor::applyEffect(patches, b));
    }
    for (const auto &factory : {safe, leave}) {
        VideoEffect fromSchema;
        fromSchema.type = factory.type;
        for (const auto &def : effectctrl::paramSchemaFor(factory.type)) {
            effectctrl::setParamValue(fromSchema, def.name, def.defaultVal);
            roundTrip &= effectctrl::paramValue(fromSchema, def.name) == def.defaultVal;
        }
        roundTrip &= fromSchema.param1 == factory.param1 && fromSchema.param2 == factory.param2
            && fromSchema.keyColor == factory.keyColor;
        for (const auto &def : effectctrl::paramSchemaFor(factory.type)) {
            if (def.type == effectctrl::ParamType::Color) {
                effectctrl::setColorParam(fromSchema, def.name, QColor(31, 157, 219));
                roundTrip &= effectctrl::colorParamValue(fromSchema, def.name) == QColor(31, 157, 219);
            } else {
                effectctrl::setParamValue(fromSchema, def.name, def.maxVal);
                roundTrip &= effectctrl::paramValue(fromSchema, def.name) == def.maxVal;
            }
        }
    }
    gate(5, roundTrip);

    bool parity = true;
    for (auto format : {QImage::Format_RGB888, QImage::Format_ARGB32}) {
        const auto input = patches.convertToFormat(format);
        for (const QVector<VideoEffect> &stack : {QVector<VideoEffect>{safe},
                 QVector<VideoEffect>{leave}, QVector<VideoEffect>{safe, leave}}) {
            ClipInfo clip;
            clip.effects = stack;
            parity &= videopreview::stackRequiresClipLocalCpu(stack, true);
            VideoEffectProcessor::setSafeLeaveColorEnabledForTesting(true);
            const auto exported = tlrender::applyClipFxStackFromSource(input, clip, 0.0);
            const auto preview = videopreview::prepareEchoClipForComposite(input, clip, 0.0, 0.0, {}, {});
            parity &= sameBits(exported, preview)
                && VideoEffectProcessor::safeLeaveColorInvocationCountForTesting() == 2 * stack.size();
        }
    }
    gate(6, parity);
    VideoEffectProcessor::setSafeLeaveColorEnabledForTesting(true);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
