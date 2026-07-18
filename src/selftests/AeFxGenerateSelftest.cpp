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
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int alpha = ((x + y) % 7 == 0) ? 0 : (96 + (x * 17 + y * 11) % 160);
            image.setPixelColor(x, y, QColor((x * 23 + y * 13) % 256,
                                             (x * 7 + y * 29) % 256,
                                             (x * 31 + y * 5) % 256,
                                             alpha));
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

int lumaAt(const QImage &image, int x, int y)
{
    const QColor c = image.pixelColor(x, y);
    return static_cast<int>(std::round(0.2126 * c.red()
                                     + 0.7152 * c.green()
                                     + 0.0722 * c.blue()));
}

double colorDistance(const QColor &a, const QColor &b)
{
    const double dr = a.red() - b.red();
    const double dg = a.green() - b.green();
    const double db = a.blue() - b.blue();
    return std::sqrt(dr * dr + dg * dg + db * db);
}

bool fillIsRedWithAlphaPreserved(const QImage &input, const QImage &output)
{
    if (input.size() != output.size())
        return false;
    for (int y = 0; y < input.height(); ++y) {
        for (int x = 0; x < input.width(); ++x) {
            const QColor src = input.pixelColor(x, y);
            const QColor out = output.pixelColor(x, y);
            if (src.alpha() != out.alpha())
                return false;
            if (src.alpha() > 0 && !(out.red() == 255 && out.green() == 0 && out.blue() == 0))
                return false;
        }
    }
    return true;
}

int rowLumaSum(const QImage &image, int y)
{
    int sum = 0;
    for (int x = 0; x < image.width(); ++x)
        sum += lumaAt(image, x, y);
    return sum;
}

bool imageHasLumaVariation(const QImage &image)
{
    int minValue = 255;
    int maxValue = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int value = lumaAt(image, x, y);
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }
    }
    return maxValue > minValue;
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

bool allTypesContain(VideoEffectType type)
{
    const auto types = VideoEffect::allTypes();
    return std::find(types.begin(), types.end(), type) != types.end();
}

} // namespace

