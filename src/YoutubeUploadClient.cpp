#include "YoutubeUploadClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QByteArray>

namespace youtube {
namespace upload {

namespace {

static QString g_apiBaseUrl;

QString initiateUrl() {
    const QString base = g_apiBaseUrl.isEmpty()
        ? QStringLiteral("https://www.googleapis.com")
        : g_apiBaseUrl;
    return base + QStringLiteral("/upload/youtube/v3/videos?uploadType=resumable&part=snippet,status");
}

// snippet + status を JSON にシリアライズ。
QByteArray buildMetadataJson(const UploadMetadata& m) {
    QJsonObject snippet;
    snippet.insert(QStringLiteral("title"),       m.title);
    snippet.insert(QStringLiteral("description"), m.description);
    if (!m.tags.isEmpty()) {
        QJsonArray tagArr;
        for (const QString& t : m.tags) tagArr.append(t);
        snippet.insert(QStringLiteral("tags"), tagArr);
    }
    snippet.insert(QStringLiteral("categoryId"), QString::number(m.categoryId));

    QJsonObject status;
    status.insert(QStringLiteral("privacyStatus"), m.privacy);

    QJsonObject root;
    root.insert(QStringLiteral("snippet"), snippet);
    root.insert(QStringLiteral("status"),  status);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// reply error 文字列化 (空 reply 含む安全 wrapper)。
QString replyErrorString(QNetworkReply* reply) {
    if (!reply) return QStringLiteral("reply is null");
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString s = reply->errorString();
    if (s.isEmpty()) s = QStringLiteral("unknown network error");
    return QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(s);
}

} // namespace

// ---------------------------------------------------------------------------
// static helpers
// ---------------------------------------------------------------------------

QByteArray Client::buildContentRange(qint64 offset, qint64 chunkLen, qint64 totalSize) {
    if (chunkLen <= 0) {
        // status query: bytes */<totalSize>
        return QByteArray("bytes */") + QByteArray::number(totalSize);
    }
    const qint64 endByte = offset + chunkLen - 1;
    QByteArray hdr = "bytes ";
    hdr += QByteArray::number(offset);
    hdr += '-';
    hdr += QByteArray::number(endByte);
    hdr += '/';
    hdr += QByteArray::number(totalSize);
    return hdr;
}

qint64 Client::parseRangeHeaderEnd(const QByteArray& rangeHeaderValue) {
    // 例: "bytes=0-524287"  → 524287
    const int eq = rangeHeaderValue.indexOf('=');
    if (eq < 0) return -1;
    const int dash = rangeHeaderValue.indexOf('-', eq + 1);
    if (dash < 0) return -1;
    const QByteArray endStr = rangeHeaderValue.mid(dash + 1).trimmed();
    bool ok = false;
    const qint64 endByte = endStr.toLongLong(&ok);
    if (!ok) return -1;
    return endByte;
}

void Client::setApiBaseUrl(const QString& url) {
    g_apiBaseUrl = url;
}

QString Client::apiBaseUrl() {
    return g_apiBaseUrl;
}

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------

Client::Client(QObject* parent)
    : QObject(parent),
      m_nam(new QNetworkAccessManager(this)) {}

Client::~Client() = default;

// ---------------------------------------------------------------------------
// initiateSession
// ---------------------------------------------------------------------------

void Client::initiateSession(const youtube::oauth::Token& token,
                             const UploadMetadata& metadata,
                             qint64 fileSize) {
    if (!token.isValid()) {
        emit sessionError(QStringLiteral("access token is empty"));
        return;
    }
    if (fileSize <= 0) {
        emit sessionError(QStringLiteral("fileSize must be positive"));
        return;
    }
    if (m_initiateReply) {
        // 古い reply を破棄してから再起動 (1 client 1 session 前提)。
        m_initiateReply->deleteLater();
        m_initiateReply.clear();
    }

    const QByteArray body = buildMetadataJson(metadata);

    QNetworkRequest req((QUrl(initiateUrl())));
    req.setRawHeader("Authorization",
                     "Bearer " + token.accessToken.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json; charset=UTF-8"));
    req.setRawHeader("X-Upload-Content-Type",   "video/*");
    req.setRawHeader("X-Upload-Content-Length", QByteArray::number(fileSize));

    m_initiateReply = m_nam->post(req, body);
    if (!m_initiateReply) {
        emit sessionError(QStringLiteral("failed to dispatch POST request"));
        return;
    }
    connect(m_initiateReply, &QNetworkReply::finished,
            this, &Client::onInitiateReplyFinished);
}

void Client::onInitiateReplyFinished() {
    if (!m_initiateReply) return;
    QNetworkReply* reply = m_initiateReply;
    reply->deleteLater();
    m_initiateReply.clear();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        emit sessionError(replyErrorString(reply));
        return;
    }
    if (httpStatus != 200 && httpStatus != 201) {
        emit sessionError(QStringLiteral("unexpected HTTP status %1 on initiate")
                              .arg(httpStatus));
        return;
    }
    const QByteArray loc = reply->rawHeader("Location");
    if (loc.isEmpty()) {
        emit sessionError(QStringLiteral("Location header missing in initiate response"));
        return;
    }
    emit sessionInitiated(QString::fromUtf8(loc));
}

// ---------------------------------------------------------------------------
// uploadChunk
// ---------------------------------------------------------------------------

void Client::uploadChunk(const QString& sessionUri,
                         const QByteArray& chunk,
                         qint64 offset,
                         qint64 totalSize) {
    if (sessionUri.isEmpty()) {
        emit chunkError(QStringLiteral("sessionUri is empty"));
        return;
    }
    if (chunk.isEmpty()) {
        emit chunkError(QStringLiteral("chunk is empty"));
        return;
    }
    if (offset < 0 || totalSize <= 0 ||
        offset + static_cast<qint64>(chunk.size()) > totalSize) {
        emit chunkError(QStringLiteral("invalid offset/totalSize"));
        return;
    }
    if (m_chunkReply) {
        m_chunkReply->deleteLater();
        m_chunkReply.clear();
    }

    m_chunkOffset    = offset;
    m_chunkLen       = static_cast<qint64>(chunk.size());
    m_chunkTotalSize = totalSize;

    QNetworkRequest req((QUrl(sessionUri)));
    const qint64 chunkLen = static_cast<qint64>(chunk.size());
    req.setRawHeader("Content-Range",
                     buildContentRange(offset, chunkLen, totalSize));
    req.setHeader(QNetworkRequest::ContentLengthHeader,
                  QVariant::fromValue<qint64>(chunkLen));
    // YouTube は X-Upload-Content-Type を initiate で受けているので
    // chunk PUT 自体は Content-Type を明示しない (省略可)。
    // ただし一部 proxy が嫌うので念のため video/* を付ける。
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("video/*"));

