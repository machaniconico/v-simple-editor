#include "oauth_mock_server.h"

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

namespace selftests {

namespace {

constexpr char kHeaderDelimiter[] = "\r\n\r\n";

QByteArray normalizedPath(const QByteArray& requestTarget) {
    const int queryPos = requestTarget.indexOf('?');
    return queryPos >= 0 ? requestTarget.left(queryPos) : requestTarget;
}

QString sessionKey(const QString& sessionUriOrPath) {
    const QUrl url(sessionUriOrPath);
    if (url.isValid() && !url.scheme().isEmpty()) {
        return url.path();
    }
    return sessionUriOrPath;
}

QString sessionKey(const QByteArray& requestTarget) {
    return QString::fromUtf8(normalizedPath(requestTarget));
}

QByteArray headerValue(const QHash<QByteArray, QByteArray>& headers,
                       const QByteArray& name) {
    return headers.value(name.toLower());
}

const char* reasonPhrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 302: return "Found";
    case 308: return "Resume Incomplete";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default:  return "Internal Server Error";
    }
}

bool parseContentRange(const QByteArray& header,
                       qint64* start,
                       qint64* end,
                       qint64* total,
                       bool* isStatusQuery) {
    if (start) *start = -1;
    if (end) *end = -1;
    if (total) *total = -1;
    if (isStatusQuery) *isStatusQuery = false;

    const QByteArray value = header.trimmed();
    if (!value.startsWith("bytes ")) {
        return false;
    }

    const QByteArray rangeAndTotal = value.mid(6);
    const int slashPos = rangeAndTotal.indexOf('/');
    if (slashPos <= 0) {
        return false;
    }

    bool totalOk = false;
    const qint64 parsedTotal =
        rangeAndTotal.mid(slashPos + 1).trimmed().toLongLong(&totalOk);
    if (!totalOk || parsedTotal < 0) {
        return false;
    }

    const QByteArray rangePart = rangeAndTotal.left(slashPos).trimmed();
    if (rangePart == "*") {
        if (total) {
            *total = parsedTotal;
        }
        if (isStatusQuery) {
            *isStatusQuery = true;
        }
        return true;
    }

    const int dashPos = rangePart.indexOf('-');
    if (dashPos <= 0) {
        return false;
    }

    bool startOk = false;
    bool endOk = false;
    const qint64 parsedStart =
        rangePart.left(dashPos).trimmed().toLongLong(&startOk);
    const qint64 parsedEnd =
        rangePart.mid(dashPos + 1).trimmed().toLongLong(&endOk);
    if (!startOk || !endOk || parsedStart < 0 || parsedEnd < parsedStart) {
        return false;
    }

    if (start) *start = parsedStart;
    if (end) *end = parsedEnd;
    if (total) *total = parsedTotal;
    return true;
}

QByteArray tokenPayload(int requestNumber, const QString& scope) {
    QJsonObject root;
    root.insert(QStringLiteral("access_token"),
                QStringLiteral("mock-access-token-%1").arg(requestNumber));
    root.insert(QStringLiteral("refresh_token"),
                QStringLiteral("mock-refresh-token-%1").arg(requestNumber));
    root.insert(QStringLiteral("expires_in"), 3600);
    root.insert(QStringLiteral("token_type"), QStringLiteral("Bearer"));
    root.insert(QStringLiteral("scope"), scope);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

OAuthMockServer::OAuthMockServer(QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this)) {
    connect(m_server, &QTcpServer::newConnection,
            this, &OAuthMockServer::onNewConnection);
}

OAuthMockServer::~OAuthMockServer() {
    stop();
}

bool OAuthMockServer::start(quint16 preferredPort) {
    stop();

    m_sessionBytes.clear();
    m_tokenRequests.clear();
    m_tokenRequestCount = 0;
    m_initiateRequestCount = 0;
    m_chunkPutCount = 0;
    m_tusPatchCount = 0;
    m_lastReceivedBytes = 0;
    m_instagramContainerCount = 0;
    m_instagramStatusCount = 0;
    m_instagramPublishCount = 0;
    m_xInitCount = 0;
    m_xAppendCount = 0;
    m_xFinalizeCount = 0;
    m_xStatusCount = 0;
    m_xTweetCount = 0;
    m_sessionCounter = 0;

    const bool listened = m_server->listen(QHostAddress::LocalHost, preferredPort)
        || (preferredPort != 0 && m_server->listen(QHostAddress::LocalHost, 0));
    if (!listened) {
        m_port = 0;
        return false;
    }

    m_port = m_server->serverPort();
    return true;
}

