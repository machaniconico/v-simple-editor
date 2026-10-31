#pragma once

#include "CameraSolver.h"
#include "Keyframe.h"
#include "LayerCompositor.h"

#include <QImage>
#include <QJsonObject>
#include <QJsonArray>
#include <QPointF>
#include <QPolygonF>
#include <QSize>
#include <QVector>
#include <QVector3D>

#include <optional>

// --- Camera state at a point in time ---

struct Camera3DState {
    QVector3D position  = QVector3D(0.0f, 0.0f, 0.0f);
    QVector3D target    = QVector3D(0.0f, 0.0f, -1.0f);
    double fov          = 60.0;    // focal length in canvas pixels (legacy semantics)
    double nearPlane    = 0.1;
    double farPlane     = 1000.0;
    double roll         = 0.0;     // degrees

    bool trueProjection = false;

    bool isDefault() const {
        return position == QVector3D(0.0f, 0.0f, 0.0f)
            && target == QVector3D(0.0f, 0.0f, -1.0f)
            && fov == 60.0 && nearPlane == 0.1
            && farPlane == 1000.0 && roll == 0.0 && !trueProjection;
    }

    void reset() { *this = Camera3DState{}; }

    QJsonObject toJson() const;
    static Camera3DState fromJson(const QJsonObject &obj);
};

// --- Camera shake — procedural jitter layered on top of keyframed base ---

struct CameraShake {
    double frequency = 4.0;
    QVector3D positionAmplitude = {0, 0, 0};
    double rotationAmplitudeDeg = 0.0;  // applied to roll
    unsigned int seed = 1;
    double smoothness = 1.0;            // >1 = lazier handheld, <1 = jittery
    bool enabled = false;
};

// --- Camera property enum for keyframe tracks ---

enum class Camera3DProperty {
    PositionX,
    PositionY,
    PositionZ,
    TargetX,
    TargetY,
    TargetZ,
    Fov,
    Roll,
    Count   // sentinel — must remain last
};

// --- Camera 3D ---

class Camera3D
{
public:
    Camera3D();
    explicit Camera3D(const Camera3DState &state);

    // --- Camera state ---

    void setCamera(const Camera3DState &state);
    Camera3DState camera() const { return m_state; }

    // --- Layer depth ---

    void setLayerDepth(int layerIndex, double z);
    double layerDepth(int layerIndex) const;

    void setLayer3DTransform(int layerIndex, const Layer3DTransform &transform);
    Layer3DTransform layer3DTransform(int layerIndex) const;

    // --- 3D projection ---

    QPointF projectTo2D(const QVector3D &point3D, const QSize &canvasSize) const;

    // --- Scene rendering ---

    QImage renderScene(const QVector<CompositeLayer> &layers,
                       const QVector<QImage> &layerImages,
                       const QSize &canvasSize, double time,
                       const QVector<Light3DState> &lights = {});

    // --- Perspective transform (static utility) ---

    static QImage applyPerspective(const QImage &image,
                                   const Layer3DTransform &layer3D,
                                   const Camera3DState &cameraState,
                                   const QSize &canvasSize);

    static QPolygonF projectLayerQuad(const Layer3DTransform &layer3D,
                                      const Camera3DState &cameraState,
                                      const QSize &canvasSize);
    static QImage applyTrueProjection(const QImage &image,
                                      const Layer3DTransform &layer3D,
                                      const Camera3DState &cameraState,
                                      const QSize &canvasSize);
    // Calling-thread-only selftest controls; disabled mode bypasses the new step.
    static void setTrueProjectionEnabledForTest(bool enabled);
    static int trueProjectionCallCountForTest();

    // --- Camera keyframes ---

    void setCameraKeyframe(double time, const Camera3DState &state,
                           KeyframePoint::Interpolation interp = KeyframePoint::Linear);
    Camera3DState getCameraAt(double time) const;

    bool hasAnimation() const;
    QVector<double> allKeyframeTimes() const;

    // --- Per-property keyframe access ---

    KeyframeTrack *track(Camera3DProperty property);
    const KeyframeTrack *track(Camera3DProperty property) const;

    // --- Camera shake ---

