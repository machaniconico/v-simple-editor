#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include "VimeoOAuth.h"

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace vimeo {
namespace upload {

struct UploadJob {
    QString filePath;
    QString title;
    QString description;
    QString privacy = QStringLiteral("unlisted");
};

class UploadClient : public QObject {
    Q_OBJECT
public:
    explicit UploadClient(const vimeo::oauth::VimeoOAuthConfig& config =
                              vimeo::oauth::VimeoOAuthConfig::defaultConfig(),
                          QObject* parent = nullptr);
    ~UploadClient() override;

    void setAccessToken(const QString& accessToken);
    QString accessToken() const { return m_accessToken; }

    void startUpload(const UploadJob& job);
    void cancel();

signals:
    void uploadProgress(qint64 sent, qint64 total);
    void uploadFinished(const QString& videoUri);
    void uploadFailed(const QString& error);

private slots:
    void onCreateUploadFinished();
    void onChunkUploadProgress(qint64 sent, qint64 total);
    void onChunkReplyFinished();
    void onCompleteReplyFinished();

private:
    enum class RetryPhase {
        None,
        CreateUpload,
        UploadChunk,
        CompleteUpload
    };

    void resetJobState();
    void failUpload(const QString& error);
    void scheduleRetry(RetryPhase phase, const QString& errorContext);
    void retryCurrentPhase();
    void createUpload();
    void sendNextChunk();
    void completeUpload();
    QByteArray readNextChunk();
    QString privacyViewFor(const QString& privacy) const;
    void clearReply(QPointer<QNetworkReply>& reply);

    static constexpr qint64 kChunkSize = 5LL * 1024LL * 1024LL;
    static constexpr int kMaxRetries = 3;

    QPointer<QNetworkAccessManager> m_nam;
    QPointer<QNetworkReply> m_createReply;
    QPointer<QNetworkReply> m_chunkReply;
    QPointer<QNetworkReply> m_completeReply;
    QPointer<QFile> m_file;

    QString m_accessToken;
    UploadJob m_job;
    QString m_videoUri;
    QString m_uploadLink;
    qint64 m_totalBytes = 0;
    qint64 m_uploadedBytes = 0;
    QByteArray m_pendingChunk;
    RetryPhase m_retryPhase = RetryPhase::None;
    int m_retryCount = 0;
};

} // namespace upload
} // namespace vimeo
