#pragma once

#include <QString>
#include <QVector>

// EBU R128 / ITU-R BS.1770-4 loudness mastering.
//
// Provides integrated-loudness measurement (K-weighting + gating) and
// loudness-normalization gain computation for streaming/broadcast targets.
namespace loudness {

// Target mastering parameters for a normalization pass.
struct R128Config {
    double targetLufs = -14.0; // integrated loudness target (LUFS)
    double truePeakDb = -1.0;  // ceiling for true-peak limiting (dBTP)
    double lra        = 11.0;  // desired loudness range (LU)
};

// Common streaming / broadcast normalization targets.
enum class LoudnessPreset {
    YouTube,
    Spotify,
    AppleMusic,
    Broadcast,
    TikTok
};

// Integrated-loudness target (LUFS) for a given delivery preset.
//   YouTube=-14.0  Spotify=-14.0  AppleMusic=-16.0  Broadcast=-23.0  TikTok=-14.0
double presetTargetLufs(LoudnessPreset p);

// Measure integrated loudness (LUFS) of an audio file.
// Decoding is best-effort: raw .raw/.pcm is read directly; otherwise a safe
// broadcast fallback (-23.0) is returned with a qWarning. The deterministic
// algorithm lives in measureIntegratedLufsFromSamples().
double measureIntegratedLufs(const QString &audioPath);

// Core ITU-R BS.1770-4 integrated-loudness measurement over a mono buffer.
// Returns the gated integrated loudness in LUFS; silent/empty input -> -70.0.
double measureIntegratedLufsFromSamples(const QVector<float> &mono, int sampleRate);

// Loudness-normalization gain (dB) needed to reach targetLufs.
//   computeGainDb(measured, target) == target - measured
double computeGainDb(double measuredLufs, double targetLufs);

} // namespace loudness
