#include "BrushAnimation.h"

#include <QFontMetricsF>
#include <QLineF>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

constexpr double kMinBrushWidth = 0.1;
constexpr double kLengthEpsilon = 1.0e-6;

double clampUnit(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

double easeInOut(double value)
{
    const double t = clampUnit(value);
    return t * t * (3.0 - 2.0 * t);
}

QPointF lerpPoint(const QPointF &a, const QPointF &b, double t)
{
    return QPointF(a.x() + (b.x() - a.x()) * t,
                   a.y() + (b.y() - a.y()) * t);
}

double polygonLength(const QPolygonF &polygon)
{
    if (polygon.size() < 2) {
        return 0.0;
    }

    double length = 0.0;
    for (int i = 1; i < polygon.size(); ++i) {
        length += QLineF(polygon.at(i - 1), polygon.at(i)).length();
    }
    return length;
}

QPainterPath polygonToPath(const QPolygonF &polygon)
{
    QPainterPath path;
    if (polygon.isEmpty()) {
        return path;
    }

    path.moveTo(polygon.first());
    for (int i = 1; i < polygon.size(); ++i) {
        path.lineTo(polygon.at(i));
    }
    return path;
}

QPainterPath trimPolygon(const QPolygonF &polygon, double targetLength)
{
    QPainterPath path;
    if (polygon.size() < 2 || targetLength <= kLengthEpsilon) {
        return path;
    }

    path.moveTo(polygon.first());
    double remaining = targetLength;

    for (int i = 1; i < polygon.size(); ++i) {
        const QPointF start = polygon.at(i - 1);
        const QPointF end = polygon.at(i);
        const double segmentLength = QLineF(start, end).length();
        if (segmentLength <= kLengthEpsilon) {
            continue;
        }

        if (remaining >= segmentLength - kLengthEpsilon) {
            path.lineTo(end);
            remaining -= segmentLength;
            continue;
        }

        const double t = std::clamp(remaining / segmentLength, 0.0, 1.0);
        path.lineTo(lerpPoint(start, end, t));
        break;
    }

    return path;
}

double pathLength(const QPainterPath &path)
{
    double length = 0.0;
    const QList<QPolygonF> polygons = path.toSubpathPolygons();
    for (const QPolygonF &polygon : polygons) {
        length += polygonLength(polygon);
    }
    return length;
}

QPainterPath trimPath(const QPainterPath &path, double targetLength)
{
    QPainterPath trimmed;
    if (path.isEmpty() || targetLength <= kLengthEpsilon) {
        return trimmed;
    }

    double remaining = targetLength;
    const QList<QPolygonF> polygons = path.toSubpathPolygons();
    for (const QPolygonF &polygon : polygons) {
        const double subpathLength = polygonLength(polygon);
        if (subpathLength <= kLengthEpsilon) {
            continue;
        }

        if (remaining >= subpathLength - kLengthEpsilon) {
            trimmed.addPath(polygonToPath(polygon));
            remaining -= subpathLength;
            continue;
        }

        trimmed.addPath(trimPolygon(polygon, remaining));
        break;
    }

    return trimmed;
}

QPainterPath buildPerCharacterPath(const QVector<QPainterPath> &characterPaths,
                                   const QVector<double> &characterLengths,
                                   double progress)
{
    QPainterPath result;
    if (characterPaths.isEmpty()) {
        return result;
    }

    const double clampedProgress = clampUnit(progress);
    if (clampedProgress <= 0.0) {
        return result;
    }

    if (clampedProgress >= 1.0) {
        for (const QPainterPath &path : characterPaths) {
            result.addPath(path);
        }
        return result;
    }

    const double slotPosition = clampedProgress * static_cast<double>(characterPaths.size());
    const int activeIndex = std::min<int>(static_cast<int>(std::floor(slotPosition)),
                                          static_cast<int>(characterPaths.size()) - 1);
    const double localProgress = easeInOut(slotPosition - std::floor(slotPosition));

    for (int i = 0; i < activeIndex; ++i) {
        result.addPath(characterPaths.at(i));
    }

    if (activeIndex >= 0 && activeIndex < characterPaths.size()) {
        const double targetLength = characterLengths.value(activeIndex) * localProgress;
        result.addPath(trimPath(characterPaths.at(activeIndex), targetLength));
    }

    return result;
}

QPainterPath buildPerStrokePath(const QPainterPath &fullPath,
                                double totalLength,
                                double progress)
{
    if (fullPath.isEmpty() || totalLength <= kLengthEpsilon) {
        return QPainterPath();
    }

    const double clampedProgress = clampUnit(progress);
    if (clampedProgress <= 0.0) {
        return QPainterPath();
    }
    if (clampedProgress >= 1.0) {
        return fullPath;
    }
    return trimPath(fullPath, totalLength * clampedProgress);
}

QString modeToString(BrushAnimationMode mode)
{
    return mode == PerCharacter
        ? QStringLiteral("PerCharacter")
        : QStringLiteral("PerStroke");
}

BrushAnimationMode modeFromString(const QString &mode)
{
    if (mode == QLatin1String("PerCharacter")) {
        return PerCharacter;
    }
    return PerStroke;
}

} // namespace

