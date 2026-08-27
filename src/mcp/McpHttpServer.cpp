#include "McpHttpServer.h"

#include "McpToolRegistry.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QHostAddress>
#include <QPointer>
#include <QSettings>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace mcp {

namespace {

constexpr int kMaxContentLength = 4 * 1024 * 1024;
constexpr qsizetype kMaxHeaderBytes = 64 * 1024;
constexpr int kMaxConnections = 16;
constexpr qint64 kIdleTimeoutMs = 30 * 1000;
constexpr char kHeaderDelimiter[] = "\r\n\r\n";

QByteArray normalizedPath(const QByteArray& requestTarget)
{
    const int queryPos = requestTarget.indexOf('?');
    return queryPos >= 0 ? requestTarget.left(queryPos) : requestTarget;
}

QByteArray headerValue(const QHash<QByteArray, QByteArray>& headers,
                       const QByteArray& name)
{
    return headers.value(name.toLower());
}

// 0 = Origin ヘッダ無し (許可), 1 = 許可 Origin, -1 = 拒否
int originVerdict(const QByteArray& origin)
{
    if (origin.isEmpty())
        return 0;

    const QUrl url = QUrl::fromEncoded(origin);
    const QString scheme = url.scheme().toLower();
    const QString host = url.host().toLower();
    const bool allowedScheme = scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https");
    const bool allowedHost = host == QStringLiteral("localhost")
        || host == QStringLiteral("127.0.0.1")
        || host == QStringLiteral("::1");
    return url.isValid() && allowedScheme && allowedHost ? 1 : -1;
}

QHash<QByteArray, QByteArray> corsHeaders(const QByteArray& origin)
{
    QHash<QByteArray, QByteArray> headers;
    if (originVerdict(origin) == 1) {
        headers.insert("Access-Control-Allow-Origin", origin);
        headers.insert("Vary", "Origin");
    }
    return headers;
}

bool isJsonContentType(const QByteArray& value)
{
    const int semicolon = value.indexOf(';');
    const QByteArray mediaType = (semicolon >= 0 ? value.left(semicolon) : value)
        .trimmed().toLower();
    return mediaType == "application/json";
}

const char* reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 415: return "Unsupported Media Type";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

bool hasCloseToken(const QByteArray& value)
{
    const QList<QByteArray> values = value.split(',');
    for (const QByteArray& item : values) {
        if (item.trimmed().toLower() == "close")
            return true;
    }
    return false;
}

QString toolCallName(const QByteArray& body)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject())
        return QString();

    const QJsonObject request = document.object();
    if (request.value(QStringLiteral("method")).toString()
        != QStringLiteral("tools/call")) {
        return QString();
    }
    return request.value(QStringLiteral("params")).toObject()
        .value(QStringLiteral("name")).toString();
}

bool successfulToolCall(const QByteArray& response)
{
    if (response.isEmpty())
        return true;

    const QJsonDocument document = QJsonDocument::fromJson(response);
    if (!document.isObject())
        return false;
    const QJsonObject root = document.object();
    if (root.contains(QStringLiteral("error")))
        return false;
    return !root.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("isError")).toBool(false);
}

} // namespace

