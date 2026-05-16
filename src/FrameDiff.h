#pragma once

#include <QImage>
#include <cmath>

// framediff: pixel-level frame comparison primitives used by the parity
// selftest harness to prove exported frames match the preview output.
namespace framediff {

// Mean squared error across all 4 channels (R,G,B,A) of every pixel.
// Both images are converted to QImage::Format_RGBA8888 internally.
// Returns -1.0 (sentinel) when the dimensions differ.
double mse(const QImage &a, const QImage &b);

// Peak signal-to-noise ratio derived from mse():
//   - mse < 0      -> -1.0 (dimension mismatch propagated)
//   - mse == 0.0   -> 1e9  (identical frames)
//   - otherwise    -> 10 * log10(255^2 / mse)
double psnr(const QImage &a, const QImage &b);

} // namespace framediff
