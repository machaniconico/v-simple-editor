#include "GpuCompositeMath.h"

#include <QtGlobal>   // qBound
#include <algorithm>
#include <numeric>

namespace gpucomposite {

QVector<int> paintOrder(const QVector<LayerDesc>& layers) {
    // Build index list, then stable-sort DESCENDING by sourceTrack so layers
    // with the same sourceTrack keep their original relative order
    // (std::stable_sort, matching clipstack::layerPaintOrderLess).
    QVector<int> order(layers.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&layers](int a, int b) {
                         // a sorts before b iff layers[a] would be painted first.
                         // layerPaintOrderLess: a.sourceTrack > b.sourceTrack.
                         return layers[a].sourceTrack > layers[b].sourceTrack;
                     });
    return order;
}

QMatrix4x4 layerTransform(const LayerDesc& layer, QSize canvas) {
    // Mirror clipgeom::resolveTransform's op-order plus renderLayer's
    // defensive src.scaled(canvasSize, Qt::IgnoreAspectRatio, ...). The GPU
    // compositor samples a native-resolution texture directly, so the CPU
    // pre-scale must be represented as the first matrix operation.
    //
    // Op-order (Qt/QMatrix4x4 post-multiplies; LAST call applies FIRST):
    //   translate(anchor) * rotate(rot) * scale(videoScale)
    //     * translate(-canvas/2) * scale(canvas/src)
    // where anchor = (cw*0.5 + dx*cw, ch*0.5 + dy*ch).
    const double cw = canvas.width();
    const double ch = canvas.height();
    const double anchorX = cw * 0.5 + layer.videoDx * cw;
    const double anchorY = ch * 0.5 + layer.videoDy * ch;
    const double srcW = layer.srcSize.width();
    const double srcH = layer.srcSize.height();
    const double srcToCanvasX = (srcW > 0.0) ? (cw / srcW) : 1.0;
    const double srcToCanvasY = (srcH > 0.0) ? (ch / srcH) : 1.0;

    QMatrix4x4 m;
    m.translate(static_cast<float>(anchorX), static_cast<float>(anchorY));
    m.rotate(static_cast<float>(layer.rotation2DDegrees), 0.0f, 0.0f, 1.0f);
    m.scale(static_cast<float>(layer.videoScale), static_cast<float>(layer.videoScale));
    m.translate(static_cast<float>(-cw * 0.5),
                static_cast<float>(-ch * 0.5));
    m.scale(static_cast<float>(srcToCanvasX), static_cast<float>(srcToCanvasY));
    // Identity check for any non-empty src:
    //   translate(canvas/2) * translate(-canvas/2) * scale(canvas/src)
    // maps the native source rectangle exactly onto the canvas rectangle.
    return m;
}

bool isLayerComposited(const LayerDesc& layer) {
    return layer.visible && !layer.srcSize.isEmpty() && layer.opacity > 0.001;
}

double clampOpacity(double o) {
    return qBound(0.0, o, 1.0);
}

RGBAf premulSourceOver(const RGBAf& src, const RGBAf& dst) {
    const float inv = 1.0f - src.a;
    RGBAf out;
    out.r = src.r + dst.r * inv;
    out.g = src.g + dst.g * inv;
    out.b = src.b + dst.b * inv;
    out.a = src.a + dst.a * inv;
    return out;
}

bool isValidMatteSource(int idx, int count) {
    return idx > 0 && idx < count;
}

bool isValidMatteSource(int matteIdx, int layerIdx, int count) {
    return matteIdx > 0 && matteIdx < count && matteIdx != layerIdx;
}

int matteMaskValue(MatteType type,
                   int straightR, int straightG, int straightB,
                   int premulAlpha)
{
    // CPU-algebra reference oracle only. The GPU matte combine shader does not
    // call this helper; its luma path uses continuous normalized floats and is
    // verified by gpu-composite-parity's luma gates.
    int mask = 255;
    switch (type) {
    case MatteType::Alpha:
        mask = premulAlpha;
        break;
    case MatteType::AlphaInverted:
        mask = 255 - premulAlpha;
        break;
    case MatteType::Luminance:
        // Rec.601 luma using STRAIGHT (un-premultiplied) RGB; truncating
        // (NOT rounding) to match MaskSystem::trackMatteLumaRec601 +
        // applyTrackMatte, which use static_cast<int>(...).
        mask = static_cast<int>(0.299 * straightR
                               + 0.587 * straightG
                               + 0.114 * straightB);
        break;
    case MatteType::LuminanceInverted:
        mask = 255 - static_cast<int>(0.299 * straightR
                                     + 0.587 * straightG
                                     + 0.114 * straightB);
        break;
    case MatteType::None:
    default:
        mask = 255;
        break;
    }
    // Clamp to [0,255] — defensive against any premulAlpha/luma edge.
    if (mask < 0)   mask = 0;
    if (mask > 255) mask = 255;
    return mask;
}

void applyMaskPremul(int& r, int& g, int& b, int& a, int maskVal)
{
    // CPU-algebra reference oracle only. Matches MaskSystem::applyMask:
    // component = component * maskVal / 255 using integer division
    // (truncating), not rounding. Inputs premultiplied.
    r = r * maskVal / 255;
    g = g * maskVal / 255;
    b = b * maskVal / 255;
    a = a * maskVal / 255;
}

} // namespace gpucomposite
