#pragma once

#include <QVector>

namespace voiceiso {

enum class OutputMode {
    VoiceOnly,
    BackgroundOnly,
    Mix,
};

struct VoiceIsolationParams {
    OutputMode mode = OutputMode::VoiceOnly;
    double strength = 0.8;
    double voiceGainDb = 0.0;
    double backgroundGainDb = -18.0;
    double voiceLowHz = 80.0;
    double voiceHighHz = 8000.0;
    int fftSize = 2048;
    int hopSize = 512;
    double noiseLearnSeconds = 0.5;
    bool adaptiveNoise = true;
    double harmonicWeight = 0.5;
    double smoothingFactor = 0.7;
};

struct VoiceIsolationResult {
    QVector<float> voice;
    QVector<float> background;
    QVector<float> output;
    double estimatedVoiceRatio = 0.0;
    // The initial floor computed by the same analysis that produced the
    // separated outputs. Consumers must not re-analyze the input just to
    // display this diagnostic.
    QVector<double> noiseFloor;
};

VoiceIsolationResult isolate(const QVector<float> &samples,
                             int sampleRate,
                             const VoiceIsolationParams &params);

QVector<double> estimateNoiseFloor(const QVector<float> &samples,
                                   int sampleRate,
                                   const VoiceIsolationParams &params);

} // namespace voiceiso
