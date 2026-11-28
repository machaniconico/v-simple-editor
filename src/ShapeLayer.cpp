#include "ShapeLayer.h"
#include "WiggleTransform.h"

#include <QFont>
#include <QFontMetricsF>
#include <QLineF>
#include <QPolygonF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
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
        && trim.offsetPct == 0.0
        && !roundCorners.enabled && !zigZag.enabled && !wigglePath.enabled
        && !puckerBloat.enabled && !twist.enabled && !offsetPath.enabled;
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
    // New modifiers are written only while enabled so legacy JSON is unchanged.
    if (roundCorners.enabled)
        obj["roundCorners"] = QJsonObject{{"enabled", true}, {"radius", roundCorners.radius}};
    if (zigZag.enabled)
        obj["zigZag"] = QJsonObject{{"enabled", true}, {"size", zigZag.size},
                                    {"ridges", zigZag.ridgesPerSegment},
                                    {"smooth", zigZag.smooth}};
    if (wigglePath.enabled) {
        QJsonObject wp{{"enabled", true}, {"amplitude", wigglePath.amplitude},
                       {"detail", wigglePath.detail}, {"seed", wigglePath.seed}};
        if (wigglePath.wigglesPerSecond != 0.0)
            wp["wigglesPerSecond"] = wigglePath.wigglesPerSecond;
        obj["wigglePath"] = wp;
    }
    if (puckerBloat.enabled)
        obj["puckerBloat"] = QJsonObject{{"enabled", true}, {"amountPct", puckerBloat.amountPct}};
    if (twist.enabled)
        obj["twist"] = QJsonObject{{"enabled", true}, {"angleDeg", twist.angleDeg}};
    if (offsetPath.enabled)
        obj["offsetPath"] = QJsonObject{{"enabled", true}, {"amount", offsetPath.amount}};
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
    const QJsonObject rc = obj["roundCorners"].toObject();
    m.roundCorners.enabled = rc["enabled"].toBool(false);
    m.roundCorners.radius = qBound(0.0, rc["radius"].toDouble(10.0), 1000000.0);
    const QJsonObject zz = obj["zigZag"].toObject();
    m.zigZag.enabled = zz["enabled"].toBool(false);
    m.zigZag.size = qBound(-1000000.0, zz["size"].toDouble(10.0), 1000000.0);
    m.zigZag.ridgesPerSegment = qBound(1, zz["ridges"].toInt(4), 100);
    m.zigZag.smooth = zz["smooth"].toBool(false);
    const QJsonObject wp = obj["wigglePath"].toObject();
    m.wigglePath.enabled = wp["enabled"].toBool(false);
    m.wigglePath.amplitude = qBound(-1000000.0, wp["amplitude"].toDouble(5.0), 1000000.0);
    m.wigglePath.detail = qBound(1, wp["detail"].toInt(4), 100);
    m.wigglePath.seed = wp["seed"].toInt(1);
    m.wigglePath.wigglesPerSecond = qBound(0.0, wp["wigglesPerSecond"].toDouble(0.0), 100.0);
    const QJsonObject pb = obj["puckerBloat"].toObject();
    m.puckerBloat.enabled = pb["enabled"].toBool(false);
    m.puckerBloat.amountPct = qBound(-1000.0, pb["amountPct"].toDouble(0.0), 1000.0);
    const QJsonObject tw = obj["twist"].toObject();
    m.twist.enabled = tw["enabled"].toBool(false);
    m.twist.angleDeg = qBound(-100000.0, tw["angleDeg"].toDouble(30.0), 100000.0);
    const QJsonObject op = obj["offsetPath"].toObject();
    m.offsetPath.enabled = op["enabled"].toBool(false);
    m.offsetPath.amount = qBound(-1000000.0, op["amount"].toDouble(10.0), 1000000.0);
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

// ===== shapemod — path-to-path modifiers =====