BrushAnimation::BrushAnimation(QObject *parent)
    : QObject(parent)
{
}

void BrushAnimation::setText(const QString &text, const QFont &font, const QPointF &basePosition)
{
    m_text = text;
    m_font = font;
    m_basePosition = basePosition;
    rebuildGeometry();
}

void BrushAnimation::setBrushWidth(double width)
{
    m_brushWidth = std::max(width, kMinBrushWidth);
}

void BrushAnimation::setMode(BrushAnimationMode mode)
{
    m_mode = mode;
}

void BrushAnimation::setProgress(double progress)
{
    m_progress = clampUnit(progress);
}

double BrushAnimation::totalLength() const
{
    return m_totalLength;
}

QImage BrushAnimation::renderFrame(QSize canvasSize, double progress) const
{
    QImage image(canvasSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    if (!canvasSize.isValid() || m_totalLength <= kLengthEpsilon || m_text.isEmpty()) {
        return image;
    }

    const double clampedProgress = clampUnit(progress);
    if (clampedProgress <= 0.0) {
        return image;
    }

    QPainterPath sourcePath;
    if (m_mode == PerCharacter) {
        sourcePath = buildPerCharacterPath(m_characterPaths, m_characterLengths, clampedProgress);
    } else {
        sourcePath = buildPerStrokePath(m_fullPath, m_totalLength, clampedProgress);
    }

    if (sourcePath.isEmpty()) {
        return image;
    }

    QPainterPathStroker stroker;
    stroker.setWidth(std::max(m_brushWidth, kMinBrushWidth));
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setMiterLimit(4.0);

    const QPainterPath stroked = stroker.createStroke(sourcePath);
    if (stroked.isEmpty()) {
        return image;
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawPath(stroked);
    return image;
}

QJsonObject BrushAnimation::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("text")] = m_text;
    obj[QStringLiteral("fontFamily")] = m_font.family();
    obj[QStringLiteral("fontSize")] = m_font.pointSize();
    obj[QStringLiteral("fontPointSizeF")] = m_font.pointSizeF();
    obj[QStringLiteral("fontWeight")] = m_font.weight();
    obj[QStringLiteral("fontItalic")] = m_font.italic();
    obj[QStringLiteral("fontUnderline")] = m_font.underline();
    obj[QStringLiteral("basePositionX")] = m_basePosition.x();
    obj[QStringLiteral("basePositionY")] = m_basePosition.y();
    obj[QStringLiteral("brushWidth")] = m_brushWidth;
    obj[QStringLiteral("mode")] = modeToString(m_mode);
    obj[QStringLiteral("progress")] = m_progress;
    return obj;
}

void BrushAnimation::fromJson(const QJsonObject &obj)
{
    QFont font(obj.value(QStringLiteral("fontFamily")).toString(QStringLiteral("Arial")));

    const double pointSizeF = obj.value(QStringLiteral("fontPointSizeF")).toDouble(-1.0);
    const int pointSize = obj.value(QStringLiteral("fontSize")).toInt(32);
    if (pointSizeF > 0.0) {
        font.setPointSizeF(pointSizeF);
    } else if (pointSize > 0) {
        font.setPointSize(pointSize);
    }

    font.setWeight(static_cast<QFont::Weight>(
        obj.value(QStringLiteral("fontWeight")).toInt(font.weight())));
    font.setItalic(obj.value(QStringLiteral("fontItalic")).toBool(font.italic()));
    font.setUnderline(obj.value(QStringLiteral("fontUnderline")).toBool(font.underline()));

    setBrushWidth(obj.value(QStringLiteral("brushWidth")).toDouble(m_brushWidth));
    setMode(modeFromString(obj.value(QStringLiteral("mode")).toString()));
    setProgress(obj.value(QStringLiteral("progress")).toDouble(0.0));

    const QPointF position(
        obj.value(QStringLiteral("basePositionX")).toDouble(0.0),
        obj.value(QStringLiteral("basePositionY")).toDouble(0.0));

    setText(obj.value(QStringLiteral("text")).toString(), font, position);
}

void BrushAnimation::rebuildGeometry()
{
    m_fullPath = QPainterPath();
    m_characterPaths.clear();
    m_characterLengths.clear();
    m_totalLength = 0.0;

    if (m_text.isEmpty()) {
        return;
    }

    m_fullPath.addText(m_basePosition, m_font, m_text);
    const double fullPathLength = pathLength(m_fullPath);

    QFontMetricsF metrics(m_font);
    for (int i = 0; i < m_text.size(); ++i) {
        const double xOffset = metrics.horizontalAdvance(m_text.left(i));
        QPainterPath charPath;
        charPath.addText(QPointF(m_basePosition.x() + xOffset, m_basePosition.y()),
                         m_font,
                         m_text.mid(i, 1));

        const double charLength = pathLength(charPath);
        if (charLength <= kLengthEpsilon) {
            continue;
        }

        m_characterPaths.append(charPath);
        m_characterLengths.append(charLength);
    }

    m_totalLength = fullPathLength;
    if (m_totalLength <= kLengthEpsilon) {
        m_totalLength = std::accumulate(m_characterLengths.begin(),
                                        m_characterLengths.end(),
                                        0.0);
    }
}
