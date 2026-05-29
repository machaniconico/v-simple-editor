#include "InstagramPublish.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtGlobal>

#include "CredentialStore.h"

namespace instagram {
namespace publish {

// ---------------------------------------------------------------------------
// IgConfig
// ---------------------------------------------------------------------------

IgConfig IgConfig::defaultConfig() {
    IgConfig cfg;

    cfg.accessToken = creds::CredentialStore::get(
        "VEDITOR_IG_ACCESS_TOKEN",
        QStringLiteral("instagram/access_token"));
    if (cfg.accessToken.isEmpty()) {
        qWarning("InstagramPublish: VEDITOR_IG_ACCESS_TOKEN not set and no QSettings fallback found");
    }

    cfg.igUserId = creds::CredentialStore::get(
        "VEDITOR_IG_USER_ID",
        QStringLiteral("instagram/ig_user_id"));

    return cfg;
}

// ---------------------------------------------------------------------------
// Publisher
// ---------------------------------------------------------------------------

Publisher::Publisher(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void Publisher::publish(const PublishJob &job, const IgConfig &config) {
    if (config.accessToken.isEmpty()) {
        emit publishFailed(QStringLiteral("Instagram access token is empty"));
        return;
    }
    if (config.igUserId.isEmpty()) {
        emit publishFailed(QStringLiteral("Instagram user ID is empty"));
        return;
    }
    if (job.videoUrl.trimmed().isEmpty()) {
        emit publishFailed(QStringLiteral("Video URL is empty"));
        return;
    }

    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }

    // Phase 1: POST /{igUserId}/media to create a container
    const QUrl createUrl(config.graphBase + QStringLiteral("/") + config.igUserId
                         + QStringLiteral("/media"));
    QNetworkRequest createReq(createUrl);
    createReq.setHeader(QNetworkRequest::ContentTypeHeader,
                        QStringLiteral("application/x-www-form-urlencoded"));

    QUrlQuery createBody;
    createBody.addQueryItem(QStringLiteral("media_type"), QStringLiteral("REELS"));
    createBody.addQueryItem(QStringLiteral("video_url"),  job.videoUrl.trimmed());
    createBody.addQueryItem(QStringLiteral("caption"),    job.caption);
    createBody.addQueryItem(QStringLiteral("access_token"), config.accessToken);

    QNetworkReply *createReply = m_nam->post(createReq, createBody.toString(QUrl::FullyEncoded).toUtf8());
    if (!createReply) {
        emit publishFailed(QStringLiteral("Failed to dispatch media container request"));
        return;
    }

    connect(createReply, &QNetworkReply::finished, this, [this, createReply, config]() {
        createReply->deleteLater();

        if (createReply->error() != QNetworkReply::NoError) {
            emit publishFailed(QStringLiteral("Media container request failed: ")
                               + createReply->errorString());
            return;
        }

        const QByteArray body = createReply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit publishFailed(QStringLiteral("Media container response is not valid JSON"));
            return;
        }

        const QString creationId = doc.object().value(QStringLiteral("id")).toString().trimmed();
        if (creationId.isEmpty()) {
            emit publishFailed(QStringLiteral("Media container response missing 'id' field"));
            return;
        }

        emit publishProgress(33);

        // Phase 2: poll until status_code == FINISHED
        startPollLoop(creationId, config, 0);
    });
}

void Publisher::startPollLoop(const QString &creationId, const IgConfig &config, int pollCount) {
    constexpr int kMaxPolls = 10;

    if (pollCount >= kMaxPolls) {
        emit publishFailed(QStringLiteral("Timed out waiting for media container to finish processing"));
        return;
    }

    // Emit progress from 33 toward 66 as we poll
    const int pollProgress = 33 + static_cast<int>((static_cast<double>(pollCount) / kMaxPolls) * 33.0);
    emit publishProgress(pollProgress);

    QTimer::singleShot(2000, this, [this, creationId, config, pollCount]() {
        if (!m_nam) {
            m_nam = new QNetworkAccessManager(this);
        }

        const QUrl statusUrl(config.graphBase + QStringLiteral("/") + creationId
                             + QStringLiteral("?fields=status_code&access_token=")
                             + config.accessToken);
        QNetworkRequest statusReq(statusUrl);

        QNetworkReply *statusReply = m_nam->get(statusReq);
        if (!statusReply) {
            emit publishFailed(QStringLiteral("Failed to dispatch status poll request"));
            return;
        }

        connect(statusReply, &QNetworkReply::finished, this, [this, statusReply, creationId, config, pollCount]() {
            statusReply->deleteLater();

            if (statusReply->error() != QNetworkReply::NoError) {
                emit publishFailed(QStringLiteral("Status poll request failed: ")
                                   + statusReply->errorString());
                return;
            }

            const QByteArray body = statusReply->readAll();
            const QJsonDocument doc = QJsonDocument::fromJson(body);
            if (!doc.isObject()) {
                emit publishFailed(QStringLiteral("Status poll response is not valid JSON"));
                return;
            }

            const QString statusCode = doc.object()
                                           .value(QStringLiteral("status_code"))
                                           .toString()
                                           .trimmed()
                                           .toUpper();

            if (statusCode == QStringLiteral("ERROR")) {
                emit publishFailed(QStringLiteral("Instagram media processing returned ERROR status"));
                return;
            }

            if (statusCode == QStringLiteral("FINISHED")
                || statusCode == QStringLiteral("PUBLISHED")) {
                // Phase 3: publish the container
                doMediaPublish(creationId, config);
                return;
            }

            // Still IN_PROGRESS — keep polling
            startPollLoop(creationId, config, pollCount + 1);
        });
    });
}

void Publisher::doMediaPublish(const QString &creationId, const IgConfig &config) {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }

    const QUrl publishUrl(config.graphBase + QStringLiteral("/") + config.igUserId
                          + QStringLiteral("/media_publish"));
    QNetworkRequest publishReq(publishUrl);
    publishReq.setHeader(QNetworkRequest::ContentTypeHeader,
                         QStringLiteral("application/x-www-form-urlencoded"));

    QUrlQuery publishBody;
    publishBody.addQueryItem(QStringLiteral("creation_id"), creationId);
    publishBody.addQueryItem(QStringLiteral("access_token"), config.accessToken);

    QNetworkReply *publishReply = m_nam->post(publishReq, publishBody.toString(QUrl::FullyEncoded).toUtf8());
    if (!publishReply) {
        emit publishFailed(QStringLiteral("Failed to dispatch media_publish request"));
        return;
    }

    connect(publishReply, &QNetworkReply::finished, this, [this, publishReply]() {
        publishReply->deleteLater();

        if (publishReply->error() != QNetworkReply::NoError) {
            emit publishFailed(QStringLiteral("media_publish request failed: ")
                               + publishReply->errorString());
            return;
        }

        const QByteArray body = publishReply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit publishFailed(QStringLiteral("media_publish response is not valid JSON"));
            return;
        }

        const QString mediaId = doc.object().value(QStringLiteral("id")).toString().trimmed();
        if (mediaId.isEmpty()) {
            emit publishFailed(QStringLiteral("media_publish response missing 'id' field"));
            return;
        }

        emit publishProgress(100);
        emit publishFinished(mediaId);
    });
}

} // namespace publish
} // namespace instagram
