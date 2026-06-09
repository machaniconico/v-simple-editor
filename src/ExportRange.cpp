#include "ExportRange.h"

#include <algorithm>
#include <cmath>

ExportFrameRange computeExportRange(double markIn,
                                    double markOut,
                                    double fps,
                                    int64_t totalFrames,
                                    bool enabled,
                                    bool hasRange)
{
    const int64_t clampedTotal = std::max<int64_t>(0, totalFrames);
    const ExportFrameRange whole{0, clampedTotal};

    if (!enabled
        || !hasRange
        || markIn < 0.0
        || !(markOut > markIn)
        || !std::isfinite(markIn)
        || !std::isfinite(markOut)
        || !std::isfinite(fps)
        || fps <= 0.0) {
        return whole;
    }

    const int64_t startFrame = std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(markIn * fps)),
        0,
        clampedTotal);
    const int64_t endFrame = std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(markOut * fps)),
        0,
        clampedTotal);

    if (endFrame <= startFrame)
        return whole;

    return {startFrame, endFrame, true};
}
