#include "TimecodeBurnInDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

TimecodeBurnInDialog::TimecodeBurnInDialog(
    const TimecodeBurnInSettings &initial,
    QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("タイムコード焼き込み設定"));
    setMinimumWidth(420);

    m_enabledCheck = new QCheckBox(tr("タイムコードを焼き込む"), this);
    m_enabledCheck->setChecked(initial.enabled);

    m_positionCombo = new QComboBox(this);
    m_positionCombo->addItem(tr("左上"), QStringLiteral("topLeft"));
    m_positionCombo->addItem(tr("上中央"), QStringLiteral("topCenter"));
    m_positionCombo->addItem(tr("右上"), QStringLiteral("topRight"));
    m_positionCombo->addItem(tr("左下"), QStringLiteral("bottomLeft"));
    m_positionCombo->addItem(tr("下中央"), QStringLiteral("bottomCenter"));
    m_positionCombo->addItem(tr("右下"), QStringLiteral("bottomRight"));
    const int positionIndex = m_positionCombo->findData(
        TimecodeBurnInSettings::positionName(initial.position));
    if (positionIndex >= 0)
        m_positionCombo->setCurrentIndex(positionIndex);

    m_fontSizeSpin = new QSpinBox(this);
    m_fontSizeSpin->setRange(1, 20);
    m_fontSizeSpin->setSuffix(tr(" %"));
    m_fontSizeSpin->setValue(initial.fontSizePct);

    m_showFramesCheck = new QCheckBox(tr("フレーム番号を表示"), this);
    m_showFramesCheck->setChecked(initial.showFrames);

    m_dropFrameCheck = new QCheckBox(tr("ドロップフレーム"), this);
    m_dropFrameCheck->setChecked(initial.dropFrame);

    m_prefixEdit = new QLineEdit(initial.prefix, this);
    m_prefixEdit->setClearButtonEnabled(true);

    m_showClipNameCheck = new QCheckBox(tr("クリップ名を表示"), this);
    m_showClipNameCheck->setChecked(initial.showClipName);

    m_showDateCheck = new QCheckBox(tr("日付を表示"), this);
    m_showDateCheck->setChecked(initial.showDate);

    m_opacitySpin = new QDoubleSpinBox(this);
    m_opacitySpin->setRange(0.0, 100.0);
    m_opacitySpin->setDecimals(0);
    m_opacitySpin->setSuffix(tr(" %"));
    m_opacitySpin->setValue(initial.opacity * 100.0);

    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttonBox, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    auto *form = new QFormLayout;
    form->addRow(QString(), m_enabledCheck);
    form->addRow(tr("位置:"), m_positionCombo);
    form->addRow(tr("文字サイズ (フレーム高):"), m_fontSizeSpin);
    form->addRow(QString(), m_showFramesCheck);
    form->addRow(QString(), m_dropFrameCheck);
    form->addRow(tr("接頭辞:"), m_prefixEdit);
    form->addRow(QString(), m_showClipNameCheck);
    form->addRow(QString(), m_showDateCheck);
    form->addRow(tr("不透明度:"), m_opacitySpin);

    auto *root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(m_buttonBox);

    const auto updateEnabledState = [this]() {
        const bool enabled = m_enabledCheck->isChecked();
        m_positionCombo->setEnabled(enabled);
        m_fontSizeSpin->setEnabled(enabled);
        m_showFramesCheck->setEnabled(enabled);
        m_dropFrameCheck->setEnabled(enabled);
        m_prefixEdit->setEnabled(enabled);
        m_showClipNameCheck->setEnabled(enabled);
        m_showDateCheck->setEnabled(enabled);
        m_opacitySpin->setEnabled(enabled);
    };
    connect(m_enabledCheck, &QCheckBox::toggled,
            this, [updateEnabledState](bool) { updateEnabledState(); });
    updateEnabledState();
}

TimecodeBurnInSettings TimecodeBurnInDialog::settings() const
{
    TimecodeBurnInSettings result;
    result.enabled = m_enabledCheck->isChecked();
    TimecodeBurnInSettings::Position position = result.position;
    if (TimecodeBurnInSettings::positionFromName(
            m_positionCombo->currentData().toString(), &position)) {
        result.position = position;
    }
    result.fontSizePct = m_fontSizeSpin->value();
    result.showFrames = m_showFramesCheck->isChecked();
    result.dropFrame = m_dropFrameCheck->isChecked();
    result.prefix = m_prefixEdit->text();
    result.showClipName = m_showClipNameCheck->isChecked();
    result.showDate = m_showDateCheck->isChecked();
    result.opacity = m_opacitySpin->value() / 100.0;
    return result;
}
