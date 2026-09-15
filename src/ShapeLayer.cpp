#include "ShapeLayer.h"

#include <QFont>
#include <QFontMetricsF>
#include <QLineF>
#include <QPolygonF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTransform>
#include <QtMath>

#include <cmath>

// ===== Name helpers =====

QString Shape::typeName(ShapeType t)
{
    switch (t) {
        case ShapeType::Rectangle:   return "Rectangle";
        case ShapeType::RoundedRect: return "RoundedRect";
        case ShapeType::Ellipse:     return "Ellipse";
        case ShapeType::Polygon:     return "Polygon";
        case ShapeType::Star:        return "Star";
        case ShapeType::Line:        return "Line";
        case ShapeType::Arrow:       return "Arrow";
        case ShapeType::Bezier:      return "Bezier";
    }
    return "Rectangle";
}

ShapeType Shape::typeFromName(const QString &name)
{
    if (name == "Rectangle")   return ShapeType::Rectangle;
    if (name == "RoundedRect") return ShapeType::RoundedRect;
    if (name == "Ellipse")     return ShapeType::Ellipse;
    if (name == "Polygon")     return ShapeType::Polygon;
    if (name == "Star")        return ShapeType::Star;
    if (name == "Line")        return ShapeType::Line;
    if (name == "Arrow")       return ShapeType::Arrow;
    if (name == "Bezier")      return ShapeType::Bezier;
    return ShapeType::Rectangle;
}

static QString capName(StrokeCap cap)
{
    switch (cap) {
        case StrokeCap::Flat:   return "Flat";
        case StrokeCap::Round:  return "Round";
        case StrokeCap::Square: return "Square";
    }
    return "Round";
}

static StrokeCap capFromName(const QString &name)
{
    if (name == "Flat")   return StrokeCap::Flat;
    if (name == "Round")  return StrokeCap::Round;
    if (name == "Square") return StrokeCap::Square;
    return StrokeCap::Round;
}

static QString joinName(StrokeJoin join)
{
    switch (join) {
        case StrokeJoin::Miter: return "Miter";
        case StrokeJoin::Round: return "Round";
        case StrokeJoin::Bevel: return "Bevel";
    }
    return "Miter";
}

static StrokeJoin joinFromName(const QString &name)
{
    if (name == "Miter") return StrokeJoin::Miter;
    if (name == "Round") return StrokeJoin::Round;
    if (name == "Bevel") return StrokeJoin::Bevel;
    return StrokeJoin::Miter;
}

static Qt::PenCapStyle toQtCap(StrokeCap cap)
{
    switch (cap) {
        case StrokeCap::Flat:   return Qt::FlatCap;
        case StrokeCap::Round:  return Qt::RoundCap;
        case StrokeCap::Square: return Qt::SquareCap;
    }
    return Qt::RoundCap;
}

static Qt::PenJoinStyle toQtJoin(StrokeJoin join)
{
    switch (join) {
        case StrokeJoin::Miter: return Qt::MiterJoin;
        case StrokeJoin::Round: return Qt::RoundJoin;
        case StrokeJoin::Bevel: return Qt::BevelJoin;
    }
    return Qt::MiterJoin;
}

// ===== ShapeFill — serialisation =====

QJsonObject ShapeFill::toJson() const
{
    QJsonObject obj;
    obj["colorR"] = color.red();
    obj["colorG"] = color.green();
    obj["colorB"] = color.blue();
    obj["colorA"] = color.alpha();
    obj["opacity"] = opacity;
    obj["enabled"] = enabled;
    obj["gradient"] = gradient;
    obj["gradStartR"] = gradientStart.red();
    obj["gradStartG"] = gradientStart.green();
    obj["gradStartB"] = gradientStart.blue();
    obj["gradStartA"] = gradientStart.alpha();
    obj["gradEndR"] = gradientEnd.red();
    obj["gradEndG"] = gradientEnd.green();
    obj["gradEndB"] = gradientEnd.blue();
    obj["gradEndA"] = gradientEnd.alpha();
    obj["gradientAngle"] = gradientAngle;
    return obj;
}

