#pragma once

#include <QDialog>

#include "LoudnessMaster.h"

class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;

// Modeless dialog: measure a file's integrated LUFS and show the
// loudness-normalization gain needed for the selected delivery preset.
class LoudnessMasterDialog : public QDialog {
    Q_OBJECT
public:
    explicit LoudnessMasterDialog(QWidget *parent = nullptr);

private slots:
    void onBrowseClicked();
    void onMeasureClicked();
    void onPresetChanged(int index);

private:
    QLineEdit   *m_fileEdit      = nullptr;
    QComboBox   *m_presetCombo   = nullptr;
    QLabel      *m_measuredLabel = nullptr;
    QLabel      *m_gainLabel     = nullptr;
    QPushButton *m_measureBtn    = nullptr;

    double m_measuredLufs = 0.0;
    bool   m_hasMeasured  = false;
};