namespace shapemod {
namespace {

constexpr double kFlatten = 16.0; // flatten at 1/16 px tolerance

// Flattened subpaths with consecutive duplicate points removed; closed ones
// drop the repeated end point and report closed = true.
struct Poly { QVector<QPointF> pts; bool closed = false; };

QVector<Poly> flatten(const QPainterPath &path)
{
    QVector<Poly> out;
    for (const QPolygonF &polygon : path.toSubpathPolygons(QTransform::fromScale(kFlatten, kFlatten))) {
        Poly p;
        for (const QPointF &pt : polygon) {
            const QPointF q = pt / kFlatten;
            if (p.pts.isEmpty() || QLineF(p.pts.last(), q).length() > 1e-9)
                p.pts.append(q);
        }
        if (p.pts.size() > 2 && QLineF(p.pts.first(), p.pts.last()).length() <= 1e-9) {
            p.pts.removeLast();
            p.closed = true;
        } else if (polygon.isClosed() && p.pts.size() > 2) {
            p.closed = true;
        }
        if (!p.pts.isEmpty()) out.append(p);
    }
    return out;
}

void appendPoly(QPainterPath &result, const QVector<QPointF> &pts, bool closed, bool smooth)
{
    if (pts.isEmpty()) return;
    result.moveTo(pts[0]);
    const int n = pts.size();
    if (!smooth || n < 3) {
        for (int i = 1; i < n; ++i) result.lineTo(pts[i]);
    } else {
        // Catmull-Rom through the points, expressed as cubic Beziers.
        const int segments = closed ? n : n - 1;
        for (int i = 0; i < segments; ++i) {
            auto at = [&](int k) {
                return closed ? pts[(k % n + n) % n] : pts[qBound(0, k, n - 1)];
            };
            const QPointF p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
            result.cubicTo(p1 + (p2 - p0) / 6.0, p2 - (p3 - p1) / 6.0, p2);
        }
    }
    if (closed) result.closeSubpath();
}

} // namespace

QPainterPath roundCorners(const QPainterPath &path, double radius)
{
    if (!(radius > 0.0) || !std::isfinite(radius)) return path;
    QPainterPath result;
    result.setFillRule(path.fillRule());
    for (const Poly &poly : flatten(path)) {
        const QVector<QPointF> &p = poly.pts;
        const int n = p.size();
        if (n < 3) {
            appendPoly(result, p, poly.closed, false);
            continue;
        }
        // Tangent points of vertex i: cut distance is capped at half of each
        // adjacent edge so neighbouring corners never overlap.
        auto cut = [&](int i, QPointF &in, QPointF &out) {
            const QPointF prev = p[(i + n - 1) % n], next = p[(i + 1) % n];
            const double dIn = QLineF(p[i], prev).length();
            const double dOut = QLineF(p[i], next).length();
            const double d = qMin(radius, qMin(dIn, dOut) / 2.0);
            in = p[i] + (prev - p[i]) * (d / dIn);
            out = p[i] + (next - p[i]) * (d / dOut);
        };
        auto corner = [&](int i) {
            QPointF in, out;
            cut(i, in, out);
            result.lineTo(in);
            result.cubicTo(in + (p[i] - in) * (2.0 / 3.0), out + (p[i] - out) * (2.0 / 3.0), out);
        };
        QPointF in0, out0;
        if (poly.closed) {
            cut(0, in0, out0);
            result.moveTo(out0);
            for (int i = 1; i < n; ++i) corner(i);
            result.lineTo(in0);
            result.cubicTo(in0 + (p[0] - in0) * (2.0 / 3.0), out0 + (p[0] - out0) * (2.0 / 3.0), out0);
            result.closeSubpath();
        } else {
            result.moveTo(p[0]);
            for (int i = 1; i < n - 1; ++i) corner(i);
            result.lineTo(p[n - 1]);
        }
    }
    return result;
}

QPainterPath zigZag(const QPainterPath &path, double size, int ridgesPerSegment, bool smooth)
{
    const int ridges = qBound(1, ridgesPerSegment, 100);
    if (!std::isfinite(size)) return path;
    QPainterPath result;
    result.setFillRule(path.fillRule());
    for (const Poly &poly : flatten(path)) {
        const QVector<QPointF> &p = poly.pts;
        const int n = p.size();
        const int segments = poly.closed ? n : n - 1;
        QVector<QPointF> pts;
        for (int i = 0; i < segments; ++i) {
            const QPointF a = p[i], b = p[(i + 1) % n];
            pts.append(a);
            const QLineF line(a, b);
            const QPointF normal(-(b.y() - a.y()) / line.length(), (b.x() - a.x()) / line.length());
            for (int k = 0; k < 2 * ridges; ++k) {
                const double t = (k + 0.5) / (2.0 * ridges);
                pts.append(a + (b - a) * t + normal * ((k % 2 == 0) ? size : -size));
            }
        }
        if (!poly.closed) pts.append(p.last());
        appendPoly(result, pts, poly.closed, smooth);
    }
    return result;
}

QPainterPath wigglePath(const QPainterPath &path, double amplitude, int detail, int seed,
                        double timeSec, double wigglesPerSecond)
{
    if (!std::isfinite(amplitude)) return path;
    const int parts = qBound(1, detail, 100);
    wiggle::WiggleParams params;
    params.enabled = true;
    params.positionFrequency = 1.0;
    params.positionAmplitude = QPointF(amplitude, amplitude);
    params.octaves = 2;
    params.seed = static_cast<unsigned int>(seed);
    // Time-varying part: 0.5 * (n2(counter * K + t * wps) - n2(counter * K)) on top of the
    // static n(counter), so total amplitude stays <= 2A and t == 0 / wps == 0 is the static path.
    // n2 is the separate series p2 = p with a shifted seed, so it does not replay the static pattern.
    wiggle::WiggleParams params2 = params;
    params2.seed = params.seed ^ 0x9E3779B9u;
    constexpr double kTimeStride = 1024.0;
    const double timeShift = std::isfinite(timeSec) && std::isfinite(wigglesPerSecond)
        ? timeSec * wigglesPerSecond : 0.0;
    QPainterPath result;
    result.setFillRule(path.fillRule());
    int counter = 0; // running vertex index = noise time, shared across subpaths
    for (const Poly &poly : flatten(path)) {
        const QVector<QPointF> &p = poly.pts;
        const int n = p.size();
        const int segments = poly.closed ? n : n - 1;
        QVector<QPointF> pts;
        for (int i = 0; i < segments; ++i) {
            const QPointF a = p[i], b = p[(i + 1) % n];
            for (int k = 0; k < parts; ++k)
                pts.append(a + (b - a) * (double(k) / parts));
        }
        if (!poly.closed) pts.append(p.last());
        for (QPointF &pt : pts) {
            pt += wiggle::evaluate(params, double(counter)).positionOffset;
            if (timeShift != 0.0)
                pt += 0.5 * (wiggle::evaluate(params2, double(counter) * kTimeStride + timeShift).positionOffset
                             - wiggle::evaluate(params2, double(counter) * kTimeStride).positionOffset);
            ++counter;
        }
        appendPoly(result, pts, poly.closed, false);
    }
    return result;
}

QPainterPath puckerBloat(const QPainterPath &path, double amountPct)
{
    if (path.isEmpty() || !std::isfinite(amountPct)) return path;
    const QPointF c = path.boundingRect().center();
    const double k = amountPct / 100.0;
    auto anchor = [&](const QPointF &p) { return c + (p - c) * (1.0 - k); };
    auto handle = [&](const QPointF &p) { return c + (p - c) * (1.0 + k); };
    QPainterPath result;
    result.setFillRule(path.fillRule());
    QList<QPainterPath> subpaths;
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        if (e.type == QPainterPath::MoveToElement) subpaths.append(QPainterPath());
        if (subpaths.isEmpty()) continue;
        if (e.type == QPainterPath::MoveToElement) subpaths.last().moveTo(e.x, e.y);
        else if (e.type == QPainterPath::LineToElement) subpaths.last().lineTo(e.x, e.y);
        else if (e.type == QPainterPath::CurveToElement && i + 2 < path.elementCount()) {
            const QPainterPath::Element e2 = path.elementAt(i + 1), e3 = path.elementAt(i + 2);
            subpaths.last().cubicTo(e.x, e.y, e2.x, e2.y, e3.x, e3.y);
            i += 2;
        }
    }
    for (const QPainterPath &sub : subpaths) {
        const int n = sub.elementCount();
        if (n == 0) continue;
        const bool closed = n > 2 && sub.elementAt(0).x == sub.elementAt(n - 1).x
            && sub.elementAt(0).y == sub.elementAt(n - 1).y;
        QPointF cur(sub.elementAt(0).x, sub.elementAt(0).y);
        result.moveTo(anchor(cur));
        for (int i = 1; i < n; ++i) {
            const QPainterPath::Element e = sub.elementAt(i);
            if (e.type == QPainterPath::CurveToElement && i + 2 < n) {
                const QPainterPath::Element e2 = sub.elementAt(i + 1), e3 = sub.elementAt(i + 2);
                const QPointF end(e3.x, e3.y);
                result.cubicTo(handle(QPointF(e.x, e.y)), handle(QPointF(e2.x, e2.y)), anchor(end));
                cur = end;
                i += 2;
            } else if (e.type == QPainterPath::LineToElement) {
                const QPointF end(e.x, e.y);
                // A straight segment becomes a cubic with handles at 1/3 and 2/3.
                result.cubicTo(handle(cur + (end - cur) / 3.0), handle(cur + (end - cur) * 2.0 / 3.0),
                               anchor(end));
                cur = end;
            }
        }
        if (closed) result.closeSubpath();
    }
    return result;
}