ShapeFill ShapeFill::fromJson(const QJsonObject &obj)
{
    ShapeFill f;
    f.color = QColor(obj["colorR"].toInt(255), obj["colorG"].toInt(255),
                     obj["colorB"].toInt(255), obj["colorA"].toInt(255));
    f.opacity = obj["opacity"].toDouble(1.0);
    f.enabled = obj["enabled"].toBool(true);
    f.gradient = obj["gradient"].toBool(false);
    f.gradientStart = QColor(obj["gradStartR"].toInt(255), obj["gradStartG"].toInt(255),
                             obj["gradStartB"].toInt(255), obj["gradStartA"].toInt(255));
    f.gradientEnd = QColor(obj["gradEndR"].toInt(0), obj["gradEndG"].toInt(0),
                           obj["gradEndB"].toInt(0), obj["gradEndA"].toInt(255));
    f.gradientAngle = obj["gradientAngle"].toDouble(0.0);
    return f;
}

// ===== ShapeStroke — serialisation =====

QJsonObject ShapeStroke::toJson() const
{
    QJsonObject obj;
    obj["colorR"] = color.red();
    obj["colorG"] = color.green();
    obj["colorB"] = color.blue();
    obj["colorA"] = color.alpha();
    obj["width"] = width;
    obj["opacity"] = opacity;
    obj["enabled"] = enabled;
    obj["cap"] = capName(cap);
    obj["join"] = joinName(join);

    QJsonArray dashes;
    for (double d : dashPattern)
        dashes.append(d);
    obj["dashPattern"] = dashes;

    return obj;
}

ShapeStroke ShapeStroke::fromJson(const QJsonObject &obj)
{
    ShapeStroke s;
    s.color = QColor(obj["colorR"].toInt(0), obj["colorG"].toInt(0),
                     obj["colorB"].toInt(0), obj["colorA"].toInt(255));
    s.width = obj["width"].toDouble(2.0);
    s.opacity = obj["opacity"].toDouble(1.0);
    s.enabled = obj["enabled"].toBool(true);
    s.cap = capFromName(obj["cap"].toString("Round"));
    s.join = joinFromName(obj["join"].toString("Miter"));

    QJsonArray dashes = obj["dashPattern"].toArray();
    for (const QJsonValue &v : dashes)
        s.dashPattern.append(v.toDouble());

    return s;
}

// ===== ShapeProperties — serialisation =====

QJsonObject ShapeProperties::toJson() const
{
    QJsonObject obj;
    obj["sizeW"] = size.width();
    obj["sizeH"] = size.height();
    obj["cornerRadius"] = cornerRadius;
    obj["radius"] = radius;
    obj["sides"] = sides;
    obj["outerRadius"] = outerRadius;
    obj["innerRadius"] = innerRadius;
    obj["points"] = points;
    obj["startX"] = startPoint.x();
    obj["startY"] = startPoint.y();
    obj["endX"] = endPoint.x();
    obj["endY"] = endPoint.y();
    obj["headSize"] = headSize;

    QJsonArray cp;
    for (const QPointF &p : controlPoints) {
        QJsonObject pt;
        pt["x"] = p.x();
        pt["y"] = p.y();
        cp.append(pt);
    }
    obj["controlPoints"] = cp;

    return obj;
}

ShapeProperties ShapeProperties::fromJson(const QJsonObject &obj)
{
    ShapeProperties p;
    p.size = QSizeF(obj["sizeW"].toDouble(100.0), obj["sizeH"].toDouble(100.0));
    p.cornerRadius = obj["cornerRadius"].toDouble(0.0);
    p.radius = obj["radius"].toDouble(50.0);
    p.sides = qBound(3, obj["sides"].toInt(6), 20);
    p.outerRadius = obj["outerRadius"].toDouble(50.0);
    p.innerRadius = obj["innerRadius"].toDouble(25.0);
    p.points = qBound(3, obj["points"].toInt(5), 20);
    p.startPoint = QPointF(obj["startX"].toDouble(0.0), obj["startY"].toDouble(0.0));
    p.endPoint = QPointF(obj["endX"].toDouble(100.0), obj["endY"].toDouble(0.0));
    p.headSize = obj["headSize"].toDouble(15.0);

    QJsonArray cp = obj["controlPoints"].toArray();
    for (const QJsonValue &v : cp) {
        QJsonObject pt = v.toObject();
        p.controlPoints.append(QPointF(pt["x"].toDouble(), pt["y"].toDouble()));
    }

    return p;
}

// ===== ShapeModifiers — default-omitting serialisation =====

bool ShapeModifiers::isDefault() const
{
    return !repeater.enabled && repeater.copies == 3
        && repeater.offset == QPointF(40.0, 0.0) && repeater.rotationDeg == 0.0
        && repeater.scale == 1.0 && repeater.opacityEnd == 1.0
        && !trim.enabled && trim.startPct == 0.0 && trim.endPct == 100.0
        && trim.offsetPct == 0.0;
}

