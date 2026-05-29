#include "MaskSystem.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <algorithm>
#include <cmath>

// ============================================================
//  Enum <-> String helpers (file-local)
// ============================================================

static QString maskShapeToString(MaskShape s)
{
    switch (s) {
    case MaskShape::Rectangle: return QStringLiteral("rectangle");
    case MaskShape::Ellipse:   return QStringLiteral("ellipse");
    case MaskShape::Polygon:   return QStringLiteral("polygon");
    case MaskShape::Path:      return QStringLiteral("path");
    }
    return QStringLiteral("rectangle");
}

static MaskShape maskShapeFromString(const QString &s)
{
    if (s == QLatin1String("ellipse"))  return MaskShape::Ellipse;
    if (s == QLatin1String("polygon"))  return MaskShape::Polygon;
    if (s == QLatin1String("path"))     return MaskShape::Path;
    return MaskShape::Rectangle;
}

static QString maskModeToString(MaskMode m)
{
    switch (m) {
    case MaskMode::Add:        return QStringLiteral("add");
    case MaskMode::Subtract:   return QStringLiteral("subtract");
    case MaskMode::Intersect:  return QStringLiteral("intersect");
    case MaskMode::Difference: return QStringLiteral("difference");
    }
    return QStringLiteral("add");
}

static MaskMode maskModeFromString(const QString &s)
{
    if (s == QLatin1String("subtract"))   return MaskMode::Subtract;
    if (s == QLatin1String("intersect"))  return MaskMode::Intersect;
    if (s == QLatin1String("difference")) return MaskMode::Difference;
    return MaskMode::Add;
}

static QString featherDirToString(FeatherDirection d)
{
    switch (d) {
    case FeatherDirection::Both:    return QStringLiteral("both");
    case FeatherDirection::InOnly:  return QStringLiteral("in");
    case FeatherDirection::OutOnly: return QStringLiteral("out");
    }
    return QStringLiteral("both");
}

static FeatherDirection featherDirFromString(const QString &s)
{
    if (s == QLatin1String("in"))  return FeatherDirection::InOnly;
    if (s == QLatin1String("out")) return FeatherDirection::OutOnly;
    return FeatherDirection::Both;
}

static QString trackMatteTypeToString(TrackMatteType t)
{
    switch (t) {
    case TrackMatteType::None:               return QStringLiteral("none");
    case TrackMatteType::AlphaMatte:         return QStringLiteral("alpha");
    case TrackMatteType::AlphaInvertedMatte: return QStringLiteral("alpha_inverted");
    case TrackMatteType::LumaMatte:          return QStringLiteral("luma");
    case TrackMatteType::LumaInvertedMatte:  return QStringLiteral("luma_inverted");
    }
    return QStringLiteral("none");
}

static TrackMatteType trackMatteTypeFromString(const QString &s)
{
    if (s == QLatin1String("alpha"))          return TrackMatteType::AlphaMatte;
    if (s == QLatin1String("alpha_inverted")) return TrackMatteType::AlphaInvertedMatte;
    if (s == QLatin1String("luma"))           return TrackMatteType::LumaMatte;
    if (s == QLatin1String("luma_inverted"))  return TrackMatteType::LumaInvertedMatte;
    return TrackMatteType::None;
}

// ============================================================
//  Mask serialisation
// ============================================================

QJsonObject Mask::toJson() const
{
    QJsonObject obj;
    obj["shape"]    = maskShapeToString(shape);
    obj["mode"]     = maskModeToString(mode);
    obj["inverted"] = inverted;
    obj["opacity"]  = opacity;
    obj["expansion"] = expansion;
    obj["name"]     = name;

    // Feather
    QJsonObject fObj;
    fObj["amount"]    = feather.amount;
    fObj["direction"] = featherDirToString(feather.direction);
    obj["feather"]    = fObj;

    // Rect
    QJsonObject rObj;
    rObj["x"] = rect.x();
    rObj["y"] = rect.y();
    rObj["w"] = rect.width();
    rObj["h"] = rect.height();
    obj["rect"] = rObj;

    // Points
    QJsonArray pts;
    for (const auto &p : points) {
        QJsonObject pt;
        pt["x"] = p.x();
        pt["y"] = p.y();
        pts.append(pt);
    }
    obj["points"] = pts;

    return obj;
}

