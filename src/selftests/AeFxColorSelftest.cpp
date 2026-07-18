#include "../EffectParamSchema.h"
#include "../VideoEffect.h"

#include <QColor>
#include <QImage>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

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

QImage makePatternImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y, QColor((x * 37 + y * 13) % 256,
                                             (x * 17 + y * 29) % 256,
                                             (x * 11 + y * 43) % 256));
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

bool isGray(const QColor &c)
{
    return c.red() == c.green() && c.green() == c.blue();
}

bool allPixelsGray(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (!isGray(image.pixelColor(x, y)))
                return false;
        }
    }
    return true;
}

bool closeEnough(double a, double b)
{
    return std::abs(a - b) <= 1e-9;
}

bool hasSchemaNames(VideoEffectType type, const QStringList &names)
{
    const auto schema = effectctrl::paramSchemaFor(type);
    if (schema.size() != names.size())
        return false;
    for (int i = 0; i < schema.size(); ++i) {
        if (schema[i].name != names[i])
            return false;
    }
    return true;
}

bool schemaHasDefaults(VideoEffectType type, const QVector<double> &defaults)
{
    const auto schema = effectctrl::paramSchemaFor(type);
    if (schema.size() < defaults.size())
        return false;
    for (int i = 0; i < defaults.size(); ++i) {
        if (!closeEnough(schema[i].defaultVal, defaults[i]))
            return false;
    }
    return true;
}

bool allTypesContain(VideoEffectType type)
{
    const auto types = VideoEffect::allTypes();
    return std::find(types.begin(), types.end(), type) != types.end();
}

bool colorClose(const QColor &a, const QColor &b, int tolerance)
{
    return std::abs(a.red() - b.red()) <= tolerance
        && std::abs(a.green() - b.green()) <= tolerance
        && std::abs(a.blue() - b.blue()) <= tolerance;
}

} // namespace

