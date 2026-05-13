#pragma once
#include <QDialog>
#include "ExportDialog.h"

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QDialogButtonBox;

class HDRSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit HDRSettingsDialog(const HDRSettings& initial, QWidget* parent = nullptr);
    HDRSettings settings() const;

private:
    QComboBox*        m_modeCombo    = nullptr;
    QDoubleSpinBox*   m_maxLumSpin   = nullptr;
    QDoubleSpinBox*   m_minLumSpin   = nullptr;
    QSpinBox*         m_maxCllSpin   = nullptr;
    QSpinBox*         m_maxFallSpin  = nullptr;
    QComboBox*        m_toneMapCombo = nullptr;
    QDialogButtonBox* m_buttonBox    = nullptr;
};
