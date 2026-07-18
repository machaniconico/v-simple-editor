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

    auto imageAt = [&layerImages, imageCount](int index) -> QImage {
        if (index < 0 || index >= imageCount)
            return QImage();
        return layerImages[index];
    };

    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    for (int i = 0; i < layerCount; ++i) {
        const CompositeLayer &layer = layers[i];

        if (!layer.visible)
            continue;

        if (matteSourceIndices.contains(i))
            continue;

        QImage layerImage = imageAt(i);
        if (layerImage.isNull())
            continue;

        if (layer.matteType != TrackMatteType::None) {
            const int matteIndex = layer.matteSourceLayerIndex;
            if (isValidMatteSource(i, matteIndex, layerCount)) {
                const QImage matteImage = imageAt(matteIndex);
                if (!matteImage.isNull()) {
                    layerImage = MaskSystem::applyTrackMatte(
                        layerImage, matteImage, layer.matteType);
                    if (g_observeAppliedMatte)
                        g_appliedMatte.store(true, std::memory_order_release);
                }
            }
        }

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

    return canvas;
}

} // namespace trackmatte
