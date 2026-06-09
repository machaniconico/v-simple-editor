#pragma once

#include "Camera3D.h"
#include "ExtrudedMesh.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QQuaternion>
#include <QSize>
#include <QString>
#include <QVector3D>

class Text3DLayer : public QObject
{
    Q_OBJECT

public:
    explicit Text3DLayer(QObject *parent = nullptr);

    void setText(const QString &text, const QFont &font);
    void setPerCharRotation(QVector3D perAxisAmount);
    void setPerCharPosition(QVector3D offset);
    void setPerCharScale(QVector3D scaleAmount);
    void setCameraDistance(double distance);
    void setRotationAnimAxis(QVector3D tumbleAxis);
    void setCamera(const Camera3D &camera);
    const Camera3D &camera() const;

    // --- True extrusion mode (US-3D-3) ---
    // When extrude is enabled, renderFrame() ignores the 2D-quad path and
    // instead builds a real extruded TriMesh from the glyph contours and
    // renders it with the software rasterizer. When disabled (the default),
    // renderFrame() behaves byte-for-byte like the original 2D-quad path.
    void setExtrudeEnabled(bool on);
    bool extrudeEnabled() const;

    // depth/bevel use the same normalised units as mesh3d::ExtrudeParams
    // (glyph contours are normalised to height = 1.0).
    void setExtrude(double depth, double bevelDepth, double bevelWidth, int bevelSegments);
    double extrudeDepth() const;
    double extrudeBevelDepth() const;
    double extrudeBevelWidth() const;
    int extrudeBevelSegments() const;

    // Material for the extruded mesh: front/back cap colour, side-wall colour,
    // ambient term, and light direction (view-space-ish, as per softras).
    void setMaterial(QColor frontColor, QColor sideColor, QColor ambient, QVector3D lightDir);
    QColor materialFrontColor() const;
    QColor materialSideColor() const;
    QColor materialAmbient() const;
    QVector3D materialLightDir() const;

    // Base orientation for the extruded mesh, in degrees. The rotation-anim
    // state (setRotationAnimAxis) adds a time-based spin on top:
    //   yaw   = baseYawDeg   + rotationAnimAxis.y() * time * kExtrudeSpinDegPerSec
    //   pitch = basePitchDeg + rotationAnimAxis.x() * time * kExtrudeSpinDegPerSec
    // (kExtrudeSpinDegPerSec == 90.0; see Text3DLayer.cpp).
    void setExtrudeYawPitch(double yawDeg, double pitchDeg);
    double extrudeBaseYaw() const;
    double extrudeBasePitch() const;

    // Number of times the extruded mesh has actually been (re)built. Used by
    // tests to verify the per-(text,font,params) mesh cache works.
    int meshBuildCount() const;

    QImage renderFrame(QSize size, double time, const Camera3D &cam) const;

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    double characterProgress(int characterIndex, double time) const;
    QImage renderFrameQuads(QSize size, double time, const Camera3D &cam) const;
    QImage renderFrameExtruded(QSize size, double time) const;
    void ensureExtrudedMesh() const;

    QString m_text;
    QFont m_font;
    QVector3D m_perCharRotation;
    QVector3D m_perCharPosition;
    QVector3D m_perCharScale;
    double m_cameraDistance = 400.0;
    Camera3D m_camera;
    QVector3D m_rotationAnimAxis;
    double m_staggerDelay = 0.08;
    double m_staggerDuration = 0.6;

    // --- Extrusion state ---
    bool m_extrudeEnabled = false;
    double m_extrudeDepth = 0.2;
    double m_extrudeBevelDepth = 0.02;
    double m_extrudeBevelWidth = 0.02;
    int m_extrudeBevelSegments = 2;
    QColor m_frontColor = QColor(220, 200, 120);
    QColor m_sideColor = QColor(180, 150, 90);
    QColor m_ambient = QColor(40, 40, 40);
    QVector3D m_lightDir = QVector3D(0.3f, 0.4f, -1.0f);
    double m_extrudeBaseYaw = 0.0;
    double m_extrudeBasePitch = 0.0;

    // --- Extruded mesh cache (keyed by text + font + extrude params) ---
    mutable mesh3d::TriMesh m_cachedMesh;
    mutable QString m_cacheKey;
    mutable bool m_cacheValid = false;
    mutable int m_meshBuildCount = 0;
};

QSize text3DPreviewProxySize(QSize fullSize);
bool shouldUseText3DPreviewProxy(const Text3DLayer &layer, QSize frameSize, int threshold);
QImage renderText3DProxy(const Text3DLayer &layer, QSize fullSize, double time, const Camera3D &cam);
