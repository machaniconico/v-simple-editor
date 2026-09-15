#pragma once

#include "Camera3D.h"
#include "LayerCompositor.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;

class CameraMotionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CameraMotionDialog(QWidget *parent = nullptr);

    // Load a Camera3D's base state, shake, and keyframes into the widgets.
    void setCamera(const Camera3D &cam);

    // Build and return a Camera3D from the current widget state.
    Camera3D camera() const;

    // Store scene layers for preview (optional; falls back to wireframe grid).
    void setSceneLayers(const QVector<CompositeLayer> &layers);

signals:
    void cameraChanged();

private slots:
    void onBaseCameraEdited();
    void onShakeEdited();
    void onPresetChanged(int index);
    void onAddKeyframe();
    void onRemoveKeyframe();
    void onPreviewTimeChanged(int sliderValue);

private:
    // Internal state
    Camera3D            m_camera;
    QVector<CompositeLayer> m_sceneLayers;

    // Base camera group
    QGroupBox       *m_baseCameraGroup  = nullptr;
    QDoubleSpinBox  *m_posX             = nullptr;
    QDoubleSpinBox  *m_posY             = nullptr;
    QDoubleSpinBox  *m_posZ             = nullptr;
    QDoubleSpinBox  *m_tgtX             = nullptr;
    QDoubleSpinBox  *m_tgtY             = nullptr;
    QDoubleSpinBox  *m_tgtZ             = nullptr;
    QDoubleSpinBox  *m_fov              = nullptr;
    QDoubleSpinBox  *m_roll             = nullptr;

    QCheckBox       *m_trueProjection   = nullptr;

    // Keyframes group
    QGroupBox       *m_kfGroup          = nullptr;
    QTableWidget    *m_kfTable          = nullptr;
    QPushButton     *m_addKfBtn         = nullptr;
    QPushButton     *m_removeKfBtn      = nullptr;

    // Preset combo
    QComboBox       *m_presetCombo      = nullptr;

    // Shake group
    QGroupBox       *m_shakeGroup       = nullptr;
    QCheckBox       *m_shakeEnabled     = nullptr;
    QDoubleSpinBox  *m_shakeFreq        = nullptr;
    QDoubleSpinBox  *m_shakePosX        = nullptr;
    QDoubleSpinBox  *m_shakePosY        = nullptr;
    QDoubleSpinBox  *m_shakePosZ        = nullptr;
    QDoubleSpinBox  *m_shakeRotDeg      = nullptr;
    QSpinBox        *m_shakeSeed        = nullptr;
    QDoubleSpinBox  *m_shakeSmooth      = nullptr;

    // Preview
    QLabel          *m_previewLabel     = nullptr;
    QSlider         *m_previewSlider    = nullptr;

    // Dialog buttons
    QDialogButtonBox *m_buttonBox       = nullptr;

    // Helper methods
    void buildUi();
    void refreshFromCamera();
    void refreshPreview();
    void applyWidgetsToCamera();

    Camera3DState baseStateFromWidgets() const;
    CameraShake   shakeFromWidgets() const;
    void          applyStateToWidgets(const Camera3DState &state);
    void          applyShakeToWidgets(const CameraShake &shake);

    // Temporarily block signals to avoid recursive updates
    void blockAllSignals(bool block);

    double previewTimeSec() const;
};
