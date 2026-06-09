#include "../EffectParamSchema.h"
#include "../VideoEffect.h"

#include <QColor>
#include <QImage>

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

int lumaAt(const QImage &image, int x, int y)
{
    const QColor c = image.pixelColor(x, y);
    return static_cast<int>(std::round(0.2126 * c.red()
                                     + 0.7152 * c.green()
                                     + 0.0722 * c.blue()));
}

double lumaSum(const QImage &image)
{
    double sum = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            sum += lumaAt(image, x, y);
    }
    return sum;
}

QImage makePatternImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y, QColor((x * 17 + y * 3) % 256,
                                             (x * 5 + y * 11) % 256,
                                             (x * 13 + y * 7) % 256));
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

int nonBlackCountRow(const QImage &image, int y)
{
    int count = 0;
    for (int x = 0; x < image.width(); ++x) {
        if (lumaAt(image, x, y) > 0)
            ++count;
    }
    return count;
}

int nonBlackCountColumn(const QImage &image, int x)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        if (lumaAt(image, x, y) > 0)
            ++count;
    }
    return count;
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

} // namespace

int runAeFxBlurSelftest()
{
    int passed = 0;
    int failed = 0;

    {
        QImage image(32, 32, QImage::Format_RGB888);
        image.fill(Qt::black);
        for (int y = 0; y < image.height(); ++y)
            image.setPixelColor(16, y, Qt::white);

        const QImage blurred = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGaussianBlur(5.0));
        const int left = lumaAt(blurred, 14, 16);
        const int right = lumaAt(blurred, 18, 16);
        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGaussianBlur(0.0));
        printGate("G1",
                  left > 0 && left < 255
                      && right > 0 && right < 255
                      && allPixelsWithin(image, identity, 1),
                  "GaussianBlur stripe spread or radius=0 identity failed",
                  passed, failed);
    }

    {
        QImage image(32, 32, QImage::Format_RGB888);
        image.fill(Qt::black);
        image.setPixelColor(16, 16, Qt::white);

        const double beforeSum = lumaSum(image);
        const QImage blurred = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGaussianBlur(5.0));
        const double afterSum = lumaSum(blurred);
        const int center = lumaAt(blurred, 16, 16);
        const int neighbor = lumaAt(blurred, 17, 16);
        printGate("G2",
                  std::abs(afterSum - beforeSum) <= beforeSum * 0.10
                      && center < 255
                      && neighbor > 0,
                  "GaussianBlur energy, center attenuation, or neighbor spread failed",
                  passed, failed);
    }

    {
        QImage image(64, 64, QImage::Format_RGB888);
        image.fill(Qt::black);
        for (int y = 28; y <= 36; ++y)
            image.setPixelColor(32, y, Qt::white);

        const QImage blurred = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createDirectionalBlur(0.0, 20.0));
        const int horizontalSpread = nonBlackCountRow(blurred, 32);
        const int verticalSpread = nonBlackCountColumn(blurred, 32);
        printGate("G3",
                  horizontalSpread > verticalSpread,
                  "DirectionalBlur angle=0 did not spread more horizontally than vertically",
                  passed, failed);
    }

    {
        QImage zoomImage(64, 64, QImage::Format_RGB888);
        zoomImage.fill(Qt::black);
        zoomImage.setPixelColor(32, 32, Qt::white);

        const QImage zoomBlurred = VideoEffectProcessor::applyEffect(
            zoomImage, VideoEffect::createRadialBlur(10.0, 1));

        QImage spinImage(64, 64, QImage::Format_RGB888);
        spinImage.fill(Qt::black);
        spinImage.setPixelColor(32, 32, Qt::white);
        spinImage.setPixelColor(42, 32, Qt::white);

        const QImage spinBlurred = VideoEffectProcessor::applyEffect(
            spinImage, VideoEffect::createRadialBlur(10.0, 0));
        const int spinSource = lumaAt(spinBlurred, 42, 32);
        const int spinNeighbor = lumaAt(spinBlurred, 42, 31);
        printGate("G4",
                  lumaAt(zoomBlurred, 32, 32) > 200
                      && lumaAt(zoomBlurred, 4, 4) > 0
                      && lumaAt(spinBlurred, 32, 32) > 200
                      && spinSource > 0 && spinSource < 255
                      && spinNeighbor > 0,
                  "RadialBlur zoom center/corner or spin smear failed",
                  passed, failed);
    }

    {
        VideoEffect gaussian = VideoEffect::createGaussianBlur();
        gaussian.param2 = 123.0;
        effectctrl::setParamValue(gaussian, "radius", 12.25);

        VideoEffect directional = VideoEffect::createDirectionalBlur();
        effectctrl::setParamValue(directional, "angle", 37.0);
        effectctrl::setParamValue(directional, "length", 42.0);

        VideoEffect radial = VideoEffect::createRadialBlur();
        effectctrl::setParamValue(radial, "amount", 24.0);
        effectctrl::setParamValue(radial, "mode", 1.0);

        printGate("G5",
                  hasSchemaNames(VideoEffectType::GaussianBlur, QStringList{ "radius" })
                      && hasSchemaNames(VideoEffectType::DirectionalBlur, QStringList{ "angle", "length" })
                      && hasSchemaNames(VideoEffectType::RadialBlur, QStringList{ "amount", "mode" })
                      && closeEnough(effectctrl::paramValue(gaussian, "radius"), 12.25)
                      && closeEnough(gaussian.param1, 12.25)
                      && closeEnough(gaussian.param2, 123.0)
                      && closeEnough(effectctrl::paramValue(directional, "angle"), 37.0)
                      && closeEnough(effectctrl::paramValue(directional, "length"), 42.0)
                      && closeEnough(directional.param1, 37.0)
                      && closeEnough(directional.param2, 42.0)
                      && closeEnough(effectctrl::paramValue(radial, "amount"), 24.0)
                      && closeEnough(effectctrl::paramValue(radial, "mode"), 1.0)
                      && closeEnough(radial.param1, 24.0)
                      && closeEnough(radial.param2, 1.0),
                  "EffectParamSchema round-trip failed",
                  passed, failed);
    }

    {
        const QImage image = makePatternImage(24, 24);
        const QImage gaussian = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createGaussianBlur(0.0));
        const QImage directional = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createDirectionalBlur(45.0, 0.0));
        const QImage radial = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createRadialBlur(0.0, 1));
        VideoEffect none;
        none.type = VideoEffectType::None;
        const QImage noneOut = VideoEffectProcessor::applyEffect(image, none);

        printGate("G6",
                  allPixelsWithin(image, gaussian, 1)
                      && allPixelsWithin(image, directional, 1)
                      && allPixelsWithin(image, radial, 1)
                      && allPixelsExact(image, noneOut),
                  "identity checks failed",
                  passed, failed);
    }

    std::printf("ae-fx-blur summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
