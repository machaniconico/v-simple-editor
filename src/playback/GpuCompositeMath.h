#pragma once

#include <QSize>
#include <QPointF>
#include <QVector>
#include <QMatrix4x4>

// gpucomposite: PURE compositing-math engine for the future GPU multi-track
// compositor (Stage 1 — dormant, NOT wired into any rendering path yet).
//
// Purpose
// -------
// This module captures the *compositing rules* used by the existing CPU
// preview/export pipeline so they can be verified HEADLESS (no QApplication,
// no QWidget, no QOpenGL) via deterministic self-tests, ahead of an actual
// GPU implementation. Behaviour must stay BIT-IDENTICAL to the current
// pipeline: this file introduces no new policy, only mirrors the canonical
// math already proven by:
//   - clipgeom::layerMatrix / placementAnchor (src/ClipGeometry.cpp)
//   - clipstack::layerPaintOrderLess          (src/VideoPlayer.cpp)
//   - trackmatte::composite / MatteType       (src/TrackMatteBake.cpp,
//                                               src/TrackMatteKey.h)
//
// Constraints
// -----------
//   * Math types only (QSize, QPointF, QVector, QMatrix4x4). No QImage,
//     no QOpenGL*, no QApplication/QWidget.
//   * Fully deterministic, side-effect free.
namespace gpucomposite {

// MatteType: a math-only MIRROR of the project's TrackMatteType
// (src/MaskSystem.h), used by trackmatte::composite. This public header and
// implementation do not include MaskSystem.h because it pulls in heavier
// UI/QImage dependencies; the headless selftest carries compile-time ordinal
// drift guards against the canonical enum.
//
// Contract honoured here:
//   - `None` is the canonical "no matte" sentinel (== TrackMatteType::None,
//     the default used throughout the codebase, e.g. LayerCompositor.h).
//     LayerDesc::matteType defaults to None and the Stage-1 self-tests treat
//     None as "composite normally, no matte consumption".
// The named variants below cover the alpha / luminance (and inverted) matte
// kinds. Their integer ordinals intentionally match TrackMatteType and are
// guarded by static_asserts in the headless selftest so a future wiring step
// cannot silently reinterpret project matte values.
enum class MatteType {
    None = 0,           // == TrackMatteType::None
    Alpha,              // == TrackMatteType::AlphaMatte
    AlphaInverted,      // == TrackMatteType::AlphaInvertedMatte
    Luminance,          // == TrackMatteType::LumaMatte
    LuminanceInverted   // == TrackMatteType::LumaInvertedMatte
};

// LayerDesc: a single decoded video track contributing to the composite.
// Field semantics mirror VideoPlayer's DecodedLayer (sans the QImage frame,
// represented here only by its dimensions in `srcSize`).
struct LayerDesc {
    int       sourceTrack = 0;                    // timeline track index
    QSize     srcSize;                            // decoded frame size (empty == null/absent)
    double    opacity = 1.0;                      // 0..1
    double    videoScale = 1.0;                   // uniform scale
    double    videoDx = 0.0;                      // normalized canvas-relative X offset
    double    videoDy = 0.0;                      // normalized canvas-relative Y offset
    double    rotation2DDegrees = 0.0;            // 2D rotation about layer center
    bool      visible = true;
    int       matteSourceIndex = -1;             // -1 == no matte
    MatteType matteType = MatteType::None;
};

// RGBAf: a premultiplied-alpha RGBA color, each component in [0,1].
struct RGBAf {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

// (1) Paint order. Returns layer indices sorted DESCENDING by sourceTrack,
//     matching clipstack::layerPaintOrderLess (a.sourceTrack > b.sourceTrack).
//     Stable: layers sharing a sourceTrack keep their original relative order.
//     Painted back-to-front, so the LOWEST sourceTrack (== V1) ends up frontmost
//     (V1-wins, commit 47416d7).
QVector<int> paintOrder(const QVector<LayerDesc>& layers);

// (2) Per-layer transform. Maps native source-pixel coordinates onto the canvas
//     using the canonical clipgeom order, including renderLayer's defensive
//     source->canvas pre-scale (Qt::IgnoreAspectRatio):
//        1. scale native source to canvas dimensions
//        2. un-center the canvas-sized source (translate by -canvas/2)
//        3. scale about origin (canvas center after step 5)
//        4. rotate about origin
//        5. translate to placement anchor (canvas center + normalized offset)
//     Returned as a single QMatrix4x4 (post-multiplied / reverse build order),
//     ready to map a native source quad. scale=1, dx=dy=0, rot=0 maps the source
//     rectangle to the canvas rectangle for any non-empty srcSize.
QMatrix4x4 layerTransform(const LayerDesc& layer, QSize canvas);

// (3) Composite eligibility: visible AND srcSize non-empty AND opacity > 0.001.
bool isLayerComposited(const LayerDesc& layer);

// (4) Clamp opacity into [0,1].
double clampOpacity(double o);

// (5) Premultiplied "source over" compositing algebra:
//        out.rgb = src.rgb + dst.rgb * (1 - src.a)
//        out.a   = src.a   + dst.a   * (1 - src.a)
//     Inputs/outputs are premultiplied (each component in [0,1]).
RGBAf premulSourceOver(const RGBAf& src, const RGBAf& dst);

// (6) Validate a matte source index: idx must reference a real layer above the
//     reserved index-0 V1 base, i.e. idx > 0 && idx < count. Mirrors
//     trackmatte::isValidMatteSource.
bool isValidMatteSource(int idx, int count);

} // namespace gpucomposite
