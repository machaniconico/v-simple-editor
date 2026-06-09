#include "AutoColor.h"

#include "WbPick.h"

#include <QColor>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kLumaR = 0.2126;
constexpr double kLumaG = 0.7152;
constexpr double kLumaB = 0.0722;
constexpr double kGradeMid = 127.5;
constexpr double kMaxAutoContrastFactor = 1.8;

double luma(double r, double g, double b)
{
    return kLumaR * r + kLumaG * g + kLumaB * b;
}

double clampGrade(double value)
{
    return qBound(-100.0, value, 100.0);
}

int clampByteFromMean(double value)
{
    return qBound(0, static_cast<int>(std::round(value)), 255);
}

} // namespace

namespace autocolor {

FrameStats analyzeFrame(const QImage &image)
{
    if (image.isNull())
        return {{0, 255, kGradeMid}, {0, 255, kGradeMid}, {0, 255, kGradeMid}};

    const int step = (image.width() > 500 && image.height() > 500) ? 2 : 1;
    int minR = 255;
    int minG = 255;
    int minB = 255;
    int maxR = 0;
    int maxG = 0;
    int maxB = 0;
    double sumR = 0.0;
    double sumG = 0.0;
    double sumB = 0.0;
    qsizetype samples = 0;

    for (int y = 0; y < image.height(); y += step) {
        for (int x = 0; x < image.width(); x += step) {
            const QColor c = image.pixelColor(x, y);
            if (c.alpha() == 0)
                continue;

            const int r = c.red();
            const int g = c.green();
            const int b = c.blue();
            minR = std::min(minR, r);
            minG = std::min(minG, g);
            minB = std::min(minB, b);
            maxR = std::max(maxR, r);
            maxG = std::max(maxG, g);
            maxB = std::max(maxB, b);
            sumR += r;
            sumG += g;
            sumB += b;
            ++samples;
        }
    }

    if (samples == 0)
        return {{0, 255, kGradeMid}, {0, 255, kGradeMid}, {0, 255, kGradeMid}};

    const double denom = static_cast<double>(samples);
    return {
        {minR, maxR, sumR / denom},
        {minG, maxG, sumG / denom},
        {minB, maxB, sumB / denom},
    };
}

ColorCorrection autoCorrection(const FrameStats &stats)
{
    ColorCorrection cc;

    const QColor neutral(
        clampByteFromMean(stats.r.mean),
        clampByteFromMean(stats.g.mean),
        clampByteFromMean(stats.b.mean));
    const wbpick::TempTintCorrection wb = wbpick::tempTintForNeutral(neutral);
    cc.temperature = wb.temperature;
    cc.tint = wb.tint;

    const double lumaMin = luma(stats.r.minV, stats.g.minV, stats.b.minV);
    const double lumaMax = luma(stats.r.maxV, stats.g.maxV, stats.b.maxV);
    const double lumaMean = luma(stats.r.mean, stats.g.mean, stats.b.mean);
    const double range = std::max(0.0, lumaMax - lumaMin);

    if (range < 255.0) {
        const double safeRange = std::max(1.0, range);
        const double fullStretchFactor = 255.0 / safeRange;
        const double contrastFactor = qBound(
            1.0,
            1.0 + (fullStretchFactor - 1.0) * 0.5,
            kMaxAutoContrastFactor);
        cc.contrast = clampGrade((std::sqrt(contrastFactor) - 1.0) * 100.0);
    }

    const double brightnessPixels = kGradeMid - lumaMean;
    cc.brightness = clampGrade(brightnessPixels / 2.55);
    if (std::abs(cc.brightness) < 0.01)
        cc.brightness = 0.0;
    if (std::abs(cc.contrast) < 0.01)
        cc.contrast = 0.0;

    return cc;
}

} // namespace autocolor
