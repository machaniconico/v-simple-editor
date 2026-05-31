#include "HdrCompositeMath.h"

#include <QColor>
#include <QMatrix4x4>
#include <QPointF>
#include <QRect>
#include <QRgba64>
#include <QVector3D>

#include <cmath>
#include <cstdint>

namespace hdrcomposite {

Rgba16 premulSourceOver16(Rgba16 dst, Rgba16 src) {
    // src,dst premultiplied. out = src + dst*(1 - srcA/MAX), MAX = 65535.
    // inv in [0..65535]; dst-channel * inv can reach 65535*65535 ≈ 4.29e9 which
    // overflows quint32, so the product is accumulated in quint64.
    const quint64 inv = static_cast<quint64>(kMax16) - static_cast<quint64>(src.a);
    auto chan = [&](quint16 s, quint16 d) -> quint16 {
        // Rounded divide: (d*inv + MAX/2) / MAX.
        const quint64 blended =
            static_cast<quint64>(s) +
            (static_cast<quint64>(d) * inv + (kMax16 / 2)) / kMax16;
        return static_cast<quint16>(blended > kMax16 ? kMax16 : blended);
    };
    return Rgba16{ chan(src.r, dst.r), chan(src.g, dst.g),
                   chan(src.b, dst.b), chan(src.a, dst.a) };
}

namespace {

// Reads a premultiplied 16-bit pixel from an RGBA64-premultiplied image.
inline Rgba16 readPixel16(const QImage& img, int x, int y) {
    // QColor::rgba64() preserves the full 16-bit channels of an RGBA64 image.
    const QRgba64 q = img.pixelColor(x, y).rgba64();
    return Rgba16{ q.red(), q.green(), q.blue(), q.alpha() };
}

inline void writePixel16(QImage& img, int x, int y, Rgba16 px) {
    QColor c;
    c.setRgba64(qRgba64(px.r, px.g, px.b, px.a));
    img.setPixelColor(x, y, c);
}

// Multiplies opacity into a premultiplied 16-bit pixel (all channels scale,
// since premultiplied colour already carries alpha).
inline Rgba16 applyOpacity16(Rgba16 px, double opacity) {
    auto scale = [&](quint16 v) -> quint16 {
        const double r = static_cast<double>(v) * opacity + 0.5;
        if (r <= 0.0) return 0;
        if (r >= static_cast<double>(kMax16)) return static_cast<quint16>(kMax16);
        return static_cast<quint16>(r);
    };
    return Rgba16{ scale(px.r), scale(px.g), scale(px.b), scale(px.a) };
}

// Inverse-maps one canvas pixel through the layer's QMatrix4x4 (2D affine) and
// returns the nearest source pixel coords. Returns false if outside the source.
inline bool inverseSample(bool invertible, const QMatrix4x4& inv, int sw, int sh,
                          int cx, int cy, int& sx, int& sy) {
    if (!invertible) return false;
    // Map canvas pixel CENTER back to source space (GL_NEAREST equivalent).
    const QVector3D srcPt =
        inv.map(QVector3D(static_cast<float>(cx) + 0.5f,
                          static_cast<float>(cy) + 0.5f, 0.0f));
    sx = static_cast<int>(std::floor(srcPt.x()));
    sy = static_cast<int>(std::floor(srcPt.y()));
    return sx >= 0 && sy >= 0 && sx < sw && sy < sh;
}

} // namespace

QImage compositeReference16(const QVector<gpucomposite::LayerDesc>& layers,
                           const QVector<QImage>& images,
                           QSize canvas) {
    if (canvas.isEmpty()) {
        for (const QImage& im : images) {
            if (!im.isNull()) { canvas = im.size(); break; }
        }
    }
    if (canvas.isEmpty()) return QImage();

    QImage out(canvas, QImage::Format_RGBA64_Premultiplied);
    out.fill(QColor(0, 0, 0, 0)); // transparent black, premultiplied

    // Geometry SSOT: reuse the 8-bit paint order (bit-depth independent).
    const QVector<int> order = gpucomposite::paintOrder(layers);

    for (int idx : order) {
        if (idx < 0 || idx >= layers.size() || idx >= images.size()) continue;
        const gpucomposite::LayerDesc& desc = layers[idx];
        if (!gpucomposite::isLayerComposited(desc)) continue;
        QImage src = images[idx];
        if (src.isNull()) continue;
        // matteTypeOrdinal != None is treated as a PLAIN layer (matte-free
        // scope; matte source/target relationship intentionally ignored here).

        if (src.format() != QImage::Format_RGBA64_Premultiplied)
            src = src.convertToFormat(QImage::Format_RGBA64_Premultiplied);

        const double opacity = gpucomposite::clampOpacity(desc.opacity);
        if (opacity <= 0.0) continue;

        // Geometry SSOT: reuse the 8-bit per-layer transform (bit-depth
        // independent). Maps native source pixels -> canvas pixels; we invert it
        // to do canvas->source nearest-neighbour sampling (GL_NEAREST), matching
        // the CPU preview compositor's clipgeom::renderLayer placement.
        const QMatrix4x4 fwd = gpucomposite::layerTransform(desc, canvas);
        bool invertible = false;
        const QMatrix4x4 inv = fwd.inverted(&invertible);
        if (!invertible) continue;

        const int sw = src.width();
        const int sh = src.height();
        const int cw = canvas.width();
        const int ch = canvas.height();

        // The placed layer can cover the whole canvas (clipgeom pre-scales the
        // source to canvas size); iterate every canvas pixel and skip those that
        // inverse-map outside the source.
        for (int cy = 0; cy < ch; ++cy) {
            for (int cx = 0; cx < cw; ++cx) {
                int sx = 0, sy = 0;
                if (!inverseSample(invertible, inv, sw, sh, cx, cy, sx, sy))
                    continue;
                Rgba16 s = readPixel16(src, sx, sy);
                if (opacity < 1.0) s = applyOpacity16(s, opacity);
                const Rgba16 d = readPixel16(out, cx, cy);
                writePixel16(out, cx, cy, premulSourceOver16(d, s));
            }
        }
    }

    return out;
}

QImage to8bit(const QImage& rgba64) {
    if (rgba64.isNull()) return QImage();
    return rgba64.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace hdrcomposite
