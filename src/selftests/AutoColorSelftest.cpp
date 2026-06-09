#include "../AutoColor.h"
#include "../VideoEffect.h"

#include <QColor>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kLumaR = 0.2126;
constexpr double kLumaG = 0.7152;
constexpr double kLumaB = 0.0722;

void printGate(const char *gate, bool ok, const char *reason, int &passed, int &failed)
{
    if (ok) {
        std::printf("[auto-color] %s: PASS\n", gate);
        ++passed;
        return;
    }
    std::printf("[auto-color] %s: FAIL - %s\n", gate, reason);
    ++failed;
}

double luma(double r, double g, double b)
{
    return kLumaR * r + kLumaG * g + kLumaB * b;
}

double lumaSpread(const autocolor::FrameStats &stats)
{
    return luma(stats.r.maxV, stats.g.maxV, stats.b.maxV)
         - luma(stats.r.minV, stats.g.minV, stats.b.minV);
}

double meanRbError(const autocolor::FrameStats &stats)
{
    return std::abs(stats.r.mean - stats.b.mean);
}

QImage makeGradientImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int v = width <= 1
                ? 0
                : static_cast<int>(std::round(255.0 * x / (width - 1)));
            image.setPixelColor(x, y, QColor(v, v, v));
        }
    }
    return image;
}

QImage makeBlueCastLowContrastImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int base = width <= 1
                ? 125
                : 100 + static_cast<int>(std::round(50.0 * x / (width - 1)));
            image.setPixelColor(x, y, QColor(base - 20, base, base + 20));
        }
    }
    return image;
}

} // namespace

int runAutoColorSelftest()
{
    int passed = 0;
    int failed = 0;

    {
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(QColor(128, 128, 128));
        const autocolor::FrameStats stats = autocolor::analyzeFrame(image);
        printGate("G1 solid gray stats",
                  stats.r.minV == 128 && stats.r.maxV == 128
                      && stats.g.minV == 128 && stats.g.maxV == 128
                      && stats.b.minV == 128 && stats.b.maxV == 128
                      && std::abs(stats.r.mean - 128.0) <= 1e-9
                      && std::abs(stats.g.mean - 128.0) <= 1e-9
                      && std::abs(stats.b.mean - 128.0) <= 1e-9,
                  "solid image should report exact min/max/mean",
                  passed, failed);
    }

    {
        const QImage image = makeGradientImage(256, 4);
        const autocolor::FrameStats stats = autocolor::analyzeFrame(image);
        printGate("G2 black-white gradient stats",
                  stats.r.minV == 0 && stats.g.minV == 0 && stats.b.minV == 0
                      && stats.r.maxV == 255 && stats.g.maxV == 255 && stats.b.maxV == 255
                      && std::abs(stats.r.mean - 127.5) <= 0.6
                      && std::abs(stats.g.mean - 127.5) <= 0.6
                      && std::abs(stats.b.mean - 127.5) <= 0.6,
                  "gradient should span full range with mid mean",
                  passed, failed);
    }

    {
        const autocolor::FrameStats stats = autocolor::analyzeFrame(makeGradientImage(256, 4));
        const ColorCorrection cc = autocolor::autoCorrection(stats);
        printGate("G3 neutral full-range identity",
                  std::abs(cc.temperature) <= 1e-9
                      && std::abs(cc.tint) <= 1e-9
                      && std::abs(cc.brightness) <= 0.25
                      && std::abs(cc.contrast) <= 1e-9,
                  "neutral full-range frame should not request visible correction",
                  passed, failed);
    }

    {
        autocolor::FrameStats stats;
        stats.r = {80, 120, 100.0};
        stats.g = {90, 130, 110.0};
        stats.b = {130, 170, 150.0};
        const ColorCorrection cc = autocolor::autoCorrection(stats);
        printGate("G4 blue cast warms",
                  cc.temperature > 0.0,
                  "blue-biased means should request positive temperature",
                  passed, failed);
    }

    {
        QImage image(51, 4, QImage::Format_RGB888);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const int v = 100 + x;
                image.setPixelColor(x, y, QColor(v, v, v));
            }
        }
        const ColorCorrection cc = autocolor::autoCorrection(autocolor::analyzeFrame(image));
        printGate("G5 low contrast boosts contrast",
                  cc.contrast > 0.0,
                  "narrow tonal range should request positive contrast",
                  passed, failed);
    }

    {
        const QImage image = makeBlueCastLowContrastImage(64, 8);
        const autocolor::FrameStats before = autocolor::analyzeFrame(image);
        const ColorCorrection cc = autocolor::autoCorrection(before);
        const QImage corrected = VideoEffectProcessor::applyEffectStack(image, cc, QVector<VideoEffect>{});
        const autocolor::FrameStats after = autocolor::analyzeFrame(corrected);
        printGate("G6 closed-loop improves WB and spread",
                  meanRbError(after) < meanRbError(before)
                      && lumaSpread(after) > lumaSpread(before),
                  "real VideoEffectProcessor application should reduce blue cast and increase tonal spread",
                  passed, failed);
    }

    std::printf("[auto-color] summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