int runAeFxGenerateSelftest()
{
    int passed = 0;
    int failed = 0;

    {
        const QImage image = makePatternImage(12, 8);
        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createFill(QColor(255, 0, 0), 0.0));
        const QImage filled = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createFill(QColor(255, 0, 0), 1.0));

        printGate("G1",
                  imagesByteIdentical(image, identity)
                      && fillIsRedWithAlphaPreserved(image, filled),
                  "Fill opacity identity, red fill, or alpha preservation failed",
                  passed, failed);
    }

    {
        const QImage image = makePatternImage(24, 8);
        const QColor startColor(255, 0, 0);
        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGradientRamp(0, 0.0, 0.0, startColor));
        const QImage ramp = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGradientRamp(0, 0.0, 1.0, startColor));
        const QColor leading = ramp.pixelColor(ramp.width() - 1, ramp.height() / 2);
        const QColor opposite = ramp.pixelColor(0, ramp.height() / 2);

        printGate("G2",
                  imagesByteIdentical(image, identity)
                      && colorDistance(leading, startColor) < colorDistance(opposite, startColor),
                  "GradientRamp opacity identity or linear leading-edge color failed",
                  passed, failed);
    }

    {
        QImage image(25, 25, QImage::Format_ARGB32);
        image.fill(Qt::black);
        image.setPixelColor(12, 12, Qt::white);

        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createBloom(200.0, 4.0, 0.0));
        const QImage bloomed = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createBloom(200.0, 4.0, 1.0));

        printGate("G3",
                  imagesByteIdentical(image, identity)
                      && lumaAt(bloomed, 15, 12) > lumaAt(image, 15, 12),
                  "Bloom intensity identity or bright-pass spread failed",
                  passed, failed);
    }

    {
        QImage image(18, 8, QImage::Format_ARGB32);
        image.fill(QColor(128, 128, 128, 255));

        const QImage noDarkness = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createScanlines(2.0, 0.0, 1.0));
        const QImage noOpacity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createScanlines(2.0, 0.75, 0.0));
        const QImage scanA = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createScanlines(2.0, 0.75, 1.0));
        const QImage scanB = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createScanlines(2.0, 0.75, 1.0));

        printGate("G4",
                  imagesByteIdentical(image, noDarkness)
                      && imagesByteIdentical(image, noOpacity)
                      && rowLumaSum(scanA, 0) != rowLumaSum(scanA, 1)
                      && imagesByteIdentical(scanA, scanB),
                  "Scanlines identity, row alternation, or determinism failed",
                  passed, failed);
    }

    {
        QImage image(32, 32, QImage::Format_ARGB32);
        image.fill(QColor(128, 128, 128, 255));

        const QImage halfA = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createHalftone(8.0, 45.0));
        const QImage halfB = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createHalftone(8.0, 45.0));

        printGate("G5",
                  imagesByteIdentical(halfA, halfB)
                      && imageHasLumaVariation(halfA),
                  "Halftone determinism or mid-gray dot pattern failed",
                  passed, failed);
    }

    {
        const QImage pattern = makePatternImage(10, 10);
        VideoEffect none;
        none.type = VideoEffectType::None;
        const QImage noneOut = VideoEffectProcessor::applyEffect(pattern, none);

        VideoEffect gradient = VideoEffect::createGradientRamp();
        effectctrl::setParamValue(gradient, "type", 1.0);
        effectctrl::setParamValue(gradient, "angle", 33.0);
        effectctrl::setParamValue(gradient, "opacity", 0.25);
        effectctrl::setColorParam(gradient, "keyColor", QColor(10, 20, 30));

        VideoEffect fill = VideoEffect::createFill();
        effectctrl::setParamValue(fill, "opacity", 0.65);
        effectctrl::setColorParam(fill, "keyColor", QColor(200, 10, 40));

        VideoEffect bloom = VideoEffect::createBloom();
        effectctrl::setParamValue(bloom, "threshold", 192.0);
        effectctrl::setParamValue(bloom, "radius", 28.0);
        effectctrl::setParamValue(bloom, "intensity", 1.6);

        VideoEffect scanlines = VideoEffect::createScanlines();
        effectctrl::setParamValue(scanlines, "lineSpacing", 6.0);
        effectctrl::setParamValue(scanlines, "darkness", 0.35);
        effectctrl::setParamValue(scanlines, "opacity", 0.8);

        VideoEffect halftone = VideoEffect::createHalftone();
        effectctrl::setParamValue(halftone, "dotSize", 11.0);
        effectctrl::setParamValue(halftone, "angle", 75.0);

        printGate("G6",
                  imagesByteIdentical(pattern, noneOut)
                      && hasSchemaNames(VideoEffectType::GradientRamp, QStringList{ "type", "angle", "opacity", "keyColor" })
                      && hasSchemaNames(VideoEffectType::Fill, QStringList{ "opacity", "keyColor" })
                      && hasSchemaNames(VideoEffectType::Bloom, QStringList{ "threshold", "radius", "intensity" })
                      && hasSchemaNames(VideoEffectType::Scanlines, QStringList{ "lineSpacing", "darkness", "opacity" })
                      && hasSchemaNames(VideoEffectType::Halftone, QStringList{ "dotSize", "angle" })
                      && allTypesContain(VideoEffectType::GradientRamp)
                      && allTypesContain(VideoEffectType::Fill)
                      && allTypesContain(VideoEffectType::Bloom)
                      && allTypesContain(VideoEffectType::Scanlines)
                      && allTypesContain(VideoEffectType::Halftone)
                      && closeEnough(effectctrl::paramValue(gradient, "type"), 1.0)
                      && closeEnough(effectctrl::paramValue(gradient, "angle"), 33.0)
                      && closeEnough(effectctrl::paramValue(gradient, "opacity"), 0.25)
                      && effectctrl::colorParamValue(gradient, "keyColor") == QColor(10, 20, 30)
                      && closeEnough(gradient.param1, 1.0)
                      && closeEnough(gradient.param2, 33.0)
                      && closeEnough(gradient.param3, 0.25)
                      && closeEnough(effectctrl::paramValue(fill, "opacity"), 0.65)
                      && effectctrl::colorParamValue(fill, "keyColor") == QColor(200, 10, 40)
                      && closeEnough(fill.param1, 0.65)
                      && closeEnough(effectctrl::paramValue(bloom, "threshold"), 192.0)
                      && closeEnough(effectctrl::paramValue(bloom, "radius"), 28.0)
                      && closeEnough(effectctrl::paramValue(bloom, "intensity"), 1.6)
                      && closeEnough(bloom.param1, 192.0)
                      && closeEnough(bloom.param2, 28.0)
                      && closeEnough(bloom.param3, 1.6)
                      && closeEnough(effectctrl::paramValue(scanlines, "lineSpacing"), 6.0)
                      && closeEnough(effectctrl::paramValue(scanlines, "darkness"), 0.35)
                      && closeEnough(effectctrl::paramValue(scanlines, "opacity"), 0.8)
                      && closeEnough(scanlines.param1, 6.0)
                      && closeEnough(scanlines.param2, 0.35)
                      && closeEnough(scanlines.param3, 0.8)
                      && closeEnough(effectctrl::paramValue(halftone, "dotSize"), 11.0)
                      && closeEnough(effectctrl::paramValue(halftone, "angle"), 75.0)
                      && closeEnough(halftone.param1, 11.0)
                      && closeEnough(halftone.param2, 75.0),
                  "None identity, schema registration, allTypes registration, or param round-trip failed",
                  passed, failed);
    }

    std::printf("ae-fx-generate summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
