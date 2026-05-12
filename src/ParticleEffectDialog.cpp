#include "ParticleEffectDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QColorDialog>
#include <QTimer>
#include <QPainter>

ParticleEffectDialog::ParticleEffectDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Particle Effect"));
    setModal(true);
    setMinimumSize(420, 700);

    setupUI();
}

void ParticleEffectDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    setupPresetRow(mainLayout);
    setupEmissionGroup(mainLayout);
    setupLifetimeGroup(mainLayout);
    setupSizeGroup(mainLayout);
    setupMotionGroup(mainLayout);
    setupColorGroup(mainLayout);
    setupOpacityGroup(mainLayout);
    setupForcesGroup(mainLayout);
    setupCollisionGroup(mainLayout);
    setupTurbulenceGroup(mainLayout);
    setupPreview(mainLayout);
    setupButtons(mainLayout);
}

// --- Preset row ---

void ParticleEffectDialog::setupPresetRow(QVBoxLayout *mainLayout)
{
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Preset:"), this));
    m_presetCombo = new QComboBox(this);
    auto presets = ParticleSystem::presetConfigs();
    for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
        m_presetCombo->addItem(it.key());
    }
    row->addWidget(m_presetCombo, 1);
    mainLayout->addLayout(row);

    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0) return;
        QString name = m_presetCombo->itemText(index);
        auto presets = ParticleSystem::presetConfigs();
        if (presets.contains(name)) {
            setConfig(presets[name]);
        }
    });
}

// --- Emission ---

void ParticleEffectDialog::setupEmissionGroup(QVBoxLayout *mainLayout)
{
    auto *group = new QGroupBox(tr("Emission"), this);
    auto *form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_typeCombo = new QComboBox(this);
    m_typeCombo->addItem(tr("Snow"), static_cast<int>(ParticleType::Snow));
    m_typeCombo->addItem(tr("Rain"), static_cast<int>(ParticleType::Rain));
    m_typeCombo->addItem(tr("Spark"), static_cast<int>(ParticleType::Spark));
    m_typeCombo->addItem(tr("Smoke"), static_cast<int>(ParticleType::Smoke));
    m_typeCombo->addItem(tr("Fire"), static_cast<int>(ParticleType::Fire));
    m_typeCombo->addItem(tr("Confetti"), static_cast<int>(ParticleType::Confetti));
    m_typeCombo->addItem(tr("Dust"), static_cast<int>(ParticleType::Dust));
    m_typeCombo->addItem(tr("Bubble"), static_cast<int>(ParticleType::Bubble));
    m_typeCombo->addItem(tr("Star"), static_cast<int>(ParticleType::Star));
    m_typeCombo->addItem(tr("Custom"), static_cast<int>(ParticleType::Custom));
    form->addRow(tr("Type:"), m_typeCombo);

    m_emitRateSpin = new QDoubleSpinBox(this);
    m_emitRateSpin->setRange(0.0, 10000.0);
    m_emitRateSpin->setSingleStep(1.0);
    m_emitRateSpin->setDecimals(1);
    form->addRow(tr("Emit Rate:"), m_emitRateSpin);

    m_maxParticlesSpin = new QSpinBox(this);
    m_maxParticlesSpin->setRange(1, 100000);
    m_maxParticlesSpin->setSingleStep(50);
    form->addRow(tr("Max Particles:"), m_maxParticlesSpin);

    auto *posLayout = new QHBoxLayout;
    m_emitPosX = new QDoubleSpinBox(this);
    m_emitPosX->setRange(-1.0, 2.0);
    m_emitPosX->setSingleStep(0.05);
    m_emitPosX->setDecimals(3);
    m_emitPosY = new QDoubleSpinBox(this);
    m_emitPosY->setRange(-1.0, 2.0);
    m_emitPosY->setSingleStep(0.05);
    m_emitPosY->setDecimals(3);
    posLayout->addWidget(new QLabel(tr("X:"), this));
    posLayout->addWidget(m_emitPosX);
    posLayout->addWidget(new QLabel(tr("Y:"), this));
    posLayout->addWidget(m_emitPosY);
    form->addRow(tr("Position:"), posLayout);

    auto *areaLayout = new QHBoxLayout;
    m_emitAreaW = new QDoubleSpinBox(this);
    m_emitAreaW->setRange(0.0, 2.0);
    m_emitAreaW->setSingleStep(0.05);
    m_emitAreaW->setDecimals(3);
    m_emitAreaH = new QDoubleSpinBox(this);
    m_emitAreaH->setRange(0.0, 2.0);
    m_emitAreaH->setSingleStep(0.05);
    m_emitAreaH->setDecimals(3);
    areaLayout->addWidget(new QLabel(tr("W:"), this));
    areaLayout->addWidget(m_emitAreaW);
    areaLayout->addWidget(new QLabel(tr("H:"), this));
    areaLayout->addWidget(m_emitAreaH);
    form->addRow(tr("Area:"), areaLayout);

    mainLayout->addWidget(group);
}

