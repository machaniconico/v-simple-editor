#include "BroadcastCaptionDialog.h"

#include <cmath>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

BroadcastCaptionDialog::BroadcastCaptionDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("放送用クローズドキャプション (CEA-608/708)"));

    auto *rootLayout = new QVBoxLayout(this);

    // --- 規格 / チャンネル / フレームレート ---
    auto *headerGroup = new QGroupBox(QStringLiteral("規格設定"), this);
    auto *headerForm = new QFormLayout(headerGroup);

    m_standardCombo = new QComboBox(headerGroup);
    m_standardCombo->addItem(QStringLiteral("CEA-608 (line-21)"),
                             QStringLiteral("CEA-608"));
    m_standardCombo->addItem(QStringLiteral("CEA-708 (DTVCC)"),
                             QStringLiteral("CEA-708"));

    m_channelSpin = new QSpinBox(headerGroup);
    m_channelSpin->setRange(1, 4);  // CC1..CC4
    m_channelSpin->setPrefix(QStringLiteral("CC"));

    m_frameRateCombo = new QComboBox(headerGroup);
    m_frameRateCombo->addItem(QStringLiteral("23.976 fps"), 23.976);
    m_frameRateCombo->addItem(QStringLiteral("24 fps"),     24.0);
    m_frameRateCombo->addItem(QStringLiteral("25 fps"),     25.0);
    m_frameRateCombo->addItem(QStringLiteral("29.97 fps (drop-frame)"), 29.97);
    m_frameRateCombo->addItem(QStringLiteral("30 fps"),     30.0);
    m_frameRateCombo->addItem(QStringLiteral("59.94 fps (drop-frame)"), 59.94);

    headerForm->addRow(QStringLiteral("規格"),           m_standardCombo);
    headerForm->addRow(QStringLiteral("CC チャンネル"),  m_channelSpin);
    headerForm->addRow(QStringLiteral("フレームレート"), m_frameRateCombo);
    rootLayout->addWidget(headerGroup);

    // --- 取り込み済み字幕 cue の状況 ---
    auto *cueGroup = new QGroupBox(QStringLiteral("字幕 cue"), this);
    auto *cueForm = new QFormLayout(cueGroup);

    m_cueCountLabel = new QLabel(cueGroup);

    cueForm->addRow(QStringLiteral("cue 数"), m_cueCountLabel);
    rootLayout->addWidget(cueGroup);

    // --- SCC エクスポート + OK / キャンセル ---
    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *exportButton = buttonBox->addButton(
        QStringLiteral("SCC をエクスポート..."), QDialogButtonBox::ActionRole);
    rootLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(exportButton, &QPushButton::clicked,
            this, &BroadcastCaptionDialog::exportSccRequested);

    // 既定値 (空ドキュメント) を反映。
    setDocument(m_base);
}

void BroadcastCaptionDialog::setDocument(const broadcastcc::BroadcastCaptionDoc &doc)
{
    m_base = doc;

    const int stdIdx = m_standardCombo->findData(doc.standard);
    m_standardCombo->setCurrentIndex(stdIdx >= 0 ? stdIdx : 0);

    m_channelSpin->setValue(doc.channel);

    // フレームレートは近傍一致で選択 (浮動小数の厳密一致を避ける)。一致が無ければ
    // 既定の 29.97 (drop-frame) 行を選ぶ。
    int fpsIdx = -1;
    for (int i = 0; i < m_frameRateCombo->count(); ++i) {
        const double v = m_frameRateCombo->itemData(i).toDouble();
        if (std::abs(v - doc.frameRate) < 0.01) {
            fpsIdx = i;
            break;
        }
    }
    if (fpsIdx < 0)
        fpsIdx = m_frameRateCombo->findData(29.97);
    m_frameRateCombo->setCurrentIndex(fpsIdx >= 0 ? fpsIdx : 0);

    m_cueCountLabel->setText(QString::number(doc.cues.size()));
}

broadcastcc::BroadcastCaptionDoc BroadcastCaptionDialog::document() const
{
    // m_base を起点に UI 編集分のみ上書きし、非編集フィールド (cue 本体) を
    // 非破壊に保持する。
    broadcastcc::BroadcastCaptionDoc doc = m_base;

    const QVariant stdData = m_standardCombo->currentData();
    doc.standard = stdData.isValid() ? stdData.toString()
                                     : QStringLiteral("CEA-608");

    doc.channel = m_channelSpin->value();

    bool ok = false;
    const double fps = m_frameRateCombo->currentData().toDouble(&ok);
    doc.frameRate = ok ? fps : 29.97;

    return doc;
}
