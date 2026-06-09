#include "../EffectParamSchema.h"
#include "../VideoEffect.h"

#include <QColor>
#include <QImage>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
            image.setPixelColor(x, y, QColor((x * 31 + y * 17) % 256,
                                             (x * 13 + y * 23) % 256,
                                             (x * 7 + y * 41) % 256));
        }
    }
    return image;
}

bool imagesByteIdentical(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format() || a.bytesPerLine() != b.bytesPerLine())
        return false;
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y), a.bytesPerLine()) != 0)
            return false;
    }
    return true;
}

bool imagesDiffer(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return true;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y))
                return true;
        }
    }
    return false;
}

int lumaAt(const QImage &image, int x, int y)
{
    const QColor c = image.pixelColor(x, y);
    return static_cast<int>(std::round(0.2126 * c.red()
                                     + 0.7152 * c.green()
                                     + 0.0722 * c.blue()));
}

int brightestYInColumn(const QImage &image, int x)
{
    int bestY = 0;
    int bestLuma = -1;
    for (int y = 0; y < image.height(); ++y) {
        const int luma = lumaAt(image, x, y);
        if (luma > bestLuma) {
            bestLuma = luma;
            bestY = y;
        }
    }
    return bestY;
}

bool lineYVaries(const QImage &image)
{
    int minY = image.height();
    int maxY = 0;
    for (int x = 2; x < image.width() - 2; ++x) {
        const int y = brightestYInColumn(image, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    return maxY - minY >= 4;
}

bool brightCentroid(const QImage &image, double &cx, double &cy)
{
    double sumX = 0.0;
    double sumY = 0.0;
    double count = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (lumaAt(image, x, y) > 128) {
                sumX += x;
                sumY += y;
                count += 1.0;
            }
        }
    }
    if (count <= 0.0)
        return false;
    cx = sumX / count;
    cy = sumY / count;
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

} // namespace