QPainterPath twist(const QPainterPath &path, double angleDeg)
{
    if (path.isEmpty() || !std::isfinite(angleDeg)) return path;
    const QPointF c = path.boundingRect().center();
    constexpr int kSplit = 8;
    QVector<Poly> polys = flatten(path);
    double radius = 0.0;
    for (const Poly &poly : polys)
        for (const QPointF &p : poly.pts)
            radius = std::max(radius, QLineF(c, p).length());
    if (!(radius > 0.0)) return path;
    QPainterPath result;
    result.setFillRule(path.fillRule());
    for (const Poly &poly : polys) {
        const int n = poly.pts.size();
        const int segments = poly.closed ? n : n - 1;
        QVector<QPointF> pts;
        for (int i = 0; i < segments; ++i) {
            const QPointF a = poly.pts[i], b = poly.pts[(i + 1) % n];
            for (int k = 0; k < kSplit; ++k)
                pts.append(a + (b - a) * (double(k) / kSplit));
        }
        if (!poly.closed) pts.append(poly.pts.last());
        for (QPointF &pt : pts) {
            const double d = QLineF(c, pt).length();
            const double rad = angleDeg * qMax(0.0, 1.0 - d / radius) * M_PI / 180.0;
            const QPointF v = pt - c;
            pt = c + QPointF(v.x() * std::cos(rad) - v.y() * std::sin(rad),
                             v.x() * std::sin(rad) + v.y() * std::cos(rad));
        }
        appendPoly(result, pts, poly.closed, false);
    }
    return result;
}

