#include "LayerCompositor.h"

#include <QPainter>
#include <QTransform>
#include <QVariant>
#include <QtMath>

#include <algorithm>

// ===== CompositeLayer — name helpers =====

QString CompositeLayer::blendModeName(BlendMode mode)
{
    switch (mode) {
        case BlendMode::Normal:     return "Normal";
        case BlendMode::Add:        return "Add";
        case BlendMode::Multiply:   return "Multiply";
        case BlendMode::Screen:     return "Screen";
        case BlendMode::Overlay:    return "Overlay";
        case BlendMode::SoftLight:  return "Soft Light";
        case BlendMode::HardLight:  return "Hard Light";
        case BlendMode::Difference: return "Difference";
        case BlendMode::Exclusion:  return "Exclusion";
        case BlendMode::ColorDodge: return "Color Dodge";
        case BlendMode::ColorBurn:  return "Color Burn";
        case BlendMode::Darken:     return "Darken";
        case BlendMode::Lighten:    return "Lighten";
    }
    return "Normal";
}

BlendMode CompositeLayer::blendModeFromName(const QString &name)
{
    if (name == "Normal")      return BlendMode::Normal;
    if (name == "Add")         return BlendMode::Add;
    if (name == "Multiply")    return BlendMode::Multiply;
    if (name == "Screen")      return BlendMode::Screen;
    if (name == "Overlay")     return BlendMode::Overlay;
    if (name == "Soft Light")  return BlendMode::SoftLight;
    if (name == "Hard Light")  return BlendMode::HardLight;
    if (name == "Difference")  return BlendMode::Difference;
    if (name == "Exclusion")   return BlendMode::Exclusion;
    if (name == "Color Dodge") return BlendMode::ColorDodge;
    if (name == "Color Burn")  return BlendMode::ColorBurn;
    if (name == "Darken")      return BlendMode::Darken;
    if (name == "Lighten")     return BlendMode::Lighten;
    return BlendMode::Normal;
}

QString CompositeLayer::sourceTypeName(LayerSourceType type)
{
    switch (type) {
        case LayerSourceType::Video:      return "Video";
        case LayerSourceType::Image:      return "Image";
        case LayerSourceType::Solid:      return "Solid";
        case LayerSourceType::Shape:      return "Shape";
        case LayerSourceType::Text:       return "Text";
        case LayerSourceType::Adjustment: return "Adjustment";
    }
    return "Video";
}

LayerSourceType CompositeLayer::sourceTypeFromName(const QString &name)
{
    if (name == "Video")      return LayerSourceType::Video;
    if (name == "Image")      return LayerSourceType::Image;
    if (name == "Solid")      return LayerSourceType::Solid;
    if (name == "Shape")      return LayerSourceType::Shape;
    if (name == "Text")       return LayerSourceType::Text;
    if (name == "Adjustment") return LayerSourceType::Adjustment;
    return LayerSourceType::Video;
}

// ===== CompositeLayer — serialisation =====

QJsonObject CompositeLayer::toJson() const
{
    QJsonObject obj;
    obj["name"]      = name;
    obj["visible"]   = visible;
    obj["locked"]    = locked;
    obj["opacity"]   = opacity;
    obj["blendMode"] = blendModeName(blendMode);

    obj["posX"] = position.x();
    obj["posY"] = position.y();
    obj["scaleX"]   = scale.x();
    obj["scaleY"]   = scale.y();
    obj["rotation"] = rotation;
    obj["anchorX"]  = anchorPoint.x();
    obj["anchorY"]  = anchorPoint.y();
    obj["zOrder"]   = zOrder;

    obj["sourceType"] = sourceTypeName(sourceType);
    obj["sourcePath"] = sourcePath;
    obj["solidColorR"] = solidColor.red();
    obj["solidColorG"] = solidColor.green();
    obj["solidColorB"] = solidColor.blue();
    obj["solidColorA"] = solidColor.alpha();

    obj["inPoint"]  = inPoint;
    obj["outPoint"] = outPoint;
    obj["matteType"] = static_cast<int>(matteType);
    obj["matteSourceLayerIndex"] = matteSourceLayerIndex;

    return obj;
}

CompositeLayer CompositeLayer::fromJson(const QJsonObject &obj)
{
    CompositeLayer l;
    l.name      = obj["name"].toString();
    l.visible   = obj["visible"].toBool(true);
    l.locked    = obj["locked"].toBool(false);
    l.opacity   = obj["opacity"].toDouble(1.0);
    l.blendMode = blendModeFromName(obj["blendMode"].toString("Normal"));

    l.position    = QPointF(obj["posX"].toDouble(0.0), obj["posY"].toDouble(0.0));
    l.scale       = QPointF(obj["scaleX"].toDouble(1.0), obj["scaleY"].toDouble(1.0));
    l.rotation    = obj["rotation"].toDouble(0.0);
    l.anchorPoint = QPointF(obj["anchorX"].toDouble(0.0), obj["anchorY"].toDouble(0.0));
    l.zOrder      = obj["zOrder"].toInt(0);

    l.sourceType = sourceTypeFromName(obj["sourceType"].toString("Video"));
    l.sourcePath = obj["sourcePath"].toString();
    l.solidColor = QColor(
        obj["solidColorR"].toInt(255),
        obj["solidColorG"].toInt(255),
        obj["solidColorB"].toInt(255),
        obj["solidColorA"].toInt(255));

    l.inPoint  = obj["inPoint"].toDouble(0.0);
    l.outPoint = obj["outPoint"].toDouble(0.0);
    l.matteType = static_cast<TrackMatteType>(
        obj["matteType"].toInt(static_cast<int>(TrackMatteType::None)));
    l.matteSourceLayerIndex = obj["matteSourceLayerIndex"].toInt(-1);

    return l;
}