McpHttpServer::McpHttpServer(McpToolRegistry* registry, QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_idleTimer(new QTimer(this))
    , m_registry(registry)
    , m_protocol(registry, ServerInfo{})
{
    connect(m_server, &QTcpServer::newConnection,
            this, &McpHttpServer::onNewConnection);
    m_idleTimer->setInterval(1000);
    connect(m_idleTimer, &QTimer::timeout,
            this, &McpHttpServer::onIdleTimeout);
    m_idleTimer->start();
}

McpHttpServer::~McpHttpServer()
{
    stop();
}

QString McpHttpServer::resolveToken()
{
    // 起動のたびにトークンを作り直すと、ユーザが Claude Code / Codex の設定ファイルに
    // 貼ったトークンが次回起動で無効になる。保存して再利用し、明示的に
    // regenerateToken() が呼ばれたときだけ作り直す。
    // 環境変数は自動テストや、設定ファイルを固定したいユーザ向けの上書き口。
    const QString override =
        qEnvironmentVariable("VEDITOR_MCP_TOKEN").trimmed();
    if (!override.isEmpty())
        return override;

    QSettings settings(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
    const QString stored = settings.value(QStringLiteral("mcpToken")).toString().trimmed();
    if (!stored.isEmpty())
        return stored;

    const QString fresh = QString::fromLatin1(generateToken());
    settings.setValue(QStringLiteral("mcpToken"), fresh);
    return fresh;
}

void McpHttpServer::regenerateToken()
{
    QSettings settings(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
    settings.remove(QStringLiteral("mcpToken"));
    if (!qEnvironmentVariableIsEmpty("VEDITOR_MCP_TOKEN")) {
        // 環境変数が勝つ設計なので、そのままでは作り直しても変わらない。
        // 呼び出し側が UI で説明できるよう、ここでは何もしない。
        return;
    }
    m_token = resolveToken();
}

bool McpHttpServer::start(quint16 preferredPort)
{
    stop();

    m_token = resolveToken();

    // ポートも環境変数で固定できるようにする (設定ファイルを使い回すため)。
    bool portOverrideOk = false;
    const int portOverride =
        qEnvironmentVariable("VEDITOR_MCP_PORT").toInt(&portOverrideOk);
    if (portOverrideOk && portOverride > 0 && portOverride <= 65535)
        preferredPort = static_cast<quint16>(portOverride);

    for (int attempt = 0; attempt < 20; ++attempt) {
        const quint32 candidate = static_cast<quint32>(preferredPort) + attempt;
        if (candidate > 65535)
            break;
        if (m_server->listen(QHostAddress::LocalHost,
                             static_cast<quint16>(candidate))) {
            m_port = m_server->serverPort();
            emit started(m_port);
            return true;
        }
    }

    m_port = 0;
    m_token.clear();   // 保存済みトークン自体は QSettings に残る (次回 start で復帰)
    return false;
}

void McpHttpServer::stop()
{
    const auto sockets = m_pending.keys();
    for (QTcpSocket* socket : sockets) {
        if (!socket)
            continue;
        socket->disconnect(this);
        socket->close();
        if (!m_processingSockets.contains(socket))
            socket->deleteLater();
    }
    m_pending.clear();

    if (m_server && m_server->isListening())
        m_server->close();

    const bool hadState = m_port != 0 || !m_token.isEmpty();
    m_port = 0;
    m_token.clear();
    if (hadState)
        emit stopped();
}

bool McpHttpServer::isRunning() const
{
    return m_server && m_server->isListening();
}

QString McpHttpServer::endpointUrl() const
{
    if (!isRunning() || m_port == 0)
        return QString();
    return QStringLiteral("http://127.0.0.1:%1/mcp").arg(m_port);
}

int McpHttpServer::maxConnections()
{
    return kMaxConnections;
}

void McpHttpServer::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        if (!socket)
            continue;

        if (m_pending.size() >= kMaxConnections) {
            connect(socket, &QTcpSocket::disconnected,
                    socket, &QObject::deleteLater);
            writeResponse(socket, 503,
                          QByteArray("{\"error\":\"too many connections\"}"),
                          {{"Content-Type", "application/json; charset=utf-8"},
                           {"Retry-After", "1"}},
                          true);
            QTimer::singleShot(5000, socket, &QObject::deleteLater);
            continue;
        }

        connect(socket, &QTcpSocket::readyRead,
                this, &McpHttpServer::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected,
                this, &McpHttpServer::onSocketDisconnected);
        Request request;
        request.lastReceivedMs = QDateTime::currentMSecsSinceEpoch();
        m_pending.insert(socket, request);
    }
}

void McpHttpServer::onSocketReadyRead()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || !m_pending.contains(socket)
        || m_processingSockets.contains(socket))
        return;

    QPointer<QTcpSocket> guardedSocket(socket);
    m_processingSockets.insert(socket);
    Request request = m_pending.value(socket);
    const QByteArray received = socket->readAll();
    if (!received.isEmpty()) {
        request.buffer += received;
        request.lastReceivedMs = QDateTime::currentMSecsSinceEpoch();
    }

    const auto finishProcessing = [&]() {
        m_processingSockets.remove(socket);
        if (guardedSocket && !m_pending.contains(socket)
            && guardedSocket->state() != QAbstractSocket::ConnectedState) {
            guardedSocket->deleteLater();
        }
    };

    while (true) {
        if (!request.headersParsed) {
            const qsizetype headerEnd = request.buffer.indexOf(
                kHeaderDelimiter, request.headerScanPosition);
            if (headerEnd < 0)
            {
                request.headerScanPosition = qMax<qsizetype>(
                    0, request.buffer.size() - 3);
                if (request.buffer.size() > kMaxHeaderBytes) {
                    writeResponse(socket, 431,
                                  QByteArray("request headers too large"),
                                  {}, true);
                    finishProcessing();
                    return;
                }
                m_pending.insert(socket, request);
                finishProcessing();
                return;
            }

            if (headerEnd + 4 > kMaxHeaderBytes) {
                writeResponse(socket, 431,
                              QByteArray("request headers too large"),
                              {}, true);
                finishProcessing();
                return;
            }

            const QByteArray headerBlock = request.buffer.left(headerEnd);
            request.buffer.remove(0, headerEnd + 4);

            const QList<QByteArray> lines = headerBlock.split('\n');
            if (lines.isEmpty()) {
                writeResponse(socket, 400, QByteArray("empty request"), {}, true);
                finishProcessing();
                return;
            }

            const QList<QByteArray> requestLineParts =
                lines.first().trimmed().split(' ');
            if (requestLineParts.size() < 2) {
                writeResponse(socket, 400, QByteArray("invalid request line"), {}, true);
                finishProcessing();
                return;
            }

            request.method = requestLineParts.at(0).trimmed().toUpper();
            request.path = requestLineParts.at(1).trimmed();
            request.headers.clear();
            request.body.clear();
            request.contentLength = 0;
            request.headersParsed = true;
            request.headerScanPosition = 0;

            bool hasContentLength = false;
            for (int i = 1; i < lines.size(); ++i) {
                const QByteArray line = lines.at(i).trimmed();
                if (line.isEmpty())
                    continue;
                const int colon = line.indexOf(':');
                if (colon <= 0)
                    continue;

                const QByteArray name = line.left(colon).trimmed().toLower();
                const QByteArray value = line.mid(colon + 1).trimmed();
                if (name == "content-length") {
                    if (hasContentLength) {
                        writeResponse(socket, 400,
                                      QByteArray("duplicate content-length"),
                                      {}, true);
                        finishProcessing();
                        return;
                    }
                    hasContentLength = true;
                }
                if (name == "transfer-encoding"
                    && value.toLower() != "identity") {
                    writeResponse(socket, 501,
                                  QByteArray("transfer-encoding not implemented"),
                                  {}, true);
                    finishProcessing();
                    return;
                }
                request.headers.insert(name, value);
            }

            bool lengthOk = false;
            const QByteArray lengthHeader = headerValue(request.headers,
                                                        "content-length");
            const qint64 contentLength = lengthHeader.toLongLong(&lengthOk);
            if (hasContentLength && (!lengthOk || contentLength < 0)) {
                writeResponse(socket, 400, QByteArray("invalid content-length"),
                              {}, true);
                finishProcessing();
                return;
            }
            if (contentLength > kMaxContentLength) {
                writeResponse(socket, 413, QByteArray("payload too large"), {}, true);
                finishProcessing();
                return;
            }
            if (lengthOk)
                request.contentLength = static_cast<int>(contentLength);
        }

        if (request.buffer.size() < request.contentLength)
        {
            m_pending.insert(socket, request);
            finishProcessing();
            return;
        }

        request.body = request.buffer.left(request.contentLength);
        request.buffer.remove(0, request.contentLength);

        const Request completed = request;
        request = Request{};
        request.buffer = completed.buffer;
        handleRequest(socket, completed);

        if (!guardedSocket || guardedSocket->state() != QAbstractSocket::ConnectedState
            || !m_pending.contains(socket)) {
            finishProcessing();
            return;
        }
        const QByteArray receivedWhileHandling = guardedSocket->readAll();
        if (!receivedWhileHandling.isEmpty()) {
            request.buffer += receivedWhileHandling;
            request.lastReceivedMs = QDateTime::currentMSecsSinceEpoch();
        }
        // handleRequest() may enter a nested event loop.  Only restore this
        // local copy after it returns; never keep a reference into m_pending.
        m_pending.insert(socket, request);
        if (request.buffer.isEmpty()) {
            finishProcessing();
            return;
        }
    }
}

