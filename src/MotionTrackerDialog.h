#pragma once

#include <QDialog>
#include <QList>
#include <QMetaType>

#include "TrackerPreset.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

class MotionTrackerDialog : public QDialog {
    Q_OBJECT

public:
    explicit MotionTrackerDialog(QWidget* parent = nullptr);

    tracker_preset::TrackerPreset selectedPreset() const;

public slots:
    void accept() override;

signals:
    void presetApplied(const tracker_preset::TrackerPreset& p);

private slots:
    void onPresetSelectionChanged(int index);
    void onSaveCustomPreset();
    void onDeleteSelectedPreset();
    void onResetToDefaults();
    void onExportPreset();
    void onImportPreset();

private:
    void rebuildPresetCombo(const QString& selectedId = QString());
    void applyPresetToWidgets(const tracker_preset::TrackerPreset& preset);
    void setWidgetSignalsBlocked(bool blocked);
    void updateDeletePresetButton();
    int currentPresetIndex() const;

    QLabel*          m_descriptionLabel            = nullptr;
    QComboBox*       m_presetCombo                 = nullptr;
    QSpinBox*        m_searchRadiusSpin            = nullptr;
    QComboBox*       m_matchMetricCombo            = nullptr;
    QCheckBox*       m_kalmanEnabledCheck          = nullptr;
    QDoubleSpinBox*  m_kalmanProcessNoiseSpin      = nullptr;
    QDoubleSpinBox*  m_kalmanMeasurementNoiseSpin  = nullptr;
    QDoubleSpinBox*  m_occlusionGateSpin           = nullptr;
    QCheckBox*       m_subPixelEnabledCheck        = nullptr;
    QDoubleSpinBox*  m_minConfidenceSpin           = nullptr;
    QPushButton*     m_saveCustomPresetButton      = nullptr;
    QPushButton*     m_deletePresetBtn             = nullptr;
    QPushButton*     m_resetBtn                    = nullptr;
    QPushButton*     m_exportBtn                   = nullptr;
    QPushButton*     m_importBtn                   = nullptr;
    QDialogButtonBox* m_buttonBox                  = nullptr;

    QList<tracker_preset::TrackerPreset> m_presets;
};

Q_DECLARE_METATYPE(tracker_preset::TrackerPreset)
