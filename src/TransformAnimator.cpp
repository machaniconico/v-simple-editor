#include "TransformAnimator.h"

#include <QPainter>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <set>

// --- Property helpers ---

QString TransformAnimator::propertyName(TransformProperty property)
{
    switch (property) {
    case TransformProperty::PositionX:  return QStringLiteral("positionX");
    case TransformProperty::PositionY:  return QStringLiteral("positionY");
    case TransformProperty::ScaleX:     return QStringLiteral("scaleX");
    case TransformProperty::ScaleY:     return QStringLiteral("scaleY");
    case TransformProperty::Rotation:   return QStringLiteral("rotation");
    case TransformProperty::Opacity:    return QStringLiteral("opacity");
    case TransformProperty::AnchorX:    return QStringLiteral("anchorX");
    case TransformProperty::AnchorY:    return QStringLiteral("anchorY");
    case TransformProperty::SkewX:      return QStringLiteral("skewX");
    case TransformProperty::SkewY:      return QStringLiteral("skewY");
    }
    return QStringLiteral("unknown");
}

TransformProperty TransformAnimator::propertyFromName(const QString &name)
{
    if (name == QLatin1String("positionX"))  return TransformProperty::PositionX;
    if (name == QLatin1String("positionY"))  return TransformProperty::PositionY;
    if (name == QLatin1String("scaleX"))     return TransformProperty::ScaleX;
    if (name == QLatin1String("scaleY"))     return TransformProperty::ScaleY;
    if (name == QLatin1String("rotation"))   return TransformProperty::Rotation;
    if (name == QLatin1String("opacity"))    return TransformProperty::Opacity;
    if (name == QLatin1String("anchorX"))    return TransformProperty::AnchorX;
    if (name == QLatin1String("anchorY"))    return TransformProperty::AnchorY;
    if (name == QLatin1String("skewX"))      return TransformProperty::SkewX;
    if (name == QLatin1String("skewY"))      return TransformProperty::SkewY;
    return TransformProperty::PositionX; // fallback
}

double TransformAnimator::propertyDefaultValue(TransformProperty property)
{
    switch (property) {
    case TransformProperty::ScaleX:   return 1.0;
    case TransformProperty::ScaleY:   return 1.0;
    case TransformProperty::Opacity:  return 1.0;
    default:                          return 0.0;
    }
}

// --- Constructor ---

TransformAnimator::TransformAnimator()
{
    ensureTracks();
}

void TransformAnimator::ensureTracks()
{
    if (m_tracks.size() == 10)
        return;

    m_tracks.clear();
    m_tracks.reserve(10);
    for (int i = 0; i < 10; ++i) {
        auto prop = static_cast<TransformProperty>(i);
        m_tracks.append(KeyframeTrack(propertyName(prop), propertyDefaultValue(prop)));
    }
}

int TransformAnimator::trackIndex(TransformProperty property) const
{
    return static_cast<int>(property);
}

// --- Keyframe management ---

void TransformAnimator::setKeyframe(double time, TransformProperty property, double value,
                                    KeyframePoint::Interpolation interp)
{
    int idx = trackIndex(property);
    m_tracks[idx].addKeyframe(time, value, interp);
}

void TransformAnimator::removeKeyframe(double time, TransformProperty property)
{
    int idx = trackIndex(property);
    const auto &kfs = m_tracks[idx].keyframes();
    for (int i = 0; i < kfs.size(); ++i) {
        if (std::abs(kfs[i].time - time) < 0.001) {
            m_tracks[idx].removeKeyframe(i);
            return;
        }
    }
}

// --- Per-property access ---

KeyframeTrack *TransformAnimator::track(TransformProperty property)
{
    return &m_tracks[trackIndex(property)];
}

const KeyframeTrack *TransformAnimator::track(TransformProperty property) const
{
    return &m_tracks[trackIndex(property)];
}

// --- Evaluation ---

TransformState TransformAnimator::getTransformAt(double time) const
{
    TransformState state;
    state.posX     = m_tracks[trackIndex(TransformProperty::PositionX)].valueAt(time);
    state.posY     = m_tracks[trackIndex(TransformProperty::PositionY)].valueAt(time);
    state.scaleX   = m_tracks[trackIndex(TransformProperty::ScaleX)].valueAt(time);
    state.scaleY   = m_tracks[trackIndex(TransformProperty::ScaleY)].valueAt(time);
    state.rotation = m_tracks[trackIndex(TransformProperty::Rotation)].valueAt(time);
    state.opacity  = m_tracks[trackIndex(TransformProperty::Opacity)].valueAt(time);
    state.anchorX  = m_tracks[trackIndex(TransformProperty::AnchorX)].valueAt(time);
    state.anchorY  = m_tracks[trackIndex(TransformProperty::AnchorY)].valueAt(time);
    state.skewX    = m_tracks[trackIndex(TransformProperty::SkewX)].valueAt(time);
    state.skewY    = m_tracks[trackIndex(TransformProperty::SkewY)].valueAt(time);
    return state;
}

