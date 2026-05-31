#pragma once

// HdrCompositeMath — pure 16-bit-per-channel compositing math.
//
// This is the CPU REFERENCE ORACLE for a future GPU RGBA16 compositor.
// There is NO wiring: nothing in the live playback/export path calls this yet,
// so adding it is bit-identical to before. It exists so the (later) GPU RGBA16
// offscreen compositor can be measured against a deterministic CPU result, the
// same way GpuCompositeMath (8-bit) is the oracle for the 8-bit GPU compositor.
//
// 16-bit precision is preserved end-to-end via QImage::Format_RGBA64_Premultiplied
// (one quint16 per channel, premultiplied alpha). Geometry (paint order and the
// per-layer transform) is bit-depth independent, so we REUSE gpucomposite's
// paintOrder() / layerTransform() / clampOpacity() rather than re-implementing
// them — keeping a single source of truth with the 8-bit path.
//
// Dependencies: QtCore + QtGui only (QImage / QTransform / QSize / QColor).
// NO QOpenGL, NO QApplication. Pure functions — headlessly testable.
//
// Matte handling: OUT OF SCOPE here (matte-free only, mirroring the 8-bit
// Stage 2 compositor). Layers whose matteType != MatteType::None are treated as plain
// (non-matte) layers — the matte source/target relationship is ignored. A
// 16-bit matte fragment (Stage4A analogue) is a later phase.

#include "GpuCompositeMath.h"

#include <QImage>
#include <QSize>
#include <QVector>
#include <cstdint>

namespace hdrcomposite {

// Maximum value of a 16-bit channel (Format_RGBA64 uses full 0..65535 range).
constexpr quint32 kMax16 = 65535u;

// Premultiplied source-over for one 16-bit RGBA pixel (channels 0..65535).
// Same algebra as the 8-bit gpucomposite::premulSourceOver, scaled to MAX=65535:
//   out = src + dst * (1 - srcA / MAX)
// src and dst are already premultiplied. Intermediate products use quint32/quint64
// to avoid overflow (dst*inv can reach 65535*65535 ≈ 4.29e9 > UINT32_MAX, so the
// channel product is computed in quint64), then rounded.
struct Rgba16 { quint16 r, g, b, a; };
Rgba16 premulSourceOver16(Rgba16 dst, Rgba16 src);

// Composites matte-free layers onto a transparent RGBA64-premultiplied canvas.
//   - canvas-sized QImage::Format_RGBA64_Premultiplied, cleared to (0,0,0,0).
//   - layers are ordered via gpucomposite::paintOrder (lower sourceTrack first).
//   - each layer is placed via gpucomposite::layerTransform (canvas pixel space),
//     sampled with GL_NEAREST-equivalent nearest-neighbour inverse mapping.
//   - opacity is gpucomposite::clampOpacity()'d, then multiplied into ALL src
//     channels (premultiplied, so colour and alpha are scaled together) before
//     premulSourceOver16.
//   - desc.matteType != MatteType::None layers are composited as PLAIN layers
//     (matte-free scope; matte relationship intentionally ignored here).
// `images` must be index-aligned with `layers`. Null/empty images are skipped.
// Returns the composited Format_RGBA64_Premultiplied image.
QImage compositeReference16(const QVector<gpucomposite::LayerDesc>& layers,
                           const QVector<QImage>& images,
                           QSize canvas);

// Converts an RGBA64(_Premultiplied) image to ARGB32_Premultiplied (8-bit) so a
// selftest can compare 16-bit reference output against the 8-bit CPU SSOT.
QImage to8bit(const QImage& rgba64);

} // namespace hdrcomposite
