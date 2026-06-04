#include "HdrCompositeMath.h"
#include "TrackMatteCompose16.h"

#include <QColor>
#include <QMatrix4x4>
#include <QPointF>
#include <QRect>
#include <QRgba64>
#include <QSet>
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

// Reads the RAW premultiplied 16-bit pixel from an RGBA64_Premultiplied image.
// Deliberately NOT QImage::pixelColor(): for premultiplied formats pixelColor()
// returns the UN-premultiplied (straight) colour, and feeding straight values into
// the premultiplied source-over below silently corrupts every translucent layer
// (opaque pixels are unaffected because straight==premultiplied at alpha=MAX, which
// is why the bug stays invisible until a genuinely translucent source appears).
// We read the stored premultiplied QRgba64 directly; the caller guarantees the
// image is Format_RGBA64_Premultiplied.
inline Rgba16 readPixel16(const QImage& img, int x, int y) {
    const QRgba64 q = reinterpret_cast<const QRgba64*>(img.constScanLine(y))[x];
    return Rgba16{ q.red(), q.green(), q.blue(), q.alpha() };
}

// Writes a RAW premultiplied 16-bit pixel. NOT setPixelColor(), which re-premultiplies
// a straight QColor — we store the already-premultiplied value verbatim.
inline void writePixel16(QImage& img, int x, int y, Rgba16 px) {
    reinterpret_cast<QRgba64*>(img.scanLine(y))[x] = qRgba64(px.r, px.g, px.b, px.a);
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

inline quint16 unpremultiply16(quint16 premul, quint16 alpha) {
    if (alpha == 0)
        return 0;
    const quint64 straight =
        static_cast<quint64>(premul) * static_cast<quint64>(kMax16) / alpha;
    return static_cast<quint16>(straight > kMax16 ? kMax16 : straight);
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

QImage compositeReference16Matte(const QVector<gpucomposite::LayerDesc>& layers,
                                const QVector<QImage>& images,
                                QSize canvas) {
    if (canvas.isEmpty()) {
        for (const QImage& im : images) {
            if (!im.isNull()) { canvas = im.size(); break; }
        }
    }
    if (canvas.isEmpty()) return QImage();

    QImage out(canvas, QImage::Format_RGBA64_Premultiplied);
    out.fill(QColor(0, 0, 0, 0));

    const int layerCount = layers.size();
    const QVector<int> order = gpucomposite::paintOrder(layers);

    QSet<int> matteSourceIndices;
    for (int i = 0; i < layerCount; ++i) {
        const gpucomposite::LayerDesc& d = layers[i];
        if (d.matteType == gpucomposite::MatteType::None)
            continue;
        if (gpucomposite::isValidMatteSource(d.matteSourceIndex, i, layerCount))
            matteSourceIndices.insert(d.matteSourceIndex);
    }

    const int cw = canvas.width();
    const int ch = canvas.height();

    for (int idx : order) {
        if (idx < 0 || idx >= layerCount || idx >= images.size()) continue;
        const gpucomposite::LayerDesc& desc = layers[idx];
        if (!gpucomposite::isLayerComposited(desc)) continue;
        if (matteSourceIndices.contains(idx)) continue;

        QImage src = images[idx];
        if (src.isNull()) continue;
        if (src.format() != QImage::Format_RGBA64_Premultiplied)
            src = src.convertToFormat(QImage::Format_RGBA64_Premultiplied);

        const double opacity = gpucomposite::clampOpacity(desc.opacity);
        if (opacity <= 0.0) continue;

        const QMatrix4x4 fwd = gpucomposite::layerTransform(desc, canvas);
        bool invertible = false;
        const QMatrix4x4 inv = fwd.inverted(&invertible);
        if (!invertible) continue;

        const int sw = src.width();
        const int sh = src.height();

        QImage matteSrc;
        QMatrix4x4 matteInv;
        bool matteInvertible = false;
        bool useMatte = false;
        if (desc.matteType != gpucomposite::MatteType::None
            && gpucomposite::isValidMatteSource(desc.matteSourceIndex, idx, layerCount)
            && desc.matteSourceIndex >= 0
            && desc.matteSourceIndex < images.size()) {
            matteSrc = images[desc.matteSourceIndex];
            if (!matteSrc.isNull()) {
                if (matteSrc.format() != QImage::Format_RGBA64_Premultiplied)
                    matteSrc = matteSrc.convertToFormat(QImage::Format_RGBA64_Premultiplied);
                const QMatrix4x4 matteFwd =
                    gpucomposite::layerTransform(layers[desc.matteSourceIndex], canvas);
                matteInv = matteFwd.inverted(&matteInvertible);
                useMatte = matteInvertible;
            }
        }

        const int mw = matteSrc.width();
        const int mh = matteSrc.height();

        for (int cy = 0; cy < ch; ++cy) {
            for (int cx = 0; cx < cw; ++cx) {
                int sx = 0, sy = 0;
                if (!inverseSample(invertible, inv, sw, sh, cx, cy, sx, sy))
                    continue;

                Rgba16 s = readPixel16(src, sx, sy);

                if (useMatte) {
                    Rgba16 mattePx{0, 0, 0, 0};
                    int mx = 0, my = 0;
                    if (inverseSample(matteInvertible, matteInv, mw, mh, cx, cy, mx, my))
                        mattePx = readPixel16(matteSrc, mx, my);

                    const quint16 straightR = unpremultiply16(mattePx.r, mattePx.a);
                    const quint16 straightG = unpremultiply16(mattePx.g, mattePx.a);
                    const quint16 straightB = unpremultiply16(mattePx.b, mattePx.a);
                    const quint16 maskVal = trackmatte16::matteMaskValue16(
                        desc.matteType, straightR, straightG, straightB, mattePx.a);
                    trackmatte16::applyMaskPremul16(s, maskVal);
                }

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
