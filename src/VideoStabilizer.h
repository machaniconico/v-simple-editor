#pragma once

#include "PlanarTracker.h"
#include "libavcore/Decode.h"
#include "libavcore/Encode.h"

#include <QImage>
#include <QObject>
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

// --- Video Stabilizer (PRD-B3: single-pass deshake via libavcore::VideoFilterGraph) ---

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

    // Stabilize: Translation model runs the in-process deshake filter through
    // libavcore::VideoFilterGraph (PRD-B3 US-B3-3); PlanarInversion model runs
    // the in-process libavcore decoder/encoder pipeline (PRD-B2 US-B2-5).
    void stabilize(const QString &inputPath, const QString &outputPath,
                   const StabilizerConfig &config = {});

    void setModel(Model model);
    Model model() const { return m_model; }
    void setPlanarTrack(const planartrack::PlanarTrack *track);
    void setPlanarStabilizeAnchorFrame(int frameIndex);
    void setOutputCropPercent(double percent);
    const planartrack::PlanarTrack *planarTrack() const { return m_planarTrack; }
    int planarStabilizeAnchorFrame() const { return m_settings.planarStabilizeAnchorFrame; }
    double outputCropPercent() const { return m_settings.outputCropPercent; }
    QImage stabilizeFrame(const QImage &source, int frameIndex) const;

    // Build deshake filter string (US-B3: in-process libavfilter path).
    // Public so that US-B3-4 selftest can invoke it directly.
    static QString buildDeshakeFilter(const StabilizerConfig &config);

    // Abort current processing
    void cancel();

signals:
    void progressChanged(int percent);
    void stabilizeComplete(bool success, const QString &message);

private:
    static planartrack::Homography invertHomographyWithFallback(const planartrack::Homography &H,
                                                                bool &usedIdentityFallback);
    static planartrack::Homography multiplyHomographies(const planartrack::Homography &lhs,
                                                        const planartrack::Homography &rhs);
    static bool isIdentityHomography(const planartrack::Homography &H);

    void refreshAnchorHomography();
    bool stabilizePlanarInversion(const QString &inputPath, const QString &outputPath);
    bool stabilizeDeshake(const QString &inputPath, const QString &outputPath,
                          const StabilizerConfig &config);
    QImage applyPlanarInversion(const QImage &source, int frameIndex) const;
    static void applyTransparentCrop(QImage &image, double cropPercent);

    bool m_cancelled = false;
    Model m_model = Model::Translation;
    Settings m_settings;
    const planartrack::PlanarTrack *m_planarTrack = nullptr;
    planartrack::Homography m_anchorHomography = {1.0, 0.0, 0.0,
                                                  0.0, 1.0, 0.0,
                                                  0.0, 0.0, 1.0};
};