QJsonObject ShapeModifiers::toJson() const
{
    QJsonObject r;
    if (repeater.enabled) r["enabled"] = true;
    if (repeater.copies != 3) r["copies"] = repeater.copies;
    if (repeater.offset.x() != 40.0) r["offsetX"] = repeater.offset.x();
    if (repeater.offset.y() != 0.0) r["offsetY"] = repeater.offset.y();
    if (repeater.rotationDeg != 0.0) r["rotationDeg"] = repeater.rotationDeg;
    if (repeater.scale != 1.0) r["scale"] = repeater.scale;
    if (repeater.opacityEnd != 1.0) r["opacityEnd"] = repeater.opacityEnd;
    QJsonObject t;
    if (trim.enabled) t["enabled"] = true;
    if (trim.startPct != 0.0) t["startPct"] = trim.startPct;
    if (trim.endPct != 100.0) t["endPct"] = trim.endPct;
    if (trim.offsetPct != 0.0) t["offsetPct"] = trim.offsetPct;
    QJsonObject obj;
    if (!r.isEmpty()) obj["repeater"] = r;
    if (!t.isEmpty()) obj["trim"] = t;
    return obj;
}

ShapeModifiers ShapeModifiers::fromJson(const QJsonObject &obj)
{
    ShapeModifiers m;
    const QJsonObject r = obj["repeater"].toObject();
    m.repeater.enabled = r["enabled"].toBool(false);
    m.repeater.copies = qBound(1, r["copies"].toInt(3), 1000);
    m.repeater.offset = QPointF(r["offsetX"].toDouble(40.0), r["offsetY"].toDouble(0.0));
    m.repeater.rotationDeg = r["rotationDeg"].toDouble(0.0);
    m.repeater.scale = qBound(0.0, r["scale"].toDouble(1.0), 100.0);
    m.repeater.opacityEnd = qBound(0.0, r["opacityEnd"].toDouble(1.0), 1.0);
    const QJsonObject t = obj["trim"].toObject();
    m.trim.enabled = t["enabled"].toBool(false);
    m.trim.startPct = qBound(0.0, t["startPct"].toDouble(0.0), 100.0);
    m.trim.endPct = qBound(0.0, t["endPct"].toDouble(100.0), 100.0);
    m.trim.offsetPct = t["offsetPct"].toDouble(0.0);
    return m;
}

// ===== Shape — serialisation =====

QJsonObject Shape::toJson() const
{
    QJsonObject obj;
    obj["type"] = typeName(type);
    obj["properties"] = properties.toJson();
    obj["fill"] = fill.toJson();
    obj["stroke"] = stroke.toJson();
    obj["posX"] = position.x();
    obj["posY"] = position.y();
    obj["rotation"] = rotation;
    obj["scale"] = scale;
    obj["name"] = name;
    if (!modifiers.isDefault())
        obj["modifiers"] = modifiers.toJson();
    return obj;
}

Shape Shape::fromJson(const QJsonObject &obj)
{
    Shape s;
    s.type = typeFromName(obj["type"].toString("Rectangle"));
    s.properties = ShapeProperties::fromJson(obj["properties"].toObject());
    s.fill = ShapeFill::fromJson(obj["fill"].toObject());
    s.stroke = ShapeStroke::fromJson(obj["stroke"].toObject());
    s.position = QPointF(obj["posX"].toDouble(0.0), obj["posY"].toDouble(0.0));
    s.rotation = obj["rotation"].toDouble(0.0);
    s.scale = obj["scale"].toDouble(1.0);
    s.name = obj["name"].toString();
    s.modifiers = ShapeModifiers::fromJson(obj["modifiers"].toObject());
    return s;
}

// ===== ShapeLayer — shape management =====

void ShapeLayer::addShape(const Shape &shape)
{
    m_shapes.append(shape);
}

bool ShapeLayer::removeShape(int index)
{
    if (index < 0 || index >= m_shapes.size())
        return false;
    m_shapes.removeAt(index);
    return true;
}

// ===== ShapeLayer — path builders =====

