#include "VfxGenerators.h"

#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRadialGradient>
#include <QtMath>

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTwoPi = 2.0 * kPi;

class DeterministicRng
{
public:
    explicit DeterministicRng(unsigned int seed)
        : m_state(seed == 0u ? 0x6D2B79F5u : static_cast<std::uint32_t>(seed))
    {
    }

    std::uint32_t next()
    {
        // xorshift32: compact, deterministic, and independent of Qt/global
        // process state. The generators only use this local value stream.
        std::uint32_t x = m_state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        m_state = x;
        return x;
    }

    double unit()
    {
        return static_cast<double>(next()) / 4294967295.0;
    }

    double signedUnit()
    {
        return unit() * 2.0 - 1.0;
    }

private:
    std::uint32_t m_state;
};

int safeWidth(const QSize &size)
{
    return qMax(1, size.width());
}

int safeHeight(const QSize &size)
{
    return qMax(1, size.height());
}

double shortEdge(const QSize &size)
{
    return static_cast<double>(qMax(1, qMin(safeWidth(size), safeHeight(size))));
}

QSize safeSize(const QSize &size)
{
    return QSize(safeWidth(size), safeHeight(size));
}

double clamp01(double value)
{
    if (!std::isfinite(value))
        return 0.0;
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

double clampDouble(double value, double minValue, double maxValue)
{
    if (!std::isfinite(value))
        return minValue;
    return value < minValue ? minValue : (value > maxValue ? maxValue : value);
}

int clampInt(int value, int minValue, int maxValue)
{
    return value < minValue ? minValue : (value > maxValue ? maxValue : value);
}

QPointF normalizedToPixel(const QPointF &point, const QSize &size)
{
    return QPointF(point.x() * static_cast<double>(safeWidth(size)),
                   point.y() * static_cast<double>(safeHeight(size)));
}

QColor withOpacity(QColor color, double opacity)
{
    const double alpha = clamp01(opacity);
    color.setAlpha(qBound(0, qRound(static_cast<double>(color.alpha()) * alpha), 255));
    return color;
}

QColor lerpColor(const QColor &a, const QColor &b, double t)
{
    const double amount = clamp01(t);
    return QColor(
        qBound(0, qRound(a.red() + (b.red() - a.red()) * amount), 255),
        qBound(0, qRound(a.green() + (b.green() - a.green()) * amount), 255),
        qBound(0, qRound(a.blue() + (b.blue() - a.blue()) * amount), 255),
        qBound(0, qRound(a.alpha() + (b.alpha() - a.alpha()) * amount), 255));
}

double keyframeValue(const KeyframeManager &manager, const QString &name,
                     double timeSeconds, double fallback)
{
    const double value = manager.valueAt(name, timeSeconds, fallback);
    return std::isfinite(value) ? value : fallback;
}

double keyframeValue(const KeyframeManager &manager, const char *name,
                     double timeSeconds, double fallback)
{
    return keyframeValue(manager, QString::fromLatin1(name), timeSeconds, fallback);
}

QPainterPath pathFromPoints(const QVector<QPointF> &points)
{
    QPainterPath path;
    if (points.isEmpty())
        return path;
    path.moveTo(points.first());
    for (int i = 1; i < static_cast<int>(points.size()); ++i)
        path.lineTo(points[i]);
    return path;
}

QPointF perpendicularUnit(const QPointF &vector)
{
    const double length = std::sqrt(vector.x() * vector.x()
                                     + vector.y() * vector.y());
    if (length <= 1e-9)
        return QPointF(0.0, 1.0);
    return QPointF(-vector.y() / length, vector.x() / length);
}

QPointF projectTilt(const QPointF &local, const QPointF &center,
                    const Layer3DTransform &tilt, double edge)
{
    const double rx = tilt.rotationX * kPi / 180.0;
    const double ry = tilt.rotationY * kPi / 180.0;
    const double rz = tilt.rotationZ * kPi / 180.0;

    double x = local.x();
    double y = local.y();
    double z = 0.0;

    const double cosX = std::cos(rx);
    const double sinX = std::sin(rx);
    const double yX = y * cosX - z * sinX;
    const double zX = y * sinX + z * cosX;
    y = yX;
    z = zX;

    const double cosY = std::cos(ry);
    const double sinY = std::sin(ry);
    const double xY = x * cosY + z * sinY;
    const double zY = -x * sinY + z * cosY;
    x = xY;
    z = zY;

    const double cosZ = std::cos(rz);
    const double sinZ = std::sin(rz);
    const double xZ = x * cosZ - y * sinZ;
    const double yZ = x * sinZ + y * cosZ;
    x = xZ;
    y = yZ;

    const double perspective = clampDouble(
        1.0 / (1.0 + z / qMax(1.0, edge * 4.0)), 0.5, 2.0);
    return center + QPointF(x * perspective, y * perspective);
}

double staticNoise(unsigned int seed, int index)
{
    const unsigned int mixed = seed ^ (static_cast<unsigned int>(index) * 0x9E3779B9u)
        ^ 0xA511E9B3u;
    DeterministicRng rng(mixed);
    return rng.signedUnit();
}

void drawGlowingPath(QPainter &painter, const QPainterPath &path,
                     const QColor &color, double coreWidth, double opacity,
                     double haloMultiplier = 5.0)
{
    if (path.isEmpty())
        return;

    const double width = qMax(0.5, coreWidth);
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(withOpacity(color, opacity * 0.13), width * haloMultiplier));
    painter.drawPath(path);
    painter.setPen(QPen(withOpacity(color, opacity * 0.32), width * 2.4));
    painter.drawPath(path);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setPen(QPen(withOpacity(color, opacity), width));
    painter.drawPath(path);
    painter.restore();
}

QPainterPath circlePath(const QSize &size, const QPointF &center,
                        double radius, double angle,
                        const Layer3DTransform &tilt)
{
    QPainterPath path;
    const double edge = shortEdge(size);
    constexpr int kSamples = 96;
    for (int i = 0; i <= kSamples; ++i) {
        const double a = angle + kTwoPi * static_cast<double>(i)
            / static_cast<double>(kSamples);
        const QPointF local(radius * std::cos(a), radius * std::sin(a));
        const QPointF projected = projectTilt(local, center, tilt, edge);
        if (i == 0)
            path.moveTo(projected);
        else
            path.lineTo(projected);
    }
    return path;
}

