#include "XVideoUpload.h"

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
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

namespace x {
namespace upload {

// ---------------------------------------------------------------------------
// XUploadConfig
// ---------------------------------------------------------------------------

XUploadConfig XUploadConfig::defaultConfig() {
    XUploadConfig cfg;

    cfg.bearerToken = creds::CredentialStore::get(
        "VEDITOR_X_BEARER_TOKEN",
        QStringLiteral("x_video/bearer_token"),
        QString(),
        /*emitWarning=*/true);
    return cfg;
}

// ---------------------------------------------------------------------------
// UploadClient
// ---------------------------------------------------------------------------

UploadClient::UploadClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this)) {}

UploadClient::~UploadClient() = default;

void UploadClient::startUpload(const UploadJob &job, const XUploadConfig &config) {
    if (config.bearerToken.isEmpty()) {
        emit uploadFailed(QStringLiteral("X bearer token not configured"));
        return;
    }
    if (job.filePath.trimmed().isEmpty()) {
        emit uploadFailed(QStringLiteral("upload file path is empty"));
        return;
    }

    QFileInfo info(job.filePath);
    if (!info.exists() || !info.isFile()) {
        emit uploadFailed(
            QStringLiteral("upload file does not exist: %1").arg(job.filePath));
        return;
    }

    const qint64 totalBytes = info.size();
    if (totalBytes <= 0) {
        emit uploadFailed(
            QStringLiteral("upload file is empty: %1").arg(job.filePath));
        return;
    }

    doInit(job, config, totalBytes);
}

// ---------------------------------------------------------------------------
// Step 1 — INIT
// ---------------------------------------------------------------------------

void UploadClient::doInit(const UploadJob &job, const XUploadConfig &cfg,
                          qint64 totalBytes) {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("command"),        QStringLiteral("INIT"));
    query.addQueryItem(QStringLiteral("total_bytes"),    QString::number(totalBytes));
    query.addQueryItem(QStringLiteral("media_type"),     QStringLiteral("video/mp4"));
    query.addQueryItem(QStringLiteral("media_category"), QStringLiteral("tweet_video"));

    const QUrl url(cfg.apiBase + QStringLiteral("/media/upload.json"));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    req.setRawHeader("Authorization",
                     QByteArray("Bearer ") + cfg.bearerToken.toUtf8());

    QNetworkReply *reply =
        m_nam->post(req, query.toString(QUrl::FullyEncoded).toUtf8());
    if (!reply) {
        emit uploadFailed(QStringLiteral("failed to dispatch INIT request"));
        return;
    }

    // Capture by value to avoid dangling refs
    connect(reply, &QNetworkReply::finished,
            this, [this, reply, job, cfg, totalBytes]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit uploadFailed(
                QStringLiteral("INIT network error: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit uploadFailed(QStringLiteral("INIT response is not JSON"));
            return;
        }
        const QString mediaId =
            doc.object().value(QStringLiteral("media_id_string")).toString().trimmed();
        if (mediaId.isEmpty()) {
            emit uploadFailed(QStringLiteral("INIT response missing media_id_string"));
            return;
        }
        doAppend(cfg, mediaId, job, totalBytes, 0, 0);
    });
}

// ---------------------------------------------------------------------------
// Step 2 — APPEND  (recursive, 5 MB chunks)
// ---------------------------------------------------------------------------

