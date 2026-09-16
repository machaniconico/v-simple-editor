#include "../VideoEffect.h"
#include "../VideoPlayer.h"
#include "../EffectPreset.h"
#include "../EffectParamSchema.h"

#include <QJsonDocument>
#include <QPainter>
#include <cstdio>
#include <cstring>

namespace {
bool sameBits(const QImage &a, const QImage &b)
{
    return a.size() == b.size() && a.format() == b.format()
        && a.sizeInBytes() == b.sizeInBytes()
        && std::memcmp(a.constBits(), b.constBits(), a.sizeInBytes()) == 0;
}
}

int runKeyersSelftest()
{
    int passed = 0, failed = 0;
    const auto check = [&](int gate, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", gate);
        ok ? ++passed : ++failed;
    };
    QImage gradient(256, 4, QImage::Format_ARGB32);
    for (int y = 0; y < gradient.height(); ++y)
        for (int x = 0; x < gradient.width(); ++x)
            gradient.setPixel(x, y, qRgb(x, x, x));
    const auto luma = VideoEffect::createLumaKey(0.2, 0.4, 0.1);
    const auto color = VideoEffect::createColorKey();
    VideoEffectProcessor::setKeyersEnabledForTesting(true);
    bool unchanged = sameBits(gradient, VideoEffectProcessor::applyEffectStack(gradient, {}, {}));
    for (auto effect : {luma, color}) {
        effect.enabled = false;
        unchanged &= sameBits(gradient, VideoEffectProcessor::applyEffectStack(gradient, {}, {effect}));
    }
    unchanged &= VideoEffectProcessor::keyersInvocationCountForTesting() == 0;
    VideoEffectProcessor::setKeyersEnabledForTesting(false);
    for (const auto &effect : {luma, color})
        unchanged &= sameBits(gradient, VideoEffectProcessor::applyEffectStack(gradient, {}, {effect}));
    unchanged &= VideoEffectProcessor::keyersInvocationCountForTesting() == 0;
    VideoEffectProcessor::setKeyersEnabledForTesting(true);
    check(1, unchanged);

    const QImage keyed = VideoEffectProcessor::applyEffectStack(gradient, {}, {luma});
    bool bands = keyed.format() == QImage::Format_ARGB32;
    for (int x = 0; x < 256; ++x) {
        const int alpha = qAlpha(keyed.pixel(x, 0));
        if (x >= 52 && x <= 101) bands &= alpha == 0;
        if (x <= 25 || x >= 128) bands &= alpha == 255;
        if (x >= 27 && x <= 50)
            bands &= alpha > 0 && alpha < qAlpha(keyed.pixel(x - 1, 0));
        if (x >= 104 && x <= 127)
            bands &= alpha < 255 && alpha > qAlpha(keyed.pixel(x - 1, 0));
    }
    auto hardLuma = luma;
    hardLuma.param3 = 0.0;
    const auto hard = VideoEffectProcessor::applyEffect(gradient, hardLuma);
    bands &= qAlpha(hard.pixel(60, 0)) == 0 && qAlpha(hard.pixel(40, 0)) == 255;
    check(2, bands && VideoEffectProcessor::keyersInvocationCountForTesting() == 2);

    QImage regions(90, 8, QImage::Format_ARGB32);
    for (int y = 0; y < regions.height(); ++y)
        for (int x = 0; x < regions.width(); ++x)
            regions.setPixel(x, y, x < 30 ? qRgb(0, 255, 0)
                : (x < 60 ? qRgb(55, 255, 0) : qRgb(255, 0, 0)));
    const QImage colorKeyed = VideoEffectProcessor::applyEffectStack(regions, {}, {color});
    bool colors = qAlpha(colorKeyed.pixel(10, 0)) == 0
        && qAlpha(colorKeyed.pixel(40, 0)) > 0 && qAlpha(colorKeyed.pixel(40, 0)) < 255
        && qAlpha(colorKeyed.pixel(70, 0)) == 255;
    auto hardColor = color;
    hardColor.param1 = 0.0;
    hardColor.param2 = 0.0;
    const auto hardColors = VideoEffectProcessor::applyEffect(regions, hardColor);
    colors &= qAlpha(hardColors.pixel(10, 0)) == 0 && qAlpha(hardColors.pixel(40, 0)) == 255;
    check(3, colors);

    bool composited = true;
    for (const auto &foreground : {keyed, colorKeyed}) {
        QImage canvas(foreground.size(), QImage::Format_ARGB32);
        canvas.fill(qRgb(24, 48, 220));
        QPainter painter(&canvas);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.drawImage(0, 0, foreground);
        painter.end();
        for (int x = 0; x < foreground.width(); ++x) {
            const auto pixel = foreground.pixel(x, 0);
            if (qAlpha(pixel) == 0) composited &= canvas.pixel(x, 0) == qRgb(24, 48, 220);
            if (qAlpha(pixel) == 255) composited &= canvas.pixel(x, 0) == pixel;
        }
    }
    check(4, composited);

    EffectPreset preset;
    auto customColor = color;
    effectctrl::setColorParam(customColor, QStringLiteral("color"), QColor(12, 190, 42));
    effectctrl::setParamValue(customColor, QStringLiteral("tolerance"), 0.17);
    effectctrl::setParamValue(customColor, QStringLiteral("softness"), 0.09);
    auto customLuma = luma;
    effectctrl::setParamValue(customLuma, QStringLiteral("lower"), 0.12);
    effectctrl::setParamValue(customLuma, QStringLiteral("upper"), 0.52);
    effectctrl::setParamValue(customLuma, QStringLiteral("softness"), 0.08);
    preset.effects = {customLuma, customColor};
    const auto restored = EffectPreset::fromJson(QJsonDocument::fromJson(
        QJsonDocument(preset.toJson()).toJson(QJsonDocument::Compact)).object());
    bool roundTrip = restored.effects.size() == 2;
    for (int i = 0; roundTrip && i < 2; ++i) {
        const auto &before = preset.effects[i];
        const auto &after = restored.effects[i];
        roundTrip &= before.type == after.type && before.keyColor == after.keyColor
            && before.param1 == after.param1 && before.param2 == after.param2
            && before.param3 == after.param3;
        for (const auto &param : effectctrl::paramSchemaFor(before.type))
            roundTrip &= effectctrl::paramValue(before, param.name) == effectctrl::paramValue(after, param.name);
        roundTrip &= sameBits(VideoEffectProcessor::applyEffect(gradient, before),
                              VideoEffectProcessor::applyEffect(gradient, after));
    }
    roundTrip &= customLuma.param1 == 0.12 && customLuma.param2 == 0.52 && customLuma.param3 == 0.08
        && customColor.param1 == 0.17 && customColor.param2 == 0.09
        && effectctrl::colorParamValue(customColor, QStringLiteral("color")) == QColor(12, 190, 42);
    // The library constructs defaults through the schema and these setters.
    for (const auto &defaults : {VideoEffect::createLumaKey(), VideoEffect::createColorKey()}) {
        VideoEffect fromSchema;
        fromSchema.type = defaults.type;
        for (const auto &param : effectctrl::paramSchemaFor(defaults.type))
            effectctrl::setParamValue(fromSchema, param.name, param.defaultVal);
        roundTrip &= fromSchema.param1 == defaults.param1 && fromSchema.param2 == defaults.param2
            && fromSchema.param3 == defaults.param3 && fromSchema.keyColor == defaults.keyColor;
    }
    check(5, roundTrip);

    const auto blur = VideoEffect::createBlur(2.0);
    const auto afterBlur = VideoEffectProcessor::applyEffectStack(gradient, {}, {luma, blur});
    const auto afterKey = VideoEffectProcessor::applyEffectStack(gradient, {}, {blur, luma});
    bool opaque = true;
    for (int x = 0; x < afterBlur.width(); ++x) opaque &= qAlpha(afterBlur.pixel(x, 0)) == 255;
    check(6, opaque && qAlpha(afterKey.pixel(76, 0)) == 0);

    // Both playback's clip-local CPU processing and tlrender's export pack
    // call applyEffectStack, exercised above; GPU routing must select it.
    check(7, videopreview::stackRequiresClipLocalCpu({luma}, true)
        && videopreview::stackRequiresClipLocalCpu({color}, true));
    VideoEffectProcessor::setKeyersEnabledForTesting(true);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
