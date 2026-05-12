#pragma once

#include <QImage>
#include <QPointF>
#include <QVector>

// ---------------------------------------------------------------------------
// OpticalFlow — dense optical flow estimation (namespace opticalflow)
//
// Pyramidal block-matching (SAD) with optional smoothing.
// No third-party dependencies — Qt + std only.
// ---------------------------------------------------------------------------

namespace opticalflow {

// ---------------------------------------------------------------------------
// FlowField
//
// Dense flow field: one QPointF per pixel (row-major, width*height).
// v[y*width + x] = (dx, dy) such that dst_pixel = src_pixel + (dx, dy).
// ---------------------------------------------------------------------------
struct FlowField {
    int width  = 0;
    int height = 0;
    QVector<QPointF> v;   // size = width * height; empty when invalid

    // Returns flow vector at (x, y).
    // Clamps x/y to [0, width-1] / [0, height-1].
    // Returns QPointF(0,0) if the field is empty or sizes are zero.
    QPointF at(int x, int y) const;
};

// ---------------------------------------------------------------------------
// FlowParams
// ---------------------------------------------------------------------------
struct FlowParams {
    int  levels      = 3;   // pyramid levels (coarse→fine)
    int  blockSize   = 16;  // block size in pixels at full resolution
    int  searchRange = 16;  // ±pixels to search at each pyramid level
    bool smooth      = true; // apply one 3×3 average smoothing pass at full res
};

// ---------------------------------------------------------------------------
// estimateFlow
//
// Pyramidal block-matching optical flow from image a → image b.
// If a and b have different sizes, b is scaled to a's size before processing.
// Returns an empty FlowField on null / zero-size input.
// Identical images → mean magnitude < 1 px.
// Pure (dx,dy) shift within searchRange * 2^(levels-1) → median ≈ (dx,dy) ±~2px.
// ---------------------------------------------------------------------------
FlowField estimateFlow(const QImage& a, const QImage& b,
                       const FlowParams& p = FlowParams{});

// ---------------------------------------------------------------------------
// warpImage
//
// Warp src by flow * t using bilinear interpolation (edge-clamp).
// output(x,y) = src at (x + t*flow.at(x,y).x,  y + t*flow.at(x,y).y).
// t=0.0 → output identical to src.
// Returns null QImage on empty src or mismatched flow.
// ---------------------------------------------------------------------------
QImage warpImage(const QImage& src, const FlowField& flow, double t = 1.0);

// ---------------------------------------------------------------------------
// flowToColor
//
// HSV visualisation: hue = atan2(vy,vx) mapped to [0,360),
// saturation = 1, value = min(1, magnitude/maxMag).
// Returns same-size QImage (Format_RGB32), deterministic.
// Returns null QImage if flow is empty.
// ---------------------------------------------------------------------------
QImage flowToColor(const FlowField& flow);

} // namespace opticalflow