// --- Lifetime ---

void ParticleEffectDialog::setupLifetimeGroup(QVBoxLayout *mainLayout)
{
    auto *group = new QGroupBox(tr("Lifetime"), this);
    auto *form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_lifeMinSpin = new QDoubleSpinBox(this);
    m_lifeMinSpin->setRange(0.01, 120.0);
    m_lifeMinSpin->setSingleStep(0.1);
    m_lifeMinSpin->setSuffix(" s");
    form->addRow(tr("Min:"), m_lifeMinSpin);

    m_lifeMaxSpin = new QDoubleSpinBox(this);
    m_lifeMaxSpin->setRange(0.01, 120.0);
    m_lifeMaxSpin->setSingleStep(0.1);
    m_lifeMaxSpin->setSuffix(" s");
    form->addRow(tr("Max:"), m_lifeMaxSpin);

    mainLayout->addWidget(group);
}

// --- Size ---

void ParticleEffectDialog::setupSizeGroup(QVBoxLayout *mainLayout)
{
    auto *group = new QGroupBox(tr("Size"), this);
    auto *form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_sizeMinSpin = new QDoubleSpinBox(this);
    m_sizeMinSpin->setRange(0.1, 200.0);
    m_sizeMinSpin->setSingleStep(0.5);
    m_sizeMinSpin->setSuffix(" px");
    form->addRow(tr("Min:"), m_sizeMinSpin);

    m_sizeMaxSpin = new QDoubleSpinBox(this);
    m_sizeMaxSpin->setRange(0.1, 200.0);
    m_sizeMaxSpin->setSingleStep(0.5);
    m_sizeMaxSpin->setSuffix(" px");
    form->addRow(tr("Max:"), m_sizeMaxSpin);

    m_sizeStartMultSpin = new QDoubleSpinBox(this);
    m_sizeStartMultSpin->setRange(0.0, 10.0);
    m_sizeStartMultSpin->setSingleStep(0.1);
    form->addRow(tr("Start Mult:"), m_sizeStartMultSpin);

    m_sizeEndMultSpin = new QDoubleSpinBox(this);
    m_sizeEndMultSpin->setRange(0.0, 10.0);
    m_sizeEndMultSpin->setSingleStep(0.1);
    form->addRow(tr("End Mult:"), m_sizeEndMultSpin);

    mainLayout->addWidget(group);
}

// --- Motion ---

