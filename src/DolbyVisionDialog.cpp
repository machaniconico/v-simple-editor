#include "DolbyVisionDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

DolbyVisionDialog::DolbyVisionDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Dolby Vision メタデータ"));

    auto *rootLayout = new QVBoxLayout(this);

    // --- プロファイル / タイトル ---
    auto *headerGroup = new QGroupBox(QStringLiteral("シーケンス"), this);
    auto *headerForm = new QFormLayout(headerGroup);

    m_profileCombo = new QComboBox(headerGroup);
    m_profileCombo->addItem(QStringLiteral("プロファイル 5 (single-layer)"), 5);
    m_profileCombo->addItem(QStringLiteral("プロファイル 8.1 (HDR10 互換)"), 81);

    m_titleEdit = new QLineEdit(headerGroup);

    headerForm->addRow(QStringLiteral("プロファイル"), m_profileCombo);
    headerForm->addRow(QStringLiteral("タイトル"),     m_titleEdit);
    rootLayout->addWidget(headerGroup);

    // --- Level6 (CLL/FALL + マスタリングディスプレイ輝度) ---
    auto *l6Group = new QGroupBox(
        QStringLiteral("Level6 (CLL/FALL・マスタリングディスプレイ)"), this);
    auto *l6Form = new QFormLayout(l6Group);

    m_maxCllSpin = new QSpinBox(l6Group);
    m_maxCllSpin->setRange(0, 10000);
    m_maxCllSpin->setSuffix(QStringLiteral(" nits"));

    m_maxFallSpin = new QSpinBox(l6Group);
    m_maxFallSpin->setRange(0, 10000);
    m_maxFallSpin->setSuffix(QStringLiteral(" nits"));

    m_masteringMaxSpin = new QSpinBox(l6Group);
    m_masteringMaxSpin->setRange(0, 10000);
    m_masteringMaxSpin->setSuffix(QStringLiteral(" nits"));

    m_masteringMinSpin = new QSpinBox(l6Group);
    m_masteringMinSpin->setRange(0, 10000);
    m_masteringMinSpin->setSuffix(QStringLiteral(" nits"));

    l6Form->addRow(QStringLiteral("MaxCLL"),                 m_maxCllSpin);
    l6Form->addRow(QStringLiteral("MaxFALL"),                m_maxFallSpin);
    l6Form->addRow(QStringLiteral("マスタリング最大輝度"),   m_masteringMaxSpin);
    l6Form->addRow(QStringLiteral("マスタリング最小輝度"),   m_masteringMinSpin);
    rootLayout->addWidget(l6Group);

    // --- 先頭ショットの Level1 輝度 (簡易編集) ---
    auto *shotGroup = new QGroupBox(
        QStringLiteral("先頭ショット Level1 輝度"), this);
    auto *shotForm = new QFormLayout(shotGroup);

    m_shotCountLabel = new QLabel(shotGroup);

    m_shotMinNitsSpin = new QDoubleSpinBox(shotGroup);
    m_shotMinNitsSpin->setRange(0.0, 10000.0);
    m_shotMinNitsSpin->setDecimals(4);
    m_shotMinNitsSpin->setSuffix(QStringLiteral(" nits"));

    m_shotAvgNitsSpin = new QDoubleSpinBox(shotGroup);
    m_shotAvgNitsSpin->setRange(0.0, 10000.0);
    m_shotAvgNitsSpin->setDecimals(4);
    m_shotAvgNitsSpin->setSuffix(QStringLiteral(" nits"));

    m_shotMaxNitsSpin = new QDoubleSpinBox(shotGroup);
    m_shotMaxNitsSpin->setRange(0.0, 10000.0);
    m_shotMaxNitsSpin->setDecimals(4);
    m_shotMaxNitsSpin->setSuffix(QStringLiteral(" nits"));

    shotForm->addRow(QStringLiteral("ショット数"),     m_shotCountLabel);
    shotForm->addRow(QStringLiteral("最小輝度 (min)"), m_shotMinNitsSpin);
    shotForm->addRow(QStringLiteral("平均輝度 (avg)"), m_shotAvgNitsSpin);
    shotForm->addRow(QStringLiteral("最大輝度 (max)"), m_shotMaxNitsSpin);
    rootLayout->addWidget(shotGroup);

    // --- XML エクスポート + OK / キャンセル ---
    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *exportButton = buttonBox->addButton(
        QStringLiteral("XML をエクスポート..."), QDialogButtonBox::ActionRole);
    rootLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(exportButton, &QPushButton::clicked,
            this, &DolbyVisionDialog::exportXmlRequested);

    // 既定値 (空メタ) を反映。
    setMetadata(m_base);
}

void DolbyVisionDialog::setMetadata(const dolbyvision::DolbyVisionMetadata &meta)
{
    m_base = meta;

    const int profileIdx = m_profileCombo->findData(meta.profile);
    m_profileCombo->setCurrentIndex(profileIdx >= 0 ? profileIdx : 0);

    m_titleEdit->setText(meta.title);

    m_maxCllSpin->setValue(meta.l6.maxCll);
    m_maxFallSpin->setValue(meta.l6.maxFall);
    m_masteringMaxSpin->setValue(meta.l6.masteringMaxNits);
    m_masteringMinSpin->setValue(meta.l6.masteringMinNits);

    m_shotCountLabel->setText(QString::number(meta.shots.size()));

    // 先頭ショットがあれば Level1 を編集可能にし、無ければ 0 / 無効化する。
    const bool hasShot = !meta.shots.isEmpty();
    m_shotMinNitsSpin->setEnabled(hasShot);
    m_shotAvgNitsSpin->setEnabled(hasShot);
    m_shotMaxNitsSpin->setEnabled(hasShot);
    if (hasShot) {
        const dolbyvision::L1Metadata &l1 = meta.shots.first().l1;
        m_shotMinNitsSpin->setValue(l1.minNits);
        m_shotAvgNitsSpin->setValue(l1.avgNits);
        m_shotMaxNitsSpin->setValue(l1.maxNits);
    } else {
        m_shotMinNitsSpin->setValue(0.0);
        m_shotAvgNitsSpin->setValue(0.0);
        m_shotMaxNitsSpin->setValue(0.0);
    }
}

dolbyvision::DolbyVisionMetadata DolbyVisionDialog::metadata() const
{
    // m_base を起点に UI 編集分のみ上書きし、非編集フィールド (Level2 trim /
    // Level5 / 2 ショット目以降) を非破壊に保持する。
    dolbyvision::DolbyVisionMetadata meta = m_base;

    bool ok = false;
    const int profile = m_profileCombo->currentData().toInt(&ok);
    meta.profile = ok ? profile : 81;

    meta.title = m_titleEdit->text();

    meta.l6.maxCll           = m_maxCllSpin->value();
    meta.l6.maxFall          = m_maxFallSpin->value();
    meta.l6.masteringMaxNits = m_masteringMaxSpin->value();
    meta.l6.masteringMinNits = m_masteringMinSpin->value();

    if (!meta.shots.isEmpty()) {
        dolbyvision::L1Metadata &l1 = meta.shots.first().l1;
        l1.minNits = m_shotMinNitsSpin->value();
        l1.avgNits = m_shotAvgNitsSpin->value();
        l1.maxNits = m_shotMaxNitsSpin->value();
    }

    return meta;
}
