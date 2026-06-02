#include "WhisperTranscribeDialog.h"

#include "SpeechRecognizer.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

WhisperTranscribeDialog::WhisperTranscribeDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("動画を文字起こし"));
    setMinimumWidth(480);

    // --- 入力ファイル ---
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(tr("動画 / 音声ファイルを選択してください"));

    m_browseButton = new QPushButton(tr("参照..."), this);

    auto* pathLayout = new QHBoxLayout;
    pathLayout->addWidget(m_pathEdit);
    pathLayout->addWidget(m_browseButton);

    // --- モデル (recognizer) ---
    m_modelCombo = new QComboBox(this);
    const auto recognizers = speech::availableRecognizers();
    for (const auto& r : recognizers) {
        if (r) {
            m_modelCombo->addItem(r->name());
        }
    }

    m_engineWarningLabel = new QLabel(
        QStringLiteral("外部エンジン(whisper-cli)が見つかりません。サンプル文字起こしになります。PATH に whisper-cli を配置してください。"),
        this);
    m_engineWarningLabel->setWordWrap(true);
    m_engineWarningLabel->setStyleSheet(QStringLiteral("color: #b00020; font-weight: 600;"));

    // --- 言語 ---
    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItem(tr("自動 (auto)"), QStringLiteral("auto"));
    m_languageCombo->addItem(tr("日本語 (ja)"), QStringLiteral("ja"));
    m_languageCombo->addItem(tr("英語 (en)"), QStringLiteral("en"));

    // --- フォーム ---
    auto* formLayout = new QFormLayout;
    formLayout->addRow(tr("入力ファイル:"), pathLayout);
    formLayout->addRow(tr("モデル:"), m_modelCombo);
    formLayout->addRow(tr("言語:"), m_languageCombo);

    // --- 結果表示 ---
    m_resultLabel = new QLabel(QString(), this);
    m_resultLabel->setWordWrap(true);

    // --- ボタン ---
    m_buttonBox = new QDialogButtonBox(this);
    auto* acceptButton = m_buttonBox->addButton(tr("文字起こし"), QDialogButtonBox::AcceptRole);
    m_buttonBox->addButton(tr("キャンセル"), QDialogButtonBox::RejectRole);
    Q_UNUSED(acceptButton);

    // --- レイアウト ---
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(formLayout);
    mainLayout->addWidget(m_engineWarningLabel);
    mainLayout->addWidget(m_resultLabel);
    mainLayout->addWidget(m_buttonBox);

    // --- 接続 ---
    connect(m_browseButton, &QPushButton::clicked, this, &WhisperTranscribeDialog::onBrowseClicked);
    connect(m_pathEdit, &QLineEdit::textChanged, this, &WhisperTranscribeDialog::updateAcceptState);
    connect(m_modelCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        updateRecognizerWarning();
    });
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateRecognizerWarning();
    updateAcceptState();
}

void WhisperTranscribeDialog::setMediaPath(const QString& path)
{
    m_pathEdit->setText(path);
}

void WhisperTranscribeDialog::setResultText(const QString& text)
{
    m_resultLabel->setText(text);
}

whisper::TranscribeRequest WhisperTranscribeDialog::request() const
{
    whisper::TranscribeRequest req;
    req.mediaPath      = m_pathEdit->text().trimmed();
    req.language       = m_languageCombo->currentData().toString();
    req.recognizerName = m_modelCombo->currentText();
    return req;
}

void WhisperTranscribeDialog::onBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("動画 / 音声ファイルを選択"),
        QString(),
        tr("メディアファイル (*.mp4 *.mov *.mkv *.avi *.webm *.wav *.mp3 *.m4a *.aac *.flac);;すべてのファイル (*.*)"));

    if (!path.isEmpty()) {
        m_pathEdit->setText(path);
    }
}

void WhisperTranscribeDialog::updateAcceptState()
{
    const bool hasPath = !m_pathEdit->text().trimmed().isEmpty();
    // Accept 系ボタンを mediaPath 有無で enable/disable
    const auto buttons = m_buttonBox->buttons();
    for (auto* b : buttons) {
        if (m_buttonBox->buttonRole(b) == QDialogButtonBox::AcceptRole) {
            b->setEnabled(hasPath);
        }
    }
}

void WhisperTranscribeDialog::updateRecognizerWarning()
{
    const QList<QSharedPointer<speech::Recognizer>> recognizers = speech::availableRecognizers();
    bool hasExternalRecognizer = false;
    for (const QSharedPointer<speech::Recognizer>& recognizer : recognizers) {
        if (recognizer
            && recognizer->isAvailable()
            && recognizer->name() != QStringLiteral("Stub")) {
            hasExternalRecognizer = true;
            break;
        }
    }

    const bool selectedStub = m_modelCombo->currentText() == QStringLiteral("Stub");
    m_engineWarningLabel->setVisible(selectedStub && !hasExternalRecognizer);
}