Mask Mask::fromJson(const QJsonObject &obj)
{
    Mask m;
    m.shape     = maskShapeFromString(obj["shape"].toString());
    m.mode      = maskModeFromString(obj["mode"].toString());
    m.inverted  = obj["inverted"].toBool(false);
    m.opacity   = obj["opacity"].toDouble(1.0);
    m.expansion = obj["expansion"].toDouble(0.0);
    m.name      = obj["name"].toString();

    // Feather
    QJsonObject fObj = obj["feather"].toObject();
    m.feather.amount    = fObj["amount"].toDouble(0.0);
    m.feather.direction = featherDirFromString(fObj["direction"].toString());

    // Rect
    QJsonObject rObj = obj["rect"].toObject();
    m.rect = QRectF(rObj["x"].toDouble(), rObj["y"].toDouble(),
                    rObj["w"].toDouble(), rObj["h"].toDouble());

    // Points
    QJsonArray pts = obj["points"].toArray();
    m.points.reserve(pts.size());
    for (const auto &val : pts) {
        QJsonObject pt = val.toObject();
        m.points.append(QPointF(pt["x"].toDouble(), pt["y"].toDouble()));
    }

    return m;
}

// ============================================================
//  TrackMatte serialisation
// ============================================================

QJsonObject TrackMatte::toJson() const
{
    QJsonObject obj;
    obj["type"]             = trackMatteTypeToString(type);
    obj["sourceLayerIndex"] = sourceLayerIndex;
    return obj;
}

TrackMatte TrackMatte::fromJson(const QJsonObject &obj)
{
    TrackMatte tm;
    tm.type             = trackMatteTypeFromString(obj["type"].toString());
    tm.sourceLayerIndex = obj["sourceLayerIndex"].toInt(-1);
    return tm;
}

// ============================================================
//  MaskSystem — mask management
// ============================================================

void MaskSystem::addMask(const Mask &mask)
{
    m_masks.append(mask);
}

void MaskSystem::removeMask(int index)
{
    if (index >= 0 && index < m_masks.size())
        m_masks.removeAt(index);
}

// ============================================================
//  MaskSystem — individual shape renderers
// ============================================================

QImage MaskSystem::renderRectMask(const QRectF &rect, const QSize &canvasSize,
                                  const MaskFeather &feather)
{
    QImage img(canvasSize, QImage::Format_Grayscale8);
    img.fill(0);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255));
    painter.drawRect(rect);
    painter.end();

    if (feather.amount > 0.0)
        img = featherMask(img, feather.amount);

    return img;
}

QImage MaskSystem::renderEllipseMask(const QRectF &rect, const QSize &canvasSize,
                                     const MaskFeather &feather)
{
    QImage img(canvasSize, QImage::Format_Grayscale8);
    img.fill(0);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255));
    painter.drawEllipse(rect);
    painter.end();

    if (feather.amount > 0.0)
        img = featherMask(img, feather.amount);

    return img;
}

QImage MaskSystem::renderPolygonMask(const QVector<QPointF> &points, const QSize &canvasSize,
                                     const MaskFeather &feather)
{
    QImage img(canvasSize, QImage::Format_Grayscale8);
    img.fill(0);

    if (points.size() < 3)
        return img;

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255));

    QPainterPath path;
    path.addPolygon(QPolygonF(points));
    path.closeSubpath();
    painter.drawPath(path);
    painter.end();

    if (feather.amount > 0.0)
        img = featherMask(img, feather.amount);

    return img;
}

// ============================================================
//  MaskSystem — feathering (iterative box blur)
// ============================================================

void MaskSystem::boxBlur(QImage &img, int radius)
{
    if (radius <= 0)
        return;

    const int w = img.width();
    const int h = img.height();
    if (w == 0 || h == 0)
        return;

    // Temporary buffer for the intermediate pass
    QImage tmp(w, h, QImage::Format_Grayscale8);

    // Horizontal pass
    for (int y = 0; y < h; ++y) {
        const uchar *srcRow = img.constScanLine(y);
        uchar *dstRow = tmp.scanLine(y);

        // Running sum for the first pixel
        int sum = 0;
        int count = 0;
        for (int kx = 0; kx <= radius; ++kx) {
            if (kx < w) { sum += srcRow[kx]; ++count; }
        }
        dstRow[0] = static_cast<uchar>(sum / count);

        // Slide the window across the row
        for (int x = 1; x < w; ++x) {
            int addIdx = x + radius;
            int remIdx = x - radius - 1;
            if (addIdx < w) { sum += srcRow[addIdx]; ++count; }
            if (remIdx >= 0) { sum -= srcRow[remIdx]; --count; }
            dstRow[x] = static_cast<uchar>(count > 0 ? sum / count : 0);
        }
    }

    // Vertical pass
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        int count = 0;
        for (int ky = 0; ky <= radius; ++ky) {
            if (ky < h) { sum += tmp.constScanLine(ky)[x]; ++count; }
        }
        img.scanLine(0)[x] = static_cast<uchar>(sum / count);

        for (int y = 1; y < h; ++y) {
            int addIdx = y + radius;
            int remIdx = y - radius - 1;
            if (addIdx < h) { sum += tmp.constScanLine(addIdx)[x]; ++count; }
            if (remIdx >= 0) { sum -= tmp.constScanLine(remIdx)[x]; --count; }
            img.scanLine(y)[x] = static_cast<uchar>(count > 0 ? sum / count : 0);
        }
    }
}

