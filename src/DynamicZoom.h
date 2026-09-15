#pragma once

#include "Keyframe.h"

namespace dynzoom {

// Normalized crop frame. cx/cy are canvas-relative center coordinates and w
// is the visible width as a fraction of the canvas. The visible height always
// follows the canvas aspect ratio (the normalized height is therefore w).
struct Rect {
    double cx = 0.5;
    double cy = 0.5;
    double w = 1.0;

    constexpr Rect() = default;
    constexpr Rect(double centerX, double centerY, double width)
        : cx(centerX), cy(centerY), w(width) {}
    // Compatibility input for callers that still provide h. Height is not
    // stored: Dynamic Zoom is always a uniform-scale, canvas-aspect crop.
    constexpr Rect(double centerX, double centerY, double width,
                   double /*ignoredHeight*/)
        : cx(centerX), cy(centerY), w(width) {}
};

enum class Easing {
    Linear,
    EaseInOut
};

enum class Preset {
    ZoomIn,
    ZoomOut,
    PanLeft,
    PanRight,
    PanUp,
    PanDown,
    Full
};

struct Result {
    KeyframeTrack positionX;
    KeyframeTrack positionY;
    KeyframeTrack scaleX;
    KeyframeTrack scaleY;
};

Result build(const Rect& start, const Rect& end,
             double clipStartSec, double clipDurationSec, Easing ease);
Rect presetRect(Preset preset);

} // namespace dynzoom
