#pragma once

#include <QVector>

#include "PlaybackTypes.h"

namespace leveler {

struct Config {
    double targetShortTermLufs = -18.0;
    double maxBoostDb = 8.0;
    double maxCutDb = 12.0;
    double windowSec = 1.0;
    double hopSec = 0.25;
    double smoothingSec = 0.5;
    double gateLufs = -45.0;
};

struct Analysis {
    QVector<AudioGainPoint> envelope;
    double minMeasuredLufs = 0.0;
    double maxMeasuredLufs = 0.0;
    bool hasMeasuredLufs = false;
};

// Generate deterministic clip-local gain automation from mono PCM. LUFS
// measurements use LoudnessAnalyzer so this feature follows the same
// K-weighting implementation as the application's loudness meters.
Analysis analyze(const QVector<float> &mono, int sampleRate,
                 const Config &config = Config{});

QVector<AudioGainPoint> computeEnvelope(
    const QVector<float> &mono, int sampleRate,
    const Config &config = Config{});

} // namespace leveler
