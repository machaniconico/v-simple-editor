#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QVector>
#include <QPair>

// --- Speed point easing ---

enum class SpeedEasing {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut
};

// --- Speed point (time + speed + easing curve) ---

struct SpeedPoint {
    double time = 0.0;       // seconds into the clip
    double speed = 1.0;      // 0.1 to 10.0 (1.0 = normal)
    SpeedEasing easing = SpeedEasing::Linear;
};

// --- Speed ramp configuration ---

struct SpeedRampConfig {
    QVector<SpeedPoint> points;
    bool preservePitch = true;  // true = atempo, false = sample rate change
};

// --- Speed Ramp Processor (variable speed via FFmpeg) ---

class SpeedRamp : public QObject
{
    Q_OBJECT

public:
    explicit SpeedRamp(QObject *parent = nullptr);

    // --- Point management ---

    void addPoint(double time, double speed,
                  SpeedEasing easing = SpeedEasing::Linear);
    void removePoint(int index);
    void clearPoints();

    const QVector<SpeedPoint> &points() const { return m_config.points; }
    SpeedRampConfig &config() { return m_config; }
    const SpeedRampConfig &config() const { return m_config; }

    // --- Speed interpolation ---

    // Interpolate speed at a given time based on speed points and easing
    static double speedAtTime(const QVector<SpeedPoint> &points, double time);

    // --- Processing ---

    // Apply variable speed ramp using FFmpeg setpts and atempo filters
    void applySpeedRamp(const QString &inputPath, const QString &outputPath,
                        const SpeedRampConfig &config);

    // Abort current processing
    void cancel();

    // --- Utility ---

    // Generate (time, speed) samples for visualization / curve display
    static QVector<QPair<double, double>> generateSpeedCurve(
        const SpeedRampConfig &config, double duration, int sampleCount = 200);

    // Compute resulting duration after applying the speed ramp
    static double calculateNewDuration(const SpeedRampConfig &config,
                                       double originalDuration);

signals:
    void progressChanged(int percent);
    void rampComplete(bool success, const QString &message);

private:
    // --- Easing helpers ---
    static double easeIn(double t);
    static double easeOut(double t);
    static double easeInOut(double t);
    static double applyEasing(double t, SpeedEasing easing);

    // --- FFmpeg helpers ---

    // Build atempo filter chain for a given speed factor
    // atempo only accepts 0.5-100.0, so chain multiple for extreme values
    static QString buildAtempoChain(double speed);

    // Build audio filter for a segment (atempo or asetrate for pitch shift)
    static QString buildAudioFilter(double speed, bool preservePitch);

    // Run an FFmpeg process, parse progress from stderr
    bool runFFmpeg(const QStringList &args, int progressBase, int progressSpan);

    // Parse duration and time= from FFmpeg stderr output
    void parseProgress(const QString &output, int progressBase, int progressSpan);

    // Find FFmpeg binary
    static QString findFFmpegBinary();

    SpeedRampConfig m_config;
    QProcess *m_process = nullptr;
    bool m_cancelled = false;
    double m_totalDuration = 0.0;  // seconds, parsed from Duration: line
};
