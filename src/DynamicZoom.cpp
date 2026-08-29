#include "DynamicZoom.h"

#include "TransformAnimator.h"

#include <algorithm>
#include <cmath>

namespace dynzoom {
namespace {

constexpr double kMinimumRectSize = 1.0e-6;

double normalizedCoordinate(double value)
{
    if (!std::isfinite(value))
        return 0.5;
    return std::clamp(value, 0.0, 1.0);
}

double normalizedSize(double value)
{
    if (!std::isfinite(value))
        return 1.0;
    return std::clamp(value, kMinimumRectSize, 1.0);
}

struct TransformValues {
    double x = 0.0;
    double y = 0.0;
    double scale = 1.0;
};

TransformValues transformForRect(const Rect& rect)
{
    const double cx = normalizedCoordinate(rect.cx);
    const double cy = normalizedCoordinate(rect.cy);
    const double width = normalizedSize(rect.w);
    const double height = normalizedSize(rect.h);

    // ClipGeometry uses one uniform videoScale. Use the larger axis ratio so
    // the requested frame fills the canvas, then translate its center back to
    // the canvas center in normalized ClipTransform coordinates.
    const double scale = std::max(1.0 / width, 1.0 / height);
    return TransformValues{
        (0.5 - cx) * scale,
        (0.5 - cy) * scale,
        scale
    };
}

KeyframeTrack makeTrack(TransformProperty property, double firstValue,
                        double lastValue, double firstTime, double lastTime,
                        KeyframePoint::Interpolation interpolation)
{
    KeyframeTrack track(TransformAnimator::propertyName(property),
                        TransformAnimator::propertyDefaultValue(property));
    track.addKeyframe(firstTime, firstValue, interpolation);
    track.addKeyframe(lastTime, lastValue, interpolation);
    return track;
}

} // namespace

Result build(const Rect& start, const Rect& end,
             double clipStartSec, double clipDurationSec, Easing ease)
{
    const double firstTime = std::isfinite(clipStartSec) ? clipStartSec : 0.0;
    const double duration = std::isfinite(clipDurationSec)
        ? std::max(0.0, clipDurationSec) : 0.0;
    const double lastTime = firstTime + duration;
    const KeyframePoint::Interpolation interpolation = ease == Easing::EaseInOut
        ? KeyframePoint::EaseInOut : KeyframePoint::Linear;
    const TransformValues first = transformForRect(start);
    const TransformValues last = transformForRect(end);

    return Result{
        makeTrack(TransformProperty::PositionX, first.x, last.x,
                  firstTime, lastTime, interpolation),
        makeTrack(TransformProperty::PositionY, first.y, last.y,
                  firstTime, lastTime, interpolation),
        makeTrack(TransformProperty::ScaleX, first.scale, last.scale,
                  firstTime, lastTime, interpolation),
        makeTrack(TransformProperty::ScaleY, first.scale, last.scale,
                  firstTime, lastTime, interpolation)
    };
}

Rect presetRect(Preset preset)
{
    switch (preset) {
    case Preset::ZoomIn:
    case Preset::ZoomOut:
        return Rect{0.5, 0.5, 0.65, 0.65};
    case Preset::PanLeft:
        return Rect{0.35, 0.5, 0.70, 0.70};
    case Preset::PanRight:
        return Rect{0.65, 0.5, 0.70, 0.70};
    case Preset::PanUp:
        return Rect{0.5, 0.35, 0.70, 0.70};
    case Preset::PanDown:
        return Rect{0.5, 0.65, 0.70, 0.70};
    case Preset::Full:
        return Rect{};
    }
    return Rect{};
}

} // namespace dynzoom
