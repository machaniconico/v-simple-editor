#include "../NoisePrint.h"
#include "../SpectralEngine.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

namespace {
constexpr int rate = 32768;
constexpr double pi = 3.14159265358979323846;
struct Metrics { double snr; double amplitude; };
Metrics measure(const std::vector<double>& samples) {
    // Last second: exactly 440 cycles and a power-of-two FFT, no leakage.
    std::vector<std::complex<double>> bins(rate);
    for (int i = 0; i < rate; ++i) bins[i] = samples[samples.size() - rate + i];
    spectral::fft(bins, false);
    const double signal = std::norm(bins[440]) + std::norm(bins[rate - 440]);
    double noise = 0;
    for (int k = 0; k < rate; ++k)
        if (k != 440 && k != rate - 440) noise += std::norm(bins[k]);
    return {10 * std::log10(signal / std::max(1e-30, noise)), 2 * std::abs(bins[440]) / rate};
}
double maxError(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return 1e30;
    double error = 0;
    for (size_t i = 0; i < a.size(); ++i) error = std::max(error, std::abs(a[i] - b[i]));
    return error;
}
}
int runNoisePrintSelftest() {
    int pass = 0, fail = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++pass : ++fail;
    };
    std::mt19937 rng(306);
    std::vector<double> noise(rate * 2), mixed(rate * 2), tone(rate * 2);
    double energy = 0;
    for (double &v : noise) {
        v = (static_cast<double>(rng()) / 4294967295.0) * 2 - 1;
        energy += v * v;
    }
    const double scale = (0.2 / std::sqrt(2.0)) / std::sqrt(energy / noise.size());
    for (size_t i = 0; i < noise.size(); ++i) {
        noise[i] *= scale;
        tone[i] = 0.2 * std::sin(2 * pi * 440 * i / rate);
        mixed[i] = noise[i] + (i >= rate / 2 ? tone[i] : 0.0);
    }
    const auto print = noiseprint::capture(mixed, rate, 0, 0.5);
    std::vector<double> cleaned;
    QString error;
    const bool ok = noiseprint::subtract(mixed, cleaned, print, 12, -20, rate, &error);
    const auto before = measure(mixed);
    const auto after = ok ? measure(cleaned) : Metrics{-100, 0};
    const double amplitudeDb = 20 * std::log10(std::max(1e-30, after.amplitude) / 0.2);
    std::fprintf(stderr, "SNR improvement %.3f dB, signal gain %.3f dB\n", after.snr - before.snr, amplitudeDb);
    gate(1, print.isValid() && ok && after.snr - before.snr >= 10);
    gate(2, ok && std::abs(amplitudeDb) <= 1);
    std::vector<double> bypass;
    gate(3, noiseprint::subtract(mixed, bypass, print, 0, -20, rate, &error)
            && maxError(mixed, bypass) <= 1e-4);
    // L has noise; R carries a clean tone. Deinterleave for independent API calls,
    // and also check digital silence for channel leakage.
    std::vector<double> stereo(mixed.size() * 2), left(mixed.size()), right(mixed.size());
    for (size_t i = 0; i < mixed.size(); ++i) { stereo[2 * i] = mixed[i]; stereo[2 * i + 1] = tone[i]; }
    for (size_t i = 0; i < mixed.size(); ++i) { left[i] = stereo[2 * i]; right[i] = stereo[2 * i + 1]; }
    std::vector<double> leftOut, rightOut;
    bool independent = noiseprint::subtract(left, leftOut, print, 12, -20, rate, &error)
        && noiseprint::subtract(right, rightOut, print, 12, -20, rate, &error);
    std::vector<double> silent(mixed.size(), 0.0), silentOut;
    independent = independent && noiseprint::subtract(silent, silentOut, print, 12, -20, rate, &error);
    gate(4, independent && maxError(leftOut, cleaned) == 0 && maxError(silentOut, silent) == 0
        && std::abs(20 * std::log10(measure(rightOut).amplitude / 0.2)) <= 1);
    auto invalid = print;
    invalid.fftSize *= 2;
    const bool badSize = !noiseprint::subtract(mixed, bypass, invalid, 12, -20, rate, &error) && !error.isEmpty();
    gate(5, badSize && !noiseprint::subtract(mixed, bypass, print, 12, -20, rate + 1, &error) && !error.isEmpty());
    const auto again = noiseprint::capture(mixed, rate, 0, 0.5);
    std::vector<double> repeated;
    gate(6, again.magnitude == print.magnitude
        && noiseprint::subtract(mixed, repeated, again, 12, -20, rate, &error)
        && maxError(repeated, cleaned) == 0);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", pass, fail);
    return fail;
}