void drawImpact(QPainter &painter, const QPointF &center, double radius,
                const QColor &color, double opacity, int rayCount,
                unsigned int seed)
{
    const double safeRadius = qMax(1.0, radius);
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setPen(QPen(withOpacity(color, opacity * 0.65), qMax(1.0, safeRadius * 0.08)));
    painter.setBrush(withOpacity(color, opacity * 0.28));
    painter.drawEllipse(center, safeRadius * 0.34, safeRadius * 0.34);

    DeterministicRng rng(seed);
    const int safeRayCount = clampInt(rayCount, 4, 48);
    for (int i = 0; i < safeRayCount; ++i) {
        const double angle = kTwoPi * static_cast<double>(i)
            / static_cast<double>(safeRayCount) + rng.signedUnit() * 0.12;
        const double inner = safeRadius * (0.45 + rng.unit() * 0.12);
        const double outer = safeRadius * (0.85 + rng.unit() * 0.45);
        const QPointF a(center.x() + std::cos(angle) * inner,
                        center.y() + std::sin(angle) * inner);
        const QPointF b(center.x() + std::cos(angle) * outer,
                        center.y() + std::sin(angle) * outer);
        painter.drawLine(a, b);
    }
    painter.restore();
}

bool setDoubleValue(double &target, const QString &name, const QString &wanted,
                    const QVariant &value)
{
    if (name != wanted)
        return false;
    target = value.toDouble();
    return std::isfinite(target);
}

bool setIntValue(int &target, const QString &name, const QString &wanted,
                const QVariant &value)
{
    if (name != wanted)
        return false;
    target = value.toInt();
    return true;
}

} // namespace

QVector<VfxGeneratorType> VfxGenerators::allTypes()
{
    QVector<VfxGeneratorType> types;
    const int count = static_cast<int>(VfxGeneratorType::Count);
    types.reserve(count);
    for (int i = 0; i < count; ++i)
        types.append(static_cast<VfxGeneratorType>(i));
    return types;
}

QString VfxGenerators::typeName(VfxGeneratorType type)
{
    switch (type) {
    case VfxGeneratorType::Explosion:   return QStringLiteral("Explosion");
    case VfxGeneratorType::Lightning:   return QStringLiteral("Lightning");
    case VfxGeneratorType::ShockWave:   return QStringLiteral("ShockWave");
    case VfxGeneratorType::EnergyBeam:  return QStringLiteral("EnergyBeam");
    case VfxGeneratorType::MagicCircle: return QStringLiteral("MagicCircle");
    case VfxGeneratorType::MuzzleFlash: return QStringLiteral("MuzzleFlash");
    case VfxGeneratorType::EnergyShield:return QStringLiteral("EnergyShield");
    case VfxGeneratorType::Count:       break;
    }
    return QStringLiteral("Unknown");
}

QString VfxGenerators::displayName(VfxGeneratorType type)
{
    switch (type) {
    case VfxGeneratorType::Explosion:   return QStringLiteral("爆発");
    case VfxGeneratorType::Lightning:   return QStringLiteral("稲妻");
    case VfxGeneratorType::ShockWave:   return QStringLiteral("衝撃波");
    case VfxGeneratorType::EnergyBeam:  return QStringLiteral("エネルギービーム");
    case VfxGeneratorType::MagicCircle: return QStringLiteral("魔法陣");
    case VfxGeneratorType::MuzzleFlash: return QStringLiteral("マズルフラッシュ");
    case VfxGeneratorType::EnergyShield:return QStringLiteral("エネルギーシールド");
    case VfxGeneratorType::Count:       break;
    }
    return QStringLiteral("VFX ジェネレータ");
}

VfxGeneratorParameters VfxGenerators::defaultParameters(VfxGeneratorType type)
{
    switch (type) {
    case VfxGeneratorType::Explosion:   return ExplosionParameters{};
    case VfxGeneratorType::Lightning:   return LightningParameters{};
    case VfxGeneratorType::ShockWave:   return ShockWaveParameters{};
    case VfxGeneratorType::EnergyBeam:  return EnergyBeamParameters{};
    case VfxGeneratorType::MagicCircle: return MagicCircleParameters{};
    case VfxGeneratorType::MuzzleFlash: return MuzzleFlashParameters{};
    case VfxGeneratorType::EnergyShield:return EnergyShieldParameters{};
    case VfxGeneratorType::Count:       break;
    }
    return ExplosionParameters{};
}

QVector<VfxParameterSpec> VfxGenerators::parameterSpecs(VfxGeneratorType type)
{
    QVector<VfxParameterSpec> specs;
    auto add = [&specs](const QString &name, const QString &label,
                        double minValue, double maxValue, double defaultValue,
                        bool integer = false) {
        VfxParameterSpec spec;
        spec.name = name;
        spec.displayName = label;
        spec.minValue = minValue;
        spec.maxValue = maxValue;
        spec.defaultValue = defaultValue;
        spec.integer = integer;
        specs.append(spec);
    };

    switch (type) {
    case VfxGeneratorType::Explosion:
        add(QStringLiteral("scale"), QStringLiteral("Scale"), 0.01, 1.0, 0.32);
        add(QStringLiteral("fragmentCount"), QStringLiteral("Fragments"), 0.0, 256.0, 28.0, true);
        add(QStringLiteral("gravity"), QStringLiteral("Gravity"), 0.0, 1000.0, 140.0);
        add(QStringLiteral("duration"), QStringLiteral("Duration (s)"), 0.05, 10.0, 1.2);
        break;
    case VfxGeneratorType::Lightning:
        add(QStringLiteral("branchProbability"), QStringLiteral("Branch probability"), 0.0, 1.0, 0.34);
        add(QStringLiteral("recursionDepth"), QStringLiteral("Recursion depth"), 0.0, 8.0, 5.0, true);
        add(QStringLiteral("jitterWidth"), QStringLiteral("Jitter"), 0.0, 0.2, 0.045);
        add(QStringLiteral("coreWidth"), QStringLiteral("Core width"), 0.5, 20.0, 2.5);
        break;
    case VfxGeneratorType::ShockWave:
        add(QStringLiteral("speed"), QStringLiteral("Speed (px/s)"), 0.0, 2000.0, 300.0);
        add(QStringLiteral("ringWidth"), QStringLiteral("Ring width"), 1.0, 100.0, 14.0);
        add(QStringLiteral("distortionStrength"), QStringLiteral("Distortion"), 0.0, 100.0, 24.0);
        add(QStringLiteral("decay"), QStringLiteral("Decay"), 0.0, 10.0, 1.8);
        break;
    case VfxGeneratorType::EnergyBeam:
        add(QStringLiteral("coreWidth"), QStringLiteral("Core width"), 0.5, 50.0, 5.0);
        add(QStringLiteral("haloWidth"), QStringLiteral("Halo width"), 1.0, 200.0, 34.0);
        add(QStringLiteral("noiseIntensity"), QStringLiteral("Noise"), 0.0, 1.0, 0.18);
        add(QStringLiteral("flowSpeed"), QStringLiteral("Flow speed"), 0.0, 20.0, 3.0);
        break;
    case VfxGeneratorType::MagicCircle:
        add(QStringLiteral("radius"), QStringLiteral("Radius"), 0.05, 1.0, 0.28);
        add(QStringLiteral("ringCount"), QStringLiteral("Rings"), 1.0, 8.0, 3.0, true);
        add(QStringLiteral("segmentCount"), QStringLiteral("Segments"), 3.0, 64.0, 12.0, true);
        add(QStringLiteral("lineWidth"), QStringLiteral("Line width"), 0.5, 20.0, 2.0);
        break;
    case VfxGeneratorType::MuzzleFlash:
        add(QStringLiteral("scale"), QStringLiteral("Scale"), 0.05, 1.0, 0.22);
        add(QStringLiteral("directionDegrees"), QStringLiteral("Direction"), -360.0, 360.0, 0.0);
        add(QStringLiteral("spikeCount"), QStringLiteral("Spikes"), 1.0, 64.0, 10.0, true);
        add(QStringLiteral("durationFrames"), QStringLiteral("Duration (frames)"), 1.0, 10.0, 2.0, true);
        break;
    case VfxGeneratorType::EnergyShield:
        add(QStringLiteral("radius"), QStringLiteral("Radius"), 0.05, 1.0, 0.34);
        add(QStringLiteral("edgeSharpness"), QStringLiteral("Edge sharpness"), 0.5, 12.0, 4.0);
        add(QStringLiteral("cellSize"), QStringLiteral("Cell size"), 6.0, 120.0, 34.0);
        add(QStringLiteral("rippleStrength"), QStringLiteral("Ripple strength"), 0.0, 2.0, 1.0);
        break;
    case VfxGeneratorType::Count:
        break;
    }
    return specs;
}

