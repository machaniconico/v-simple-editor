#include "TwitchStreamDialog.h"
#include "TwitchStreamConfig.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// ctor
// ---------------------------------------------------------------------------

TwitchStreamDialog::TwitchStreamDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Twitch ライブ配信設定"));
    setModal(false);
    setMinimumWidth(560);

    // ---- form ---------------------------------------------------------------
    auto *formLayout = new QFormLayout;
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // Stream key (password echo)
    m_streamKeyEdit = new QLineEdit;
    m_streamKeyEdit->setEchoMode(QLineEdit::Password);
    m_streamKeyEdit->setPlaceholderText(tr("live_XXXXXXXXXXXX"));
    formLayout->addRow(tr("ストリームキー:"), m_streamKeyEdit);

    // Server
    m_serverCombo = new QComboBox;
    m_serverCombo->addItem(tr("US West  (live-sjc)"));
    m_serverCombo->addItem(tr("US East  (live-iad)"));
    m_serverCombo->addItem(tr("EU       (live-fra)"));
    m_serverCombo->addItem(tr("Asia     (live-tyo)"));
    m_serverCombo->addItem(tr("Auto     (live)"));
    m_serverCombo->setCurrentIndex(4); // Auto
    formLayout->addRow(tr("サーバー:"), m_serverCombo);

    // Bitrate
    m_bitrateSpin = new QSpinBox;
    m_bitrateSpin->setRange(1000, 15000);
    m_bitrateSpin->setValue(6000);
    m_bitrateSpin->setSuffix(tr(" kbps"));
    formLayout->addRow(tr("ビットレート:"), m_bitrateSpin);

    // FPS
    m_fpsSpin = new QSpinBox;
    m_fpsSpin->setRange(24, 120);
    m_fpsSpin->setValue(60);
    m_fpsSpin->setSuffix(tr(" fps"));
    formLayout->addRow(tr("フレームレート:"), m_fpsSpin);

    // Save key checkbox
    m_saveKeyCheck = new QCheckBox(tr("ストリームキーを保存する"));
    formLayout->addRow(QString(), m_saveKeyCheck);

    // ---- buttons ------------------------------------------------------------
    auto *generateBtn = new QPushButton(tr("生成"));
    auto *copyBtn     = new QPushButton(tr("クリップボードにコピー"));

    auto *btnLayout = new QHBoxLayout;
    btnLayout->addWidget(generateBtn);
    btnLayout->addWidget(copyBtn);
    btnLayout->addStretch();

    // ---- command preview ----------------------------------------------------
    m_commandView = new QPlainTextEdit;
    m_commandView->setReadOnly(true);
    m_commandView->setPlaceholderText(tr("「生成」を押すと ffmpeg コマンドが表示されます。"));
    m_commandView->setMinimumHeight(80);

    // ---- assemble -----------------------------------------------------------
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(formLayout);
    mainLayout->addLayout(btnLayout);
    mainLayout->addWidget(m_commandView);

    // ---- connections --------------------------------------------------------
    connect(generateBtn, &QPushButton::clicked, this, &TwitchStreamDialog::onGenerateClicked);
    connect(copyBtn,     &QPushButton::clicked, this, &TwitchStreamDialog::onCopyClicked);

    // ---- restore QSettings --------------------------------------------------
    QSettings settings;
    const QString savedKey = settings.value(QStringLiteral("twitch/stream_key")).toString();
    if (!savedKey.isEmpty())
        m_streamKeyEdit->setText(savedKey);

    const int savedServer = settings.value(QStringLiteral("twitch/server"), 4).toInt();
    if (savedServer >= 0 && savedServer < m_serverCombo->count())
        m_serverCombo->setCurrentIndex(savedServer);
}

// ---------------------------------------------------------------------------
// slots
// ---------------------------------------------------------------------------

void TwitchStreamDialog::onGenerateClicked()
{
    twitch::stream::StreamConfig cfg;
    cfg.streamKey    = m_streamKeyEdit->text().trimmed();
    cfg.server       = static_cast<twitch::stream::StreamServer>(m_serverCombo->currentIndex());
    cfg.bitrate      = m_bitrateSpin->value();
    cfg.framerate    = m_fpsSpin->value();
    // audioBitrate and resolution keep defaults (160 kbps, 1920x1080)

    const QStringList args = twitch::stream::buildFfmpegCommand(cfg, QStringLiteral("<input>"));
    m_commandView->setPlainText(args.join(QLatin1Char(' ')));

    if (m_saveKeyCheck->isChecked()) {
        QSettings settings;
        settings.setValue(QStringLiteral("twitch/stream_key"), m_streamKeyEdit->text());
        settings.setValue(QStringLiteral("twitch/server"), m_serverCombo->currentIndex());
    }
}

void TwitchStreamDialog::onCopyClicked()
{
    QApplication::clipboard()->setText(m_commandView->toPlainText());
}
