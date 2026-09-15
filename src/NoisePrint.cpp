#include "NoisePrint.h"
#include "SpectralEngine.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace noiseprint {
namespace {
bool validSize(int n) { return n >= 2 && n <= 65536 && (n & (n - 1)) == 0; }
bool finiteSamples(const std::vector<double>& v) {
    return std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); });
}
}
bool NoisePrint::isValid() const {
    return sampleRate > 0 && validSize(fftSize)
        && magnitude.size() == static_cast<size_t>(fftSize / 2 + 1)
        && std::all_of(magnitude.begin(), magnitude.end(), [](double x) {
            return std::isfinite(x) && x >= 0.0;
        });
}
NoisePrint capture(const std::vector<double>& mono, int sampleRate,
                   double startSec, double endSec, int fftSize) {
    NoisePrint result;
    if (sampleRate <= 0 || !validSize(fftSize) || !std::isfinite(startSec)
        || !std::isfinite(endSec) || startSec < 0 || endSec <= startSec
        || endSec > mono.size() / static_cast<double>(sampleRate)) return result;
    const size_t first = static_cast<size_t>(std::llround(startSec * sampleRate));
    const size_t last = std::min(mono.size(), static_cast<size_t>(std::llround(endSec * sampleRate)));
    if (last <= first || last - first < static_cast<size_t>(fftSize)) return result;
    std::vector<double> range(mono.begin() + first, mono.begin() + last);
    if (!finiteSamples(range)) return result;
    auto spectrum = spectral::stft(range, sampleRate, fftSize, fftSize / 2);
    result.sampleRate = sampleRate;
    result.fftSize = fftSize;
    result.magnitude.assign(fftSize / 2 + 1, 0.0);
    // Only complete windows: zero-padded tails would underestimate the print.
    const size_t count = 1 + (range.size() - fftSize) / (fftSize / 2);
    for (size_t f = 0; f < count; ++f)
        for (size_t k = 0; k < result.magnitude.size(); ++k)
            result.magnitude[k] += std::abs(spectrum.frames[f][k]) / count;
    return result;
}
bool subtract(const std::vector<double>& in, std::vector<double>& out,
              const NoisePrint& print, double amountDb, double floorDb,
              int sampleRate, QString* error) {
    if (error) error->clear();
    auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!print.isValid()) return fail(QStringLiteral("ノイズプリントの FFT サイズまたは振幅が不正です。"));
    if (sampleRate != print.sampleRate) return fail(QStringLiteral("ノイズプリントと音声のサンプルレートが一致しません。"));
    if (!std::isfinite(amountDb) || amountDb < 0 || amountDb > 40
        || !std::isfinite(floorDb) || floorDb > 0 || floorDb < -120
        || !finiteSamples(in)) return fail(QStringLiteral("音声または除去設定が不正です。"));
    if (in.size() > static_cast<size_t>(std::numeric_limits<int>::max() - print.fftSize * 2))
        return fail(QStringLiteral("処理する音声が長すぎます。"));
    if (amountDb == 0 || in.empty()) { out = in; return true; }
    const int pad = print.fftSize / 2;
    std::vector<double> padded(pad, 0.0);
    padded.insert(padded.end(), in.begin(), in.end());
    padded.resize(padded.size() + pad, 0.0);
    auto s = spectral::stft(padded, sampleRate, print.fftSize, pad);
    // Oversubtraction rises continuously from zero; 12 dB uses alpha=2.
    const double alpha = amountDb / 6.0;
    const double floor = std::pow(10.0, floorDb / 20.0);
    for (auto& frame : s.frames) {
        for (int k = 0; k <= print.fftSize / 2; ++k) {
            const double magnitude = std::abs(frame[k]);
            const double gain = magnitude > 0
                ? std::max(floor, 1.0 - alpha * print.magnitude[k] / magnitude) : 1.0;
            frame[k] *= gain;
            if (k > 0 && k < print.fftSize / 2) frame[print.fftSize - k] *= gain;
        }
    }
    const auto restored = spectral::istft(s, static_cast<int>(padded.size()));
    out.assign(restored.begin() + pad, restored.begin() + pad + in.size());
    return true;
}
} // namespace noiseprint