void UploadClient::doAppend(const XUploadConfig &cfg, const QString &mediaId,
                             const UploadJob &job, qint64 totalBytes,
                             int segmentIndex, qint64 offset) {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }

    QFile *file = new QFile(job.filePath, this);
    if (!file->open(QIODevice::ReadOnly)) {
        file->deleteLater();
        emit uploadFailed(
            QStringLiteral("failed to open file for APPEND: %1")
                .arg(file->errorString()));
        return;
    }
    if (!file->seek(offset)) {
        file->deleteLater();
        emit uploadFailed(QStringLiteral("failed to seek upload file"));
        return;
    }
    const QByteArray chunk = file->read(kChunkSize);
    file->close();
    file->deleteLater();

    if (chunk.isEmpty()) {
        // No more data → go to FINALIZE
        doFinalize(cfg, mediaId, job);
        return;
    }

    const QUrl url(cfg.apiBase + QStringLiteral("/media/upload.json"));
    QNetworkRequest req(url);
    req.setRawHeader("Authorization",
                     QByteArray("Bearer ") + cfg.bearerToken.toUtf8());

    QHttpMultiPart *multiPart =
        new QHttpMultiPart(QHttpMultiPart::FormDataType, this);

    auto addField = [&](const QString &name, const QByteArray &value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"%1\"").arg(name));
        part.setBody(value);
        multiPart->append(part);
    };

    addField(QStringLiteral("command"),       QByteArray("APPEND"));
    addField(QStringLiteral("media_id"),      mediaId.toUtf8());
    addField(QStringLiteral("segment_index"), QByteArray::number(segmentIndex));

    QHttpPart mediaPart;
    mediaPart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QStringLiteral("form-data; name=\"media\"; filename=\"chunk\""));
    mediaPart.setHeader(QNetworkRequest::ContentTypeHeader,
                        QStringLiteral("application/octet-stream"));
    mediaPart.setBody(chunk);
    multiPart->append(mediaPart);

    QNetworkReply *reply = m_nam->post(req, multiPart);
    if (!reply) {
        multiPart->deleteLater();
        emit uploadFailed(QStringLiteral("failed to dispatch APPEND request"));
        return;
    }
    multiPart->setParent(reply);   // reply takes ownership

    const qint64 nextOffset   = offset + static_cast<qint64>(chunk.size());
    const int    nextSegment  = segmentIndex + 1;

    connect(reply, &QNetworkReply::uploadProgress,
            this, [this, offset, totalBytes](qint64 sent, qint64 /*total*/) {
        const qint64 totalSent = qMin(totalBytes, offset + sent);
        emit uploadProgress(totalSent, totalBytes);
    });

    connect(reply, &QNetworkReply::finished,
            this, [this, reply, cfg, mediaId, job, totalBytes,
                   nextSegment, nextOffset]() {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            emit uploadFailed(
                QStringLiteral("APPEND error (HTTP %1): %2")
                    .arg(status)
                    .arg(reply->errorString()));
            return;
        }
        emit uploadProgress(nextOffset, totalBytes);
        if (nextOffset >= totalBytes) {
            doFinalize(cfg, mediaId, job);
        } else {
            doAppend(cfg, mediaId, job, totalBytes, nextSegment, nextOffset);
        }
    });
}

// ---------------------------------------------------------------------------
// Step 3 — FINALIZE
// ---------------------------------------------------------------------------

void UploadClient::doFinalize(const XUploadConfig &cfg, const QString &mediaId,
                               const UploadJob &job) {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("command"),  QStringLiteral("FINALIZE"));
    query.addQueryItem(QStringLiteral("media_id"), mediaId);

    const QUrl url(cfg.apiBase + QStringLiteral("/media/upload.json"));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    req.setRawHeader("Authorization",
                     QByteArray("Bearer ") + cfg.bearerToken.toUtf8());

    QNetworkReply *reply =
        m_nam->post(req, query.toString(QUrl::FullyEncoded).toUtf8());
    if (!reply) {
        emit uploadFailed(QStringLiteral("failed to dispatch FINALIZE request"));
        return;
    }

    connect(reply, &QNetworkReply::finished,
            this, [this, reply, cfg, mediaId, job]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit uploadFailed(
                QStringLiteral("FINALIZE network error: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            const QJsonObject root = doc.object();
            if (root.contains(QStringLiteral("processing_info"))) {
                // Server needs time to transcode — enter STATUS poll
                doStatusPoll(cfg, mediaId, job);
                return;
            }
        }
        // No processing_info → immediately create tweet
        doCreateTweet(cfg, mediaId, job.tweetText);
    });
}