void OAuthMockServer::stop() {
    const auto sockets = m_pending.keys();
    for (QTcpSocket* socket : sockets) {
        if (!socket) {
            continue;
        }
        socket->disconnect(this);
        socket->close();
        socket->deleteLater();
    }

    m_pending.clear();
    if (m_server && m_server->isListening()) {
        m_server->close();
    }
    m_port = 0;
}

QString OAuthMockServer::baseUrl() const {
    if (!m_server || !m_server->isListening() || m_port == 0) {
        return QString();
    }
    return QStringLiteral("http://127.0.0.1:%1").arg(m_port);
}

qint64 OAuthMockServer::receivedBytesForSession(const QString& sessionUri) const {
    return m_sessionBytes.value(sessionKey(sessionUri), 0);
}

void OAuthMockServer::onNewConnection() {
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        if (!socket) {
            continue;
        }

        connect(socket, &QTcpSocket::readyRead,
                this, &OAuthMockServer::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected,
                this, &OAuthMockServer::onSocketDisconnected);
        m_pending.insert(socket, Request{});
    }
}

void OAuthMockServer::onSocketReadyRead() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) {
        return;
    }

    Request& req = m_pending[socket];
    req.buffer += socket->readAll();

    while (true) {
        if (!req.headersParsed) {
            const int headerEnd = req.buffer.indexOf(kHeaderDelimiter);
            if (headerEnd < 0) {
                return;
            }

            const QByteArray headerBlock = req.buffer.left(headerEnd);
            req.buffer.remove(0, headerEnd + 4);

            const QList<QByteArray> lines = headerBlock.split('\n');
            if (lines.isEmpty()) {
                writeResponse(socket, 400, QByteArray("empty request"));
                return;
            }

            const QList<QByteArray> requestLineParts =
                lines.first().trimmed().split(' ');
            if (requestLineParts.size() < 2) {
                writeResponse(socket, 400, QByteArray("invalid request line"));
                return;
            }

            req.method = requestLineParts.at(0).trimmed().toUpper();
            req.path = requestLineParts.at(1).trimmed();
            req.headers.clear();
            req.body.clear();
            req.contentLength = 0;
            req.headersParsed = true;

            for (int i = 1; i < lines.size(); ++i) {
                const QByteArray line = lines.at(i).trimmed();
                if (line.isEmpty()) {
                    continue;
                }
                const int colon = line.indexOf(':');
                if (colon <= 0) {
                    continue;
                }

                const QByteArray name = line.left(colon).trimmed().toLower();
                const QByteArray value = line.mid(colon + 1).trimmed();
                req.headers.insert(name, value);
            }

            bool ok = false;
            const int contentLength =
                headerValue(req.headers, "content-length").toInt(&ok);
            if (ok && contentLength >= 0) {
                req.contentLength = contentLength;
            }
        }

        if (req.contentLength < 0) {
            writeResponse(socket, 400, QByteArray("invalid content-length"));
            return;
        }
        if (req.buffer.size() < req.contentLength) {
            return;
        }

        req.body = req.buffer.left(req.contentLength);
        req.buffer.remove(0, req.contentLength);

        const Request completed = req;
        req = Request{};
        req.buffer = completed.buffer;
        handleRequest(socket, completed);
        return;
    }
}

void OAuthMockServer::onSocketDisconnected() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) {
        return;
    }
    m_pending.remove(socket);
    socket->deleteLater();
}

