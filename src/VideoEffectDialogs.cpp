#include "VideoEffectDialogs.h"
#include <QDialogButtonBox>
#include <QMessageBox>

// ===== ColorCorrectionDialog =====

ColorCorrectionDialog::ColorCorrectionDialog(const ColorCorrection &initial, QWidget *parent)
    : QDialog(parent), m_cc(initial)
{
    setWindowTitle("Color Correction / Grading");
    setMinimumWidth(450);

    auto *mainLayout = new QVBoxLayout(this);

    // Sliders grid
    auto *grid = new QGridLayout();
    grid->setColumnStretch(1, 1);

    int row = 0;
    m_exposure   = addSlider(grid, row++, "Exposure",    -300, 300,  static_cast<int>(m_cc.exposure * 100), 100);
    m_brightness = addSlider(grid, row++, "Brightness",  -100, 100,  static_cast<int>(m_cc.brightness));
    m_contrast   = addSlider(grid, row++, "Contrast",    -100, 100,  static_cast<int>(m_cc.contrast));
    m_highlights = addSlider(grid, row++, "Highlights",  -100, 100,  static_cast<int>(m_cc.highlights));
    m_shadows    = addSlider(grid, row++, "Shadows",     -100, 100,  static_cast<int>(m_cc.shadows));
    m_saturation = addSlider(grid, row++, "Saturation",  -100, 100,  static_cast<int>(m_cc.saturation));
    m_hue        = addSlider(grid, row++, "Hue",         -180, 180,  static_cast<int>(m_cc.hue));
    m_temperature= addSlider(grid, row++, "Temperature", -100, 100,  static_cast<int>(m_cc.temperature));
    m_tint       = addSlider(grid, row++, "Tint",        -100, 100,  static_cast<int>(m_cc.tint));
    m_gamma      = addSlider(grid, row++, "Gamma",       10,   300,  static_cast<int>(m_cc.gamma * 100), 100);

    mainLayout->addLayout(grid);

    // Reset button
    auto *resetBtn = new QPushButton("Reset All");
    connect(resetBtn, &QPushButton::clicked, this, &ColorCorrectionDialog::resetAll);
    mainLayout->addWidget(resetBtn);

    // OK / Cancel
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

ColorCorrectionDialog::SliderRow ColorCorrectionDialog::addSlider(
    QGridLayout *layout, int row, const QString &label, int min, int max, int initial, int scale)
{
    auto *lbl = new QLabel(label);
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(min, max);
    slider->setValue(initial);
    auto *valLabel = new QLabel();

    if (scale > 1)
        valLabel->setText(QString::number(initial / static_cast<double>(scale), 'f', 2));
    else
        valLabel->setText(QString::number(initial));
    valLabel->setFixedWidth(50);
    valLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    layout->addWidget(lbl, row, 0);
    layout->addWidget(slider, row, 1);
    layout->addWidget(valLabel, row, 2);

    connect(slider, &QSlider::valueChanged, this, &ColorCorrectionDialog::onSliderChanged);

    return { slider, valLabel };
}

void ColorCorrectionDialog::onSliderChanged()
{
    m_cc.brightness  = m_brightness.slider->value();
    m_cc.contrast    = m_contrast.slider->value();
    m_cc.saturation  = m_saturation.slider->value();
    m_cc.hue         = m_hue.slider->value();
    m_cc.temperature = m_temperature.slider->value();
    m_cc.tint        = m_tint.slider->value();
    m_cc.gamma       = m_gamma.slider->value() / 100.0;
    m_cc.highlights  = m_highlights.slider->value();
    m_cc.shadows     = m_shadows.slider->value();
    m_cc.exposure    = m_exposure.slider->value() / 100.0;

    // Update labels
    m_brightness.valueLabel->setText(QString::number(m_cc.brightness));
    m_contrast.valueLabel->setText(QString::number(m_cc.contrast));
    m_saturation.valueLabel->setText(QString::number(m_cc.saturation));
    m_hue.valueLabel->setText(QString::number(m_cc.hue));
    m_temperature.valueLabel->setText(QString::number(m_cc.temperature));
    m_tint.valueLabel->setText(QString::number(m_cc.tint));
    m_gamma.valueLabel->setText(QString::number(m_cc.gamma, 'f', 2));
    m_highlights.valueLabel->setText(QString::number(m_cc.highlights));
    m_shadows.valueLabel->setText(QString::number(m_cc.shadows));
    m_exposure.valueLabel->setText(QString::number(m_cc.exposure, 'f', 2));
}

void ColorCorrectionDialog::resetAll()
{
    m_cc.reset();
    m_brightness.slider->setValue(0);
    m_contrast.slider->setValue(0);
    m_saturation.slider->setValue(0);
    m_hue.slider->setValue(0);
    m_temperature.slider->setValue(0);
    m_tint.slider->setValue(0);
    m_gamma.slider->setValue(100);
    m_highlights.slider->setValue(0);
    m_shadows.slider->setValue(0);
    m_exposure.slider->setValue(0);
}

// ===== VideoEffectDialog =====

VideoEffectDialog::VideoEffectDialog(const QVector<VideoEffect> &initial, QWidget *parent)
    : QDialog(parent), m_effects(initial)
{
    setWindowTitle("Video Effects");
    setMinimumSize(500, 400);

    auto *mainLayout = new QHBoxLayout(this);

    // Left panel: effect list + add/remove
    auto *leftPanel = new QVBoxLayout();

    m_effectList = new QListWidget();
    leftPanel->addWidget(m_effectList);

    auto *addRow = new QHBoxLayout();
    m_typeCombo = new QComboBox();
    for (auto t : VideoEffect::allTypes())
        m_typeCombo->addItem(VideoEffect::typeName(t), static_cast<int>(t));
    addRow->addWidget(m_typeCombo);

    auto *addBtn = new QPushButton("Add");
    connect(addBtn, &QPushButton::clicked, this, &VideoEffectDialog::addEffect);
    addRow->addWidget(addBtn);
    leftPanel->addLayout(addRow);

    auto *btnRow = new QHBoxLayout();
    auto *removeBtn = new QPushButton("Remove");
    connect(removeBtn, &QPushButton::clicked, this, &VideoEffectDialog::removeEffect);
    btnRow->addWidget(removeBtn);

    auto *upBtn = new QPushButton("Up");
    connect(upBtn, &QPushButton::clicked, this, &VideoEffectDialog::moveUp);
    btnRow->addWidget(upBtn);

    auto *downBtn = new QPushButton("Down");
    connect(downBtn, &QPushButton::clicked, this, &VideoEffectDialog::moveDown);
    btnRow->addWidget(downBtn);
    leftPanel->addLayout(btnRow);

    mainLayout->addLayout(leftPanel);

    // Right panel: parameters
    auto *rightPanel = new QVBoxLayout();

    m_enabledCheck = new QCheckBox("Enabled");
    m_enabledCheck->setChecked(true);
    connect(m_enabledCheck, &QCheckBox::toggled, this, &VideoEffectDialog::onParamChanged);
    rightPanel->addWidget(m_enabledCheck);

    auto *paramGroup = new QGroupBox("Parameters");
    auto *paramGrid = new QGridLayout(paramGroup);

    m_param1Label = new QLabel("Param 1:");
    m_param1Spin = new QDoubleSpinBox();
    m_param1Spin->setRange(0, 100); m_param1Spin->setDecimals(1);
    paramGrid->addWidget(m_param1Label, 0, 0);
    paramGrid->addWidget(m_param1Spin, 0, 1);

    m_param2Label = new QLabel("Param 2:");
    m_param2Spin = new QDoubleSpinBox();
    m_param2Spin->setRange(0, 100); m_param2Spin->setDecimals(1);
    paramGrid->addWidget(m_param2Label, 1, 0);
    paramGrid->addWidget(m_param2Spin, 1, 1);

    m_param3Label = new QLabel("Param 3:");
    m_param3Spin = new QDoubleSpinBox();
    m_param3Spin->setRange(0, 100); m_param3Spin->setDecimals(1);
    paramGrid->addWidget(m_param3Label, 2, 0);
    paramGrid->addWidget(m_param3Spin, 2, 1);

    m_colorButton = new QPushButton("Key Color");
    connect(m_colorButton, &QPushButton::clicked, this, &VideoEffectDialog::pickColor);
    paramGrid->addWidget(m_colorButton, 3, 0, 1, 2);

    rightPanel->addWidget(paramGroup);
    rightPanel->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rightPanel->addWidget(buttons);

    mainLayout->addLayout(rightPanel);

    connect(m_effectList, &QListWidget::currentRowChanged, this, &VideoEffectDialog::onEffectSelected);
    connect(m_param1Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &VideoEffectDialog::onParamChanged);
    connect(m_param2Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &VideoEffectDialog::onParamChanged);
    connect(m_param3Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &VideoEffectDialog::onParamChanged);

    refreshList();
    if (!m_effects.isEmpty()) {
        m_effectList->setCurrentRow(0);
        updateParamUI(0);
    }
}

void VideoEffectDialog::addEffect()
{
    auto type = static_cast<VideoEffectType>(m_typeCombo->currentData().toInt());
    VideoEffect effect;
    switch (type) {
    case VideoEffectType::Blur:      effect = VideoEffect::createBlur(); break;
    case VideoEffectType::Sharpen:   effect = VideoEffect::createSharpen(); break;
    case VideoEffectType::Mosaic:    effect = VideoEffect::createMosaic(); break;
    case VideoEffectType::ChromaKey: effect = VideoEffect::createChromaKey(); break;
    case VideoEffectType::Vignette:  effect = VideoEffect::createVignette(); break;
    case VideoEffectType::Sepia:     effect = VideoEffect::createSepia(); break;
    case VideoEffectType::Grayscale: effect = VideoEffect::createGrayscale(); break;
    case VideoEffectType::Invert:    effect = VideoEffect::createInvert(); break;
    case VideoEffectType::Noise:     effect = VideoEffect::createNoise(); break;
    default: return;
    }
    m_effects.append(effect);
    refreshList();
    m_effectList->setCurrentRow(m_effects.size() - 1);
}

void VideoEffectDialog::removeEffect()
{
    int row = m_effectList->currentRow();
    if (row < 0 || row >= m_effects.size()) return;
    m_effects.removeAt(row);
    refreshList();
}

void VideoEffectDialog::moveUp()
{
    int row = m_effectList->currentRow();
    if (row <= 0) return;
    std::swap(m_effects[row], m_effects[row - 1]);
    refreshList();
    m_effectList->setCurrentRow(row - 1);
}

void VideoEffectDialog::moveDown()
{
    int row = m_effectList->currentRow();
    if (row < 0 || row >= m_effects.size() - 1) return;
    std::swap(m_effects[row], m_effects[row + 1]);
    refreshList();
    m_effectList->setCurrentRow(row + 1);
}

void VideoEffectDialog::refreshList()
{
    m_effectList->clear();
    for (const auto &e : m_effects) {
        QString name = VideoEffect::typeName(e.type);
        if (!e.enabled) name += " [OFF]";
        m_effectList->addItem(name);
    }
}

void VideoEffectDialog::onEffectSelected(int row)
{
    updateParamUI(row);
}

void VideoEffectDialog::updateParamUI(int index)
{
    bool valid = index >= 0 && index < m_effects.size();
    m_param1Label->setVisible(false); m_param1Spin->setVisible(false);
    m_param2Label->setVisible(false); m_param2Spin->setVisible(false);
    m_param3Label->setVisible(false); m_param3Spin->setVisible(false);
    m_colorButton->setVisible(false);

    if (!valid) return;

    const auto &e = m_effects[index];
    m_enabledCheck->setChecked(e.enabled);

    // Block signals during UI update
    m_param1Spin->blockSignals(true);
    m_param2Spin->blockSignals(true);
    m_param3Spin->blockSignals(true);

    switch (e.type) {
    case VideoEffectType::Blur:
        m_param1Label->setText("Radius:"); m_param1Spin->setRange(1, 50);
        m_param1Spin->setValue(e.param1);
        m_param1Label->setVisible(true); m_param1Spin->setVisible(true);
        break;
    case VideoEffectType::Sharpen:
        m_param1Label->setText("Amount:"); m_param1Spin->setRange(0.1, 10.0);
        m_param1Spin->setValue(e.param1);
        m_param1Label->setVisible(true); m_param1Spin->setVisible(true);
        break;
    case VideoEffectType::Mosaic:
        m_param1Label->setText("Block Size:"); m_param1Spin->setRange(2, 100);
        m_param1Spin->setValue(e.param1);
        m_param1Label->setVisible(true); m_param1Spin->setVisible(true);
        break;
    case VideoEffectType::ChromaKey:
        m_param1Label->setText("Tolerance:"); m_param1Spin->setRange(1, 200);
        m_param1Spin->setValue(e.param1);
        m_param2Label->setText("Softness:"); m_param2Spin->setRange(0, 100);
        m_param2Spin->setValue(e.param2);
        m_param1Label->setVisible(true); m_param1Spin->setVisible(true);
        m_param2Label->setVisible(true); m_param2Spin->setVisible(true);
        m_colorButton->setVisible(true);
        m_colorButton->setStyleSheet(QString("background-color: %1").arg(e.keyColor.name()));
        break;
    case VideoEffectType::Vignette:
        m_param1Label->setText("Intensity:"); m_param1Spin->setRange(0.0, 1.0);
        m_param1Spin->setValue(e.param1); m_param1Spin->setSingleStep(0.1);
        m_param2Label->setText("Radius:"); m_param2Spin->setRange(0.0, 1.0);
        m_param2Spin->setValue(e.param2); m_param2Spin->setSingleStep(0.1);
        m_param1Label->setVisible(true); m_param1Spin->setVisible(true);
        m_param2Label->setVisible(true); m_param2Spin->setVisible(true);
        break;
    case VideoEffectType::Sepia:
        m_param1Label->setText("Intensity:"); m_param1Spin->setRange(0.0, 1.0);
        m_param1Spin->setValue(e.param1); m_param1Spin->setSingleStep(0.1);
        m_param1Label->setVisible(true); m_param1Spin->setVisible(true);
        break;
    case VideoEffectType::Noise:
        m_param1Label->setText("Amount:"); m_param1Spin->setRange(1, 100);
        m_param1Spin->setValue(e.param1);
        m_param1Label->setVisible(true); m_param1Spin->setVisible(true);
        break;
    default:
        break; // Grayscale, Invert: no params
    }

    m_param1Spin->blockSignals(false);
    m_param2Spin->blockSignals(false);
    m_param3Spin->blockSignals(false);
}

void VideoEffectDialog::onParamChanged()
{
    int row = m_effectList->currentRow();
    if (row < 0 || row >= m_effects.size()) return;

    m_effects[row].enabled = m_enabledCheck->isChecked();
    m_effects[row].param1 = m_param1Spin->value();
    m_effects[row].param2 = m_param2Spin->value();
    m_effects[row].param3 = m_param3Spin->value();
    refreshList();
    m_effectList->setCurrentRow(row);
}

void VideoEffectDialog::pickColor()
{
    int row = m_effectList->currentRow();
    if (row < 0 || row >= m_effects.size()) return;

    QColor color = QColorDialog::getColor(m_effects[row].keyColor, this, "Select Key Color");
    if (color.isValid()) {
        m_effects[row].keyColor = color;
        m_colorButton->setStyleSheet(QString("background-color: %1").arg(color.name()));
    }
}

// ===== PluginEffectDialog =====

PluginEffectDialog::PluginEffectDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Plugin Effects");
    setMinimumSize(450, 350);

    auto *mainLayout = new QHBoxLayout(this);

    // Left: plugin list
    auto *leftLayout = new QVBoxLayout();
    auto *listLabel = new QLabel("Available Plugins:");
    leftLayout->addWidget(listLabel);

    m_pluginList = new QListWidget();
    const auto &plugins = PluginRegistry::instance().allPlugins();
    for (const auto &p : plugins)
        m_pluginList->addItem(QString("[%1] %2").arg(p->category(), p->name()));
    leftLayout->addWidget(m_pluginList);
    mainLayout->addLayout(leftLayout);

    // Right: description + params
    auto *rightLayout = new QVBoxLayout();

    m_descLabel = new QLabel("Select a plugin");
    m_descLabel->setWordWrap(true);
    rightLayout->addWidget(m_descLabel);

    auto *paramGroup = new QGroupBox("Parameters");
    m_paramLayout = new QVBoxLayout(paramGroup);
    rightLayout->addWidget(paramGroup);
    rightLayout->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rightLayout->addWidget(buttons);

    mainLayout->addLayout(rightLayout);

    connect(m_pluginList, &QListWidget::currentRowChanged, this, &PluginEffectDialog::onPluginSelected);
}

