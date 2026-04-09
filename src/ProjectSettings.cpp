#include "ProjectSettings.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QGroupBox>
#include <QPushButton>
#include <QDialogButtonBox>

QVector<ProjectPreset> ProjectSettingsDialog::presets()
{
    return {
        {"YouTube (1080p 16:9)",          1920, 1080, 30},
        {"YouTube (4K 16:9)",             3840, 2160, 30},
        {"YouTube Shorts (9:16)",         1080, 1920, 30},
        {"TikTok / Reels (9:16)",         1080, 1920, 30},
        {"X / Twitter (720p)",            1280,  720, 30},
        {"Facebook (1080p 16:9)",         1920, 1080, 30},
        {"Twitch Clip (1080p)",           1920, 1080, 60},
        {"Discord (720p size-opt)",       1280,  720, 30},
        {"Niconico (1080p)",              1920, 1080, 30},
        {"Square 1:1 (Instagram post)",   1080, 1080, 30},
    };
}

ProjectSettingsDialog::ProjectSettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("New Project");
    setMinimumWidth(420);
    setupUI();
}

void ProjectSettingsDialog::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);

    // Project name
    auto *nameGroup = new QGroupBox("Project");
    auto *nameLayout = new QFormLayout(nameGroup);
    m_nameEdit = new QLineEdit("Untitled", this);
    nameLayout->addRow("Name:", m_nameEdit);
    mainLayout->addWidget(nameGroup);

    // Preset selection
    auto *presetGroup = new QGroupBox("Resolution Preset");
    auto *presetLayout = new QVBoxLayout(presetGroup);

    m_presetCombo = new QComboBox(this);
    const auto presetList = presets();
    for (const auto &p : presetList) {
        QString label = QString("%1  (%2x%3, %4fps)")
            .arg(p.name).arg(p.width).arg(p.height).arg(p.fps);
        m_presetCombo->addItem(label);
    }
    m_presetCombo->addItem("Custom...");
    presetLayout->addWidget(m_presetCombo);

    // Custom resolution
    auto *customLayout = new QHBoxLayout();
    m_widthSpin = new QSpinBox(this);
    m_widthSpin->setRange(320, 7680);
    m_widthSpin->setValue(1920);
    m_widthSpin->setSuffix(" px");

    auto *xLabel = new QLabel("x");

    m_heightSpin = new QSpinBox(this);
    m_heightSpin->setRange(240, 4320);
    m_heightSpin->setValue(1080);
    m_heightSpin->setSuffix(" px");

    customLayout->addWidget(new QLabel("Width:"));
    customLayout->addWidget(m_widthSpin);
    customLayout->addWidget(xLabel);
    customLayout->addWidget(new QLabel("Height:"));
    customLayout->addWidget(m_heightSpin);
    presetLayout->addLayout(customLayout);

    m_widthSpin->setEnabled(false);
    m_heightSpin->setEnabled(false);

    mainLayout->addWidget(presetGroup);

    // FPS
    auto *fpsGroup = new QGroupBox("Frame Rate");
    auto *fpsLayout = new QHBoxLayout(fpsGroup);
    m_fpsCombo = new QComboBox(this);
    m_fpsCombo->addItems({"24", "25", "30", "50", "60"});
    m_fpsCombo->setCurrentText("30");
    fpsLayout->addWidget(new QLabel("FPS:"));
    fpsLayout->addWidget(m_fpsCombo);
    fpsLayout->addStretch();
    mainLayout->addWidget(fpsGroup);

    // Preview label
    auto *previewLabel = new QLabel(this);
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setStyleSheet("color: #666; font-size: 12px;");
    auto updatePreview = [this, previewLabel]() {
        double ar = static_cast<double>(m_widthSpin->value()) / m_heightSpin->value();
        QString orientation = (ar > 1.0) ? "Landscape" : (ar < 1.0) ? "Portrait" : "Square";
        previewLabel->setText(QString("%1x%2 | %3 | %4fps")
            .arg(m_widthSpin->value()).arg(m_heightSpin->value())
            .arg(orientation).arg(m_fpsCombo->currentText()));
    };
    mainLayout->addWidget(previewLabel);

    // Buttons
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText("Create");
    mainLayout->addWidget(buttons);

    // Connections
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ProjectSettingsDialog::onPresetChanged);
    connect(m_widthSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, updatePreview]() {
        onCustomResolutionChanged();
        updatePreview();
    });
    connect(m_heightSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, updatePreview]() {
        onCustomResolutionChanged();
        updatePreview();
    });
    connect(m_fpsCombo, &QComboBox::currentTextChanged, this, [updatePreview]() { updatePreview(); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_config.name = m_nameEdit->text();
        m_config.width = m_widthSpin->value();
        m_config.height = m_heightSpin->value();
        m_config.fps = m_fpsCombo->currentText().toInt();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Init
    onPresetChanged(0);
    updatePreview();
}

void ProjectSettingsDialog::onPresetChanged(int index)
{
    const auto presetList = presets();
    bool isCustom = (index >= presetList.size());

    m_widthSpin->setEnabled(isCustom);
    m_heightSpin->setEnabled(isCustom);

    if (!isCustom) {
        const auto &p = presetList[index];
        m_widthSpin->setValue(p.width);
        m_heightSpin->setValue(p.height);
        m_fpsCombo->setCurrentText(QString::number(p.fps));
    }
}

void ProjectSettingsDialog::onCustomResolutionChanged()
{
    // If user edits custom values, just update config
}
