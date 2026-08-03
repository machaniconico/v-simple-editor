#pragma once

#include "Keyframe.h"

#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QSizeF>
#include <QVector>
#include <QVector3D>

// Shared by Camera3D, the compositor, and the light sampler. Keeping the
// value type here avoids a Camera3D/LayerCompositor include cycle while
// preserving Camera3D's JSON keys and rotation convention.
struct Layer3DTransform {
    double positionZ = 0.0;
    double rotationX = 0.0;
    double rotationY = 0.0;
    double rotationZ = 0.0;

    bool isDefault() const {
        return positionZ == 0.0 && rotationX == 0.0
            && rotationY == 0.0 && rotationZ == 0.0;
    }

    void reset() { *this = Layer3DTransform{}; }

    QJsonObject toJson() const;
    static Layer3DTransform fromJson(const QJsonObject &obj);
};

enum class LightType {
    Ambient,
    Parallel,
    Point,
    Spot
};

QString lightTypeName(LightType type);
LightType lightTypeFromName(const QString &name);

struct Light3DState {
    LightType type      = LightType::Point;
    QString   name      = QStringLiteral("Light 1");
    bool      enabled   = true;
    QVector3D position  = QVector3D(0.0f, 0.0f, 500.0f);
    QVector3D target    = QVector3D(0.0f, 0.0f, 0.0f);
    QColor    color     = Qt::white;
    double    intensity = 1.0;
    bool      falloffEnabled  = false;
    double    falloffRadius   = 500.0;
    double    falloffDistance = 500.0;
    double    coneAngle       = 90.0;
    double    coneFeather     = 50.0;

    bool isDefault() const;
    void reset();
    QJsonObject toJson() const;
    static Light3DState fromJson(const QJsonObject &obj);
};

struct LayerMaterial {
    bool   acceptsLights = true;
    double ambientCoeff  = 1.0;
    double diffuseCoeff  = 0.5;
    double specularCoeff = 0.0;
    double shininess     = 20.0;

    bool isDefault() const;
    QJsonObject toJson() const;
    static LayerMaterial fromJson(const QJsonObject &obj);
};

enum class Light3DProperty {
    PositionX,
    PositionY,
    PositionZ,
    TargetX,
    TargetY,
    TargetZ,
    ColorR,
    ColorG,
    ColorB,
    Intensity,
    FalloffRadius,
    FalloffDistance,
    ConeAngle,
    ConeFeather,
    Count
};

class Light3D
{
public:
    Light3D();
    explicit Light3D(const Light3DState &state);

    void setState(const Light3DState &state);
    // Update the non-animated state and, for properties that already have
    // animation tracks, write the edited value at `time`. This is the dialog
    // edit seam: changing a keyed property must not be silently shadowed by
    // its old track value.
    void setStateAt(double time, const Light3DState &state);
    Light3DState state() const { return m_state; }
    Light3DState stateAt(double time) const;

    void setKeyframe(double time, const Light3DState &state,
                     KeyframePoint::Interpolation interp = KeyframePoint::Linear);
    bool hasAnimation() const;
    QVector<double> allKeyframeTimes() const;

    KeyframeTrack *track(Light3DProperty property);
    const KeyframeTrack *track(Light3DProperty property) const;

    QJsonObject toJson() const;
    static Light3D fromJson(const QJsonObject &obj);

    static QString propertyName(Light3DProperty property);
    static Light3DProperty propertyFromName(const QString &name);
    static double propertyDefaultValue(Light3DProperty property);

private:
    Light3DState m_state;
    QVector<KeyframeTrack> m_tracks;

    void ensureTracks();
};

namespace light3d {

struct LightingResult {
    double r = 1.0;
    double g = 1.0;
    double b = 1.0;
};

QVector3D layerNormal(const Layer3DTransform &xform);

// `positionOffset` is the pixel offset from the canvas centre. This is the
// canonical world-space anchor used by every compositor path.
QVector3D layerCenterWorld(const QSizeF &canvasSize,
                           const QPointF &positionOffset = QPointF());

bool hasActiveLights(const QVector<Light3DState> &lights);

QVector<Light3DState> statesAt(const QVector<Light3D> &lights, double time);

LightingResult computeLighting(const QVector<Light3DState> &lights,
                               const QVector3D &worldPos,
                               const QVector3D &normal,
                               const QVector3D &viewPos,
                               const LayerMaterial &material);

QImage applyLighting(const QImage &layerImage,
                     const QVector<Light3DState> &lights,
                     const Layer3DTransform &xform,
                     const QVector3D &layerCenterWorld,
                     const QSizeF &layerWorldSize,
                     const QVector3D &viewPos,
                     const LayerMaterial &material);

} // namespace light3d
