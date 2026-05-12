#pragma once

// ---------------------------------------------------------------------------
// TimeRemap — time-remap curve + frame blend / optical-flow interpolation
// namespace timeremap
//
// Allows mapping output time → source time via a piecewise-linear curve,
// then synthesising the resulting frame via nearest-frame, cross-fade, or
// optical-flow interpolation.
//
// No third-party dependencies — Qt + std + OpticalFlow.h only.
// ---------------------------------------------------------------------------

#include <QImage>
#include <QJsonObject>
#include <QVector>
#include <functional>

namespace timeremap {

// ---------------------------------------------------------------------------
// TimeRemapKey
//
// One keyframe on the time-remap curve.
//   outTime : composition / output time in seconds
//   srcTime : source clip time in seconds
// ---------------------------------------------------------------------------
struct TimeRemapKey {
    double outTime = 0.0;
    double srcTime = 0.0;
};

// ---------------------------------------------------------------------------
// FrameBlendMode
// ---------------------------------------------------------------------------
enum class FrameBlendMode {
    NearestFrame, // round to nearest integer source frame
    Blend,        // linear cross-fade between floor/ceil source frames
    OpticalFlow   // warp via opticalflow::estimateFlow + warpImage
};

// ---------------------------------------------------------------------------
// TimeRemapCurve
//
// Piecewise-linear mapping from output time to source time.
//
// srcTimeAt() edge cases (documented here and in the .cpp):
//   0 keys  → identity: returns outTime unchanged
//   1 key   → constant: always returns that key's srcTime
//   ≥2 keys → piecewise-linear interpolation; clamps to the first/last srcTime
//             outside the key range
// ---------------------------------------------------------------------------
struct TimeRemapCurve {
    QVector<TimeRemapKey> keys;
    FrameBlendMode        blendMode  = FrameBlendMode::NearestFrame;
    double                sourceFps  = 30.0;

    // Insert a key at (outT, srcT) keeping keys sorted ascending by outTime.
    void addKey(double outT, double srcT);

    // Piecewise-linear interpolation (see class doc for edge cases).
    double srcTimeAt(double outTime) const;

    // JSON serialisation (keys, blendMode enum as int, sourceFps).
    QJsonObject toJson() const;
    static TimeRemapCurve fromJson(const QJsonObject& obj);
};

// ---------------------------------------------------------------------------
// resolveFrame
//
// Synthesise the output frame for outTime given a fetch callback.
//
//   fetchFrame(srcFrameIndex) — caller supplies frames indexed by integer
//       source frame number (0-based); may return a null QImage for
//       out-of-range requests; resolveFrame never crashes in that case.
//
// Algorithm:
//   st = curve.srcTimeAt(outTime)
//   f  = st * curve.sourceFps   (fractional source frame index)
//
//   NearestFrame  → fetchFrame( round(f) )
//   Blend         → cross-fade fetchFrame(floor(f)) and fetchFrame(floor(f)+1)
//                   by frac = f - floor(f), per pixel; frac==0 → just floor frame
//   OpticalFlow   → estimateFlow(a, b, FlowParams{}) then warpImage(a, flow, frac)
//
// Null-image fallback: if a frame is null, the non-null neighbour is used;
// if both are null, a 1×1 transparent QImage is returned.
// ---------------------------------------------------------------------------
QImage resolveFrame(const TimeRemapCurve&                      curve,
                    double                                     outTime,
                    const std::function<QImage(int srcFrameIndex)>& fetchFrame);

} // namespace timeremap
