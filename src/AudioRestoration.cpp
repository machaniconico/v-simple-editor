#include "AudioRestoration.h"
#include "SpectralEngine.h"

#include <algorithm>
#include <complex>
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace audiorestore {

// ---------------------------------------------------------------------------
// deClick — single-sample spike removal
// ---------------------------------------------------------------------------
//
// For an interior sample s[i], the linear prediction from its neighbours is
// p = 0.5 * (s[i-1] + s[i+1]). A genuine click is a single-sample outlier:
//   1. |s[i] - p| exceeds the absolute `threshold`, AND
//   2. the deviation is large *relative* to the local neighbour spread, so
//      we do not flag legitimate fast transients where the neighbours are
//      themselves large.
//
// Detected clicks are replaced with the linear interpolation p. We detect on
// the original buffer and write into a copy so a click cannot influence the
// detection of an adjacent sample.
// ---------------------------------------------------------------------------
void deClick(QVector<float> &samples, int sampleRate, double threshold)
{
    Q_UNUSED(sampleRate);
    const int n = samples.size();
    if (n < 3 || threshold <= 0.0)
        return;

    QVector<float> out = samples; // detect on `samples`, write to `out`

    for (int i = 1; i < n - 1; ++i) {
        const double prev = static_cast<double>(samples[i - 1]);
        const double cur  = static_cast<double>(samples[i]);
        const double next = static_cast<double>(samples[i + 1]);

        const double pred = 0.5 * (prev + next);
        const double dev  = std::fabs(cur - pred);

        // Local neighbour magnitude — how big a jump the signal "should"
        // legitimately be making around this sample.
        const double neighbourLevel =
            std::fabs(next - prev) + std::fabs(prev) * 0.05 + 1e-6;

        // Click iff the deviation clears the absolute threshold AND is large
        // compared to the legitimate local activity (factor 3.0).
        if (dev > threshold && dev > 3.0 * neighbourLevel) {
            out[i] = static_cast<float>(pred);
        }
    }

    samples = out;
}

// ---------------------------------------------------------------------------
// Biquad direct-form-I helper (RBJ cookbook notch)
// ---------------------------------------------------------------------------
//
// w0    = 2*pi*f0/Fs
// alpha = sin(w0) / (2*Q)
//
//   b0 =  1            b1 = -2*cos(w0)        b2 =  1
//   a0 =  1 + alpha    a1 = -2*cos(w0)        a2 =  1 - alpha
//
// All coefficients normalised by a0, applied in place as direct-form-I:
//
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
// ---------------------------------------------------------------------------
namespace {

void applyNotch(QVector<float> &s, double f0, double fs, double Q)
{
    if (f0 <= 0.0 || fs <= 0.0 || f0 >= 0.5 * fs)
        return; // at/above Nyquist — nothing sensible to notch

    const double w0     = 2.0 * M_PI * f0 / fs;
    const double cosw0  = std::cos(w0);
    const double sinw0  = std::sin(w0);
    const double alpha  = sinw0 / (2.0 * Q);

    const double b0 = 1.0;
    const double b1 = -2.0 * cosw0;
    const double b2 = 1.0;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosw0;
    const double a2 = 1.0 - alpha;

    // Normalise by a0.
    const double nb0 = b0 / a0;
    const double nb1 = b1 / a0;
    const double nb2 = b2 / a0;
    const double na1 = a1 / a0;
    const double na2 = a2 / a0;

    double x1 = 0.0, x2 = 0.0; // x[n-1], x[n-2]
    double y1 = 0.0, y2 = 0.0; // y[n-1], y[n-2]

    const int n = s.size();
    for (int i = 0; i < n; ++i) {
        const double x0 = static_cast<double>(s[i]);
        const double y0 = nb0 * x0 + nb1 * x1 + nb2 * x2
                          - na1 * y1 - na2 * y2;

        s[i] = static_cast<float>(y0);

        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// deHum — cascade of narrow notch filters at f, 2f, ... harmonics*f
// ---------------------------------------------------------------------------
void deHum(QVector<float> &samples, int sampleRate, double baseHz, int harmonics)
{
    if (samples.isEmpty() || sampleRate <= 0 || baseHz <= 0.0 || harmonics < 1)
        return;

    const double Q  = 30.0; // narrow notch per spec
    const double fs = static_cast<double>(sampleRate);

    for (int h = 1; h <= harmonics; ++h) {
        const double f0 = baseHz * static_cast<double>(h);
        if (f0 >= 0.5 * fs)
            break; // remaining harmonics are above Nyquist
        applyNotch(samples, f0, fs, Q);
    }
}

// ---------------------------------------------------------------------------
// spectralNoiseGate — block-wise RMS noise gate
// ---------------------------------------------------------------------------
//
// SIMPLIFICATION (documented): operates on consecutive non-overlapping
// 1024-sample rectangular blocks. For each block we compute RMS, convert to
// dBFS, and if it falls below `thresholdDb` we scale the whole block by 0.1
// (a soft -20 dB attenuation rather than a hard mute). No overlap-add and no
// inter-block crossfade — adequate for the restoration use-case and matches
// the story's "simple block is fine; document the simplification" allowance.
// ---------------------------------------------------------------------------
void spectralNoiseGate(QVector<float> &samples, int sampleRate, double thresholdDb)
{
    Q_UNUSED(sampleRate);
    const int n = samples.size();
    if (n == 0)
        return;

    const int    kWindow   = 1024;
    const float  kSoftGain = 0.1f;   // ~ -20 dBFS soft attenuation

    for (int start = 0; start < n; start += kWindow) {
        const int end = qMin(start + kWindow, n);
        const int len = end - start;
        if (len <= 0)
            break;

        double sumSq = 0.0;
        for (int i = start; i < end; ++i) {
            const double v = static_cast<double>(samples[i]);
            sumSq += v * v;
        }

        const double rms = std::sqrt(sumSq / static_cast<double>(len));

        // dBFS relative to full-scale 1.0; guard against log10(0).
        const double rmsDb = (rms > 1e-12)
            ? 20.0 * std::log10(rms)
            : -200.0;

        if (rmsDb < thresholdDb) {
            for (int i = start; i < end; ++i)
                samples[i] *= kSoftGain;
        }
    }
}

// ---------------------------------------------------------------------------
// spectralSubtraction — overlap-add STFT spectral subtraction
// ---------------------------------------------------------------------------
void spectralSubtraction(QVector<float> &samples, int sampleRate, double strength)
{
    const int n = samples.size();
    const int kFftSize = 1024;
    const int kHopSize = 256;
    if (n < kFftSize || sampleRate <= 0 || strength <= 0.0)
        return;

    const double amount = std::max(0.0, std::min(2.0, strength));
    if (amount <= 0.0)
        return;

    std::vector<double> input;
    input.reserve(static_cast<size_t>(n));
    for (float s : samples)
        input.push_back(std::isfinite(s) ? static_cast<double>(s) : 0.0);

    spectral::Stft spectrum = spectral::stft(input, sampleRate, kFftSize, kHopSize);
    if (spectrum.fftSize != kFftSize || spectrum.hopSize <= 0 || spectrum.frames.empty())
        return;

    const int positiveBins = (kFftSize / 2) + 1;
    std::vector<double> noiseMag(static_cast<size_t>(positiveBins), 0.0);
    std::vector<double> mags;
    mags.reserve(spectrum.frames.size());

    for (int k = 0; k < positiveBins; ++k) {
        mags.clear();
        for (const auto &frame : spectrum.frames) {
            if (static_cast<int>(frame.size()) == kFftSize)
                mags.push_back(std::abs(frame[static_cast<size_t>(k)]));
        }
        if (mags.empty())
            continue;

        std::sort(mags.begin(), mags.end());
        const size_t idx = std::min(mags.size() - 1,
                                    static_cast<size_t>(mags.size() * 0.20));
        noiseMag[static_cast<size_t>(k)] = mags[idx];
    }

    const double spectralFloor = 0.08;
    for (auto &frame : spectrum.frames) {
        if (static_cast<int>(frame.size()) != kFftSize)
            continue;

        for (int k = 0; k < positiveBins; ++k) {
            const size_t bin = static_cast<size_t>(k);
            const double mag = std::abs(frame[bin]);
            if (mag <= 1e-20) {
                frame[bin] = std::complex<double>(0.0, 0.0);
                if (k > 0 && k < kFftSize / 2)
                    frame[static_cast<size_t>(kFftSize - k)] = std::complex<double>(0.0, 0.0);
                continue;
            }

            const double reduced = std::max(mag - amount * noiseMag[bin],
                                            spectralFloor * mag);
            const double gain = reduced / mag;
            frame[bin] *= gain;

            if (k > 0 && k < kFftSize / 2) {
                const size_t mirror = static_cast<size_t>(kFftSize - k);
                frame[mirror] *= gain;
            }
        }
    }

    const std::vector<double> restored = spectral::istft(spectrum, n);
    if (static_cast<int>(restored.size()) != n)
        return;

    for (int i = 0; i < n; ++i) {
        double v = restored[static_cast<size_t>(i)];
        if (!std::isfinite(v))
            v = 0.0;
        v = std::max(-1.0, std::min(1.0, v));
        samples[i] = static_cast<float>(v);
    }
}

// ---------------------------------------------------------------------------
// processAll — conditional 3-stage pipeline
// ---------------------------------------------------------------------------
QVector<float> processAll(const QVector<float> &in, int sampleRate, const RestoreConfig &cfg)
{
    QVector<float> out = in; // copy; never mutate caller's buffer

    // Sanitize non-finite input: a single NaN/Inf would otherwise propagate
    // through the deHum biquad feedback and corrupt every subsequent sample.
    for (float &s : out) {
        if (!std::isfinite(s))
            s = 0.0f;
    }

    if (cfg.doDeclick)
        deClick(out, sampleRate, cfg.declickThreshold);

    if (cfg.doDehum)
        deHum(out, sampleRate, cfg.dehumFreq, cfg.dehumHarmonics);

    if (cfg.doNoiseGate)
        spectralSubtraction(out, sampleRate, cfg.noiseReductionStrength);

    return out;
}

} // namespace audiorestore
