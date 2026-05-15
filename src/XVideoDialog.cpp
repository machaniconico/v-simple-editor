#include "XVideoDialog.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

XVideoDialog::XVideoDialog(QWidget *parent)
    : QDialog(parent)
    , m_client(new x::upload::UploadClient(this))
    , m_fileEdit(new QLineEdit(this))
    , m_tweetEdit(new QPlainTextEdit(this))
    , m_progress(new QProgressBar(this))
    , m_status(new QLabel(this))
    , m_postBtn(new QPushButton(tr("Post to X"), this))
{
    setWindowTitle(tr("Post Video to X (Twitter)"));
    setModal(false);

    // ---- file row ----
    m_fileEdit->setReadOnly(true);
    m_fileEdit->setPlaceholderText(tr("Select a video file..."));

    QPushButton *browseBtn = new QPushButton(tr("Browse..."), this);
    connect(browseBtn, &QPushButton::clicked, this, &XVideoDialog::onBrowseClicked);

    QHBoxLayout *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_fileEdit, 1);
    fileRow->addWidget(browseBtn);

    // ---- tweet text ----
    m_tweetEdit->setPlaceholderText(tr("Tweet text (max 280 characters)..."));
    m_tweetEdit->setMaximumBlockCount(1);   // single paragraph
    // enforce 280-char limit via a simple character check in onPostClicked

    // ---- progress ----
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setVisible(false);

    // ---- status ----
    m_status->setText(QString());
    m_status->setWordWrap(true);

    // ---- form layout ----
    QFormLayout *form = new QFormLayout;
    form->addRow(tr("Video file:"), fileRow);
    form->addRow(tr("Tweet text:"), m_tweetEdit);

    // ---- main layout ----
    QVBoxLayout *main = new QVBoxLayout(this);
    main->addLayout(form);
    main->addWidget(m_progress);
    main->addWidget(m_status);
    main->addWidget(m_postBtn);

    // ---- connections ----
    connect(m_postBtn, &QPushButton::clicked,
            this, &XVideoDialog::onPostClicked);
    connect(m_client, &x::upload::UploadClient::uploadProgress,
            this, &XVideoDialog::onProgress);
    connect(m_client, &x::upload::UploadClient::uploadFinished,
            this, &XVideoDialog::onFinished);
    connect(m_client, &x::upload::UploadClient::uploadFailed,
            this, &XVideoDialog::onFailed);

    resize(520, 260);
}

XVideoDialog::~XVideoDialog() = default;

void XVideoDialog::onBrowseClicked() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select Video File"),
        QString(),
        tr("Video files (*.mp4 *.mov);;All files (*)"));
    if (!path.isEmpty()) {
        m_fileEdit->setText(path);
    }
}

void XVideoDialog::onPostClicked() {
    const QString filePath  = m_fileEdit->text().trimmed();
    const QString tweetText = m_tweetEdit->toPlainText().trimmed();

    if (filePath.isEmpty()) {
        m_status->setText(tr("Please select a video file."));
        return;
    }
    if (tweetText.isEmpty()) {
        m_status->setText(tr("Please enter tweet text."));
        return;
    }
    if (tweetText.length() > 280) {
        m_status->setText(tr("Tweet text must be 280 characters or fewer."));
        return;
    }

    m_postBtn->setEnabled(false);
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_status->setText(tr("Uploading..."));

    x::upload::UploadJob job;
    job.filePath  = filePath;
    job.tweetText = tweetText;

    m_client->startUpload(job, x::upload::XUploadConfig::defaultConfig());
}

void XVideoDialog::onProgress(qint64 sent, qint64 total) {
    if (total > 0) {
        const int pct = static_cast<int>((sent * 100LL) / total);
        m_progress->setValue(pct);
    }
}

void XVideoDialog::onFinished(const QString &tweetId) {
    m_progress->setValue(100);
    m_status->setText(tr("Posted! Tweet ID: %1").arg(tweetId));
    m_postBtn->setEnabled(true);
}

void XVideoDialog::onFailed(const QString &error) {
    m_progress->setVisible(false);
    m_status->setText(tr("Upload failed: %1").arg(error));
    m_postBtn->setEnabled(true);
}
