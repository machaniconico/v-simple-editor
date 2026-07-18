#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include "playback/smartrender_flag.h"
#include "Timeline.h"

namespace smartrender {

struct SegmentEligibility {
    bool eligible = false;
    QString reason;
};

struct PassThroughEligibility {
    bool eligible = false;
    QString reason;
};

struct PassThroughTimelineRequest {
    QVector<QVector<ClipInfo>> videoTracks;
    QVector<QVector<ClipInfo>> audioTracks;
    QVector<bool> videoTrackMuted;
    QVector<bool> videoTrackHidden;
    QVector<bool> audioTrackMuted;
    QVector<bool> audioTrackHidden;
    QHash<QString, TimelineTrackMatteEntry> trackMatteEntries;
    QHash<QString, QString> clipParentEntries;
    bool hasAdjustmentLayers = false;
    bool hasMarkedRange = false;
    double markedIn = 0.0;
    double markedOut = 0.0;
    double timelineDurationSec = 0.0;
    QString outputPath;
    QString outputCodec;
    int outputWidth = 0;
    int outputHeight = 0;
    double outputFps = 0.0;
    qint64 jobStartUs = 0;
    qint64 jobEndUs = 0;
    bool exportMarkedRangeOnly = false;
    bool clipOdtEnabled = false;
    bool hdrExport16Enabled = false;
    bool hdrMatte16Enabled = false;
    bool applyAces = false;
    bool hdr10 = false;
    bool hlg = false;
};

SegmentEligibility canStreamCopy(const ClipInfo& clip,
                                 const QString& outputCodec,
                                 int outputWidth,
                                 int outputHeight,
                                 double outputFps,
                                 bool hasEffects,
                                 bool hasColorCorrection,
                                 bool hasTransform,
                                 bool hasTransitions,
                                 bool hasSpeedChange,
                                 bool hasKeyframes,
                                 bool hasLayerStyle,
                                 bool hasTrackMatte,
                                 bool isOverlayLayer);

PassThroughEligibility timelinePassThrough(
    const PassThroughTimelineRequest& request);

PassThroughEligibility timelinePassThrough(const Timeline* timeline,
                                           const QString& outputPath,
                                           const QString& outputCodec,
                                           int outputWidth,
                                           int outputHeight,
                                           double outputFps,
                                           qint64 jobStartUs,
                                           qint64 jobEndUs,
                                           bool exportMarkedRangeOnly,
                                           bool clipOdtEnabled,
                                           bool hdrExport16Enabled,
                                           bool hdrMatte16Enabled,
                                           bool applyAces,
                                           bool hdr10,
                                           bool hlg);

} // namespace smartrender
