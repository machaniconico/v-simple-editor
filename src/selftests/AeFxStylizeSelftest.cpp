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

int lumaAt(const QImage &image, int x, int y)
{
    const QColor c = image.pixelColor(x, y);
    return static_cast<int>(std::round(0.2126 * c.red()
                                     + 0.7152 * c.green()
                                     + 0.0722 * c.blue()));
}

int maxLuma(const QImage &image)
{
    int maxValue = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            maxValue = std::max(maxValue, lumaAt(image, x, y));
    }
    return maxValue;
}

QImage makePatternImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y, QColor((x * 29 + y * 7) % 256,
                                             (x * 11 + y * 19) % 256,
                                             (x * 5 + y * 31) % 256));
        }
    }
    return image;
}

QImage makeBinaryPatternImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x)
            image.setPixelColor(x, y, ((x + y) % 2) == 0 ? Qt::black : Qt::white);
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

bool allChannelsBinary(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            const int channels[] = { c.red(), c.green(), c.blue() };
            for (int value : channels) {
                if (!((value <= 1) || (value >= 254)))
                    return false;
            }
        }
    }
    return true;
}

bool allPixelsBlackOrWhite(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            const bool black = c.red() == 0 && c.green() == 0 && c.blue() == 0;
            const bool white = c.red() == 255 && c.green() == 255 && c.blue() == 255;
            if (!black && !white)
                return false;
        }
    }
    return true;
}

int brightCountInColumn(const QImage &image, int x)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        if (lumaAt(image, x, y) > 128)
            ++count;
    }
    return count;
}

bool allPixelsMidGray(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (std::abs(c.red() - 128) > 2
                || std::abs(c.green() - 128) > 2
                || std::abs(c.blue() - 128) > 2)
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

bool allTypesContain(VideoEffectType type)
{
    const auto types = VideoEffect::allTypes();
    return std::find(types.begin(), types.end(), type) != types.end();
}

} // namespace

