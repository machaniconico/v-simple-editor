#include "../WbPick.h"
#include "../VideoEffect.h"

#include <QColor>
#include <QImage>

#include <cmath>
#include <cstdio>

namespace {

void printGate(const char *gate, bool ok, const char *reason, int &passed, int &failed)
{
    if (ok) {
        std::printf("[wb-eyedropper] %s: PASS\n", gate);
        ++passed;
        return;
    }
    std::printf("[wb-eyedropper] %s: FAIL - %s\n", gate, reason);
    ++failed;
}

double neutralTintError(const QColor &c)
{
    const double rbMid = (c.red() + c.blue()) * 0.5;
    return std::abs(c.green() - rbMid);
}

} // namespace

int runWbEyedropperSelftest()
{
    int passed = 0;
    int failed = 0;

    {
        const auto correction = wbpick::tempTintForNeutral(QColor(128, 128, 128));
        printGate("G1 neutral -> zero",
                  std::abs(correction.temperature) <= 1e-9
                      && std::abs(correction.tint) <= 1e-9,
                  "neutral pixel should not request temperature/tint correction",
                  passed, failed);
    }

    {
        const auto correction = wbpick::tempTintForNeutral(QColor(100, 110, 160));
        printGate("G2 blue cast -> positive temperature",
                  correction.temperature > 0.0,
                  "bluish pixel should warm via positive temperature",
                  passed, failed);
    }

    {
        const auto correction = wbpick::tempTintForNeutral(QColor(180, 140, 90));
        printGate("G3 orange cast -> negative temperature",
                  correction.temperature < 0.0,
                  "orange/red pixel should cool via negative temperature",
                  passed, failed);
    }

    {
        const auto correction = wbpick::tempTintForNeutral(QColor(110, 160, 110));
        printGate("G4 green cast -> positive magenta tint",
                  correction.tint > 0.0,
                  "positive tint is magenta in VideoEffect: -G, +R/+B",
                  passed, failed);
    }

    {
        const QColor casted(100, 110, 160);
        const auto correction = wbpick::tempTintForNeutral(casted);
        QImage img(1, 1, QImage::Format_RGB888);
        img.setPixelColor(0, 0, casted);
        VideoEffectProcessor::adjustTemperatureTint(
            img, correction.temperature, correction.tint);
        const QColor corrected = img.pixelColor(0, 0);

        const double beforeRb = std::abs(casted.red() - casted.blue());
        const double afterRb = std::abs(corrected.red() - corrected.blue());
        const double beforeTint = neutralTintError(casted);
        const double afterTint = neutralTintError(corrected);
        printGate("G5 closed-loop closer to neutral",
                  afterRb < beforeRb && afterTint < beforeTint,
                  "computed inverse must reduce both R/B and green-vs-magenta errors",
                  passed, failed);
    }

    {
        const auto correction = wbpick::tempTintForNeutral(QColor(0, 255, 255));
        printGate("G6 extreme cast clamps",
                  correction.temperature >= -100.0 && correction.temperature <= 100.0
                      && correction.tint >= -100.0 && correction.tint <= 100.0,
                  "temperature/tint must remain in [-100, 100]",
                  passed, failed);
    }

    std::printf("[wb-eyedropper] summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
