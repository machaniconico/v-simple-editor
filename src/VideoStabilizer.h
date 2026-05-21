#pragma once

#include "PlanarTracker.h"

#include <QImage>
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
    enum class Model {
        Translation = 0,
        PlanarInversion
    };

    struct Settings {
        int planarStabilizeAnchorFrame = 0;
        double outputCropPercent = 0.0;
    };

    explicit VideoStabilizer(QObject *parent = nullptr);

    // Two-pass stabilization: analyze then apply
    void stabilize(const QString &inputPath, const QString &outputPath,
                   const StabilizerConfig &config = {});

    // Run only pass 1 (analysis), emits analysisComplete with .trf path
    void analyzeOnly(const QString &inputPath, const StabilizerConfig &config = {});

    void setModel(Model model);
    Model model() const { return m_model; }
    void setPlanarTrack(const planartrack::PlanarTrack *track);
    void setPlanarStabilizeAnchorFrame(int frameIndex);
    void setOutputCropPercent(double percent);
    const planartrack::PlanarTrack *planarTrack() const { return m_planarTrack; }
    int planarStabilizeAnchorFrame() const { return m_settings.planarStabilizeAnchorFrame; }
    double outputCropPercent() const { return m_settings.outputCropPercent; }
    QImage stabilizeFrame(const QImage &source, int frameIndex) const;

    // Abort current processing
    void cancel();

signals:
    void progressChanged(int percent);
    void stabilizeComplete(bool success, const QString &message);
    void analysisComplete(const QString &trfPath);

private:
    struct VideoStreamInfo {
        int width = 0;
        int height = 0;
        double fps = 0.0;
    };

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
    static VideoStreamInfo probeVideoStream(const QString &inputPath);
    static planartrack::Homography invertHomographyWithFallback(const planartrack::Homography &H,
                                                                bool &usedIdentityFallback);
    static planartrack::Homography multiplyHomographies(const planartrack::Homography &lhs,
                                                        const planartrack::Homography &rhs);
    static bool isIdentityHomography(const planartrack::Homography &H);

    void refreshAnchorHomography();
    bool stabilizePlanarInversion(const QString &inputPath, const QString &outputPath);
    QImage applyPlanarInversion(const QImage &source, int frameIndex) const;
    static void applyTransparentCrop(QImage &image, double cropPercent);

    QProcess *m_process = nullptr;
    bool m_cancelled = false;
    double m_totalDuration = 0.0;  // seconds, parsed from Duration: line
    Model m_model = Model::Translation;
    Settings m_settings;
    const planartrack::PlanarTrack *m_planarTrack = nullptr;
    planartrack::Homography m_anchorHomography = {1.0, 0.0, 0.0,
                                                  0.0, 1.0, 0.0,
                                                  0.0, 0.0, 1.0};
};
