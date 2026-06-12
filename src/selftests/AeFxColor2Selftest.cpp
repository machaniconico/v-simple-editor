#include "../EffectParamSchema.h"
#include "../Timeline.h"
#include "../VideoEffect.h"
#include "../clipanim/ClipAnim.h"

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct ExpectedParam {
    const char *name;
    effectctrl::ParamType type;
    double minVal;
    double maxVal;
    double defaultVal;
};

void printGate(const char *gate, bool ok, const char *reason, int &passed, int &failed)
{
    if (ok) {
        std::printf("PASS: %s\n", gate);
        ++passed;
        return;
    }
    std::printf("FAIL: %s %s\n", gate, reason);
    ++failed;
}

QImage makeGradientImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int r = (x * 255) / qMax(1, width - 1);
            const int g = (y * 255) / qMax(1, height - 1);
            const int b = ((x + y) * 255) / qMax(1, width + height - 2);
            image.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    return image;
}

int maxArgbDiff(QRgb a, QRgb b)
{
    return std::max({ std::abs(qAlpha(a) - qAlpha(b)),
                      std::abs(qRed(a) - qRed(b)),
                      std::abs(qGreen(a) - qGreen(b)),
                      std::abs(qBlue(a) - qBlue(b)) });
}

bool allPixelsWithin(const QImage &a, const QImage &b, int tolerance)
{
    if (a.size() != b.size())
        return false;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (maxArgbDiff(a.pixel(x, y), b.pixel(x, y)) > tolerance)
                return false;
        }
    }
    return true;
}

bool allPixelsExact(const QImage &a, const QImage &b)
{
    return allPixelsWithin(a, b, 0);
}

int lumaAt(const QImage &image, int x, int y)
{
    const QColor c = image.pixelColor(x, y);
    return static_cast<int>(std::round(0.2126 * c.red()
                                     + 0.7152 * c.green()
                                     + 0.0722 * c.blue()));
}

int saturationSpan(const QColor &c)
{
    return std::max({ c.red(), c.green(), c.blue() })
         - std::min({ c.red(), c.green(), c.blue() });
}

int lumaSpread(const QImage &image)
{
    int minLuma = 255;
    int maxLuma = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int luma = lumaAt(image, x, y);
            minLuma = std::min(minLuma, luma);
            maxLuma = std::max(maxLuma, luma);
        }
    }
    return maxLuma - minLuma;
}

bool allPixelsBrighter(const QImage &before, const QImage &after)
{
    if (before.size() != after.size())
        return false;
    for (int y = 0; y < before.height(); ++y) {
        for (int x = 0; x < before.width(); ++x) {
            if (lumaAt(after, x, y) <= lumaAt(before, x, y))
                return false;
        }
    }
    return true;
}

bool closeEnough(double a, double b)
{
    return std::abs(a - b) <= 1e-9;
}

double encodedRgb(const QColor &color)
{
    return color.isValid() ? static_cast<double>(color.rgb()) : 0.0;
}

bool allTypesContain(VideoEffectType type)
{
    const auto types = VideoEffect::allTypes();
    return std::find(types.begin(), types.end(), type) != types.end();
}

bool schemaMatches(VideoEffectType type, const QVector<ExpectedParam> &expected)
{
    const auto schema = effectctrl::paramSchemaFor(type);
    if (schema.size() != expected.size())
        return false;
    for (int i = 0; i < schema.size(); ++i) {
        const auto &actual = schema[i];
        const auto &want = expected[i];
        if (actual.name != QString::fromLatin1(want.name)
            || actual.type != want.type
            || !closeEnough(actual.minVal, want.minVal)
            || !closeEnough(actual.maxVal, want.maxVal)
            || !closeEnough(actual.defaultVal, want.defaultVal)) {
            return false;
        }
    }
    return true;
}

