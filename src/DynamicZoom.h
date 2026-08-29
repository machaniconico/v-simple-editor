#pragma once

#include "Keyframe.h"

namespace dynzoom {

// Normalized crop frame. cx/cy are canvas-relative center coordinates and
// w/h are fractions of the canvas dimensions.
struct Rect {
    double cx = 0.5;
    double cy = 0.5;
    double w = 1.0;
    double h = 1.0;
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
