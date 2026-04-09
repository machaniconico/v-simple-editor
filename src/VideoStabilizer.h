#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

// --- Stabilization crop mode ---

enum class StabCropMode {
    Keep,   // keep black borders (no cropping)
    Crop    // crop to remove borders
};

// --- Interpolation method ---

enum class StabInterpolation {
    Bilinear,
    Bicubic
};

// --- Stabilizer configuration ---

struct StabilizerConfig {
    int shakiness = 5;          // 1-10: how shaky the input is
    int accuracy = 15;          // 1-15: detection accuracy
    int smoothing = 10;         // 1-100: frames used for smoothing (higher = smoother)
    StabCropMode cropMode = StabCropMode::Keep;
    double zoom = 0.0;          // -100 to 100: additional zoom to hide borders
    StabInterpolation interpolation = StabInterpolation::Bicubic;
};

// --- Video Stabilizer (two-pass via FFmpeg vidstab) ---

class VideoStabilizer : public QObject
{
    Q_OBJECT

public:
    explicit VideoStabilizer(QObject *parent = nullptr);

    // Two-pass stabilization: analyze then apply
    void stabilize(const QString &inputPath, const QString &outputPath,
                   const StabilizerConfig &config = {});

    // Run only pass 1 (analysis), emits analysisComplete with .trf path
    void analyzeOnly(const QString &inputPath, const StabilizerConfig &config = {});

    // Abort current processing
    void cancel();

signals:
    void progressChanged(int percent);
    void stabilizeComplete(bool success, const QString &message);
    void analysisComplete(const QString &trfPath);

private:
    // Build vidstabdetect filter string for pass 1
    static QString buildDetectFilter(const StabilizerConfig &config,
                                     const QString &trfPath);

    // Build vidstabtransform filter string for pass 2
    static QString buildTransformFilter(const StabilizerConfig &config,
                                        const QString &trfPath);

    // Run an FFmpeg process and parse progress from stderr
    bool runFFmpeg(const QStringList &args, int progressBase, int progressSpan);

    // Parse duration and time= from FFmpeg stderr output
    void parseProgress(const QString &output, int progressBase, int progressSpan);

    // Find FFmpeg binary
    static QString findFFmpegBinary();

    QProcess *m_process = nullptr;
    bool m_cancelled = false;
    double m_totalDuration = 0.0;  // seconds, parsed from Duration: line
};