void OAuthMockServer::handleRequest(QTcpSocket* socket, const Request& req) {
    const QByteArray method = req.method;
    const QByteArray path = req.path;
    const QByteArray pathOnly = normalizedPath(req.path);

    if ((method == "POST") &&
        (pathOnly == "/token" ||
         pathOnly == "/oauth/access_token" ||
         pathOnly == "/oauth/authorize/client")) {
        routeToken(socket, req);
        return;
    }
    if (method == "GET" &&
        (pathOnly == "/o/oauth2/v2/auth" || pathOnly == "/oauth/authorize")) {
        routeAuthorize(socket, req);
        return;
    }
    if (method == "POST" && pathOnly.startsWith("/upload/youtube/v3/videos")) {
        routeYoutubeInitiate(socket, req);
        return;
    }
    if (method == "PUT" && pathOnly.startsWith("/mock-session/yt/")) {
        routeYoutubeChunk(socket, req);
        return;
    }
    if (method == "POST" && pathOnly == "/me/videos") {
        routeVimeoCreate(socket, req);
        return;
    }
    if ((method == "PATCH" || method == "HEAD") &&
        pathOnly.startsWith("/mock-session/vimeo/")) {
        routeVimeoTus(socket, req);
        return;
    }
    if (method == "POST" && path.contains("/media_publish")) {
        routeInstagramPublish(socket, req);
        return;
    }
    if (method == "POST" &&
        path.contains("/media") &&
        !path.contains("/upload/youtube") &&
        !path.contains("/me/videos") &&
        !path.contains("/media/upload.json")) {
        routeInstagramContainer(socket, req);
        return;
    }
    if (method == "GET" && path.contains("fields=status_code")) {
        routeInstagramStatus(socket, req);
        return;
    }
    if (path.contains("/media/upload.json")) {
        if (path.contains("command=INIT") || req.body.contains("command=INIT")) {
            routeXInit(socket, req);
            return;
        }
        if (path.contains("command=FINALIZE") || req.body.contains("command=FINALIZE")) {
            routeXFinalize(socket, req);
            return;
        }
        if (method == "GET" && path.contains("command=STATUS")) {
            routeXStatus(socket, req);
            return;
        }
        if (method == "POST") {
            routeXAppend(socket, req);
            return;
        }
    }
    if (method == "POST" && path.endsWith("/tweets")) {
        routeXCreateTweet(socket, req);
        return;
    }

    writeResponse(socket, 404, QByteArray("not found"));
}

void OAuthMockServer::writeResponse(QTcpSocket* socket,
                                    int status,
                                    const QByteArray& body,
                                    const QHash<QByteArray, QByteArray>& extraHeaders) {
    if (!socket) {
        return;
    }

    QByteArray response;
    response += "HTTP/1.1 ";
    response += QByteArray::number(status);
    response += ' ';
    response += reasonPhrase(status);
    response += "\r\n";

    QHash<QByteArray, QByteArray> headers = extraHeaders;
    if (!headers.contains("Content-Length")) {
        headers.insert("Content-Length", QByteArray::number(body.size()));
    }
    if (!headers.contains("Content-Type")) {
        headers.insert("Content-Type",
                       body.startsWith("{") ? "application/json" : "text/plain; charset=UTF-8");
    }
    headers.insert("Connection", "close");

    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        response += it.key();
        response += ": ";
        response += it.value();
        response += "\r\n";
    }
    response += "\r\n";
    response += body;

    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

void OAuthMockServer::routeToken(QTcpSocket* socket, const Request& req) {
    ++m_tokenRequestCount;
    QByteArray rawRequest = req.method + ' ' + req.path + '\n' + req.body;
    m_tokenRequests.append(rawRequest);
    const QString scope = normalizedPath(req.path) == "/oauth/access_token"
        || normalizedPath(req.path) == "/oauth/authorize/client"
        ? QStringLiteral("private public video_files")
        : QStringLiteral("youtube.upload");

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Content-Type", "application/json");
    writeResponse(socket, 200, tokenPayload(m_tokenRequestCount, scope), headers);
}

void OAuthMockServer::routeAuthorize(QTcpSocket* socket, const Request& req) {
    const QUrl requestUrl =
        QUrl::fromEncoded(QByteArray("http://127.0.0.1") + req.path);
    const QUrlQuery query(requestUrl);

    QUrl redirect;
    const QString redirectUri = query.queryItemValue(QStringLiteral("redirect_uri"));
    if (!redirectUri.isEmpty()) {
        redirect = QUrl(redirectUri);
    }
    if (!redirect.isValid() || redirect.isEmpty()) {
        redirect = QUrl(baseUrl() + QStringLiteral("/oauth/callback"));
    }

    QUrlQuery redirectQuery(redirect);
    redirectQuery.addQueryItem(QStringLiteral("code"),
                               QStringLiteral("mock-authorization-code"));
    const QString state = query.queryItemValue(QStringLiteral("state"));
    if (!state.isEmpty()) {
        redirectQuery.addQueryItem(QStringLiteral("state"), state);
    }
    redirect.setQuery(redirectQuery);

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Location", redirect.toString(QUrl::FullyEncoded).toUtf8());
    writeResponse(socket, 302, QByteArray(), headers);
}