QPainterPath ShapeLayer::buildShapePath(const Shape &shape)
{
    QPainterPath path;
    const ShapeProperties &props = shape.properties;

    switch (shape.type) {

    case ShapeType::Rectangle: {
        double hw = props.size.width() * 0.5;
        double hh = props.size.height() * 0.5;
        path.addRect(-hw, -hh, props.size.width(), props.size.height());
        break;
    }

    case ShapeType::RoundedRect: {
        double hw = props.size.width() * 0.5;
        double hh = props.size.height() * 0.5;
        path.addRoundedRect(-hw, -hh, props.size.width(), props.size.height(),
                            props.cornerRadius, props.cornerRadius);
        break;
    }

    case ShapeType::Ellipse: {
        double hw = props.size.width() * 0.5;
        double hh = props.size.height() * 0.5;
        path.addEllipse(QPointF(0, 0), hw, hh);
        break;
    }

    case ShapeType::Polygon: {
        int n = qBound(3, props.sides, 20);
        double r = props.radius;
        QPolygonF poly;
        for (int i = 0; i < n; ++i) {
            double angle = (2.0 * M_PI * i / n) - M_PI / 2.0; // start at top
            poly << QPointF(std::cos(angle) * r, std::sin(angle) * r);
        }
        path.addPolygon(poly);
        path.closeSubpath();
        break;
    }

    case ShapeType::Star: {
        int n = qBound(3, props.points, 20);
        double oR = props.outerRadius;
        double iR = props.innerRadius;
        QPolygonF star;
        for (int i = 0; i < n * 2; ++i) {
            double angle = (M_PI * i / n) - M_PI / 2.0; // start at top
            double r = (i % 2 == 0) ? oR : iR;
            star << QPointF(std::cos(angle) * r, std::sin(angle) * r);
        }
        path.addPolygon(star);
        path.closeSubpath();
        break;
    }

    case ShapeType::Line: {
        path.moveTo(props.startPoint);
        path.lineTo(props.endPoint);
        break;
    }

    case ShapeType::Arrow: {
        // Shaft
        path.moveTo(props.startPoint);
        path.lineTo(props.endPoint);

        // Arrowhead
        double dx = props.endPoint.x() - props.startPoint.x();
        double dy = props.endPoint.y() - props.startPoint.y();
        double angle = std::atan2(dy, dx);
        double hs = props.headSize;

        QPointF tip = props.endPoint;
        QPointF left(tip.x() - hs * std::cos(angle - M_PI / 6.0),
                     tip.y() - hs * std::sin(angle - M_PI / 6.0));
        QPointF right(tip.x() - hs * std::cos(angle + M_PI / 6.0),
                      tip.y() - hs * std::sin(angle + M_PI / 6.0));

        path.moveTo(left);
        path.lineTo(tip);
        path.lineTo(right);
        break;
    }

    case ShapeType::Bezier: {
        const QVector<QPointF> &cp = props.controlPoints;
        if (cp.size() < 2) break;

        path.moveTo(cp[0]);

        // Walk control points in groups of 3: ctrl1, ctrl2, endPt
        int i = 1;
        while (i + 2 < cp.size()) {
            path.cubicTo(cp[i], cp[i + 1], cp[i + 2]);
            i += 3;
        }

        // Remaining points: quadratic or line fallback
        if (i + 1 < cp.size()) {
            path.quadTo(cp[i], cp[i + 1]);
        } else if (i < cp.size()) {
            path.lineTo(cp[i]);
        }
        break;
    }

    } // switch

    return path;
}

// ===== ShapeLayer — fill / stroke helpers =====

QBrush ShapeLayer::createGradientBrush(const ShapeFill &fill, const QRectF &boundingRect)
{
    double rad = qDegreesToRadians(fill.gradientAngle);
    double cx = boundingRect.center().x();
    double cy = boundingRect.center().y();
    double halfDiag = std::sqrt(boundingRect.width() * boundingRect.width() +
                                boundingRect.height() * boundingRect.height()) * 0.5;

    QPointF start(cx - halfDiag * std::cos(rad),
                  cy - halfDiag * std::sin(rad));
    QPointF end(cx + halfDiag * std::cos(rad),
                cy + halfDiag * std::sin(rad));

    QLinearGradient grad(start, end);
    grad.setColorAt(0.0, fill.gradientStart);
    grad.setColorAt(1.0, fill.gradientEnd);

    return QBrush(grad);
}

void ShapeLayer::applyFill(QPainter &painter, const Shape &shape, const QRectF &bounds)
{
    if (!shape.fill.enabled) {
        painter.setBrush(Qt::NoBrush);
        return;
    }

    if (shape.fill.gradient) {
        QBrush brush = createGradientBrush(shape.fill, bounds);
        painter.setBrush(brush);
    } else {
        QColor c = shape.fill.color;
        c.setAlphaF(c.alphaF() * shape.fill.opacity);
        painter.setBrush(c);
    }
}

