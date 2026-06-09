#pragma once

#include <QString>

#include "playback/smartrender_flag.h"
#include "Timeline.h"

namespace smartrender {

struct SegmentEligibility {
    bool eligible = false;
    QString reason;
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

} // namespace smartrender
