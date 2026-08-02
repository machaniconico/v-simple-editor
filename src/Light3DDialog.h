#pragma once

#include "Light3D.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

class Light3DDialog : public QDialog
{
    Q_OBJECT

public:
    explicit Light3DDialog(QWidget *parent = nullptr);

    void setLights(const QVector<Light3D> &lights);
    QVector<Light3D> lights() const { return m_lights; }

    void setMaterial(const LayerMaterial &material);
    LayerMaterial material() const { return m_material; }
    void setMaterialTargetAvailable(bool available);

signals:
    void settingsChanged();

private slots:
    void onCurrentLightChanged(int row);
    void onLightItemChanged(QListWidgetItem *item);
    void onAddLight();
    void onDuplicateLight();
    void onRemoveLight();
    void onLightEdited();
    void onTypeChanged(int index);
    void onColorClicked();
    void onAddKeyframe();
    void onMaterialEdited();

private:
    QVector<Light3D> m_lights;
    LayerMaterial m_material;
    bool m_updating = false;
    bool m_materialTargetAvailable = false;

    QListWidget *m_lightList = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_duplicateButton = nullptr;
    QPushButton *m_removeButton = nullptr;

    QGroupBox *m_lightSettingsGroup = nullptr;
    QGroupBox *m_materialGroup = nullptr;
    QCheckBox *m_enabledCheck = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QDoubleSpinBox *m_position[3] = {};
    QDoubleSpinBox *m_target[3] = {};
    QPushButton *m_colorButton = nullptr;
    QDoubleSpinBox *m_intensitySpin = nullptr;
    QCheckBox *m_falloffCheck = nullptr;
    QDoubleSpinBox *m_falloffRadiusSpin = nullptr;
    QDoubleSpinBox *m_falloffDistanceSpin = nullptr;
    QDoubleSpinBox *m_coneAngleSpin = nullptr;
    QDoubleSpinBox *m_coneFeatherSpin = nullptr;
    QDoubleSpinBox *m_keyframeTimeSpin = nullptr;
    QLabel *m_keyframeSummary = nullptr;

    QCheckBox *m_acceptsLightsCheck = nullptr;
    QDoubleSpinBox *m_ambientCoeffSpin = nullptr;
    QDoubleSpinBox *m_diffuseCoeffSpin = nullptr;
    QDoubleSpinBox *m_specularCoeffSpin = nullptr;
    QDoubleSpinBox *m_shininessSpin = nullptr;

    QDialogButtonBox *m_buttonBox = nullptr;

    void buildUi();
    void rebuildLightList(int selectedRow = -1);
    void refreshInspector();
    void updateTypeSpecificUi();
    void updateColorButton();
    void updateKeyframeSummary();
    void syncCurrentLightFromWidgets();
    Light3DState stateFromWidgets() const;
    void refreshMaterialWidgets();
    void syncMaterialFromWidgets();
    void emitSettingsChanged();

    int currentRow() const;
    QString uniqueLightName(const QString &preferred) const;
};