void ShapeLayer::applyStroke(QPainter &painter, const ShapeStroke &stroke)
{
    if (!stroke.enabled || stroke.width <= 0.0) {
        painter.setPen(Qt::NoPen);
        return;
    }

    QColor c = stroke.color;
    c.setAlphaF(c.alphaF() * stroke.opacity);

    QPen pen(c, stroke.width);
    pen.setCapStyle(toQtCap(stroke.cap));
    pen.setJoinStyle(toQtJoin(stroke.join));

    if (!stroke.dashPattern.isEmpty())
        pen.setDashPattern(stroke.dashPattern);

    painter.setPen(pen);
}

// ===== ShapeLayer — rendering =====

QPainterPath ShapeLayer::trimPathRange(const QPainterPath &path, double startPct,
                                        double endPct, double offsetPct)
{
    if (!std::isfinite(startPct) || !std::isfinite(endPct) || !std::isfinite(offsetPct))
        return QPainterPath();
    const double start = qBound(0.0, startPct, 100.0);
    const double end = qBound(0.0, endPct, 100.0);
    // Reversed start/end select the same interval. Only offset wraps.
    const double span = std::abs(end - start) / 100.0;
    if (span >= 1.0) return path;
    if (span <= 0.0 || path.isEmpty()) return QPainterPath();

    // Flatten in a magnified coordinate system for subpixel curve accuracy.
    // Keep subpaths separate: moveTo gaps must never count towards path length.
    const auto polygons = path.toSubpathPolygons(QTransform::fromScale(16.0, 16.0));
    double total = 0.0;
    for (const auto &polygon : polygons)
        for (int i = 1; i < polygon.size(); ++i)
            total += QLineF(polygon[i - 1], polygon[i]).length();
    if (total <= 0.0 || !std::isfinite(total)) return QPainterPath();

    double from = std::fmod(qMin(start, end) / 100.0
                            + std::fmod(offsetPct, 100.0) / 100.0, 1.0);
    if (from < 0.0) from += 1.0;
    QPainterPath result;
    result.setFillRule(path.fillRule());
    auto appendRange = [&](double low, double high) {
        double cursor = 0.0;
        for (const auto &polygon : polygons) {
            bool started = false;
            for (int i = 1; i < polygon.size(); ++i) {
                const QPointF a = polygon[i - 1];
                const QPointF b = polygon[i];
                const double length = QLineF(a, b).length();
                const double lo = qMax(low, cursor);
                const double hi = qMin(high, cursor + length);
                if (length > 0.0 && hi > lo) {
                    const QPointF first = (a + (b - a) * ((lo - cursor) / length)) / 16.0;
                    const QPointF last = (a + (b - a) * ((hi - cursor) / length)) / 16.0;
                    if (!started) { result.moveTo(first); started = true; }
                    result.lineTo(last);
                }
                cursor += length;
            }
        }
    };
    appendRange(from * total, qMin(1.0, from + span) * total);
    if (from + span > 1.0) appendRange(0.0, (from + span - 1.0) * total);
    return result;
}

void ShapeLayer::renderShape(const Shape &shape, QPainter &painter)
{
    painter.save();

    // Transform: translate, rotate, scale
    painter.translate(shape.position);
    painter.rotate(shape.rotation);
    painter.scale(shape.scale, shape.scale);

    QPainterPath path = buildShapePath(shape);
    if (shape.modifiers.trim.enabled) {
        const auto &trim = shape.modifiers.trim;
        path = trimPathRange(path, trim.startPct, trim.endPct, trim.offsetPct);
    }
    if (shape.modifiers.repeater.enabled) {
        const auto &r = shape.modifiers.repeater;
        const int copies = qBound(1, r.copies, 1000);
        const double baseOpacity = painter.opacity();
        for (int i = 0; i < copies; ++i) {
            painter.save();
            const double fraction = copies > 1 ? double(i) / (copies - 1) : 0.0;
            painter.setOpacity(baseOpacity * (1.0 + (qBound(0.0, r.opacityEnd, 1.0) - 1.0) * fraction));
            applyFill(painter, shape, path.boundingRect());
            applyStroke(painter, shape.stroke);
            if (shape.type == ShapeType::Line || shape.type == ShapeType::Arrow)
                painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            painter.restore();
            // Repeated composition: each copy inherits every preceding step.
            painter.translate(r.offset);
            painter.rotate(r.rotationDeg);
            painter.scale(r.scale, r.scale);
            const QTransform t = painter.worldTransform();
            if (!std::isfinite(t.m11()) || !std::isfinite(t.m12())
                || !std::isfinite(t.m21()) || !std::isfinite(t.m22())
                || !std::isfinite(t.dx()) || !std::isfinite(t.dy()))
                break;
        }
        painter.restore();
        return;
    }
    QRectF bounds = path.boundingRect();

    // Fill
    applyFill(painter, shape, bounds);

    // Stroke
    applyStroke(painter, shape.stroke);

    // Draw
    if (shape.type == ShapeType::Line || shape.type == ShapeType::Arrow) {
        // Lines/arrows: stroke only (no fill for the path itself)
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    } else {
        painter.drawPath(path);
    }

    painter.restore();
}