// ===== LayerCompositor — layer management =====

void LayerCompositor::addLayer(const CompositeLayer &layer)
{
    m_layers.append(layer);
}

bool LayerCompositor::removeLayer(int index)
{
    if (index < 0 || index >= m_layers.size())
        return false;
    m_layers.removeAt(index);
    return true;
}

bool LayerCompositor::moveLayer(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_layers.size())
        return false;
    if (toIndex < 0 || toIndex >= m_layers.size())
        return false;
    if (fromIndex == toIndex)
        return true;

    CompositeLayer layer = m_layers[fromIndex];
    m_layers.removeAt(fromIndex);
    m_layers.insert(toIndex, layer);
    return true;
}

QVector<CompositeLayer> LayerCompositor::layers() const
{
    QVector<CompositeLayer> sorted = m_layers;
    std::sort(sorted.begin(), sorted.end(),
              [](const CompositeLayer &a, const CompositeLayer &b) {
                  return a.zOrder < b.zOrder;
              });
    return sorted;
}

// ===== Generic property setter =====

bool LayerCompositor::setLayerProperty(int index, const QString &property, const QVariant &value)
{
    if (index < 0 || index >= m_layers.size())
        return false;

    CompositeLayer &l = m_layers[index];

    if      (property == "name")        l.name        = value.toString();
    else if (property == "visible")     l.visible     = value.toBool();
    else if (property == "locked")      l.locked      = value.toBool();
    else if (property == "opacity")     l.opacity     = qBound(0.0, value.toDouble(), 1.0);
    else if (property == "blendMode")   l.blendMode   = static_cast<BlendMode>(value.toInt());
    else if (property == "position")    l.position    = value.toPointF();
    else if (property == "scale")       l.scale       = value.toPointF();
    else if (property == "rotation")    l.rotation    = value.toDouble();
    else if (property == "anchorPoint") l.anchorPoint = value.toPointF();
    else if (property == "zOrder")      l.zOrder      = value.toInt();
    else if (property == "sourceType")  l.sourceType  = static_cast<LayerSourceType>(value.toInt());
    else if (property == "sourcePath")  l.sourcePath  = value.toString();
    else if (property == "solidColor")  l.solidColor  = value.value<QColor>();
    else if (property == "inPoint")     l.inPoint     = value.toDouble();
    else if (property == "outPoint")    l.outPoint    = value.toDouble();
    else return false;

    return true;
}

// ===== Blend math — single channel (normalised 0-1) =====

double LayerCompositor::blendChannel(double base, double top, BlendMode mode)
{
    switch (mode) {
        case BlendMode::Normal:
            return top;

        case BlendMode::Add:
            return qMin(base + top, 1.0);

        case BlendMode::Multiply:
            return base * top;

        case BlendMode::Screen:
            return 1.0 - (1.0 - base) * (1.0 - top);

        case BlendMode::Overlay:
            return base < 0.5
                ? 2.0 * base * top
                : 1.0 - 2.0 * (1.0 - base) * (1.0 - top);

        case BlendMode::SoftLight:
            // Pegtop formula
            return (1.0 - 2.0 * top) * base * base + 2.0 * top * base;

        case BlendMode::HardLight:
            return top < 0.5
                ? 2.0 * base * top
                : 1.0 - 2.0 * (1.0 - base) * (1.0 - top);

        case BlendMode::Difference:
            return qAbs(base - top);

        case BlendMode::Exclusion:
            return base + top - 2.0 * base * top;

        case BlendMode::ColorDodge:
            if (top >= 1.0) return 1.0;
            return qMin(base / (1.0 - top), 1.0);

        case BlendMode::ColorBurn:
            if (top <= 0.0) return 0.0;
            return qMax(1.0 - (1.0 - base) / top, 0.0);

        case BlendMode::Darken:
            return qMin(base, top);

        case BlendMode::Lighten:
            return qMax(base, top);
    }
    return top;
}

// ===== Pixel-level blending (ARGB) =====

