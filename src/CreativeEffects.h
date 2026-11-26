#pragma once

#include "Camera3D.h"
#include "Light3D.h"

#include <QDialog>
#include <QImage>
#include <QString>

class QCheckBox;
class QDoubleSpinBox;

namespace creative {

struct HalationParams {
    bool enabled = false;
    double threshold = 0.78;  // luminance threshold [0.0..1.0]
    double radiusPx = 12.0;   // highlight bleed blur radius in px [0.5..64.0]
    double strength = 0.65;   // screen-blend intensity [0.0..2.0]
};

struct PosterizeTimeParams {
    bool enabled = false;
    double targetFps = 12.0;  // quantized frame rate [0.5..120.0]
};

struct CameraDofParams {
    bool enabled = false;
    double focusDistance = 1000.0; // focal plane distance in canvas px
    double aperture = 50.0;        // aperture factor (larger = shallower DOF)
    double maxBlurPx = 18.0;       // clamp for bokeh blur radius [0.0..64.0]
};

struct ClipCreativeConfig {
    HalationParams halation;
    PosterizeTimeParams posterizeTime;
    CameraDofParams cameraDof;

    bool isAnyEnabled() const {
        return halation.enabled || posterizeTime.enabled || cameraDof.enabled;
    }
};

// Fast-path guard for TimelineFrameRenderer: single atomic read when no clip
// has creative effects enabled (zero QSettings disk I/O per frame).
bool hasAnyActiveConfig();

ClipCreativeConfig configForClip(const QString &clipUid);
void setConfigForClip(const QString &clipUid, const ClipCreativeConfig &cfg);
void clearConfigForClip(const QString &clipUid);
void clearAllConfigsForTest();

// Posterize Time: quantizes clip-local seconds onto discrete targetFps steps.
// Strict no-op when disabled or targetFps <= 0.
double quantizePosterizeTime(double localSec, const PosterizeTimeParams &params);

// Film Halation: extracts highlights above threshold, blurs with a warm
// red-orange glow, and screen-blends back while preserving source alpha.
QImage applyFilmHalation(const QImage &src, const HalationParams &params);

// 3D Camera Depth of Field: computes thin-lens Circle of Confusion blur radius
// from layer depth vs camera focus plane, and applies premultiplied bokeh blur.
double layerDistanceFromCamera(const Layer3DTransform &layer3D,
                               const Camera3DState &camera);
double computeDofBlurRadiusPx(double layerDistance, const CameraDofParams &params);
QImage applyCameraDofBlur(const QImage &src, double radiusPx);

class CreativeEffectsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CreativeEffectsDialog(const QString &clipUid,
                                   const QString &clipLabel,
                                   QWidget *parent = nullptr);

    ClipCreativeConfig currentConfig() const;

private:
    void applyFromConfig(const ClipCreativeConfig &cfg);
    void saveAndAccept();

    QString m_clipUid;

    QCheckBox *m_halationCheck = nullptr;
    QDoubleSpinBox *m_halationThreshold = nullptr;
    QDoubleSpinBox *m_halationRadius = nullptr;
    QDoubleSpinBox *m_halationStrength = nullptr;

    QCheckBox *m_posterizeCheck = nullptr;
    QDoubleSpinBox *m_posterizeFps = nullptr;

    QCheckBox *m_dofCheck = nullptr;
    QDoubleSpinBox *m_dofFocus = nullptr;
    QDoubleSpinBox *m_dofAperture = nullptr;
    QDoubleSpinBox *m_dofMaxBlur = nullptr;
};

} // namespace creative