void OAuthMockServer::routeYoutubeInitiate(QTcpSocket* socket, const Request& req) {
    Q_UNUSED(req);

    ++m_initiateRequestCount;
    const int sessionId = ++m_sessionCounter;
    const QString sessionUri =
        QStringLiteral("%1/mock-session/yt/%2").arg(baseUrl()).arg(sessionId);
    m_sessionBytes.insert(sessionKey(sessionUri), 0);

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Location", sessionUri.toUtf8());
    writeResponse(socket, 200, QByteArray(), headers);
}

void OAuthMockServer::routeYoutubeChunk(QTcpSocket* socket, const Request& req) {
    qint64 start = -1;
    qint64 end = -1;
    qint64 total = -1;
    bool isStatusQuery = false;
    if (!parseContentRange(headerValue(req.headers, "content-range"),
                           &start, &end, &total, &isStatusQuery)) {
        writeResponse(socket, 400, QByteArray("invalid content-range"));
        return;
    }

    const QString key = sessionKey(req.path);
    const qint64 currentBytes = m_sessionBytes.value(key, 0);

    if (isStatusQuery) {
        QHash<QByteArray, QByteArray> headers;
        if (currentBytes > 0) {
            headers.insert("Range",
                           QByteArray("bytes=0-")
                               + QByteArray::number(currentBytes - 1));
        }
        if (currentBytes >= total) {
            headers.insert("Content-Type", "application/json");
            writeResponse(socket, 200,
                          QByteArray("{\"id\":\"mock-yt-video-id\"}"),
                          headers);
            return;
        }
        writeResponse(socket, 308, QByteArray(), headers);
        return;
    }

    ++m_chunkPutCount;
    const qint64 chunkBytes = static_cast<qint64>(req.body.size());
    const qint64 receivedBytes = chunkBytes > 0 ? chunkBytes : (end - start + 1);
    m_lastReceivedBytes = receivedBytes;
    const qint64 updatedBytes = currentBytes + receivedBytes;
    const qint64 committedBytes = updatedBytes > (end + 1)
        ? updatedBytes
        : (end + 1);
    m_sessionBytes.insert(key, committedBytes);

    if (end + 1 >= total) {
        QHash<QByteArray, QByteArray> headers;
        headers.insert("Content-Type", "application/json");
        writeResponse(socket, 200,
                      QByteArray("{\"id\":\"mock-yt-video-id\"}"),
                      headers);
        return;
    }

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Range", QByteArray("bytes=0-") + QByteArray::number(end));
    writeResponse(socket, 308, QByteArray(), headers);
}

void OAuthMockServer::routeVimeoCreate(QTcpSocket* socket, const Request& req) {
    Q_UNUSED(req);

    ++m_initiateRequestCount;
    const int sessionId = ++m_sessionCounter;
    const QString uploadLink =
        QStringLiteral("%1/mock-session/vimeo/%2").arg(baseUrl()).arg(sessionId);
    m_sessionBytes.insert(sessionKey(uploadLink), 0);

    QJsonObject upload;
    upload.insert(QStringLiteral("approach"), QStringLiteral("tus"));
    upload.insert(QStringLiteral("upload_link"), uploadLink);
    upload.insert(QStringLiteral("upload_link_secure"), uploadLink);

    QJsonObject root;
    root.insert(QStringLiteral("uri"), QStringLiteral("/videos/mock-vimeo-id"));
    root.insert(QStringLiteral("upload"), upload);

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Content-Type", "application/json");
    writeResponse(socket, 201,
                  QJsonDocument(root).toJson(QJsonDocument::Compact),
                  headers);
}