void PluginEffectDialog::onPluginSelected(int row)
{
    const auto &plugins = PluginRegistry::instance().allPlugins();
    if (row < 0 || row >= plugins.size()) return;

    auto plugin = plugins[row];
    m_selectedPlugin = plugin->name();
    m_descLabel->setText(plugin->description());

    // Clear old param controls
    for (auto *spin : m_paramSpins) {
        m_paramLayout->removeWidget(spin);
        delete spin;
    }
    m_paramSpins.clear();
    m_paramValues.clear();

    // Remove leftover labels
    while (m_paramLayout->count() > 0) {
        auto *item = m_paramLayout->takeAt(0);
        delete item->widget();
        delete item;
    }

    // Create param controls
    for (const auto &def : plugin->parameterDefs()) {
        auto *label = new QLabel(def.name);
        m_paramLayout->addWidget(label);

        auto *spin = new QDoubleSpinBox();
        spin->setRange(def.min, def.max);
        spin->setValue(def.defaultValue);
        spin->setDecimals(1);
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, &PluginEffectDialog::onParamChanged);
        m_paramLayout->addWidget(spin);
        m_paramSpins.append(spin);
        m_paramValues.append(def.defaultValue);
    }
}

void PluginEffectDialog::onParamChanged()
{
    m_paramValues.clear();
    for (auto *spin : m_paramSpins)
        m_paramValues.append(spin->value());
}

