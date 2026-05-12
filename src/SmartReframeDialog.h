#pragma once

#include <QDialog>
#include <QSize>

class QComboBox;
class QSpinBox;
class QSlider;
class QCheckBox;
class QGroupBox;
class QDialogButtonBox;
class QLabel;

struct SmartReframeParams {
    double aspectW = 9.0;
    double aspectH = 16.0;
    QSize outputSize = QSize(1080, 1920);
    double smoothness = 0.7;
    double motionWeight = 0.5;
    double paddingPercent = 8.0;
    bool useMotion = true;
};

class SmartReframeDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SmartReframeDialog(QWidget *parent = nullptr);

    SmartReframeParams params() const;

private slots:
    void onAspectPresetChanged(int index);
    void onOutputResolutionChanged(int index);

private:
    void setupUI();

    QComboBox *m_aspectPresetCombo = nullptr;
    QSpinBox *m_customWSpin = nullptr;
    QSpinBox *m_customHSpin = nullptr;
    QComboBox *m_outputResCombo = nullptr;
    QSlider *m_smoothnessSlider = nullptr;
    QSlider *m_motionWeightSlider = nullptr;
    QSpinBox *m_paddingSpin = nullptr;
    QCheckBox *m_useMotionCheck = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
    QLabel *m_smoothnessLabel = nullptr;
    QLabel *m_motionWeightLabel = nullptr;
    QLabel *m_outputResLabel = nullptr;
};
