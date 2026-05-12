#include "RotoBrushTool.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <algorithm>
#include <cmath>

namespace rotobrush {

// ============================================================
//  Helpers
// ============================================================

// smoothstep: 0 at x<=edge0, 1 at x>=edge1, cubic ramp in between.
double RotoBrushTool::smoothstep(double edge0, double edge1, double x)
{
    if (edge1 <= edge0) return (x >= edge1) ? 1.0 : 0.0;
    double t = (x - edge0) / (edge1 - edge0);
    t = std::max(0.0, std::min(1.0, t));
    return t * t * (3.0 - 2.0 * t);
}

// ============================================================
//  Mask management
// ============================================================

void RotoBrushTool::setMask(QImage mask)
{
    if (mask.isNull() || mask.width() == 0 || mask.height() == 0) {
        m_mask = QImage();
        return;
    }
    if (mask.format() == QImage::Format_Grayscale8) {
        m_mask = mask;
    } else {
        m_mask = mask.convertToFormat(QImage::Format_Grayscale8);
    }
    m_hasSnapshot = false;
    m_inStroke    = false;
}

const QImage& RotoBrushTool::mask() const
{
    return m_mask;
}

// ============================================================
//  Brush parameters
// ============================================================

void RotoBrushTool::setBrush(double radiusPx, double hardness, double opacity)
{
    m_radiusPx = std::max(1.0, radiusPx);
    m_hardness = std::max(0.0, std::min(1.0, hardness));
    m_opacity  = std::max(0.0, std::min(1.0, opacity));
}

// ============================================================
//  Stroke API
// ============================================================

void RotoBrushTool::beginStroke(QPointF p, Op op)
{
    m_op          = op;
    m_inStroke    = true;

    // Snapshot for undo (deep copy)
    if (!m_mask.isNull()) {
        m_snapshot    = m_mask.copy();
        m_hasSnapshot = true;
    } else {
        m_snapshot    = QImage();
        m_hasSnapshot = false;
    }

    m_lastPoint = p;
    stampDab(p);
}

void RotoBrushTool::strokeTo(QPointF p)
{
    if (!m_inStroke || m_mask.isNull()) return;

    const double dx      = p.x() - m_lastPoint.x();
    const double dy      = p.y() - m_lastPoint.y();
    const double dist    = std::sqrt(dx * dx + dy * dy);

    // Spacing: dabs every max(1, radius/4) pixels.
    const double spacing = std::max(1.0, m_radiusPx / 4.0);

    if (dist < 1e-6) {
        // Zero-length move: stamp once, nothing to interpolate.
        stampDab(p);
        m_lastPoint = p;
        return;
    }

    // Number of dabs to place (cap at 65535 to keep the loop bounded).
    const int steps = std::min(static_cast<int>(std::ceil(dist / spacing)), 65535);

    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(steps);
        stampDab(QPointF(m_lastPoint.x() + t * dx,
                         m_lastPoint.y() + t * dy));
    }

    m_lastPoint = p;
}

void RotoBrushTool::endStroke()
{
    m_inStroke = false;
}

// ============================================================
//  Utility
// ============================================================

void RotoBrushTool::clear()
{
    if (!m_mask.isNull())
        m_mask.fill(0);
}

void RotoBrushTool::fill()
{
    if (!m_mask.isNull())
        m_mask.fill(255);
}

bool RotoBrushTool::undo()
{
    if (!m_hasSnapshot || m_snapshot.isNull()) return false;
    m_mask        = m_snapshot.copy();
    m_hasSnapshot = false;
    m_inStroke    = false;
    return true;
}

// ============================================================
//  Preview overlay
// ============================================================

QImage RotoBrushTool::previewOverlay(const QColor& tint) const
{
    if (m_mask.isNull()) return QImage();

    const int w = m_mask.width();
    const int h = m_mask.height();
    QImage out(w, h, QImage::Format_ARGB32);
    out.fill(Qt::transparent);

    const int tr = tint.red();
    const int tg = tint.green();
    const int tb = tint.blue();

    for (int y = 0; y < h; ++y) {
        const uchar*  src = m_mask.constScanLine(y);
        QRgb*         dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = src[x];
            if (a > 0)
                dst[x] = qRgba(tr, tg, tb, a);
        }
    }
    return out;
}

// ============================================================
//  stampDab — core painting primitive
// ============================================================

void RotoBrushTool::stampDab(QPointF centre)
{
    if (m_mask.isNull()) return;

    const double r        = m_radiusPx;
    const double hardEdge = r * m_hardness;  // inner disc radius (full coverage)
    const double opacity  = m_opacity;

    const int cx = static_cast<int>(std::round(centre.x()));
    const int cy = static_cast<int>(std::round(centre.y()));
    const int ir = static_cast<int>(std::ceil(r));

    const int w  = m_mask.width();
    const int h  = m_mask.height();

    // Bounding rect (clamped to image bounds)
    const int x0 = std::max(0, cx - ir);
    const int x1 = std::min(w - 1, cx + ir);
    const int y0 = std::max(0, cy - ir);
    const int y1 = std::min(h - 1, cy + ir);

    for (int py = y0; py <= y1; ++py) {
        uchar* row = m_mask.scanLine(py);
        for (int px = x0; px <= x1; ++px) {
            const double ddx  = px - centre.x();
            const double ddy  = py - centre.y();
            const double dist = std::sqrt(ddx * ddx + ddy * ddy);

            if (dist > r) continue;

            // coverage: 1 inside hardEdge, smooth ramp to 0 at r
            double coverage;
            if (dist <= hardEdge) {
                coverage = 1.0;
            } else {
                // smoothstep from 1->0 as dist goes from hardEdge->r
                coverage = 1.0 - smoothstep(hardEdge, r, dist);
            }

            const int oldAlpha = row[px];

            int newAlpha;
            if (m_op == Op::Add) {
                // Raise toward 255: newAlpha = max(oldAlpha, round(coverage*opacity*255))
                const int dabAlpha = static_cast<int>(std::round(coverage * opacity * 255.0));
                newAlpha = std::max(oldAlpha, dabAlpha);
            } else {
                // Subtract: newAlpha = round(oldAlpha * (1 - coverage*opacity))
                newAlpha = static_cast<int>(std::round(
                    static_cast<double>(oldAlpha) * (1.0 - coverage * opacity)));
            }

            // Clamp to [0, 255]
            row[px] = static_cast<uchar>(std::max(0, std::min(255, newAlpha)));
        }
    }
}

} // namespace rotobrush
