#pragma once

#include <QVector>

// ---------------------------------------------------------------------------
// audiorestore — Sprint 22 / US-AREST-1
//
// Mono float (-1..1) audio restoration DSP primitives:
//   - deClick            : single-sample spike removal via linear interpolation
//   - deHum              : cascaded RBJ-cookbook biquad notch filters at
//                          baseHz and its harmonics (mains hum removal)
//   - spectralSubtraction: STFT overlap-add spectral subtraction NR
//   - spectralNoiseGate  : legacy block-wise RMS noise gate fallback
//   - processAll         : conditionally runs the 3 stages per RestoreConfig
//
// All functions operate on a single mono channel. Multi-channel callers should
// deinterleave, process each channel, then re-interleave.
// ---------------------------------------------------------------------------

namespace audiorestore {

struct RestoreConfig {
    double declickThreshold = 0.3;   // spike detection threshold (linear amplitude)
    double dehumFreq        = 50.0;  // mains fundamental (50 Hz EU / 60 Hz US)
    int    dehumHarmonics   = 4;     // number of notch stages (f, 2f, 3f, 4f)
    double noiseGateDb      = -45.0; // window RMS below this (dBFS) is attenuated
    double noiseReductionStrength = 1.0; // [0..2] over-subtraction coefficient
    bool   doDeclick        = true;
    bool   doDehum          = true;
    bool   doNoiseGate      = true;
};

// Detect single-sample spikes: a sample is a click if it deviates strongly
// from the linear prediction of its two neighbours AND that deviation is large
// relative to the local neighbour level. Detected clicks are replaced with the
// linear interpolation 0.5*(s[i-1]+s[i+1]).
void deClick(QVector<float> &samples, int sampleRate, double threshold);

// Apply a cascade of narrow biquad notch filters at baseHz, 2*baseHz, ...,
// harmonics*baseHz using the RBJ audio-EQ cookbook notch (Q ~= 30),
// direct-form-I, in place. Harmonics at/above Nyquist are skipped.
void deHum(QVector<float> &samples, int sampleRate, double baseHz, int harmonics);

// Sliding 1024-sample window noise gate. If a window's RMS expressed in dBFS
// is below thresholdDb, that window is softly attenuated by a factor of 0.1.
// SIMPLIFICATION: non-overlapping rectangular blocks (no overlap-add / no
// crossfade between blocks). This is intentional and adequate for the
// restoration use-case; documented per the story spec.
void spectralNoiseGate(QVector<float> &samples, int sampleRate, double thresholdDb);

// STFT overlap-add spectral subtraction. Estimates a per-bin noise magnitude
// profile from low-percentile frame magnitudes, subtracts strength*noise while
// preserving phase, mirrors positive-bin attenuation to symmetric bins, and
// reconstructs via SpectralEngine::istft().
void spectralSubtraction(QVector<float> &samples, int sampleRate, double strength);

// Copy `in`, conditionally run deClick / deHum / spectralSubtraction according
// to the boolean flags in `cfg`, and return the processed buffer.
QVector<float> processAll(const QVector<float> &in, int sampleRate, const RestoreConfig &cfg);

} // namespace audiorestore
