#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QTransform>

namespace clipgeom {

// CANONICAL CLIP-PLACEMENT CONTRACT (single source of truth).
// videoDx is a NORMALIZED fraction of canvas WIDTH; videoDy a NORMALIZED
// fraction of canvas HEIGHT (NOT pixels).
//
// Anchor = LAYER CENTER: the layer's center maps to the placement point
//   (canvasW*0.5 + videoDx*canvasW, canvasH*0.5 + videoDy*canvasH).
// Scale and rotation pivot about the layer center.
//
// src MAY be any size: renderLayer scales it to canvasSize internally
// (Qt::IgnoreAspectRatio, smooth-aware) before applying the transform,
// so the layer-center anchor and identity-fill contract hold for any
// input resolution (e.g. native 1920×1080 src into a 640×360 canvas).
// resolveTransform always operates in canvas coordinates.
//
// Identity (scale=1, dx=dy=rot=0) => src scaled to canvas, fills it exactly.
//
// Internal op order (Qt post-multiplies; last call = first point-transform):
//   translate(placement point)       [applied 4th]
//   rotate(rotationDeg)              [applied 3rd — pivot = placement point]
//   scale(videoScale)                [applied 2nd — about placement point]
//   translate(-canvasW/2,-canvasH/2) [applied 1st — un-centers canvas-sized source]
//
// Future readers/adopters MUST NOT reintroduce a pixel-vs-normalized
// offset divergence, drop the un-centering translate, or omit the rotation.
struct ClipTransform {
    double videoScale  = 1.0;
    double videoDx     = 0.0;   // CANONICAL: normalized fraction of canvas WIDTH
    double videoDy     = 0.0;   // CANONICAL: normalized fraction of canvas HEIGHT
    double rotationDeg = 0.0;   // == ClipInfo::rotation2DDegrees
};

// Returns the QTransform that maps a canvas-sized source drawn at (0,0)
// into its placed position, anchored at the canvas center, applying
// translate -> rotate -> scale in that exact order.
QTransform resolveTransform(const ClipTransform& t, QSize canvasSize);

ClipTransform composeParented(const ClipTransform& child,
                              const ClipTransform& parent,
                              QSize canvasSize);

QString nullObjectFilePath();
bool isNullObjectFilePath(const QString& filePath);

// Returns a QImage(canvasSize, Format_ARGB32_Premultiplied) filled
// Qt::transparent with `src` drawn through resolveTransform(). `src` may be
// any size; it is scaled to canvasSize internally (Qt::IgnoreAspectRatio,
// smooth-aware) before placement so the center-anchor math is always correct.
// `smooth` toggles QPainter SmoothPixmapTransform + Antialiasing (and the
// pre-scale transformation quality).
QImage renderLayer(const QImage& src, const ClipTransform& t,
                   QSize canvasSize, bool smooth);

} // namespace clipgeom
