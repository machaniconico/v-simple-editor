#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>

// ============================================================
//  RotoBrushTool — soft-brush mask painter for roto correction
//
//  Mask format: QImage::Format_Grayscale8
//    - pixel value 0   = fully transparent (masked out)
//    - pixel value 255 = fully opaque (masked in)
//
//  Dab blending:
//    Add      : newAlpha = max(oldAlpha, round(coverage * opacity * 255))
//    Subtract : newAlpha = round(oldAlpha * (1.0 - coverage * opacity))
//
//  Soft falloff: coverage = 1 for dist <= radius*hardness,
//                smoothstep to 0 at dist == radius, 0 beyond.
//
//  Stroke spacing: dabs are stamped every max(1, radius/4) pixels
//  along the stroke so there are no gaps even during fast drags.
//
//  Undo: one level — snapshot taken at beginStroke(), restored by undo().
// ============================================================

namespace rotobrush {

class RotoBrushTool
{
public:
    enum class Op { Add, Subtract };

    RotoBrushTool() = default;

    // --- Mask management ---

    // Accepts any QImage format; converts internally to Grayscale8.
    // A null or empty image clears the working mask.
    void setMask(QImage mask);
    const QImage& mask() const;

    // --- Brush parameters ---

    // radiusPx : brush radius in pixels (clamped to >= 1)
    // hardness : [0..1] — 1 = hard disc, 0 = full feather across radius
    // opacity  : [0..1] — 1 = full strength per dab
    void setBrush(double radiusPx, double hardness, double opacity);

    // --- Stroke API ---

    // Snapshot current mask for undo, then stamp a dab at p.
    void beginStroke(QPointF p, Op op);

    // Interpolate dabs from last stroke position to p (spacing = max(1, radius/4)).
    void strokeTo(QPointF p);

    // Finish the current stroke (no-op in this model, provided for symmetry).
    void endStroke();

    // --- Utility ---

    void clear();   // set all pixels to 0   (transparent)
    void fill();    // set all pixels to 255 (opaque)

    // Restore mask to state at last beginStroke(); returns false if no snapshot.
    bool undo();

    // Optional: returns ARGB32 image with tint blended by mask alpha (for overlay display).
    QImage previewOverlay(const QColor& tint) const;

private:
    // Apply a single dab centred at centre onto m_mask using m_op.
    void stampDab(QPointF centre);

    // Smoothstep helper: t in [0,1] -> smooth [0,1]
    static double smoothstep(double edge0, double edge1, double x);

    QImage  m_mask;
    QImage  m_snapshot;          // undo buffer

    double  m_radiusPx  = 10.0;
    double  m_hardness  = 0.5;
    double  m_opacity   = 1.0;

    QPointF m_lastPoint;
    Op      m_op        = Op::Add;
    bool    m_hasSnapshot = false;
    bool    m_inStroke    = false;
};

} // namespace rotobrush