QPainterPath offsetPath(const QPainterPath &path, double amount)
{
    if (path.isEmpty() || !std::isfinite(amount) || amount == 0.0) return path;
    QPainterPathStroker stroker;
    // Drops points within 1e-3 px of the line through their neighbours (the stroker also emits
    // redundant collinear join points); this keeps the boolean cheap without a visible change.
    auto lean = [](const QPolygonF &poly) {
        QVector<QPointF> pts;
        for (const QPointF &pt : poly) {
            if (!pts.isEmpty() && QLineF(pts.last(), pt).length() < 1e-6) continue;
            while (pts.size() >= 2) {
                const QPointF a = pts[pts.size() - 2], b = pts.last();
                const double cross = (b.x() - a.x()) * (pt.y() - a.y()) - (b.y() - a.y()) * (pt.x() - a.x());
                // b must also project inside a..pt: a reversal or cusp is not redundant.
                const bool between = QPointF::dotProduct(b - a, pt - a) >= 0.0
                    && QPointF::dotProduct(b - pt, a - pt) >= 0.0;
                if (between && std::abs(cross) < 1e-3 * QLineF(a, pt).length()) pts.removeLast();
                else break;
            }
            pts.append(pt);
        }
        return pts;
    };
    QPainterPath closedPart, openPart;
    closedPart.setFillRule(path.fillRule());
    for (const Poly &poly : flatten(path)) {
        if (poly.closed) appendPoly(closedPart, lean(QPolygonF(poly.pts)), true, false);
        else appendPoly(openPart, poly.pts, false, false);
    }
    auto strokeOf = [&](const QPainterPath &p) {
        QPainterPath out;
        out.setFillRule(Qt::WindingFill);
        for (const QPolygonF &poly : stroker.createStroke(p).toSubpathPolygons(QTransform())) {
            const QVector<QPointF> pts = lean(poly);
            if (pts.size() > 2) {
                out.addPolygon(QPolygonF(pts));
                out.closeSubpath();
            }
        }
        return out;
    };
    stroker.setWidth(2.0 * std::abs(amount));
    stroker.setJoinStyle(Qt::MiterJoin);
    stroker.setMiterLimit(4.0);
    QPainterPath result;
    if (!closedPart.isEmpty()) {
        const QPainterPath band = strokeOf(closedPart);
        result = amount > 0.0 ? closedPart.united(band) : closedPart.subtracted(band);
    }
    if (!openPart.isEmpty()) {
        stroker.setCapStyle(Qt::FlatCap);
        result = result.isEmpty() ? strokeOf(openPart) : result.united(strokeOf(openPart));
    }
    return result;
}

} // namespace shapemod

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

