#include "TextPathWarp.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPolygonF>
#include <QStringList>
#include <QTransform>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEpsilon = 1.0e-6;
constexpr double kMinTaperScale = 0.05;

struct PathElementData {
    QPainterPath::ElementType type = QPainterPath::MoveToElement;
    QPointF point;
};

struct SubpathData {
    QVector<PathElementData> elements;
    QPainterPath path;
    QRectF bounds;
    QPointF centroid;
};

double degreesToRadians(double degrees)
{
    return degrees * kPi / 180.0;
}

double safeWidth(const QRectF &rect)
{
    return std::max(rect.width(), kEpsilon);
}

QPainterPath buildPathFromElements(const QVector<PathElementData> &elements)
{
    QPainterPath path;
    for (int i = 0; i < elements.size(); ++i) {
        const PathElementData &element = elements.at(i);
        switch (element.type) {
        case QPainterPath::MoveToElement:
            path.moveTo(element.point);
            break;
        case QPainterPath::LineToElement:
            path.lineTo(element.point);
            break;
        case QPainterPath::CurveToElement:
            if (i + 2 >= elements.size()) {
                break;
            }

            path.cubicTo(element.point,
                         elements.at(i + 1).point,
                         elements.at(i + 2).point);
            i += 2;
            break;
        case QPainterPath::CurveToDataElement:
            break;
        }
    }
    return path;
}

QPointF polygonCentroid(const QPolygonF &polygon, const QRectF &fallbackBounds)
{
    if (polygon.size() < 3) {
        return fallbackBounds.center();
    }

    double signedArea = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &a = polygon.at(i);
        const QPointF &b = polygon.at((i + 1) % polygon.size());
        const double cross = (a.x() * b.y()) - (b.x() * a.y());
        signedArea += cross;
        cx += (a.x() + b.x()) * cross;
        cy += (a.y() + b.y()) * cross;
    }

    signedArea *= 0.5;
    if (std::abs(signedArea) <= kEpsilon) {
        return fallbackBounds.center();
    }

    const double scale = 1.0 / (6.0 * signedArea);
    return QPointF(cx * scale, cy * scale);
}

QPointF pathCentroid(const QPainterPath &path, const QRectF &bounds)
{
    return polygonCentroid(path.toFillPolygon(), bounds);
}

QVector<SubpathData> splitIntoSubpaths(const QPainterPath &path)
{
    QVector<SubpathData> subpaths;
    QVector<PathElementData> currentElements;

    auto flushCurrent = [&subpaths, &currentElements]() {
        if (currentElements.isEmpty()) {
            return;
        }

        SubpathData subpath;
        subpath.elements = currentElements;
        subpath.path = buildPathFromElements(currentElements);
        subpath.bounds = subpath.path.boundingRect();
        subpath.centroid = pathCentroid(subpath.path, subpath.bounds);
        subpaths.push_back(subpath);
        currentElements.clear();
    };

    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        if (element.type == QPainterPath::MoveToElement && !currentElements.isEmpty()) {
            flushCurrent();
        }

        currentElements.push_back(PathElementData{
            element.type,
            QPointF(element.x, element.y)
        });
    }

    flushCurrent();
    return subpaths;
}

double normalizedX(const QPointF &point, const QRectF &bounds)
{
    if (bounds.width() <= kEpsilon) {
        return 0.5;
    }

    return std::clamp((point.x() - bounds.left()) / safeWidth(bounds), 0.0, 1.0);
}

QPointF warpPoint(const QPointF &point,
                  const QRectF &bounds,
                  const QPointF &centroid,
                  double bendDegrees,
                  double inflateAmount,
                  double twistDegrees,
                  double taperAmount)
{
    const QPointF boundsCenter = bounds.center();
    const double xNorm = normalizedX(point, bounds);
    const double xCentered = xNorm - 0.5;
    const double yRelative = point.y() - boundsCenter.y();

    QPointF warped = point;

    if (std::abs(bendDegrees) > kEpsilon) {
        const double magnitude = degreesToRadians(std::abs(bendDegrees));
        const double bendSign = bendDegrees < 0.0 ? -1.0 : 1.0;
        const double theta = xCentered * magnitude;
        const double radius = safeWidth(bounds) / std::max(magnitude, kEpsilon);
        const double arcX = std::sin(theta) * radius;
        const double arcY = bendSign * (1.0 - std::cos(theta)) * radius;
        const double normalX = bendSign * std::sin(theta);
        const double normalY = -std::cos(theta);

        warped.setX(boundsCenter.x() + arcX - yRelative * normalX);
        warped.setY(boundsCenter.y() + arcY - yRelative * normalY);
    }

    const double inflateScale = qFuzzyIsNull(inflateAmount) ? 1.0 : inflateAmount;
    if (std::abs(inflateScale - 1.0) > kEpsilon) {
        const QPointF delta = warped - centroid;
        warped = centroid + QPointF(delta.x() * inflateScale,
                                    delta.y() * inflateScale);
    }

    if (std::abs(twistDegrees) > kEpsilon) {
        const double angle = degreesToRadians(twistDegrees * xCentered);
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        const QPointF delta = warped - boundsCenter;
        warped = boundsCenter + QPointF((delta.x() * cosine) - (delta.y() * sine),
                                        (delta.x() * sine) + (delta.y() * cosine));
    }

    if (std::abs(taperAmount) > kEpsilon) {
        const double scaleY = std::max(kMinTaperScale,
                                       1.0 + taperAmount * ((2.0 * xNorm) - 1.0));
        warped.setY(boundsCenter.y() + (warped.y() - boundsCenter.y()) * scaleY);
    }

    return warped;
}