// ===== KeyframeDialog =====

KeyframeDialog::KeyframeDialog(const KeyframeTrack &track, double clipDuration, QWidget *parent)
    : QDialog(parent), m_track(track), m_clipDuration(clipDuration)
{
    setWindowTitle(QString("Keyframes: %1").arg(track.propertyName()));
    setMinimumSize(400, 300);

    auto *mainLayout = new QVBoxLayout(this);

    auto *titleLabel = new QLabel(QString("Property: %1 (Default: %2)")
        .arg(track.propertyName()).arg(track.defaultValue()));
    mainLayout->addWidget(titleLabel);

    m_kfList = new QListWidget();
    mainLayout->addWidget(m_kfList);

    // Add keyframe controls
    auto *addLayout = new QHBoxLayout();

    addLayout->addWidget(new QLabel("Time:"));
    m_timeSpin = new QDoubleSpinBox();
    m_timeSpin->setRange(0.0, clipDuration);
    m_timeSpin->setDecimals(2);
    m_timeSpin->setSuffix("s");
    addLayout->addWidget(m_timeSpin);

    addLayout->addWidget(new QLabel("Value:"));
    m_valueSpin = new QDoubleSpinBox();
    m_valueSpin->setRange(-10000, 10000);
    m_valueSpin->setDecimals(2);
    addLayout->addWidget(m_valueSpin);

    m_interpCombo = new QComboBox();
    m_interpCombo->addItem("Linear", static_cast<int>(KeyframePoint::Linear));
    m_interpCombo->addItem("Ease In", static_cast<int>(KeyframePoint::EaseIn));
    m_interpCombo->addItem("Ease Out", static_cast<int>(KeyframePoint::EaseOut));
    m_interpCombo->addItem("Ease In/Out", static_cast<int>(KeyframePoint::EaseInOut));
    m_interpCombo->addItem("Hold", static_cast<int>(KeyframePoint::Hold));
    addLayout->addWidget(m_interpCombo);

    mainLayout->addLayout(addLayout);

    auto *btnLayout = new QHBoxLayout();
    auto *addBtn = new QPushButton("Add Keyframe");
    connect(addBtn, &QPushButton::clicked, this, &KeyframeDialog::addKeyframe);
    btnLayout->addWidget(addBtn);

    auto *removeBtn = new QPushButton("Remove");
    connect(removeBtn, &QPushButton::clicked, this, &KeyframeDialog::removeKeyframe);
    btnLayout->addWidget(removeBtn);
    mainLayout->addLayout(btnLayout);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);

    connect(m_kfList, &QListWidget::currentRowChanged, this, &KeyframeDialog::onSelectionChanged);
    refreshList();
}

