#pragma once
#include <QDialog>
#include <QImage>
#include <QPointF>

#include "SocialPreset.h"
#include "AspectReframer.h"

class QComboBox;
class QLabel;
class QSlider;
class QPushButton;
class QDialogButtonBox;
class QGroupBox;

class SocialExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit SocialExportDialog(QWidget* parent = nullptr);

    // 元動画から 1 フレームをサンプルとして渡す (プレビュー用)
    void setSampleFrame(const QImage& sample);

    // 現在の選択を取得
    social::Preset      selectedPreset() const;
    reframe::ReframeParams reframeParams() const;

signals:
    void exportRequested(const social::Preset& preset,
                         const reframe::ReframeParams& reframeParams);

private slots:
    void onPresetChanged(int index);
    void onReframeModeChanged(int index);
    void onManualOffsetXChanged(int value);  // slider 0..100 → 0..1
    void onManualOffsetYChanged(int value);
    void onZoomChanged(int value);            // 100..400 → 1.0..4.0
    void onExportClicked();

private:
    void updatePreview();
    void updateReframeControlsEnabled();

    QImage m_sample;

    QComboBox*        m_presetCombo      = nullptr;
    QLabel*           m_presetInfoLabel  = nullptr;

    QGroupBox*        m_reframeGroup     = nullptr;
    QComboBox*        m_modeCombo        = nullptr;
    QSlider*          m_manualXSlider    = nullptr;
    QSlider*          m_manualYSlider    = nullptr;
    QSlider*          m_zoomSlider       = nullptr;
    QLabel*           m_manualXLabel     = nullptr;
    QLabel*           m_manualYLabel     = nullptr;
    QLabel*           m_zoomLabel        = nullptr;

    QLabel*           m_previewLabel     = nullptr;
    QLabel*           m_sourcePreviewLabel = nullptr;

    QDialogButtonBox* m_buttonBox        = nullptr;
};
