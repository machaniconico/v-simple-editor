#include "MotionTrackerDialog.h"

#include "TrackerPresetRegistry.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
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

    m_descriptionLabel = new QLabel(this);
    m_descriptionLabel->setWordWrap(true);
    m_descriptionLabel->setMinimumHeight(40);
    {
        QPalette p = m_descriptionLabel->palette();
        p.setColor(QPalette::WindowText, p.color(QPalette::Disabled, QPalette::WindowText));
        m_descriptionLabel->setPalette(p);
    }
    m_descriptionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

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
    m_deletePresetBtn = new QPushButton(tr("選択中の preset を削除"), this);
    m_resetBtn = new QPushButton(tr("Reset to defaults"), this);
    m_exportBtn = new QPushButton(tr("Preset を JSON エクスポート"), this);
    m_importBtn = new QPushButton(tr("Preset を JSON インポート"), this);
    m_deletePresetBtn->setEnabled(false);
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
    root->addWidget(m_descriptionLabel);
    root->addLayout(form);
    auto* presetButtons = new QHBoxLayout;
    presetButtons->addWidget(m_saveCustomPresetButton);
    presetButtons->addWidget(m_deletePresetBtn);
    presetButtons->addWidget(m_resetBtn);
    presetButtons->addWidget(m_exportBtn);
    presetButtons->addWidget(m_importBtn);
    root->addLayout(presetButtons);
    root->addWidget(m_buttonBox);

    connect(m_presetCombo, &QComboBox::currentIndexChanged,
            this, &MotionTrackerDialog::onPresetSelectionChanged);
    connect(m_saveCustomPresetButton, &QPushButton::clicked,
            this, &MotionTrackerDialog::onSaveCustomPreset);
    connect(m_deletePresetBtn, &QPushButton::clicked,
            this, &MotionTrackerDialog::onDeleteSelectedPreset);
    connect(m_resetBtn, &QPushButton::clicked,
            this, &MotionTrackerDialog::onResetToDefaults);
    connect(m_exportBtn, &QPushButton::clicked,
            this, &MotionTrackerDialog::onExportPreset);
    connect(m_importBtn, &QPushButton::clicked,
            this, &MotionTrackerDialog::onImportPreset);
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
    updateDeletePresetButton();

    bool ok = false;
    const int presetIndex = m_presetCombo->itemData(index).toInt(&ok);
    if (!ok || presetIndex < 0 || presetIndex >= m_presets.size()) {
        if (m_descriptionLabel)
            m_descriptionLabel->setText(tr("説明: なし"));
        return;
    }
    const tracker_preset::TrackerPreset& preset = m_presets.at(presetIndex);
    if (m_descriptionLabel) {
        m_descriptionLabel->setText(
            preset.description.isEmpty() ? tr("説明: なし") : preset.description);
    }
    applyPresetToWidgets(preset);
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

void MotionTrackerDialog::onDeleteSelectedPreset()
{
    const int index = currentPresetIndex();
    if (index < 0)
        return;

    const tracker_preset::TrackerPreset preset = m_presets.at(index);
    if (tracker_preset::findBuiltin(preset.id).has_value())
        return;

    const QMessageBox::StandardButton answer =
        QMessageBox::question(this,
                              tr("選択中の preset を削除"),
                              tr("「%1」を削除しますか?").arg(preset.displayName),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    if (tracker_preset::Registry::instance().removeUserPreset(preset.id))
        rebuildPresetCombo();
}

void MotionTrackerDialog::onResetToDefaults()
{
    if (m_presetCombo->count() <= 0)
        return;

    int defaultComboIndex = -1;
    for (int comboIndex = 0; comboIndex < m_presetCombo->count(); ++comboIndex) {
        bool ok = false;
        const int presetIndex = m_presetCombo->itemData(comboIndex).toInt(&ok);
        if (ok && presetIndex >= 0 && presetIndex < m_presets.size()
            && m_presets.at(presetIndex).id == QStringLiteral("slow-pan-static-bg")) {
            defaultComboIndex = comboIndex;
            break;
        }
    }

    if (defaultComboIndex < 0)
        defaultComboIndex = 0;

    if (m_presetCombo->currentIndex() == defaultComboIndex)
        onPresetSelectionChanged(defaultComboIndex);
    else
        m_presetCombo->setCurrentIndex(defaultComboIndex);
}

void MotionTrackerDialog::onExportPreset()
{
    QString fileName = QFileDialog::getSaveFileName(this,
                                                    tr("Preset を JSON エクスポート"),
                                                    QString(),
                                                    tr("Tracker Preset JSON (*.json)"));
    if (fileName.isEmpty())
        return;
    if (!fileName.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        fileName.append(QStringLiteral(".json"));

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this,
                             tr("Preset を JSON エクスポート"),
                             tr("JSON ファイルを書き込めませんでした。"));
        return;
    }

    const QJsonObject obj = tracker_preset::toJson(selectedPreset());
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        QMessageBox::warning(this,
                             tr("Preset を JSON エクスポート"),
                             tr("JSON ファイルを書き込めませんでした。"));
    }
}

void MotionTrackerDialog::onImportPreset()
{
    const QString fileName = QFileDialog::getOpenFileName(this,
                                                          tr("Preset を JSON インポート"),
                                                          QString(),
                                                          tr("Tracker Preset JSON (*.json)"));
    if (fileName.isEmpty())
        return;

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this,
                             tr("Preset を JSON インポート"),
                             tr("JSON ファイルを読み込めませんでした。"));
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(this,
                             tr("Preset を JSON インポート"),
                             tr("JSON が不正です"));
        return;
    }

    auto imported = tracker_preset::fromJson(doc.object());
    if (!imported) {
        QMessageBox::warning(this,
                             tr("Preset を JSON インポート"),
                             tr("JSON が不正です"));
        return;
    }

    imported->id = makeUserPresetId(imported->displayName);
    if (!tracker_preset::Registry::instance().saveUserPreset(*imported)) {
        QMessageBox::warning(this,
                             tr("Preset を JSON インポート"),
                             tr("Preset を保存できませんでした。"));
        return;
    }

    rebuildPresetCombo(imported->id);
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

    if (selectedIndex >= 0) {
        const tracker_preset::TrackerPreset& sel = m_presets.at(selectedIndex);
        applyPresetToWidgets(sel);
        if (m_descriptionLabel) {
            m_descriptionLabel->setText(
                sel.description.isEmpty() ? tr("説明: なし") : sel.description);
        }
    } else {
        if (m_descriptionLabel)
            m_descriptionLabel->setText(tr("説明: なし"));
    }
    updateDeletePresetButton();
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

void MotionTrackerDialog::updateDeletePresetButton()
{
    if (!m_deletePresetBtn)
        return;

    const int index = currentPresetIndex();
    if (index < 0) {
        m_deletePresetBtn->setEnabled(false);
        return;
    }

    m_deletePresetBtn->setEnabled(!tracker_preset::findBuiltin(m_presets.at(index).id).has_value());
}

int MotionTrackerDialog::currentPresetIndex() const
{
    bool ok = false;
    const int index = m_presetCombo->currentData().toInt(&ok);
    if (!ok || index < 0 || index >= m_presets.size())
        return -1;
    return index;
}