void KeyframeDialog::addKeyframe()
{
    auto interp = static_cast<KeyframePoint::Interpolation>(m_interpCombo->currentData().toInt());
    m_track.addKeyframe(m_timeSpin->value(), m_valueSpin->value(), interp);
    refreshList();
}

void KeyframeDialog::removeKeyframe()
{
    int row = m_kfList->currentRow();
    if (row >= 0) {
        m_track.removeKeyframe(row);
        refreshList();
    }
}

void KeyframeDialog::onSelectionChanged()
{
    int row = m_kfList->currentRow();
    if (row >= 0 && row < m_track.count()) {
        const auto &kf = m_track.keyframes()[row];
        m_timeSpin->setValue(kf.time);
        m_valueSpin->setValue(kf.value);
        m_interpCombo->setCurrentIndex(static_cast<int>(kf.interpolation));
    }
}

void KeyframeDialog::refreshList()
{
    m_kfList->clear();
    static const char* interpNames[] = {"Linear", "EaseIn", "EaseOut", "EaseInOut", "Hold"};
    for (const auto &kf : m_track.keyframes()) {
        m_kfList->addItem(QString("%1s = %2 (%3)")
            .arg(kf.time, 0, 'f', 2)
            .arg(kf.value, 0, 'f', 2)
            .arg(interpNames[static_cast<int>(kf.interpolation)]));
    }
}
