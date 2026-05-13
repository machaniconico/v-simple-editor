#include "HDRSettingsDialog.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>

HDRSettingsDialog::HDRSettingsDialog(const HDRSettings& initial, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("HDR Settings"));

    // Mode combo
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(tr("SDR"),   QString("sdr"));
    m_modeCombo->addItem(tr("HDR10"), QString("hdr10"));
    m_modeCombo->addItem(tr("HLG"),   QString("hlg"));
    {
        int idx = m_modeCombo->findData(initial.mode);
        if (idx >= 0) m_modeCombo->setCurrentIndex(idx);
    }

    // Max luminance spin
    m_maxLumSpin = new QDoubleSpinBox(this);
    m_maxLumSpin->setRange(100.0, 10000.0);
    m_maxLumSpin->setDecimals(1);
    m_maxLumSpin->setSuffix(tr(" cd/m²"));
    m_maxLumSpin->setValue(initial.masterDisplayLuminanceMax);

    // Min luminance spin
    m_minLumSpin = new QDoubleSpinBox(this);
    m_minLumSpin->setRange(0.001, 1.0);
    m_minLumSpin->setDecimals(4);
    m_minLumSpin->setSuffix(tr(" cd/m²"));
    m_minLumSpin->setValue(initial.masterDisplayLuminanceMin);

    // MaxCLL spin
    m_maxCllSpin = new QSpinBox(this);
    m_maxCllSpin->setRange(0, 10000);
    m_maxCllSpin->setSuffix(tr(" cd/m²"));
    m_maxCllSpin->setValue(initial.maxCll);

    // MaxFALL spin
    m_maxFallSpin = new QSpinBox(this);
    m_maxFallSpin->setRange(0, 10000);
    m_maxFallSpin->setSuffix(tr(" cd/m²"));
    m_maxFallSpin->setValue(initial.maxFall);

    // Tone map combo
    m_toneMapCombo = new QComboBox(this);
    m_toneMapCombo->addItem(tr("Reinhard"), QString("reinhard"));
    m_toneMapCombo->addItem(tr("Hable"),    QString("hable"));
    m_toneMapCombo->addItem(tr("None"),     QString("none"));
    {
        int idx = m_toneMapCombo->findData(initial.previewToneMap);
        if (idx >= 0) m_toneMapCombo->setCurrentIndex(idx);
    }

    // Button box
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Layout
    auto* form = new QFormLayout;
    form->addRow(tr("Mode:"),                 m_modeCombo);
    form->addRow(tr("Max Luminance:"),        m_maxLumSpin);
    form->addRow(tr("Min Luminance:"),        m_minLumSpin);
    form->addRow(tr("MaxCLL:"),               m_maxCllSpin);
    form->addRow(tr("MaxFALL:"),              m_maxFallSpin);
    form->addRow(tr("Preview Tone Map:"),     m_toneMapCombo);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(m_buttonBox);
    setLayout(root);
}

HDRSettings HDRSettingsDialog::settings() const
{
    HDRSettings s;
    s.mode                      = m_modeCombo->currentData().toString();
    s.masterDisplayLuminanceMax = m_maxLumSpin->value();
    s.masterDisplayLuminanceMin = m_minLumSpin->value();
    s.maxCll                    = m_maxCllSpin->value();
    s.maxFall                   = m_maxFallSpin->value();
    s.previewToneMap            = m_toneMapCombo->currentData().toString();
    return s;
}
