#pragma once

// US-2111: the stabilizer warp shared by the GL preview and the export. The
// matrix formula lives only here (inverseMatrixUv); GLPreview copies its nine
// values into the uStab uniform, and the export applies the same matrix on the
// CPU through inverseTransform + warpClampToEdge (pixel centres, bilinear,
// clamp-to-edge like the GL texture sampling).

#include "MotionStabilizer.h" // StabilizerKeyframe

#include <QImage>
#include <QTransform>
#include <QVector>
#include <QtGlobal>

#include <array>

struct ClipInfo;

namespace stabwarp {

// One interpolated keyframe value: dx / dy in source pixels, theta in radians.
struct StabSample {
    double dx = 0.0;
    double dy = 0.0;
    double theta = 0.0;
    double scale = 1.0;
};

// The keyframe value at `sourceUs` (source time, microseconds): lerp between the
// two keyframes around it (std::lower_bound), clamped to the first / last outside.
// Identity when `keyframes` is empty.
StabSample sampleAt(const QVector<StabilizerKeyframe> &keyframes, qint64 sourceUs);

// The inverse 2D affine in centred UV space (uv - 0.5), row-major
// [invS*cT, invS*sT, -dx/fw; invS*(-sT), invS*cT, -dy/fh; 0, 0, 1] with
// cT / sT = cos / sin(-theta). A non-positive frame size falls back to 1920x1080.
std::array<float, 9> inverseMatrixUv(const StabSample &sample, int frameW, int frameH);

// True when the sample leaves the frame unchanged (dx / dy in UV, theta and
// scale - 1 all within 1e-6), the GL preview's "stabilizer off" test.
bool isIdentity(const StabSample &sample, int frameW, int frameH);

// inverseMatrixUv in pixel coordinates: output pixel centre (x, y) -> centred UV
// ((x + 0.5) / w - 0.5) -> matrix -> pixel centre ((u + 0.5) * w - 0.5) of the
// source pixel to read.
QTransform inverseTransform(const StabSample &sample, int frameW, int frameH);
// The same in a `dstW` x `dstH` image showing a frame decoded at `srcW` x `srcH`
// scaled to fill it: the UV matrix comes from the source size (dx / dy are source
// pixels), the pixel-centre sandwich from the destination size.
QTransform inverseTransform(const StabSample &sample, int srcW, int srcH, int dstW, int dstH);

// Inverse-maps every output pixel centre through `inverse` and samples `src`
// bilinearly, clamping to the edge pixels (never transparent fill). Same size and
// format as `src`.
QImage warpClampToEdge(const QImage &src, const QTransform &inverse);

// The export stage: warps `frame` (the decoded, brushed source, before FX) by the
// clip's stabilizer keyframes at `sourceSec` when the clip lies on V1
// (`trackIdx` 0) of the top-level sequence (`sequenceDepth` 0) and has keys;
// otherwise, or when the sample is the identity, returns `frame` unchanged.
QImage applyForExport(const QImage &frame, const ClipInfo &clip, int trackIdx,
                      int sequenceDepth, double sourceSec);

} // namespace stabwarp