VideoEffect effectFromGuiAddPath(VideoEffectType type)
{
    VideoEffect effect;
    effect.type = type;
    const auto schema = effectctrl::paramSchemaFor(type);
    for (const auto &def : schema) {
        effectctrl::setParamValue(effect, def.name, def.defaultVal);
    }
    return effect;
}

bool keyColorRoundTrips(VideoEffectType type, const QColor &color)
{
    VideoEffect effect;
    effect.type = type;
    effectctrl::setColorParam(effect, "keyColor", color);
    effectctrl::setParamValue(effect, "keyColor", encodedRgb(color));
    return effect.keyColor == color
        && effectctrl::colorParamValue(effect, "keyColor") == color
        && closeEnough(effectctrl::paramValue(effect, "keyColor"), encodedRgb(color));
}

KeyframeTrack twoPointTrack(const QString &name,
                            double firstValue,
                            double secondValue)
{
    KeyframeTrack track(name, firstValue);
    track.addKeyframe(0.0, firstValue);
    track.addKeyframe(1.0, secondValue);
    return track;
}

ClipInfo clipWithEffect(const VideoEffect &effect)
{
    ClipInfo clip;
    clip.duration = 1.0;
    clip.effects.append(effect);
    return clip;
}

bool sameEffectState(const VideoEffect &a, const VideoEffect &b)
{
    return a.type == b.type
        && a.enabled == b.enabled
        && closeEnough(a.param1, b.param1)
        && closeEnough(a.param2, b.param2)
        && closeEnough(a.param3, b.param3)
        && a.keyColor == b.keyColor
        && closeEnough(a.startSec, b.startSec)
        && closeEnough(a.endSec, b.endSec);
}

bool noTrackEffectsStayIdentical()
{
    const VideoEffect original = VideoEffect::createPhotoFilter(QColor(12, 34, 56), 40.0);
    const ClipInfo clip = clipWithEffect(original);
    const QVector<VideoEffect> evaluated = clipanim::effectiveEffectsAt(clip, 0.5);
    return evaluated.size() == 1 && sameEffectState(evaluated.first(), original);
}

bool keyColorChannelsInterpolate()
{
    ClipInfo clip = clipWithEffect(
        VideoEffect::createPhotoFilter(QColor(10, 20, 30), 40.0));
    clip.keyframes.addTrack(twoPointTrack(QStringLiteral("effect.0.keyColor.r"),
                                          10.0, 110.0));
    clip.keyframes.addTrack(twoPointTrack(QStringLiteral("effect.0.keyColor.g"),
                                          20.0, 120.0));
    clip.keyframes.addTrack(twoPointTrack(QStringLiteral("effect.0.keyColor.b"),
                                          30.0, 130.0));

    const QVector<VideoEffect> evaluated = clipanim::effectiveEffectsAt(clip, 0.5);
    return evaluated.size() == 1
        && effectctrl::colorParamValue(evaluated.first(), "keyColor") == QColor(60, 70, 80)
        && closeEnough(effectctrl::paramValue(evaluated.first(), "density"), 40.0);
}

bool legacyPackedKeyColorTrackStaysInactive()
{
    const VideoEffect original = VideoEffect::createPhotoFilter(QColor(12, 34, 56), 40.0);
    ClipInfo clip = clipWithEffect(original);
    KeyframeTrack legacy(QStringLiteral("effect.0.keyColor"), 0.0);
    legacy.addKeyframe(0.0, 0.0);
    legacy.addKeyframe(1.0, 0.0);
    clip.keyframes.addTrack(legacy);

    const QVector<VideoEffect> evaluated = clipanim::effectiveEffectsAt(clip, 0.5);
    return evaluated.size() == 1 && sameEffectState(evaluated.first(), original);
}

} // namespace

