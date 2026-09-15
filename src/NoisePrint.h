#pragma once
#include <QString>
#include <vector>

namespace noiseprint {
struct NoisePrint {
    int sampleRate = 0;
    int fftSize = 2048;
    // Nonnegative mean magnitudes, DC through Nyquist (fftSize / 2 + 1).
    std::vector<double> magnitude;
    bool isValid() const;
};
NoisePrint capture(const std::vector<double>& mono, int sampleRate,
                   double startSec, double endSec, int fftSize = 2048);
bool subtract(const std::vector<double>& in, std::vector<double>& out,
              const NoisePrint& print, double amountDb, double floorDb,
              int sampleRate, QString* error = nullptr);
} // namespace noiseprint
