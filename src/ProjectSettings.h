#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QButtonGroup>
#include <QRadioButton>
#include <QString>

struct ProjectPreset {
    QString name;
    int width;
    int height;
    int fps;
};

struct ProjectConfig {
    QString name = "Untitled";
    int width = 1920;
    int height = 1080;
    int fps = 30;

    double aspectRatio() const { return static_cast<double>(width) / height; }
    QString resolutionLabel() const { return QString("%1x%2").arg(width).arg(height); }
    QString fpsLabel() const { return QString("%1fps").arg(fps); }
};

class ProjectSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProjectSettingsDialog(QWidget *parent = nullptr);

    ProjectConfig config() const { return m_config; }

    static QVector<ProjectPreset> presets();

private slots:
    void onPresetChanged(int index);
    void onCustomResolutionChanged();

private:
    void setupUI();

    ProjectConfig m_config;
    QLineEdit *m_nameEdit;
    QComboBox *m_presetCombo;
    QSpinBox *m_widthSpin;
    QSpinBox *m_heightSpin;
    QComboBox *m_fpsCombo;
};
