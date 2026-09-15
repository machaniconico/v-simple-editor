#pragma once

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QDialogButtonBox;

class MusicRemixDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MusicRemixDialog(QWidget *parent = nullptr);

    void setDetectedBpm(double bpm);
    void setTargetDuration(double seconds);
    double targetDuration() const;
    bool rippleFollowingClips() const;

private:
    void updateDurationLabel(double seconds);

    QDoubleSpinBox *m_targetSpin = nullptr;
    QLabel *m_bpmLabel = nullptr;
    QLabel *m_durationLabel = nullptr;
    QCheckBox *m_rippleCheck = nullptr;
};
