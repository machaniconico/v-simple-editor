#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;

namespace x {
namespace upload {

struct XUploadConfig {
    //   bearerToken 取得経路: env (VEDITOR_X_BEARER_TOKEN) → QSettings (x_video/bearer_token)
    //                      → 空 (creds::CredentialStore)
    QString bearerToken;
    QString apiBase       = QStringLiteral("https://upload.twitter.com/1.1");
    QString tweetApiBase  = QStringLiteral("https://api.twitter.com/2");

    static XUploadConfig defaultConfig();
};

struct UploadJob {
    QString filePath;
    QString tweetText;
};

class UploadClient : public QObject {
    Q_OBJECT
public:
    explicit UploadClient(QObject *parent = nullptr);
    ~UploadClient() override;

    void startUpload(const UploadJob &job, const XUploadConfig &config);

signals:
    void uploadProgress(qint64 sent, qint64 total);
    void uploadFinished(const QString &tweetId);
    void uploadFailed(const QString &error);

private:
    void doInit(const UploadJob &job, const XUploadConfig &cfg, qint64 totalBytes);
    void doAppend(const XUploadConfig &cfg, const QString &mediaId,
                  const UploadJob &job, qint64 totalBytes,
                  int segmentIndex, qint64 offset);
    void doFinalize(const XUploadConfig &cfg, const QString &mediaId,
                    const UploadJob &job);
    void doStatusPoll(const XUploadConfig &cfg, const QString &mediaId,
                      const UploadJob &job);
    void doCreateTweet(const XUploadConfig &cfg, const QString &mediaId,
                       const QString &tweetText);

    QPointer<QNetworkAccessManager> m_nam;

    static constexpr qint64 kChunkSize = 5LL * 1024LL * 1024LL;
};

} // namespace upload
} // namespace x
