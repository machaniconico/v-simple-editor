#pragma once

#include "PlanarTracker.h"

#include <QImage>
#include <atomic>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

// --- Warp type ---

enum class WarpType {
    MeshWarp,
    PuppetPin,
    Bulge,
    Pinch,
    Twirl,
    Wave,
    Ripple,
    Spherize,
    Fisheye
};

enum class WarpMode {
    Mesh = 0,
    Puppet = 1,
    Parametric = 2,
    Homography = 3
};

// --- Puppet pin (anchor point with influence area) ---

struct WarpPin {
    QPointF originalPos;           // position before deformation
    QPointF deformedPos;           // position after deformation
    double radius = 100.0;        // influence radius in pixels
    double stiffness = 0.5;       // 0.0 (soft) to 1.0 (rigid)

    QJsonObject toJson() const;
    static WarpPin fromJson(const QJsonObject &obj);
};

// --- Mesh grid (rows x cols control-point lattice) ---

struct MeshGrid {
    int rows = 4;
    int cols = 4;
    QVector<QVector<QPointF>> controlPoints;  // [row][col]

    QJsonObject toJson() const;
    static MeshGrid fromJson(const QJsonObject &obj);
};

// --- Warp configuration ---

struct WarpConfig {
    WarpType type = WarpType::MeshWarp;

    // MeshWarp
    MeshGrid meshGrid;

    // PuppetPin
    QVector<WarpPin> pins;

    // Parametric warps (Bulge / Pinch / Twirl / Spherize / Fisheye)
    QPointF center = QPointF(0.5, 0.5);   // normalized (0-1)
    double radius = 0.5;                   // normalized
    double amount = 0.5;                   // effect strength

    // Twirl-specific
    double angle = 90.0;                   // degrees

    // Wave-specific
    double amplitude = 10.0;               // pixels
    double frequency = 0.05;               // cycles per pixel
    double phase = 0.0;                    // radians

    QJsonObject toJson() const;
    static WarpConfig fromJson(const QJsonObject &obj);
};

struct WarpDistortionSettings {
    int homographyOversample = 1;
};

WarpDistortionSettings &warpDistortionSettings();
void applyHomography(const QImage &src, const planartrack::Homography &H, QImage &dst);

// --- Warp & Distortion engine (static-only) ---

class WarpDistortion
{
public:
    WarpDistortion() = delete;

    // --- High-level dispatch ---

    static QImage applyWarp(const QImage &image, const WarpConfig &config);

    // --- Individual warp effects ---

    static QImage applyMeshWarp(const QImage &image, const MeshGrid &mesh);
    static QImage applyPuppetPin(const QImage &image, const QVector<WarpPin> &pins);

    static QImage applyBulge(const QImage &image, QPointF center,
                             double radius, double amount);
    static QImage applyPinch(const QImage &image, QPointF center,
                             double radius, double amount);
    static QImage applyTwirl(const QImage &image, QPointF center,
                             double radius, double angle);
    static QImage applyWave(const QImage &image, double amplitude,
                            double frequency, double phase);
    static QImage applyRipple(const QImage &image, QPointF center,
                              double amplitude, double frequency);
    static QImage applySpherize(const QImage &image, QPointF center,
                                double radius, double amount);
    static QImage applyFisheye(const QImage &image, QPointF center,
                               double radius, double amount);

    // --- Utility ---

    // Create a uniform grid of control points for MeshWarp
    static MeshGrid createDefaultMesh(const QSize &imageSize, int rows, int cols);

private:
    // Bilinear pixel sampling from a 32-bit ARGB image
    static QRgb bilinearSample(const QImage &image, double x, double y);
};

// Clip storage uses UV positions and a radius relative to the shorter frame side.
// The engine above intentionally continues to accept absolute source pixels.
namespace puppetpin {
inline std::atomic<bool> disabledForTesting{false};
inline std::atomic<quint64> invocationCountForTesting{0};
inline bool equal(const QVector<WarpPin> &a, const QVector<WarpPin> &b)
{
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].originalPos != b[i].originalPos || a[i].deformedPos != b[i].deformedPos
            || a[i].radius != b[i].radius || a[i].stiffness != b[i].stiffness) return false;
    return true;
}
inline QImage applyNormalized(const QImage &source, const QVector<WarpPin> &normalized)
{
    if (source.isNull() || normalized.isEmpty() || disabledForTesting) return source;
    QVector<WarpPin> pixels = normalized;
    for (auto &pin : pixels) {
        pin.originalPos = QPointF(pin.originalPos.x() * source.width(), pin.originalPos.y() * source.height());
        pin.deformedPos = QPointF(pin.deformedPos.x() * source.width(), pin.deformedPos.y() * source.height());
        pin.radius *= qMin(source.width(), source.height());
    }
    ++invocationCountForTesting;
    return WarpDistortion::applyPuppetPin(source, pixels).convertToFormat(source.format());
}
}