int runAeFxColor2Selftest()
{
    int passed = 0;
    int failed = 0;

    {
        const QImage gradient = makeGradientImage(16, 8);
        const QVector<VideoEffect> effects = {
            VideoEffect::createCurves(),
            VideoEffect::createChannelMixer(),
            VideoEffect::createVibrance(),
            VideoEffect::createPhotoFilter(),
            VideoEffect::createTritone(),
            VideoEffect::createBrightnessContrast()
        };

        bool identities = true;
        for (const auto &effect : effects) {
            identities = identities
                && allPixelsWithin(gradient, VideoEffectProcessor::applyEffect(gradient, effect), 0);
        }

        printGate("G1", identities,
                  "identity params changed the gradient",
                  passed, failed);
    }

    {
        QImage image(3, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(32, 32, 32));
        image.setPixelColor(1, 0, QColor(96, 96, 96));
        image.setPixelColor(2, 0, QColor(160, 160, 160));

        const QImage bright = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createBrightnessContrast(50.0, 0.0));
        const QImage contrast = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createBrightnessContrast(0.0, 50.0));

        printGate("G2",
                  allPixelsBrighter(image, bright)
                      && lumaSpread(contrast) > lumaSpread(image),
                  "BrightnessContrast brightness or contrast behavior failed",
                  passed, failed);
    }

    {
        QImage image(2, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(120, 130, 140));
        image.setPixelColor(1, 0, QColor(255, 0, 0));

        const QImage vibrant = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createVibrance(100.0));
        const int lowSatGain = saturationSpan(vibrant.pixelColor(0, 0))
            - saturationSpan(image.pixelColor(0, 0));
        const int highSatGain = saturationSpan(vibrant.pixelColor(1, 0))
            - saturationSpan(image.pixelColor(1, 0));

        printGate("G3",
                  lowSatGain > 0 && lowSatGain > highSatGain,
                  "Vibrance did not favor the low-saturation pixel",
                  passed, failed);
    }

    {
        QImage image(1, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(120, 120, 120));
        const QColor warm(236, 138, 0);

        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createPhotoFilter(warm, 0.0));
        const QImage filtered = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createPhotoFilter(warm, 50.0));
        const QColor before = image.pixelColor(0, 0);
        const QColor after = filtered.pixelColor(0, 0);

        printGate("G4",
                  allPixelsExact(image, identity)
                      && (after.red() - after.blue()) > (before.red() - before.blue())
                      && after.red() > before.red(),
                  "PhotoFilter density=0 identity or warm bias failed",
                  passed, failed);
    }

    {
        QImage image(1, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(8, 8, 8));

        const QImage lifted = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createCurves(50.0, 0.0, 0.0));

        printGate("G5",
                  lumaAt(lifted, 0, 0) > lumaAt(image, 0, 0),
                  "Curves shadows=+50 did not lift near-black",
                  passed, failed);
    }

    {
        const QImage pattern = makeGradientImage(6, 6);
        VideoEffect none;
        none.type = VideoEffectType::None;
        const QImage noneOut = VideoEffectProcessor::applyEffect(pattern, none);

        VideoEffect curves = VideoEffect::createCurves();
        effectctrl::setParamValue(curves, "shadows", 12.0);
        effectctrl::setParamValue(curves, "highlights", -8.0);
        effectctrl::setParamValue(curves, "midContrast", 25.0);

        VideoEffect mixer = VideoEffect::createChannelMixer();
        effectctrl::setParamValue(mixer, "redFromRed", 120.0);
        effectctrl::setParamValue(mixer, "greenFromGreen", 80.0);
        effectctrl::setParamValue(mixer, "blueFromBlue", 150.0);

        VideoEffect vibrance = VideoEffect::createVibrance();
        effectctrl::setParamValue(vibrance, "vibrance", 35.0);

        VideoEffect photoFilter = VideoEffect::createPhotoFilter();
        effectctrl::setParamValue(photoFilter, "density", 45.0);
        effectctrl::setColorParam(photoFilter, "keyColor", QColor(220, 120, 20));

        VideoEffect tritone = VideoEffect::createTritone();
        effectctrl::setParamValue(tritone, "blend", 0.6);
        effectctrl::setColorParam(tritone, "keyColor", QColor(10, 20, 90));

        VideoEffect brightnessContrast = VideoEffect::createBrightnessContrast();
        effectctrl::setParamValue(brightnessContrast, "brightness", -15.0);
        effectctrl::setParamValue(brightnessContrast, "contrast", 40.0);

        printGate("G6",
                  allPixelsExact(pattern, noneOut)
                      && schemaMatches(VideoEffectType::Curves, QVector<ExpectedParam>{
                          { "shadows", effectctrl::ParamType::Float, -100.0, 100.0, 0.0 },
                          { "highlights", effectctrl::ParamType::Float, -100.0, 100.0, 0.0 },
                          { "midContrast", effectctrl::ParamType::Float, -100.0, 100.0, 0.0 } })
                      && schemaMatches(VideoEffectType::ChannelMixer, QVector<ExpectedParam>{
                          { "redFromRed", effectctrl::ParamType::Float, 0.0, 200.0, 100.0 },
                          { "greenFromGreen", effectctrl::ParamType::Float, 0.0, 200.0, 100.0 },
                          { "blueFromBlue", effectctrl::ParamType::Float, 0.0, 200.0, 100.0 } })
                      && schemaMatches(VideoEffectType::Vibrance, QVector<ExpectedParam>{
                          { "vibrance", effectctrl::ParamType::Float, -100.0, 100.0, 0.0 } })
                      && schemaMatches(VideoEffectType::PhotoFilter, QVector<ExpectedParam>{
                          { "density", effectctrl::ParamType::Float, 0.0, 100.0, 0.0 },
                          { "keyColor", effectctrl::ParamType::Color, 0.0, 0.0, encodedRgb(QColor(236, 138, 0)) } })
                      && schemaMatches(VideoEffectType::Tritone, QVector<ExpectedParam>{
                          { "blend", effectctrl::ParamType::Float, 0.0, 1.0, 0.0 },
                          { "keyColor", effectctrl::ParamType::Color, 0.0, 0.0, encodedRgb(QColor(0, 0, 0)) } })
                      && schemaMatches(VideoEffectType::BrightnessContrast, QVector<ExpectedParam>{
                          { "brightness", effectctrl::ParamType::Float, -100.0, 100.0, 0.0 },
                          { "contrast", effectctrl::ParamType::Float, -100.0, 100.0, 0.0 } })
                      && allTypesContain(VideoEffectType::Curves)
                      && allTypesContain(VideoEffectType::ChannelMixer)
                      && allTypesContain(VideoEffectType::Vibrance)
                      && allTypesContain(VideoEffectType::PhotoFilter)
                      && allTypesContain(VideoEffectType::Tritone)
                      && allTypesContain(VideoEffectType::BrightnessContrast)
                      && VideoEffect::typeName(VideoEffectType::Curves) == QStringLiteral("カーブ")
                      && VideoEffect::typeName(VideoEffectType::ChannelMixer) == QStringLiteral("チャンネルミキサー")
                      && VideoEffect::typeName(VideoEffectType::Vibrance) == QStringLiteral("自然な彩度")
                      && VideoEffect::typeName(VideoEffectType::PhotoFilter) == QStringLiteral("フォトフィルター")
                      && VideoEffect::typeName(VideoEffectType::Tritone) == QStringLiteral("トライトーン")
                      && VideoEffect::typeName(VideoEffectType::BrightnessContrast) == QStringLiteral("明るさ・コントラスト")
                      && closeEnough(effectctrl::paramValue(curves, "shadows"), 12.0)
                      && closeEnough(effectctrl::paramValue(curves, "highlights"), -8.0)
                      && closeEnough(effectctrl::paramValue(curves, "midContrast"), 25.0)
                      && closeEnough(curves.param1, 12.0)
                      && closeEnough(curves.param2, -8.0)
                      && closeEnough(curves.param3, 25.0)
                      && closeEnough(effectctrl::paramValue(mixer, "redFromRed"), 120.0)
                      && closeEnough(effectctrl::paramValue(mixer, "greenFromGreen"), 80.0)
                      && closeEnough(effectctrl::paramValue(mixer, "blueFromBlue"), 150.0)
                      && closeEnough(mixer.param1, 120.0)
                      && closeEnough(mixer.param2, 80.0)
                      && closeEnough(mixer.param3, 150.0)
                      && closeEnough(effectctrl::paramValue(vibrance, "vibrance"), 35.0)
                      && closeEnough(vibrance.param1, 35.0)
                      && closeEnough(effectctrl::paramValue(photoFilter, "density"), 45.0)
                      && effectctrl::colorParamValue(photoFilter, "keyColor") == QColor(220, 120, 20)
                      && closeEnough(photoFilter.param1, 45.0)
                      && photoFilter.keyColor == QColor(220, 120, 20)
                      && closeEnough(effectctrl::paramValue(tritone, "blend"), 0.6)
                      && effectctrl::colorParamValue(tritone, "keyColor") == QColor(10, 20, 90)
                      && closeEnough(tritone.param1, 0.6)
                      && tritone.keyColor == QColor(10, 20, 90)
                      && closeEnough(effectctrl::paramValue(brightnessContrast, "brightness"), -15.0)
                      && closeEnough(effectctrl::paramValue(brightnessContrast, "contrast"), 40.0)
                      && closeEnough(brightnessContrast.param1, -15.0)
                      && closeEnough(brightnessContrast.param2, 40.0),
                  "None identity, schema, type registration, names, or param round-trip failed",
                  passed, failed);
    }

    {
        const VideoEffect photoFilter = effectFromGuiAddPath(VideoEffectType::PhotoFilter);
        const VideoEffect tritone = effectFromGuiAddPath(VideoEffectType::Tritone);
        const VideoEffect chromaKey = effectFromGuiAddPath(VideoEffectType::ChromaKey);
        const VideoEffect fill = effectFromGuiAddPath(VideoEffectType::Fill);
        const VideoEffect tint = effectFromGuiAddPath(VideoEffectType::Tint);
        const VideoEffect gradientRamp = effectFromGuiAddPath(VideoEffectType::GradientRamp);

        printGate("G8",
                  effectctrl::colorParamValue(photoFilter, "keyColor") == QColor(236, 138, 0)
                      && effectctrl::colorParamValue(tritone, "keyColor") == QColor(0, 0, 0)
                      && effectctrl::colorParamValue(chromaKey, "color") == QColor(0, 255, 0)
                      && effectctrl::colorParamValue(fill, "keyColor") == QColor(255, 255, 255)
                      && effectctrl::colorParamValue(tint, "keyColor") == QColor(255, 255, 255)
                      && effectctrl::colorParamValue(gradientRamp, "keyColor") == QColor(255, 255, 255),
                  "GUI-add color defaults did not match effect design defaults",
                  passed, failed);
    }

    {
        const QColor custom(12, 34, 56);

        printGate("G9",
                  keyColorRoundTrips(VideoEffectType::PhotoFilter, custom)
                      && keyColorRoundTrips(VideoEffectType::Tritone, custom)
                      && keyColorRoundTrips(VideoEffectType::Tint, custom)
                      && keyColorRoundTrips(VideoEffectType::GradientRamp, custom)
                      && keyColorRoundTrips(VideoEffectType::Fill, custom),
                  "packed RGB keyColor set/read round-trip failed",
                  passed, failed);
    }

    {
        printGate("G10",
                  keyColorChannelsInterpolate()
                      && noTrackEffectsStayIdentical()
                      && legacyPackedKeyColorTrackStaysInactive(),
                  "ClipAnim keyColor channel tracks, no-track identity, or legacy packed inactivity failed",
                  passed, failed);
    }

    std::printf("ae-fx-color2 summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
