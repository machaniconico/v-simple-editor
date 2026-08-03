#include "Light3DDialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace {

QDoubleSpinBox *makeSpin(QWidget *parent, double minimum, double maximum,
                          double step, int decimals)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    return spin;
}

QString typeLabel(LightType type)
{
    switch (type) {
    case LightType::Ambient:  return QStringLiteral("Ambient / 環境光");
    case LightType::Parallel: return QStringLiteral("Parallel / 平行光");
    case LightType::Point:    return QStringLiteral("Point / 点光源");
    case LightType::Spot:     return QStringLiteral("Spot / スポット");
    }
    return QStringLiteral("Point / 点光源");
}

} // namespace

Light3DDialog::Light3DDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("3D ライト"));
    setModal(false);
    resize(900, 680);
    buildUi();
    refreshInspector();
    refreshMaterialWidgets();
}

void Light3DDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto *listPanel = new QWidget(splitter);
    listPanel->setMinimumWidth(190);
    auto *listLayout = new QVBoxLayout(listPanel);
    listLayout->setContentsMargins(0, 0, 8, 0);
    listLayout->addWidget(new QLabel(QStringLiteral("ライト一覧"), listPanel));

    m_lightList = new QListWidget(listPanel);
    m_lightList->setObjectName(QStringLiteral("light3dList"));
    m_lightList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_lightList->setEditTriggers(QAbstractItemView::DoubleClicked
                                 | QAbstractItemView::EditKeyPressed);
    m_lightList->setAlternatingRowColors(true);
    listLayout->addWidget(m_lightList, 1);

    auto *listButtons = new QHBoxLayout;
    m_addButton = new QPushButton(QStringLiteral("追加"), listPanel);
    m_duplicateButton = new QPushButton(QStringLiteral("複製"), listPanel);
    m_removeButton = new QPushButton(QStringLiteral("削除"), listPanel);
    listButtons->addWidget(m_addButton);
    listButtons->addWidget(m_duplicateButton);
    listButtons->addWidget(m_removeButton);
    listLayout->addLayout(listButtons);
    splitter->addWidget(listPanel);

    auto *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *inspector = new QWidget(scroll);
    auto *inspectorLayout = new QVBoxLayout(inspector);
    inspectorLayout->setContentsMargins(4, 0, 0, 0);
    inspectorLayout->setSpacing(8);

    m_lightSettingsGroup = new QGroupBox(QStringLiteral("選択ライト"), inspector);
    auto *lightForm = new QFormLayout(m_lightSettingsGroup);
    m_enabledCheck = new QCheckBox(QStringLiteral("有効"), m_lightSettingsGroup);
    lightForm->addRow(QString(), m_enabledCheck);

    m_typeCombo = new QComboBox(m_lightSettingsGroup);
    const LightType types[] = {LightType::Ambient, LightType::Parallel,
                               LightType::Point, LightType::Spot};
    for (LightType type : types)
        m_typeCombo->addItem(typeLabel(type), static_cast<int>(type));
    lightForm->addRow(QStringLiteral("種別"), m_typeCombo);

    auto makeVectorRow = [this](QDoubleSpinBox *spins[3], QWidget *parent) {
        auto *row = new QHBoxLayout;
        const QString labels[] = {QStringLiteral("X"), QStringLiteral("Y"),
                                  QStringLiteral("Z")};
        for (int i = 0; i < 3; ++i) {
            spins[i] = makeSpin(parent, -100000.0, 100000.0, 1.0, 3);
            row->addWidget(new QLabel(labels[i], parent));
            row->addWidget(spins[i], 1);
        }
        return row;
    };
    lightForm->addRow(QStringLiteral("位置"), makeVectorRow(m_position, m_lightSettingsGroup));
    lightForm->addRow(QStringLiteral("ターゲット"), makeVectorRow(m_target, m_lightSettingsGroup));

    m_colorButton = new QPushButton(m_lightSettingsGroup);
    m_colorButton->setText(QStringLiteral("色を選択"));
    m_colorButton->setMinimumWidth(130);
    lightForm->addRow(QStringLiteral("色"), m_colorButton);

    m_intensitySpin = makeSpin(m_lightSettingsGroup, 0.0, 100.0, 0.05, 3);
    m_intensitySpin->setSuffix(QStringLiteral(" 係数"));
    lightForm->addRow(QStringLiteral("強度"), m_intensitySpin);

    auto *falloffGroup = new QGroupBox(QStringLiteral("減衰（Point / Spot）"), m_lightSettingsGroup);
    auto *falloffForm = new QFormLayout(falloffGroup);
    m_falloffCheck = new QCheckBox(QStringLiteral("減衰を有効化"), falloffGroup);
    falloffForm->addRow(QString(), m_falloffCheck);
    m_falloffRadiusSpin = makeSpin(falloffGroup, 0.0, 100000.0, 1.0, 2);
    m_falloffDistanceSpin = makeSpin(falloffGroup, 0.0, 100000.0, 1.0, 2);
    falloffForm->addRow(QStringLiteral("非減衰半径"), m_falloffRadiusSpin);
    falloffForm->addRow(QStringLiteral("ゼロまでの距離"), m_falloffDistanceSpin);
    lightForm->addRow(falloffGroup);

    auto *spotGroup = new QGroupBox(QStringLiteral("スポットコーン"), m_lightSettingsGroup);
    auto *spotForm = new QFormLayout(spotGroup);
    m_coneAngleSpin = makeSpin(spotGroup, 0.1, 179.9, 1.0, 2);
    m_coneAngleSpin->setSuffix(QStringLiteral(" °"));
    m_coneFeatherSpin = makeSpin(spotGroup, 0.0, 100.0, 1.0, 1);
    m_coneFeatherSpin->setSuffix(QStringLiteral(" %"));
    spotForm->addRow(QStringLiteral("コーン角（全角）"), m_coneAngleSpin);
    spotForm->addRow(QStringLiteral("フェザー"), m_coneFeatherSpin);
    lightForm->addRow(spotGroup);
    inspectorLayout->addWidget(m_lightSettingsGroup);

    auto *keyframeGroup = new QGroupBox(QStringLiteral("ライトキーフレーム"), inspector);
    auto *keyframeLayout = new QHBoxLayout(keyframeGroup);
    keyframeLayout->addWidget(new QLabel(QStringLiteral("時刻"), keyframeGroup));
    m_keyframeTimeSpin = makeSpin(keyframeGroup, 0.0, 1000000.0, 0.1, 3);
    m_keyframeTimeSpin->setSuffix(QStringLiteral(" s"));
    keyframeLayout->addWidget(m_keyframeTimeSpin);
    auto *addKeyframeButton = new QPushButton(QStringLiteral("現在値を追加"), keyframeGroup);
    keyframeLayout->addWidget(addKeyframeButton);
    m_keyframeSummary = new QLabel(keyframeGroup);
    m_keyframeSummary->setMinimumWidth(120);
    keyframeLayout->addWidget(m_keyframeSummary);
    keyframeLayout->addStretch(1);
    inspectorLayout->addWidget(keyframeGroup);

    m_materialGroup = new QGroupBox(QStringLiteral("選択クリップのマテリアル"), inspector);
    auto *materialForm = new QFormLayout(m_materialGroup);
    m_acceptsLightsCheck = new QCheckBox(QStringLiteral("ライトを受ける"), m_materialGroup);
    materialForm->addRow(QString(), m_acceptsLightsCheck);
    auto makeCoeff = [this](QWidget *parent) {
        return makeSpin(parent, 0.0, 1.0, 0.05, 3);
    };
    m_ambientCoeffSpin = makeCoeff(m_materialGroup);
    m_diffuseCoeffSpin = makeCoeff(m_materialGroup);
    m_specularCoeffSpin = makeCoeff(m_materialGroup);
    m_shininessSpin = makeSpin(m_materialGroup, 1.0, 512.0, 1.0, 2);
    m_ambientCoeffSpin->setObjectName(QStringLiteral("light3dAmbientCoeff"));
    m_diffuseCoeffSpin->setObjectName(QStringLiteral("light3dDiffuseCoeff"));
    m_specularCoeffSpin->setObjectName(QStringLiteral("light3dSpecularCoeff"));
    m_shininessSpin->setObjectName(QStringLiteral("light3dShininess"));
    materialForm->addRow(QStringLiteral("Ambient 係数"), m_ambientCoeffSpin);
    materialForm->addRow(QStringLiteral("Diffuse 係数"), m_diffuseCoeffSpin);
    materialForm->addRow(QStringLiteral("Specular 係数"), m_specularCoeffSpin);
    materialForm->addRow(QStringLiteral("Shininess"), m_shininessSpin);
    inspectorLayout->addWidget(m_materialGroup);
    inspectorLayout->addStretch(1);
    scroll->setWidget(inspector);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(m_buttonBox);

    connect(m_lightList, &QListWidget::currentRowChanged,
            this, &Light3DDialog::onCurrentLightChanged);
    connect(m_lightList, &QListWidget::itemChanged,
            this, &Light3DDialog::onLightItemChanged);
    connect(m_addButton, &QPushButton::clicked, this, &Light3DDialog::onAddLight);
    connect(m_duplicateButton, &QPushButton::clicked,
            this, &Light3DDialog::onDuplicateLight);
    connect(m_removeButton, &QPushButton::clicked,
            this, &Light3DDialog::onRemoveLight);
    connect(m_enabledCheck, &QCheckBox::toggled,
            this, &Light3DDialog::onLightEdited);
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Light3DDialog::onTypeChanged);
    connect(m_colorButton, &QPushButton::clicked,
            this, &Light3DDialog::onColorClicked);
    connect(m_falloffCheck, &QCheckBox::toggled,
            this, &Light3DDialog::onLightEdited);
    connect(addKeyframeButton, &QPushButton::clicked,
            this, &Light3DDialog::onAddKeyframe);
    connect(m_acceptsLightsCheck, &QCheckBox::toggled,
            this, &Light3DDialog::onMaterialEdited);
    connect(m_buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    auto connectLightSpin = [this](QDoubleSpinBox *spin) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &Light3DDialog::onLightEdited);
    };
    for (QDoubleSpinBox *spin : m_position)
        connectLightSpin(spin);
    for (QDoubleSpinBox *spin : m_target)
        connectLightSpin(spin);
    connectLightSpin(m_intensitySpin);
    connectLightSpin(m_falloffRadiusSpin);
    connectLightSpin(m_falloffDistanceSpin);
    connectLightSpin(m_coneAngleSpin);
    connectLightSpin(m_coneFeatherSpin);

    auto connectMaterialSpin = [this](QDoubleSpinBox *spin) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &Light3DDialog::onMaterialEdited);
    };
    connectMaterialSpin(m_ambientCoeffSpin);
    connectMaterialSpin(m_diffuseCoeffSpin);
    connectMaterialSpin(m_specularCoeffSpin);
    connectMaterialSpin(m_shininessSpin);
}