// --- Image processing ---

QImage TransformAnimator::applyTransform(const QImage &image, const TransformState &state)
{
    if (image.isNull())
        return image;

    // Short-circuit for identity transform (only opacity might differ)
    if (state.posX == 0.0 && state.posY == 0.0
        && state.scaleX == 1.0 && state.scaleY == 1.0
        && state.rotation == 0.0 && state.opacity >= 1.0
        && state.anchorX == 0.0 && state.anchorY == 0.0
        && state.skewX == 0.0 && state.skewY == 0.0) {
        return image;
    }

    QImage result(image.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // Compute anchor in image coordinates (default = center)
    double cx = image.width() / 2.0 + state.anchorX;
    double cy = image.height() / 2.0 + state.anchorY;

    QTransform transform;
    // 1. Move origin to anchor
    transform.translate(cx, cy);
    // 2. Apply position offset
    transform.translate(state.posX, state.posY);
    // 3. Rotate
    transform.rotate(state.rotation);
    // 4. Skew
    transform.shear(std::tan(state.skewX * M_PI / 180.0),
                    std::tan(state.skewY * M_PI / 180.0));
    // 5. Scale
    transform.scale(state.scaleX, state.scaleY);
    // 6. Move origin back
    transform.translate(-cx, -cy);

    painter.setTransform(transform);
    painter.setOpacity(std::clamp(state.opacity, 0.0, 1.0));
    painter.drawImage(0, 0, image);
    painter.end();

    return result;
}

// --- Motion path ---

QVector<QPointF> TransformAnimator::generateMotionPath(TransformProperty property,
                                                       double startTime, double endTime,
                                                       int samples) const
{
    QVector<QPointF> path;
    if (samples < 2 || endTime <= startTime)
        return path;

    path.reserve(samples);
    double step = (endTime - startTime) / (samples - 1);

    for (int i = 0; i < samples; ++i) {
        double t = startTime + step * i;
        double val = m_tracks[trackIndex(property)].valueAt(t);
        path.append(QPointF(t, val));
    }
    return path;
}

// --- Query ---

bool TransformAnimator::hasAnimation() const
{
    for (const auto &track : m_tracks)
        if (track.hasKeyframes()) return true;
    return false;
}

QVector<double> TransformAnimator::allKeyframeTimes() const
{
    std::set<double> times; // auto-sorted, deduplicated by 3-decimal rounding
    for (const auto &track : m_tracks) {
        for (const auto &kf : track.keyframes()) {
            // Round to millisecond to de-duplicate close times
            double rounded = std::round(kf.time * 1000.0) / 1000.0;
            times.insert(rounded);
        }
    }
    QVector<double> result;
    result.reserve(static_cast<int>(times.size()));
    for (double t : times)
        result.append(t);
    return result;
}

// --- Serialisation ---

QJsonObject TransformAnimator::toJson() const
{
    QJsonObject obj;
    QJsonArray tracksArr;

    for (const auto &track : m_tracks) {
        if (!track.hasKeyframes())
            continue;

        QJsonObject trackObj;
        trackObj[QStringLiteral("property")] = track.propertyName();

        QJsonArray kfArr;
        for (const auto &kf : track.keyframes()) {
            kfArr.append(keyframePointToJson(kf));
        }
        trackObj[QStringLiteral("keyframes")] = kfArr;
        tracksArr.append(trackObj);
    }

    obj[QStringLiteral("tracks")] = tracksArr;
    return obj;
}

void TransformAnimator::fromJson(const QJsonObject &obj)
{
    ensureTracks();

    QJsonArray tracksArr = obj[QStringLiteral("tracks")].toArray();
    for (const auto &trackVal : tracksArr) {
        QJsonObject trackObj = trackVal.toObject();
        QString propName = trackObj[QStringLiteral("property")].toString();
        TransformProperty prop = propertyFromName(propName);
        int idx = trackIndex(prop);

        // Clear existing keyframes by re-creating the track
        m_tracks[idx] = KeyframeTrack(propName, propertyDefaultValue(prop));

        QJsonArray kfArr = trackObj[QStringLiteral("keyframes")].toArray();
        for (const auto &kfVal : kfArr) {
            const KeyframePoint kf = keyframePointFromJson(kfVal.toObject());
            m_tracks[idx].addKeyframe(kf.time, kf.value, kf.interpolation,
                                      kf.bezX1, kf.bezY1, kf.bezX2, kf.bezY2);
        }
    }
}

// --- Built-in animation presets ---

TransformAnimator TransformAnimator::createSlideIn(SlideDirection direction, double duration,
                                                   double distance)
{
    TransformAnimator anim;
    TransformProperty prop = TransformProperty::PositionX;
    double startVal = 0.0;

    switch (direction) {
    case SlideDirection::Left:
        prop = TransformProperty::PositionX;
        startVal = -distance;
        break;
    case SlideDirection::Right:
        prop = TransformProperty::PositionX;
        startVal = distance;
        break;
    case SlideDirection::Top:
        prop = TransformProperty::PositionY;
        startVal = -distance;
        break;
    case SlideDirection::Bottom:
        prop = TransformProperty::PositionY;
        startVal = distance;
        break;
    }

    anim.setKeyframe(0.0, prop, startVal, KeyframePoint::EaseOut);
    anim.setKeyframe(duration, prop, 0.0);
    return anim;
}

TransformAnimator TransformAnimator::createFadeIn(double duration)
{
    TransformAnimator anim;
    anim.setKeyframe(0.0, TransformProperty::Opacity, 0.0, KeyframePoint::EaseIn);
    anim.setKeyframe(duration, TransformProperty::Opacity, 1.0);
    return anim;
}

TransformAnimator TransformAnimator::createFadeOut(double duration)
{
    TransformAnimator anim;
    anim.setKeyframe(0.0, TransformProperty::Opacity, 1.0, KeyframePoint::EaseOut);
    anim.setKeyframe(duration, TransformProperty::Opacity, 0.0);
    return anim;
}

TransformAnimator TransformAnimator::createZoomIn(double duration)
{
    TransformAnimator anim;
    anim.setKeyframe(0.0, TransformProperty::ScaleX, 0.0, KeyframePoint::EaseOut);
    anim.setKeyframe(0.0, TransformProperty::ScaleY, 0.0, KeyframePoint::EaseOut);
    anim.setKeyframe(duration, TransformProperty::ScaleX, 1.0);
    anim.setKeyframe(duration, TransformProperty::ScaleY, 1.0);
    // Also fade in for a smoother look
    anim.setKeyframe(0.0, TransformProperty::Opacity, 0.0, KeyframePoint::EaseOut);
    anim.setKeyframe(duration * 0.3, TransformProperty::Opacity, 1.0);
    return anim;
}

TransformAnimator TransformAnimator::createZoomOut(double duration)
{
    TransformAnimator anim;
    anim.setKeyframe(0.0, TransformProperty::ScaleX, 1.0, KeyframePoint::EaseIn);
    anim.setKeyframe(0.0, TransformProperty::ScaleY, 1.0, KeyframePoint::EaseIn);
    anim.setKeyframe(duration, TransformProperty::ScaleX, 0.0);
    anim.setKeyframe(duration, TransformProperty::ScaleY, 0.0);
    // Also fade out
    anim.setKeyframe(duration * 0.7, TransformProperty::Opacity, 1.0, KeyframePoint::EaseIn);
    anim.setKeyframe(duration, TransformProperty::Opacity, 0.0);
    return anim;
}

TransformAnimator TransformAnimator::createSpin(double duration, int rotations)
{
    TransformAnimator anim;
    anim.setKeyframe(0.0, TransformProperty::Rotation, 0.0, KeyframePoint::EaseInOut);
    anim.setKeyframe(duration, TransformProperty::Rotation, 360.0 * rotations);
    return anim;
}

TransformAnimator TransformAnimator::createBounce(double duration)
{
    TransformAnimator anim;
    double d = duration;

    // Start off-screen above, drop down with bounce
    anim.setKeyframe(0.0,         TransformProperty::PositionY, -400.0, KeyframePoint::EaseIn);
    anim.setKeyframe(d * 0.40,    TransformProperty::PositionY,    0.0, KeyframePoint::EaseOut);
    anim.setKeyframe(d * 0.55,    TransformProperty::PositionY, -100.0, KeyframePoint::EaseIn);
    anim.setKeyframe(d * 0.70,    TransformProperty::PositionY,    0.0, KeyframePoint::EaseOut);
    anim.setKeyframe(d * 0.80,    TransformProperty::PositionY,  -30.0, KeyframePoint::EaseIn);
    anim.setKeyframe(d * 0.90,    TransformProperty::PositionY,    0.0, KeyframePoint::EaseOut);
    anim.setKeyframe(d * 0.95,    TransformProperty::PositionY,   -8.0, KeyframePoint::EaseIn);
    anim.setKeyframe(d,           TransformProperty::PositionY,    0.0);

    // Slight squash on impact
    anim.setKeyframe(0.0,         TransformProperty::ScaleY, 1.0, KeyframePoint::Linear);
    anim.setKeyframe(d * 0.38,    TransformProperty::ScaleY, 1.0, KeyframePoint::Linear);
    anim.setKeyframe(d * 0.42,    TransformProperty::ScaleY, 0.85, KeyframePoint::EaseOut);
    anim.setKeyframe(d * 0.50,    TransformProperty::ScaleY, 1.0);

    return anim;
}
