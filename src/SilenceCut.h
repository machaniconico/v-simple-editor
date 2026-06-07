#pragma once

#include <QVector>

namespace silencecut {

struct Config {
    double thresholdDb   = -40.0;
    double minSilenceSec = 0.50;
    double paddingSec    = 0.10;
    double minKeepSec    = 0.20;
    double windowSec     = 0.02;
};

struct Segment {
    double startSec = 0.0;
    double endSec   = 0.0;
    double durationSec() const { return endSec - startSec; }
};

QVector<Segment> detectKeepSegments(const QVector<float>& samples, int sampleRate, const Config& cfg);
QVector<Segment> detectSilenceSegments(const QVector<float>& samples, int sampleRate, const Config& cfg);

} // namespace silencecut
