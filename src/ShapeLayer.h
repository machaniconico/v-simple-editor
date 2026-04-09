#pragma once

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>

// --- Shape Type ---

enum class ShapeType {
    Rectangle,
    RoundedRect,
    Ellipse,
    Polygon,
    Star,
    Line,
    Arrow,
    Bezier
};

// --- Stroke Cap / Join ---

enum class StrokeCap {
    Flat,
    Round,
    Square
};

enum class StrokeJoin {
    Miter,
    Round,
    Bevel
};

// --- Shape Fill ---

struct ShapeFill {
    QColor color = Qt::white;
    double opacity = 1.0;
    bool enabled = true;

    bool gradient = false;
    QColor gradientStart = Qt::white;
    QColor gradientEnd = Qt::black;
    double gradientAngle = 0.0;   // degrees

    QJsonObject toJson() const;
    static ShapeFill fromJson(const QJsonObject &obj);
};

// --- Shape Stroke ---

struct ShapeStroke {
    QColor color = Qt::black;
    double width = 2.0;
    double opacity = 1.0;
    bool enabled = true;

    QVector<double> dashPattern;  // e.g. {10, 5} for dashed lines
    StrokeCap cap = StrokeCap::Round;
    StrokeJoin join = StrokeJoin::Miter;

    QJsonObject toJson() const;
    static ShapeStroke fromJson(const QJsonObject &obj);
};

// --- Shape Properties (union-like, depends on ShapeType) ---

struct ShapeProperties {
    // Rectangle / RoundedRect / Ellipse
    QSizeF size = QSizeF(100.0, 100.0);
    double cornerRadius = 0.0;         // RoundedRect only

    // Polygon
    double radius = 50.0;
    int sides = 6;                     // 3-20

    // Star
    double outerRadius = 50.0;
    double innerRadius = 25.0;
    int points = 5;                    // 3-20

    // Line / Arrow
    QPointF startPoint = QPointF(0.0, 0.0);
    QPointF endPoint = QPointF(100.0, 0.0);
    double headSize = 15.0;            // Arrow only

    // Bezier
    QVector<QPointF> controlPoints;

    QJsonObject toJson() const;
    static ShapeProperties fromJson(const QJsonObject &obj);
};

// --- Shape ---

struct Shape {
    ShapeType type = ShapeType::Rectangle;
    ShapeProperties properties;
    ShapeFill fill;
    ShapeStroke stroke;

    QPointF position = QPointF(0.0, 0.0);
    double rotation = 0.0;     // degrees
    double scale = 1.0;
    QString name;

    QJsonObject toJson() const;
    static Shape fromJson(const QJsonObject &obj);

    static QString typeName(ShapeType t);
    static ShapeType typeFromName(const QString &name);
};

// --- Shape Layer ---

class ShapeLayer
{
public:
    ShapeLayer() = default;

    // --- Shape management ---

    void addShape(const Shape &shape);
    bool removeShape(int index);
    const QVector<Shape> &shapes() const { return m_shapes; }

    // --- Rendering ---

    // Render all shapes onto a transparent QImage
    QImage renderShapes(const QSize &canvasSize) const;

    // Render a single shape using the given painter
    static void renderShape(const Shape &shape, QPainter &painter);

    // Create a gradient brush from fill config
    static QBrush createGradientBrush(const ShapeFill &fill, const QRectF &boundingRect);

    // --- Built-in presets ---

    static Shape createRectangle(const QSizeF &size,
                                 const ShapeFill &fill = ShapeFill(),
                                 const ShapeStroke &stroke = ShapeStroke());

    static Shape createCircle(double radius,
                              const ShapeFill &fill = ShapeFill(),
                              const ShapeStroke &stroke = ShapeStroke());

    static Shape createStar(int points, double outerR, double innerR,
                            const ShapeFill &fill = ShapeFill(),
                            const ShapeStroke &stroke = ShapeStroke());

    static Shape createArrow(const QPointF &start, const QPointF &end,
                             double headSize = 15.0,
                             const ShapeStroke &stroke = ShapeStroke());

    static Shape createCallout(const QString &text,
                               const QColor &bgColor = QColor(40, 40, 40, 220));

    // --- Serialisation ---

    QJsonObject toJson() const;
    static ShapeLayer fromJson(const QJsonObject &obj);

private:
    QVector<Shape> m_shapes;

    // --- Render helpers ---

    static QPainterPath buildShapePath(const Shape &shape);
    static void applyFill(QPainter &painter, const Shape &shape, const QRectF &bounds);
    static void applyStroke(QPainter &painter, const ShapeStroke &stroke);
};
