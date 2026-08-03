#pragma once

#include "Camera3D.h"
#include "Keyframe.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QVector>

#include <variant>

// Procedural VFX are image generators rather than video effects.  They always
// return a transparent premultiplied RGBA image, so the caller can place the
// result on a normal LayerCompositor layer (including Screen/Add blending).
enum class VfxGeneratorType {
    Explosion = 0,
    Lightning,
    ShockWave,
    EnergyBeam,
    MagicCircle,
    MuzzleFlash,
    EnergyShield,
    Count
};

struct ExplosionParameters {
    QPointF center = QPointF(0.5, 0.5);
    double scale = 0.32;             // fraction of the short canvas edge
    int fragmentCount = 28;
    double gravity = 140.0;          // pixels/sec^2 at scale 1
    double colorTemperature = 0.0;   // 0 = hot white, 1 = deep red
    double duration = 1.2;           // seconds
    double opacity = 1.0;
    unsigned int seed = 1337u;
    KeyframeManager keyframes;
};

struct LightningParameters {
    QPointF start = QPointF(0.18, 0.18);
    QPointF end = QPointF(0.82, 0.82);
    double branchProbability = 0.34;
    int recursionDepth = 5;
    double jitterWidth = 0.045;      // fraction of the short canvas edge
    double coreWidth = 2.5;          // pixels at scale 1
    QColor color = QColor(150, 220, 255, 255);
    double flickerRate = 9.0;        // pulses/sec
    double flickerDepth = 0.35;
    double opacity = 1.0;
    unsigned int seed = 7331u;
    KeyframeManager keyframes;
};

struct ShockWaveParameters {
    QPointF center = QPointF(0.5, 0.5);
    double initialRadius = 8.0;      // pixels
    double speed = 300.0;            // pixels/sec
    double ringWidth = 14.0;         // pixels
    double distortionStrength = 24.0;
    double decay = 1.8;
    double duration = 1.5;
    double opacity = 1.0;
    QColor color = QColor(130, 220, 255, 220);
    KeyframeManager keyframes;
};

struct EnergyBeamParameters {
    QPointF start = QPointF(0.18, 0.5);
    QPointF end = QPointF(0.82, 0.5);
    double coreWidth = 5.0;
    double haloWidth = 34.0;
    QColor color = QColor(80, 190, 255, 255);
    double noiseIntensity = 0.18;
    double flowSpeed = 3.0;
    double duration = 2.0;
    double opacity = 1.0;
    unsigned int seed = 9091u;
    KeyframeManager keyframes;
};

struct MagicCircleParameters {
    QPointF center = QPointF(0.5, 0.5);
    double radius = 0.28;             // fraction of the short canvas edge
    int ringCount = 3;
    QVector<double> rotationSpeeds = QVector<double>{0.75, -0.42, 0.23};
    int segmentCount = 12;
    double lineWidth = 2.0;
    QColor color = QColor(160, 100, 255, 235);
    Layer3DTransform tilt;
    double duration = 4.0;
    double opacity = 1.0;
    unsigned int seed = 5150u;
    KeyframeManager keyframes;
};

struct MuzzleFlashParameters {
    QPointF center = QPointF(0.5, 0.5);
    double directionDegrees = 0.0;
    double scale = 0.22;              // fraction of the short canvas edge
    int spikeCount = 10;
    int durationFrames = 2;
    double frameRate = 30.0;
    double opacity = 1.0;
    unsigned int seed = 4421u;
    KeyframeManager keyframes;

    double lifetimeSeconds() const
    {
        return durationFrames > 0 && frameRate > 0.0
            ? static_cast<double>(durationFrames) / frameRate
            : 0.0;
    }
};

