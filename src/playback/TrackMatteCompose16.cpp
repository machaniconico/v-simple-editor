#include "TrackMatteCompose16.h"

#include <QColor>
#include <QPainter>
#include <QRgba64>
#include <QSet>
#include <QtGlobal>

namespace trackmatte16 {
namespace {

constexpr quint32 kMax16 = hdrcomposite::kMax16;

bool usesLuma(gpucomposite::MatteType type)
{
    return type == gpucomposite::MatteType::Luminance
        || type == gpucomposite::MatteType::LuminanceInverted;
}

hdrcomposite::Rgba16 readRgba16(const QRgba64& px)
{
    return hdrcomposite::Rgba16{px.red(), px.green(), px.blue(), px.alpha()};
}

QRgba64 writeRgba16(const hdrcomposite::Rgba16& px)
{
    return qRgba64(px.r, px.g, px.b, px.a);
}

quint16 unpremultiply16(quint16 premul, quint16 alpha)
{
    if (alpha == 0)
        return 0;
    const quint32 straight =
        qBound(0u, static_cast<quint32>(premul) * kMax16 / alpha, kMax16);
    return static_cast<quint16>(straight);
}

quint32 lumaMask16(quint16 straightR16, quint16 straightG16, quint16 straightB16)
{
    const quint32 luma = static_cast<quint32>(0.299 * straightR16
                                            + 0.587 * straightG16
                                            + 0.114 * straightB16);
    return qBound(0u, luma, kMax16);
}

} // namespace

quint16 matteMaskValue16(gpucomposite::MatteType type,
                         quint16 straightR16,
                         quint16 straightG16,
                         quint16 straightB16,
                         quint16 premulA16)
{
    quint32 mask = kMax16;
    switch (type) {
    case gpucomposite::MatteType::Alpha:
        mask = premulA16;
        break;
    case gpucomposite::MatteType::AlphaInverted:
        mask = kMax16 - premulA16;
        break;
    case gpucomposite::MatteType::Luminance:
        mask = lumaMask16(straightR16, straightG16, straightB16);
        break;
    case gpucomposite::MatteType::LuminanceInverted:
        mask = kMax16 - lumaMask16(straightR16, straightG16, straightB16);
        break;
    case gpucomposite::MatteType::None:
    default:
        mask = kMax16;
        break;
    }
    return static_cast<quint16>(qBound(0u, mask, kMax16));
}

void applyMaskPremul16(hdrcomposite::Rgba16& px, quint16 maskVal)
{
    px.r = static_cast<quint16>(static_cast<quint32>(px.r) * maskVal / kMax16);
    px.g = static_cast<quint16>(static_cast<quint32>(px.g) * maskVal / kMax16);
    px.b = static_cast<quint16>(static_cast<quint32>(px.b) * maskVal / kMax16);
    px.a = static_cast<quint16>(static_cast<quint32>(px.a) * maskVal / kMax16);
}

QImage composeExport(const QVector<gpucomposite::LayerDesc>& layers,
                     const QVector<QImage>& canvasSizedImages,
                     QSize canvas,
                     const MatteColorCtx& ctx)
{
    if (canvas.isEmpty())
        return QImage();

    QImage out(canvas, QImage::Format_RGBA64_Premultiplied);
    out.fill(QColor(0, 0, 0, 0));

    const int layerCount = static_cast<int>(layers.size());
    const int imageCount = static_cast<int>(canvasSizedImages.size());

    QSet<int> matteSourceIndices;
    for (int i = 0; i < layerCount; ++i) {
        const gpucomposite::LayerDesc& layer = layers[i];
        if (layer.matteType == gpucomposite::MatteType::None)
            continue;
        if (gpucomposite::isValidMatteSource(layer.matteSourceIndex, i, layerCount))
            matteSourceIndices.insert(layer.matteSourceIndex);
    }

    auto imageAt = [&canvasSizedImages, imageCount](int index) -> QImage {
        if (index < 0 || index >= imageCount)
            return QImage();
        return canvasSizedImages[index];
    };

    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    for (int i = 0; i < layerCount; ++i) {
        const gpucomposite::LayerDesc& layer = layers[i];
        if (!layer.visible)
            continue;
        if (matteSourceIndices.contains(i))
            continue;

        QImage layerImage64 = imageAt(i);
        if (layerImage64.isNull())
            continue;
        layerImage64 = layerImage64.convertToFormat(QImage::Format_RGBA64_Premultiplied);

        if (layer.matteType != gpucomposite::MatteType::None
            && gpucomposite::isValidMatteSource(layer.matteSourceIndex, i, layerCount)) {
            QImage matte64 = imageAt(layer.matteSourceIndex);
            if (!matte64.isNull()) {
                matte64 = matte64.convertToFormat(QImage::Format_RGBA64_Premultiplied);
                if (usesLuma(layer.matteType) && ctx.matteSourceIsLinear)
                    matte64 = clipodt::applyOdt16(matte64, ctx.odtForLuma);
                if (matte64.size() != canvas)
                    matte64 = matte64.scaled(canvas, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                       .convertToFormat(QImage::Format_RGBA64_Premultiplied);

                const int w = qMin(layerImage64.width(), matte64.width());
                const int h = qMin(layerImage64.height(), matte64.height());
                for (int y = 0; y < h; ++y) {
                    QRgba64* layerLine = reinterpret_cast<QRgba64*>(layerImage64.scanLine(y));
                    const QRgba64* matteLine = reinterpret_cast<const QRgba64*>(matte64.constScanLine(y));
                    for (int x = 0; x < w; ++x) {
                        const QRgba64 mattePx = matteLine[x];
                        const quint16 premulA = mattePx.alpha();
                        const quint16 straightR = usesLuma(layer.matteType)
                            ? unpremultiply16(mattePx.red(), premulA)
                            : 0;
                        const quint16 straightG = usesLuma(layer.matteType)
                            ? unpremultiply16(mattePx.green(), premulA)
                            : 0;
                        const quint16 straightB = usesLuma(layer.matteType)
                            ? unpremultiply16(mattePx.blue(), premulA)
                            : 0;
                        const quint16 maskVal = matteMaskValue16(
                            layer.matteType, straightR, straightG, straightB, premulA);
                        hdrcomposite::Rgba16 px = readRgba16(layerLine[x]);
                        applyMaskPremul16(px, maskVal);
                        layerLine[x] = writeRgba16(px);
                    }
                }
            }
        }

        painter.setOpacity(qBound(0.0, layer.opacity, 1.0));
        painter.drawImage(0, 0, layerImage64);
    }
    painter.end();

    return out;
}

} // namespace trackmatte16
