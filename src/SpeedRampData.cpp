#include "SpeedRampData.h"

#include <algorithm>
#include <limits>

namespace speedramp {

namespace {

constexpr qint64 kDedupToleranceUs = 1000; // 1 ms

inline double clampSpeed(double s) {
    if (s < SpeedRamp::kMinSpeed) return SpeedRamp::kMinSpeed;
    if (s > SpeedRamp::kMaxSpeed) return SpeedRamp::kMaxSpeed;
    return s;
}

// Locate the segment [i, i+1] that brackets `t` in clipTime. Caller must
// have at least 1 keyframe. Returns the index of the LEFT keyframe of the
// bracketing segment; if t is at or past the last keyframe, returns
// keyframes.size()-1 (i.e. there is no right neighbor).
inline int leftIndex(const QVector<SpeedKeyframe> &kfs, qint64 t) {
    if (t <= kfs.first().clipTimeUs)
        return 0;
    if (t >= kfs.last().clipTimeUs)
        return kfs.size() - 1;
    // Binary search for the largest index with clipTimeUs <= t.
    int lo = 0, hi = kfs.size() - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (kfs[mid].clipTimeUs <= t) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

} // namespace

double SpeedRamp::speedAt(qint64 clipTimeUs) const
{
    if (keyframes.isEmpty())
        return 1.0;
    if (keyframes.size() == 1 || clipTimeUs <= keyframes.first().clipTimeUs)
        return keyframes.first().speed;
    if (clipTimeUs >= keyframes.last().clipTimeUs)
        return keyframes.last().speed;

    const int i = leftIndex(keyframes, clipTimeUs);
    // i+1 must exist because we ruled out clipTimeUs >= last above.
    const SpeedKeyframe &a = keyframes[i];
    const SpeedKeyframe &b = keyframes[i + 1];
    const qint64 span = b.clipTimeUs - a.clipTimeUs;
    if (span <= 0) return a.speed;
    const double frac = static_cast<double>(clipTimeUs - a.clipTimeUs)
                      / static_cast<double>(span);
    return a.speed + (b.speed - a.speed) * frac;
}

qint64 SpeedRamp::timelineToSourceUs(qint64 timelineRelativeUs) const
{
    if (keyframes.isEmpty())
        return timelineRelativeUs;
    if (timelineRelativeUs <= 0)
        return 0;

    // Walk segments forward in TIMELINE space. Within each segment the
    // speed is linear in clip-time; trapezoidal area = exact integral.
    double sourceAccum = 0.0;
    const qint64 t = timelineRelativeUs;

    for (int i = 0; i + 1 < keyframes.size(); ++i) {
        const SpeedKeyframe &a = keyframes[i];
        const SpeedKeyframe &b = keyframes[i + 1];
        const qint64 segStart = a.clipTimeUs;
        const qint64 segEnd   = b.clipTimeUs;
        if (segEnd <= segStart) continue; // degenerate, skip
        if (t <= segStart) {
            // Target lies before this segment — no contribution.
            return static_cast<qint64>(sourceAccum);
        }
        if (t >= segEnd) {
            // Whole segment contributes: trapezoidal area = (b-a) * (sa+sb)/2.
            const double dt = static_cast<double>(segEnd - segStart);
            sourceAccum += dt * (a.speed + b.speed) * 0.5;
        } else {
            // Partial segment ending at t. Speed at t = lerp(a.speed,b.speed)
            // by frac = (t-a)/(b-a). Source advance over [a, t] = (t-a) *
            // (a.speed + speed_at_t)/2 (still trapezoidal — exact for
            // linear speed).
            const double dt = static_cast<double>(t - segStart);
            const double frac = dt / static_cast<double>(segEnd - segStart);
            const double sAtT = a.speed + (b.speed - a.speed) * frac;
            sourceAccum += dt * (a.speed + sAtT) * 0.5;
            return static_cast<qint64>(sourceAccum);
        }
    }

    // Past the last keyframe: speed is held at the last keyframe's value.
    const qint64 lastKf = keyframes.last().clipTimeUs;
    if (t > lastKf) {
        sourceAccum += static_cast<double>(t - lastKf) * keyframes.last().speed;
    }
    return static_cast<qint64>(sourceAccum);
}

qint64 SpeedRamp::sourceToTimelineUs(qint64 sourceRelativeUs) const
{
    if (keyframes.isEmpty())
        return sourceRelativeUs;
    if (sourceRelativeUs <= 0)
        return 0;

    // Walk segments forward in TIMELINE space, accumulating both timeline
    // and source advance. When the segment's source-advance would overshoot
    // the target, solve a quadratic for the partial step.
    //
    // Within a segment of width dt = (b.t - a.t), speed(τ) = sa + (sb-sa)*τ/dt
    // for τ ∈ [0, dt]. Source advance over [0, x] is:
    //   S(x) = sa*x + (sb-sa)*x²/(2*dt)
    // We want S(x) = remaining (the still-needed source advance).
    //
    // Letting m = (sb-sa)/(2*dt):
    //   m*x² + sa*x - remaining = 0
    // Solve via the standard quadratic formula. When m == 0 (constant speed)
    // this collapses to x = remaining / sa. When sa == 0 and m > 0 we get
    // x = sqrt(remaining/m). Speeds are clamped >= kMinSpeed > 0 so sa > 0
    // is the typical fast path.
    //
    // After the last keyframe, speed is held constant at last.speed, so
    // remaining timeline = remaining_source / last.speed.

    double sourceAccum = 0.0;
    double timelineAccum = 0.0;
    const double target = static_cast<double>(sourceRelativeUs);

    for (int i = 0; i + 1 < keyframes.size(); ++i) {
        const SpeedKeyframe &a = keyframes[i];
        const SpeedKeyframe &b = keyframes[i + 1];
        const qint64 segStart = a.clipTimeUs;
        const qint64 segEnd   = b.clipTimeUs;
        if (segEnd <= segStart) continue;

        const double dt = static_cast<double>(segEnd - segStart);
        const double segSourceAdvance = dt * (a.speed + b.speed) * 0.5;

        if (sourceAccum + segSourceAdvance < target) {
            // Whole segment fits; advance and continue.
            sourceAccum  += segSourceAdvance;
            timelineAccum = static_cast<double>(segEnd);
            continue;
        }

        // Partial step inside this segment. Solve for x in [0, dt] s.t.
        // segment-source-from-a(x) = target - sourceAccum.
        const double remaining = target - sourceAccum;
        const double sa = a.speed;
        const double sb = b.speed;
        const double m  = (sb - sa) / (2.0 * dt);
        double x = 0.0;
        if (std::abs(m) < 1e-15) {
            // constant speed sa
            x = (sa > 1e-15) ? remaining / sa : 0.0;
        } else {
            // m*x² + sa*x - remaining = 0
            const double disc = sa * sa + 4.0 * m * remaining;
            const double sq = std::sqrt(std::max(0.0, disc));
            // Pick the positive root.
            const double r1 = (-sa + sq) / (2.0 * m);
            const double r2 = (-sa - sq) / (2.0 * m);
            if (r1 >= 0.0 && r1 <= dt) x = r1;
            else if (r2 >= 0.0 && r2 <= dt) x = r2;
            else x = std::max(0.0, std::min(dt, std::max(r1, r2)));
        }
        return static_cast<qint64>(timelineAccum + x);
    }

    // Past the last keyframe: hold last.speed constant.
    const SpeedKeyframe &last = keyframes.last();
    timelineAccum = static_cast<double>(last.clipTimeUs);
    const double remaining = target - sourceAccum;
    if (remaining > 0.0) {
        const double s = (last.speed > 1e-15) ? last.speed : 1.0;
        timelineAccum += remaining / s;
    }
    return static_cast<qint64>(timelineAccum);
}

qint64 SpeedRamp::outputDuration(qint64 sourceDuration) const
{
    if (sourceDuration <= 0) return 0;
    return sourceToTimelineUs(sourceDuration);
}

namespace {

// timelineToSourceUs in double-precision seconds (same segment rules: no
// source advance before the first keyframe, last speed held after the last).
double sourceSecondsAt(const QVector<SpeedKeyframe> &kfs, double tauSec)
{
    if (kfs.isEmpty())
        return tauSec;
    if (tauSec <= 0.0)
        return 0.0;
    double sourceAccum = 0.0;
    for (int i = 0; i + 1 < kfs.size(); ++i) {
        const SpeedKeyframe &a = kfs[i];
        const SpeedKeyframe &b = kfs[i + 1];
        if (b.clipTimeUs <= a.clipTimeUs)
            continue;
        const double segStart = static_cast<double>(a.clipTimeUs) / 1'000'000.0;
        const double segEnd = static_cast<double>(b.clipTimeUs) / 1'000'000.0;
        if (tauSec <= segStart)
            return sourceAccum;
        if (tauSec >= segEnd) {
            sourceAccum += (segEnd - segStart) * (a.speed + b.speed) * 0.5;
        } else {
            const double dt = tauSec - segStart;
            const double sAtT = a.speed + (b.speed - a.speed) * dt / (segEnd - segStart);
            return sourceAccum + dt * (a.speed + sAtT) * 0.5;
        }
    }
    const double lastSec = static_cast<double>(kfs.last().clipTimeUs) / 1'000'000.0;
    if (tauSec > lastSec)
        sourceAccum += (tauSec - lastSec) * kfs.last().speed;
    return sourceAccum;
}

// First ramp time (seconds) at which sourceSecondsAt reaches limitSec, or
// +inf when it never does. Inside a segment the source is quadratic in tau
// and is solved in the stable form u = 2R / (v + sqrt(v^2 + 2aR)).
double saturationTauSec(const QVector<SpeedKeyframe> &kfs, double limitSec)
{
    if (kfs.isEmpty())
        return limitSec;
    if (limitSec <= 0.0)
        return 0.0;
    double sourceAccum = 0.0;
    for (int i = 0; i + 1 < kfs.size(); ++i) {
        const SpeedKeyframe &a = kfs[i];
        const SpeedKeyframe &b = kfs[i + 1];
        if (b.clipTimeUs <= a.clipTimeUs)
            continue;
        const double segStart = static_cast<double>(a.clipTimeUs) / 1'000'000.0;
        const double segEnd = static_cast<double>(b.clipTimeUs) / 1'000'000.0;
        const double advance = (segEnd - segStart) * (a.speed + b.speed) * 0.5;
        if (sourceAccum + advance >= limitSec) {
            const double remaining = limitSec - sourceAccum;
            const double accel = (b.speed - a.speed) / (segEnd - segStart);
            const double denom = a.speed
                + std::sqrt(std::max(0.0, a.speed * a.speed + 2.0 * accel * remaining));
            const double u = denom > 0.0 ? 2.0 * remaining / denom : segEnd - segStart;
            return segStart + std::min(u, segEnd - segStart);
        }
        sourceAccum += advance;
    }
    const double lastSec = static_cast<double>(kfs.last().clipTimeUs) / 1'000'000.0;
    const double lastSpeed = kfs.last().speed;
    if (lastSpeed <= 0.0)
        return std::numeric_limits<double>::infinity();
    return lastSec + (limitSec - sourceAccum) / lastSpeed;
}

} // namespace

bool SpeedRamp::sliced(qint64 fromUs, qint64 toUs, double timeScale, double speedScale,
                       SpeedRamp *out) const
{
    if (!out || keyframes.isEmpty() || toUs <= fromUs || !(timeScale > 0.0)
        || !(speedScale > 0.0)) {
        return false;
    }
    if (fromUs < keyframes.first().clipTimeUs)
        return false;

    constexpr double kSpeedTolerance = 1e-9;
    SpeedRamp piece;
    double minSpeed = std::numeric_limits<double>::infinity();
    double maxSpeed = -std::numeric_limits<double>::infinity();
    const auto append = [&](qint64 clipTimeUs, double speed) {
        minSpeed = std::min(minSpeed, speed);
        maxSpeed = std::max(maxSpeed, speed);
        const double pieceSpeed = speed * speedScale;
        if (pieceSpeed < kMinSpeed - kSpeedTolerance || pieceSpeed > kMaxSpeed + kSpeedTolerance)
            return false;
        const qint64 pieceUs = qRound64(static_cast<double>(clipTimeUs - fromUs) * timeScale);
        if (piece.keyframes.isEmpty() || pieceUs > piece.keyframes.last().clipTimeUs)
            piece.keyframes.append({pieceUs, clampSpeed(pieceSpeed)});
        return true;
    };
    if (!append(fromUs, speedAt(fromUs)))
        return false;
    for (const SpeedKeyframe &kf : keyframes) {
        if (kf.clipTimeUs > fromUs && kf.clipTimeUs < toUs && !append(kf.clipTimeUs, kf.speed))
            return false;
    }
    if (!append(toUs, speedAt(toUs)))
        return false;
    // A ramp that varies on the piece must not collapse to identity, which
    // consumers evaluate as uniform speed and would drop the curvature.
    if (maxSpeed - minSpeed > kSpeedTolerance && piece.isIdentity())
        return false;
    *out = piece;
    return true;
}

QVector<SpeedRamp::CurvePoint> SpeedRamp::approximateSourceCurve(
    double fromSec, double toSec, double speed, double limitSec, double *epsilonOut) const
{
    constexpr int kMaxKeys = 4096;
    double epsilon = 1e-5;
    QVector<CurvePoint> points;
    if (epsilonOut)
        *epsilonOut = epsilon;
    if (!(toSec > fromSec) || !(speed > 0.0))
        return points;

    const auto sourceAt = [&](double localSec) {
        return std::min(sourceSecondsAt(keyframes, localSec * speed), limitSec);
    };
    const auto speedAtLocal = [&](double localSec) {
        return speedAt(qRound64(localSec * speed * 1'000'000.0));
    };

    // Span breaks: both ends, every keyframe and the saturation time t*.
    QVector<double> breaks{fromSec, toSec};
    for (const SpeedKeyframe &kf : keyframes) {
        const double localSec = static_cast<double>(kf.clipTimeUs) / 1'000'000.0 / speed;
        if (localSec > fromSec && localSec < toSec)
            breaks.append(localSec);
    }
    const double saturationSec = saturationTauSec(keyframes, limitSec) / speed;
    if (saturationSec > fromSec && saturationSec < toSec)
        breaks.append(saturationSec);
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());

