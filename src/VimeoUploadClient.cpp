#include "VimeoUploadClient.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtGlobal>

namespace vimeo {
namespace upload {

namespace {
QString g_apiBaseUrl;

QString createVideoUrl() {
    const QString base = g_apiBaseUrl.isEmpty()
        ? QStringLiteral("https://api.vimeo.com")
        : g_apiBaseUrl;
    return base + QStringLiteral("/me/videos");
}

QString replyMessage(QNetworkReply* reply, const QByteArray& body) {
    QString detail;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        detail = obj.value(QStringLiteral("developer_message")).toString().trimmed();
        if (detail.isEmpty()) {
            detail = obj.value(QStringLiteral("error")).toString().trimmed();
        }
        if (detail.isEmpty()) {
            detail = obj.value(QStringLiteral("description")).toString().trimmed();
        }
    }

    if (detail.isEmpty()) {
        detail = QString::fromUtf8(body).trimmed();
    }
    if (detail.isEmpty() && reply) {
        detail = reply->errorString();
    }

    const int status = reply
        ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
        : 0;
    return QStringLiteral("HTTP %1: %2").arg(status).arg(detail);
}

QString extractUploadLink(const QJsonObject& root) {
    const QJsonObject upload = root.value(QStringLiteral("upload")).toObject();
    QString uploadLink = upload.value(QStringLiteral("upload_link")).toString().trimmed();
    if (uploadLink.isEmpty()) {
        uploadLink = upload.value(QStringLiteral("upload_link_secure")).toString().trimmed();
    }
    return uploadLink;
}

qint64 headerOffset(const QNetworkReply* reply) {
    if (!reply) {
        return -1;
    }
    bool ok = false;
    const qint64 offset = reply->rawHeader("Upload-Offset").trimmed().toLongLong(&ok);
    return ok ? offset : -1;
}

} // namespace

UploadClient::UploadClient(const vimeo::oauth::VimeoOAuthConfig& config, QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_accessToken(config.accessToken.trimmed()) {}

UploadClient::~UploadClient() = default;

void UploadClient::setApiBaseUrl(const QString& url) {
    g_apiBaseUrl = url;
}

QString UploadClient::apiBaseUrl() {
    return g_apiBaseUrl;
}

void UploadClient::setAccessToken(const QString& accessToken) {
    m_accessToken = accessToken.trimmed();
}

void UploadClient::startUpload(const UploadJob& job) {
    if (m_accessToken.isEmpty()) {
        emit uploadFailed(QStringLiteral("Vimeo access token is empty"));
        return;
    }
    if (job.filePath.trimmed().isEmpty()) {
        emit uploadFailed(QStringLiteral("upload file path is empty"));
        return;
    }

    QFileInfo info(job.filePath);
    if (!info.exists() || !info.isFile()) {
        emit uploadFailed(QStringLiteral("upload file does not exist: %1").arg(job.filePath));
        return;
    }

    cancel();

    m_job = job;
    m_totalBytes = info.size();
    if (m_totalBytes <= 0) {
        emit uploadFailed(QStringLiteral("upload file is empty: %1").arg(job.filePath));
        return;
    }

    m_file = new QFile(job.filePath, this);
    if (!m_file || !m_file->open(QIODevice::ReadOnly)) {
        const QString error = m_file ? m_file->errorString()
                                     : QStringLiteral("failed to allocate QFile");
        resetJobState();
        emit uploadFailed(QStringLiteral("failed to open upload file: %1").arg(error));
        return;
    }

    m_uploadedBytes = 0;
    m_pendingChunk.clear();
    m_retryPhase = RetryPhase::None;
    m_retryCount = 0;
    createUpload();
}

void UploadClient::cancel() {
    clearReply(m_createReply);
    clearReply(m_chunkReply);
    clearReply(m_completeReply);
    resetJobState();
}

void UploadClient::onCreateUploadFinished() {
    if (!m_createReply) {
        return;
    }

    QNetworkReply* reply = m_createReply;
    m_createReply.clear();

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString errorText = replyMessage(reply, body);
    reply->deleteLater();

    if ((networkError != QNetworkReply::NoError && status < 200) || status >= 400) {
        scheduleRetry(RetryPhase::CreateUpload, errorText);
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        scheduleRetry(RetryPhase::CreateUpload,
                      QStringLiteral("Vimeo create upload response was not JSON"));
        return;
    }

    const QJsonObject root = doc.object();
    m_videoUri = root.value(QStringLiteral("uri")).toString().trimmed();
    m_uploadLink = extractUploadLink(root);
    if (m_uploadLink.isEmpty()) {
        scheduleRetry(RetryPhase::CreateUpload,
                      QStringLiteral("Vimeo create upload response missing upload link"));
        return;
    }

    m_retryPhase = RetryPhase::None;
    m_retryCount = 0;
    sendNextChunk();
}

void UploadClient::onChunkUploadProgress(qint64 sent, qint64) {
    const qint64 totalSent = qMin(m_totalBytes, m_uploadedBytes + sent);
    emit uploadProgress(totalSent, m_totalBytes);
}

void UploadClient::onChunkReplyFinished() {
    if (!m_chunkReply) {
        return;
    }

    QNetworkReply* reply = m_chunkReply;
    m_chunkReply.clear();

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const qint64 nextOffset = headerOffset(reply);
    const QString errorText = replyMessage(reply, body);
    reply->deleteLater();

    if ((networkError != QNetworkReply::NoError && status < 200) || status >= 400) {
        scheduleRetry(RetryPhase::UploadChunk, errorText);
        return;
    }

    const qint64 chunkBytes = static_cast<qint64>(m_pendingChunk.size());
    m_uploadedBytes = nextOffset >= 0 ? nextOffset : (m_uploadedBytes + chunkBytes);
    m_retryPhase = RetryPhase::None;
    m_retryCount = 0;
    m_pendingChunk.clear();
    emit uploadProgress(m_uploadedBytes, m_totalBytes);

    if (m_uploadedBytes >= m_totalBytes) {
        completeUpload();
        return;
    }

    sendNextChunk();
}

void UploadClient::onCompleteReplyFinished() {
    if (!m_completeReply) {
        return;
    }

    QNetworkReply* reply = m_completeReply;
    m_completeReply.clear();

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const qint64 remoteOffset = headerOffset(reply);
    const QString errorText = replyMessage(reply, body);
    reply->deleteLater();

    if ((networkError != QNetworkReply::NoError && status < 200) || status >= 400) {
        scheduleRetry(RetryPhase::CompleteUpload, errorText);
        return;
    }

    if (remoteOffset >= 0 && remoteOffset < m_totalBytes) {
        m_uploadedBytes = remoteOffset;
        m_pendingChunk.clear();
        m_retryPhase = RetryPhase::None;
        m_retryCount = 0;
        sendNextChunk();
        return;
    }

    const QString videoUri = m_videoUri.isEmpty() ? m_uploadLink : m_videoUri;
    resetJobState();
    emit uploadFinished(videoUri);
}

void UploadClient::resetJobState() {
    if (m_file) {
        if (m_file->isOpen()) {
            m_file->close();
        }
        m_file->deleteLater();
        m_file.clear();
    }

    m_job = UploadJob();
    m_videoUri.clear();
    m_uploadLink.clear();
    m_totalBytes = 0;
    m_uploadedBytes = 0;
    m_pendingChunk.clear();
    m_retryPhase = RetryPhase::None;
    m_retryCount = 0;
}

void UploadClient::failUpload(const QString& error) {
    clearReply(m_createReply);
    clearReply(m_chunkReply);
    clearReply(m_completeReply);
    resetJobState();
    emit uploadFailed(error);
}

void UploadClient::scheduleRetry(RetryPhase phase, const QString& errorContext) {
    if (m_retryCount >= kMaxRetries) {
        failUpload(errorContext);
        return;
    }

    ++m_retryCount;
    m_retryPhase = phase;
    const int delayMs = 1000 * (1 << (m_retryCount - 1));
    QTimer::singleShot(delayMs, this, [this, phase]() {
        if (m_retryPhase != phase) {
            return;
        }
        retryCurrentPhase();
    });
}

void UploadClient::retryCurrentPhase() {
    switch (m_retryPhase) {
    case RetryPhase::CreateUpload:
        createUpload();
        break;
    case RetryPhase::UploadChunk:
        sendNextChunk();
        break;
    case RetryPhase::CompleteUpload:
        completeUpload();
        break;
    case RetryPhase::None:
    default:
        break;
    }
}

void UploadClient::createUpload() {
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    clearReply(m_createReply);

    QJsonObject uploadObject;
    uploadObject.insert(QStringLiteral("approach"), QStringLiteral("tus"));
    uploadObject.insert(QStringLiteral("size"), QString::number(m_totalBytes));

    QJsonObject privacyObject;
    privacyObject.insert(QStringLiteral("view"), privacyViewFor(m_job.privacy));

    QJsonObject root;
    root.insert(QStringLiteral("upload"), uploadObject);
    root.insert(QStringLiteral("privacy"), privacyObject);
    if (!m_job.title.isEmpty()) {
        root.insert(QStringLiteral("name"), m_job.title);
    }
    if (!m_job.description.isEmpty()) {
        root.insert(QStringLiteral("description"), m_job.description);
    }

    QNetworkRequest request{QUrl(createVideoUrl())};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json; charset=UTF-8"));
    request.setRawHeader("Accept", "application/vnd.vimeo.*+json;version=3.4");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_accessToken.toUtf8());
    request.setRawHeader("Tus-Resumable", "1.0.0");
    request.setRawHeader("Upload-Length", QByteArray::number(m_totalBytes));

    m_createReply = m_nam->post(request, QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!m_createReply) {
        scheduleRetry(RetryPhase::CreateUpload,
                      QStringLiteral("failed to dispatch Vimeo create upload request"));
        return;
    }

    connect(m_createReply, &QNetworkReply::finished,
            this, &UploadClient::onCreateUploadFinished);
}

void UploadClient::sendNextChunk() {
    if (!m_file || !m_file->isOpen()) {
        failUpload(QStringLiteral("upload file is not open"));
        return;
    }
    if (m_uploadLink.isEmpty()) {
        failUpload(QStringLiteral("upload link is empty"));
        return;
    }

    clearReply(m_chunkReply);

    if (m_pendingChunk.isEmpty()) {
        if (!m_file->seek(m_uploadedBytes)) {
            failUpload(QStringLiteral("failed to seek upload file"));
            return;
        }
        m_pendingChunk = readNextChunk();
        if (m_pendingChunk.isEmpty()) {
            failUpload(QStringLiteral("failed to read next upload chunk"));
            return;
        }
    }

    QNetworkRequest request{QUrl(m_uploadLink)};
    request.setRawHeader("Tus-Resumable", "1.0.0");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_accessToken.toUtf8());
    request.setRawHeader("Upload-Offset", QByteArray::number(m_uploadedBytes));
    request.setRawHeader("Upload-Length", QByteArray::number(m_totalBytes));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/offset+octet-stream"));
    request.setHeader(QNetworkRequest::ContentLengthHeader,
                      QVariant::fromValue<qint64>(m_pendingChunk.size()));