QImage ShapeLayer::renderShapes(const QSize &canvasSize) const
{
    return renderShapesToImage(m_shapes, canvasSize);
}

QImage ShapeLayer::renderShapesToImage(const QVector<Shape> &shapes,
                                       const QSize &canvasSize)
{
    if (canvasSize.isEmpty())
        return QImage();

    QImage image(canvasSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const Shape &shape : shapes)
        renderShape(shape, painter);

    painter.end();
    return image;
}

// ===== ShapeLayer — presets =====

Shape ShapeLayer::createRectangle(const QSizeF &size,
                                  const ShapeFill &fill,
                                  const ShapeStroke &stroke)
{
    Shape s;
    s.type = ShapeType::Rectangle;
    s.properties.size = size;
    s.fill = fill;
    s.stroke = stroke;
    s.name = "Rectangle";
    return s;
}

Shape ShapeLayer::createCircle(double radius,
                               const ShapeFill &fill,
                               const ShapeStroke &stroke)
{
    Shape s;
    s.type = ShapeType::Ellipse;
    s.properties.size = QSizeF(radius * 2.0, radius * 2.0);
    s.fill = fill;
    s.stroke = stroke;
    s.name = "Circle";
    return s;
}

Shape ShapeLayer::createStar(int points, double outerR, double innerR,
                             const ShapeFill &fill,
                             const ShapeStroke &stroke)
{
    Shape s;
    s.type = ShapeType::Star;
    s.properties.points = qBound(3, points, 20);
    s.properties.outerRadius = outerR;
    s.properties.innerRadius = innerR;
    s.fill = fill;
    s.stroke = stroke;
    s.name = "Star";
    return s;
}

Shape ShapeLayer::createArrow(const QPointF &start, const QPointF &end,
                              double headSize,
                              const ShapeStroke &stroke)
{
    Shape s;
    s.type = ShapeType::Arrow;
    s.properties.startPoint = start;
    s.properties.endPoint = end;
    s.properties.headSize = headSize;
    s.fill.enabled = false;
    s.stroke = stroke;
    s.stroke.enabled = true;
    s.name = "Arrow";
    return s;
}

Shape ShapeLayer::createCallout(const QString &text, const QColor &bgColor)
{
    // Measure text to size the rounded rect
    QFont font("Arial", 18, QFont::Bold);
    QFontMetricsF fm(font);
    QRectF textRect = fm.boundingRect(text);

    double padH = 24.0;
    double padV = 16.0;
    double w = textRect.width() + padH * 2.0;
    double h = textRect.height() + padV * 2.0;

    Shape s;
    s.type = ShapeType::RoundedRect;
    s.properties.size = QSizeF(w, h);
    s.properties.cornerRadius = 12.0;

    s.fill.color = bgColor;
    s.fill.enabled = true;
    s.fill.opacity = 1.0;

    s.stroke.color = Qt::white;
    s.stroke.width = 1.5;
    s.stroke.enabled = true;

    s.name = text.left(20);
    return s;
}

// ===== ShapeLayer — serialisation =====

QJsonObject ShapeLayer::toJson() const
{
    QJsonObject obj;

    QJsonArray arr;
    for (const Shape &s : m_shapes)
        arr.append(s.toJson());
    obj["shapes"] = arr;

    return obj;
}

ShapeLayer ShapeLayer::fromJson(const QJsonObject &obj)
{
    ShapeLayer layer;

    QJsonArray arr = obj["shapes"].toArray();
    for (const QJsonValue &v : arr)
        layer.m_shapes.append(Shape::fromJson(v.toObject()));

    return layer;
}