QTransform centeredTransform(const QPainterPath &path, const QSize &size)
{
    const QRectF bounds = path.boundingRect();
    QTransform transform;
    transform.translate((static_cast<double>(size.width()) - bounds.width()) * 0.5 - bounds.left(),
                        (static_cast<double>(size.height()) - bounds.height()) * 0.5 - bounds.top());
    return transform;
}

} // namespace

TextPathWarp::TextPathWarp(QObject *parent)
    : QObject(parent)
{
}

void TextPathWarp::setText(const QString &text, const QFont &font)
{
    m_text = text;
    m_font = font;
    rebuildBasePath();
}

void TextPathWarp::setBendDegrees(double bendDegrees)
{
    m_bendDegrees = std::isfinite(bendDegrees) ? bendDegrees : 0.0;
}

void TextPathWarp::setInflateAmount(double inflateAmount)
{
    m_inflateAmount = std::isfinite(inflateAmount) ? inflateAmount : 0.0;
}

void TextPathWarp::setTwistDegrees(double twistDegrees)
{
    m_twistDegrees = std::isfinite(twistDegrees) ? twistDegrees : 0.0;
}

void TextPathWarp::setTaperAmount(double taperAmount)
{
    m_taperAmount = std::isfinite(taperAmount) ? taperAmount : 0.0;
}

QImage TextPathWarp::renderFrame(QSize size) const
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    if (!size.isValid() || m_text.isEmpty() || m_basePath.isEmpty()) {
        return image;
    }

    const QPainterPath renderPath = hasActiveWarp() ? buildWarpedPath() : m_basePath;
    if (renderPath.isEmpty()) {
        return image;
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.setWorldTransform(centeredTransform(renderPath, size));
    painter.drawPath(renderPath);
    return image;
}

QJsonObject TextPathWarp::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("text")] = m_text;
    obj[QStringLiteral("font")] = m_font.toString();
    obj[QStringLiteral("bendDegrees")] = m_bendDegrees;
    obj[QStringLiteral("inflateAmount")] = m_inflateAmount;
    obj[QStringLiteral("twistDegrees")] = m_twistDegrees;
    obj[QStringLiteral("taperAmount")] = m_taperAmount;
    return obj;
}

void TextPathWarp::fromJson(const QJsonObject &obj)
{
    QFont font = m_font;
    const QString fontDescriptor = obj[QStringLiteral("font")].toString();
    if (!fontDescriptor.isEmpty()) {
        font.fromString(fontDescriptor);
    }

    setText(obj[QStringLiteral("text")].toString(), font);
    setBendDegrees(obj[QStringLiteral("bendDegrees")].toDouble(0.0));
    setInflateAmount(obj[QStringLiteral("inflateAmount")].toDouble(0.0));
    setTwistDegrees(obj[QStringLiteral("twistDegrees")].toDouble(0.0));
    setTaperAmount(obj[QStringLiteral("taperAmount")].toDouble(0.0));
}

void TextPathWarp::rebuildBasePath()
{
    m_basePath = buildTextPath();
}

QPainterPath TextPathWarp::buildTextPath() const
{
    QPainterPath path;
    if (m_text.isEmpty()) {
        return path;
    }

    QFont font = m_font;
    if (font.family().isEmpty()) {
        font.setFamily(QStringLiteral("Sans Serif"));
    }
    if (font.pointSizeF() <= 0.0 && font.pixelSize() <= 0) {
        font.setPointSize(32);
    }

    const QFontMetricsF metrics(font);
    const QStringList lines = m_text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    double baselineY = metrics.ascent();
    const double lineAdvance = std::max(1.0, metrics.lineSpacing());

    for (const QString &line : lines) {
        path.addText(QPointF(0.0, baselineY), font, line);
        baselineY += lineAdvance;
    }

    return path;
}

QPainterPath TextPathWarp::buildWarpedPath() const
{
    QPainterPath warpedPath;
    warpedPath.setFillRule(m_basePath.fillRule());

    const QRectF warpBounds = m_basePath.boundingRect();
    const QPointF warpCentroid = pathCentroid(m_basePath, warpBounds);
    const QVector<SubpathData> subpaths = splitIntoSubpaths(m_basePath);
    for (const SubpathData &subpath : subpaths) {
        QVector<PathElementData> warpedElements = subpath.elements;
        for (PathElementData &element : warpedElements) {
            element.point = warpPoint(element.point,
                                      warpBounds,
                                      warpCentroid,
                                      m_bendDegrees,
                                      m_inflateAmount,
                                      m_twistDegrees,
                                      m_taperAmount);
        }

        warpedPath.addPath(buildPathFromElements(warpedElements));
    }

    return warpedPath;
}

bool TextPathWarp::hasActiveWarp() const
{
    const bool inflateActive = !qFuzzyIsNull(m_inflateAmount)
        && std::abs(m_inflateAmount - 1.0) > kEpsilon;

    return std::abs(m_bendDegrees) > kEpsilon
        || inflateActive
        || std::abs(m_twistDegrees) > kEpsilon
        || std::abs(m_taperAmount) > kEpsilon;
}