int runAeFxColorSelftest()
{
    int passed = 0;
    int failed = 0;

    {
        QImage image(3, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(64, 64, 64));
        image.setPixelColor(1, 0, QColor(128, 128, 128));
        image.setPixelColor(2, 0, QColor(200, 200, 200));

        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createLevels(0.0, 255.0, 1.0));
        const QImage bright = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createLevels(0.0, 255.0, 0.5));
        const QImage clipped = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createLevels(0.0, 128.0, 1.0));

        printGate("G1",
                  allPixelsExact(image, identity)
                      && bright.pixelColor(1, 0).red() > 128
                      && clipped.pixelColor(1, 0) == QColor(255, 255, 255)
                      && clipped.pixelColor(2, 0) == QColor(255, 255, 255),
                  "Levels identity, gamma brighten, or inputWhite clip failed",
                  passed, failed);
    }

    {
        QImage image(1, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(60, 90, 120));

        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createExposure(0.0));
        const QImage up = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createExposure(1.0));
        const QImage down = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createExposure(-1.0));

        printGate("G2",
                  allPixelsExact(image, identity)
                      && colorClose(up.pixelColor(0, 0), QColor(120, 180, 240), 1)
                      && colorClose(down.pixelColor(0, 0), QColor(30, 45, 60), 1),
                  "Exposure identity, +1 stop, or -1 stop failed",
                  passed, failed);
    }

    {
        QImage image(2, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(255, 0, 0));
        image.setPixelColor(1, 0, QColor(32, 96, 160));

        const QImage blackWhite = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createBlackWhite());
        const int redGray = blackWhite.pixelColor(0, 0).red();

        printGate("G3",
                  allPixelsGray(blackWhite)
                      && std::abs(redGray - static_cast<int>(std::round(0.299 * 255.0))) <= 1,
                  "BlackWhite grayscale output or default red weight failed",
                  passed, failed);
    }

    {
        const QImage image = makePatternImage(8, 4);
        const QImage neutral = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createHueSaturation(0.0, 0.0, 0.0));
        const QImage desat = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createHueSaturation(0.0, -100.0, 0.0));

        printGate("G4",
                  allPixelsExact(image, neutral)
                      && allPixelsGray(desat),
                  "HueSaturation neutral identity or saturation=-100 grayscale failed",
                  passed, failed);
    }

    {
        QImage image(2, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(64, 128, 192));
        image.setPixelColor(1, 0, QColor(255, 255, 255));

        const QColor keyColor(20, 80, 200);
        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createTint(0.0, keyColor));
        const QImage tinted = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createTint(1.0, keyColor));

        printGate("G5",
                  allPixelsExact(image, identity)
                      && colorClose(tinted.pixelColor(1, 0), keyColor, 1),
                  "Tint amount=0 identity or white-to-keyColor mapping failed",
                  passed, failed);
    }

    {
        const QImage pattern = makePatternImage(6, 6);
        VideoEffect none;
        none.type = VideoEffectType::None;
        const QImage noneOut = VideoEffectProcessor::applyEffect(pattern, none);

        VideoEffect levels = VideoEffect::createLevels();
        effectctrl::setParamValue(levels, "inputBlack", 12.0);
        effectctrl::setParamValue(levels, "inputWhite", 220.0);
        effectctrl::setParamValue(levels, "gamma", 0.75);

        VideoEffect tint = VideoEffect::createTint();
        effectctrl::setParamValue(tint, "amount", 0.65);
        effectctrl::setColorParam(tint, "keyColor", QColor(180, 210, 240));

        VideoEffect blackWhite = VideoEffect::createBlackWhite();
        effectctrl::setParamValue(blackWhite, "redWeight", 0.4);
        effectctrl::setParamValue(blackWhite, "greenWeight", 0.5);
        effectctrl::setParamValue(blackWhite, "blueWeight", 0.1);

        VideoEffect exposure = VideoEffect::createExposure();
        effectctrl::setParamValue(exposure, "stops", 1.25);

        VideoEffect hueSaturation = VideoEffect::createHueSaturation();
        effectctrl::setParamValue(hueSaturation, "hueDegrees", 45.0);
        effectctrl::setParamValue(hueSaturation, "saturation", -25.0);
        effectctrl::setParamValue(hueSaturation, "lightness", 10.0);

        printGate("G6",
                  allPixelsExact(pattern, noneOut)
                      && hasSchemaNames(VideoEffectType::Levels, QStringList{ "inputBlack", "inputWhite", "gamma" })
                      && hasSchemaNames(VideoEffectType::Tint, QStringList{ "amount", "keyColor" })
                      && hasSchemaNames(VideoEffectType::BlackWhite, QStringList{ "redWeight", "greenWeight", "blueWeight" })
                      && hasSchemaNames(VideoEffectType::Exposure, QStringList{ "stops" })
                      && hasSchemaNames(VideoEffectType::HueSaturation, QStringList{ "hueDegrees", "saturation", "lightness" })
                      && schemaHasDefaults(VideoEffectType::Levels, QVector<double>{ 0.0, 255.0, 1.0 })
                      && schemaHasDefaults(VideoEffectType::Tint, QVector<double>{ 1.0 })
                      && schemaHasDefaults(VideoEffectType::BlackWhite, QVector<double>{ 0.299, 0.587, 0.114 })
                      && schemaHasDefaults(VideoEffectType::Exposure, QVector<double>{ 0.0 })
                      && schemaHasDefaults(VideoEffectType::HueSaturation, QVector<double>{ 0.0, 0.0, 0.0 })
                      && allTypesContain(VideoEffectType::Levels)
                      && allTypesContain(VideoEffectType::Tint)
                      && allTypesContain(VideoEffectType::BlackWhite)
                      && allTypesContain(VideoEffectType::Exposure)
                      && allTypesContain(VideoEffectType::HueSaturation)
                      && closeEnough(effectctrl::paramValue(levels, "inputBlack"), 12.0)
                      && closeEnough(effectctrl::paramValue(levels, "inputWhite"), 220.0)
                      && closeEnough(effectctrl::paramValue(levels, "gamma"), 0.75)
                      && closeEnough(levels.param1, 12.0)
                      && closeEnough(levels.param2, 220.0)
                      && closeEnough(levels.param3, 0.75)
                      && closeEnough(effectctrl::paramValue(tint, "amount"), 0.65)
                      && effectctrl::colorParamValue(tint, "keyColor") == QColor(180, 210, 240)
                      && closeEnough(tint.param1, 0.65)
                      && tint.keyColor == QColor(180, 210, 240)
                      && closeEnough(effectctrl::paramValue(blackWhite, "redWeight"), 0.4)
                      && closeEnough(effectctrl::paramValue(blackWhite, "greenWeight"), 0.5)
                      && closeEnough(effectctrl::paramValue(blackWhite, "blueWeight"), 0.1)
                      && closeEnough(blackWhite.param1, 0.4)
                      && closeEnough(blackWhite.param2, 0.5)
                      && closeEnough(blackWhite.param3, 0.1)
                      && closeEnough(effectctrl::paramValue(exposure, "stops"), 1.25)
                      && closeEnough(exposure.param1, 1.25)
                      && closeEnough(effectctrl::paramValue(hueSaturation, "hueDegrees"), 45.0)
                      && closeEnough(effectctrl::paramValue(hueSaturation, "saturation"), -25.0)
                      && closeEnough(effectctrl::paramValue(hueSaturation, "lightness"), 10.0)
                      && closeEnough(hueSaturation.param1, 45.0)
                      && closeEnough(hueSaturation.param2, -25.0)
                      && closeEnough(hueSaturation.param3, 10.0),
                  "None identity, schema registration, allTypes registration, or param round-trip failed",
                  passed, failed);
    }

    std::printf("ae-fx-color summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
