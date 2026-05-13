#pragma once

#include <QImage>
#include <QString>
#include <QVector>

// Abstract interface for upscale engines — future plug-in implementations
// (e.g. Real-ESRGAN, ESPCN, Waifu2x) inherit from AIUpscaleEngine.
class AIUpscaleEngine {
public:
    virtual ~AIUpscaleEngine() = default;
    virtual QString name() const = 0;
    virtual int maxScale() const = 0;
    virtual QImage upscale(const QImage& src, int scale) = 0;
};

// --- Baseline engines ---

class LanczosUpscaler : public AIUpscaleEngine {
public:
    QString name() const override { return "Lanczos3"; }
    int maxScale() const override { return 4; }
    QImage upscale(const QImage& src, int scale) override;
};

class BicubicUpscaler : public AIUpscaleEngine {
public:
    QString name() const override { return "Bicubic"; }
    int maxScale() const override { return 4; }
    QImage upscale(const QImage& src, int scale) override;
};

// --- Engine registry ---

class AIUpscaleManager {
public:
    // Returns all registered engines (Lanczos + Bicubic + any registered).
    static QVector<AIUpscaleEngine*> engines();

    // Linear search by name(). Returns nullptr if not found.
    static AIUpscaleEngine* engineByName(const QString& name);

    // Register an external engine. Caller retains ownership.
    static void registerEngine(AIUpscaleEngine* engine);
};
