// =============================================================================
//  ExposureAids.h
// -----------------------------------------------------------------------------
//  Preview-only exposure / focus monitoring aids (False Color / Zebra /
//  Focus Peaking), equivalent to the monitoring overlays found in
//  Premiere Pro / DaVinci Resolve.
//
//  Design contract
//  ---------------
//   * PREVIEW ONLY. These transforms are visual monitoring helpers and must
//     NEVER be applied to the export / render pipeline. They intentionally
//     discard or recolor pixel data and are not photometrically reversible.
//   * Pure functions. Depends only on QtGui (QImage / QColor / QRgb). No
//     QApplication / QWidget, no global state, no file or device I/O.
//     Safe to call from headless self-tests.
//   * Deterministic. Given the same (src, mode, cfg) the output is identical
//     bit-for-bit. There is no animation / phase that depends on wall time.
//   * Side-effect free. The input QImage is never mutated; a freshly allocated
//     same-size QImage is returned. Internally the source is normalized to
//     QImage::Format_ARGB32 so any input format is accepted.
//
//  Threading: re-entrant. Operates only on local copies of the input image.
// =============================================================================

#pragma once

#include <QImage>
#include <QColor>

namespace exposureaid {

/// Which monitoring aid to render.
///  - None        : identity (returns a copy of src unchanged).
///  - FalseColor  : map luma into a fixed IRE-zone color palette.
///  - Zebra       : diagonal stripes over pixels above an IRE threshold.
///  - FocusPeaking: highlight high-gradient (in-focus edge) pixels.
enum class AidMode {
    None,
    FalseColor,
    Zebra,
    FocusPeaking,
};

/// Tunable parameters for the aids. Defaults mirror common NLE presets.
struct AidConfig {
    // --- Zebra ---------------------------------------------------------------
    /// Pixels whose luma709*100 (approx. IRE, 0..100) is >= this value get
    /// striped. 95 IRE is a common "near clipping" warning threshold.
    double zebraThresholdIre = 95.0;
    /// Diagonal stripe period in pixels (one bright + one passthrough band).
    int zebraStripePx = 6;

    // --- Focus Peaking -------------------------------------------------------
    /// Normalized gradient magnitude (0..1). Pixels whose local edge strength
    /// exceeds this are tinted with peakingColor.
    double peakingSensitivity = 0.20;
    /// Tint color used to mark in-focus edges.
    QColor peakingColor = QColor(255, 0, 0);
};

/// Rec.709 relative luma from 8-bit channels. Returns 0..1.
///  luma = 0.2126*R + 0.7152*G + 0.0722*B  (channels normalized to 0..1).
/// Public so self-tests can verify the photometric mapping directly.
double luma709(int r, int g, int b);

/// Apply the requested monitoring aid to `src` and return a new same-size
/// QImage (Format_ARGB32). `src` is never modified. mode == None returns an
/// exact copy of `src` (identity). An empty / null `src` returns it unchanged.
QImage apply(const QImage& src, AidMode mode, const AidConfig& cfg);

} // namespace exposureaid
