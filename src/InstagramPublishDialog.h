#pragma once

#include "InstagramPublish.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;

class InstagramPublishDialog : public QDialog {
    Q_OBJECT
public:
    explicit InstagramPublishDialog(QWidget *parent = nullptr);

private slots:
    void onPublishClicked();
    void onProgress(int percent);
    void onFinished(const QString &mediaId);
    void onFailed(const QString &error);

private:
    instagram::publish::Publisher *m_publisher = nullptr;
    QLineEdit       *m_videoUrlEdit  = nullptr;
    QPlainTextEdit  *m_captionEdit   = nullptr;
    QCheckBox       *m_shareToFeed   = nullptr;
    QProgressBar    *m_progress      = nullptr;
    QLabel          *m_status        = nullptr;
};
