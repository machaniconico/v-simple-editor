#pragma once

#include "TimecodeBurnIn.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

class TimecodeBurnInDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TimecodeBurnInDialog(
        const TimecodeBurnInSettings &initial,
        QWidget *parent = nullptr);

    TimecodeBurnInSettings settings() const;

private:
    QCheckBox *m_enabledCheck = nullptr;
    QComboBox *m_positionCombo = nullptr;
    QSpinBox *m_fontSizeSpin = nullptr;
    QCheckBox *m_showFramesCheck = nullptr;
    QCheckBox *m_dropFrameCheck = nullptr;
    QLineEdit *m_prefixEdit = nullptr;
    QCheckBox *m_showClipNameCheck = nullptr;
    QCheckBox *m_showDateCheck = nullptr;
    QDoubleSpinBox *m_opacitySpin = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
};