int runAeFxDistortSelftest()
{
    int passed = 0;
    int failed = 0;

    {
        QImage image(32, 16, QImage::Format_RGB888);
        image.fill(Qt::black);
        for (int y = 0; y < image.height(); ++y)
            image.setPixelColor(16, y, Qt::white);

        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createRGBSplit(0.0, 0.0));
        const QImage split = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createRGBSplit(4.0, 0.0));
        const QColor nearLine = split.pixelColor(12, 8);
        const QColor originalNearLine = image.pixelColor(12, 8);

        printGate("G1",
                  imagesByteIdentical(image, identity)
                      && originalNearLine.red() == originalNearLine.blue()
                      && nearLine.red() != nearLine.blue(),
                  "RGBSplit identity or channel separation failed",
                  passed, failed);
    }

    {
        QImage image(64, 64, QImage::Format_RGB888);
        image.fill(Qt::black);
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, 31, Qt::white);
            image.setPixelColor(x, 32, Qt::white);
            image.setPixelColor(x, 33, Qt::white);
        }

        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createWaveWarp(0.0, 16.0, 0.0));
        const QImage warped = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createWaveWarp(6.0, 16.0, 0.0));

        printGate("G2",
                  imagesByteIdentical(image, identity)
                      && lineYVaries(warped),
                  "WaveWarp identity or horizontal-line warp failed",
                  passed, failed);
    }

    {
        QImage image(64, 64, QImage::Format_RGB888);
        image.fill(Qt::black);
        for (int y = 30; y <= 34; ++y) {
            for (int x = 46; x <= 50; ++x)
                image.setPixelColor(x, y, Qt::white);
        }

        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createRipple(0.0, 500.0, 90.0));
        const QImage rippled = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createRipple(8.0, 500.0, 90.0));
        double beforeX = 0.0, beforeY = 0.0, afterX = 0.0, afterY = 0.0;
        const bool beforeOk = brightCentroid(image, beforeX, beforeY);
        const bool afterOk = brightCentroid(rippled, afterX, afterY);

        printGate("G3",
                  imagesByteIdentical(image, identity)
                      && beforeOk && afterOk
                      && std::hypot(afterX - beforeX, afterY - beforeY) >= 2.0,
                  "Ripple identity or radial displacement failed",
                  passed, failed);
    }

    {
        const QImage image = makePatternImage(48, 24);
        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGlitchVHS(0.0, 8.0, 7.0));
        const QImage glitchedA = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGlitchVHS(0.8, 8.0, 7.0));
        const QImage glitchedB = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGlitchVHS(0.8, 8.0, 7.0));

        printGate("G4",
                  imagesByteIdentical(image, identity)
                      && imagesByteIdentical(glitchedA, glitchedB)
                      && imagesDiffer(image, glitchedA),
                  "GlitchVHS identity, determinism, or nonzero effect failed",
                  passed, failed);
    }

    {
        const QImage pattern = makePatternImage(10, 10);
        VideoEffect none;
        none.type = VideoEffectType::None;
        const QImage noneOut = VideoEffectProcessor::applyEffect(pattern, none);

        VideoEffect rgbSplit = VideoEffect::createRGBSplit();
        effectctrl::setParamValue(rgbSplit, "offsetX", -9.0);
        effectctrl::setParamValue(rgbSplit, "offsetY", 3.0);

        VideoEffect waveWarp = VideoEffect::createWaveWarp();
        effectctrl::setParamValue(waveWarp, "amplitude", 22.0);
        effectctrl::setParamValue(waveWarp, "wavelength", 123.0);
        effectctrl::setParamValue(waveWarp, "phase", 45.0);

        VideoEffect ripple = VideoEffect::createRipple();
        effectctrl::setParamValue(ripple, "amplitude", 18.0);
        effectctrl::setParamValue(ripple, "wavelength", 150.0);
        effectctrl::setParamValue(ripple, "phase", 90.0);

        VideoEffect glitch = VideoEffect::createGlitchVHS();
        effectctrl::setParamValue(glitch, "intensity", 0.75);
        effectctrl::setParamValue(glitch, "blockHeight", 16.0);
        effectctrl::setParamValue(glitch, "seed", 99.0);

        printGate("G5",
                  imagesByteIdentical(pattern, noneOut)
                      && hasSchemaNames(VideoEffectType::RGBSplit, QStringList{ "offsetX", "offsetY" })
                      && hasSchemaNames(VideoEffectType::WaveWarp, QStringList{ "amplitude", "wavelength", "phase" })
                      && hasSchemaNames(VideoEffectType::Ripple, QStringList{ "amplitude", "wavelength", "phase" })
                      && hasSchemaNames(VideoEffectType::GlitchVHS, QStringList{ "intensity", "blockHeight", "seed" })
                      && schemaHasDefaults(VideoEffectType::RGBSplit, QVector<double>{ 6.0, 0.0 })
                      && schemaHasDefaults(VideoEffectType::WaveWarp, QVector<double>{ 12.0, 80.0, 0.0 })
                      && schemaHasDefaults(VideoEffectType::Ripple, QVector<double>{ 12.0, 80.0, 0.0 })
                      && schemaHasDefaults(VideoEffectType::GlitchVHS, QVector<double>{ 0.5, 12.0, 1.0 })
                      && allTypesContain(VideoEffectType::RGBSplit)
                      && allTypesContain(VideoEffectType::WaveWarp)
                      && allTypesContain(VideoEffectType::Ripple)
                      && allTypesContain(VideoEffectType::GlitchVHS)
                      && closeEnough(effectctrl::paramValue(rgbSplit, "offsetX"), -9.0)
                      && closeEnough(effectctrl::paramValue(rgbSplit, "offsetY"), 3.0)
                      && closeEnough(rgbSplit.param1, -9.0)
                      && closeEnough(rgbSplit.param2, 3.0)
                      && closeEnough(effectctrl::paramValue(waveWarp, "amplitude"), 22.0)
                      && closeEnough(effectctrl::paramValue(waveWarp, "wavelength"), 123.0)
                      && closeEnough(effectctrl::paramValue(waveWarp, "phase"), 45.0)
                      && closeEnough(waveWarp.param1, 22.0)
                      && closeEnough(waveWarp.param2, 123.0)
                      && closeEnough(waveWarp.param3, 45.0)
                      && closeEnough(effectctrl::paramValue(ripple, "amplitude"), 18.0)
                      && closeEnough(effectctrl::paramValue(ripple, "wavelength"), 150.0)
                      && closeEnough(effectctrl::paramValue(ripple, "phase"), 90.0)
                      && closeEnough(ripple.param1, 18.0)
                      && closeEnough(ripple.param2, 150.0)
                      && closeEnough(ripple.param3, 90.0)
                      && closeEnough(effectctrl::paramValue(glitch, "intensity"), 0.75)
                      && closeEnough(effectctrl::paramValue(glitch, "blockHeight"), 16.0)
                      && closeEnough(effectctrl::paramValue(glitch, "seed"), 99.0)
                      && closeEnough(glitch.param1, 0.75)
                      && closeEnough(glitch.param2, 16.0)
                      && closeEnough(glitch.param3, 99.0),
                  "None identity, schema registration, allTypes registration, or param round-trip failed",
                  passed, failed);
    }

    {
        const QImage image = makePatternImage(64, 32);
        const QImage glitchedA = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGlitchVHS(1.0, 6.0, 321.0));
        const QImage glitchedB = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGlitchVHS(1.0, 6.0, 321.0));

        printGate("G6",
                  imagesByteIdentical(glitchedA, glitchedB),
                  "GlitchVHS same-params byte determinism failed",
                  passed, failed);
    }

    std::printf("ae-fx-distort summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
