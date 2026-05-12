#pragma once

#include <QColor>
#include <QImage>
#include <QMap>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>

// --- Particle Type ---

enum class ParticleType {
    Snow,
    Rain,
    Spark,
    Smoke,
    Fire,
    Confetti,
    Dust,
    Bubble,
    Star,
    Custom
};

// --- Per-particle state (internal) ---

struct Particle {
    QPointF position;
    QPointF velocity;
    QPointF acceleration;

    double life = 0.0;           // remaining life (seconds)
    double maxLife = 1.0;        // total lifespan (seconds)
    double size = 4.0;
    double rotation = 0.0;       // degrees
    double rotationSpeed = 0.0;  // degrees/sec

    QColor color = Qt::white;
    double opacity = 1.0;
};

// --- Force Field ---

struct ForceField {
    enum Kind {
        PointAttract,
        PointRepel,
        Vortex,
        Wind
    };
    Kind kind = PointAttract;
    QPointF position = QPointF(0.5, 0.5);  // normalized (0-1)
    double strength = 100.0;               // px/s^2 at radius=0
    double radius = 0.4;                   // normalized falloff radius
};

// --- Emitter Configuration ---

struct ParticleEmitterConfig {
    ParticleType type = ParticleType::Snow;

    // Emission
    double emitRate = 50.0;        // particles per second
    int maxParticles = 500;
    QPointF emitPosition = QPointF(0.5, 0.0);  // normalized (0-1)
    QSizeF emitAreaSize = QSizeF(1.0, 0.0);    // normalized width/height

    // Lifetime
    double lifeMin = 2.0;         // seconds
    double lifeMax = 4.0;

    // Size
    double sizeMin = 2.0;
    double sizeMax = 6.0;

    // Speed & direction
    double speedMin = 30.0;       // pixels/sec
    double speedMax = 80.0;
    double direction = 270.0;     // degrees (270 = downward)
    double spread = 30.0;         // degrees spread around direction

    // Forces
    QPointF gravity = QPointF(0.0, 0.0);   // pixels/sec^2
    QPointF wind = QPointF(0.0, 0.0);      // pixels/sec^2

    // Force fields
    QVector<ForceField> forceFields;

    // Collision
    bool collisionFloor = false;
    double floorY = 1.0;           // normalized; particles bounce when position.y >= floorY*canvasH
    double restitution = 0.5;      // 0..1 velocity retained on bounce
    double floorFriction = 0.1;    // tangential damping on bounce

    // Turbulence
    double turbulenceAmount = 0.0; // px/s^2
    double turbulenceScale = 3.0;  // spatial frequency
    double turbulenceSpeed = 1.0;  // temporal evolution rate

    // Color
    QColor startColor = Qt::white;
    QColor endColor = Qt::white;

    // Opacity ramp (fraction of life 0-1)
    double fadeIn = 0.1;          // ramp up over first 10% of life
    double fadeOut = 0.2;         // ramp down over last 20% of life

    // Size over life multipliers
    double sizeStartMult = 1.0;
    double sizeEndMult = 1.0;
};

// --- Particle System ---

class ParticleSystem
{
public:
    ParticleSystem();

    // --- Configuration ---

    void setConfig(const ParticleEmitterConfig &config);
    const ParticleEmitterConfig &config() const { return m_config; }

    // --- Simulation ---

    void update(double deltaTime);
    void reset();
    int particleCount() const { return m_particles.size(); }

    // --- Rendering ---

    // Render current particle state onto a transparent QImage
    QImage renderFrame(const QSize &canvasSize, double time);

    // Generate a complete sequence of frames
    QVector<QImage> renderParticleSequence(const QSize &canvasSize,
                                           double startTime, double endTime,
                                           double fps = 30.0);

    // --- Presets ---

    static QMap<QString, ParticleEmitterConfig> presetConfigs();

private:
    // --- Spawning ---

    void spawnParticle(const QSize &canvasSize);

    // --- Per-particle helpers ---

    static double lifeFraction(const Particle &p);
    static double computeOpacity(const Particle &p, const ParticleEmitterConfig &cfg);
    static QColor interpolateColor(const QColor &a, const QColor &b, double t);
    static double computeSize(const Particle &p, const ParticleEmitterConfig &cfg);

    // --- Force field evaluation ---

    QPointF evaluateForceFields(const QPointF &normPos, const QSize &canvasSize) const;

    // --- Render helpers ---

    void renderParticle(QPainter &painter, const Particle &p,
                        const ParticleEmitterConfig &cfg) const;

    // --- Internal physics step (shared by update and renderParticleSequence) ---

    void stepParticles(double deltaTime, const QSize &canvasSize);

    ParticleEmitterConfig m_config;
    QVector<Particle> m_particles;
    double m_emitAccumulator = 0.0;  // fractional particle accumulation
    double m_simTime = 0.0;          // accumulated simulation time for turbulence
};