void OAuthMockServer::routeVimeoTus(QTcpSocket* socket, const Request& req) {
    const QString key = sessionKey(req.path);
    const qint64 currentBytes = m_sessionBytes.value(key, 0);

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Tus-Resumable", "1.0.0");

    if (req.method == "HEAD") {
        headers.insert("Upload-Offset", QByteArray::number(currentBytes));
        writeResponse(socket, 204, QByteArray(), headers);
        return;
    }

    ++m_tusPatchCount;
    bool offsetOk = false;
    const qint64 uploadOffset =
        headerValue(req.headers, "upload-offset").toLongLong(&offsetOk);
    if (!offsetOk || uploadOffset < 0) {
        writeResponse(socket, 400, QByteArray("invalid upload-offset"), headers);
        return;
    }

    const qint64 receivedBytes = static_cast<qint64>(req.body.size());
    m_lastReceivedBytes = receivedBytes;
    const qint64 newOffset = uploadOffset + receivedBytes;
    const qint64 committedOffset = newOffset > currentBytes
        ? newOffset
        : currentBytes;
    m_sessionBytes.insert(key, committedOffset);

    headers.insert("Upload-Offset", QByteArray::number(committedOffset));
    writeResponse(socket, 204, QByteArray(), headers);
}

void OAuthMockServer::routeInstagramContainer(QTcpSocket* socket, const Request& req) {
    Q_UNUSED(req);

    ++m_instagramContainerCount;
    const QByteArray response =
        "{\"id\": \"mock_ig_creation_"
        + QByteArray::number(m_instagramContainerCount)
        + "\"}";
    writeResponse(socket, 200, response, {{"Content-Type", "application/json"}});
}

void OAuthMockServer::routeInstagramStatus(QTcpSocket* socket, const Request& req) {
    Q_UNUSED(req);

    ++m_instagramStatusCount;
    const QByteArray status =
        (m_instagramStatusCount >= 2) ? "FINISHED" : "IN_PROGRESS";
    const QByteArray response =
        "{\"status_code\": \"" + status + "\"}";
    writeResponse(socket, 200, response, {{"Content-Type", "application/json"}});
}

void OAuthMockServer::routeInstagramPublish(QTcpSocket* socket, const Request& req) {
    Q_UNUSED(req);

    ++m_instagramPublishCount;
    const QByteArray response =
        "{\"id\": \"mock_ig_media_"
        + QByteArray::number(m_instagramPublishCount)
        + "\"}";
    writeResponse(socket, 200, response, {{"Content-Type", "application/json"}});
}

void OAuthMockServer::routeXInit(QTcpSocket* socket, const Request& req) {
    Q_UNUSED(req);

    ++m_xInitCount;
    const QByteArray response =
        "{\"media_id_string\": \"mock_x_media_"
        + QByteArray::number(m_xInitCount)
        + "\"}";
    writeResponse(socket, 200, response, {{"Content-Type", "application/json"}});
}

void OAuthMockServer::routeXAppend(QTcpSocket* socket, const Request& req) {
    ++m_xAppendCount;
    m_lastReceivedBytes += req.body.size();
    writeResponse(socket, 204, QByteArray(), {});
}

void OAuthMockServer::routeXFinalize(QTcpSocket* socket, const Request& req) {
    Q_UNUSED(req);

    ++m_xFinalizeCount;
    const QByteArray response =
        "{\"media_id_string\": \"mock_x_media_finalized\"}";
    writeResponse(socket, 200, response, {{"Content-Type", "application/json"}});
}

void OAuthMockServer::routeXStatus(QTcpSocket* socket, const Request& req) {
    Q_UNUSED(req);

    ++m_xStatusCount;
    const QByteArray response =
        "{\"processing_info\": {\"state\": \"succeeded\", \"check_after_secs\": 1}}";
    writeResponse(socket, 200, response, {{"Content-Type", "application/json"}});
}

void OAuthMockServer::routeXCreateTweet(QTcpSocket* socket, const Request& req) {
    Q_UNUSED(req);

    ++m_xTweetCount;
    const QByteArray response =
        "{\"data\": {\"id\": \"mock_tweet_"
        + QByteArray::number(m_xTweetCount)
        + "\"}}";
    writeResponse(socket, 201, response, {{"Content-Type", "application/json"}});
}

} // namespace selftests
