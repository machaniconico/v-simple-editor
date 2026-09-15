#include "DialogueLeveler.h"

#include "LoudnessAnalyzer.h"

#include <QtGlobal>

#include <cmath>

namespace leveler {
namespace {

struct Measurement {
    double timeSec = 0.0;
    double lufs = -70.0;
    double gainDb = 0.0;
    bool gated = true;
};

bool validConfig(const Config &config)
{
    return std::isfinite(config.targetShortTermLufs)
        && std::isfinite(config.maxBoostDb) && config.maxBoostDb >= 0.0
        && std::isfinite(config.maxCutDb) && config.maxCutDb >= 0.0
        && std::isfinite(config.windowSec) && config.windowSec > 0.0
        && std::isfinite(config.hopSec) && config.hopSec > 0.0
        && std::isfinite(config.smoothingSec) && config.smoothingSec >= 0.0
        && std::isfinite(config.gateLufs);
}

double measureWindow(const QVector<float> &mono, int sampleRate,
                     int firstSample, int lastSample)
{
    if (firstSample < 0 || lastSample <= firstSample
        || lastSample > mono.size()) {
        return -70.0;
    }

    LoudnessAnalyzer analyzer;
    analyzer.setSampleRate(sampleRate);
    analyzer.processBlock(mono.constData() + firstSample,
                          lastSample - firstSample, 1);
    return analyzer.shortTermLUFS();
}

} // namespace

Analysis analyze(const QVector<float> &mono, int sampleRate,
                 const Config &config)
{
    Analysis result;
    if (mono.isEmpty() || sampleRate <= 0 || !validConfig(config))
        return result;

    const int sampleCount = static_cast<int>(mono.size());
    const int windowSamples = qMax(1, qRound(config.windowSec * sampleRate));
    const int hopSamples = qMax(1, qRound(config.hopSec * sampleRate));
    const int halfWindow = windowSamples / 2;
    const double durationSec = static_cast<double>(sampleCount) / sampleRate;

    QVector<int> pointSamples;
    for (int sample = 0; sample < sampleCount; sample += hopSamples)
        pointSamples.append(sample);
    if (pointSamples.isEmpty() || pointSamples.last() != sampleCount)
        pointSamples.append(sampleCount);

    QVector<Measurement> measurements;
    measurements.reserve(pointSamples.size());
    for (int pointSample : pointSamples) {
        int first = qBound(0, pointSample - halfWindow, sampleCount);
        int last = qBound(first, first + windowSamples, sampleCount);
        if (last - first < windowSamples) {
            first = qMax(0, last - windowSamples);
        }

        Measurement measurement;
        measurement.timeSec = qMin(
            durationSec, static_cast<double>(pointSample) / sampleRate);
        measurement.lufs = measureWindow(mono, sampleRate, first, last);
        measurement.gated = !std::isfinite(measurement.lufs)
            || measurement.lufs < config.gateLufs;
        if (!measurement.gated) {
            measurement.gainDb = qBound(
                -config.maxCutDb,
                config.targetShortTermLufs - measurement.lufs,
                config.maxBoostDb);
            if (!result.hasMeasuredLufs) {
                result.minMeasuredLufs = measurement.lufs;
                result.maxMeasuredLufs = measurement.lufs;
                result.hasMeasuredLufs = true;
            } else {
                result.minMeasuredLufs = qMin(result.minMeasuredLufs,
                                               measurement.lufs);
                result.maxMeasuredLufs = qMax(result.maxMeasuredLufs,
                                               measurement.lufs);
            }
        }
        measurements.append(measurement);
    }

    // Hold the nearest measured value through gated regions. A positive hold
    // is limited to unity so silence/noise-floor intervals are never boosted.
    int firstMeasured = -1;
    for (int i = 0; i < measurements.size(); ++i) {
        if (!measurements[i].gated) {
            firstMeasured = i;
            break;
        }
    }
    if (firstMeasured >= 0) {
        for (int i = 0; i < firstMeasured; ++i)
            measurements[i].gainDb = qMin(0.0, measurements[firstMeasured].gainDb);
        double heldDb = measurements[firstMeasured].gainDb;
        for (int i = firstMeasured + 1; i < measurements.size(); ++i) {
            if (measurements[i].gated)
                measurements[i].gainDb = qMin(0.0, heldDb);
            else
                heldDb = measurements[i].gainDb;
        }
    }

    const double actualHopSec = static_cast<double>(hopSamples) / sampleRate;
    const double smoothingAlpha = config.smoothingSec <= 0.0
        ? 1.0
        : 1.0 - std::exp(-actualHopSec / config.smoothingSec);
    double smoothedDb = measurements.isEmpty() ? 0.0
                                                : measurements.first().gainDb;
    result.envelope.reserve(measurements.size());
    for (int i = 0; i < measurements.size(); ++i) {
        if (i > 0) {
            smoothedDb += smoothingAlpha
                * (measurements[i].gainDb - smoothedDb);
        }
        if (measurements[i].gated)
            smoothedDb = qMin(0.0, smoothedDb);
        AudioGainPoint point;
        point.time = measurements[i].timeSec;
        point.gain = std::pow(10.0, smoothedDb / 20.0);
        result.envelope.append(point);
    }
    return result;
}

QVector<AudioGainPoint> computeEnvelope(const QVector<float> &mono,
                                        int sampleRate,
                                        const Config &config)
{
    return analyze(mono, sampleRate, config).envelope;
}

} // namespace leveler