// ---------------------------------------------------------------------------
// Step 4 — STATUS poll
// ---------------------------------------------------------------------------

void UploadClient::doStatusPoll(const XUploadConfig &cfg, const QString &mediaId,
                                 const UploadJob &job) {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }

    QUrl url(cfg.apiBase + QStringLiteral("/media/upload.json"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("command"),  QStringLiteral("STATUS"));
    q.addQueryItem(QStringLiteral("media_id"), mediaId);
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("Authorization",
                     QByteArray("Bearer ") + cfg.bearerToken.toUtf8());

    QNetworkReply *reply = m_nam->get(req);
    if (!reply) {
        emit uploadFailed(QStringLiteral("failed to dispatch STATUS request"));
        return;
    }

    connect(reply, &QNetworkReply::finished,
            this, [this, reply, cfg, mediaId, job]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit uploadFailed(
                QStringLiteral("STATUS network error: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit uploadFailed(QStringLiteral("STATUS response is not JSON"));
            return;
        }
        const QJsonObject root = doc.object();
        const QJsonObject procInfo =
            root.value(QStringLiteral("processing_info")).toObject();
        const QString state =
            procInfo.value(QStringLiteral("state")).toString().trimmed().toLower();

        if (state == QStringLiteral("succeeded")) {
            doCreateTweet(cfg, mediaId, job.tweetText);
            return;
        }
        if (state == QStringLiteral("failed")) {
            const QString errMsg =
                procInfo.value(QStringLiteral("error"))
                    .toObject()
                    .value(QStringLiteral("message"))
                    .toString()
                    .trimmed();
            emit uploadFailed(
                QStringLiteral("X media processing failed: %1").arg(errMsg));
            return;
        }

        // Still in_progress — respect check_after_secs
        const int checkAfterSecs = procInfo.value(QStringLiteral("check_after_secs")).toInt(5);
        const int delayMs = qMax(1, checkAfterSecs) * 1000;
        QTimer::singleShot(delayMs, this, [this, cfg, mediaId, job]() {
            doStatusPoll(cfg, mediaId, job);
        });
    });
}

// ---------------------------------------------------------------------------
// Step 5 — CREATE TWEET
// ---------------------------------------------------------------------------

void UploadClient::doCreateTweet(const XUploadConfig &cfg, const QString &mediaId,
                                  const QString &tweetText) {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }

    QJsonArray mediaIds;
    mediaIds.append(mediaId);

    QJsonObject mediaObj;
    mediaObj.insert(QStringLiteral("media_ids"), mediaIds);

    QJsonObject body;
    body.insert(QStringLiteral("text"),  tweetText);
    body.insert(QStringLiteral("media"), mediaObj);

    const QUrl url(cfg.tweetApiBase + QStringLiteral("/tweets"));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json; charset=UTF-8"));
    req.setRawHeader("Authorization",
                     QByteArray("Bearer ") + cfg.bearerToken.toUtf8());

    QNetworkReply *reply =
        m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!reply) {
        emit uploadFailed(QStringLiteral("failed to dispatch CREATE TWEET request"));
        return;
    }

    connect(reply, &QNetworkReply::finished,
            this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit uploadFailed(
                QStringLiteral("CREATE TWEET network error: %1")
                    .arg(reply->errorString()));
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            emit uploadFailed(QStringLiteral("CREATE TWEET response is not JSON"));
            return;
        }
        const QString tweetId =
            doc.object()
                .value(QStringLiteral("data"))
                .toObject()
                .value(QStringLiteral("id"))
                .toString()
                .trimmed();
        if (tweetId.isEmpty()) {
            emit uploadFailed(
                QStringLiteral("CREATE TWEET response missing data.id"));
            return;
        }
        emit uploadFinished(tweetId);
    });
}

} // namespace upload
} // namespace x
