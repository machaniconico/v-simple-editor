#pragma once

#include <QDialog>

#include "XVideoUpload.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class XVideoDialog : public QDialog {
    Q_OBJECT
public:
    explicit XVideoDialog(QWidget *parent = nullptr);
    ~XVideoDialog() override;

private slots:
    void onBrowseClicked();
    void onPostClicked();
    void onProgress(qint64 sent, qint64 total);
    void onFinished(const QString &tweetId);
    void onFailed(const QString &error);

private:
    x::upload::UploadClient *m_client;
    QLineEdit               *m_fileEdit;
    QPlainTextEdit          *m_tweetEdit;
    QProgressBar            *m_progress;
    QLabel                  *m_status;
    QPushButton             *m_postBtn;
};