double VfxGenerators::durationSeconds(VfxGeneratorType type,
                                      const VfxGeneratorParameters &parameters)
{
    return std::visit([type](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        Q_UNUSED(type);
        if constexpr (std::is_same_v<T, MuzzleFlashParameters>)
            return value.lifetimeSeconds();
        else if constexpr (std::is_same_v<T, ExplosionParameters>)
            return qMax(0.05, value.duration);
        else if constexpr (std::is_same_v<T, ShockWaveParameters>)
            return qMax(0.05, value.duration);
        else if constexpr (std::is_same_v<T, EnergyBeamParameters>)
            return qMax(0.05, value.duration);
        else if constexpr (std::is_same_v<T, MagicCircleParameters>)
            return qMax(0.05, value.duration);
        else if constexpr (std::is_same_v<T, EnergyShieldParameters>)
            return qMax(0.05, value.duration);
        else
            return 1.0;
    }, parameters);
}

QImage VfxGenerators::render(VfxGeneratorType type, const QSize &canvasSize,
                             const VfxGeneratorParameters &parameters,
                             double timeSeconds)
{
    switch (type) {
    case VfxGeneratorType::Explosion:
        if (const auto *p = std::get_if<ExplosionParameters>(&parameters))
            return renderExplosion(canvasSize, timeSeconds, *p);
        break;
    case VfxGeneratorType::Lightning:
        if (const auto *p = std::get_if<LightningParameters>(&parameters))
            return renderLightning(canvasSize, timeSeconds, *p);
        break;
    case VfxGeneratorType::ShockWave:
        if (const auto *p = std::get_if<ShockWaveParameters>(&parameters))
            return renderShockWave(canvasSize, timeSeconds, *p);
        break;
    case VfxGeneratorType::EnergyBeam:
        if (const auto *p = std::get_if<EnergyBeamParameters>(&parameters))
            return renderEnergyBeam(canvasSize, timeSeconds, *p);
        break;
    case VfxGeneratorType::MagicCircle:
        if (const auto *p = std::get_if<MagicCircleParameters>(&parameters))
            return renderMagicCircle(canvasSize, timeSeconds, *p);
        break;
    case VfxGeneratorType::MuzzleFlash:
        if (const auto *p = std::get_if<MuzzleFlashParameters>(&parameters))
            return renderMuzzleFlash(canvasSize, timeSeconds, *p);
        break;
    case VfxGeneratorType::EnergyShield:
        if (const auto *p = std::get_if<EnergyShieldParameters>(&parameters))
            return renderEnergyShield(canvasSize, timeSeconds, *p);
        break;
    case VfxGeneratorType::Count:
        break;
    }
    const int typeIndex = static_cast<int>(type);
    const int typeCount = static_cast<int>(VfxGeneratorType::Count);
    if (typeIndex < 0 || typeIndex >= typeCount) {
        QImage empty(safeSize(canvasSize), QImage::Format_ARGB32_Premultiplied);
        empty.fill(Qt::transparent);
        return empty;
    }
    return render(type, canvasSize, defaultParameters(type), timeSeconds);
}

QImage VfxGenerators::renderDefault(VfxGeneratorType type, const QSize &canvasSize,
                                    double timeSeconds)
{
    return render(type, canvasSize, defaultParameters(type), timeSeconds);
}

