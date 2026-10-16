#include "ClipExpressionBindings.h"

#include <QJsonValue>
#include <cmath>

namespace exprbind {

// ---------------------------------------------------------------------------
// Property-path registry
// ---------------------------------------------------------------------------

QVector<PropertyPathInfo> knownPropertyPathInfos()
{
    QVector<PropertyPathInfo> infos;
    infos.reserve(17);

    infos.push_back({QStringLiteral("transform.position.x"),  QStringLiteral("Position X")});
    infos.push_back({QStringLiteral("transform.position.y"),  QStringLiteral("Position Y")});
    infos.push_back({QStringLiteral("transform.scale"),       QStringLiteral("Scale")});
    infos.push_back({QStringLiteral("transform.rotation"),    QStringLiteral("Rotation")});
    infos.push_back({QStringLiteral("transform.opacity"),     QStringLiteral("Opacity")});

    infos.push_back({QStringLiteral("camera.positionX"),      QStringLiteral("Camera Position X")});
    infos.push_back({QStringLiteral("camera.positionY"),      QStringLiteral("Camera Position Y")});
    infos.push_back({QStringLiteral("camera.positionZ"),      QStringLiteral("Camera Position Z")});
    infos.push_back({QStringLiteral("camera.targetX"),        QStringLiteral("Camera Target X")});
    infos.push_back({QStringLiteral("camera.targetY"),        QStringLiteral("Camera Target Y")});
    infos.push_back({QStringLiteral("camera.targetZ"),        QStringLiteral("Camera Target Z")});
    infos.push_back({QStringLiteral("camera.fov"),            QStringLiteral("Camera FOV")});
    infos.push_back({QStringLiteral("camera.roll"),           QStringLiteral("Camera Roll")});

    infos.push_back({QStringLiteral("text3d.extrudeDepth"),   QStringLiteral("Extrude Depth")});
    infos.push_back({QStringLiteral("text3d.bevelDepth"),     QStringLiteral("Bevel Depth")});
    infos.push_back({QStringLiteral("text3d.yaw"),            QStringLiteral("Yaw")});
    infos.push_back({QStringLiteral("text3d.pitch"),          QStringLiteral("Pitch")});

    return infos;
}

QStringList knownPropertyPaths()
{
    const QVector<PropertyPathInfo> infos = knownPropertyPathInfos();
    QStringList paths;
    paths.reserve(static_cast<int>(infos.size()));
    for (const PropertyPathInfo &info : infos) {
        paths.append(info.path);
    }
    return paths;
}

// ---------------------------------------------------------------------------
// Path validator
// ---------------------------------------------------------------------------

QString validatePath(const QString &propPath)
{
    const QStringList known = knownPropertyPaths();
    if (known.contains(propPath)) {
        return QString();
    }
    return QStringLiteral("Unknown property path: %1").arg(propPath);
}

// ---------------------------------------------------------------------------
// ClipExpressionBindings
// ---------------------------------------------------------------------------

void ClipExpressionBindings::setExpression(const QString &propPath, const QString &code)
{
    const QString trimmed = code.trimmed();
    if (trimmed.isEmpty()) {
        m_bindings.remove(propPath);
    } else {
        m_bindings.insert(propPath, trimmed);
    }
}

void ClipExpressionBindings::clearExpression(const QString &propPath)
{
    m_bindings.remove(propPath);
}

bool ClipExpressionBindings::hasExpression(const QString &propPath) const
{
    return m_bindings.contains(propPath);
}

QString ClipExpressionBindings::expression(const QString &propPath) const
{
    return m_bindings.value(propPath, QString());
}

QStringList ClipExpressionBindings::boundPaths() const
{
    return QStringList(m_bindings.keys());
}

double ClipExpressionBindings::resolve(const QString &propPath,
                                       const ExpressionContext &ctx,
                                       double keyframeValue) const
{
    // No binding -> return keyframe value unchanged.
    auto it = m_bindings.constFind(propPath);
    if (it == m_bindings.constEnd()) {
        return keyframeValue;
    }

    const QString &code = it.value();
    if (code.trimmed().isEmpty()) {
        return keyframeValue;
    }

    // Copy the context by value and inject the upstream keyframe value.
    ExpressionContext ctxCopy;
    ctxCopy.time            = ctx.time;
    ctxCopy.layerIndex      = ctx.layerIndex;
    ctxCopy.fps             = ctx.fps;
    ctxCopy.duration        = ctx.duration;
    ctxCopy.canvasWidth     = ctx.canvasWidth;
    ctxCopy.canvasHeight    = ctx.canvasHeight;
    ctxCopy.value           = keyframeValue;
    ctxCopy.sampleValueAtTime = ctx.sampleValueAtTime;
    ctxCopy.audioLevelAtTime = ctx.audioLevelAtTime ? ctx.audioLevelAtTime : m_audioLevelAtTime;

    const ExpressionResult result = Expression::evaluate(code, ctxCopy);

    if (!result.success) {
        return keyframeValue;
    }

    if (!std::isfinite(result.value)) {
        return keyframeValue;
    }

    return result.value;
}

QJsonObject ClipExpressionBindings::toJson() const
{
    QJsonObject obj;
    for (auto it = m_bindings.constBegin(); it != m_bindings.constEnd(); ++it) {
        obj.insert(it.key(), QJsonValue(it.value()));
    }
    return obj;
}

void ClipExpressionBindings::fromJson(const QJsonObject &obj)
{
    m_bindings.clear();
    m_audioLevelAtTime = {};
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        // Only accept string values (lenient: skip arrays, objects, numbers, etc.)
        if (!it.value().isString()) {
            continue;
        }
        const QString code = it.value().toString().trimmed();
        if (!code.isEmpty()) {
            m_bindings.insert(it.key(), code);
        }
    }
}

bool ClipExpressionBindings::isEmpty() const
{
    return m_bindings.isEmpty();
}

} // namespace exprbind
