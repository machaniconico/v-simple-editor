#pragma once

#include <cstdint>

struct ExportFrameRange {
    int64_t startFrame = 0;
    int64_t endFrame = 0;
    bool usedMarkedRange = false;

    int64_t frameCount() const { return endFrame > startFrame ? endFrame - startFrame : 0; }
};

ExportFrameRange computeExportRange(double markIn,
                                    double markOut,
                                    double fps,
                                    int64_t totalFrames,
                                    bool enabled,
                                    bool hasRange);
