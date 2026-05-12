#include "VfxControlsPanel.h"
#include <QFormLayout>
#include <QLabel>

VfxControlsPanel::VfxControlsPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(8);

    buildGlowSection();
    buildBloomSection();
    buildChromaticAberrationSection();
    buildLightWrapSection();

    mainLayout->addStretch();
}

// ── Glow ──────────────────────────────────────────────────────────────

void VfxControlsPanel::buildGlowSection()
{
    m_glowGroup = new QGroupBox(QStringLiteral("Glow"), this);
    auto *form = new QFormLayout(m_glowGroup);
    form->setContentsMargins(6, 6, 6, 6);

    m_glowEnable = new QCheckBox(QStringLiteral("Enable"), m_glowGroup);
    form->addRow(m_glowEnable);

    m_glowThreshold = new QDoubleSpinBox(m_glowGroup);
    m_glowThreshold->setRange(0.0, 1.0);
    m_glowThreshold->setSingleStep(0.05);
    m_glowThreshold->setValue(0.5);
    form->addRow(QStringLiteral("Threshold"), m_glowThreshold);

    m_glowRadius = new QDoubleSpinBox(m_glowGroup);
    m_glowRadius->setRange(0.0, 50.0);
    m_glowRadius->setSingleStep(1.0);
    m_glowRadius->setValue(10.0);
    form->addRow(QStringLiteral("Radius"), m_glowRadius);

    m_glowIntensity = new QDoubleSpinBox(m_glowGroup);
    m_glowIntensity->setRange(0.0, 3.0);
    m_glowIntensity->setSingleStep(0.1);
    m_glowIntensity->setValue(1.0);
    form->addRow(QStringLiteral("Intensity"), m_glowIntensity);

    auto wire = [this]() { emitGlow(); };
    connect(m_glowEnable, &QCheckBox::toggled, this, wire);
    connect(m_glowThreshold, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);
    connect(m_glowRadius, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);
    connect(m_glowIntensity, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);

    static_cast<QVBoxLayout*>(layout())->addWidget(m_glowGroup);
}

// ── Bloom ─────────────────────────────────────────────────────────────

void VfxControlsPanel::buildBloomSection()
{
    m_bloomGroup = new QGroupBox(QStringLiteral("Bloom"), this);
    auto *form = new QFormLayout(m_bloomGroup);
    form->setContentsMargins(6, 6, 6, 6);

    m_bloomEnable = new QCheckBox(QStringLiteral("Enable"), m_bloomGroup);
    form->addRow(m_bloomEnable);

    m_bloomThreshold = new QDoubleSpinBox(m_bloomGroup);
    m_bloomThreshold->setRange(0.0, 1.0);
    m_bloomThreshold->setSingleStep(0.05);
    m_bloomThreshold->setValue(0.5);
    form->addRow(QStringLiteral("Threshold"), m_bloomThreshold);

    m_bloomIntensity = new QDoubleSpinBox(m_bloomGroup);
    m_bloomIntensity->setRange(0.0, 3.0);
    m_bloomIntensity->setSingleStep(0.1);
    m_bloomIntensity->setValue(1.0);
    form->addRow(QStringLiteral("Intensity"), m_bloomIntensity);

    m_bloomSpread = new QDoubleSpinBox(m_bloomGroup);
    m_bloomSpread->setRange(0.0, 1.0);
    m_bloomSpread->setSingleStep(0.05);
    m_bloomSpread->setValue(0.3);
    form->addRow(QStringLiteral("Spread"), m_bloomSpread);

    auto wire = [this]() { emitBloom(); };
    connect(m_bloomEnable, &QCheckBox::toggled, this, wire);
    connect(m_bloomThreshold, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);
    connect(m_bloomIntensity, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);
    connect(m_bloomSpread, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);

    static_cast<QVBoxLayout*>(layout())->addWidget(m_bloomGroup);
}

// ── Chromatic Aberration ──────────────────────────────────────────────

void VfxControlsPanel::buildChromaticAberrationSection()
{
    m_chromaGroup = new QGroupBox(QStringLiteral("Chromatic Aberration"), this);
    auto *form = new QFormLayout(m_chromaGroup);
    form->setContentsMargins(6, 6, 6, 6);

    m_chromaEnable = new QCheckBox(QStringLiteral("Enable"), m_chromaGroup);
    form->addRow(m_chromaEnable);

    m_chromaAmount = new QDoubleSpinBox(m_chromaGroup);
    m_chromaAmount->setRange(0.0, 20.0);
    m_chromaAmount->setSingleStep(0.5);
    m_chromaAmount->setValue(5.0);
    form->addRow(QStringLiteral("Amount (px)"), m_chromaAmount);

    m_chromaFalloff = new QDoubleSpinBox(m_chromaGroup);
    m_chromaFalloff->setRange(1.0, 4.0);
    m_chromaFalloff->setSingleStep(0.25);
    m_chromaFalloff->setValue(2.0);
    form->addRow(QStringLiteral("Radial Falloff"), m_chromaFalloff);

    auto wire = [this]() { emitChromaticAberration(); };
    connect(m_chromaEnable, &QCheckBox::toggled, this, wire);
    connect(m_chromaAmount, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);
    connect(m_chromaFalloff, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);

    static_cast<QVBoxLayout*>(layout())->addWidget(m_chromaGroup);
}