QRgb LayerCompositor::blendPixel(QRgb base, QRgb top, BlendMode mode, double opacity)
{
    double topA = qAlpha(top) / 255.0 * opacity;
    if (topA <= 0.0)
        return base;

    double baseR = qRed(base)   / 255.0;
    double baseG = qGreen(base) / 255.0;
    double baseB = qBlue(base)  / 255.0;
    double baseA = qAlpha(base) / 255.0;

    double topR = qRed(top)   / 255.0;
    double topG = qGreen(top) / 255.0;
    double topB = qBlue(top)  / 255.0;

    // Blend each channel
    double blendR = blendChannel(baseR, topR, mode);
    double blendG = blendChannel(baseG, topG, mode);
    double blendB = blendChannel(baseB, topB, mode);

    // Composite with alpha
    double outA = topA + baseA * (1.0 - topA);
    if (outA <= 0.0)
        return qRgba(0, 0, 0, 0);

    double outR = (blendR * topA + baseR * baseA * (1.0 - topA)) / outA;
    double outG = (blendG * topA + baseG * baseA * (1.0 - topA)) / outA;
    double outB = (blendB * topA + baseB * baseA * (1.0 - topA)) / outA;

    return qRgba(
        qBound(0, static_cast<int>(outR * 255.0 + 0.5), 255),
        qBound(0, static_cast<int>(outG * 255.0 + 0.5), 255),
        qBound(0, static_cast<int>(outB * 255.0 + 0.5), 255),
        qBound(0, static_cast<int>(outA * 255.0 + 0.5), 255));
}

// ===== Image-level blending =====

QImage LayerCompositor::blendImages(const QImage &base, const QImage &top,
                                    BlendMode mode, double opacity)
{
    QImage result = base.convertToFormat(QImage::Format_ARGB32);
    QImage topImg = top.convertToFormat(QImage::Format_ARGB32);

    int w = qMin(result.width(),  topImg.width());
    int h = qMin(result.height(), topImg.height());

    for (int y = 0; y < h; ++y) {
        QRgb *baseLine = reinterpret_cast<QRgb *>(result.scanLine(y));
        const QRgb *topLine = reinterpret_cast<const QRgb *>(topImg.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            baseLine[x] = blendPixel(baseLine[x], topLine[x], mode, opacity);
        }
    }

    return result;
}

// ===== Transform a layer image =====

QImage LayerCompositor::transformLayer(const QImage &source, const CompositeLayer &layer,
                                       const QSize &canvasSize)
{
    QImage canvas(canvasSize, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QTransform transform;
    // Translate to position
    transform.translate(layer.position.x(), layer.position.y());
    // Translate to anchor, rotate, then translate back
    transform.translate(layer.anchorPoint.x(), layer.anchorPoint.y());
    transform.rotate(layer.rotation);
    transform.scale(layer.scale.x(), layer.scale.y());
    transform.translate(-layer.anchorPoint.x(), -layer.anchorPoint.y());

    painter.setTransform(transform);
    painter.drawImage(0, 0, source);
    painter.end();

    return canvas;
}

// ===== Composite all visible layers =====

QImage LayerCompositor::compositeFrame(const QVector<CompositeLayer> &layers,
                                       const QSize &canvasSize, double time)
{
    QImage canvas(canvasSize, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);

    // Sort by zOrder (bottom to top)
    QVector<CompositeLayer> sorted = layers;
    std::sort(sorted.begin(), sorted.end(),
              [](const CompositeLayer &a, const CompositeLayer &b) {
                  return a.zOrder < b.zOrder;
              });

    for (const auto &layer : sorted) {
        // Skip invisible layers
        if (!layer.visible)
            continue;

        // Skip layers outside their time range
        if (time < layer.inPoint)
            continue;
        if (layer.outPoint > 0.0 && time > layer.outPoint)
            continue;

        // Obtain the layer source image
        QImage layerImage;

        if (layer.sourceType == LayerSourceType::Solid) {
            // Generate a solid-colour layer
            layerImage = QImage(canvasSize, QImage::Format_ARGB32);
            layerImage.fill(layer.solidColor);
        } else if (layer.sourceType == LayerSourceType::Image) {
            // Load from disk
            layerImage = QImage(layer.sourcePath);
            if (layerImage.isNull())
                continue;
            layerImage = layerImage.convertToFormat(QImage::Format_ARGB32);
        } else {
            // Video / Shape / Text / Adjustment — placeholder: transparent
            // (actual frame retrieval is handled by the pipeline caller)
            continue;
        }

        // Apply transform (position, scale, rotation)
        QImage transformed = transformLayer(layerImage, layer, canvasSize);

        // Blend onto canvas
        canvas = blendImages(canvas, transformed, layer.blendMode, layer.opacity);
    }

    return canvas;
}

// ===== Serialisation =====

QJsonObject LayerCompositor::toJson() const
{
    QJsonObject obj;
    QJsonArray arr;
    for (const auto &l : m_layers)
        arr.append(l.toJson());
    obj["layers"] = arr;
    return obj;
}

LayerCompositor LayerCompositor::fromJson(const QJsonObject &obj)
{
    LayerCompositor comp;
    const QJsonArray arr = obj["layers"].toArray();
    for (const auto &v : arr)
        comp.m_layers.append(CompositeLayer::fromJson(v.toObject()));
    return comp;
}
