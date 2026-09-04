#pragma once

#include <QVector>
#include <QString>

namespace remix {

inline constexpr double kMaxTargetSec = 24.0 * 60.0 * 60.0;

struct Config {
    double crossfadeSec = 0.05;
};

struct Segment {
    double srcStart = 0.0;
    double srcEnd = 0.0;
};

struct Plan {
    QVector<Segment> segments;
    double resultDuration = 0.0;
    double crossfadeSec = 0.05;
    bool valid = false;
    QString error;
};

// Build a deterministic source-segment reconstruction around beat boundaries.
// Segment times are local timeline seconds in the input clip. The first and
// last source boundaries are always retained so the intro and outro survive.
Plan planRemix(const QVector<double> &beatTimes,
               double clipDuration,
               double targetDuration,
               const Config &config = Config{});

} // namespace remix
