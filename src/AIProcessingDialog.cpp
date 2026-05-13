#include "AIProcessingDialog.h"
#include "AIUpscale.h"
#include "FrameInterpolator.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QVBoxLayout>

AIProcessingDialog::AIProcessingDialog(const AIProcessingSettings& initial, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("AI Processing Settings"));

    // --- Upscale group ---
    auto* upscaleGroup  = new QGroupBox(tr("AI Upscale"), this);
    auto* upscaleForm   = new QFormLayout(upscaleGroup);

    m_upscaleEnabledCheck = new QCheckBox(tr("Enable"), upscaleGroup);
    m_upscaleEnabledCheck->setChecked(initial.upscaleEnabled);
    upscaleForm->addRow(m_upscaleEnabledCheck);

    m_upscaleEngineCombo = new QComboBox(upscaleGroup);
    const auto upscaleEngines = AIUpscaleManager::engines();
    if (upscaleEngines.isEmpty()) {
        m_upscaleEngineCombo->addItem("Lanczos3");
        m_upscaleEngineCombo->addItem("Bicubic");
    } else {
        for (AIUpscaleEngine* e : upscaleEngines)
            m_upscaleEngineCombo->addItem(e->name());
    }
    {
        int idx = m_upscaleEngineCombo->findText(initial.upscaleEngine);
        if (idx >= 0) m_upscaleEngineCombo->setCurrentIndex(idx);
    }
    upscaleForm->addRow(tr("Engine:"), m_upscaleEngineCombo);

    m_upscaleFactorSpin = new QSpinBox(upscaleGroup);
    m_upscaleFactorSpin->setRange(1, 4);
    m_upscaleFactorSpin->setValue(initial.upscaleFactor);
    upscaleForm->addRow(tr("Scale factor:"), m_upscaleFactorSpin);

    // --- Frame interpolation group ---
    auto* interpGroup = new QGroupBox(tr("Frame Interpolation"), this);
    auto* interpForm  = new QFormLayout(interpGroup);

    m_frameInterpEnabledCheck = new QCheckBox(tr("Enable"), interpGroup);
    m_frameInterpEnabledCheck->setChecked(initial.frameInterpEnabled);
    interpForm->addRow(m_frameInterpEnabledCheck);

    m_frameInterpEngineCombo = new QComboBox(interpGroup);
    const auto interpEngines = FrameInterpolatorManager::engines();
    if (interpEngines.isEmpty()) {
        m_frameInterpEngineCombo->addItem("Linear");
        m_frameInterpEngineCombo->addItem("Motion-Blend");
    } else {
        for (FrameInterpolatorEngine* e : interpEngines)
            m_frameInterpEngineCombo->addItem(e->name());
    }
    {
        int idx = m_frameInterpEngineCombo->findText(initial.frameInterpEngine);
        if (idx >= 0) m_frameInterpEngineCombo->setCurrentIndex(idx);
    }
    interpForm->addRow(tr("Engine:"), m_frameInterpEngineCombo);

    m_frameInterpFactorSpin = new QSpinBox(interpGroup);
    m_frameInterpFactorSpin->setRange(1, 8);
    m_frameInterpFactorSpin->setValue(initial.frameInterpFactor);
    interpForm->addRow(tr("Frame factor:"), m_frameInterpFactorSpin);

    // --- Button box ---
    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &AIProcessingDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &AIProcessingDialog::reject);

    // --- Top-level layout ---
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(upscaleGroup);
    layout->addWidget(interpGroup);
    layout->addWidget(m_buttonBox);
}

AIProcessingSettings AIProcessingDialog::settings() const
{
    AIProcessingSettings s;
    s.upscaleEnabled     = m_upscaleEnabledCheck->isChecked();
    s.upscaleEngine      = m_upscaleEngineCombo->currentText();
    s.upscaleFactor      = m_upscaleFactorSpin->value();
    s.frameInterpEnabled = m_frameInterpEnabledCheck->isChecked();
    s.frameInterpEngine  = m_frameInterpEngineCombo->currentText();
    s.frameInterpFactor  = m_frameInterpFactorSpin->value();
    return s;
}
