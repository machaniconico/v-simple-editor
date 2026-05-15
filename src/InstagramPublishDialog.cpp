#include "InstagramPublishDialog.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

InstagramPublishDialog::InstagramPublishDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Publish to Instagram"));
    setModal(false);

    // --- form widgets ---
    m_videoUrlEdit = new QLineEdit(this);
    m_videoUrlEdit->setPlaceholderText(QStringLiteral("https://example.com/video.mp4"));

    m_captionEdit = new QPlainTextEdit(this);
    m_captionEdit->setPlaceholderText(QStringLiteral("Enter caption…"));
    m_captionEdit->setFixedHeight(80);

    m_shareToFeed = new QCheckBox(QStringLiteral("Share to feed"), this);
    m_shareToFeed->setChecked(true);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Video URL:"), m_videoUrlEdit);
    form->addRow(QStringLiteral("Caption:"),   m_captionEdit);
    form->addRow(QString(),                    m_shareToFeed);

    // --- publish button ---
    auto *publishBtn = new QPushButton(QStringLiteral("Publish"), this);

    // --- progress / status ---
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setVisible(false);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    // --- layout ---
    auto *root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(publishBtn);
    root->addWidget(m_progress);
    root->addWidget(m_status);
    setLayout(root);

    // --- publisher ---
    m_publisher = new instagram::publish::Publisher(this);
    connect(m_publisher, &instagram::publish::Publisher::publishProgress,
            this, &InstagramPublishDialog::onProgress);
    connect(m_publisher, &instagram::publish::Publisher::publishFinished,
            this, &InstagramPublishDialog::onFinished);
    connect(m_publisher, &instagram::publish::Publisher::publishFailed,
            this, &InstagramPublishDialog::onFailed);

    connect(publishBtn, &QPushButton::clicked,
            this, &InstagramPublishDialog::onPublishClicked);
}

void InstagramPublishDialog::onPublishClicked() {
    m_status->clear();
    m_progress->setValue(0);
    m_progress->setVisible(true);

    instagram::publish::PublishJob job;
    job.videoUrl    = m_videoUrlEdit->text().trimmed();
    job.caption     = m_captionEdit->toPlainText();
    job.shareToFeed = m_shareToFeed->isChecked();

    m_publisher->publish(job, instagram::publish::IgConfig::defaultConfig());
}

void InstagramPublishDialog::onProgress(int percent) {
    m_progress->setValue(percent);
}

void InstagramPublishDialog::onFinished(const QString &mediaId) {
    m_progress->setValue(100);
    m_status->setText(QStringLiteral("Published successfully. Media ID: ") + mediaId);
}

void InstagramPublishDialog::onFailed(const QString &error) {
    m_progress->setVisible(false);
    m_status->setText(QStringLiteral("Error: ") + error);
}