    // speed * |dv| * dt per span. Spans before the first keyframe (no source
    // advance) and after t* (held at limitSec) are linear: 0.
    const double firstKeySec = keyframes.isEmpty()
        ? 0.0
        : static_cast<double>(keyframes.first().clipTimeUs) / 1'000'000.0 / speed;
    QVector<double> spanCurvature;
    for (int i = 0; i + 1 < breaks.size(); ++i) {
        const double t0 = breaks[i];
        const double t1 = breaks[i + 1];
        if (keyframes.isEmpty() || t1 <= firstKeySec || t0 >= saturationSec) {
            spanCurvature.append(0.0);
            continue;
        }
        const double dv = std::abs(speedAtLocal(t1) - speedAtLocal(t0));
        spanCurvature.append(speed * dv * (t1 - t0));
    }

    QVector<int> spanCounts(spanCurvature.size(), 1);
    for (;;) {
        double total = 1.0;
        bool anySubdivided = false;
        for (int i = 0; i < spanCurvature.size(); ++i) {
            const double n = std::max(1.0, std::ceil(std::sqrt(spanCurvature[i] / (8.0 * epsilon))));
            anySubdivided = anySubdivided || n > 1.0;
            total += n;
            spanCounts[i] = n > kMaxKeys ? kMaxKeys + 1 : static_cast<int>(n);
        }
        if (total <= kMaxKeys || !anySubdivided)
            break;
        epsilon *= 2.0;
    }
    if (epsilonOut)
        *epsilonOut = epsilon;

