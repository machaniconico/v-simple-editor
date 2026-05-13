#pragma once
#include <QImage>
#include <QString>
#include <QVector>

// ---------------------------------------------------------------------------
// FrameInterpolatorEngine — abstract base for interpolation backends
// ---------------------------------------------------------------------------

class FrameInterpolatorEngine {
public:
    virtual ~FrameInterpolatorEngine() = default;
    virtual QString name() const = 0;
    virtual QImage interpolate(const QImage& a, const QImage& b, double t) = 0;
};

// ---------------------------------------------------------------------------
// LinearBlendInterpolator — per-pixel weighted blend (baseline)
// ---------------------------------------------------------------------------

class LinearBlendInterpolator : public FrameInterpolatorEngine {
public:
    QString name() const override { return "Linear"; }
    QImage interpolate(const QImage& a, const QImage& b, double t) override;
};

// ---------------------------------------------------------------------------
// MotionBlendInterpolator — future RIFE-pluggable; currently linear delegate
// ---------------------------------------------------------------------------

class MotionBlendInterpolator : public FrameInterpolatorEngine {
public:
    QString name() const override { return "Motion-Blend"; }
    QImage interpolate(const QImage& a, const QImage& b, double t) override;
};

// ---------------------------------------------------------------------------
// FrameInterpolatorManager — registry of available engines
// ---------------------------------------------------------------------------

class FrameInterpolatorManager {
public:
    static QVector<FrameInterpolatorEngine*> engines();
    static FrameInterpolatorEngine* engineByName(const QString& name);
    static void registerEngine(FrameInterpolatorEngine* engine);
};
