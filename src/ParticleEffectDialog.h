#pragma once

#include <QDialog>
#include <QColor>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>
#include "ParticleSystem.h"

class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QDialogButtonBox;
class QScrollArea;
class QWidget;
class QTimer;
class QHBoxLayout;
class QVBoxLayout;

class ParticleEffectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ParticleEffectDialog(QWidget *parent = nullptr);

    void setConfig(const ParticleEmitterConfig &config);
    ParticleEmitterConfig config() const;

private:
    struct ForceRowWidgets {
        QHBoxLayout *layout = nullptr;
        QComboBox *kindCombo = nullptr;
        QDoubleSpinBox *posX = nullptr;
        QDoubleSpinBox *posY = nullptr;
        QDoubleSpinBox *strength = nullptr;
        QDoubleSpinBox *radius = nullptr;
        QPushButton *removeBtn = nullptr;
    };

    void setupUI();
    void setupPresetRow(QVBoxLayout *mainLayout);
    void setupEmissionGroup(QVBoxLayout *mainLayout);
    void setupLifetimeGroup(QVBoxLayout *mainLayout);
    void setupSizeGroup(QVBoxLayout *mainLayout);
    void setupMotionGroup(QVBoxLayout *mainLayout);
    void setupColorGroup(QVBoxLayout *mainLayout);
    void setupOpacityGroup(QVBoxLayout *mainLayout);
    void setupForcesGroup(QVBoxLayout *mainLayout);
    void setupCollisionGroup(QVBoxLayout *mainLayout);
    void setupTurbulenceGroup(QVBoxLayout *mainLayout);
    void setupPreview(QVBoxLayout *mainLayout);
    void setupButtons(QVBoxLayout *mainLayout);

    void refreshPreview();
    void onParamChanged();
    void updateColorButton(QPushButton *btn, const QColor &color);
    void addForceRow(const ForceField &ff = ForceField());
    void removeForceRow(const ForceRowWidgets &row);
    void clearForceRows();

    // Preset
    QComboBox *m_presetCombo = nullptr;

    // Emission
    QComboBox *m_typeCombo = nullptr;
    QDoubleSpinBox *m_emitRateSpin = nullptr;
    QSpinBox *m_maxParticlesSpin = nullptr;
    QDoubleSpinBox *m_emitPosX = nullptr;
    QDoubleSpinBox *m_emitPosY = nullptr;
    QDoubleSpinBox *m_emitAreaW = nullptr;
    QDoubleSpinBox *m_emitAreaH = nullptr;

    // Lifetime
    QDoubleSpinBox *m_lifeMinSpin = nullptr;
    QDoubleSpinBox *m_lifeMaxSpin = nullptr;

    // Size
    QDoubleSpinBox *m_sizeMinSpin = nullptr;
    QDoubleSpinBox *m_sizeMaxSpin = nullptr;
    QDoubleSpinBox *m_sizeStartMultSpin = nullptr;
    QDoubleSpinBox *m_sizeEndMultSpin = nullptr;

    // Motion
    QDoubleSpinBox *m_speedMinSpin = nullptr;
    QDoubleSpinBox *m_speedMaxSpin = nullptr;
    QDoubleSpinBox *m_directionSpin = nullptr;
    QDoubleSpinBox *m_spreadSpin = nullptr;
    QDoubleSpinBox *m_gravityX = nullptr;
    QDoubleSpinBox *m_gravityY = nullptr;
    QDoubleSpinBox *m_windX = nullptr;
    QDoubleSpinBox *m_windY = nullptr;

    // Color
    QPushButton *m_startColorBtn = nullptr;
    QPushButton *m_endColorBtn = nullptr;

    // Opacity
    QDoubleSpinBox *m_fadeInSpin = nullptr;
    QDoubleSpinBox *m_fadeOutSpin = nullptr;

    // Forces
    QScrollArea *m_forcesScroll = nullptr;
    QWidget *m_forcesContainer = nullptr;
    QVBoxLayout *m_forcesLayout = nullptr;
    QPushButton *m_addForceBtn = nullptr;

    QVector<ForceRowWidgets> m_forceRows;

    // Collision
    QCheckBox *m_collisionFloorCheck = nullptr;
    QDoubleSpinBox *m_floorYSpin = nullptr;
    QDoubleSpinBox *m_restitutionSpin = nullptr;
    QDoubleSpinBox *m_floorFrictionSpin = nullptr;

    // Turbulence
    QDoubleSpinBox *m_turbulenceAmountSpin = nullptr;
    QDoubleSpinBox *m_turbulenceScaleSpin = nullptr;
    QDoubleSpinBox *m_turbulenceSpeedSpin = nullptr;

    // Preview
    QLabel *m_previewLabel = nullptr;
    QTimer *m_previewDebounce = nullptr;

    // Buttons
    QDialogButtonBox *m_buttonBox = nullptr;

    QColor m_startColor = Qt::white;
    QColor m_endColor = Qt::white;
};
