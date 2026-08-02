#include "Light3D.h"

#include <QJsonValue>
#include <QQuaternion>

#include <cmath>
#include <set>

namespace {

constexpr double kEpsilon = 1.0e-9;
constexpr double kPi = 3.14159265358979323846;

bool sameDouble(double a, double b)
{
    return std::abs(a - b) <= kEpsilon;
}

QString colorToJson(const QColor &color)
{
    return color.name(QColor::HexArgb);
}

QColor colorFromJson(const QJsonValue &value, const QColor &fallback)
{
    if (!value.isString())
        return fallback;
    const QColor parsed(value.toString());
    return parsed.isValid() ? parsed : fallback;
}

QVector3D normalizedOr(const QVector3D &value, const QVector3D &fallback)
{
    if (value.lengthSquared() <= static_cast<float>(kEpsilon))
        return fallback;
    return value.normalized();
}

double statePropertyValue(const Light3DState &state, Light3DProperty property)
{
    switch (property) {
    case Light3DProperty::PositionX:       return state.position.x();
    case Light3DProperty::PositionY:       return state.position.y();
    case Light3DProperty::PositionZ:       return state.position.z();
    case Light3DProperty::TargetX:         return state.target.x();
    case Light3DProperty::TargetY:         return state.target.y();
    case Light3DProperty::TargetZ:         return state.target.z();
    case Light3DProperty::ColorR:          return state.color.redF();
    case Light3DProperty::ColorG:          return state.color.greenF();
    case Light3DProperty::ColorB:          return state.color.blueF();
    case Light3DProperty::Intensity:       return state.intensity;
    case Light3DProperty::FalloffRadius:   return state.falloffRadius;
    case Light3DProperty::FalloffDistance: return state.falloffDistance;
    case Light3DProperty::ConeAngle:       return state.coneAngle;
    case Light3DProperty::ConeFeather:     return state.coneFeather;
    case Light3DProperty::Count:           break;
    }
    return 0.0;
}

QQuaternion rotationFor(const Layer3DTransform &xform)
{
    return QQuaternion::fromEulerAngles(
        static_cast<float>(xform.rotationX),
        static_cast<float>(xform.rotationY),
        static_cast<float>(xform.rotationZ));
}

double smoothStep(double value)
{
    const double t = qBound(0.0, value, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double falloffFactor(const Light3DState &light, double distance)
{
    if (!light.falloffEnabled)
        return 1.0;
    if (distance <= light.falloffRadius)
        return 1.0;

    const double transition = qMax(0.0, light.falloffDistance);
    if (transition <= kEpsilon || distance >= light.falloffRadius + transition)
        return 0.0;

    const double t = (distance - light.falloffRadius) / transition;
    return 1.0 - smoothStep(t);
}

double spotFactor(const Light3DState &light, const QVector3D &worldPos)
{
    const QVector3D fromLight = worldPos - light.position;
    const QVector3D spotDirection = normalizedOr(
        light.target - light.position, QVector3D(0.0f, 0.0f, -1.0f));
    const QVector3D pointDirection = normalizedOr(fromLight, spotDirection);
    const double cosAngle = qBound(
        -1.0, static_cast<double>(QVector3D::dotProduct(pointDirection, spotDirection)), 1.0);
    const double halfAngle = qBound(0.05, light.coneAngle * 0.5, 179.9);
    const double feather = qBound(0.0, light.coneFeather, 100.0) / 100.0;
    const double outerAngle = halfAngle;
    const double innerAngle = halfAngle * (1.0 - feather);
    const double outerCos = std::cos(outerAngle * kPi / 180.0);
    const double innerCos = std::cos(innerAngle * kPi / 180.0);

    if (cosAngle >= innerCos)
        return 1.0;
    if (cosAngle <= outerCos)
        return 0.0;
    if (sameDouble(innerCos, outerCos))
        return 0.0;

    // cosAngle decreases monotonically as the point moves toward the cone edge.
    return 1.0 - smoothStep((innerCos - cosAngle) / (innerCos - outerCos));
}

void addColoredContribution(double &channel,
                            double colorComponent,
                            double intensity,
                            double diffuse,
                            double specular,
                            const LayerMaterial &material)
{
    channel += colorComponent * intensity
        * (material.diffuseCoeff * diffuse + material.specularCoeff * specular);
}

} // namespace

QString lightTypeName(LightType type)
{
    switch (type) {
    case LightType::Ambient:  return QStringLiteral("Ambient");
    case LightType::Parallel: return QStringLiteral("Parallel");
    case LightType::Point:    return QStringLiteral("Point");
    case LightType::Spot:     return QStringLiteral("Spot");
    }
    return QStringLiteral("Point");
}

LightType lightTypeFromName(const QString &name)
{
    if (name.compare(QStringLiteral("Ambient"), Qt::CaseInsensitive) == 0)
        return LightType::Ambient;
    if (name.compare(QStringLiteral("Parallel"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Directional"), Qt::CaseInsensitive) == 0)
        return LightType::Parallel;
    if (name.compare(QStringLiteral("Spot"), Qt::CaseInsensitive) == 0)
        return LightType::Spot;
    return LightType::Point;
}

bool Light3DState::isDefault() const
{
    const Light3DState d;
    return type == d.type
        && name == d.name
        && enabled == d.enabled
        && position == d.position
        && target == d.target
        && color == d.color
        && sameDouble(intensity, d.intensity)
        && falloffEnabled == d.falloffEnabled
        && sameDouble(falloffRadius, d.falloffRadius)
        && sameDouble(falloffDistance, d.falloffDistance)
        && sameDouble(coneAngle, d.coneAngle)
        && sameDouble(coneFeather, d.coneFeather);
}

void Light3DState::reset()
{
    *this = Light3DState{};
}

QJsonObject Light3DState::toJson() const
{
    const Light3DState d;
    QJsonObject obj;
    if (type != d.type)
        obj[QStringLiteral("type")] = lightTypeName(type);
    if (name != d.name)
        obj[QStringLiteral("name")] = name;
    if (enabled != d.enabled)
        obj[QStringLiteral("enabled")] = enabled;
    if (position != d.position) {
        obj[QStringLiteral("posX")] = static_cast<double>(position.x());
        obj[QStringLiteral("posY")] = static_cast<double>(position.y());
        obj[QStringLiteral("posZ")] = static_cast<double>(position.z());
    }
    if (target != d.target) {
        obj[QStringLiteral("targetX")] = static_cast<double>(target.x());
        obj[QStringLiteral("targetY")] = static_cast<double>(target.y());
        obj[QStringLiteral("targetZ")] = static_cast<double>(target.z());
    }
    if (color != d.color)
        obj[QStringLiteral("color")] = colorToJson(color);
    if (!sameDouble(intensity, d.intensity))
        obj[QStringLiteral("intensity")] = intensity;
    if (falloffEnabled)
        obj[QStringLiteral("falloffEnabled")] = true;
    if (!sameDouble(falloffRadius, d.falloffRadius))
        obj[QStringLiteral("falloffRadius")] = falloffRadius;
    if (!sameDouble(falloffDistance, d.falloffDistance))
        obj[QStringLiteral("falloffDistance")] = falloffDistance;
    if (!sameDouble(coneAngle, d.coneAngle))
        obj[QStringLiteral("coneAngle")] = coneAngle;
    if (!sameDouble(coneFeather, d.coneFeather))
        obj[QStringLiteral("coneFeather")] = coneFeather;
    return obj;
}

Light3DState Light3DState::fromJson(const QJsonObject &obj)
{
    Light3DState s;
    s.type = lightTypeFromName(obj.value(QStringLiteral("type")).toString(
        lightTypeName(s.type)));
    s.name = obj.value(QStringLiteral("name")).toString(s.name);
    s.enabled = obj.value(QStringLiteral("enabled")).toBool(s.enabled);
    s.position = QVector3D(
        static_cast<float>(obj.value(QStringLiteral("posX")).toDouble(s.position.x())),
        static_cast<float>(obj.value(QStringLiteral("posY")).toDouble(s.position.y())),
        static_cast<float>(obj.value(QStringLiteral("posZ")).toDouble(s.position.z())));
    s.target = QVector3D(
        static_cast<float>(obj.value(QStringLiteral("targetX")).toDouble(s.target.x())),
        static_cast<float>(obj.value(QStringLiteral("targetY")).toDouble(s.target.y())),
        static_cast<float>(obj.value(QStringLiteral("targetZ")).toDouble(s.target.z())));
    s.color = colorFromJson(obj.value(QStringLiteral("color")), s.color);
    s.intensity = obj.value(QStringLiteral("intensity")).toDouble(s.intensity);
    s.falloffEnabled = obj.value(QStringLiteral("falloffEnabled")).toBool(s.falloffEnabled);
    s.falloffRadius = obj.value(QStringLiteral("falloffRadius")).toDouble(s.falloffRadius);
    s.falloffDistance = obj.value(QStringLiteral("falloffDistance")).toDouble(s.falloffDistance);
    s.coneAngle = obj.value(QStringLiteral("coneAngle")).toDouble(s.coneAngle);
    s.coneFeather = obj.value(QStringLiteral("coneFeather")).toDouble(s.coneFeather);
    return s;
}

bool LayerMaterial::isDefault() const
{
    const LayerMaterial d;
    return acceptsLights == d.acceptsLights
        && sameDouble(ambientCoeff, d.ambientCoeff)
        && sameDouble(diffuseCoeff, d.diffuseCoeff)
        && sameDouble(specularCoeff, d.specularCoeff)
        && sameDouble(shininess, d.shininess);
}

QJsonObject LayerMaterial::toJson() const
{
    const LayerMaterial d;
    QJsonObject obj;
    if (acceptsLights != d.acceptsLights)
        obj[QStringLiteral("acceptsLights")] = acceptsLights;
    if (!sameDouble(ambientCoeff, d.ambientCoeff))
        obj[QStringLiteral("ambientCoeff")] = ambientCoeff;
    if (!sameDouble(diffuseCoeff, d.diffuseCoeff))
        obj[QStringLiteral("diffuseCoeff")] = diffuseCoeff;
    if (!sameDouble(specularCoeff, d.specularCoeff))
        obj[QStringLiteral("specularCoeff")] = specularCoeff;
    if (!sameDouble(shininess, d.shininess))
        obj[QStringLiteral("shininess")] = shininess;
    return obj;
}

LayerMaterial LayerMaterial::fromJson(const QJsonObject &obj)
{
    LayerMaterial m;
    m.acceptsLights = obj.value(QStringLiteral("acceptsLights")).toBool(m.acceptsLights);
    m.ambientCoeff = obj.value(QStringLiteral("ambientCoeff")).toDouble(m.ambientCoeff);
    m.diffuseCoeff = obj.value(QStringLiteral("diffuseCoeff")).toDouble(m.diffuseCoeff);
    m.specularCoeff = obj.value(QStringLiteral("specularCoeff")).toDouble(m.specularCoeff);
    m.shininess = obj.value(QStringLiteral("shininess")).toDouble(m.shininess);
    return m;
}

Light3D::Light3D()
{
    ensureTracks();
}

Light3D::Light3D(const Light3DState &state)
    : m_state(state)
{
    ensureTracks();
}

QString Light3D::propertyName(Light3DProperty property)
{
    switch (property) {
    case Light3DProperty::PositionX:       return QStringLiteral("light.positionX");
    case Light3DProperty::PositionY:       return QStringLiteral("light.positionY");
    case Light3DProperty::PositionZ:       return QStringLiteral("light.positionZ");
    case Light3DProperty::TargetX:         return QStringLiteral("light.targetX");
    case Light3DProperty::TargetY:         return QStringLiteral("light.targetY");
    case Light3DProperty::TargetZ:         return QStringLiteral("light.targetZ");
    case Light3DProperty::ColorR:          return QStringLiteral("light.colorR");
    case Light3DProperty::ColorG:          return QStringLiteral("light.colorG");
    case Light3DProperty::ColorB:          return QStringLiteral("light.colorB");
    case Light3DProperty::Intensity:       return QStringLiteral("light.intensity");
    case Light3DProperty::FalloffRadius:   return QStringLiteral("light.falloffRadius");
    case Light3DProperty::FalloffDistance: return QStringLiteral("light.falloffDistance");
    case Light3DProperty::ConeAngle:       return QStringLiteral("light.coneAngle");
    case Light3DProperty::ConeFeather:     return QStringLiteral("light.coneFeather");
    case Light3DProperty::Count:           break;
    }
    return QStringLiteral("unknown");
}

Light3DProperty Light3D::propertyFromName(const QString &name)
{
    for (int i = 0; i < static_cast<int>(Light3DProperty::Count); ++i) {
        const auto property = static_cast<Light3DProperty>(i);
        if (propertyName(property) == name)
            return property;
    }
    return Light3DProperty::Count;
}

double Light3D::propertyDefaultValue(Light3DProperty property)
{
    const Light3DState d;
    switch (property) {
    case Light3DProperty::PositionX:       return d.position.x();
    case Light3DProperty::PositionY:       return d.position.y();
    case Light3DProperty::PositionZ:       return d.position.z();
    case Light3DProperty::TargetX:         return d.target.x();
    case Light3DProperty::TargetY:         return d.target.y();
    case Light3DProperty::TargetZ:         return d.target.z();
    case Light3DProperty::ColorR:          return d.color.redF();
    case Light3DProperty::ColorG:          return d.color.greenF();
    case Light3DProperty::ColorB:          return d.color.blueF();
    case Light3DProperty::Intensity:       return d.intensity;
    case Light3DProperty::FalloffRadius:   return d.falloffRadius;
    case Light3DProperty::FalloffDistance: return d.falloffDistance;
    case Light3DProperty::ConeAngle:       return d.coneAngle;
    case Light3DProperty::ConeFeather:     return d.coneFeather;
    case Light3DProperty::Count:           break;
    }
    return 0.0;
}

void Light3D::ensureTracks()
{
    const int count = static_cast<int>(Light3DProperty::Count);
    if (m_tracks.size() == count)
        return;
    m_tracks.clear();
    m_tracks.reserve(count);
    for (int i = 0; i < count; ++i) {
        const auto property = static_cast<Light3DProperty>(i);
        m_tracks.append(KeyframeTrack(propertyName(property),
                                      propertyDefaultValue(property)));
    }
}

void Light3D::setState(const Light3DState &state)
{
    m_state = state;
    ensureTracks();
}

void Light3D::setStateAt(double time, const Light3DState &state)
{
    setState(state);

    const int count = static_cast<int>(Light3DProperty::Count);
    for (int i = 0; i < count; ++i) {
        KeyframeTrack &propertyTrack = m_tracks[i];
        if (propertyTrack.hasKeyframes())
            propertyTrack.addKeyframe(
                time,
                statePropertyValue(state, static_cast<Light3DProperty>(i)));
    }
}

KeyframeTrack *Light3D::track(Light3DProperty property)
{
    ensureTracks();
    const int index = static_cast<int>(property);
    if (index < 0 || index >= m_tracks.size())
        return nullptr;
    return &m_tracks[index];
}

const KeyframeTrack *Light3D::track(Light3DProperty property) const
{
    const int index = static_cast<int>(property);
    if (index < 0 || index >= m_tracks.size())
        return nullptr;
    return &m_tracks[index];
}

void Light3D::setKeyframe(double time, const Light3DState &state,
                          KeyframePoint::Interpolation interp)
{
    setState(state);
    const double values[] = {
        state.position.x(), state.position.y(), state.position.z(),
        state.target.x(), state.target.y(), state.target.z(),
        state.color.redF(), state.color.greenF(), state.color.blueF(),
        state.intensity, state.falloffRadius, state.falloffDistance,
        state.coneAngle, state.coneFeather
    };
    for (int i = 0; i < static_cast<int>(Light3DProperty::Count); ++i) {
        m_tracks[i].addKeyframe(time, values[i], interp);
    }
}

Light3DState Light3D::stateAt(double time) const
{
    Light3DState result = m_state;
    const auto value = [this, time](Light3DProperty property, double fallback) {
        const KeyframeTrack *tr = track(property);
        return tr && tr->hasKeyframes() ? tr->valueAt(time) : fallback;
    };
    result.position.setX(static_cast<float>(value(Light3DProperty::PositionX,
                                                   result.position.x())));
    result.position.setY(static_cast<float>(value(Light3DProperty::PositionY,
                                                   result.position.y())));
    result.position.setZ(static_cast<float>(value(Light3DProperty::PositionZ,
                                                   result.position.z())));
    result.target.setX(static_cast<float>(value(Light3DProperty::TargetX,
                                                 result.target.x())));
    result.target.setY(static_cast<float>(value(Light3DProperty::TargetY,
                                                 result.target.y())));
    result.target.setZ(static_cast<float>(value(Light3DProperty::TargetZ,
                                                 result.target.z())));
    const double r = qBound(0.0, value(Light3DProperty::ColorR, result.color.redF()), 1.0);
    const double g = qBound(0.0, value(Light3DProperty::ColorG, result.color.greenF()), 1.0);
    const double b = qBound(0.0, value(Light3DProperty::ColorB, result.color.blueF()), 1.0);
    result.color = QColor::fromRgbF(r, g, b, result.color.alphaF());
    result.intensity = value(Light3DProperty::Intensity, result.intensity);
    result.falloffRadius = value(Light3DProperty::FalloffRadius, result.falloffRadius);
    result.falloffDistance = value(Light3DProperty::FalloffDistance, result.falloffDistance);
    result.coneAngle = value(Light3DProperty::ConeAngle, result.coneAngle);
    result.coneFeather = value(Light3DProperty::ConeFeather, result.coneFeather);
    return result;
}

bool Light3D::hasAnimation() const
{
    for (const KeyframeTrack &tr : m_tracks) {
        if (tr.hasKeyframes())
            return true;
    }
    return false;
}

QVector<double> Light3D::allKeyframeTimes() const
{
    std::set<double> times;
    for (const KeyframeTrack &tr : m_tracks) {
        for (const KeyframePoint &kf : tr.keyframes())
            times.insert(std::round(kf.time * 1000.0) / 1000.0);
    }
    QVector<double> result;
    result.reserve(static_cast<int>(times.size()));
    for (double time : times)
        result.append(time);
    return result;
}

QJsonObject Light3D::toJson() const
{
    QJsonObject obj = m_state.toJson();
    QJsonArray tracks;
    for (const KeyframeTrack &tr : m_tracks) {
        if (!tr.hasKeyframes())
            continue;
        QJsonObject trackObj;
        trackObj[QStringLiteral("property")] = tr.propertyName();
        QJsonArray keyframes;
        for (const KeyframePoint &kf : tr.keyframes())
            keyframes.append(keyframePointToJson(kf));
        trackObj[QStringLiteral("keyframes")] = keyframes;
        tracks.append(trackObj);
    }
    if (!tracks.isEmpty())
        obj[QStringLiteral("tracks")] = tracks;
    return obj;
}

Light3D Light3D::fromJson(const QJsonObject &obj)
{
    Light3D result(Light3DState::fromJson(obj));
    const QJsonArray tracks = obj.value(QStringLiteral("tracks")).toArray();
    for (const QJsonValue &value : tracks) {
        const QJsonObject trackObj = value.toObject();
        const Light3DProperty property = propertyFromName(
            trackObj.value(QStringLiteral("property")).toString());
        if (property == Light3DProperty::Count)
            continue;
        KeyframeTrack *tr = result.track(property);
        if (!tr)
            continue;
        for (const QJsonValue &kfValue : trackObj.value(QStringLiteral("keyframes")).toArray()) {
            const KeyframePoint kf = keyframePointFromJson(kfValue.toObject());
            tr->addKeyframe(kf.time, kf.value, kf.interpolation,
                            kf.bezX1, kf.bezY1, kf.bezX2, kf.bezY2,
                            kf.hasSpatialTangent, kf.spatialOutX, kf.spatialOutY,
                            kf.spatialInX, kf.spatialInY);
        }
    }
    return result;
}

namespace light3d {

QVector3D layerCenterWorld(const QSizeF &canvasSize,
                           const QPointF &positionOffset)
{
    return QVector3D(
        static_cast<float>(canvasSize.width() * 0.5 + positionOffset.x()),
        static_cast<float>(canvasSize.height() * 0.5 + positionOffset.y()),
        0.0f);
}

bool hasActiveLights(const QVector<Light3DState> &lights)
{
    for (const Light3DState &light : lights) {
        if (light.enabled && light.intensity > 0.0)
            return true;
    }
    return false;
}

QVector<Light3DState> statesAt(const QVector<Light3D> &lights, double time)
{
    QVector<Light3DState> result;
    result.reserve(lights.size());
    for (const Light3D &light : lights)
        result.append(light.stateAt(time));
    return result;
}

QVector3D layerNormal(const Layer3DTransform &xform)
{
    return normalizedOr(rotationFor(xform).rotatedVector(QVector3D(0.0f, 0.0f, 1.0f)),
                        QVector3D(0.0f, 0.0f, 1.0f));
}

LightingResult computeLighting(const QVector<Light3DState> &lights,
                               const QVector3D &worldPos,
                               const QVector3D &normal,
                               const QVector3D &viewPos,
                               const LayerMaterial &material)
{
    LightingResult result;
    if (!material.acceptsLights)
        return result;

    const QVector3D n = normalizedOr(normal, QVector3D(0.0f, 0.0f, 1.0f));
    const QVector3D viewDirection = normalizedOr(
        viewPos - worldPos, QVector3D(0.0f, 0.0f, 1.0f));

    for (const Light3DState &light : lights) {
        if (!light.enabled || light.intensity <= 0.0)
            continue;

        const double intensity = qMax(0.0, light.intensity);
        const double red = light.color.redF();
        const double green = light.color.greenF();
        const double blue = light.color.blueF();

        if (light.type == LightType::Ambient) {
            const double contribution = intensity * material.ambientCoeff;
            result.r += red * contribution;
            result.g += green * contribution;
            result.b += blue * contribution;
            continue;
        }

        QVector3D direction;
        double attenuation = 1.0;
        double cone = 1.0;
        if (light.type == LightType::Parallel) {
            direction = normalizedOr(light.position - light.target,
                                     QVector3D(0.0f, 0.0f, 1.0f));
        } else {
            const QVector3D toLight = light.position - worldPos;
            const double distance = std::sqrt(
                static_cast<double>(toLight.lengthSquared()));
            direction = normalizedOr(toLight, n);
            attenuation = falloffFactor(light, distance);
            if (light.type == LightType::Spot)
                cone = spotFactor(light, worldPos);
        }

        if (attenuation <= 0.0 || cone <= 0.0)
            continue;

        const double diffuse = qMax(0.0, static_cast<double>(
            QVector3D::dotProduct(n, direction))) * attenuation * cone;
        double specular = 0.0;
        if (material.specularCoeff > 0.0) {
            const QVector3D halfway = normalizedOr(direction + viewDirection, n);
            const double shininess = qMax(1.0, material.shininess);
            specular = std::pow(qMax(0.0, static_cast<double>(
                QVector3D::dotProduct(n, halfway))), shininess)
                * attenuation * cone;
        }

        addColoredContribution(result.r, red, intensity, diffuse, specular, material);
        addColoredContribution(result.g, green, intensity, diffuse, specular, material);
        addColoredContribution(result.b, blue, intensity, diffuse, specular, material);
    }

    result.r = qMax(0.0, result.r);
    result.g = qMax(0.0, result.g);
    result.b = qMax(0.0, result.b);
    return result;
}

QImage applyLighting(const QImage &layerImage,
                     const QVector<Light3DState> &lights,
                     const Layer3DTransform &xform,
                     const QVector3D &layerCenterWorld,
                     const QSizeF &layerWorldSize,
                     const QVector3D &viewPos,
                     const LayerMaterial &material)
{
    if (layerImage.isNull() || lights.isEmpty() || !material.acceptsLights)
        return layerImage;

    if (!hasActiveLights(lights))
        return layerImage;

    const QImage source = layerImage.convertToFormat(QImage::Format_ARGB32);
    QImage result = source;
    const double width = layerWorldSize.width() > 0.0
        ? layerWorldSize.width() : source.width();
    const double height = layerWorldSize.height() > 0.0
        ? layerWorldSize.height() : source.height();
    const QQuaternion rotation = rotationFor(xform);
    const QVector3D center = layerCenterWorld + QVector3D(
        0.0f, 0.0f, static_cast<float>(xform.positionZ));
    const QVector3D halfWidth = rotation.rotatedVector(
        QVector3D(static_cast<float>(width * 0.5), 0.0f, 0.0f));
    const QVector3D halfHeight = rotation.rotatedVector(
        QVector3D(0.0f, static_cast<float>(height * 0.5), 0.0f));
    const QVector3D topLeft = center - halfWidth - halfHeight;
    const QVector3D topRight = center + halfWidth - halfHeight;
    const QVector3D bottomLeft = center - halfWidth + halfHeight;
    const QVector3D bottomRight = center + halfWidth + halfHeight;
    const QVector3D normal = layerNormal(xform);

    for (int y = 0; y < source.height(); ++y) {
        const double v = (y + 0.5) / qMax(1, source.height());
        QRgb *dst = reinterpret_cast<QRgb *>(result.scanLine(y));
        const QRgb *src = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            if (qAlpha(src[x]) == 0)
                continue;
            const double u = (x + 0.5) / qMax(1, source.width());
            const QVector3D worldPos = topLeft * ((1.0 - u) * (1.0 - v))
                + topRight * (u * (1.0 - v))
                + bottomLeft * ((1.0 - u) * v)
                + bottomRight * (u * v);
            const LightingResult gain = computeLighting(
                lights, worldPos, normal, viewPos, material);
            dst[x] = qRgba(
                qBound(0, static_cast<int>(qRed(src[x]) * gain.r + 0.5), 255),
                qBound(0, static_cast<int>(qGreen(src[x]) * gain.g + 0.5), 255),
                qBound(0, static_cast<int>(qBlue(src[x]) * gain.b + 0.5), 255),
                qAlpha(src[x]));
        }
    }
    return result;
}

} // namespace light3d