// ── Light Wrap ────────────────────────────────────────────────────────

void VfxControlsPanel::buildLightWrapSection()
{
    m_lightWrapGroup = new QGroupBox(QStringLiteral("Light Wrap"), this);
    auto *form = new QFormLayout(m_lightWrapGroup);
    form->setContentsMargins(6, 6, 6, 6);

    m_lightWrapEnable = new QCheckBox(QStringLiteral("Enable"), m_lightWrapGroup);
    form->addRow(m_lightWrapEnable);

    m_lightWrapAmount = new QDoubleSpinBox(m_lightWrapGroup);
    m_lightWrapAmount->setRange(0.0, 1.0);
    m_lightWrapAmount->setSingleStep(0.05);
    m_lightWrapAmount->setValue(0.5);
    form->addRow(QStringLiteral("Amount"), m_lightWrapAmount);

    m_lightWrapRadius = new QDoubleSpinBox(m_lightWrapGroup);
    m_lightWrapRadius->setRange(0.0, 30.0);
    m_lightWrapRadius->setSingleStep(1.0);
    m_lightWrapRadius->setValue(10.0);
    form->addRow(QStringLiteral("Radius"), m_lightWrapRadius);

    auto wire = [this]() { emitLightWrap(); };
    connect(m_lightWrapEnable, &QCheckBox::toggled, this, wire);
    connect(m_lightWrapAmount, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);
    connect(m_lightWrapRadius, qOverload<double>(&QDoubleSpinBox::valueChanged), this, wire);

    static_cast<QVBoxLayout*>(layout())->addWidget(m_lightWrapGroup);
}

// ── Emit helpers ──────────────────────────────────────────────────────

void VfxControlsPanel::emitGlow()
{
    if (m_blocking) return;
    emit glowChanged(
        m_glowEnable->isChecked(),
        static_cast<float>(m_glowThreshold->value()),
        static_cast<float>(m_glowRadius->value()),
        static_cast<float>(m_glowIntensity->value()));
}

void VfxControlsPanel::emitBloom()
{
    if (m_blocking) return;
    emit bloomChanged(
        m_bloomEnable->isChecked(),
        static_cast<float>(m_bloomThreshold->value()),
        static_cast<float>(m_bloomIntensity->value()),
        static_cast<float>(m_bloomSpread->value()));
}

void VfxControlsPanel::emitChromaticAberration()
{
    if (m_blocking) return;
    emit chromaticAberrationChanged(
        m_chromaEnable->isChecked(),
        static_cast<float>(m_chromaAmount->value()),
        static_cast<float>(m_chromaFalloff->value()));
}

void VfxControlsPanel::emitLightWrap()
{
    if (m_blocking) return;
    emit lightWrapChanged(
        m_lightWrapEnable->isChecked(),
        static_cast<float>(m_lightWrapAmount->value()),
        static_cast<float>(m_lightWrapRadius->value()));
}

// ── Getters ───────────────────────────────────────────────────────────

GlowState VfxControlsPanel::glowState() const
{
    return {
        m_glowEnable->isChecked(),
        static_cast<float>(m_glowThreshold->value()),
        static_cast<float>(m_glowRadius->value()),
        static_cast<float>(m_glowIntensity->value()),
    };
}

BloomState VfxControlsPanel::bloomState() const
{
    return {
        m_bloomEnable->isChecked(),
        static_cast<float>(m_bloomThreshold->value()),
        static_cast<float>(m_bloomIntensity->value()),
        static_cast<float>(m_bloomSpread->value()),
    };
}

ChromaticAberrationState VfxControlsPanel::chromaticAberrationState() const
{
    return {
        m_chromaEnable->isChecked(),
        static_cast<float>(m_chromaAmount->value()),
        static_cast<float>(m_chromaFalloff->value()),
    };
}

LightWrapState VfxControlsPanel::lightWrapState() const
{
    return {
        m_lightWrapEnable->isChecked(),
        static_cast<float>(m_lightWrapAmount->value()),
        static_cast<float>(m_lightWrapRadius->value()),
    };
}

// ── blockAndReset ─────────────────────────────────────────────────────

void VfxControlsPanel::blockAndReset()
{
    m_blocking = true;

    m_glowEnable->setChecked(false);
    m_glowThreshold->setValue(0.5);
    m_glowRadius->setValue(10.0);
    m_glowIntensity->setValue(1.0);

    m_bloomEnable->setChecked(false);
    m_bloomThreshold->setValue(0.5);
    m_bloomIntensity->setValue(1.0);
    m_bloomSpread->setValue(0.3);

    m_chromaEnable->setChecked(false);
    m_chromaAmount->setValue(5.0);
    m_chromaFalloff->setValue(2.0);

    m_lightWrapEnable->setChecked(false);
    m_lightWrapAmount->setValue(0.5);
    m_lightWrapRadius->setValue(10.0);

    m_blocking = false;
}
