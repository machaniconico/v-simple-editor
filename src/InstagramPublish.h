#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;

namespace instagram {
namespace publish {

struct IgConfig {
    QString accessToken;
    QString igUserId;
    QString graphBase = QStringLiteral("https://graph.facebook.com/v19.0");

    static IgConfig defaultConfig();
};

struct PublishJob {
    QString videoUrl;
    QString caption;
    bool shareToFeed = true;
};

class Publisher : public QObject {
    Q_OBJECT
public:
    explicit Publisher(QObject *parent = nullptr);

    void publish(const PublishJob &job, const IgConfig &config);

signals:
    void publishProgress(int percent);
    void publishFinished(const QString &mediaId);
    void publishFailed(const QString &error);

private:
    void startPollLoop(const QString &creationId, const IgConfig &config, int pollCount);
    void doMediaPublish(const QString &creationId, const IgConfig &config);

    QPointer<QNetworkAccessManager> m_nam;
};

} // namespace publish
} // namespace instagram