QImage VfxGenerators::renderExplosion(const QSize &canvasSize, double timeSeconds,
                                      const ExplosionParameters &parameters)
{
    const QSize size = safeSize(canvasSize);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const double duration = qMax(0.05, keyframeValue(
        parameters.keyframes, "duration", timeSeconds, parameters.duration));
    if (timeSeconds < 0.0 || timeSeconds >= duration)
        return image;

    const double phase = clamp01(timeSeconds / duration);
    const double edge = shortEdge(size);
    const QPointF center = normalizedToPixel(
        QPointF(keyframeValue(parameters.keyframes, "centerX", timeSeconds, parameters.center.x()),
                keyframeValue(parameters.keyframes, "centerY", timeSeconds, parameters.center.y())),
        size);
    const double scale = qMax(0.01, keyframeValue(
        parameters.keyframes, "scale", timeSeconds, parameters.scale));
    const double radius = edge * scale * (0.16 + 0.54 * std::pow(phase, 0.62));
    const double opacity = clamp01(keyframeValue(
        parameters.keyframes, "opacity", timeSeconds, parameters.opacity));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor hot = lerpColor(QColor(255, 255, 245, 255),
                           QColor(255, 178, 45, 255), phase * 1.4);
    hot = lerpColor(hot, QColor(118, 18, 12, 255),
                    clamp01(phase * 0.8 + parameters.colorTemperature * 0.45));

    QRadialGradient fireGradient(center, qMax(1.0, radius * 1.35));
    fireGradient.setColorAt(0.0, withOpacity(QColor(255, 255, 255, 255), opacity));
    fireGradient.setColorAt(0.25, withOpacity(hot, opacity * (1.0 - phase * 0.35)));
    fireGradient.setColorAt(0.7, withOpacity(QColor(255, 72, 14, 255), opacity * (1.0 - phase * 0.65)));
    fireGradient.setColorAt(1.0, withOpacity(QColor(120, 16, 10, 0), opacity));
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fireGradient);
    painter.drawEllipse(center, radius * 1.35, radius * 1.35);

    const int fragmentCount = clampInt(qRound(keyframeValue(
        parameters.keyframes, "fragmentCount", timeSeconds,
        static_cast<double>(parameters.fragmentCount))), 0, 512);
    DeterministicRng fragmentRng(parameters.seed);
    const double fragmentTime = qMax(0.0, timeSeconds - duration * 0.08);
    for (int i = 0; i < fragmentCount; ++i) {
        const double angle = fragmentRng.unit() * kTwoPi;
        const double speed = edge * scale * (0.35 + fragmentRng.unit() * 1.35);
        const double gravity = keyframeValue(
            parameters.keyframes, "gravity", timeSeconds, parameters.gravity);
        const QPointF velocity(std::cos(angle) * speed,
                               std::sin(angle) * speed);
        const QPointF position = center + QPointF(
            velocity.x() * fragmentTime,
            velocity.y() * fragmentTime
                + 0.5 * gravity * scale * fragmentTime * fragmentTime);
        const QPointF previous = center + QPointF(
            velocity.x() * qMax(0.0, fragmentTime - 0.035),
            velocity.y() * qMax(0.0, fragmentTime - 0.035)
                + 0.5 * gravity * scale * qMax(0.0, fragmentTime - 0.035)
                    * qMax(0.0, fragmentTime - 0.035));
        const double fragmentAlpha = opacity * (1.0 - phase) * (0.35 + 0.65 * fragmentRng.unit());
        painter.setPen(QPen(withOpacity(lerpColor(hot, QColor(75, 12, 10, 255), phase), fragmentAlpha),
                            qMax(1.0, edge * scale * (0.006 + 0.008 * fragmentRng.unit()))));
        painter.drawLine(previous, position);
    }

    if (phase > 0.12) {
        DeterministicRng smokeRng(parameters.seed ^ 0xD1B54A32u);
        const double smokePhase = clamp01((phase - 0.12) / 0.88);
        for (int i = 0; i < 10; ++i) {
            const double angle = smokeRng.unit() * kTwoPi;
            const double offset = radius * (0.35 + smokeRng.unit() * 0.8);
            const double rise = edge * scale * smokePhase * (0.12 + smokeRng.unit() * 0.24);
            const QPointF smokeCenter = center + QPointF(
                std::cos(angle) * offset,
                std::sin(angle) * offset - rise);
            const double smokeRadius = edge * scale
                * (0.04 + smokeRng.unit() * 0.10 + smokePhase * 0.14);
            const double smokeAlpha = opacity * (1.0 - smokePhase) * 0.2;
            painter.setBrush(withOpacity(QColor(60, 52, 58, 210), smokeAlpha));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(smokeCenter, smokeRadius, smokeRadius * 0.82);
        }
    }

    painter.end();
    return image;
}

LightningGeometry VfxGenerators::lightningGeometry(const QSize &canvasSize,
                                                   const LightningParameters &parameters,
                                                   double timeSeconds)
{
    const QSize size = safeSize(canvasSize);
    const double edge = shortEdge(size);
    const double time = qMax(0.0, timeSeconds);
    const QPointF start = normalizedToPixel(
        QPointF(keyframeValue(parameters.keyframes, "startX", time, parameters.start.x()),
                keyframeValue(parameters.keyframes, "startY", time, parameters.start.y())),
        size);
    const QPointF end = normalizedToPixel(
        QPointF(keyframeValue(parameters.keyframes, "endX", time, parameters.end.x()),
                keyframeValue(parameters.keyframes, "endY", time, parameters.end.y())),
        size);
    const int depth = clampInt(qRound(keyframeValue(
        parameters.keyframes, "recursionDepth", time,
        static_cast<double>(parameters.recursionDepth))), 0, 8);
    const double jitter = qMax(0.0, keyframeValue(
        parameters.keyframes, "jitterWidth", time, parameters.jitterWidth)) * edge;

    QVector<QPointF> path;
    path.append(start);
    path.append(end);
    DeterministicRng mainRng(parameters.seed ^ 0x51ED270Bu);
    for (int level = 0; level < depth; ++level) {
        QVector<QPointF> next;
        next.reserve(static_cast<int>(path.size()) * 2);
        next.append(path.first());
        const double displacementScale = 1.0
            / std::pow(1.7, static_cast<double>(level));
        for (int i = 0; i + 1 < static_cast<int>(path.size()); ++i) {
            const QPointF a = path[i];
            const QPointF b = path[i + 1];
            const QPointF midpoint = (a + b) * 0.5;
            const QPointF normal = perpendicularUnit(b - a);
            const double displacement = mainRng.signedUnit()
                * jitter * displacementScale;
            next.append(midpoint + normal * displacement);
            next.append(b);
        }
        path = next;
    }

    LightningGeometry geometry;
    geometry.mainPath = path;

    const double branchProbability = clamp01(keyframeValue(
        parameters.keyframes, "branchProbability", time,
        parameters.branchProbability));
    if (branchProbability <= 0.0 || path.size() < 3)
        return geometry;

    DeterministicRng branchRng(parameters.seed ^ 0xB5297A4Du);
    const int stride = qMax(1, static_cast<int>(path.size()) / 16);
    for (int i = 1; i + 1 < static_cast<int>(path.size()); i += stride) {
        if (branchRng.unit() > branchProbability)
            continue;

        const QPointF tangent = path[i + 1] - path[i - 1];
        const QPointF normal = perpendicularUnit(tangent);
        const double sign = branchRng.unit() < 0.5 ? -1.0 : 1.0;
        const double length = edge * (0.025 + branchRng.unit() * 0.13);
        const QPointF branchEnd = path[i] + normal * (sign * length);
        const QPointF branchMid = path[i]
            + (branchEnd - path[i]) * (0.55 + branchRng.unit() * 0.22)
            + perpendicularUnit(branchEnd - path[i]) * branchRng.signedUnit()
                * length * 0.18;
        geometry.branches.append(QVector<QPointF>{path[i], branchMid, branchEnd});
    }
    return geometry;
}

QImage VfxGenerators::renderLightning(const QSize &canvasSize, double timeSeconds,
                                      const LightningParameters &parameters)
{
    const QSize size = safeSize(canvasSize);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const LightningGeometry geometry = lightningGeometry(size, parameters, timeSeconds);
    if (geometry.mainPath.isEmpty())
        return image;

    const double flickerRate = keyframeValue(
        parameters.keyframes, "flickerRate", timeSeconds, parameters.flickerRate);
    const double flickerDepth = clamp01(keyframeValue(
        parameters.keyframes, "flickerDepth", timeSeconds, parameters.flickerDepth));
    const double flicker = 0.5 + 0.5 * std::sin(
        kTwoPi * flickerRate * qMax(0.0, timeSeconds)
        + static_cast<double>(parameters.seed % 360u) * kPi / 180.0);
    const double pulse = clamp01(1.0 - flickerDepth * (1.0 - flicker));
    const double opacity = clamp01(keyframeValue(
        parameters.keyframes, "opacity", timeSeconds, parameters.opacity)) * pulse;
    const double width = qMax(0.5, keyframeValue(
        parameters.keyframes, "coreWidth", timeSeconds, parameters.coreWidth));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawGlowingPath(painter, pathFromPoints(geometry.mainPath), parameters.color,
                    width, opacity, 6.0);

    for (int i = 0; i < static_cast<int>(geometry.branches.size()); ++i) {
        const double branchOpacity = opacity * (0.35 + 0.3
            * staticNoise(parameters.seed, i));
        drawGlowingPath(painter, pathFromPoints(geometry.branches[i]), parameters.color,
                        width * 0.55, qMax(0.1, branchOpacity), 3.2);
    }

    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setPen(Qt::NoPen);
    painter.setBrush(withOpacity(QColor(255, 255, 255, 255), opacity * 0.9));
    painter.drawEllipse(geometry.mainPath.first(), width * 1.1, width * 1.1);
    painter.drawEllipse(geometry.mainPath.last(), width * 1.1, width * 1.1);
    painter.end();
    return image;
}

