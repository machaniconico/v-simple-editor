#include "../EffectParamSchema.h"
#include "../VideoEffect.h"

#include <QColor>
#include <QImage>
#include <QString>
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
            image.setPixelColor(x, y, QColor((x * 37 + y * 19) % 256,
                                             (x * 11 + y * 29) % 256,
                                             (x * 17 + y * 43) % 256));
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

bool imagesSamePixels(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return false;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y))
                return false;
        }
    }
    return true;
}

bool colorClose(const QColor &a, const QColor &b, int tolerance = 0)
{
    return std::abs(a.red() - b.red()) <= tolerance
        && std::abs(a.green() - b.green()) <= tolerance
        && std::abs(a.blue() - b.blue()) <= tolerance
        && std::abs(a.alpha() - b.alpha()) <= tolerance;
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

int brightPixelsInRow(const QImage &image, int y)
{
    int count = 0;
    for (int x = 0; x < image.width(); ++x) {
        if (image.pixelColor(x, y).red() > 200)
            ++count;
    }
    return count;
}

bool outputSizeMatchesInput(const QImage &input, const QImage &output)
{
    return input.size() == output.size() && !output.isNull();
}

} // namespace

int runAeFxDistort2Selftest()
{
    int passed = 0;
    int failed = 0;

    {
        const QImage image = makePatternImage(33, 33);
        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createBulge(0.0, 0.5));
        const QImage bulged = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createBulge(80.0, 1.0));

        printGate("G1",
                  imagesByteIdentical(image, identity)
                      && colorClose(bulged.pixelColor(16, 16), image.pixelColor(16, 16))
                      && !colorClose(bulged.pixelColor(22, 16), image.pixelColor(22, 16)),
                  "Bulge identity, center stability, or off-center displacement failed",
                  passed, failed);
    }

    {
        const QImage image = makePatternImage(33, 33);
        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createTwirl(0.0, 0.5));
        const QImage twirled = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createTwirl(90.0, 1.0));

        printGate("G2",
                  imagesByteIdentical(image, identity)
                      && colorClose(twirled.pixelColor(16, 16), image.pixelColor(16, 16))
                      && !colorClose(twirled.pixelColor(22, 16), image.pixelColor(22, 16)),
                  "Twirl identity, center stability, or off-center rotation failed",
                  passed, failed);
    }

    {
        const QImage image = makePatternImage(32, 32);
        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createPolarCoordinates(0, 0.0));
        const QImage polar = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createPolarCoordinates(0, 1.0));

        printGate("G3",
                  imagesByteIdentical(image, identity)
                      && !colorClose(polar.pixelColor(0, 0), image.pixelColor(0, 0)),
                  "PolarCoordinates identity or full-strength corner remap failed",
                  passed, failed);
    }

    {
        const QImage image = makePatternImage(16, 16);
        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createMotionTile(1, 1, false));
        const QImage tiled = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createMotionTile(2, 2, false));

        printGate("G4",
                  imagesByteIdentical(image, identity)
                      && colorClose(tiled.pixelColor(4, 4), image.pixelColor(8, 8)),
                  "MotionTile identity or 2x2 repeated source lookup failed",
                  passed, failed);
    }

    {
        const QImage image = makePatternImage(16, 8);
        const QImage mirroredA = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createMirror(0));
        const QImage mirroredB = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createMirror(0));

        bool rightHalfMirrored = true;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = image.width() / 2; x < image.width(); ++x) {
                if (!colorClose(mirroredA.pixelColor(x, y), image.pixelColor(image.width() - 1 - x, y))) {
                    rightHalfMirrored = false;
                    break;
                }
            }
        }

        printGate("G5",
                  rightHalfMirrored && imagesSamePixels(mirroredA, mirroredB),
                  "Mirror mode=0 right-half reflection or determinism failed",
                  passed, failed);
    }

    {
        QImage image(41, 21, QImage::Format_RGB888);
        image.fill(Qt::black);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 18; x <= 22; ++x)
                image.setPixelColor(x, y, Qt::white);
        }

        const QImage identity = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createCornerPinSimple(0.0, 0.0));
        const QImage pinned = VideoEffectProcessor::applyEffect(
            image, VideoEffect::createCornerPinSimple(60.0, 0.0));
        const int topBright = brightPixelsInRow(pinned, 0);
        const int bottomBright = brightPixelsInRow(pinned, pinned.height() - 1);

        printGate("G6",
                  imagesByteIdentical(image, identity)
                      && topBright != bottomBright,
                  "CornerPinSimple identity or trapezoid row scaling failed",
                  passed, failed);
    }

    {
        const QImage pattern = makePatternImage(10, 10);
        VideoEffect none;
        none.type = VideoEffectType::None;
        const QImage noneOut = VideoEffectProcessor::applyEffect(pattern, none);

        VideoEffect bulge = VideoEffect::createBulge();
        effectctrl::setParamValue(bulge, "amount", 42.0);
        effectctrl::setParamValue(bulge, "radius", 0.75);

        VideoEffect twirl = VideoEffect::createTwirl();
        effectctrl::setParamValue(twirl, "angle", 135.0);
        effectctrl::setParamValue(twirl, "radius", 0.65);

        VideoEffect mirror = VideoEffect::createMirror();
        effectctrl::setParamValue(mirror, "mode", 2.0);

        VideoEffect polar = VideoEffect::createPolarCoordinates();
        effectctrl::setParamValue(polar, "type", 1.0);
        effectctrl::setParamValue(polar, "amount", 0.85);

        VideoEffect tile = VideoEffect::createMotionTile();
        effectctrl::setParamValue(tile, "tilesX", 3.0);
        effectctrl::setParamValue(tile, "tilesY", 4.0);
        effectctrl::setParamValue(tile, "mirrorEdges", 1.0);

        VideoEffect corner = VideoEffect::createCornerPinSimple();
        effectctrl::setParamValue(corner, "horizontalTilt", -35.0);
        effectctrl::setParamValue(corner, "verticalTilt", 25.0);

        const QVector<VideoEffect> edgeEffects = {
            VideoEffect::createBulge(50.0, 1.0),
            VideoEffect::createTwirl(45.0, 1.0),
            VideoEffect::createMirror(0),
            VideoEffect::createPolarCoordinates(1, 1.0),
            VideoEffect::createMotionTile(2, 2, true),
            VideoEffect::createCornerPinSimple(50.0, -50.0)
        };
        const QImage one = makePatternImage(1, 1);
        const QImage two = makePatternImage(2, 2);
        bool edgeSafe = true;
        for (const auto &effect : edgeEffects) {
            edgeSafe = edgeSafe
                && outputSizeMatchesInput(one, VideoEffectProcessor::applyEffect(one, effect))
                && outputSizeMatchesInput(two, VideoEffectProcessor::applyEffect(two, effect));
        }

        printGate("G7",
                  imagesByteIdentical(pattern, noneOut)
                      && hasSchemaNames(VideoEffectType::Bulge, QStringList{ "amount", "radius" })
                      && hasSchemaNames(VideoEffectType::Twirl, QStringList{ "angle", "radius" })
                      && hasSchemaNames(VideoEffectType::Mirror, QStringList{ "mode" })
                      && hasSchemaNames(VideoEffectType::PolarCoordinates, QStringList{ "type", "amount" })
                      && hasSchemaNames(VideoEffectType::MotionTile, QStringList{ "tilesX", "tilesY", "mirrorEdges" })
                      && hasSchemaNames(VideoEffectType::CornerPinSimple, QStringList{ "horizontalTilt", "verticalTilt" })
                      && schemaHasDefaults(VideoEffectType::Bulge, QVector<double>{ 0.0, 0.5 })
                      && schemaHasDefaults(VideoEffectType::Twirl, QVector<double>{ 0.0, 0.5 })
                      && schemaHasDefaults(VideoEffectType::Mirror, QVector<double>{ 0.0 })
                      && schemaHasDefaults(VideoEffectType::PolarCoordinates, QVector<double>{ 0.0, 0.0 })
                      && schemaHasDefaults(VideoEffectType::MotionTile, QVector<double>{ 1.0, 1.0, 0.0 })
                      && schemaHasDefaults(VideoEffectType::CornerPinSimple, QVector<double>{ 0.0, 0.0 })
                      && allTypesContain(VideoEffectType::Bulge)
                      && allTypesContain(VideoEffectType::Twirl)
                      && allTypesContain(VideoEffectType::Mirror)
                      && allTypesContain(VideoEffectType::PolarCoordinates)
                      && allTypesContain(VideoEffectType::MotionTile)
                      && allTypesContain(VideoEffectType::CornerPinSimple)
                      && closeEnough(effectctrl::paramValue(bulge, "amount"), 42.0)
                      && closeEnough(effectctrl::paramValue(bulge, "radius"), 0.75)
                      && closeEnough(effectctrl::paramValue(twirl, "angle"), 135.0)
                      && closeEnough(effectctrl::paramValue(twirl, "radius"), 0.65)
                      && closeEnough(effectctrl::paramValue(mirror, "mode"), 2.0)
                      && closeEnough(effectctrl::paramValue(polar, "type"), 1.0)
                      && closeEnough(effectctrl::paramValue(polar, "amount"), 0.85)
                      && closeEnough(effectctrl::paramValue(tile, "tilesX"), 3.0)
                      && closeEnough(effectctrl::paramValue(tile, "tilesY"), 4.0)
                      && closeEnough(effectctrl::paramValue(tile, "mirrorEdges"), 1.0)
                      && closeEnough(effectctrl::paramValue(corner, "horizontalTilt"), -35.0)
                      && closeEnough(effectctrl::paramValue(corner, "verticalTilt"), 25.0)
                      && closeEnough(bulge.param1, 42.0)
                      && closeEnough(bulge.param2, 0.75)
                      && closeEnough(twirl.param1, 135.0)
                      && closeEnough(twirl.param2, 0.65)
                      && closeEnough(mirror.param1, 2.0)
                      && closeEnough(polar.param1, 1.0)
                      && closeEnough(polar.param2, 0.85)
                      && closeEnough(tile.param1, 3.0)
                      && closeEnough(tile.param2, 4.0)
                      && closeEnough(tile.param3, 1.0)
                      && closeEnough(corner.param1, -35.0)
                      && closeEnough(corner.param2, 25.0)
                      && edgeSafe,
                  "Schema round-trip, None identity, allTypes registration, or edge-pixel safety failed",
                  passed, failed);
    }

    std::printf("ae-fx-distort2 summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