void ParticleEffectDialog::setupMotionGroup(QVBoxLayout *mainLayout)
{
    auto *group = new QGroupBox(tr("Motion"), this);
    auto *form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_speedMinSpin = new QDoubleSpinBox(this);
    m_speedMinSpin->setRange(0.0, 5000.0);
    m_speedMinSpin->setSingleStep(5.0);
    m_speedMinSpin->setSuffix(" px/s");
    form->addRow(tr("Speed Min:"), m_speedMinSpin);

    m_speedMaxSpin = new QDoubleSpinBox(this);
    m_speedMaxSpin->setRange(0.0, 5000.0);
    m_speedMaxSpin->setSingleStep(5.0);
    m_speedMaxSpin->setSuffix(" px/s");
    form->addRow(tr("Speed Max:"), m_speedMaxSpin);

    m_directionSpin = new QDoubleSpinBox(this);
    m_directionSpin->setRange(0.0, 360.0);
    m_directionSpin->setSingleStep(5.0);
    m_directionSpin->setSuffix(" deg");
    form->addRow(tr("Direction:"), m_directionSpin);

    m_spreadSpin = new QDoubleSpinBox(this);
    m_spreadSpin->setRange(0.0, 360.0);
    m_spreadSpin->setSingleStep(5.0);
    m_spreadSpin->setSuffix(" deg");
    form->addRow(tr("Spread:"), m_spreadSpin);

    auto *gravLayout = new QHBoxLayout;
    m_gravityX = new QDoubleSpinBox(this);
    m_gravityX->setRange(-5000.0, 5000.0);
    m_gravityX->setSingleStep(10.0);
    m_gravityY = new QDoubleSpinBox(this);
    m_gravityY->setRange(-5000.0, 5000.0);
    m_gravityY->setSingleStep(10.0);
    gravLayout->addWidget(new QLabel(tr("X:"), this));
    gravLayout->addWidget(m_gravityX);
    gravLayout->addWidget(new QLabel(tr("Y:"), this));
    gravLayout->addWidget(m_gravityY);
    form->addRow(tr("Gravity:"), gravLayout);

    auto *windLayout = new QHBoxLayout;
    m_windX = new QDoubleSpinBox(this);
    m_windX->setRange(-5000.0, 5000.0);
    m_windX->setSingleStep(10.0);
    m_windY = new QDoubleSpinBox(this);
    m_windY->setRange(-5000.0, 5000.0);
    m_windY->setSingleStep(10.0);
    windLayout->addWidget(new QLabel(tr("X:"), this));
    windLayout->addWidget(m_windX);
    windLayout->addWidget(new QLabel(tr("Y:"), this));
    windLayout->addWidget(m_windY);
    form->addRow(tr("Wind:"), windLayout);

    mainLayout->addWidget(group);
}

// --- Color ---

void ParticleEffectDialog::setupColorGroup(QVBoxLayout *mainLayout)
{
    auto *group = new QGroupBox(tr("Color"), this);
    auto *form = new QFormLayout(group);

    m_startColorBtn = new QPushButton(this);
    m_startColorBtn->setMinimumSize(64, 24);
    m_startColorBtn->setMaximumSize(96, 32);
    connect(m_startColorBtn, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_startColor, this, tr("Start Color"), QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            m_startColor = c;
            updateColorButton(m_startColorBtn, m_startColor);
            onParamChanged();
        }
    });

    m_endColorBtn = new QPushButton(this);
    m_endColorBtn->setMinimumSize(64, 24);
    m_endColorBtn->setMaximumSize(96, 32);
    connect(m_endColorBtn, &QPushButton::clicked, this, [this]() {
        QColor c = QColorDialog::getColor(m_endColor, this, tr("End Color"), QColorDialog::ShowAlphaChannel);
        if (c.isValid()) {
            m_endColor = c;
            updateColorButton(m_endColorBtn, m_endColor);
            onParamChanged();
        }
    });

    form->addRow(tr("Start:"), m_startColorBtn);
    form->addRow(tr("End:"), m_endColorBtn);

    mainLayout->addWidget(group);
}

void ParticleEffectDialog::updateColorButton(QPushButton *btn, const QColor &color)
{
    QString style = QString("background-color: rgba(%1,%2,%3,%4); border: 1px solid #555; border-radius: 3px;")
        .arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
    btn->setStyleSheet(style);
    btn->setText(color.name(QColor::HexArgb));
}

// --- Opacity ---

void ParticleEffectDialog::setupOpacityGroup(QVBoxLayout *mainLayout)
{
    auto *group = new QGroupBox(tr("Opacity"), this);
    auto *form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_fadeInSpin = new QDoubleSpinBox(this);
    m_fadeInSpin->setRange(0.0, 1.0);
    m_fadeInSpin->setSingleStep(0.05);
    m_fadeInSpin->setDecimals(3);
    form->addRow(tr("Fade In:"), m_fadeInSpin);

    m_fadeOutSpin = new QDoubleSpinBox(this);
    m_fadeOutSpin->setRange(0.0, 1.0);
    m_fadeOutSpin->setSingleStep(0.05);
    m_fadeOutSpin->setDecimals(3);
    form->addRow(tr("Fade Out:"), m_fadeOutSpin);

    mainLayout->addWidget(group);
}