double VfxGenerators::shockWaveRadius(const ShockWaveParameters &parameters,
                                      double timeSeconds)
{
    const double time = qMax(0.0, timeSeconds);
    const double initialRadius = qMax(0.0, keyframeValue(
        parameters.keyframes, "initialRadius", time, parameters.initialRadius));
    const double speed = qMax(0.0, keyframeValue(
        parameters.keyframes, "speed", time, parameters.speed));
    return initialRadius + speed * time;
}

QImage VfxGenerators::renderShockWave(const QSize &canvasSize, double timeSeconds,
                                      const ShockWaveParameters &parameters)
{
    const QSize size = safeSize(canvasSize);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const double duration = qMax(0.05, keyframeValue(
        parameters.keyframes, "duration", timeSeconds, parameters.duration));
    if (timeSeconds < 0.0 || timeSeconds >= duration)
        return image;

    const double time = qMax(0.0, timeSeconds);
    const QPointF center = normalizedToPixel(
        QPointF(keyframeValue(parameters.keyframes, "centerX", time, parameters.center.x()),
                keyframeValue(parameters.keyframes, "centerY", time, parameters.center.y())),
        size);
    const double radius = shockWaveRadius(parameters, time);
    const double width = qMax(1.0, keyframeValue(
        parameters.keyframes, "ringWidth", time, parameters.ringWidth));
    const double decay = qMax(0.0, keyframeValue(
        parameters.keyframes, "decay", time, parameters.decay));
    const double alpha = clamp01(std::exp(-decay * time)
                                  * keyframeValue(parameters.keyframes, "opacity", time,
                                                  parameters.opacity));
    const double distortion = keyframeValue(parameters.keyframes,
                                            "distortionStrength", time,
                                            parameters.distortionStrength);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(withOpacity(parameters.color, alpha), width));
    painter.drawEllipse(center, radius, radius);

    const double secondaryRadius = radius + distortion * 0.16
        * std::sin(time * 9.0);
    painter.setPen(QPen(withOpacity(parameters.color, alpha * 0.35), qMax(1.0, width * 0.45)));
    painter.drawArc(QRectF(center.x() - secondaryRadius, center.y() - secondaryRadius,
                           secondaryRadius * 2.0, secondaryRadius * 2.0),
                    15 * 16, 210 * 16);
    painter.drawArc(QRectF(center.x() - secondaryRadius, center.y() - secondaryRadius,
                           secondaryRadius * 2.0, secondaryRadius * 2.0),
                    240 * 16, 80 * 16);

    const int spokeCount = 12;
    painter.setPen(QPen(withOpacity(parameters.color, alpha * 0.25), qMax(1.0, width * 0.25)));
    for (int i = 0; i < spokeCount; ++i) {
        const double angle = kTwoPi * static_cast<double>(i)
            / static_cast<double>(spokeCount) + time * 1.7;
        const double length = distortion * (0.25 + 0.75
            * (0.5 + 0.5 * std::sin(time * 7.0 + i)));
        const QPointF a(center.x() + std::cos(angle) * qMax(0.0, radius - length),
                        center.y() + std::sin(angle) * qMax(0.0, radius - length));
        const QPointF b(center.x() + std::cos(angle) * (radius + length),
                        center.y() + std::sin(angle) * (radius + length));
        painter.drawLine(a, b);
    }
    painter.end();
    return image;
}

QImage VfxGenerators::applyShockWave(const QImage &source, double timeSeconds,
                                     const ShockWaveParameters &parameters)
{
    if (source.isNull())
        return {};

    const QImage input = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage output = input;
    const double duration = qMax(0.05, keyframeValue(
        parameters.keyframes, "duration", timeSeconds, parameters.duration));
    if (timeSeconds < 0.0 || timeSeconds >= duration)
        return output;

    const double time = qMax(0.0, timeSeconds);
    const QPointF center = normalizedToPixel(
        QPointF(keyframeValue(parameters.keyframes, "centerX", time, parameters.center.x()),
                keyframeValue(parameters.keyframes, "centerY", time, parameters.center.y())),
        input.size());
    const double radius = shockWaveRadius(parameters, time);
    const double width = qMax(1.0, keyframeValue(
        parameters.keyframes, "ringWidth", time, parameters.ringWidth));
    const double distortion = keyframeValue(
        parameters.keyframes, "distortionStrength", time,
        parameters.distortionStrength);
    const double decay = qMax(0.0, keyframeValue(
        parameters.keyframes, "decay", time, parameters.decay));
    const double opacity = clamp01(keyframeValue(
        parameters.keyframes, "opacity", time, parameters.opacity));
    const double decayFactor = std::exp(-decay * time) * opacity;
    const double influenceLimit = width * 2.5;

    // The ring is a localized radial UV displacement. Pixels outside the ring
    // keep their original premultiplied bytes, which makes the effect safe to
    // place over arbitrary footage without introducing a colored border.
    for (int y = 0; y < input.height(); ++y) {
        for (int x = 0; x < input.width(); ++x) {
            const double dx = static_cast<double>(x) - center.x();
            const double dy = static_cast<double>(y) - center.y();
            const double distance = std::sqrt(dx * dx + dy * dy);
            const double offsetFromRing = std::abs(distance - radius);
            if (offsetFromRing > influenceLimit || distance <= 1e-9)
                continue;

            const double normalizedOffset = offsetFromRing / width;
            const double ringWeight = std::exp(-0.5 * normalizedOffset
                                               * normalizedOffset);
            const double displacement = distortion * decayFactor * ringWeight;
            if (!std::isfinite(displacement) || std::abs(displacement) < 0.01)
                continue;

            const double sourceX = static_cast<double>(x)
                - dx / distance * displacement;
            const double sourceY = static_cast<double>(y)
                - dy / distance * displacement;
            const int sampleX = qBound(0, qRound(sourceX), input.width() - 1);
            const int sampleY = qBound(0, qRound(sourceY), input.height() - 1);
            output.setPixel(x, y, input.pixel(sampleX, sampleY));
        }
    }
    return output;
}

