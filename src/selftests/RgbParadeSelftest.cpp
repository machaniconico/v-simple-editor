#include "../LumetriScopes.h"

#include <QImage>

#include <cstdio>

namespace {

int peakBin(const RgbParadeData::ColumnHistogram &bins)
{
    int best = 0;
    for (int i = 1; i < 256; ++i) {
        if (bins[i] > bins[best])
            best = i;
    }
    return best;
}

bool everyColumnPeaksAt(const RgbParadeData &data, int channel, int expected, int tolerance = 0)
{
    if (data.isEmpty())
        return false;
    for (int x = 0; x < data.width(); ++x) {
        const int peak = peakBin(data.channels[channel][x]);
        if (peak < expected - tolerance || peak > expected + tolerance)
            return false;
    }
    return true;
}

bool channelsMatchEveryColumn(const RgbParadeData &data)
{
    if (data.isEmpty())
        return false;
    for (int x = 0; x < data.width(); ++x) {
        if (data.channels[0][x] != data.channels[1][x]
            || data.channels[0][x] != data.channels[2][x]) {
            return false;
        }
    }
    return true;
}

bool monotonicPeaks(const RgbParadeData &data, int channel)
{
    if (data.isEmpty())
        return false;
    int previous = -1;
    for (int x = 0; x < data.width(); ++x) {
        const int peak = peakBin(data.channels[channel][x]);
        if (peak < previous)
            return false;
        previous = peak;
    }
    return peakBin(data.channels[channel].front()) <= 2
        && peakBin(data.channels[channel].back()) >= 253;
}

QImage solidImage(int width, int height, QRgb color)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

QImage horizontalGradientImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int value = width > 1 ? qRound((255.0 * x) / (width - 1)) : 0;
            image.setPixel(x, y, qRgb(value, value, value));
        }
    }
    return image;
}

void printGate(const char *gate, bool ok, int &passed, int &failed)
{
    std::printf("[rgb-parade] %s: %s\n", gate, ok ? "PASS" : "FAIL");
    ok ? ++passed : ++failed;
}

} // namespace

int runRgbParadeSelftest()
{
    int passed = 0;
    int failed = 0;

    const RgbParadeData red = computeRgbParadeData(solidImage(8, 4, qRgb(255, 0, 0)));
    printGate("G1 pure red maps R to 255 and G/B to 0",
              everyColumnPeaksAt(red, 0, 255)
                  && everyColumnPeaksAt(red, 1, 0)
                  && everyColumnPeaksAt(red, 2, 0),
              passed,
              failed);

    const RgbParadeData gray = computeRgbParadeData(solidImage(8, 4, qRgb(128, 128, 128)));
    printGate("G2 mid-gray maps all channels to 128 with equal distributions",
              everyColumnPeaksAt(gray, 0, 128, 2)
                  && everyColumnPeaksAt(gray, 1, 128, 2)
                  && everyColumnPeaksAt(gray, 2, 128, 2)
                  && channelsMatchEveryColumn(gray),
              passed,
              failed);

    const RgbParadeData gradient = computeRgbParadeData(horizontalGradientImage(16, 4));
    printGate("G3 horizontal gradient peaks increase monotonically per channel",
              monotonicPeaks(gradient, 0)
                  && monotonicPeaks(gradient, 1)
                  && monotonicPeaks(gradient, 2),
              passed,
              failed);

    const QImage parade = renderRgbParadeImage(gray, 96, 48);
    const RgbParadeData empty = computeRgbParadeData(QImage());
    const QImage emptyParade = renderRgbParadeImage(empty, 96, 48);
    printGate("G4 render dimensions are three-panel compatible and null input is safe",
              !parade.isNull()
                  && parade.width() == 96
                  && parade.height() == 48
                  && parade.width() % 3 == 0
                  && empty.isEmpty()
                  && !emptyParade.isNull()
                  && emptyParade.width() == 96
                  && emptyParade.height() == 48,
              passed,
              failed);

    std::printf("[rgb-parade] summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
