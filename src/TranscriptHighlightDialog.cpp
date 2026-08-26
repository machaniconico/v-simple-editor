#include "TranscriptHighlightDialog.h"

#include "CredentialStore.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QVBoxLayout>

TranscriptHighlightDialog::TranscriptHighlightDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("文字起こしからハイライト検出"));
    setMinimumWidth(520);

    // --- プロバイダ ---
    m_providerCombo = new QComboBox(this);
    m_providerCombo->addItem(tr("Anthropic Claude (anthropic)"), QStringLiteral("anthropic"));
    m_providerCombo->addItem(tr("オフライン (ヒューリスティック)"), QStringLiteral("offline"));
    m_providerCombo->addItem(tr("OpenAI (openai)"), QStringLiteral("openai"));
    m_providerCombo->addItem(tr("Google Gemini (gemini)"), QStringLiteral("gemini"));

    // --- 抽出数 ---
    m_countSpin = new QSpinBox(this);
    m_countSpin->setRange(1, 50);
    m_countSpin->setValue(10);

    // --- 説明 ---
    m_descLabel = new QLabel(
        tr("現在の字幕トラックから AI が見どころを検出します。"
           "API キーは設定から登録してください。"),
        this);
    m_descLabel->setWordWrap(true);

    m_apiKeyWarningLabel = new QLabel(
        tr("ANTHROPIC_API_KEY が未設定です。オフライン検出(ヒューリスティック)に切り替えてください。"),
        this);
    m_apiKeyWarningLabel->setWordWrap(true);
    m_apiKeyWarningLabel->setVisible(false);

    // --- フォーム ---
    auto* formLayout = new QFormLayout;
    formLayout->addRow(tr("プロバイダ:"), m_providerCombo);
    formLayout->addRow(tr("抽出数:"), m_countSpin);

    // --- 結果表示 ---
    m_resultEdit = new QPlainTextEdit(this);
    m_resultEdit->setReadOnly(true);

    // --- ボタン ---
    m_buttonBox = new QDialogButtonBox(this);
    m_buttonBox->addButton(tr("検出"), QDialogButtonBox::AcceptRole);
    m_buttonBox->addButton(tr("閉じる"), QDialogButtonBox::RejectRole);

    // --- レイアウト ---
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_descLabel);
    mainLayout->addLayout(formLayout);
    mainLayout->addWidget(m_apiKeyWarningLabel);
    mainLayout->addWidget(m_resultEdit);
    mainLayout->addWidget(m_buttonBox);

    // --- 接続 ---
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_providerCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, [this](int) { updateDetectState(); });

    updateDetectState();
}

void TranscriptHighlightDialog::setTranscript(const QList<caption::Clip>& transcript)
{
    m_transcript = transcript;
    updateDetectState();
}

void TranscriptHighlightDialog::setResultText(const QString& text)
{
    m_resultEdit->setPlainText(text);
}

transcripthl::HighlightRequest TranscriptHighlightDialog::request() const
{
    transcripthl::HighlightRequest req;
    req.transcript  = m_transcript;
    req.targetCount = m_countSpin->value();
    req.provider    = m_providerCombo->currentData().toString();
    return req;
}

void TranscriptHighlightDialog::updateDetectState()
{
    const QString provider = m_providerCombo->currentData().toString();
    const char* envName = nullptr;
    QString settingsKey;
    if (provider.compare(QStringLiteral("anthropic"), Qt::CaseInsensitive) == 0) {
        envName = "ANTHROPIC_API_KEY";
        settingsKey = QStringLiteral("apiKeys/anthropic");
    } else if (provider.compare(QStringLiteral("openai"), Qt::CaseInsensitive) == 0) {
        envName = "OPENAI_API_KEY";
        settingsKey = QStringLiteral("apiKeys/openai");
    } else if (provider.compare(QStringLiteral("gemini"), Qt::CaseInsensitive) == 0) {
        envName = "GEMINI_API_KEY";
        settingsKey = QStringLiteral("apiKeys/gemini");
    }

    const bool needsApiKey = envName != nullptr;
    if (needsApiKey) {
        const QString apiKey = creds::CredentialStore::get(envName, settingsKey);
        m_apiKeyWarningLabel->setText(
            tr("%1 が未設定です。オフライン検出(ヒューリスティック)にフォールバックします。")
                .arg(QString::fromLatin1(envName)));
        m_apiKeyWarningLabel->setVisible(apiKey.trimmed().isEmpty());
    } else {
        m_apiKeyWarningLabel->setVisible(false);
    }

    // transcript が空のときは検出ボタンを disable
    const bool hasTranscript = !m_transcript.isEmpty();
    const auto buttons = m_buttonBox->buttons();
    for (auto* b : buttons) {
        if (m_buttonBox->buttonRole(b) == QDialogButtonBox::AcceptRole) {
            b->setEnabled(hasTranscript);
        }
    }
}