QImage VfxGenerators::renderEnergyBeam(const QSize &canvasSize, double timeSeconds,
                                       const EnergyBeamParameters &parameters)
{
    const QSize size = safeSize(canvasSize);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const double duration = qMax(0.05, keyframeValue(
        parameters.keyframes, "duration", timeSeconds, parameters.duration));
    if (timeSeconds < 0.0 || timeSeconds >= duration)
        return image;

    const QPointF start = normalizedToPixel(
        QPointF(keyframeValue(parameters.keyframes, "startX", timeSeconds, parameters.start.x()),
                keyframeValue(parameters.keyframes, "startY", timeSeconds, parameters.start.y())),
        size);
    const QPointF end = normalizedToPixel(
        QPointF(keyframeValue(parameters.keyframes, "endX", timeSeconds, parameters.end.x()),
                keyframeValue(parameters.keyframes, "endY", timeSeconds, parameters.end.y())),
        size);
    const QPointF direction = end - start;
    const QPointF normal = perpendicularUnit(direction);
    const int segments = 48;
    const double noiseIntensity = clamp01(keyframeValue(
        parameters.keyframes, "noiseIntensity", timeSeconds, parameters.noiseIntensity));
    const double flowSpeed = keyframeValue(
        parameters.keyframes, "flowSpeed", timeSeconds, parameters.flowSpeed);
    const double coreWidth = qMax(0.5, keyframeValue(
        parameters.keyframes, "coreWidth", timeSeconds, parameters.coreWidth));
    const double haloWidth = qMax(coreWidth, keyframeValue(
        parameters.keyframes, "haloWidth", timeSeconds, parameters.haloWidth));
    const double opacity = clamp01(keyframeValue(
        parameters.keyframes, "opacity", timeSeconds, parameters.opacity));
    const double edge = shortEdge(size);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int i = 0; i < segments; ++i) {
        const double u0 = static_cast<double>(i) / static_cast<double>(segments);
        const double u1 = static_cast<double>(i + 1) / static_cast<double>(segments);
        const double n0 = staticNoise(parameters.seed, i)
            * noiseIntensity * edge * 0.018
            * std::sin(kTwoPi * (flowSpeed * timeSeconds + u0 * 2.3));
        const double n1 = staticNoise(parameters.seed, i + 1)
            * noiseIntensity * edge * 0.018
            * std::sin(kTwoPi * (flowSpeed * timeSeconds + u1 * 2.3));
        const QPointF p0 = start + direction * u0 + normal * (i == 0 || i == segments - 1 ? 0.0 : n0);
        const QPointF p1 = start + direction * u1 + normal * (i + 1 == segments ? 0.0 : n1);
        const double flow = 0.65 + 0.35 * std::sin(
            kTwoPi * (flowSpeed * timeSeconds + u0 * 3.0));
        painter.setPen(QPen(withOpacity(parameters.color, opacity * 0.10 * flow), haloWidth));
        painter.drawLine(p0, p1);
        painter.setPen(QPen(withOpacity(parameters.color, opacity * (0.78 + 0.22 * flow)), coreWidth));
        painter.drawLine(p0, p1);
    }
    painter.end();

    QPainter impactPainter(&image);
    impactPainter.setRenderHint(QPainter::Antialiasing, true);
    const double impactRadius = qMax(coreWidth * 3.0, haloWidth * 0.48);
    drawImpact(impactPainter, start, impactRadius * 0.7, parameters.color,
               opacity * 0.8, 12, parameters.seed ^ 0x12121212u);
    drawImpact(impactPainter, end, impactRadius, parameters.color,
               opacity, 16, parameters.seed ^ 0x34343434u);
    impactPainter.end();
    return image;
}

MagicCircleGeometry VfxGenerators::magicCircleGeometry(
    const QSize &canvasSize, const MagicCircleParameters &parameters,
    double timeSeconds)
{
    const double edge = shortEdge(canvasSize);
    const int ringCount = clampInt(qRound(keyframeValue(
        parameters.keyframes, "ringCount", timeSeconds,
        static_cast<double>(parameters.ringCount))), 1, 12);
    const double radius = qMax(1.0, keyframeValue(
        parameters.keyframes, "radius", timeSeconds, parameters.radius) * edge);
    MagicCircleGeometry geometry;
    geometry.rings.reserve(ringCount);
    for (int i = 0; i < ringCount; ++i) {
        double speed = 0.0;
        if (i < static_cast<int>(parameters.rotationSpeeds.size()))
            speed = parameters.rotationSpeeds[i];
        else
            speed = (i % 2 == 0 ? 0.5 : -0.35) / (1.0 + i * 0.2);
        speed = keyframeValue(parameters.keyframes,
                              QStringLiteral("ringSpeed.%1").arg(i),
                              timeSeconds, speed);
        MagicCircleRingGeometry ring;
        ring.radius = radius * (0.52 + 0.48
            * static_cast<double>(i + 1) / static_cast<double>(ringCount));
        ring.angleRadians = speed * timeSeconds;
        geometry.rings.append(ring);
    }
    return geometry;
}

QImage VfxGenerators::renderMagicCircle(const QSize &canvasSize, double timeSeconds,
                                        const MagicCircleParameters &parameters)
{
    const QSize size = safeSize(canvasSize);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const QPointF center = normalizedToPixel(
        QPointF(keyframeValue(parameters.keyframes, "centerX", timeSeconds, parameters.center.x()),
                keyframeValue(parameters.keyframes, "centerY", timeSeconds, parameters.center.y())),
        size);
    const MagicCircleGeometry geometry = magicCircleGeometry(size, parameters, timeSeconds);
    const int segmentCount = clampInt(qRound(keyframeValue(
        parameters.keyframes, "segmentCount", timeSeconds,
        static_cast<double>(parameters.segmentCount))), 3, 96);
    const double lineWidth = qMax(0.5, keyframeValue(
        parameters.keyframes, "lineWidth", timeSeconds, parameters.lineWidth));
    const double opacity = clamp01(keyframeValue(
        parameters.keyframes, "opacity", timeSeconds, parameters.opacity));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int ringIndex = 0; ringIndex < static_cast<int>(geometry.rings.size()); ++ringIndex) {
        const MagicCircleRingGeometry &ring = geometry.rings[ringIndex];
        const QColor ringColor = withOpacity(parameters.color,
                                             opacity * (0.48 + 0.12 * (ringIndex % 3)));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ringColor, lineWidth));
        painter.drawPath(circlePath(size, center, ring.radius, ring.angleRadians,
                                    parameters.tilt));

        const int polygonSides = qMax(3, segmentCount / 2 + (ringIndex % 3));
        QPolygonF polygon;
        for (int i = 0; i < polygonSides; ++i) {
            const double angle = ring.angleRadians + kTwoPi * static_cast<double>(i)
                / static_cast<double>(polygonSides);
            const QPointF local(ring.radius * 0.72 * std::cos(angle),
                                ring.radius * 0.72 * std::sin(angle));
            polygon.append(projectTilt(local, center, parameters.tilt, shortEdge(size)));
        }
        painter.setPen(QPen(withOpacity(parameters.color, opacity * 0.34),
                            qMax(0.5, lineWidth * 0.65)));
        painter.drawPolygon(polygon);

        painter.setPen(QPen(withOpacity(parameters.color, opacity * 0.74),
                            qMax(0.5, lineWidth * 0.55)));
        for (int i = 0; i < segmentCount; ++i) {
            const double angle = ring.angleRadians + kTwoPi * static_cast<double>(i)
                / static_cast<double>(segmentCount);
            const QPointF innerLocal(ring.radius * 0.88 * std::cos(angle),
                                     ring.radius * 0.88 * std::sin(angle));
            const QPointF outerLocal(ring.radius * (i % 3 == 0 ? 1.08 : 1.01)
                                         * std::cos(angle),
                                     ring.radius * (i % 3 == 0 ? 1.08 : 1.01)
                                         * std::sin(angle));
            painter.drawLine(projectTilt(innerLocal, center, parameters.tilt, shortEdge(size)),
                             projectTilt(outerLocal, center, parameters.tilt, shortEdge(size)));
        }
    }

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setPen(QPen(withOpacity(parameters.color, opacity * 0.8), lineWidth * 1.15));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, qMax(1.0, geometry.rings.first().radius * 0.12),
                        qMax(1.0, geometry.rings.first().radius * 0.12));
    painter.end();
    return image;
}

