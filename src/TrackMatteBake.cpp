#include "TrackMatteBake.h"

#include "MaskSystem.h"

#include <QPainter>
#include <QSet>
#include <QtGlobal>
#include <atomic>

namespace trackmatte {

static std::atomic_bool g_appliedMatte{false};
static const bool g_observeAppliedMatte =
    !qEnvironmentVariableIsEmpty("VEDITOR_TRACKMATTE_SELFTEST");

bool selftestAppliedMatte()
{
    return g_appliedMatte.load(std::memory_order_acquire);
}

void selftestReset()
{
    g_appliedMatte.store(false, std::memory_order_release);
}

// RM-3: layer index 0 is the V1 base in renderFrameAt's contract and must
// NEVER be consumable as a matte source — a malformed / hand-edited
// matteSourceClipId that resolves to 0 must be ignored (the layer
// composites normally), not blank the frame by matte'ing against the base.
static bool isValidMatteSource(int layerIndex, int matteIndex, int layerCount)
{
    return matteIndex > 0                 // 0 == V1 base — never a matte src
        && matteIndex < layerCount
        && matteIndex != layerIndex;
}

QImage composite(const QVector<CompositeLayer>& layers,
                 const QVector<QImage>& layerImages,
                 QSize canvasSize)
{
    // RM-2: stack with the IDENTICAL premultiplied-SourceOver path the
    // matte-free branch of renderFrameAt uses (ARGB32_Premultiplied
    // canvas, QPainter setOpacity + CompositionMode_SourceOver,
    // SmoothPixmapTransform=false) instead of the old straight-alpha
    // LayerCompositor::blendImages hand blend. Compositing a layer onto a
    // transparent premultiplied canvas via SourceOver is byte-identical
    // to the matte-free formula (SourceOver over transparent is the
    // identity for the first opaque layer, so initialising transparent
    // and drawing layer 0 == initialising from the base). The ONLY
    // delta a track matte introduces is an alpha modification of the
    // matte'd layer's OWN image (MaskSystem::applyTrackMatte) applied
    // BEFORE it enters this shared stacking loop — every non-matte layer
    // is composited byte-for-byte as in the matte-free branch.
    QImage canvas(canvasSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    const int layerCount = static_cast<int>(layers.size());
    const int imageCount = static_cast<int>(layerImages.size());
    QSet<int> matteSourceIndices;
    for (int i = 0; i < layerCount; ++i) {
        const CompositeLayer &layer = layers[i];
        if (layer.matteType == TrackMatteType::None)
            continue;

        const int matteIndex = layer.matteSourceLayerIndex;
        if (!isValidMatteSource(i, matteIndex, layerCount))
            continue;

        matteSourceIndices.insert(matteIndex);
    }

    // US-2303: a visible layer with a blend mode that is not a matte source is
    // left out of the stack and blended over it afterwards, ascending, as the
    // matte-free paths overlay their blended layers after the Normal stack.
    // Without one the loop below runs exactly as before.
    QVector<int> blendedIndices;
    for (int i = 0; i < layerCount; ++i) {
        const CompositeLayer &layer = layers[i];
        if (layer.visible && !matteSourceIndices.contains(i)
            && layer.blendMode != BlendMode::Normal) {
            blendedIndices.append(i);
        }
    }

    auto imageAt = [&layerImages, imageCount](int index) -> QImage {
        if (index < 0 || index >= imageCount)
            return QImage();
        return layerImages[index];
    };

    // The layer's own matte, when it has a usable one, cuts its alpha.
    auto mattedImage = [&](int i, QImage layerImage) -> QImage {
        const CompositeLayer &layer = layers[i];
        if (layer.matteType == TrackMatteType::None)
            return layerImage;
        const int matteIndex = layer.matteSourceLayerIndex;
        if (!isValidMatteSource(i, matteIndex, layerCount))
            return layerImage;
        const QImage matteImage = imageAt(matteIndex);
        if (matteImage.isNull())
            return layerImage;
        if (g_observeAppliedMatte)
            g_appliedMatte.store(true, std::memory_order_release);
        return MaskSystem::applyTrackMatte(layerImage, matteImage, layer.matteType);
    };

    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    for (int i = 0; i < layerCount; ++i) {
        const CompositeLayer &layer = layers[i];

        if (!layer.visible)
            continue;

        if (matteSourceIndices.contains(i) || blendedIndices.contains(i))
            continue;

        QImage layerImage = imageAt(i);
        if (layerImage.isNull())
            continue;

        layerImage = mattedImage(i, layerImage);

        // MaskSystem::applyTrackMatte returns ARGB32_Premultiplied;
        // renderLayer images are already canvas-sized. Drawing a
        // premultiplied source onto the premultiplied canvas is the
        // exact one-unpremultiply round-trip the matte-free path and the
        // old blendImages chain both perform, so the matte'd-layer pixel
        // stays the AE/Premiere contract (fg RGB preserved, alpha == the
        // matte coverage) and non-matte layers are byte-identical.
        p.setOpacity(qBound(0.0, layer.opacity, 1.0));
        p.drawImage(0, 0, layerImage);
    }
    p.end();

    if (blendedIndices.isEmpty())
        return canvas;

    // Each blended layer, cut by its own matte first, goes through the blend
    // equation of the matte-free paths (LayerCompositor::blendImages, chained
    // in its Format_ARGB32), and the result is premultiplied once at the end
    // as they do.
    QImage blended = canvas;
    for (int i : blendedIndices) {
        const CompositeLayer &layer = layers[i];
        const QImage layerImage = imageAt(i);
        if (layerImage.isNull() || layer.opacity <= 0.001)
            continue;
        blended = LayerCompositor::blendImages(
            blended, mattedImage(i, layerImage), layer.blendMode,
            qBound(0.0, layer.opacity, 1.0));
    }
    return blended.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace trackmatte
