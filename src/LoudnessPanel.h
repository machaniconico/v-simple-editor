#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;
class QProgressBar;

// LoudnessPanel — dock panel showing EBU R128 loudness readouts and
// delivery-target normalization UI. Does NOT touch AudioMixer directly;
// the owner (MainWindow) wires normalizeRequested() to the mixer's master
// loudness normalizer.
class LoudnessPanel : public QWidget {
    Q_OBJECT
public:
    explicit LoudnessPanel(QWidget *parent = nullptr);

    // Update the four readout labels from a measurement pass.
    void setMeasurement(double integratedLUFS, double momentaryLUFS,
                        double shortTermLUFS, double truePeakDBTP);

    // Return the LUFS target for the currently selected delivery preset,
    // or the custom spin value when 'Custom' is active.
    double selectedTargetLUFS() const;

signals:
    // Emitted once per click of the normalize button. gainDb ==
    // selectedTargetLUFS() - lastIntegratedLUFS. If no measurement has
    // been set yet the signal is NOT emitted and a hint label is shown.
    void normalizeRequested(double targetLUFS, double gainDb);

private slots:
    void onNormalizeClicked();
    void onDeliveryTargetChanged(int index);

private:
    void buildUi();
    void updateGauge();
    void updateHint();

    bool m_hasMeasurement = false;
    double m_integratedLUFS = 0.0;

    QLabel *m_integratedLabel = nullptr;
    QLabel *m_momentaryLabel = nullptr;
    QLabel *m_shortTermLabel = nullptr;
    QLabel *m_truePeakLabel = nullptr;

    QComboBox *m_deliveryTarget = nullptr;
    QDoubleSpinBox *m_customSpin = nullptr;

    QPushButton *m_normalizeBtn = nullptr;

    QProgressBar *m_gauge = nullptr;
    QLabel *m_hintLabel = nullptr;
};