QImage VfxGenerators::renderMuzzleFlash(const QSize &canvasSize, double timeSeconds,
                                        const MuzzleFlashParameters &parameters)
{
    const QSize size = safeSize(canvasSize);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const double keyframedFrames = qMax(0.0, keyframeValue(
        parameters.keyframes, "durationFrames", timeSeconds,
        static_cast<double>(parameters.durationFrames)));
    const double keyframedFrameRate = qMax(0.001, keyframeValue(
        parameters.keyframes, "frameRate", timeSeconds, parameters.frameRate));
    const double lifetime = keyframedFrames / keyframedFrameRate;
    if (lifetime <= 0.0 || timeSeconds < 0.0 || timeSeconds >= lifetime)
        return image;

    const double phase = clamp01(timeSeconds / lifetime);
    const double edge = shortEdge(size);
    const QPointF center = normalizedToPixel(parameters.center, size);
    const double scale = qMax(0.02, keyframeValue(
        parameters.keyframes, "scale", timeSeconds, parameters.scale));
    const double direction = keyframeValue(
        parameters.keyframes, "directionDegrees", timeSeconds,
        parameters.directionDegrees) * kPi / 180.0;
    const double opacity = clamp01(keyframeValue(
        parameters.keyframes, "opacity", timeSeconds, parameters.opacity))
        * (1.0 - phase);
    const double radius = edge * scale * (0.55 + 0.4 * (1.0 - phase));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    QRadialGradient flashGradient(center, qMax(1.0, radius * 0.7));
    flashGradient.setColorAt(0.0, withOpacity(QColor(255, 255, 255, 255), opacity));
    flashGradient.setColorAt(0.32, withOpacity(QColor(255, 245, 180, 255), opacity * 0.9));
    flashGradient.setColorAt(1.0, withOpacity(QColor(255, 95, 12, 0), opacity));
    painter.setPen(Qt::NoPen);
    painter.setBrush(flashGradient);
    painter.drawEllipse(center, radius * 0.72, radius * 0.72);

    DeterministicRng rng(parameters.seed);
    const int spikeCount = clampInt(qRound(keyframeValue(
        parameters.keyframes, "spikeCount", timeSeconds,
        static_cast<double>(parameters.spikeCount))), 1, 64);
    painter.setPen(QPen(withOpacity(QColor(255, 205, 80, 255), opacity * 0.85),
                        qMax(1.0, edge * scale * 0.018)));
    for (int i = 0; i < spikeCount; ++i) {
        const double angle = direction
            + (static_cast<double>(i) / static_cast<double>(spikeCount) - 0.5) * kPi * 0.85
            + rng.signedUnit() * 0.12;
        const double length = radius * (0.72 + rng.unit() * 0.9);
        const QPointF start = center + QPointF(std::cos(angle), std::sin(angle)) * radius * 0.12;
        const QPointF end = center + QPointF(std::cos(angle), std::sin(angle)) * length;
        painter.drawLine(start, end);
    }

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (int i = 0; i < 4; ++i) {
        const double puff = radius * (0.12 + i * 0.05);
        const QPointF puffCenter = center + QPointF(
            std::cos(direction + kPi + rng.signedUnit() * 0.25) * radius * (0.15 + i * 0.09),
            std::sin(direction + kPi + rng.signedUnit() * 0.25) * radius * (0.15 + i * 0.09));
        painter.setBrush(withOpacity(QColor(74, 68, 68, 170), opacity * 0.22));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(puffCenter, puff, puff * 0.65);
    }
    painter.end();
    return image;
}

