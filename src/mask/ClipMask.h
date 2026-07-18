#pragma once

#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QVector>

class MaskSystem;

namespace clipmask {

enum class CombineMode {
    Add,
    Subtract,
    Intersect,
    Difference
};

struct BezierPoint {
    QPointF vertex;
    QPointF inTangent;
    QPointF outTangent;

    bool operator==(const BezierPoint &other) const;
    bool operator!=(const BezierPoint &other) const { return !(*this == other); }
};

struct BezierMask {
    QString name;
    QVector<BezierPoint> points;
    CombineMode mode = CombineMode::Add;
    bool closed = true;
    double feather = 0.0;
    double expansion = 0.0;
    bool inverted = false;
    double opacity = 1.0;

    bool isEmpty() const { return points.isEmpty(); }
    bool operator==(const BezierMask &other) const;
    bool operator!=(const BezierMask &other) const { return !(*this == other); }
};

struct ClipMaskStack {
    QVector<BezierMask> masks;

    bool isDefault() const { return masks.isEmpty(); }
    bool operator==(const ClipMaskStack &other) const;
    bool operator!=(const ClipMaskStack &other) const { return !(*this == other); }
};

QString combineModeToken(CombineMode mode);
CombineMode combineModeFromToken(const QString &token,
                                 CombineMode fallback = CombineMode::Add);

QJsonObject pointToJson(const BezierPoint &point);
BezierPoint pointFromJson(const QJsonObject &obj);

QJsonObject maskToJson(const BezierMask &mask);
BezierMask maskFromJson(const QJsonObject &obj);

QJsonObject toJson(const ClipMaskStack &stack);
ClipMaskStack fromJson(const QJsonObject &obj);

ClipMaskStack fromMaskSystem(const MaskSystem &system);
MaskSystem toMaskSystem(const ClipMaskStack &stack);
bool isDefault(const MaskSystem &system);

QJsonObject maskSystemToJson(const MaskSystem &system);
MaskSystem maskSystemFromJson(const QJsonObject &obj);

} // namespace clipmask
