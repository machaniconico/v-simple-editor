#pragma once

#include <QDialog>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QListWidget>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QStackedWidget>
#include <QColorDialog>
#include "VideoEffect.h"
#include "Keyframe.h"
#include "EffectPlugin.h"

// --- Color Correction Dialog ---

class ColorCorrectionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ColorCorrectionDialog(const ColorCorrection &initial = ColorCorrection{},
                                    QWidget *parent = nullptr);
    ColorCorrection result() const { return m_cc; }

private slots:
    void onSliderChanged();
    void resetAll();

private:
    struct SliderRow {
        QSlider *slider;
        QLabel *valueLabel;
    };

    SliderRow addSlider(QGridLayout *layout, int row, const QString &label,
                        int min, int max, int initial, int scale = 1);

    ColorCorrection m_cc;

    SliderRow m_brightness, m_contrast, m_saturation, m_hue;
    SliderRow m_temperature, m_tint, m_gamma, m_highlights, m_shadows, m_exposure;
};

// --- Video Effect Dialog ---

class VideoEffectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VideoEffectDialog(const QVector<VideoEffect> &initial = {},
                                QWidget *parent = nullptr);
    QVector<VideoEffect> result() const { return m_effects; }

private slots:
    void addEffect();
    void removeEffect();
    void moveUp();
    void moveDown();
    void onEffectSelected(int row);
    void onParamChanged();
    void pickColor();

private:
    void refreshList();
    void updateParamUI(int index);

    QVector<VideoEffect> m_effects;
    QListWidget *m_effectList;
    QComboBox *m_typeCombo;

    // Parameter controls
    QStackedWidget *m_paramStack;
    QDoubleSpinBox *m_param1Spin;
    QDoubleSpinBox *m_param2Spin;
    QDoubleSpinBox *m_param3Spin;
    QLabel *m_param1Label;
    QLabel *m_param2Label;
    QLabel *m_param3Label;
    QPushButton *m_colorButton;
    QCheckBox *m_enabledCheck;
};

// --- Plugin Effect Dialog ---

class PluginEffectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PluginEffectDialog(QWidget *parent = nullptr);

    QString selectedPlugin() const { return m_selectedPlugin; }
    QVector<double> parameterValues() const { return m_paramValues; }

private slots:
    void onPluginSelected(int row);
    void onParamChanged();

private:
    QListWidget *m_pluginList;
    QLabel *m_descLabel;
    QVBoxLayout *m_paramLayout;
    QVector<QDoubleSpinBox*> m_paramSpins;
    QString m_selectedPlugin;
    QVector<double> m_paramValues;
};

// --- Keyframe Editor Dialog ---

class KeyframeDialog : public QDialog
{
    Q_OBJECT

public:
    explicit KeyframeDialog(const KeyframeTrack &track, double clipDuration,
                            QWidget *parent = nullptr);
    KeyframeTrack result() const { return m_track; }

private slots:
    void addKeyframe();
    void removeKeyframe();
    void onSelectionChanged();

private:
    void refreshList();

    KeyframeTrack m_track;
    double m_clipDuration;
    QListWidget *m_kfList;
    QDoubleSpinBox *m_timeSpin;
    QDoubleSpinBox *m_valueSpin;
    QComboBox *m_interpCombo;
};
