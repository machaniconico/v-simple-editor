#pragma once

#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include <functional>

namespace deflicker {

enum class Mode {
    GlobalLuma,
    GlobalRgb,
    RollingBands,
};

struct DeflickerParams {
    Mode mode = Mode::GlobalLuma;
    int temporalWindow = 9;
    double strength = 1.0;
    bool useMedianTarget = true;
    double maxGain = 2.0;
    double minGain = 0.5;
    QRect analysisRegion;
    int bandHeight = 8;
    double bandSmoothing = 0.5;
};

struct FrameStats {
    double lumaMean = 0.0;
    double rMean = 0.0;
    double gMean = 0.0;
    double bMean = 0.0;
    QVector<double> bandLuma;
};

using FrameFetcher = std::function<QImage(int)>;
using FrameSink = std::function<bool(int, const QImage &, QString *)>;
using CancellationProbe = std::function<bool()>;

struct StreamingProcessResult {
    bool success = false;
    bool cancelled = false;
    int processedFrames = 0;
    QVector<FrameStats> stats;
    QString error;
};

// Return the actual region used for analysis. An explicit region that does
// not intersect the frame remains empty; it must not silently become full
// frame analysis.
QRect effectiveAnalysisRegion(const QImage &frame,
                              const DeflickerParams &params);

// Convert a region stored in project/canvas coordinates to decoded-frame
// coordinates. The result is intentionally not clipped so callers can report
// an entirely out-of-range region instead of silently using the full frame.
QRect scaleAnalysisRegion(const QRect &canvasRegion,
                          const QSize &canvasSize,
                          const QSize &frameSize);

FrameStats analyzeFrame(const QImage &frame, const DeflickerParams &params);

struct FrameCorrection {
    double gainR = 1.0;
    double gainG = 1.0;
    double gainB = 1.0;
    QVector<double> bandGain;
};

FrameCorrection computeCorrection(const QVector<FrameStats> &stats,
                                  int frameIndex,
                                  const DeflickerParams &params);

QImage applyCorrection(const QImage &frame,
                       const FrameCorrection &correction,
                       const DeflickerParams &params);

QVector<QImage> processSequence(const QVector<QImage> &frames,
                                const DeflickerParams &params);

// Two-pass sequence processing for long clips. The first pass retains only
// FrameStats. The second pass fetches, corrects, sinks, and releases one image
// at a time. A false sink result or a true cancellation probe stops without
// retaining generated frames.
StreamingProcessResult processSequenceStreaming(
    int frameCount,
    const FrameFetcher &frameFetcher,
    const DeflickerParams &params,
    const FrameSink &frameSink = FrameSink(),
    const CancellationProbe &shouldCancel = CancellationProbe());

} // namespace deflicker
