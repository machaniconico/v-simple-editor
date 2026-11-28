#pragma once

#include "PlaybackTypes.h"

#include <QtGlobal>

// Rubber-band (volume envelope) row mapping shared by Timeline and the selftests.
// The row's top is gain = audio::kMaxClipGain, the bottom is gain = 0 and unity
// stays at the vertical centre: 0..1 spans the lower half, 1..kMaxClipGain the
// upper half (piecewise linear).
namespace envgeom {

inline double gainToY(double gain, int rowHeight)
{
    const double g = qBound(0.0, gain, audio::kMaxClipGain);
    if (g <= 1.0)
        return rowHeight * (1.0 - g / 2.0);
    return rowHeight * 0.5 * (1.0 - (g - 1.0) / (audio::kMaxClipGain - 1.0));
}

inline double yToGain(double y, int rowHeight)
{
    if (rowHeight <= 0) return 1.0;
    const double t = qBound(0.0, y / static_cast<double>(rowHeight), 1.0);
    if (t >= 0.5)
        return (1.0 - t) * 2.0;
    return 1.0 + (0.5 - t) / 0.5 * (audio::kMaxClipGain - 1.0);
}

} // namespace envgeom
