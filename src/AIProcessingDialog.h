#pragma once
#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QSpinBox;
class QDialogButtonBox;

struct AIProcessingSettings {
    bool upscaleEnabled = false;
    QString upscaleEngine = "Lanczos3";
    int upscaleFactor = 2;          // 1..4
    bool frameInterpEnabled = false;
    QString frameInterpEngine = "Linear";
    int frameInterpFactor = 2;      // 中間フレーム枚数 = factor - 1
};

class AIProcessingDialog : public QDialog {
    Q_OBJECT
public:
    explicit AIProcessingDialog(const AIProcessingSettings& initial, QWidget* parent = nullptr);
    AIProcessingSettings settings() const;

private:
    QCheckBox*        m_upscaleEnabledCheck    = nullptr;
    QComboBox*        m_upscaleEngineCombo      = nullptr;
    QSpinBox*         m_upscaleFactorSpin       = nullptr;
    QCheckBox*        m_frameInterpEnabledCheck = nullptr;
    QComboBox*        m_frameInterpEngineCombo  = nullptr;
    QSpinBox*         m_frameInterpFactorSpin   = nullptr;
    QDialogButtonBox* m_buttonBox               = nullptr;
};
