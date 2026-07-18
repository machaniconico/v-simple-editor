#pragma once

#include <QDialog>

#include "ProjectSettings.h"

class QComboBox;
class QSpinBox;
class QLineEdit;

class SequenceSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SequenceSettingsDialog(const ProjectConfig &initial,
                                    QWidget *parent = nullptr);

    ProjectConfig config() const { return m_config; }

private slots:
    void onPresetChanged(int index);
    void onCustomResolutionChanged();

private:
    void setupUi();
    void selectInitialResolution();
    static int evenDimension(int value);

    ProjectConfig m_config;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_resolutionCombo = nullptr;
    QSpinBox *m_widthSpin = nullptr;
    QSpinBox *m_heightSpin = nullptr;
    QSpinBox *m_fpsSpin = nullptr;
    bool m_updating = false;
};
