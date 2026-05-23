#include "MotionTrackerDialog.h"

#include "TrackerPresetRegistry.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHash>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

QString metricText(const QComboBox* combo)
{
    return combo ? combo->currentText().trimmed().toUpper() : QStringLiteral("NCC");
}

QString makeUserPresetId(const QString& name)
{
    QString slug;
    bool lastWasDash = false;
    const QString lower = name.trimmed().toLower();
    for (const QChar ch : lower) {
        const ushort u = ch.unicode();
        const bool isAsciiLetter = (u >= 'a' && u <= 'z');
        const bool isAsciiDigit = (u >= '0' && u <= '9');
        if (isAsciiLetter || isAsciiDigit) {
            slug.append(ch);
            lastWasDash = false;
        } else if (!lastWasDash) {
            slug.append(QLatin1Char('-'));
            lastWasDash = true;
        }
    }

    while (slug.startsWith(QLatin1Char('-')))
        slug.remove(0, 1);
    while (slug.endsWith(QLatin1Char('-')))
        slug.chop(1);
    if (slug.isEmpty())
        slug = QStringLiteral("preset");

    const QString hash = QString::number(static_cast<qulonglong>(qHash(name)), 16);
    return QStringLiteral("user-%1-%2").arg(slug, hash);
}

} // namespace

MotionTrackerDialog::MotionTrackerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("モーショントラッカー"));
    setObjectName(QStringLiteral("motionTrackerDialog"));

    tracker_preset::Registry::instance().reloadFromSettings();

    m_presetCombo = new QComboBox(this);

    m_searchRadiusSpin = new QSpinBox(this);
    m_searchRadiusSpin->setRange(0, 128);
    m_searchRadiusSpin->setSuffix(tr(" px"));

    m_matchMetricCombo = new QComboBox(this);
    m_matchMetricCombo->addItem(QStringLiteral("NCC"));
    m_matchMetricCombo->addItem(QStringLiteral("SSD"));
    m_matchMetricCombo->addItem(QStringLiteral("ZNCC"));

    m_kalmanEnabledCheck = new QCheckBox(this);

    m_kalmanProcessNoiseSpin = new QDoubleSpinBox(this);
    m_kalmanProcessNoiseSpin->setRange(0.0, 1.0);
    m_kalmanProcessNoiseSpin->setSingleStep(0.01);
    m_kalmanProcessNoiseSpin->setDecimals(3);

    m_kalmanMeasurementNoiseSpin = new QDoubleSpinBox(this);
    m_kalmanMeasurementNoiseSpin->setRange(0.0, 10.0);
    m_kalmanMeasurementNoiseSpin->setSingleStep(0.1);
    m_kalmanMeasurementNoiseSpin->setDecimals(3);

    m_occlusionGateSpin = new QDoubleSpinBox(this);
    m_occlusionGateSpin->setRange(0.0, 1.0);
    m_occlusionGateSpin->setSingleStep(0.05);
    m_occlusionGateSpin->setDecimals(3);

    m_subPixelEnabledCheck = new QCheckBox(this);

    m_minConfidenceSpin = new QDoubleSpinBox(this);
    m_minConfidenceSpin->setRange(0.0, 1.0);
    m_minConfidenceSpin->setSingleStep(0.05);
    m_minConfidenceSpin->setDecimals(3);

    m_saveCustomPresetButton = new QPushButton(tr("カスタム preset 保存"), this);
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto* form = new QFormLayout;
    form->addRow(tr("searchRadius"), m_searchRadiusSpin);
    form->addRow(tr("matchMetric"), m_matchMetricCombo);
    form->addRow(tr("kalmanEnabled"), m_kalmanEnabledCheck);
    form->addRow(tr("kalmanProcessNoise"), m_kalmanProcessNoiseSpin);
    form->addRow(tr("kalmanMeasurementNoise"), m_kalmanMeasurementNoiseSpin);
    form->addRow(tr("occlusionGate"), m_occlusionGateSpin);
    form->addRow(tr("subPixelEnabled"), m_subPixelEnabledCheck);
    form->addRow(tr("minConfidence"), m_minConfidenceSpin);

    auto* root = new QVBoxLayout(this);
    root->addWidget(m_presetCombo);
    root->addLayout(form);
    root->addWidget(m_saveCustomPresetButton);
    root->addWidget(m_buttonBox);

    connect(m_presetCombo, &QComboBox::currentIndexChanged,
            this, &MotionTrackerDialog::onPresetSelectionChanged);
    connect(m_saveCustomPresetButton, &QPushButton::clicked,
            this, &MotionTrackerDialog::onSaveCustomPreset);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &MotionTrackerDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &MotionTrackerDialog::reject);

    rebuildPresetCombo();
}