    m_chunkReply = m_nam->sendCustomRequest(request, "PATCH", m_pendingChunk);
    if (!m_chunkReply) {
        scheduleRetry(RetryPhase::UploadChunk,
                      QStringLiteral("failed to dispatch Vimeo tus PATCH request"));
        return;
    }

    connect(m_chunkReply, &QNetworkReply::uploadProgress,
            this, &UploadClient::onChunkUploadProgress);
    connect(m_chunkReply, &QNetworkReply::finished,
            this, &UploadClient::onChunkReplyFinished);
}

void UploadClient::completeUpload() {
    if (m_uploadLink.isEmpty()) {
        failUpload(QStringLiteral("upload link is empty"));
        return;
    }

    clearReply(m_completeReply);

    QNetworkRequest request{QUrl(m_uploadLink)};
    request.setRawHeader("Tus-Resumable", "1.0.0");
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_accessToken.toUtf8());
    request.setRawHeader("Upload-Length", QByteArray::number(m_totalBytes));

    m_completeReply = m_nam->head(request);
    if (!m_completeReply) {
        scheduleRetry(RetryPhase::CompleteUpload,
                      QStringLiteral("failed to dispatch Vimeo upload completion check"));
        return;
    }

    connect(m_completeReply, &QNetworkReply::finished,
            this, &UploadClient::onCompleteReplyFinished);
}

QByteArray UploadClient::readNextChunk() {
    if (!m_file) {
        return {};
    }
    return m_file->read(kChunkSize);
}

QString UploadClient::privacyViewFor(const QString& privacy) const {
    const QString normalized = privacy.trimmed().toLower();
    if (normalized == QStringLiteral("public")) {
        return QStringLiteral("anybody");
    }
    if (normalized == QStringLiteral("private")) {
        return QStringLiteral("nobody");
    }
    return QStringLiteral("unlisted");
}

void UploadClient::clearReply(QPointer<QNetworkReply>& reply) {
    if (!reply) {
        return;
    }
    reply->abort();
    reply->deleteLater();
    reply.clear();
}

} // namespace upload
} // namespace vimeo