void Light3DDialog::setLights(const QVector<Light3D> &lights)
{
    m_lights = lights;
    rebuildLightList(m_lights.isEmpty() ? -1 : 0);
}

void Light3DDialog::setMaterial(const LayerMaterial &material)
{
    m_material = material;
    refreshMaterialWidgets();
}

void Light3DDialog::setMaterialTargetAvailable(bool available)
{
    m_materialTargetAvailable = available;
    if (m_materialGroup)
        m_materialGroup->setEnabled(available);
}

int Light3DDialog::currentRow() const
{
    return m_lightList ? m_lightList->currentRow() : -1;
}

QString Light3DDialog::uniqueLightName(const QString &preferred) const
{
    QString candidate = preferred;
    int suffix = 2;
    auto containsName = [this](const QString &name) {
        for (const Light3D &light : m_lights) {
            if (light.state().name == name)
                return true;
        }
        return false;
    };
    while (containsName(candidate))
        candidate = QStringLiteral("%1 %2").arg(preferred).arg(suffix++);
    return candidate;
}

void Light3DDialog::rebuildLightList(int selectedRow)
{
    const bool previousUpdating = m_updating;
    m_updating = true;
    const QSignalBlocker listSignals(m_lightList);
    m_lightList->clear();
    for (const Light3D &light : m_lights) {
        const Light3DState state = light.state();
        auto *item = new QListWidgetItem(state.name, m_lightList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        item->setCheckState(state.enabled ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(lightTypeName(state.type));
    }
    if (!m_lights.isEmpty())
        m_lightList->setCurrentRow(qBound(0, selectedRow, m_lights.size() - 1));
    else
        m_lightList->setCurrentRow(-1);
    refreshInspector();
    m_updating = previousUpdating;
}

void Light3DDialog::refreshInspector()
{
    const bool previousUpdating = m_updating;
    m_updating = true;
    const int row = currentRow();
    const bool hasLight = row >= 0 && row < m_lights.size();
    m_lightSettingsGroup->setEnabled(hasLight);
    m_duplicateButton->setEnabled(hasLight);
    m_removeButton->setEnabled(hasLight);
    if (hasLight) {
        const Light3DState state = m_lights.at(row).state();
        m_enabledCheck->setChecked(state.enabled);
        m_typeCombo->setCurrentIndex(m_typeCombo->findData(static_cast<int>(state.type)));
        m_position[0]->setValue(state.position.x());
        m_position[1]->setValue(state.position.y());
        m_position[2]->setValue(state.position.z());
        m_target[0]->setValue(state.target.x());
        m_target[1]->setValue(state.target.y());
        m_target[2]->setValue(state.target.z());
        m_colorButton->setProperty("lightColor", state.color);
        m_intensitySpin->setValue(state.intensity);
        m_falloffCheck->setChecked(state.falloffEnabled);
        m_falloffRadiusSpin->setValue(state.falloffRadius);
        m_falloffDistanceSpin->setValue(state.falloffDistance);
        m_coneAngleSpin->setValue(state.coneAngle);
        m_coneFeatherSpin->setValue(state.coneFeather);
    } else {
        m_enabledCheck->setChecked(false);
        m_colorButton->setProperty("lightColor", QColor(Qt::white));
    }
    m_updating = previousUpdating;
    updateColorButton();
    updateTypeSpecificUi();
    updateKeyframeSummary();
}

void Light3DDialog::updateTypeSpecificUi()
{
    const int row = currentRow();
    const bool hasLight = row >= 0 && row < m_lights.size();
    const LightType type = hasLight
        ? m_lights.at(row).state().type : LightType::Point;
    const bool hasDirection = type == LightType::Parallel || type == LightType::Spot;
    const bool hasPosition = type != LightType::Ambient;
    const bool hasFalloff = type == LightType::Point || type == LightType::Spot;
    const bool hasCone = type == LightType::Spot;
    for (QDoubleSpinBox *spin : m_position)
        spin->setEnabled(hasLight && hasPosition);
    for (QDoubleSpinBox *spin : m_target)
        spin->setEnabled(hasLight && hasDirection);
    m_falloffCheck->setEnabled(hasLight && hasFalloff);
    m_falloffRadiusSpin->setEnabled(hasLight && hasFalloff);
    m_falloffDistanceSpin->setEnabled(hasLight && hasFalloff);
    m_coneAngleSpin->setEnabled(hasLight && hasCone);
    m_coneFeatherSpin->setEnabled(hasLight && hasCone);
}

void Light3DDialog::updateColorButton()
{
    const QColor color = m_colorButton->property("lightColor").value<QColor>();
    const QColor safeColor = color.isValid() ? color : QColor(Qt::white);
    m_colorButton->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: 1px solid #707780; padding: 4px 12px; }")
        .arg(safeColor.name(), safeColor.lightnessF() > 0.55 ? QStringLiteral("#111111")
                                                             : QStringLiteral("#ffffff")));
}

void Light3DDialog::updateKeyframeSummary()
{
    const int row = currentRow();
    if (row < 0 || row >= m_lights.size()) {
        m_keyframeSummary->setText(QStringLiteral("ライト未選択"));
        return;
    }
    m_keyframeSummary->setText(QStringLiteral("%1 キーフレーム")
                                   .arg(m_lights.at(row).allKeyframeTimes().size()));
}

Light3DState Light3DDialog::stateFromWidgets() const
{
    Light3DState state;
    const int row = currentRow();
    if (row >= 0 && row < m_lights.size())
        state = m_lights.at(row).state();
    state.enabled = m_enabledCheck->isChecked();
    state.type = static_cast<LightType>(m_typeCombo->currentData().toInt());
    state.position = QVector3D(
        static_cast<float>(m_position[0]->value()),
        static_cast<float>(m_position[1]->value()),
        static_cast<float>(m_position[2]->value()));
    state.target = QVector3D(
        static_cast<float>(m_target[0]->value()),
        static_cast<float>(m_target[1]->value()),
        static_cast<float>(m_target[2]->value()));
    const QColor selectedColor = m_colorButton->property("lightColor").value<QColor>();
    if (selectedColor.isValid())
        state.color = selectedColor;
    state.intensity = m_intensitySpin->value();
    state.falloffEnabled = m_falloffCheck->isChecked();
    state.falloffRadius = m_falloffRadiusSpin->value();
    state.falloffDistance = m_falloffDistanceSpin->value();
    state.coneAngle = m_coneAngleSpin->value();
    state.coneFeather = m_coneFeatherSpin->value();
    return state;
}

void Light3DDialog::syncCurrentLightFromWidgets()
{
    if (m_updating)
        return;
    const int row = currentRow();
    if (row < 0 || row >= m_lights.size())
        return;
    const Light3DState state = stateFromWidgets();
    m_lights[row].setStateAt(m_keyframeTimeSpin->value(), state);
    m_updating = true;
    if (auto *item = m_lightList->item(row)) {
        item->setText(state.name);
        item->setCheckState(state.enabled ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(lightTypeName(state.type));
    }
    m_updating = false;
    updateTypeSpecificUi();
    updateKeyframeSummary();
}

void Light3DDialog::refreshMaterialWidgets()
{
    if (!m_acceptsLightsCheck)
        return;
    const bool previousUpdating = m_updating;
    m_updating = true;
    m_acceptsLightsCheck->setChecked(m_material.acceptsLights);
    m_ambientCoeffSpin->setValue(m_material.ambientCoeff);
    m_diffuseCoeffSpin->setValue(m_material.diffuseCoeff);
    m_specularCoeffSpin->setValue(m_material.specularCoeff);
    m_shininessSpin->setValue(m_material.shininess);
    m_updating = previousUpdating;
    m_materialGroup->setEnabled(m_materialTargetAvailable);
}

void Light3DDialog::syncMaterialFromWidgets()
{
    if (m_updating)
        return;
    m_material.acceptsLights = m_acceptsLightsCheck->isChecked();
    m_material.ambientCoeff = m_ambientCoeffSpin->value();
    m_material.diffuseCoeff = m_diffuseCoeffSpin->value();
    m_material.specularCoeff = m_specularCoeffSpin->value();
    m_material.shininess = m_shininessSpin->value();
}

void Light3DDialog::emitSettingsChanged()
{
    if (!m_updating)
        emit settingsChanged();
}

void Light3DDialog::onCurrentLightChanged(int)
{
    refreshInspector();
}

void Light3DDialog::onLightItemChanged(QListWidgetItem *item)
{
    if (m_updating || !item)
        return;
    const int row = m_lightList->row(item);
    if (row < 0 || row >= m_lights.size())
        return;
    Light3DState state = m_lights.at(row).state();
    state.name = item->text().trimmed().isEmpty()
        ? uniqueLightName(QStringLiteral("Light")) : item->text().trimmed();
    state.enabled = item->checkState() == Qt::Checked;
    m_lights[row].setState(state);
    refreshInspector();
    emitSettingsChanged();
}

void Light3DDialog::onAddLight()
{
    Light3DState state;
    state.name = uniqueLightName(QStringLiteral("Light 1"));
    m_lights.append(Light3D(state));
    rebuildLightList(m_lights.size() - 1);
    emitSettingsChanged();
}

void Light3DDialog::onDuplicateLight()
{
    const int row = currentRow();
    if (row < 0 || row >= m_lights.size())
        return;
    Light3D copy = m_lights.at(row);
    Light3DState state = copy.state();
    state.name = uniqueLightName(state.name + QStringLiteral(" copy"));
    copy.setState(state);
    m_lights.insert(row + 1, copy);
    rebuildLightList(row + 1);
    emitSettingsChanged();
}

void Light3DDialog::onRemoveLight()
{
    const int row = currentRow();
    if (row < 0 || row >= m_lights.size())
        return;
    m_lights.removeAt(row);
    rebuildLightList(row < m_lights.size() ? row : m_lights.size() - 1);
    emitSettingsChanged();
}

void Light3DDialog::onLightEdited()
{
    syncCurrentLightFromWidgets();
    emitSettingsChanged();
}

void Light3DDialog::onTypeChanged(int)
{
    if (m_updating)
        return;
    syncCurrentLightFromWidgets();
    updateTypeSpecificUi();
    emitSettingsChanged();
}

void Light3DDialog::onColorClicked()
{
    const QColor current = m_colorButton->property("lightColor").value<QColor>();
    const QColor selected = QColorDialog::getColor(
        current.isValid() ? current : QColor(Qt::white), this,
        QStringLiteral("ライトの色"), QColorDialog::ShowAlphaChannel);
    if (!selected.isValid())
        return;
    m_colorButton->setProperty("lightColor", selected);
    updateColorButton();
    syncCurrentLightFromWidgets();
    emitSettingsChanged();
}

void Light3DDialog::onAddKeyframe()
{
    const int row = currentRow();
    if (row < 0 || row >= m_lights.size())
        return;
    syncCurrentLightFromWidgets();
    m_lights[row].setKeyframe(m_keyframeTimeSpin->value(),
                              m_lights[row].state());
    updateKeyframeSummary();
    emitSettingsChanged();
}

void Light3DDialog::onMaterialEdited()
{
    syncMaterialFromWidgets();
    emitSettingsChanged();
}