    for (int i = 0; i + 1 < breaks.size(); ++i) {
        const double t0 = breaks[i];
        const double t1 = breaks[i + 1];
        const int n = spanCounts[i];
        for (int j = 0; j < n; ++j) {
            const double localSec = t0 + (t1 - t0) * static_cast<double>(j) / n;
            points.append({localSec - fromSec, sourceAt(localSec)});
        }
    }
    points.append({toSec - fromSec, sourceAt(toSec)});
    return points;
}

void SpeedRamp::addKeyframe(qint64 clipTimeUs, double speed)
{
    const double s = clampSpeed(speed);

    // Replace if a keyframe already lives within 1 ms.
    for (int i = 0; i < keyframes.size(); ++i) {
        if (std::abs(keyframes[i].clipTimeUs - clipTimeUs) <= kDedupToleranceUs) {
            keyframes[i].clipTimeUs = clipTimeUs;
            keyframes[i].speed = s;
            // Re-sort defensively in case clipTimeUs was nudged.
            std::sort(keyframes.begin(), keyframes.end(),
                      [](const SpeedKeyframe &x, const SpeedKeyframe &y) {
                          return x.clipTimeUs < y.clipTimeUs;
                      });
            return;
        }
    }

    keyframes.append({clipTimeUs, s});
    std::sort(keyframes.begin(), keyframes.end(),
              [](const SpeedKeyframe &x, const SpeedKeyframe &y) {
                  return x.clipTimeUs < y.clipTimeUs;
              });
}

void SpeedRamp::removeKeyframe(qint64 clipTimeUs)
{
    for (int i = 0; i < keyframes.size(); ++i) {
        if (std::abs(keyframes[i].clipTimeUs - clipTimeUs) <= kDedupToleranceUs) {
            keyframes.removeAt(i);
            break;
        }
    }
    if (keyframes.isEmpty())
        clearAndSetIdentity();
}

void SpeedRamp::clearAndSetIdentity()
{
    keyframes.clear();
    keyframes.append({0, 1.0});
}

QJsonObject SpeedRamp::toJson() const
{
    QJsonObject root;
    QJsonArray arr;
    for (const auto &kf : keyframes) {
        QJsonObject o;
        o["t"] = static_cast<double>(kf.clipTimeUs);
        o["s"] = kf.speed;
        arr.append(o);
    }
    root["keyframes"] = arr;
    return root;
}

SpeedRamp SpeedRamp::fromJson(const QJsonObject &o)
{
    SpeedRamp r;
    const QJsonArray arr = o.value("keyframes").toArray();
    r.keyframes.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject kfo = v.toObject();
        SpeedKeyframe kf;
        kf.clipTimeUs = static_cast<qint64>(kfo.value("t").toDouble(0.0));
        kf.speed = clampSpeed(kfo.value("s").toDouble(1.0));
        r.keyframes.append(kf);
    }
    if (r.keyframes.isEmpty())
        r.keyframes.append({0, 1.0});
    else
        std::sort(r.keyframes.begin(), r.keyframes.end(),
                  [](const SpeedKeyframe &x, const SpeedKeyframe &y) {
                      return x.clipTimeUs < y.clipTimeUs;
                  });
    return r;
}

} // namespace speedramp
