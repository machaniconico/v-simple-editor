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

} // namespace gpucomposite