// --- Forces ---

void ParticleEffectDialog::setupForcesGroup(QVBoxLayout *mainLayout)
{
    auto *group = new QGroupBox(tr("Forces"), this);
    auto *vbox = new QVBoxLayout(group);

    m_forcesScroll = new QScrollArea(this);
    m_forcesScroll->setWidgetResizable(true);
    m_forcesScroll->setMaximumHeight(200);
    m_forcesScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_forcesScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_forcesContainer = new QWidget;
    m_forcesLayout = new QVBoxLayout(m_forcesContainer);
    m_forcesLayout->setContentsMargins(4, 4, 4, 4);
    m_forcesLayout->setSpacing(4);
    m_forcesLayout->addStretch();

    m_forcesScroll->setWidget(m_forcesContainer);
    vbox->addWidget(m_forcesScroll);

    auto *btnRow = new QHBoxLayout;
    m_addForceBtn = new QPushButton(tr("Add Force"), this);
    connect(m_addForceBtn, &QPushButton::clicked, this, [this]() {
        addForceRow();
        onParamChanged();
    });
    btnRow->addWidget(m_addForceBtn);
    btnRow->addStretch();
    vbox->addLayout(btnRow);

    mainLayout->addWidget(group);
}

void ParticleEffectDialog::addForceRow(const ForceField &ff)
{
    ForceRowWidgets row;

    row.layout = new QHBoxLayout;
    row.layout->setContentsMargins(0, 0, 0, 0);
    row.layout->setSpacing(4);

    row.kindCombo = new QComboBox(this);
    row.kindCombo->addItem(tr("Point Attract"), static_cast<int>(ForceField::PointAttract));
    row.kindCombo->addItem(tr("Point Repel"), static_cast<int>(ForceField::PointRepel));
    row.kindCombo->addItem(tr("Vortex"), static_cast<int>(ForceField::Vortex));
    row.kindCombo->addItem(tr("Wind"), static_cast<int>(ForceField::Wind));
    row.kindCombo->setCurrentIndex(static_cast<int>(ff.kind));
    row.kindCombo->setMinimumWidth(110);
    row.layout->addWidget(row.kindCombo);

    auto *posLayout = new QHBoxLayout;
    posLayout->setSpacing(2);
    row.posX = new QDoubleSpinBox(this);
    row.posX->setRange(0.0, 1.0);
    row.posX->setSingleStep(0.05);
    row.posX->setDecimals(2);
    row.posX->setValue(ff.position.x());
    row.posX->setMinimumWidth(55);
    row.posY = new QDoubleSpinBox(this);
    row.posY->setRange(0.0, 1.0);
    row.posY->setSingleStep(0.05);
    row.posY->setDecimals(2);
    row.posY->setValue(ff.position.y());
    row.posY->setMinimumWidth(55);
    posLayout->addWidget(new QLabel(tr("Pos:"), this));
    posLayout->addWidget(row.posX);
    posLayout->addWidget(row.posY);
    row.layout->addLayout(posLayout);

    row.strength = new QDoubleSpinBox(this);
    row.strength->setRange(-10000.0, 10000.0);
    row.strength->setSingleStep(10.0);
    row.strength->setValue(ff.strength);
    row.strength->setMinimumWidth(70);
    row.layout->addWidget(row.strength);

    row.radius = new QDoubleSpinBox(this);
    row.radius->setRange(0.01, 2.0);
    row.radius->setSingleStep(0.05);
    row.radius->setDecimals(2);
    row.radius->setValue(ff.radius);
    row.radius->setMinimumWidth(55);
    row.layout->addWidget(row.radius);

    row.removeBtn = new QPushButton(tr("X"), this);
    row.removeBtn->setMaximumWidth(28);
    row.removeBtn->setStyleSheet("color: red; font-weight: bold;");
    connect(row.removeBtn, &QPushButton::clicked, this, [this, row]() {
        removeForceRow(row);
        onParamChanged();
    });
    row.layout->addWidget(row.removeBtn);

    // Insert before the stretch
    int stretchIdx = m_forcesLayout->count() - 1;
    m_forcesLayout->insertLayout(stretchIdx, row.layout);

    m_forceRows.append(row);

    // Connect value-changed signals for debounce preview
    connect(row.kindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ParticleEffectDialog::onParamChanged);
    connect(row.posX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &ParticleEffectDialog::onParamChanged);
    connect(row.posY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &ParticleEffectDialog::onParamChanged);
    connect(row.strength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &ParticleEffectDialog::onParamChanged);
    connect(row.radius, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &ParticleEffectDialog::onParamChanged);
}

