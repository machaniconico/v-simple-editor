#pragma once

#include <QObject>
#include <QDialog>

class QLineEdit;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QSlider;

// ---------------------------------------------------------------------------
// AudioRestorationDialog — Sprint 22 / US-AREST-1
//
// Modeless dialog driving the audiorestore pipeline. Lets the user pick an
// audio file, toggle the de-click / de-hum / noise-gate stages, choose the
// mains hum fundamental (50 / 60 Hz), and apply the restoration.
// ---------------------------------------------------------------------------
class AudioRestorationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AudioRestorationDialog(QWidget *parent = nullptr);

private slots:
    void onBrowseClicked();
    void onApplyClicked();

private:
    QLineEdit   *m_fileEdit    = nullptr;
    QCheckBox   *m_declickCheck = nullptr;
    QCheckBox   *m_dehumCheck   = nullptr;
    QCheckBox   *m_gateCheck    = nullptr;
    QComboBox   *m_humFreqCombo = nullptr;
    QSlider     *m_nrStrengthSlider = nullptr;
    QDoubleSpinBox *m_nrStrengthSpin = nullptr;
    QPushButton *m_applyBtn     = nullptr;
};