    m_chunkReply = m_nam->sendCustomRequest(req, "PUT", chunk);
    if (!m_chunkReply) {
        emit chunkError(QStringLiteral("failed to dispatch PUT request"));
        return;
    }
    connect(m_chunkReply, &QNetworkReply::finished,
            this, &Client::onChunkReplyFinished);
}

void Client::onChunkReplyFinished() {
    if (!m_chunkReply) return;
    QNetworkReply* reply = m_chunkReply;
    reply->deleteLater();
    m_chunkReply.clear();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 308 Resume Incomplete はネットワーク error 扱いされる場合があるので
    // status code を優先してチェックする。
    if (httpStatus == 308) {
        const QByteArray rng = reply->rawHeader("Range");
        qint64 nextOffset = m_chunkOffset + m_chunkLen; // fallback
        if (!rng.isEmpty()) {
            const qint64 endByte = parseRangeHeaderEnd(rng);
            if (endByte >= 0) nextOffset = endByte + 1;
        }
        emit chunkUploaded(nextOffset);
        return;
    }
    if (httpStatus == 200 || httpStatus == 201) {
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        QString videoId;
        if (doc.isObject()) {
            videoId = doc.object().value(QStringLiteral("id")).toString();
        }
        if (videoId.isEmpty()) {
            // JSON parse 失敗でも完了は完了。空 id で通知してから上位で処理。
            emit completed(QString());
        } else {
            emit completed(videoId);
        }
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        emit chunkError(replyErrorString(reply));
        return;
    }
    emit chunkError(QStringLiteral("unexpected HTTP status %1 on chunk PUT")
                        .arg(httpStatus));
}

// ---------------------------------------------------------------------------
// queryStatus
// ---------------------------------------------------------------------------

void Client::queryStatus(const QString& sessionUri, qint64 totalSize) {
    if (sessionUri.isEmpty()) {
        emit statusError(QStringLiteral("sessionUri is empty"));
        return;
    }
    if (totalSize <= 0) {
        emit statusError(QStringLiteral("totalSize must be positive"));
        return;
    }
    if (m_statusReply) {
        m_statusReply->deleteLater();
        m_statusReply.clear();
    }

    m_statusTotalSize = totalSize;

    QNetworkRequest req((QUrl(sessionUri)));
    req.setRawHeader("Content-Range", buildContentRange(0, 0, totalSize));
    req.setHeader(QNetworkRequest::ContentLengthHeader,
                  QVariant::fromValue<qint64>(0));

    m_statusReply = m_nam->sendCustomRequest(req, "PUT", QByteArray());
    if (!m_statusReply) {
        emit statusError(QStringLiteral("failed to dispatch status PUT"));
        return;
    }
    connect(m_statusReply, &QNetworkReply::finished,
            this, &Client::onStatusReplyFinished);
}

void Client::onStatusReplyFinished() {
    if (!m_statusReply) return;
    QNetworkReply* reply = m_statusReply;
    reply->deleteLater();
    m_statusReply.clear();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (httpStatus == 308) {
        const QByteArray rng = reply->rawHeader("Range");
        qint64 nextOffset = 0;
        if (!rng.isEmpty()) {
            const qint64 endByte = parseRangeHeaderEnd(rng);
            if (endByte >= 0) nextOffset = endByte + 1;
        }
        emit statusKnown(nextOffset);
        return;
    }
    if (httpStatus == 200 || httpStatus == 201) {
        // 既に完了している場合は totalSize を次オフセットとして通知。
        emit statusKnown(m_statusTotalSize);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        emit statusError(replyErrorString(reply));
        return;
    }
    emit statusError(QStringLiteral("unexpected HTTP status %1 on status query")
                         .arg(httpStatus));
}

} // namespace upload
} // namespace youtube