void ParticleEffectDialog::removeForceRow(const ForceRowWidgets &row)
{
    int idx = -1;
    for (int i = 0; i < m_forceRows.size(); ++i) {
        if (m_forceRows[i].layout == row.layout) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return;

    // Delete all widgets in the row layout
    QHBoxLayout *hLayout = row.layout;
    while (hLayout->count() > 0) {
        auto *wItem = hLayout->takeAt(0);
        if (wItem->widget()) {
            wItem->widget()->deleteLater();
        } else if (wItem->layout()) {
            auto *subLayout = wItem->layout();
            while (subLayout->count() > 0) {
                auto *subItem = subLayout->takeAt(0);
                if (subItem->widget()) {
                    subItem->widget()->deleteLater();
                }
            }
            delete subLayout;
        }
        delete wItem;
    }

    // Remove the layout item from parent
    m_forcesLayout->removeItem(hLayout);
    delete hLayout;

    m_forceRows.removeAt(idx);
}

// --- Collision ---

void ParticleEffectDialog::setupCollisionGroup(QVBoxLayout *mainLayout)
{
    auto *group = new QGroupBox(tr("Collision"), this);
    auto *form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_collisionFloorCheck = new QCheckBox(tr("Enable Floor"), this);
    form->addRow(m_collisionFloorCheck);

    m_floorYSpin = new QDoubleSpinBox(this);
    m_floorYSpin->setRange(0.0, 2.0);
    m_floorYSpin->setSingleStep(0.05);
    m_floorYSpin->setDecimals(3);
    form->addRow(tr("Floor Y:"), m_floorYSpin);

    m_restitutionSpin = new QDoubleSpinBox(this);
    m_restitutionSpin->setRange(0.0, 1.0);
    m_restitutionSpin->setSingleStep(0.05);
    m_restitutionSpin->setDecimals(3);
    form->addRow(tr("Restitution:"), m_restitutionSpin);

    m_floorFrictionSpin = new QDoubleSpinBox(this);
    m_floorFrictionSpin->setRange(0.0, 1.0);
    m_floorFrictionSpin->setSingleStep(0.05);
    m_floorFrictionSpin->setDecimals(3);
    form->addRow(tr("Floor Friction:"), m_floorFrictionSpin);

    mainLayout->addWidget(group);
}

// --- Turbulence ---

void ParticleEffectDialog::setupTurbulenceGroup(QVBoxLayout *mainLayout)
{
    auto *group = new QGroupBox(tr("Turbulence"), this);
    auto *form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_turbulenceAmountSpin = new QDoubleSpinBox(this);
    m_turbulenceAmountSpin->setRange(0.0, 5000.0);
    m_turbulenceAmountSpin->setSingleStep(10.0);
    form->addRow(tr("Amount:"), m_turbulenceAmountSpin);

    m_turbulenceScaleSpin = new QDoubleSpinBox(this);
    m_turbulenceScaleSpin->setRange(0.1, 50.0);
    m_turbulenceScaleSpin->setSingleStep(0.5);
    form->addRow(tr("Scale:"), m_turbulenceScaleSpin);

    m_turbulenceSpeedSpin = new QDoubleSpinBox(this);
    m_turbulenceSpeedSpin->setRange(0.0, 20.0);
    m_turbulenceSpeedSpin->setSingleStep(0.1);
    form->addRow(tr("Speed:"), m_turbulenceSpeedSpin);

    mainLayout->addWidget(group);
}

// --- Preview ---

void ParticleEffectDialog::setupPreview(QVBoxLayout *mainLayout)
{
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Preview:"), this));
    row->addStretch();

    auto *previewGroup = new QGroupBox(this);
    auto *previewLayout = new QVBoxLayout(previewGroup);
    previewLayout->setContentsMargins(2, 2, 2, 2);

    m_previewLabel = new QLabel(this);
    m_previewLabel->setMinimumSize(320, 180);
    m_previewLabel->setMaximumSize(320, 180);
    m_previewLabel->setScaledContents(false);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #444;");
    previewLayout->addWidget(m_previewLabel);

    row->addWidget(previewGroup);
    mainLayout->addLayout(row);

    m_previewDebounce = new QTimer(this);
    m_previewDebounce->setSingleShot(true);
    m_previewDebounce->setInterval(300);
    connect(m_previewDebounce, &QTimer::timeout, this, &ParticleEffectDialog::refreshPreview);
}

void ParticleEffectDialog::refreshPreview()
{
    ParticleEmitterConfig cfg = config();
    ParticleSystem ps;
    ps.setConfig(cfg);

    QSize canvasSize(320, 180);
    double dt = 1.0 / 60.0;
    double totalSim = 2.0;

    // Simulate forward for ~2 seconds
    while (totalSim > 0) {
        ps.update(dt);
        totalSim -= dt;
    }

    QImage frame = ps.renderFrame(canvasSize, 2.0);
    if (!frame.isNull()) {
        QPixmap pm = QPixmap::fromImage(frame);
        m_previewLabel->setPixmap(pm.scaled(320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        // Even with no particles, renderFrame should return a valid transparent image
        // but as fallback, set a blank pixmap
        if (m_previewLabel->pixmap().isNull()) {
            QPixmap blank(320, 180);
            blank.fill(Qt::transparent);
            m_previewLabel->setPixmap(blank);
        }
    }
}

void ParticleEffectDialog::onParamChanged()
{
    if (m_previewDebounce) {
        m_previewDebounce->start();
    }
}

// --- Buttons ---

void ParticleEffectDialog::setupButtons(QVBoxLayout *mainLayout)
{
    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(m_buttonBox);
}

// --- setConfig ---

void ParticleEffectDialog::setConfig(const ParticleEmitterConfig &cfg)
{
    // Emission
    int typeIdx = m_typeCombo->findData(static_cast<int>(cfg.type));
    if (typeIdx >= 0) m_typeCombo->setCurrentIndex(typeIdx);

    m_emitRateSpin->setValue(cfg.emitRate);
    m_maxParticlesSpin->setValue(cfg.maxParticles);
    m_emitPosX->setValue(cfg.emitPosition.x());
    m_emitPosY->setValue(cfg.emitPosition.y());
    m_emitAreaW->setValue(cfg.emitAreaSize.width());
    m_emitAreaH->setValue(cfg.emitAreaSize.height());

    // Lifetime
    m_lifeMinSpin->setValue(cfg.lifeMin);
    m_lifeMaxSpin->setValue(cfg.lifeMax);

    // Size
    m_sizeMinSpin->setValue(cfg.sizeMin);
    m_sizeMaxSpin->setValue(cfg.sizeMax);
    m_sizeStartMultSpin->setValue(cfg.sizeStartMult);
    m_sizeEndMultSpin->setValue(cfg.sizeEndMult);

    // Motion
    m_speedMinSpin->setValue(cfg.speedMin);
    m_speedMaxSpin->setValue(cfg.speedMax);
    m_directionSpin->setValue(cfg.direction);
    m_spreadSpin->setValue(cfg.spread);
    m_gravityX->setValue(cfg.gravity.x());
    m_gravityY->setValue(cfg.gravity.y());
    m_windX->setValue(cfg.wind.x());
    m_windY->setValue(cfg.wind.y());

    // Color
    m_startColor = cfg.startColor;
    m_endColor = cfg.endColor;
    updateColorButton(m_startColorBtn, m_startColor);
    updateColorButton(m_endColorBtn, m_endColor);

    // Opacity
    m_fadeInSpin->setValue(cfg.fadeIn);
    m_fadeOutSpin->setValue(cfg.fadeOut);

    // Forces: clear existing rows, then rebuild
    clearForceRows();
    for (const auto &ff : cfg.forceFields) {
        addForceRow(ff);
    }

    // Collision
    m_collisionFloorCheck->setChecked(cfg.collisionFloor);
    m_floorYSpin->setValue(cfg.floorY);
    m_restitutionSpin->setValue(cfg.restitution);
    m_floorFrictionSpin->setValue(cfg.floorFriction);

    // Turbulence
    m_turbulenceAmountSpin->setValue(cfg.turbulenceAmount);
    m_turbulenceScaleSpin->setValue(cfg.turbulenceScale);
    m_turbulenceSpeedSpin->setValue(cfg.turbulenceSpeed);

    // Update preset combo to match
    auto presets = ParticleSystem::presetConfigs();
    int presetIdx = -1;
    for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
        if (it.value().type == cfg.type &&
            qFuzzyCompare(it.value().emitRate, cfg.emitRate) &&
            it.value().maxParticles == cfg.maxParticles) {
            presetIdx = m_presetCombo->findText(it.key());
            if (presetIdx >= 0) break;
        }
    }
    if (presetIdx >= 0) {
        m_presetCombo->blockSignals(true);
        m_presetCombo->setCurrentIndex(presetIdx);
        m_presetCombo->blockSignals(false);
    } else {
        m_presetCombo->setCurrentIndex(-1);
    }

    refreshPreview();
}

void ParticleEffectDialog::clearForceRows()
{
    // Remove all row layouts from m_forcesLayout (everything before the stretch)
    while (m_forceRows.size() > 0) {
        removeForceRow(m_forceRows.first());
    }
}

// --- config() ---

ParticleEmitterConfig ParticleEffectDialog::config() const
{
    ParticleEmitterConfig cfg;

    // Emission
    cfg.type = static_cast<ParticleType>(m_typeCombo->currentData().toInt());
    cfg.emitRate = m_emitRateSpin->value();
    cfg.maxParticles = m_maxParticlesSpin->value();
    cfg.emitPosition = QPointF(m_emitPosX->value(), m_emitPosY->value());
    cfg.emitAreaSize = QSizeF(m_emitAreaW->value(), m_emitAreaH->value());

    // Lifetime
    cfg.lifeMin = m_lifeMinSpin->value();
    cfg.lifeMax = m_lifeMaxSpin->value();

    // Size
    cfg.sizeMin = m_sizeMinSpin->value();
    cfg.sizeMax = m_sizeMaxSpin->value();
    cfg.sizeStartMult = m_sizeStartMultSpin->value();
    cfg.sizeEndMult = m_sizeEndMultSpin->value();

    // Motion
    cfg.speedMin = m_speedMinSpin->value();
    cfg.speedMax = m_speedMaxSpin->value();
    cfg.direction = m_directionSpin->value();
    cfg.spread = m_spreadSpin->value();
    cfg.gravity = QPointF(m_gravityX->value(), m_gravityY->value());
    cfg.wind = QPointF(m_windX->value(), m_windY->value());

    // Color
    cfg.startColor = m_startColor;
    cfg.endColor = m_endColor;

    // Opacity
    cfg.fadeIn = m_fadeInSpin->value();
    cfg.fadeOut = m_fadeOutSpin->value();

    // Forces
    cfg.forceFields.clear();
    for (const auto &row : m_forceRows) {
        ForceField ff;
        ff.kind = static_cast<ForceField::Kind>(row.kindCombo->currentData().toInt());
        ff.position = QPointF(row.posX->value(), row.posY->value());
        ff.strength = row.strength->value();
        ff.radius = row.radius->value();
        cfg.forceFields.append(ff);
    }

    // Collision
    cfg.collisionFloor = m_collisionFloorCheck->isChecked();
    cfg.floorY = m_floorYSpin->value();
    cfg.restitution = m_restitutionSpin->value();
    cfg.floorFriction = m_floorFrictionSpin->value();

    // Turbulence
    cfg.turbulenceAmount = m_turbulenceAmountSpin->value();
    cfg.turbulenceScale = m_turbulenceScaleSpin->value();
    cfg.turbulenceSpeed = m_turbulenceSpeedSpin->value();

    return cfg;
}