QImage MaskSystem::featherMask(const QImage &mask, double amount)
{
    if (amount <= 0.0)
        return mask;

    QImage result = mask;
    if (result.format() != QImage::Format_Grayscale8)
        result = result.convertToFormat(QImage::Format_Grayscale8);

    int radius = static_cast<int>(std::ceil(amount));

    // Three iterations of box blur approximate a Gaussian blur
    boxBlur(result, radius);
    boxBlur(result, radius);
    boxBlur(result, radius);

    return result;
}

// ============================================================
//  MaskSystem — mask combination
// ============================================================

void MaskSystem::combineMasks(QImage &base, const QImage &layer, MaskMode mode)
{
    const int w = base.width();
    const int h = base.height();

    for (int y = 0; y < h; ++y) {
        uchar *baseRow  = base.scanLine(y);
        const uchar *layerRow = layer.constScanLine(y);

        for (int x = 0; x < w; ++x) {
            int b = baseRow[x];
            int l = layerRow[x];

            switch (mode) {
            case MaskMode::Add:
                baseRow[x] = static_cast<uchar>(std::min(255, std::max(b, l)));
                break;
            case MaskMode::Subtract:
                baseRow[x] = static_cast<uchar>(std::max(0, b - l));
                break;
            case MaskMode::Intersect:
                baseRow[x] = static_cast<uchar>(std::min(b, l));
                break;
            case MaskMode::Difference:
                baseRow[x] = static_cast<uchar>(std::abs(b - l));
                break;
            }
        }
    }
}

// ============================================================
//  MaskSystem — generate composite mask from all masks
// ============================================================

QImage MaskSystem::generateMaskImage(const QVector<Mask> &masks, const QSize &canvasSize)
{
    if (masks.isEmpty() || canvasSize.isEmpty())
        return QImage();

    // Start with fully visible (white) canvas
    QImage result(canvasSize, QImage::Format_Grayscale8);
    result.fill(255);

    bool firstMask = true;

    for (const auto &mask : masks) {
        // Render the individual shape
        QImage layer;
        switch (mask.shape) {
        case MaskShape::Rectangle:
            layer = renderRectMask(mask.rect, canvasSize, mask.feather);
            break;
        case MaskShape::Ellipse:
            layer = renderEllipseMask(mask.rect, canvasSize, mask.feather);
            break;
        case MaskShape::Polygon:
        case MaskShape::Path:
            layer = renderPolygonMask(mask.points, canvasSize, mask.feather);
            break;
        }

        if (layer.isNull())
            continue;

        // Apply expansion (dilate / erode the mask)
        if (std::abs(mask.expansion) > 0.01) {
            // Positive expansion = grow mask (blur then threshold higher)
            // Negative expansion = shrink mask (blur then threshold lower)
            double blurAmt = std::abs(mask.expansion);
            QImage expanded = featherMask(layer, blurAmt);

            int threshold = mask.expansion > 0.0 ? 64 : 192;
            const int w = expanded.width();
            const int h = expanded.height();
            for (int y = 0; y < h; ++y) {
                uchar *row = expanded.scanLine(y);
                for (int x = 0; x < w; ++x)
                    row[x] = row[x] >= threshold ? 255 : 0;
            }

            // Re-apply feathering after expansion for smooth edges
            if (mask.feather.amount > 0.0)
                expanded = featherMask(expanded, mask.feather.amount);

            layer = expanded;
        }

        // Apply opacity
        if (mask.opacity < 1.0) {
            const int w = layer.width();
            const int h = layer.height();
            double op = qBound(0.0, mask.opacity, 1.0);
            for (int y = 0; y < h; ++y) {
                uchar *row = layer.scanLine(y);
                for (int x = 0; x < w; ++x)
                    row[x] = static_cast<uchar>(row[x] * op);
            }
        }

        // Invert if requested
        if (mask.inverted) {
            const int w = layer.width();
            const int h = layer.height();
            for (int y = 0; y < h; ++y) {
                uchar *row = layer.scanLine(y);
                for (int x = 0; x < w; ++x)
                    row[x] = 255 - row[x];
            }
        }

        // Combine with result
        if (firstMask) {
            // First mask replaces the canvas entirely
            result = layer;
            firstMask = false;
        } else {
            combineMasks(result, layer, mask.mode);
        }
    }

    return result;
}