QImage VfxGenerators::renderEnergyShield(const QSize &canvasSize, double timeSeconds,
                                         const EnergyShieldParameters &parameters)
{
    const QSize size = safeSize(canvasSize);
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const double duration = qMax(0.05, keyframeValue(
        parameters.keyframes, "duration", timeSeconds, parameters.duration));
    if (timeSeconds < 0.0 || timeSeconds >= duration)
        return image;

    const double edge = shortEdge(size);
    const QPointF center = normalizedToPixel(
        QPointF(keyframeValue(parameters.keyframes, "centerX", timeSeconds, parameters.center.x()),
                keyframeValue(parameters.keyframes, "centerY", timeSeconds, parameters.center.y())),
        size);
    const double radius = qMax(1.0, keyframeValue(
        parameters.keyframes, "radius", timeSeconds, parameters.radius) * edge);
    const double sharpness = qMax(0.5, keyframeValue(
        parameters.keyframes, "edgeSharpness", timeSeconds, parameters.edgeSharpness));
    const double opacity = clamp01(keyframeValue(
        parameters.keyframes, "opacity", timeSeconds, parameters.opacity));
    const double cellSize = qMax(6.0, keyframeValue(
        parameters.keyframes, "cellSize", timeSeconds, parameters.cellSize));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.save();
    painter.translate(center);
    painter.scale(1.0, 0.72);
    QRadialGradient fresnel(QPointF(0.0, 0.0), radius);
    fresnel.setColorAt(0.0, withOpacity(parameters.color, opacity * 0.01));
    fresnel.setColorAt(0.55, withOpacity(parameters.color, opacity * 0.03));
    fresnel.setColorAt(0.82, withOpacity(parameters.color, opacity * 0.12));
    fresnel.setColorAt(0.96, withOpacity(parameters.color, opacity * 0.42));
    fresnel.setColorAt(1.0, withOpacity(parameters.color, opacity * 0.82));
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fresnel);
    painter.drawEllipse(QPointF(0.0, 0.0), radius, radius);
    painter.restore();

    const double halfHeight = radius * 0.72;
    const double horizontalStep = cellSize * 1.5;
    const double verticalStep = cellSize * std::sqrt(3.0);
    const int minColumn = static_cast<int>(std::floor((center.x() - radius) / horizontalStep)) - 1;
    const int maxColumn = static_cast<int>(std::ceil((center.x() + radius) / horizontalStep)) + 1;
    const int minRow = static_cast<int>(std::floor((center.y() - halfHeight) / verticalStep)) - 1;
    const int maxRow = static_cast<int>(std::ceil((center.y() + halfHeight) / verticalStep)) + 1;
    DeterministicRng cellRng(parameters.seed);
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int column = minColumn; column <= maxColumn; ++column) {
        for (int row = minRow; row <= maxRow; ++row) {
            const double x = static_cast<double>(column) * horizontalStep;
            const double y = static_cast<double>(row) * verticalStep
                + (column % 2 == 0 ? 0.0 : verticalStep * 0.5);
            const double nx = (x - center.x()) / radius;
            const double ny = (y - center.y()) / halfHeight;
            if (nx * nx + ny * ny > 0.94)
                continue;

            const double phase = 0.5 + 0.5 * std::sin(
                timeSeconds * 2.5 + static_cast<double>(column * 17 + row * 31)
                    + cellRng.signedUnit());
            const double cellOpacity = opacity * (0.08 + 0.16 * phase);
            QPolygonF hex;
            for (int side = 0; side < 6; ++side) {
                const double angle = kPi / 6.0 + kTwoPi * static_cast<double>(side) / 6.0;
                hex.append(QPointF(x + std::cos(angle) * cellSize * 0.48,
                                   y + std::sin(angle) * cellSize * 0.48));
            }
            painter.setPen(QPen(withOpacity(parameters.color, cellOpacity),
                                qMax(0.6, cellSize * 0.035)));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolygon(hex);
        }
    }
    painter.restore();

    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int i = 0; i < static_cast<int>(parameters.impactPoints.size()); ++i) {
        const QPointF impact = normalizedToPixel(parameters.impactPoints[i], size);
        const double rippleSpeed = qMax(1.0, keyframeValue(
            parameters.keyframes, "rippleSpeed", timeSeconds, parameters.rippleSpeed));
        const double rippleRadius = std::fmod(
            qMax(0.0, timeSeconds) * rippleSpeed + static_cast<double>(i) * radius * 0.37,
            qMax(1.0, radius * 1.6));
        const double rippleAlpha = clamp01(keyframeValue(
            parameters.keyframes, "rippleStrength", timeSeconds,
            parameters.rippleStrength))
            * (1.0 - clamp01(rippleRadius / qMax(1.0, radius * 1.6))) * opacity;
        painter.setPen(QPen(withOpacity(parameters.color, rippleAlpha * 0.9),
                            qMax(1.0, cellSize * 0.07)));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(impact, qMax(1.0, rippleRadius),
                            qMax(1.0, rippleRadius * 0.72));
        painter.setPen(QPen(withOpacity(parameters.color, opacity * 0.8),
                            qMax(1.0, cellSize * 0.09)));
        painter.drawEllipse(impact, qMax(2.0, cellSize * 0.12),
                            qMax(2.0, cellSize * 0.09));
    }
    painter.restore();

    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(withOpacity(parameters.color, opacity * clamp01(sharpness / 5.0)),
                        qMax(1.0, sharpness * 0.8)));
    painter.drawEllipse(center, radius, radius * 0.72);
    painter.end();
    return image;
}

bool VfxGenerators::setParameter(VfxGeneratorParameters &parameters,
                                 const QString &name, const QVariant &value)
{
    return std::visit([&](auto &p) {
        using T = std::decay_t<decltype(p)>;
        bool changed = false;
        if constexpr (std::is_same_v<T, ExplosionParameters>) {
            changed = setDoubleValue(p.scale, name, QStringLiteral("scale"), value)
                || setIntValue(p.fragmentCount, name, QStringLiteral("fragmentCount"), value)
                || setDoubleValue(p.gravity, name, QStringLiteral("gravity"), value)
                || setDoubleValue(p.duration, name, QStringLiteral("duration"), value);
        } else if constexpr (std::is_same_v<T, LightningParameters>) {
            changed = setDoubleValue(p.branchProbability, name, QStringLiteral("branchProbability"), value)
                || setIntValue(p.recursionDepth, name, QStringLiteral("recursionDepth"), value)
                || setDoubleValue(p.jitterWidth, name, QStringLiteral("jitterWidth"), value)
                || setDoubleValue(p.coreWidth, name, QStringLiteral("coreWidth"), value);
        } else if constexpr (std::is_same_v<T, ShockWaveParameters>) {
            changed = setDoubleValue(p.speed, name, QStringLiteral("speed"), value)
                || setDoubleValue(p.ringWidth, name, QStringLiteral("ringWidth"), value)
                || setDoubleValue(p.distortionStrength, name, QStringLiteral("distortionStrength"), value)
                || setDoubleValue(p.decay, name, QStringLiteral("decay"), value);
        } else if constexpr (std::is_same_v<T, EnergyBeamParameters>) {
            changed = setDoubleValue(p.coreWidth, name, QStringLiteral("coreWidth"), value)
                || setDoubleValue(p.haloWidth, name, QStringLiteral("haloWidth"), value)
                || setDoubleValue(p.noiseIntensity, name, QStringLiteral("noiseIntensity"), value)
                || setDoubleValue(p.flowSpeed, name, QStringLiteral("flowSpeed"), value);
        } else if constexpr (std::is_same_v<T, MagicCircleParameters>) {
            changed = setDoubleValue(p.radius, name, QStringLiteral("radius"), value)
                || setIntValue(p.ringCount, name, QStringLiteral("ringCount"), value)
                || setIntValue(p.segmentCount, name, QStringLiteral("segmentCount"), value)
                || setDoubleValue(p.lineWidth, name, QStringLiteral("lineWidth"), value);
        } else if constexpr (std::is_same_v<T, MuzzleFlashParameters>) {
            changed = setDoubleValue(p.scale, name, QStringLiteral("scale"), value)
                || setDoubleValue(p.directionDegrees, name, QStringLiteral("directionDegrees"), value)
                || setIntValue(p.spikeCount, name, QStringLiteral("spikeCount"), value)
                || setIntValue(p.durationFrames, name, QStringLiteral("durationFrames"), value);
        } else if constexpr (std::is_same_v<T, EnergyShieldParameters>) {
            changed = setDoubleValue(p.radius, name, QStringLiteral("radius"), value)
                || setDoubleValue(p.edgeSharpness, name, QStringLiteral("edgeSharpness"), value)
                || setDoubleValue(p.cellSize, name, QStringLiteral("cellSize"), value)
                || setDoubleValue(p.rippleStrength, name, QStringLiteral("rippleStrength"), value);
        }
        return changed;
    }, parameters);
}
