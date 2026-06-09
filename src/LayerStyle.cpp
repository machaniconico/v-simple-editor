#include "LayerStyle.h"

#include <QPainter>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

double clamp01(double value)
{
    return qBound(0.0, value, 1.0);
}

QString colorToJson(const QColor &color)
{
    return color.name(QColor::HexArgb);
}

QColor colorFromJson(const QJsonObject &obj, const char *key,
                     const QColor &fallback)
{
    const QColor parsed(
        obj.value(QString::fromLatin1(key)).toString(colorToJson(fallback)));
    return parsed.isValid() ? parsed : fallback;
}

QImage alphaMaskFor(const QImage &image)
{
    QImage mask = image.convertToFormat(QImage::Format_ARGB32);
    QPainter p(&mask);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(mask.rect(), Qt::white);
    p.end();
    return mask;
}

QImage colorizeAlpha(const QImage &alpha, const QColor &color, double opacity)
{
    QImage colored(alpha.size(), QImage::Format_ARGB32_Premultiplied);
    colored.fill(Qt::transparent);

    QPainter p(&colored);
    p.drawImage(0, 0, alpha);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    QColor c = color;
    c.setAlphaF(clamp01(color.alphaF() * opacity));
    p.fillRect(colored.rect(), c);
    p.end();

    return colored;
}

QImage boxBlurAlpha(const QImage &alpha, double radius)
{
    const int r = static_cast<int>(std::ceil(std::max(0.0, radius)));
    if (r <= 0 || alpha.isNull())
        return alpha;

    const QImage src = alpha.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    if (w <= 0 || h <= 0)
        return alpha;

    std::vector<int> horizontal(static_cast<std::size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            int count = 0;
            const int x0 = std::max(0, x - r);
            const int x1 = std::min(w - 1, x + r);
            for (int xx = x0; xx <= x1; ++xx) {
                sum += qAlpha(line[xx]);
                ++count;
            }
            horizontal[static_cast<std::size_t>(y) * w + x] =
                count > 0 ? sum / count : 0;
        }
    }

    QImage out(src.size(), QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            int count = 0;
            const int y0 = std::max(0, y - r);
            const int y1 = std::min(h - 1, y + r);
            for (int yy = y0; yy <= y1; ++yy) {
                sum += horizontal[static_cast<std::size_t>(yy) * w + x];
                ++count;
            }
            const int a = count > 0 ? sum / count : 0;
            dst[x] = qRgba(255, 255, 255, a);
        }
    }
    return out;
}

QImage outsideStrokeAlpha(const QImage &alpha, double width)
{
    const int r = static_cast<int>(std::ceil(std::max(0.0, width)));
    if (r <= 0 || alpha.isNull()) {
        QImage empty(alpha.size(), QImage::Format_ARGB32);
        empty.fill(Qt::transparent);
        return empty;
    }

    QImage expanded(alpha.size(), QImage::Format_ARGB32);
    expanded.fill(Qt::transparent);

    QPainter p(&expanded);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const double limitSq = width * width + 0.0001;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx == 0 && dy == 0)
                continue;
            if (static_cast<double>(dx * dx + dy * dy) > limitSq)
                continue;
            p.drawImage(dx, dy, alpha);
        }
    }
    p.end();

    const QImage srcAlpha = alpha.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < expanded.height(); ++y) {
        QRgb *strokeLine = reinterpret_cast<QRgb *>(expanded.scanLine(y));
        const QRgb *sourceLine =
            reinterpret_cast<const QRgb *>(srcAlpha.constScanLine(y));
        for (int x = 0; x < expanded.width(); ++x) {
            const int outsideAlpha =
                qMax(0, qAlpha(strokeLine[x]) - qAlpha(sourceLine[x]));
            strokeLine[x] = qRgba(255, 255, 255, outsideAlpha);
        }
    }

    return expanded;
}

} // namespace

bool LayerStyle::isIdentity() const
{
    return !dropShadowEnabled && !strokeEnabled;
}

QJsonObject LayerStyle::toJson() const
{
    QJsonObject obj;
    obj["dropShadowEnabled"] = dropShadowEnabled;
    obj["shadowColor"] = colorToJson(shadowColor);
    obj["shadowOffsetX"] = shadowOffset.x();
    obj["shadowOffsetY"] = shadowOffset.y();
    obj["shadowBlurRadius"] = shadowBlurRadius;
    obj["shadowOpacity"] = shadowOpacity;
    obj["strokeEnabled"] = strokeEnabled;
    obj["strokeColor"] = colorToJson(strokeColor);
    obj["strokeWidth"] = strokeWidth;
    return obj;
}

LayerStyle LayerStyle::fromJson(const QJsonObject &obj)
{
    LayerStyle s;
    s.dropShadowEnabled = obj["dropShadowEnabled"].toBool(false);
    s.shadowColor = colorFromJson(obj, "shadowColor", s.shadowColor);
    s.shadowOffset = QPointF(obj["shadowOffsetX"].toDouble(s.shadowOffset.x()),
                             obj["shadowOffsetY"].toDouble(s.shadowOffset.y()));
    s.shadowBlurRadius = obj["shadowBlurRadius"].toDouble(s.shadowBlurRadius);
    s.shadowOpacity = obj["shadowOpacity"].toDouble(s.shadowOpacity);
    s.strokeEnabled = obj["strokeEnabled"].toBool(false);
    s.strokeColor = colorFromJson(obj, "strokeColor", s.strokeColor);
    s.strokeWidth = obj["strokeWidth"].toDouble(s.strokeWidth);
    return s;
}

namespace layerstyle {

QImage apply(const QImage &layerRgba, const LayerStyle &s)
{
    if (s.isIdentity())
        return layerRgba;
    if (layerRgba.isNull())
        return QImage();

    const QImage source =
        layerRgba.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage alpha = alphaMaskFor(source);

    QImage result(source.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    QPainter p(&result);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    if (s.dropShadowEnabled) {
        const QImage shadowAlpha = boxBlurAlpha(alpha, s.shadowBlurRadius);
        const QImage shadow = colorizeAlpha(shadowAlpha, s.shadowColor,
                                            s.shadowOpacity);
        p.drawImage(s.shadowOffset, shadow);
    }

    if (s.strokeEnabled) {
        const QImage strokeAlpha = outsideStrokeAlpha(alpha, s.strokeWidth);
        const QImage stroke = colorizeAlpha(strokeAlpha, s.strokeColor, 1.0);
        p.drawImage(0, 0, stroke);
    }

    p.drawImage(0, 0, source);
    p.end();

    return result;
}

} // namespace layerstyle
