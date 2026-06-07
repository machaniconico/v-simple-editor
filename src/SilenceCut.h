#pragma once

#include <QVector>

#include "Timeline.h"   // for ClipInfo

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

/**
 * Convert keep-segments (relative to clip's active start = src.inPoint) into
 * sub-ClipInfo objects derived from src.
 *
 * keepsActiveSec: segments in source-seconds relative to src.inPoint
 *   (i.e. keep.startSec==0 means src.inPoint, keep.endSec==3 means src.inPoint+3s).
 * Returns sub-clips with inPoint/outPoint set in absolute source-file seconds,
 * other ClipInfo fields copied from src.
 * Empty keeps -> returns empty vector (caller interprets as "nothing to cut").
 */
QVector<ClipInfo> planKeepClips(const ClipInfo& src,
                                const QVector<Segment>& keepsActiveSec);

} // namespace silencecut
