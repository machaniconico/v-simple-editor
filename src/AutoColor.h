#pragma once

#include "VideoEffect.h"

#include <QImage>

namespace autocolor {

struct ChannelStats {
    int minV = 0;
    int maxV = 0;
    double mean = 0.0;
};

struct FrameStats {
    ChannelStats r;
    ChannelStats g;
    ChannelStats b;
};

FrameStats analyzeFrame(const QImage &image);
ColorCorrection autoCorrection(const FrameStats &stats, const ColorCorrection &existing);
ColorCorrection autoCorrection(const FrameStats &stats);

} // namespace autocolor