    void setShake(const CameraShake &s);
    CameraShake shake() const { return m_shake; }

    // --- Feature camera-solve mapping (JSON "solveMapping"; absent when unset) ---

    void setSolveMapping(const std::optional<camsolve::SolveMapping> &mapping) { m_solveMapping = mapping; }
    std::optional<camsolve::SolveMapping> solveMapping() const { return m_solveMapping; }

    // Bundle point cloud in reference-centred solver units (JSON "solvePoints"; omitted when empty).
    void setSolvePoints(const QVector<QVector3D> &points) { m_solvePoints = points; }
    const QVector<QVector3D> &solvePoints() const { return m_solvePoints; }
    // The cloud in world px, S*p - f*e3; empty without a solve mapping.
    QVector<QVector3D> solvePointsWorldPx() const;

    // --- Built-in camera moves (static factory methods) ---

    static Camera3D createDollyZoom(double startZ, double endZ, double duration);
    static Camera3D createPanShot(double startX, double endX, double duration);
    static Camera3D createOrbitShot(const QVector3D &centerPoint, double radius,
                                    double duration);
    static Camera3D createZoomShot(double startFov, double endFov, double duration);

    // --- Shake presets (static factory methods) ---

    static Camera3D createHandheld(const Camera3DState &base, double intensity = 1.0);
    static Camera3D createEarthquake(const Camera3DState &base, double intensity = 1.0);
    static Camera3D createSubtleDrift(const Camera3DState &base, double intensity = 1.0);

    // --- Serialisation ---

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

    // --- Utility ---

    static QString propertyName(Camera3DProperty property);
    static Camera3DProperty propertyFromName(const QString &name);
    static double propertyDefaultValue(Camera3DProperty property);

private:
    Camera3DState m_state;

    // One KeyframeTrack per Camera3DProperty (indexed by enum value)
    QVector<KeyframeTrack> m_tracks;

    // Per-layer 3D transforms (indexed by layer index)
    QVector<Layer3DTransform> m_layerTransforms;

    CameraShake m_shake;

    std::optional<camsolve::SolveMapping> m_solveMapping;
    QVector<QVector3D> m_solvePoints;

    // --- Deterministic value-noise helpers (shake) ---

    static unsigned int hashMix(unsigned int h);
    static double hashNoise(double x, unsigned int seed);
    static double smoothStep(double t);
    static double interpolatedNoise(double x, unsigned int seed);
    static double fbmNoise(double x, unsigned int seed, double smoothness);

    void ensureTracks();
    int trackIndex(Camera3DProperty property) const;
    void ensureLayerIndex(int index);
};

struct ClipInfo;

namespace camera3d {
// US-1509: the 3D layer transform a projection / lighting site must use. Returns
// clip.layer3D unchanged unless the clip is a 3D layer with autoOrientToCamera under a
// trueProjection camera; then rotationX / rotationY face the layer at layerPosPx
// (canvas px, centre origin, y down) at the camera position (rotationZ is kept).
Layer3DTransform effectiveLayer3DAt(const ClipInfo &clip, QPointF layerPosPx,
                                    const Camera3DState &camera);
// US-1813: layer3D for a projection done at uploadSize instead of the canvas: worldX / worldY
// (canvas px) scale by uploadW / canvasW, Z and the rotations are kept (fov is not scaled).
// Unchanged when either size is empty.
Layer3DTransform layer3DForPreviewScale(const Layer3DTransform &layer3D, const QSize &uploadSize,
                                        const QSize &canvasSize);
// US-1810: canvas px of a world point (canvas px, centre origin, y down) under the
// projectLayerQuad projection; nullopt when fov + view z <= 1e-3 (at / behind the pinhole).
std::optional<QPointF> projectWorldPoint(const QVector3D &worldPx, const Camera3DState &camera,
                                         const QSize &canvas);
} // namespace camera3d

// Shared opt-in compositing step for preview and export.
QImage applyProjectCameraProjection(const QImage &image,
                                    const Layer3DTransform &layer3D,
                                    bool is3DLayer,
                                    const Camera3DState &camera,
                                    const QSize &canvasSize);
