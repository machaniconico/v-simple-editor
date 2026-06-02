#include "SubtitleTranslatorDialog.h"
#include "SubtitleIO.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QMessageBox>
#include <QLabel>
#include <QLineEdit>

SubtitleTranslatorDialog::SubtitleTranslatorDialog(QWidget *parent)
    : QDialog(parent)
    , m_client(new subxlat::TranslatorClient(this))
{
    setWindowTitle(tr("Subtitle Translator"));
    setModal(false);
    resize(620, 480);

    // --- provider combo ---
    m_providerCombo = new QComboBox(this);
    m_providerCombo->addItem(tr("Stub (offline)"),  static_cast<int>(subxlat::Provider::Stub));
    m_providerCombo->addItem(tr("Google Translate"), static_cast<int>(subxlat::Provider::GoogleV2));
    m_providerCombo->addItem(tr("DeepL"),            static_cast<int>(subxlat::Provider::DeepL));

    const subxlat::TranslateConfig defaultCfg = subxlat::TranslateConfig::defaultConfig();

    // --- API key ---
    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setText(defaultCfg.apiKey);
    m_apiKeyEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);

    m_apiWarningLabel = new QLabel(
        QStringLiteral("翻訳APIキーが未設定です。[言語]を前置するスタブ動作になります。実翻訳には API キーと provider 設定が必要です。"),
        this);
    m_apiWarningLabel->setWordWrap(true);
    m_apiWarningLabel->setStyleSheet(QStringLiteral("color: #b00020; font-weight: 600;"));

    // --- target language combo ---
    m_targetLangCombo = new QComboBox(this);
    const QStringList langs = { "ja", "en", "es", "fr", "de", "zh", "ko" };
    for (const QString &l : langs)
        m_targetLangCombo->addItem(l, l);

    // --- preview ---
    m_preview = new QPlainTextEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setPlaceholderText(tr("Translated text will appear here..."));

    // --- buttons ---
    QPushButton *loadBtn      = new QPushButton(tr("Load SRT..."), this);
    QPushButton *translateBtn = new QPushButton(tr("Translate"), this);

    // --- layout ---
    QFormLayout *form = new QFormLayout;
    form->addRow(tr("Provider:"),     m_providerCombo);
    form->addRow(tr("API key:"),      m_apiKeyEdit);
    form->addRow(tr("Target lang:"),  m_targetLangCombo);

    QHBoxLayout *btnRow = new QHBoxLayout;
    btnRow->addWidget(loadBtn);
    btnRow->addWidget(translateBtn);
    btnRow->addStretch();

    QVBoxLayout *root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(m_apiWarningLabel);
    root->addLayout(btnRow);
    root->addWidget(new QLabel(tr("Preview:"), this));
    root->addWidget(m_preview);

    // --- connections ---
    connect(loadBtn,      &QPushButton::clicked, this, &SubtitleTranslatorDialog::onLoadSrtClicked);
    connect(translateBtn, &QPushButton::clicked, this, &SubtitleTranslatorDialog::onTranslateClicked);
    connect(m_providerCombo,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this,
            [this](int) {
                updateApiWarning();
            });
    connect(m_apiKeyEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        updateApiWarning();
    });

    connect(m_client, &subxlat::TranslatorClient::translateFinished,
            this, &SubtitleTranslatorDialog::onFinished);
    connect(m_client, &subxlat::TranslatorClient::translateFailed,
            this, &SubtitleTranslatorDialog::onFailed);

    updateApiWarning();
}

void SubtitleTranslatorDialog::onLoadSrtClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open SRT File"),
        QString(),
        tr("SRT subtitles (*.srt);;All files (*.*)"));

    if (path.isEmpty())
        return;

    const subtitle::ImportResult result = subtitle::importSrt(path);
    if (!result.success) {
        QMessageBox::warning(this, tr("Load Error"),
                             tr("Failed to load SRT: %1").arg(result.error));
        return;
    }

    m_track.clear();
    for (const caption::Clip &c : result.clips)
        m_track.addClip(c);

    m_preview->setPlainText(
        tr("Loaded %1 clip(s). Press Translate to start.").arg(m_track.clipCount()));
}

void SubtitleTranslatorDialog::onTranslateClicked()
{
    if (m_track.clipCount() == 0) {
        QMessageBox::information(this, tr("No subtitles"),
                                 tr("Please load an SRT file first."));
        return;
    }

    subxlat::TranslateConfig cfg = subxlat::TranslateConfig::defaultConfig();
    cfg.provider   = static_cast<subxlat::Provider>(m_providerCombo->currentData().toInt());
    cfg.apiKey     = m_apiKeyEdit->text();
    cfg.targetLang = m_targetLangCombo->currentData().toString();

    m_preview->setPlainText(tr("Translating..."));
    m_client->translateTrack(m_track, cfg);
}

void SubtitleTranslatorDialog::onFinished(const caption::Track &translated)
{
    QStringList lines;
    const int n = translated.clipCount();
    for (int i = 0; i < n; ++i) {
        const caption::Clip c = translated.clipAt(i);
        lines << QStringLiteral("%1 --> %2\n%3")
                     .arg(c.startMs)
                     .arg(c.endMs)
                     .arg(c.text);
    }
    m_preview->setPlainText(lines.join(QStringLiteral("\n\n")));
}

void SubtitleTranslatorDialog::onFailed(const QString &error)
{
    QMessageBox::critical(this, tr("Translation Failed"), error);
    m_preview->setPlainText(tr("Translation failed: %1").arg(error));
}

void SubtitleTranslatorDialog::updateApiWarning()
{
    const subxlat::Provider provider =
        static_cast<subxlat::Provider>(m_providerCombo->currentData().toInt());
    const bool isStubProvider = provider == subxlat::Provider::Stub;
    const bool hasApiKey = !m_apiKeyEdit->text().isEmpty();
    m_apiWarningLabel->setVisible(isStubProvider && !hasApiKey);
}