tracker_preset::TrackerPreset MotionTrackerDialog::selectedPreset() const
{
    tracker_preset::TrackerPreset preset;
    const int index = currentPresetIndex();
    if (index >= 0)
        preset = m_presets.at(index);
    else {
        preset.id = QStringLiteral("custom");
        preset.displayName = tr("Custom");
    }

    preset.searchRadius = m_searchRadiusSpin->value();
    preset.matchMetric = metricText(m_matchMetricCombo);
    preset.kalmanEnabled = m_kalmanEnabledCheck->isChecked();
    preset.kalmanProcessNoise = m_kalmanProcessNoiseSpin->value();
    preset.kalmanMeasurementNoise = m_kalmanMeasurementNoiseSpin->value();
    preset.occlusionGate = m_occlusionGateSpin->value();
    preset.subPixelEnabled = m_subPixelEnabledCheck->isChecked();
    preset.minConfidence = m_minConfidenceSpin->value();
    return preset;
}

void MotionTrackerDialog::accept()
{
    emit presetApplied(selectedPreset());
    QDialog::accept();
}

void MotionTrackerDialog::onPresetSelectionChanged(int index)
{
    bool ok = false;
    const int presetIndex = m_presetCombo->itemData(index).toInt(&ok);
    if (!ok || presetIndex < 0 || presetIndex >= m_presets.size())
        return;
    applyPresetToWidgets(m_presets.at(presetIndex));
}

void MotionTrackerDialog::onSaveCustomPreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               tr("カスタム preset 保存"),
                                               tr("名前:"),
                                               QLineEdit::Normal,
                                               QString(),
                                               &ok).trimmed();
    if (!ok || name.isEmpty())
        return;

    tracker_preset::TrackerPreset preset = selectedPreset();
    preset.id = makeUserPresetId(name);
    preset.displayName = name;

    if (tracker_preset::Registry::instance().saveUserPreset(preset))
        rebuildPresetCombo(preset.id);
}

void MotionTrackerDialog::rebuildPresetCombo(const QString& selectedId)
{
    m_presets = tracker_preset::Registry::instance().allPresets();
    std::sort(m_presets.begin(), m_presets.end(),
              [](const tracker_preset::TrackerPreset& lhs,
                 const tracker_preset::TrackerPreset& rhs) {
                  const int byName = QString::localeAwareCompare(lhs.displayName, rhs.displayName);
                  if (byName != 0)
                      return byName < 0;
                  return lhs.id < rhs.id;
              });

    const bool comboWasBlocked = m_presetCombo->blockSignals(true);
    m_presetCombo->clear();

    int selectedIndex = m_presets.isEmpty() ? -1 : 0;
    for (int i = 0; i < m_presets.size(); ++i) {
        const tracker_preset::TrackerPreset& preset = m_presets.at(i);
        m_presetCombo->addItem(preset.displayName, i);
        if (!selectedId.isEmpty() && preset.id == selectedId)
            selectedIndex = i;
    }

    m_presetCombo->setCurrentIndex(selectedIndex);
    m_presetCombo->blockSignals(comboWasBlocked);

    if (selectedIndex >= 0)
        applyPresetToWidgets(m_presets.at(selectedIndex));
}

void MotionTrackerDialog::applyPresetToWidgets(const tracker_preset::TrackerPreset& preset)
{
    setWidgetSignalsBlocked(true);

    m_searchRadiusSpin->setValue(preset.searchRadius);
    const int metricIndex = m_matchMetricCombo->findText(preset.matchMetric, Qt::MatchFixedString);
    m_matchMetricCombo->setCurrentIndex(metricIndex >= 0 ? metricIndex : 0);
    m_kalmanEnabledCheck->setChecked(preset.kalmanEnabled);
    m_kalmanProcessNoiseSpin->setValue(preset.kalmanProcessNoise);
    m_kalmanMeasurementNoiseSpin->setValue(preset.kalmanMeasurementNoise);
    m_occlusionGateSpin->setValue(preset.occlusionGate);
    m_subPixelEnabledCheck->setChecked(preset.subPixelEnabled);
    m_minConfidenceSpin->setValue(preset.minConfidence);

    setWidgetSignalsBlocked(false);
}

void MotionTrackerDialog::setWidgetSignalsBlocked(bool blocked)
{
    m_searchRadiusSpin->blockSignals(blocked);
    m_matchMetricCombo->blockSignals(blocked);
    m_kalmanEnabledCheck->blockSignals(blocked);
    m_kalmanProcessNoiseSpin->blockSignals(blocked);
    m_kalmanMeasurementNoiseSpin->blockSignals(blocked);
    m_occlusionGateSpin->blockSignals(blocked);
    m_subPixelEnabledCheck->blockSignals(blocked);
    m_minConfidenceSpin->blockSignals(blocked);
}

int MotionTrackerDialog::currentPresetIndex() const
{
    bool ok = false;
    const int index = m_presetCombo->currentData().toInt(&ok);
    if (!ok || index < 0 || index >= m_presets.size())
        return -1;
    return index;
}