struct EnergyShieldParameters {
    QPointF center = QPointF(0.5, 0.5);
    double radius = 0.34;             // fraction of the short canvas edge
    double edgeSharpness = 4.0;
    double cellSize = 34.0;            // pixels
    QColor color = QColor(70, 210, 255, 210);
    QVector<QPointF> impactPoints = QVector<QPointF>{QPointF(0.72, 0.46)};
    double rippleStrength = 1.0;
    double rippleSpeed = 260.0;
    double duration = 3.0;
    double opacity = 1.0;
    unsigned int seed = 8181u;
    KeyframeManager keyframes;
};

using VfxGeneratorParameters = std::variant<
    ExplosionParameters,
    LightningParameters,
    ShockWaveParameters,
    EnergyBeamParameters,
    MagicCircleParameters,
    MuzzleFlashParameters,
    EnergyShieldParameters>;

struct LightningGeometry {
    QVector<QPointF> mainPath;
    QVector<QVector<QPointF>> branches;
};

struct MagicCircleRingGeometry {
    double radius = 0.0;
    double angleRadians = 0.0;
};

struct MagicCircleGeometry {
    QVector<MagicCircleRingGeometry> rings;
};

struct VfxParameterSpec {
    QString name;
    QString displayName;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    bool integer = false;
};

class VfxGenerators
{
public:
    // This is the registry SSOT used by EffectLibraryModel::registerAll().
    static QVector<VfxGeneratorType> allTypes();
    static QString typeName(VfxGeneratorType type);
    static QString displayName(VfxGeneratorType type);
    static VfxGeneratorParameters defaultParameters(VfxGeneratorType type);
    static QVector<VfxParameterSpec> parameterSpecs(VfxGeneratorType type);

    static QImage render(VfxGeneratorType type, const QSize &canvasSize,
                         const VfxGeneratorParameters &parameters,
                         double timeSeconds);
    static QImage render(VfxGeneratorType type, const QSize &canvasSize,
                         double timeSeconds,
                         const VfxGeneratorParameters &parameters)
    {
        return render(type, canvasSize, parameters, timeSeconds);
    }
    static QImage renderDefault(VfxGeneratorType type, const QSize &canvasSize,
                                double timeSeconds);
    static double durationSeconds(VfxGeneratorType type,
                                  const VfxGeneratorParameters &parameters);

    static QImage renderExplosion(const QSize &canvasSize, double timeSeconds,
                                  const ExplosionParameters &parameters);
    static QImage renderLightning(const QSize &canvasSize, double timeSeconds,
                                  const LightningParameters &parameters);
    static QImage renderShockWave(const QSize &canvasSize, double timeSeconds,
                                  const ShockWaveParameters &parameters);
    static QImage applyShockWave(const QImage &source, double timeSeconds,
                                 const ShockWaveParameters &parameters);
    static QImage renderEnergyBeam(const QSize &canvasSize, double timeSeconds,
                                   const EnergyBeamParameters &parameters);
    static QImage renderMagicCircle(const QSize &canvasSize, double timeSeconds,
                                    const MagicCircleParameters &parameters);
    static QImage renderMuzzleFlash(const QSize &canvasSize, double timeSeconds,
                                    const MuzzleFlashParameters &parameters);
    static QImage renderEnergyShield(const QSize &canvasSize, double timeSeconds,
                                     const EnergyShieldParameters &parameters);

    static LightningGeometry lightningGeometry(const QSize &canvasSize,
                                               const LightningParameters &parameters,
                                               double timeSeconds = 0.0);
    static double shockWaveRadius(const ShockWaveParameters &parameters,
                                  double timeSeconds);
    static MagicCircleGeometry magicCircleGeometry(
        const QSize &canvasSize, const MagicCircleParameters &parameters,
        double timeSeconds);

    // Used by library inspectors and future keyframe editors. Scalar values
    // are evaluated through the existing KeyframeManager in each parameter
    // object, so the generator never switches to frame-number animation.
    static bool setParameter(VfxGeneratorParameters &parameters,
                             const QString &name, const QVariant &value);
};