void McpHttpServer::onSocketDisconnected()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;
    m_pending.remove(socket);
    if (!m_processingSockets.contains(socket))
        socket->deleteLater();
}

void McpHttpServer::onIdleTimeout()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto sockets = m_pending.keys();
    for (QTcpSocket* socket : sockets) {
        if (!socket)
            continue;
        if (m_processingSockets.contains(socket))
            continue;
        const Request request = m_pending.value(socket);
        if (request.lastReceivedMs > 0
            && now - request.lastReceivedMs >= kIdleTimeoutMs) {
            socket->disconnectFromHost();
        }
    }
}

void McpHttpServer::handleRequest(QTcpSocket* socket, const Request& request)
{
    QPointer<QTcpSocket> guardedSocket(socket);
    const QByteArray path = normalizedPath(request.path);
    const bool closeConnection = hasCloseToken(headerValue(request.headers,
                                                           "connection"));

    if (path != "/mcp") {
        writeResponse(socket, 404, QByteArray("not found"), {}, closeConnection);
        return;
    }

    const QByteArray origin = headerValue(request.headers, "origin");
    if (originVerdict(origin) < 0) {
        writeResponse(socket, 403, QByteArray("{\"error\":\"forbidden origin\"}"),
                      {{"Content-Type", "application/json; charset=utf-8"}},
                      closeConnection);
        return;
    }

    if (request.method == "GET") {
        writeResponse(socket, 405, QByteArray(),
                      {{"Allow", "POST, OPTIONS"}}, closeConnection);
        return;
    }

    if (request.method == "OPTIONS") {
        QHash<QByteArray, QByteArray> headers = corsHeaders(origin);
        headers.insert("Access-Control-Allow-Headers",
                       "content-type, authorization, mcp-protocol-version");
        headers.insert("Access-Control-Allow-Methods", "POST, OPTIONS");
        writeResponse(socket, 204, QByteArray(),
                      headers,
                      closeConnection);
        return;
    }

    if (request.method != "POST") {
        writeResponse(socket, 404, QByteArray("not found"), {}, closeConnection);
        return;
    }

    if (!authorized(request)) {
        QHash<QByteArray, QByteArray> headers = corsHeaders(origin);
        headers.insert("Content-Type", "application/json; charset=utf-8");
        writeResponse(socket, 401, QByteArray("{\"error\":\"unauthorized\"}"),
                      headers,
                      closeConnection);
        return;
    }

    const QByteArray contentType = headerValue(request.headers, "content-type");
    if (!isJsonContentType(contentType)) {
        QHash<QByteArray, QByteArray> headers = corsHeaders(origin);
        headers.insert("Content-Type", "application/json; charset=utf-8");
        writeResponse(socket, 415,
                      QByteArray("{\"error\":\"content-type must be application/json\"}"),
                      headers,
                      closeConnection);
        return;
    }

    const QByteArray mcpVersion = headerValue(request.headers,
                                              "mcp-protocol-version");
    if (!mcpVersion.isEmpty()
        && !McpProtocol::isSupportedProtocolVersion(
            QString::fromLatin1(mcpVersion))) {
        QJsonObject errorBody;
        errorBody.insert(QStringLiteral("error"),
                         QStringLiteral("unsupported MCP-Protocol-Version: %1")
                             .arg(QString::fromLatin1(mcpVersion)));
        QHash<QByteArray, QByteArray> headers = corsHeaders(origin);
        headers.insert("Content-Type", "application/json; charset=utf-8");
        writeResponse(socket, 400,
                      QJsonDocument(errorBody).toJson(QJsonDocument::Compact),
                      headers,
                      closeConnection);
        return;
    }

    if (!guardedSocket
        || guardedSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    const QString calledTool = toolCallName(request.body);
    const QByteArray response = m_protocol.handleMessage(request.body);
    if (!guardedSocket
        || guardedSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    if (!calledTool.isEmpty())
        emit toolCalled(calledTool, successfulToolCall(response));

    if (response.isEmpty()) {
        QHash<QByteArray, QByteArray> headers = corsHeaders(origin);
        writeResponse(socket, 202, QByteArray(), headers, closeConnection);
        return;
    }

    QHash<QByteArray, QByteArray> headers = corsHeaders(origin);
    headers.insert("Content-Type", "application/json; charset=utf-8");
    writeResponse(socket, 200, response,
                  headers,
                  closeConnection);
}

void McpHttpServer::writeResponse(QTcpSocket* socket, int status,
                                  const QByteArray& body,
                                  const QHash<QByteArray, QByteArray>& extraHeaders,
                                  bool closeConnection)
{
    QPointer<QTcpSocket> guardedSocket(socket);
    if (!guardedSocket
        || guardedSocket->state() != QAbstractSocket::ConnectedState)
        return;

    QByteArray response;
    response += "HTTP/1.1 ";
    response += QByteArray::number(status);
    response += ' ';
    response += reasonPhrase(status);
    response += "\r\n";

    QHash<QByteArray, QByteArray> headers = extraHeaders;
    headers.insert("Content-Length", QByteArray::number(body.size()));
    if (closeConnection)
        headers.insert("Connection", "close");

    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        response += it.key();
        response += ": ";
        response += it.value();
        response += "\r\n";
    }
    response += "\r\n";
    response += body;

    guardedSocket->write(response);
    guardedSocket->flush();
    if (closeConnection)
        guardedSocket->disconnectFromHost();
}

bool McpHttpServer::authorized(const Request& request) const
{
    const QByteArray authorization = headerValue(request.headers,
                                                 "authorization");
    if (authorization.startsWith("Bearer ")) {
        const QByteArray bearerToken = authorization.mid(7).trimmed();
        if (constantTimeEquals(bearerToken, m_token.toLatin1()))
            return true;
    }

    const QUrl url = QUrl::fromEncoded(QByteArray("http://127.0.0.1")
                                       + request.path);
    const QByteArray queryToken = QUrlQuery(url).queryItemValue(
        QStringLiteral("token")).toUtf8();
    return constantTimeEquals(queryToken, m_token.toLatin1());
}

bool McpHttpServer::constantTimeEquals(const QByteArray& lhs,
                                       const QByteArray& rhs)
{
    const int maxLength = qMax(lhs.size(), rhs.size());
    unsigned char difference = static_cast<unsigned char>(lhs.size() ^ rhs.size());
    for (int i = 0; i < maxLength; ++i) {
        const unsigned char left = i < lhs.size()
            ? static_cast<unsigned char>(lhs.at(i)) : 0;
        const unsigned char right = i < rhs.size()
            ? static_cast<unsigned char>(rhs.at(i)) : 0;
        difference = static_cast<unsigned char>(difference | (left ^ right));
    }
    return difference == 0;
}

QByteArray McpHttpServer::generateToken()
{
    quint32 randomBytes[4] = {};
    QRandomGenerator::system()->generate(randomBytes, randomBytes + 4);

    QByteArray token;
    token.reserve(32);
    for (quint32 value : randomBytes)
        token += QByteArray::number(value, 16).rightJustified(8, '0');
    return token;
}

} // namespace mcp