int runAeFxStylizeSelftest()
{
    int passed = 0;
    int failed = 0;

    {
        const QImage image = makePatternImage(16, 8);
        const QImage twoLevels = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createPosterize(2.0));
        const QImage unchanged = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createPosterize(256.0));
        printGate("G1",
                  allChannelsBinary(twoLevels)
                      && allPixelsWithin(image, unchanged, 0),
                  "Posterize levels=2 binary collapse or levels=256 identity failed",
                  passed, failed);
    }

    {
        QImage gradient(8, 1, QImage::Format_RGB888);
        for (int x = 0; x < gradient.width(); ++x) {
            const int value = static_cast<int>(std::round(x * 255.0 / (gradient.width() - 1)));
            gradient.setPixelColor(x, 0, QColor(value, value, value));
        }

        const QImage thresholded = VideoEffectProcessor::applyEffect(
            gradient, VideoEffect::createThreshold(128.0));
        bool leftBlackRightWhite = true;
        for (int x = 0; x < gradient.width(); ++x) {
            const int expected = x < 4 ? 0 : 255;
            const QColor c = thresholded.pixelColor(x, 0);
            leftBlackRightWhite = leftBlackRightWhite
                && c.red() == expected && c.green() == expected && c.blue() == expected;
        }
        printGate("G2",
                  allPixelsBlackOrWhite(thresholded) && leftBlackRightWhite,
                  "Threshold did not produce only black/white with expected gradient split",
                  passed, failed);
    }

    {
        QImage image(3, 1, QImage::Format_RGB888);
        image.setPixelColor(0, 0, QColor(64, 64, 64));
        image.setPixelColor(1, 0, QColor(128, 128, 128));
        image.setPixelColor(2, 0, QColor(200, 200, 200));

        const QImage solarized = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createSolarize(128.0));
        printGate("G3",
                  solarized.pixelColor(0, 0) == QColor(64, 64, 64)
                      && solarized.pixelColor(1, 0) == QColor(128, 128, 128)
                      && solarized.pixelColor(2, 0) == QColor(55, 55, 55),
                  "Solarize below/equal threshold preservation or above-threshold inversion failed",
                  passed, failed);
    }

    {
        QImage solid(16, 16, QImage::Format_RGB888);
        solid.fill(QColor(80, 120, 160));
        const QImage solidEdges = VideoEffectProcessor::applyEffect(
            solid, VideoEffect::createFindEdges(1.0));

        QImage edge(16, 16, QImage::Format_RGB888);
        for (int y = 0; y < edge.height(); ++y) {
            for (int x = 0; x < edge.width(); ++x)
                edge.setPixelColor(x, y, x < 8 ? Qt::black : Qt::white);
        }
        const QImage edgeOut = VideoEffectProcessor::applyEffect(
            edge, VideoEffect::createFindEdges(1.0));
        const int edgeColumnHits = brightCountInColumn(edgeOut, 7)
                                 + brightCountInColumn(edgeOut, 8);
        printGate("G4",
                  maxLuma(solidEdges) <= 1
                      && edgeColumnHits >= edge.height(),
                  "FindEdges solid image or sharp-edge response failed",
                  passed, failed);
    }

    {
        QImage dim(21, 21, QImage::Format_RGB888);
        dim.fill(QColor(64, 64, 64));
        const QImage dimGlow = VideoEffectProcessor::applyEffect(
            dim, VideoEffect::createGlow(250.0, 5.0, 1.0));

        QImage spot(21, 21, QImage::Format_RGB888);
        spot.fill(Qt::black);
        spot.setPixelColor(10, 10, Qt::white);
        const QImage spotGlow = VideoEffectProcessor::applyEffect(
            spot, VideoEffect::createGlow(200.0, 3.0, 1.0));

        const QImage noIntensity = VideoEffectProcessor::applyEffect(
            spot, VideoEffect::createGlow(200.0, 3.0, 0.0));
        printGate("G5",
                  allPixelsWithin(dim, dimGlow, 0)
                      && lumaAt(spotGlow, 11, 10) > lumaAt(spot, 11, 10)
                      && allPixelsExact(spot, noIntensity),
                  "Glow high-threshold identity, bright spread, or intensity=0 identity failed",
                  passed, failed);
    }

    {
        QImage flat(9, 9, QImage::Format_RGB888);
        flat.fill(QColor(80, 160, 200));
        const QImage embossed = VideoEffectProcessor::applyEffect(
            flat, VideoEffect::createEmboss(45.0, 2.0));
        printGate("G6",
                  allPixelsMidGray(embossed),
                  "Emboss flat region was not near 128 gray",
                  passed, failed);
    }

    {
        const QImage pattern = makePatternImage(12, 12);
        const QImage binary = makeBinaryPatternImage(12, 12);

        VideoEffect none;
        none.type = VideoEffectType::None;
        const QImage noneOut = VideoEffectProcessor::applyEffect(pattern, none);
        const QImage glowOut = VideoEffectProcessor::applyEffect(
            pattern, VideoEffect::createGlow(128.0, 10.0, 0.0));
        const QImage findEdgesOut = VideoEffectProcessor::applyEffect(
            pattern, VideoEffect::createFindEdges(0.0));
        const QImage embossOut = VideoEffectProcessor::applyEffect(
            pattern, VideoEffect::createEmboss(45.0, 0.0));
        const QImage posterizeOut = VideoEffectProcessor::applyEffect(
            pattern, VideoEffect::createPosterize(256.0));
        const QImage thresholdOut = VideoEffectProcessor::applyEffect(
            binary, VideoEffect::createThreshold(128.0));
        const QImage solarizeOut = VideoEffectProcessor::applyEffect(
            pattern, VideoEffect::createSolarize(255.0));

        VideoEffect glow = VideoEffect::createGlow();
        effectctrl::setParamValue(glow, "threshold", 220.0);
        effectctrl::setParamValue(glow, "radius", 12.5);
        effectctrl::setParamValue(glow, "intensity", 2.25);

        VideoEffect findEdges = VideoEffect::createFindEdges();
        effectctrl::setParamValue(findEdges, "intensity", 1.5);

        VideoEffect emboss = VideoEffect::createEmboss();
        effectctrl::setParamValue(emboss, "angle", 135.0);
        effectctrl::setParamValue(emboss, "amount", 3.5);

        VideoEffect posterize = VideoEffect::createPosterize();
        effectctrl::setParamValue(posterize, "levels", 8.0);

        VideoEffect threshold = VideoEffect::createThreshold();
        effectctrl::setParamValue(threshold, "level", 96.0);

        VideoEffect solarize = VideoEffect::createSolarize();
        effectctrl::setParamValue(solarize, "threshold", 180.0);

        printGate("G7",
                  allPixelsExact(pattern, noneOut)
                      && allPixelsWithin(pattern, glowOut, 0)
                      && allPixelsWithin(pattern, findEdgesOut, 0)
                      && allPixelsWithin(pattern, embossOut, 0)
                      && allPixelsWithin(pattern, posterizeOut, 0)
                      && allPixelsWithin(binary, thresholdOut, 0)
                      && allPixelsWithin(pattern, solarizeOut, 0)
                      && hasSchemaNames(VideoEffectType::Glow, QStringList{ "threshold", "radius", "intensity" })
                      && hasSchemaNames(VideoEffectType::FindEdges, QStringList{ "intensity" })
                      && hasSchemaNames(VideoEffectType::Emboss, QStringList{ "angle", "amount" })
                      && hasSchemaNames(VideoEffectType::Posterize, QStringList{ "levels" })
                      && hasSchemaNames(VideoEffectType::Threshold, QStringList{ "level" })
                      && hasSchemaNames(VideoEffectType::Solarize, QStringList{ "threshold" })
                      && allTypesContain(VideoEffectType::Glow)
                      && allTypesContain(VideoEffectType::FindEdges)
                      && allTypesContain(VideoEffectType::Emboss)
                      && allTypesContain(VideoEffectType::Posterize)
                      && allTypesContain(VideoEffectType::Threshold)
                      && allTypesContain(VideoEffectType::Solarize)
                      && closeEnough(effectctrl::paramValue(glow, "threshold"), 220.0)
                      && closeEnough(effectctrl::paramValue(glow, "radius"), 12.5)
                      && closeEnough(effectctrl::paramValue(glow, "intensity"), 2.25)
                      && closeEnough(glow.param1, 220.0)
                      && closeEnough(glow.param2, 12.5)
                      && closeEnough(glow.param3, 2.25)
                      && closeEnough(effectctrl::paramValue(findEdges, "intensity"), 1.5)
                      && closeEnough(findEdges.param1, 1.5)
                      && closeEnough(effectctrl::paramValue(emboss, "angle"), 135.0)
                      && closeEnough(effectctrl::paramValue(emboss, "amount"), 3.5)
                      && closeEnough(emboss.param1, 135.0)
                      && closeEnough(emboss.param2, 3.5)
                      && closeEnough(effectctrl::paramValue(posterize, "levels"), 8.0)
                      && closeEnough(posterize.param1, 8.0)
                      && closeEnough(effectctrl::paramValue(threshold, "level"), 96.0)
                      && closeEnough(threshold.param1, 96.0)
                      && closeEnough(effectctrl::paramValue(solarize, "threshold"), 180.0)
                      && closeEnough(solarize.param1, 180.0),
                  "identity checks, allTypes registration, or schema round-trip failed",
                  passed, failed);
    }

    std::printf("ae-fx-stylize summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
