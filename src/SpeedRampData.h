#pragma once

// Per-clip variable playback speed via a curve of (clipTimeUs, speed)
// keyframes. Between keyframes the speed is linearly interpolated; outside
// the last keyframe the curve holds the last keyframe's speed.
//
// Producer: Timeline (per-clip storage, JSON round-trip via ProjectFile).
// Consumers (US-INT-2 Phase A + B):
//   - VideoPlayer::entryLocalPositionUs   — timeline → source forward map
//   - VideoPlayer::fileLocalToTimelineUs  — source → timeline inverse map
//   - AudioMixer  per-fragment atempo (envvar VEDITOR_AUDIO_ATEMPO=1)
//
// The struct lives in `namespace speedramp` because there is already a
// `class SpeedRamp` in src/SpeedRamp.h that drives FFmpeg-based offline
// re-encode. The two are unrelated: this header is the in-memory curve,
// the existing class is an export pipeline.

#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <QtGlobal>
#include <cmath>

namespace speedramp {

struct SpeedKeyframe {
    qint64 clipTimeUs = 0;   // time within the clip (relative to clip start)
    double speed = 1.0;      // 0.1 .. 10.0 multiplier

    bool operator==(const SpeedKeyframe &other) const {
        return clipTimeUs == other.clipTimeUs
            && std::abs(speed - other.speed) < 1e-9;
    }
};

struct SpeedRamp {
    // Sorted by clipTimeUs ascending. Always at least 1 entry after any
    // mutating operation (identity / addKeyframe / removeKeyframe /
    // clearAndSetIdentity). speed is clamped to [0.1, 10.0] on insert.
    QVector<SpeedKeyframe> keyframes;

    static constexpr double kMinSpeed = 0.1;
    static constexpr double kMaxSpeed = 10.0;

    // Identity ramp: a single keyframe at clip-start with speed 1.0,
    // i.e. no time remap.
    static SpeedRamp identity() {
        SpeedRamp r;
        r.keyframes.append({0, 1.0});
        return r;
    }

    // True iff every keyframe sits at speed 1.0 (within tight epsilon).
    // Consumers can early-out the speed-curve evaluation when this is true.
    bool isIdentity() const {
        if (keyframes.isEmpty())
            return true;
        for (const auto &kf : keyframes) {
            if (std::abs(kf.speed - 1.0) > 1e-6)
                return false;
        }
        return true;
    }

    // ---- CORE API ----

    // Sample the speed at a clip-relative time using linear interpolation
    // between bracketing keyframes. Before the first keyframe → first
    // keyframe's speed. After the last → last keyframe's speed.
    double speedAt(qint64 clipTimeUs) const;

    // Given a clip-relative timeline-output time, return the corresponding
    // source-frame time. Integrates the speed curve via per-segment
    // trapezoidal area (exact for piecewise-linear speed).
    //   speed = 2.0  → source advances 2x as fast as timeline
    //   speed = 0.5  → source advances 0.5x as fast as timeline
    qint64 timelineToSourceUs(qint64 timelineRelativeUs) const;

    // Inverse of timelineToSourceUs. Walks segments forward in source
    // accumulating timeline advance until the target source time is hit.
    // Within each segment a linear-speed interpolation gives a quadratic
    // in t so the per-segment timeline-step is solved analytically (closed
    // form below); no bisection needed.
    qint64 sourceToTimelineUs(qint64 sourceRelativeUs) const;

    // Total output (timeline) duration that a source clip of length
    // sourceDuration produces under this ramp. Equivalent to
    // sourceToTimelineUs(sourceDuration) + holds-trailing-speed.
    qint64 outputDuration(qint64 sourceDuration) const;

    // ---- split pieces (US-1501) ----

    // The ramp of one split piece that covers ramp time [fromUs, toUs] of
    // this ramp. Piece time is tau' = (tau - fromUs) * timeScale and piece
    // speeds are v' = v * speedScale, so the piece consumes the same source
    // as this ramp over [fromUs, toUs] starting from 0. Keyframes are built
    // directly (no 1 ms de-duplication): a key at piece time 0, the keys
    // inside the range and a key at the piece end. Returns false (and leaves
    // *out alone) when a piece speed falls outside [kMinSpeed, kMaxSpeed] or
    // fromUs lies before the first keyframe (the source does not advance
    // there, which no speed in range can express).
    bool sliced(qint64 fromUs, qint64 toUs, double timeScale, double speedScale,
                SpeedRamp *out) const;

    struct CurvePoint {
        double localSec = 0.0;   // seconds from the start of the range
        double sourceSec = 0.0;  // min(source(tau), limitSec), in-relative
    };

    // Piecewise-linear approximation of g(t) = min(source(t * speed),
    // limitSec) over clip-local seconds [fromSec, toSec] (source in
    // seconds, evaluated in double precision). Keys sit at both ends, at
    // every keyframe and at the time g first reaches limitSec; each span
    // between them is cut into n = max(1, ceil(sqrt(speed * |dv| * dt /
    // (8 * eps)))) equal parts so the interpolation error stays <= eps.
    // eps starts at 1e-5 s and doubles while the curve has more than 4096
    // keys, stopping once every span has n = 1. *epsilonOut receives the
    // final eps.
    QVector<CurvePoint> approximateSourceCurve(double fromSec, double toSec,
                                               double speed, double limitSec,
                                               double *epsilonOut = nullptr) const;

    // ---- mutators ----

    // Insert a new keyframe (or replace if one already exists within 1 ms
    // of clipTimeUs). speed is clamped to [kMinSpeed, kMaxSpeed]. Keeps
    // the vector sorted and de-duplicated.
    void addKeyframe(qint64 clipTimeUs, double speed);

    // Remove the first keyframe within 1 ms tolerance. Always preserves at
    // least one keyframe — if removal would empty the vector, the curve
    // is reset to identity().
    void removeKeyframe(qint64 clipTimeUs);

    // Reset to the single-keyframe identity ramp.
    void clearAndSetIdentity();

    // ---- JSON ----

    QJsonObject toJson() const;
    static SpeedRamp fromJson(const QJsonObject &o);

    bool operator==(const SpeedRamp &other) const {
        return keyframes == other.keyframes;
    }
};

} // namespace speedramp