// ============================================================
//  MaskSystem — apply mask to source image
// ============================================================

QImage MaskSystem::applyMask(const QImage &sourceImage, const QImage &maskImage)
{
    QImage src = sourceImage;
    if (src.format() != QImage::Format_ARGB32_Premultiplied)
        src = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage mask = maskImage;
    if (mask.format() != QImage::Format_Grayscale8)
        mask = mask.convertToFormat(QImage::Format_Grayscale8);

    // Scale mask to match source if dimensions differ
    if (mask.size() != src.size())
        mask = mask.scaled(src.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    const int w = src.width();
    const int h = src.height();

    QImage result(w, h, QImage::Format_ARGB32_Premultiplied);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcRow  = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const uchar *maskRow = mask.constScanLine(y);
        QRgb *dstRow = reinterpret_cast<QRgb *>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            int maskVal = maskRow[x];  // 0-255
            int a = qAlpha(srcRow[x]) * maskVal / 255;
            int r = qRed(srcRow[x])   * maskVal / 255;
            int g = qGreen(srcRow[x]) * maskVal / 255;
            int b = qBlue(srcRow[x])  * maskVal / 255;
            dstRow[x] = qRgba(r, g, b, a);
        }
    }

    return result;
}

// ============================================================
//  MaskSystem — track matte
// ============================================================

static int trackMatteLumaRec601(QRgb pixel)
{
    // AE/Premiere luma matte uses Rec.601 (ref: Adobe AE Luma Matte / Premiere Matte Luma docs; ITU-R BT.601-7 Sec. 2.5.1).
    return static_cast<int>(0.299 * qRed(pixel)
                          + 0.587 * qGreen(pixel)
                          + 0.114 * qBlue(pixel));
}

QImage MaskSystem::applyTrackMatte(const QImage &sourceImage,
                                   const QImage &matteImage,
                                   TrackMatteType matteType)
{
    if (matteType == TrackMatteType::None || sourceImage.isNull() || matteImage.isNull())
        return sourceImage;

    QImage src = sourceImage;
    if (src.format() != QImage::Format_ARGB32_Premultiplied)
        src = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage matte = matteImage;
    if (matte.format() != QImage::Format_ARGB32_Premultiplied)
        matte = matte.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    if (matte.size() != src.size())
        matte = matte.scaled(src.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QImage matteRgb;
    const bool useLuma = matteType == TrackMatteType::LumaMatte
                      || matteType == TrackMatteType::LumaInvertedMatte;
    if (useLuma)
        matteRgb = matte.convertToFormat(QImage::Format_ARGB32);

    const int w = src.width();
    const int h = src.height();

    // Extract matte channel into a grayscale image
    QImage matteMask(w, h, QImage::Format_Grayscale8);

    for (int y = 0; y < h; ++y) {
        const QRgb *matteRow = reinterpret_cast<const QRgb *>(matte.constScanLine(y));
        const QRgb *matteRgbRow = useLuma
            ? reinterpret_cast<const QRgb *>(matteRgb.constScanLine(y))
            : nullptr;
        uchar *maskRow = matteMask.scanLine(y);

        for (int x = 0; x < w; ++x) {
            int val = 0;

            switch (matteType) {
            case TrackMatteType::AlphaMatte:
                val = qAlpha(matteRow[x]);
                break;
            case TrackMatteType::AlphaInvertedMatte:
                val = 255 - qAlpha(matteRow[x]);
                break;
            case TrackMatteType::LumaMatte:
                val = trackMatteLumaRec601(matteRgbRow[x]);
                break;
            case TrackMatteType::LumaInvertedMatte:
                val = 255 - trackMatteLumaRec601(matteRgbRow[x]);
                break;
            case TrackMatteType::None:
                val = 255;
                break;
            }

            maskRow[x] = static_cast<uchar>(qBound(0, val, 255));
        }
    }

    return applyMask(src, matteMask);
}

// ============================================================
//  MaskSystem — serialisation
// ============================================================

QJsonObject MaskSystem::toJson() const
{
    QJsonObject obj;

    QJsonArray masksArr;
    for (const auto &m : m_masks)
        masksArr.append(m.toJson());
    obj["masks"] = masksArr;

    return obj;
}

void MaskSystem::fromJson(const QJsonObject &obj)
{
    m_masks.clear();

    QJsonArray masksArr = obj["masks"].toArray();
    m_masks.reserve(masksArr.size());
    for (const auto &val : masksArr)
        m_masks.append(Mask::fromJson(val.toObject()));
}