void ShapeLayer::renderShape(const Shape &shape, QPainter &painter, double timeSec)
{
    painter.save();

    // Transform: translate, rotate, scale
    painter.translate(shape.position);
    painter.rotate(shape.rotation);
    painter.scale(shape.scale, shape.scale);

    QPainterPath path = buildShapePath(shape);
    if (shape.modifiers.roundCorners.enabled)
        path = shapemod::roundCorners(path, shape.modifiers.roundCorners.radius);
    if (shape.modifiers.zigZag.enabled) {
        const auto &z = shape.modifiers.zigZag;
        path = shapemod::zigZag(path, z.size, z.ridgesPerSegment, z.smooth);
    }
    if (shape.modifiers.wigglePath.enabled) {
        const auto &w = shape.modifiers.wigglePath;
        path = shapemod::wigglePath(path, w.amplitude, w.detail, w.seed, timeSec,
                                    w.wigglesPerSecond);
    }
    if (shape.modifiers.puckerBloat.enabled)
        path = shapemod::puckerBloat(path, shape.modifiers.puckerBloat.amountPct);
    if (shape.modifiers.twist.enabled)
        path = shapemod::twist(path, shape.modifiers.twist.angleDeg);
    if (shape.modifiers.offsetPath.enabled)
        path = shapemod::offsetPath(path, shape.modifiers.offsetPath.amount);
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
                                       const QSize &canvasSize,
                                       double timeSec)
{
    if (canvasSize.isEmpty())
        return QImage();

    QImage image(canvasSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const Shape &shape : shapes)
        renderShape(shape, painter, timeSec);

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
