#include <QEventLoop>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>
#include <QStringList>
#include <QSet>
#include <QDebug>
#include <QtGlobal>

#include <cmath>
#include <limits>
#include <stdexcept>

#include "../mcp/McpEditorTools.h"
#include "../mcp/McpHttpServer.h"
#include "../mcp/McpProtocol.h"
#include "../mcp/McpStdioBridge.h"
#include "../mcp/McpToolRegistry.h"
#include "../AiChatDock.h"
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../UndoManager.h"

namespace {

QJsonObject parseObject(const QByteArray& response)
{
    return QJsonDocument::fromJson(response).object();
}

QJsonObject rpcRequest(int id, const QString& method,
                       const QJsonObject& params = {})
{
    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("id"), id);
    request.insert(QStringLiteral("method"), method);
    if (!params.isEmpty())
        request.insert(QStringLiteral("params"), params);
    return request;
}

QByteArray compact(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

struct HttpResult {
    int status = 0;
    QByteArray body;
    QByteArray allowOrigin;
};

HttpResult get(QNetworkAccessManager* manager, const QUrl& url)
{
    QNetworkReply* reply = manager->get(QNetworkRequest(url));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, [&loop, reply]() {
        if (reply->isRunning())
            reply->abort();
        loop.quit();
    });
    loop.exec();

    HttpResult result;
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    reply->deleteLater();
    return result;
}

HttpResult post(QNetworkAccessManager* manager, const QUrl& url,
                const QByteArray& body, const QByteArray& token = {},
                const QHash<QByteArray, QByteArray>& extraHeaders = {},
                const QByteArray& contentType = "application/json")
{
    QNetworkRequest request(url);
    request.setRawHeader("Content-Type", contentType);
    if (!token.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + token);
    for (auto it = extraHeaders.constBegin(); it != extraHeaders.constEnd(); ++it)
        request.setRawHeader(it.key(), it.value());

    QNetworkReply* reply = manager->post(request, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, [&loop, reply]() {
        if (reply->isRunning())
            reply->abort();
        loop.quit();
    });
    loop.exec();

    HttpResult result;
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    result.allowOrigin = reply->rawHeader("Access-Control-Allow-Origin");
    reply->deleteLater();
    return result;
}

int rawHttpStatus(quint16 port, const QByteArray& request)
{
    // McpHttpServer はこの selftest と同じスレッドのイベントループ上で動く。
    // QTcpSocket::waitForConnected() / waitForReadyRead() はそのソケットの
    // イベントしか回さないため、サーバ側は接続を accept することすらできず
    // 必ずタイムアウトする。フルのイベントループを回すこと。
    QTcpSocket socket;
    QByteArray response;
    QEventLoop loop;

    QObject::connect(&socket, &QTcpSocket::connected, &socket,
                     [&socket, &request]() { socket.write(request); });
    QObject::connect(&socket, &QTcpSocket::readyRead, &socket,
                     [&socket, &response, &loop]() {
        response += socket.readAll();
        if (response.indexOf("\r\n\r\n") >= 0)
            loop.quit();
    });
    QObject::connect(&socket, &QTcpSocket::disconnected, &socket,
                     [&loop]() { loop.quit(); });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);

    socket.connectToHost(QHostAddress::LocalHost, port);
    loop.exec();
    response += socket.readAll();
    const int firstSpace = response.indexOf(' ');
    if (firstSpace < 0)
        return 0;
    const int secondSpace = response.indexOf(' ', firstSpace + 1);
    if (secondSpace < 0)
        return 0;
    bool ok = false;
    const int status = response.mid(firstSpace + 1,
                                    secondSpace - firstSpace - 1).toInt(&ok);
    return ok ? status : 0;
}

} // namespace

int runMcpSelftest()
{
    qInfo().noquote() << "[mcp] selftest start";
    int passed = 0;
    int failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[mcp] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& message) {
        ++failed;
        qWarning().noquote() << "[mcp] FAIL" << name << ":" << message;
    };
    auto toolResult = [](const QJsonObject& response) {
        return response.value(QStringLiteral("result")).toObject();
    };
    auto toolPayload = [&toolResult](const QJsonObject& response) {
        return toolResult(response).value(QStringLiteral("structuredContent")).toObject();
    };

    mcp::McpToolRegistry registry;
    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    QJsonObject properties;
    properties.insert(QStringLiteral("value"), QJsonObject{
        {QStringLiteral("type"), QStringLiteral("integer")}
    });
    schema.insert(QStringLiteral("properties"), properties);

    registry.registerTool({
        QStringLiteral("echo"),
        QStringLiteral("Echo arguments"),
        schema,
        [](const QJsonObject& args, QString*) { return args; }
    });
    registry.registerTool({
        QStringLiteral("error-tool"),
        QStringLiteral("Always fails"),
        schema,
        [](const QJsonObject&, QString* error) {
            if (error)
                *error = QStringLiteral("expected tool error");
            return QJsonObject();
        }
    });
    registry.registerTool({
        QStringLiteral("throw-tool"),
        QStringLiteral("Throws"),
        schema,
        [](const QJsonObject&, QString*) -> QJsonObject {
            throw std::runtime_error("expected exception");
        }
    });
    registry.registerTool({
        QStringLiteral("image-tool"),
        QStringLiteral("Returns image content"),
        schema,
        {},
        [](const QJsonObject&, QString*, QJsonArray* content) {
            if (content) {
                content->append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("image")},
                    {QStringLiteral("data"), QStringLiteral("aGVsbG8=")},
                    {QStringLiteral("mimeType"), QStringLiteral("image/png")}
                });
            }
            return QJsonObject{{QStringLiteral("ok"), true}};
        }
    });
    registry.registerTool({
        QStringLiteral("slow-tool"),
        QStringLiteral("Delays before returning"),
        schema,
        [](const QJsonObject&, QString*) {
            QEventLoop loop;
            QTimer::singleShot(1500, &loop, &QEventLoop::quit);
            loop.exec();
            return QJsonObject{{QStringLiteral("ok"), true}};
        }
    });

    mcp::McpProtocol protocol(&registry, mcp::ServerInfo{});

    const QJsonObject initialize = parseObject(protocol.handleMessage(compact(
        rpcRequest(1, QStringLiteral("initialize")))));
    const QJsonObject initializeResult = initialize.value(QStringLiteral("result")).toObject();
    const bool g1 = initializeResult.value(QStringLiteral("protocolVersion")).toString()
            == QStringLiteral("2025-06-18")
        && initializeResult.value(QStringLiteral("capabilities")).toObject()
            .value(QStringLiteral("tools")).isObject()
        && initializeResult.value(QStringLiteral("serverInfo")).toObject()
            .value(QStringLiteral("name")).toString() == QStringLiteral("v-simple-editor");
    g1 ? pass("G1 initialize contract")
       : fail("G1 initialize contract", QStringLiteral("unexpected initialize response"));

    const QJsonObject supportedInitialize = parseObject(
        protocol.handleMessage(compact(rpcRequest(
            71, QStringLiteral("initialize"), QJsonObject{
                {QStringLiteral("protocolVersion"), QStringLiteral("2025-03-26")}
            }))));
    const QJsonObject unsupportedInitialize = parseObject(
        protocol.handleMessage(compact(rpcRequest(
            72, QStringLiteral("initialize"), QJsonObject{
                {QStringLiteral("protocolVersion"), QStringLiteral("1999-01-01")}
            }))));
    const bool g63 = supportedInitialize.value(QStringLiteral("result")).toObject()
                         .value(QStringLiteral("protocolVersion")).toString()
                         == QStringLiteral("2025-03-26")
        && unsupportedInitialize.value(QStringLiteral("result")).toObject()
               .value(QStringLiteral("protocolVersion")).toString()
               == QStringLiteral("2025-06-18");
    g63 ? pass("G63 initialize version negotiation")
        : fail("G63 initialize version negotiation",
               QStringLiteral("requested protocol versions were not negotiated correctly"));

    const QJsonObject parseError = parseObject(protocol.handleMessage(QByteArray("{")));
    const bool g2 = parseError.value(QStringLiteral("error")).toObject()
        .value(QStringLiteral("code")).toInt() == -32700;
    g2 ? pass("G2 parse error")
       : fail("G2 parse error", QStringLiteral("wrong error code"));

    const QJsonObject batchError = parseObject(protocol.handleMessage(QByteArray("[]")));
    const bool g3 = batchError.value(QStringLiteral("error")).toObject()
        .value(QStringLiteral("code")).toInt() == -32600;
    g3 ? pass("G3 batch rejected")
       : fail("G3 batch rejected", QStringLiteral("wrong error code"));

    const QJsonObject unknownError = parseObject(protocol.handleMessage(compact(
        rpcRequest(2, QStringLiteral("foo/bar")))));
    const bool g4 = unknownError.value(QStringLiteral("error")).toObject()
        .value(QStringLiteral("code")).toInt() == -32601;
    g4 ? pass("G4 unknown method")
       : fail("G4 unknown method", QStringLiteral("wrong error code"));

    QJsonObject notification;
    notification.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    notification.insert(QStringLiteral("method"), QStringLiteral("notifications/initialized"));
    const bool g5 = protocol.handleMessage(compact(notification)).isEmpty();
    g5 ? pass("G5 notification has no response")
       : fail("G5 notification has no response", QStringLiteral("response was not empty"));

    QJsonObject listRequest = rpcRequest(3, QStringLiteral("tools/list"));
    const QJsonArray tools = parseObject(protocol.handleMessage(compact(listRequest)))
        .value(QStringLiteral("result")).toObject().value(QStringLiteral("tools")).toArray();
    bool hasEcho = false;
    bool hasErrorTool = false;
    bool schemasHeld = true;
    for (const QJsonValue& value : tools) {
        const QJsonObject tool = value.toObject();
        const QString name = tool.value(QStringLiteral("name")).toString();
        hasEcho = hasEcho || name == QStringLiteral("echo");
        hasErrorTool = hasErrorTool || name == QStringLiteral("error-tool");
        if ((name == QStringLiteral("echo") || name == QStringLiteral("error-tool"))
            && tool.value(QStringLiteral("inputSchema")).toObject() != schema) {
            schemasHeld = false;
        }
    }
    const bool g6 = hasEcho && hasErrorTool && schemasHeld;
    g6 ? pass("G6 tools list and schemas")
       : fail("G6 tools list and schemas", QStringLiteral("registered tools were not retained"));

    QJsonObject callParams;
    callParams.insert(QStringLiteral("name"), QStringLiteral("echo"));
    callParams.insert(QStringLiteral("arguments"), QJsonObject{
        {QStringLiteral("value"), 42}
    });
    const QJsonObject successCall = parseObject(protocol.handleMessage(compact(
        rpcRequest(4, QStringLiteral("tools/call"), callParams))));
    const QJsonObject successResult = successCall.value(QStringLiteral("result")).toObject();
    const bool g7 = !successResult.value(QStringLiteral("isError")).toBool(true)
        && toolPayload(successCall)
            .value(QStringLiteral("value")).toInt() == 42;
    g7 ? pass("G7 successful tool call")
       : fail("G7 successful tool call", QStringLiteral("structured result was not preserved"));

    callParams.insert(QStringLiteral("name"), QStringLiteral("error-tool"));
    const QJsonObject errorCall = parseObject(protocol.handleMessage(compact(
        rpcRequest(5, QStringLiteral("tools/call"), callParams))));
    const QJsonObject errorResult = errorCall.value(QStringLiteral("result")).toObject();
    const QJsonArray errorContent = errorResult.value(QStringLiteral("content")).toArray();
    const bool g8 = errorResult.value(QStringLiteral("isError")).toBool(false)
        && !errorCall.contains(QStringLiteral("error"))
        && !errorContent.isEmpty()
        && errorContent.first().toObject().value(QStringLiteral("text")).toString()
            == QStringLiteral("expected tool error");
    g8 ? pass("G8 tool error is result isError")
       : fail("G8 tool error is result isError", QStringLiteral("wrong tool error shape"));

    callParams.insert(QStringLiteral("name"), QStringLiteral("missing-tool"));
    const QJsonObject missingCall = parseObject(protocol.handleMessage(compact(
        rpcRequest(6, QStringLiteral("tools/call"), callParams))));
    const bool g9 = missingCall.value(QStringLiteral("error")).toObject()
        .value(QStringLiteral("code")).toInt() == -32602;
    g9 ? pass("G9 unknown tool")
       : fail("G9 unknown tool", QStringLiteral("wrong error code"));

    callParams.insert(QStringLiteral("name"), QStringLiteral("throw-tool"));
    const QJsonObject throwCall = parseObject(protocol.handleMessage(compact(
        rpcRequest(7, QStringLiteral("tools/call"), callParams))));
    const bool g10 = throwCall.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("isError")).toBool(false);
    g10 ? pass("G10 throwing handler is contained")
        : fail("G10 throwing handler is contained", QStringLiteral("exception escaped"));

    const QJsonObject imageCall = parseObject(protocol.handleMessage(compact(
        rpcRequest(70, QStringLiteral("tools/call"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("image-tool")},
            {QStringLiteral("arguments"), QJsonObject{}}
        }))));
    const QJsonObject imageResult = toolResult(imageCall);
    const QJsonArray imageContent = imageResult.value(QStringLiteral("content"))
        .toArray();
    const QJsonObject imageItem = imageContent.isEmpty()
        ? QJsonObject() : imageContent.first().toObject();
    const QJsonObject imagePayload = toolPayload(imageCall);
    const bool g10Image = !imageResult.value(QStringLiteral("isError")).toBool(true)
        && imageContent.size() == 1
        && imageItem.value(QStringLiteral("type")).toString() == QStringLiteral("image")
        && imageItem.value(QStringLiteral("data")).toString() == QStringLiteral("aGVsbG8=")
        && imageItem.value(QStringLiteral("mimeType")).toString()
               == QStringLiteral("image/png")
        && imagePayload.value(QStringLiteral("ok")).toBool(false);
    g10Image ? pass("G10 image content result shape")
             : fail("G10 image content result shape",
                    QStringLiteral("image content was not emitted in MCP shape"));

    mcp::McpHttpServer server(&registry);
    const bool serverStarted = server.start();
    if (!serverStarted) {
        fail("G11 HTTP bearer POST", QStringLiteral("server failed to start"));
        fail("G12 HTTP unauthorized", QStringLiteral("server failed to start"));
        fail("G13 HTTP query token", QStringLiteral("server failed to start"));
        fail("G14 HTTP GET method", QStringLiteral("server failed to start"));
        fail("G32 oversized HTTP header", QStringLiteral("server failed to start"));
        fail("G33 chunked transfer rejected", QStringLiteral("server failed to start"));
        fail("G34 duplicate content-length rejected", QStringLiteral("server failed to start"));
        fail("G59 HTTP Origin rejected", QStringLiteral("server failed to start"));
        fail("G60 HTTP localhost Origin allowed", QStringLiteral("server failed to start"));
        fail("G61 HTTP content-type rejected", QStringLiteral("server failed to start"));
        fail("G62 HTTP protocol header", QStringLiteral("server failed to start"));
        fail("G66 connection limit 503", QStringLiteral("server failed to start"));
        fail("G67 stdio bridge no head-of-line blocking / timeout",
             QStringLiteral("server failed to start"));
    } else {
        QNetworkAccessManager manager;
        const QUrl endpoint(server.endpointUrl());
        const QByteArray ping = compact(rpcRequest(8, QStringLiteral("ping")));

        const HttpResult bearer = post(&manager, endpoint, ping,
                                       server.token().toUtf8());
        const QJsonObject bearerJson = parseObject(bearer.body);
        const bool g11 = bearer.status == 200
            && bearerJson.value(QStringLiteral("jsonrpc")).toString() == QStringLiteral("2.0")
            && bearerJson.value(QStringLiteral("result")).isObject();
        g11 ? pass("G11 HTTP bearer POST")
            : fail("G11 HTTP bearer POST", QStringLiteral("status=%1").arg(bearer.status));

        const HttpResult noToken = post(&manager, endpoint, ping);
        const HttpResult badToken = post(&manager, endpoint, ping,
                                         QByteArray("wrong-token"));
        const bool g12 = noToken.status == 401 && badToken.status == 401;
        g12 ? pass("G12 HTTP unauthorized")
            : fail("G12 HTTP unauthorized", QStringLiteral("noToken=%1 badToken=%2")
                    .arg(noToken.status).arg(badToken.status));

        QUrl queryEndpoint(endpoint);
        QUrlQuery query(queryEndpoint);
        query.addQueryItem(QStringLiteral("token"), server.token());
        queryEndpoint.setQuery(query);
        const HttpResult queryToken = post(&manager, queryEndpoint, ping);
        const QJsonObject queryJson = parseObject(queryToken.body);
        const bool g13 = queryToken.status == 200
            && queryJson.value(QStringLiteral("result")).isObject();
        g13 ? pass("G13 HTTP query token")
            : fail("G13 HTTP query token", QStringLiteral("status=%1").arg(queryToken.status));

        const HttpResult getResult = get(&manager, endpoint);
        const bool g14 = getResult.status == 405;
        g14 ? pass("G14 HTTP GET rejected")
            : fail("G14 HTTP GET rejected", QStringLiteral("status=%1").arg(getResult.status));

        const QByteArray oversizedHeader =
            QByteArray("POST /mcp HTTP/1.1\r\nHost: localhost\r\nX-Fill: ")
            + QByteArray(64 * 1024, 'a');
        const int oversizedHeaderStatus = rawHttpStatus(server.port(),
                                                         oversizedHeader);
        oversizedHeaderStatus == 431
            ? pass("G32 oversized HTTP header")
            : fail("G32 oversized HTTP header",
                   QStringLiteral("status=%1").arg(oversizedHeaderStatus));

        const QByteArray chunkedRequest(
            "POST /mcp HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "0\r\n\r\n");
        const int chunkedStatus = rawHttpStatus(server.port(), chunkedRequest);
        chunkedStatus == 501
            ? pass("G33 chunked transfer rejected")
            : fail("G33 chunked transfer rejected",
                   QStringLiteral("status=%1").arg(chunkedStatus));

        const QByteArray duplicateLengthRequest(
            "POST /mcp HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 0\r\n"
            "Content-Length: 0\r\n\r\n");
        const int duplicateLengthStatus = rawHttpStatus(
            server.port(), duplicateLengthRequest);
        duplicateLengthStatus == 400
            ? pass("G34 duplicate content-length rejected")
            : fail("G34 duplicate content-length rejected",
                   QStringLiteral("status=%1").arg(duplicateLengthStatus));

        const HttpResult evilOrigin = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"Origin", "http://evil.example"}});
        const HttpResult nullOrigin = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"Origin", "null"}});
        const bool g59 = evilOrigin.status == 403 && nullOrigin.status == 403;
        g59 ? pass("G59 HTTP Origin rejected")
            : fail("G59 HTTP Origin rejected",
                   QStringLiteral("evil=%1 null=%2")
                       .arg(evilOrigin.status).arg(nullOrigin.status));

        const HttpResult localhostOrigin = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"Origin", "http://localhost:3000"}});
        const bool g60 = localhostOrigin.status == 200
            && localhostOrigin.allowOrigin == "http://localhost:3000";
        g60 ? pass("G60 HTTP localhost Origin allowed")
            : fail("G60 HTTP localhost Origin allowed",
                   QStringLiteral("status=%1 allowOrigin=%2")
                       .arg(localhostOrigin.status)
                       .arg(QString::fromLatin1(localhostOrigin.allowOrigin)));

        const HttpResult badContentType = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            {}, QByteArray("text/plain"));
        badContentType.status == 415
            ? pass("G61 HTTP content-type rejected")
            : fail("G61 HTTP content-type rejected",
                   QStringLiteral("status=%1").arg(badContentType.status));

        const HttpResult unsupportedVersion = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"MCP-Protocol-Version", "1999-01-01"}});
        const HttpResult supportedVersion = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"MCP-Protocol-Version", "2025-03-26"}});
        const bool g62 = unsupportedVersion.status == 400
            && supportedVersion.status == 200;
        g62 ? pass("G62 HTTP protocol header")
            : fail("G62 HTTP protocol header",
                   QStringLiteral("unsupported=%1 supported=%2")
                       .arg(unsupportedVersion.status)
                       .arg(supportedVersion.status));

        QList<QPointer<QTcpSocket>> idle;
        idle.reserve(mcp::McpHttpServer::maxConnections());
        for (int index = 0; index < mcp::McpHttpServer::maxConnections(); ++index) {
            QTcpSocket* socket = new QTcpSocket;
            idle.append(QPointer<QTcpSocket>(socket));
            socket->connectToHost(QHostAddress::LocalHost, server.port());
        }
        {
            QEventLoop loop;
            QTimer::singleShot(300, &loop, &QEventLoop::quit);
            loop.exec();
        }
        const QByteArray connectionLimitRequest(
            "POST /mcp HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 0\r\n\r\n");
        const int connectionLimitStatus = rawHttpStatus(
            server.port(), connectionLimitRequest);
        connectionLimitStatus == 503
            ? pass("G66 connection limit 503")
            : fail("G66 connection limit 503",
                   QStringLiteral("status=%1").arg(connectionLimitStatus));
        for (const QPointer<QTcpSocket>& socket : idle) {
            if (!socket)
                continue;
            socket->close();
            socket->deleteLater();
        }
        {
            QEventLoop loop;
            QTimer::singleShot(300, &loop, &QEventLoop::quit);
            loop.exec();
        }

        QProcess bridge;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("VEDITOR_MCP_TOKEN"), server.token());
        environment.insert(QStringLiteral("VEDITOR_MCP_TIMEOUT_MS"),
                            QStringLiteral("1000"));
        environment.remove(QStringLiteral("VEDITOR_MCP_SELFTEST"));
        environment.remove(QStringLiteral("VEDITOR_ALL_SELFTEST"));
        bridge.setProcessEnvironment(environment);

        const QByteArray bridgeInput = compact(rpcRequest(
            101, QStringLiteral("tools/call"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("slow-tool")},
                {QStringLiteral("arguments"), QJsonObject{}}
            })) + '\n'
            + compact(rpcRequest(102, QStringLiteral("ping"))) + '\n'
            + compact(rpcRequest(103, QStringLiteral("ping"))) + '\n';
        QByteArray bridgeOutput;
        QVector<QJsonObject> bridgeResponses;
        QEventLoop bridgeLoop;
        auto consumeBridgeOutput = [&]() {
            bridgeOutput += bridge.readAllStandardOutput();
            int newline = -1;
            while ((newline = bridgeOutput.indexOf('\n')) >= 0) {
                QByteArray line = bridgeOutput.left(newline);
                bridgeOutput.remove(0, newline + 1);
                if (line.endsWith('\r'))
                    line.chop(1);
                const QJsonObject response = parseObject(line);
                if (!response.isEmpty())
                    bridgeResponses.append(response);
            }
            if (bridgeResponses.size() >= 3)
                bridgeLoop.quit();
        };
        QObject::connect(&bridge, &QProcess::started, &bridge,
                         [&bridge, bridgeInput]() { bridge.write(bridgeInput); });
        QObject::connect(&bridge, &QProcess::readyReadStandardOutput, &bridge,
                         consumeBridgeOutput);
        QObject::connect(&bridge, &QProcess::errorOccurred, &bridge,
                         [&bridgeLoop](QProcess::ProcessError) {
                             bridgeLoop.quit();
                         });
        QTimer::singleShot(10000, &bridgeLoop, &QEventLoop::quit);
        bridge.start(QCoreApplication::applicationFilePath(), QStringList{
            QStringLiteral("--mcp-stdio"),
            QStringLiteral("--port"),
            QString::number(server.port())
        });
        bridgeLoop.exec();
        consumeBridgeOutput();
        bridge.closeWriteChannel();
        const bool bridgeFinished = bridge.waitForFinished(5000);
        if (!bridgeFinished && bridge.state() != QProcess::NotRunning) {
            bridge.kill();
            bridge.waitForFinished(1000);
        }

        bool firstResponseIsPing = false;
        bool slowRequestTimedOut = false;
        bool finalPingSucceeded = false;
        if (!bridgeResponses.isEmpty()) {
            firstResponseIsPing = bridgeResponses.first()
                .value(QStringLiteral("id")).toInt(-1) == 102;
        }
        for (const QJsonObject& response : bridgeResponses) {
            const int id = response.value(QStringLiteral("id")).toInt(-1);
            if (id == 101) {
                slowRequestTimedOut = response.value(QStringLiteral("error"))
                    .toObject().value(QStringLiteral("message")).toString()
                    == QStringLiteral("editor did not respond in time");
            } else if (id == 103) {
                finalPingSucceeded = response.value(QStringLiteral("result")).isObject();
            }
        }
        const bool g67 = firstResponseIsPing && slowRequestTimedOut
            && finalPingSucceeded && bridgeResponses.size() == 3;
        g67 ? pass("G67 stdio bridge no head-of-line blocking / timeout")
            : fail("G67 stdio bridge no head-of-line blocking / timeout",
                   QStringLiteral("responses=%1 firstIsPing=%2 timeout=%3 finalPing=%4")
                       .arg(bridgeResponses.size())
                       .arg(firstResponseIsPing)
                       .arg(slowRequestTimedOut)
                       .arg(finalPingSucceeded));
    }

    mcp::McpEditorTools nullWindowTools(nullptr, &registry);
    nullWindowTools.registerReadTools();

    const QJsonObject readToolsList = parseObject(protocol.handleMessage(compact(
        rpcRequest(9, QStringLiteral("tools/list")))));
    const QJsonArray readTools = readToolsList.value(QStringLiteral("result"))
        .toObject().value(QStringLiteral("tools")).toArray();
    const QStringList expectedReadToolNames{
        QStringLiteral("get_project_info"),
        QStringLiteral("get_frame"),
        QStringLiteral("get_export_status"),
        QStringLiteral("get_timeline"),
        QStringLiteral("get_captions"),
        QStringLiteral("list_commands")
    };
    bool allReadToolsListed = true;
    for (const QString& expectedName : expectedReadToolNames) {
        bool found = false;
        for (const QJsonValue& value : readTools) {
            if (value.toObject().value(QStringLiteral("name")).toString() == expectedName) {
                found = true;
                break;
            }
        }
        allReadToolsListed = allReadToolsListed && found;
    }
    allReadToolsListed ? pass("G15 MCP read tools listed")
                       : fail("G15 MCP read tools listed",
                              QStringLiteral("one or more read tools are missing"));

    bool allReadToolsError = true;
    int nullToolRequestId = 10;
    for (const QString& toolName : expectedReadToolNames) {
        const QJsonObject params{
            {QStringLiteral("name"), toolName},
            {QStringLiteral("arguments"), QJsonObject{}}
        };
        const QJsonObject response = parseObject(protocol.handleMessage(compact(
            rpcRequest(nullToolRequestId++, QStringLiteral("tools/call"), params))));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        allReadToolsError = allReadToolsError
            && result.value(QStringLiteral("isError")).toBool(false);
    }
    allReadToolsError ? pass("G16 null editor read tools are safe")
                      : fail("G16 null editor read tools are safe",
                             QStringLiteral("a read tool did not return isError"));

    mcp::McpEditorTools nullWindowWriteTools(nullptr, &registry);
    nullWindowWriteTools.registerWriteTools();
    const QJsonObject writeToolsList = parseObject(protocol.handleMessage(compact(
        rpcRequest(11, QStringLiteral("tools/list")))));
    const QJsonArray writeTools = writeToolsList.value(QStringLiteral("result"))
        .toObject().value(QStringLiteral("tools")).toArray();
    const QStringList expectedWriteToolNames{
        QStringLiteral("export_video"),
        QStringLiteral("import_media"),
        QStringLiteral("save_project"),
        QStringLiteral("open_project"),
        QStringLiteral("select_clip"),
        QStringLiteral("clear_selection"),
        QStringLiteral("run_command"),
        QStringLiteral("split_clip"),
        QStringLiteral("delete_clip"),
        QStringLiteral("move_clip"),
        QStringLiteral("set_clip_property"),
        QStringLiteral("add_caption"),
        QStringLiteral("set_playhead"),
        QStringLiteral("undo"),
        QStringLiteral("redo")
    };
    bool allWriteToolsListed = true;
    for (const QString& expectedName : expectedWriteToolNames) {
        bool found = false;
        for (const QJsonValue& value : writeTools) {
            if (value.toObject().value(QStringLiteral("name")).toString() == expectedName) {
                found = true;
                break;
            }
        }
        allWriteToolsListed = allWriteToolsListed && found;
    }
    allWriteToolsListed ? pass("G17 MCP write tools listed")
                        : fail("G17 MCP write tools listed",
                               QStringLiteral("one or more write tools are missing"));

    bool allWriteToolsError = true;
    int nullWriteToolRequestId = 12;
    for (const QString& toolName : expectedWriteToolNames) {
        const QJsonObject params{
            {QStringLiteral("name"), toolName},
            {QStringLiteral("arguments"), QJsonObject{}}
        };
        const QJsonObject response = parseObject(protocol.handleMessage(compact(
            rpcRequest(nullWriteToolRequestId++, QStringLiteral("tools/call"), params))));
        allWriteToolsError = allWriteToolsError
            && response.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("isError")).toBool(false);
    }
    allWriteToolsError ? pass("G18 null editor write tools are safe")
                       : fail("G18 null editor write tools are safe",
                              QStringLiteral("a write tool did not return isError"));

    auto callWriteTool = [&](int id, const QString& name,
                             const QJsonObject& arguments) {
        return parseObject(protocol.handleMessage(compact(rpcRequest(
            id, QStringLiteral("tools/call"), QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("arguments"), arguments}
            }))));
    };
    const QJsonObject unsupportedProperty = callWriteTool(
        21, QStringLiteral("set_clip_property"), QJsonObject{
            {QStringLiteral("property"), QStringLiteral("notSupported")},
            {QStringLiteral("value"), 1.0}
        });
    const bool g19 = unsupportedProperty.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("isError")).toBool(false);
    g19 ? pass("G19 unsupported clip property rejected")
        : fail("G19 unsupported clip property rejected",
               QStringLiteral("unsupported property was accepted"));

    const QJsonObject outOfRangeProperty = callWriteTool(
        22, QStringLiteral("set_clip_property"), QJsonObject{
            {QStringLiteral("property"), QStringLiteral("opacity")},
            {QStringLiteral("value"), 2.0}
        });
    const bool g20 = outOfRangeProperty.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("isError")).toBool(false);
    g20 ? pass("G20 clip property range rejected")
        : fail("G20 clip property range rejected",
               QStringLiteral("out-of-range property was accepted"));

    const QJsonObject missingSplitTime = callWriteTool(
        23, QStringLiteral("split_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}
        });
    const QJsonObject nanSplitTime = callWriteTool(
        24, QStringLiteral("split_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0},
            {QStringLiteral("timeSec"),
             std::numeric_limits<double>::quiet_NaN()}
        });
    const bool g21 = missingSplitTime.value(QStringLiteral("result")).toObject()
                         .value(QStringLiteral("isError")).toBool(false)
        && nanSplitTime.value(QStringLiteral("result")).toObject()
               .value(QStringLiteral("isError")).toBool(false);
    g21 ? pass("G21 split argument validation")
        : fail("G21 split argument validation",
               QStringLiteral("missing or NaN split time was accepted"));

    const QJsonObject invalidCaption = callWriteTool(
        25, QStringLiteral("add_caption"), QJsonObject{
            {QStringLiteral("text"), QStringLiteral("invalid")},
            {QStringLiteral("startSec"), 2.0},
            {QStringLiteral("endSec"), 2.0}
        });
    const bool g22 = invalidCaption.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("isError")).toBool(false);
    g22 ? pass("G22 caption interval validation")
        : fail("G22 caption interval validation",
               QStringLiteral("non-positive caption interval was accepted"));

    const QByteArray multilineJson(
        "{\n  \"jsonrpc\": \"2.0\",\n  \"id\": 26,\n  \"method\": \"ping\"\n}");
    const QByteArray compactJson = McpStdioBridge::compactJson(multilineJson);
    const QJsonObject compactObject = parseObject(compactJson);
    const bool g23 = !compactJson.contains('\n')
        && compactObject.value(QStringLiteral("jsonrpc")).toString()
               == QStringLiteral("2.0")
        && compactObject.value(QStringLiteral("id")).toInt() == 26
        && compactObject.value(QStringLiteral("method")).toString()
               == QStringLiteral("ping");
    g23 ? pass("G23 JSON compact one-line framing")
        : fail("G23 JSON compact one-line framing",
               QStringLiteral("JSON was not compacted to one line"));

    const QJsonValue extractedId = McpStdioBridge::extractRequestId(compactJson);
    const QJsonValue malformedId = McpStdioBridge::extractRequestId(QByteArray("{"));
    const bool g24 = extractedId.isDouble() && extractedId.toInt() == 26
        && malformedId.isNull();
    g24 ? pass("G24 request id extraction")
        : fail("G24 request id extraction",
               QStringLiteral("normal or malformed request id handling is wrong"));

    const QByteArray transportError = McpStdioBridge::makeTransportError(
        extractedId, QStringLiteral("editor not running"));
    const QJsonObject transportErrorObject = parseObject(transportError);
    const QJsonObject transportErrorDetails =
        transportErrorObject.value(QStringLiteral("error")).toObject();
    const bool g25 = !transportError.contains('\n')
        && transportErrorObject.value(QStringLiteral("id")).toInt() == 26
        && transportErrorDetails.value(QStringLiteral("code")).toInt() == -32603
        && transportErrorDetails.value(QStringLiteral("message")).toString()
               == QStringLiteral("editor not running");
    g25 ? pass("G25 transport error response")
        : fail("G25 transport error response",
               QStringLiteral("internal transport error shape is wrong"));

    QProcessEnvironment baseEnvironment;
    baseEnvironment.insert(QStringLiteral("ANTHROPIC_API_KEY"),
                           QStringLiteral("key"));
    baseEnvironment.insert(QStringLiteral("ANTHROPIC_AUTH_TOKEN"),
                           QStringLiteral("token"));
    baseEnvironment.insert(QStringLiteral("PATH"), QStringLiteral("keep-me"));
    const QProcessEnvironment childEnvironment =
        AiChatDock::childEnvironment(baseEnvironment);
    const bool g26 = !childEnvironment.contains(QStringLiteral("ANTHROPIC_API_KEY"))
        && !childEnvironment.contains(QStringLiteral("ANTHROPIC_AUTH_TOKEN"))
        && childEnvironment.value(QStringLiteral("PATH")) == QStringLiteral("keep-me");
    g26 ? pass("G26 child environment removes Anthropic credentials")
        : fail("G26 child environment removes Anthropic credentials",
               QStringLiteral("credential removal or PATH preservation failed"));

    const QStringList arguments = AiChatDock::buildArguments(
        QStringLiteral("/tmp/veditor-mcp.json"), QStringLiteral("edit this"));
    // プロンプトは -p の直後。可変長の --allowedTools より後ろに置くと
    // claude 側がプロンプトを許可ツール名として飲み込む (実測)。
    const int allowedToolsIndex = arguments.indexOf(QStringLiteral("--allowedTools"));
    const int promptIndex = arguments.indexOf(QStringLiteral("edit this"));
    const bool g27 = arguments.contains(QStringLiteral("--strict-mcp-config"))
        && arguments.indexOf(QStringLiteral("--output-format")) >= 0
        && arguments.value(arguments.indexOf(QStringLiteral("--output-format")) + 1)
               == QStringLiteral("stream-json")
        && arguments.value(0) == QStringLiteral("-p")
        && promptIndex == 1
        && allowedToolsIndex > promptIndex
        && arguments.constLast() == QStringLiteral("mcp__veditor");
    g27 ? pass("G27 AI chat CLI arguments")
        : fail("G27 AI chat CLI arguments",
               QStringLiteral("required stream-json arguments or prompt order is wrong"));

    const QByteArray mcpConfig = AiChatDock::buildMcpConfig(
        9876, QStringLiteral("test-token"));
    const QJsonObject mcpConfigObject = parseObject(mcpConfig);
    const QJsonObject veditor = mcpConfigObject.value(QStringLiteral("mcpServers"))
        .toObject().value(QStringLiteral("veditor")).toObject();
    const bool g28 = veditor.value(QStringLiteral("url")).toString()
                         == QStringLiteral("http://127.0.0.1:9876/mcp")
        && veditor.value(QStringLiteral("headers")).toObject()
               .value(QStringLiteral("Authorization")).toString()
               == QStringLiteral("Bearer test-token");
    g28 ? pass("G28 AI chat MCP config JSON")
        : fail("G28 AI chat MCP config JSON",
               QStringLiteral("port or bearer token was not encoded correctly"));

    const QStringList changedToolNames{
        QStringLiteral("split_clip"), QStringLiteral("delete_clip"),
        QStringLiteral("move_clip"), QStringLiteral("set_clip_property")
    };
    const QHash<QString, QJsonObject> unknownArgumentCases{
        {QStringLiteral("split_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("timeSec"), 1.0},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("delete_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("move_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("newStartSec"), 0.0},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("set_clip_property"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("value"), 1.0}, {QStringLiteral("track"), QStringLiteral("video")}
        }}
    };
    bool unknownArgumentsRejected = true;
    for (const QString& toolName : changedToolNames) {
        const QJsonObject response = callWriteTool(
            29, toolName, unknownArgumentCases.value(toolName));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        unknownArgumentsRejected = unknownArgumentsRejected
            && result.value(QStringLiteral("isError")).toBool(false)
            && result.value(QStringLiteral("content")).toArray().first()
                   .toObject().value(QStringLiteral("text")).toString()
                   == QStringLiteral("unknown argument: track");
    }
    unknownArgumentsRejected ? pass("G29 unknown write arguments rejected")
                             : fail("G29 unknown write arguments rejected",
                                    QStringLiteral("a changed tool accepted an unknown key"));

    mcp::McpToolRegistry projectInfoRegistry;
    MainWindow projectInfoWindow;
    projectInfoWindow.hide();
    mcp::McpEditorTools projectInfoTools(&projectInfoWindow, &projectInfoRegistry);
    projectInfoTools.registerReadTools();
    mcp::McpProtocol projectInfoProtocol(&projectInfoRegistry, mcp::ServerInfo{});
    const QJsonObject projectInfoResponse = parseObject(
        projectInfoProtocol.handleMessage(compact(rpcRequest(
            30, QStringLiteral("tools/call"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("get_project_info")},
                {QStringLiteral("arguments"), QJsonObject{}}
            }))));
    const QJsonObject projectInfoResult = toolPayload(projectInfoResponse);
    const bool g30 = !projectInfoResult.contains(QStringLiteral("hasUnsavedChanges"))
        || (projectInfoResult.value(QStringLiteral("hasUnsavedChanges")).isBool()
            && projectInfoResult.value(QStringLiteral("hasUnsavedChanges")).toBool()
                == projectInfoWindow.isWindowModified());
    g30 ? pass("G30 project unsaved state is truthful")
        : fail("G30 project unsaved state is truthful",
               QStringLiteral("hasUnsavedChanges was missing a boolean/state contract"));

    projectInfoTools.registerWriteTools();
    auto callProjectInfoTool = [&](int id, const QString& name,
                                   const QJsonObject& arguments) {
        return parseObject(projectInfoProtocol.handleMessage(compact(rpcRequest(
            id, QStringLiteral("tools/call"), QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("arguments"), arguments}
            }))));
    };
    const QJsonArray projectInfoToolDescriptors = parseObject(
        projectInfoProtocol.handleMessage(compact(
            rpcRequest(73, QStringLiteral("tools/list")))))
        .value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("tools")).toArray();
    bool outputSchemasDeclared = projectInfoToolDescriptors.size() == 21;
    for (const QJsonValue& value : projectInfoToolDescriptors) {
        outputSchemasDeclared = outputSchemasDeclared
            && value.toObject().value(QStringLiteral("outputSchema"))
                   .toObject().value(QStringLiteral("type")).toString()
                   == QStringLiteral("object");
    }
    const bool g64 = outputSchemasDeclared;
    g64 ? pass("G64 outputSchema declared")
        : fail("G64 outputSchema declared",
               QStringLiteral("expected 21 object output schemas, got %1")
                   .arg(projectInfoToolDescriptors.size()));

    auto requiredOutputFieldsPresent = [&projectInfoToolDescriptors](
        const QString& toolName, const QJsonObject& payload) {
        QJsonObject outputSchema;
        for (const QJsonValue& value : projectInfoToolDescriptors) {
            const QJsonObject descriptor = value.toObject();
            if (descriptor.value(QStringLiteral("name")).toString() == toolName) {
                outputSchema = descriptor.value(QStringLiteral("outputSchema"))
                    .toObject();
                break;
            }
        }
        if (outputSchema.isEmpty())
            return false;
        for (const QJsonValue& value : outputSchema.value(QStringLiteral("required"))
                 .toArray()) {
            if (!payload.contains(value.toString()))
                return false;
        }
        return true;
    };
    const QJsonObject listCommandsResponse = callProjectInfoTool(
        74, QStringLiteral("list_commands"), QJsonObject{});
    const bool projectInfoFieldsPresent =
        !toolResult(projectInfoResponse).value(QStringLiteral("isError")).toBool(false)
        && requiredOutputFieldsPresent(QStringLiteral("get_project_info"),
                                       toolPayload(projectInfoResponse));
    const bool listCommandsFieldsPresent =
        !toolResult(listCommandsResponse).value(QStringLiteral("isError")).toBool(false)
        && requiredOutputFieldsPresent(QStringLiteral("list_commands"),
                                       toolPayload(listCommandsResponse));
    QJsonObject successfulSelectResponse;
    auto toolErrorText = [&toolResult](const QJsonObject& response) {
        const QJsonArray content = toolResult(response)
            .value(QStringLiteral("content")).toArray();
        return content.isEmpty()
            ? QString()
            : content.first().toObject().value(QStringLiteral("text")).toString();
    };

    const QJsonObject saveWithoutPath = callProjectInfoTool(
        42, QStringLiteral("save_project"), QJsonObject{});
    const QJsonObject saveWithoutPathResult = toolResult(saveWithoutPath);
    const bool g42 = saveWithoutPathResult.value(QStringLiteral("isError")).toBool(false)
        && toolErrorText(saveWithoutPath)
               == QStringLiteral("保存先のパスを指定してください");
    g42 ? pass("G42 save without path is rejected non-interactively")
        : fail("G42 save without path is rejected non-interactively",
               QStringLiteral("save_project opened a dialog or returned a wrong error"));

    Timeline *projectTimeline = projectInfoWindow.findChild<Timeline *>();
    const QJsonObject missingImport = callProjectInfoTool(
        43, QStringLiteral("import_media"), QJsonObject{
            {QStringLiteral("filePath"),
             QStringLiteral("/definitely/missing/mcp-selftest-media.mp4")}
        });
    const bool g43 = toolResult(missingImport).value(QStringLiteral("isError")).toBool(false)
        && toolErrorText(missingImport)
               == QStringLiteral("ファイルが見つかりません: /definitely/missing/mcp-selftest-media.mp4");
    g43 ? pass("G43 missing media import is rejected")
        : fail("G43 missing media import is rejected",
               QStringLiteral("missing media was accepted or changed the error text"));

    QTemporaryFile importSource;
    const bool importSourceReady = importSource.open();
    const int importVideoCountBefore = projectTimeline
        && !projectTimeline->videoTracks().isEmpty()
        && projectTimeline->videoTracks().first()
        ? projectTimeline->videoTracks().first()->clipCount() : -1;
    const int importAudioCountBefore = projectTimeline
        && !projectTimeline->audioTracks().isEmpty()
        && projectTimeline->audioTracks().first()
        ? projectTimeline->audioTracks().first()->clipCount() : -1;
    QJsonObject successfulImport;
    if (importSourceReady) {
        successfulImport = callProjectInfoTool(
            44, QStringLiteral("import_media"), QJsonObject{
                {QStringLiteral("filePath"), importSource.fileName()},
                {QStringLiteral("trackIndex"), 0}
            });
    }
    const QJsonObject successfulImportResult = toolPayload(successfulImport);
    const int importVideoCountAfter = projectTimeline
        && !projectTimeline->videoTracks().isEmpty()
        && projectTimeline->videoTracks().first()
        ? projectTimeline->videoTracks().first()->clipCount() : -1;
    const int importAudioCountAfter = projectTimeline
        && !projectTimeline->audioTracks().isEmpty()
        && projectTimeline->audioTracks().first()
        ? projectTimeline->audioTracks().first()->clipCount() : -1;
    const QJsonArray importedClips = successfulImportResult
        .value(QStringLiteral("clips")).toArray();
    bool importResponseFields = !importedClips.isEmpty();
    for (const QJsonValue &value : importedClips) {
        const QJsonObject clip = value.toObject();
        importResponseFields = importResponseFields
            && clip.value(QStringLiteral("kind")).isString()
            && clip.value(QStringLiteral("trackIndex")).isDouble()
            && clip.value(QStringLiteral("clipIndex")).isDouble()
            && clip.value(QStringLiteral("startSec")).isDouble()
            && clip.value(QStringLiteral("durationSec")).isDouble();
    }
    bool importUndoRestored = false;
    if (successfulImportResult.value(QStringLiteral("ok")).toBool(false)) {
        const QJsonObject importUndo = callProjectInfoTool(
            45, QStringLiteral("undo"), QJsonObject{});
        const QJsonObject importUndoResult = toolPayload(importUndo);
        const int importVideoCountUndo = projectTimeline
            && !projectTimeline->videoTracks().isEmpty()
            && projectTimeline->videoTracks().first()
            ? projectTimeline->videoTracks().first()->clipCount() : -1;
        const int importAudioCountUndo = projectTimeline
            && !projectTimeline->audioTracks().isEmpty()
            && projectTimeline->audioTracks().first()
            ? projectTimeline->audioTracks().first()->clipCount() : -1;
        importUndoRestored = importUndoResult.value(QStringLiteral("ok")).toBool(false)
            && importVideoCountUndo == importVideoCountBefore
            && importAudioCountUndo == importAudioCountBefore;
    }
    const bool g44 = importSourceReady
        && successfulImportResult.value(QStringLiteral("ok")).toBool(false)
        && importResponseFields
        && importVideoCountAfter == importVideoCountBefore + 1
        && importAudioCountAfter == importAudioCountBefore + 1
        && importUndoRestored;
    g44 ? pass("G44 media import and one-step undo")
        : fail("G44 media import and one-step undo",
               QStringLiteral("import count, response fields, or undo restoration is wrong"));

    auto makeTestClip = [](const QString &name, int linkGroup) {
        ClipInfo clip;
        clip.filePath = name;
        clip.displayName = name;
        clip.duration = 5.0;
        clip.outPoint = 5.0;
        clip.linkGroup = linkGroup;
        return clip;
    };
    const bool timelineReady = projectTimeline
        && !projectTimeline->videoTracks().isEmpty()
        && !projectTimeline->audioTracks().isEmpty()
        && projectTimeline->videoTracks().first()
        && projectTimeline->audioTracks().first();
    if (timelineReady) {
        TimelineTrack *video0 = projectTimeline->videoTracks().first();
        TimelineTrack *audio0 = projectTimeline->audioTracks().first();
        const ClipInfo selectedVideo = makeTestClip(QStringLiteral("selected-video"), 777);
        ClipInfo selectedAudio = selectedVideo;
        selectedAudio.filePath = QStringLiteral("selected-audio");
        selectedAudio.displayName = QStringLiteral("selected-audio");
        video0->setClips(QVector<ClipInfo>{selectedVideo});
        audio0->setClips(QVector<ClipInfo>{selectedAudio});
        projectTimeline->clearSelection();

        const QJsonObject invalidSelect = callProjectInfoTool(
            46, QStringLiteral("select_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 99}
            });
        const bool invalidSelectRejected = toolResult(invalidSelect)
            .value(QStringLiteral("isError")).toBool(false)
            && video0->selectedClip() < 0
            && audio0->selectedClip() < 0;
        invalidSelectRejected
            ? pass("G45 select_clip rejects out-of-range index")
            : fail("G45 select_clip rejects out-of-range index",
                   QStringLiteral("invalid selection changed state or was accepted"));

        const QJsonObject selectResponse = callProjectInfoTool(
            47, QStringLiteral("select_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0}
            });
        successfulSelectResponse = selectResponse;
        const QJsonObject selectResult = toolPayload(selectResponse);
        const QJsonObject selectionTimelineResponse = callProjectInfoTool(
            48, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("all")}
            });
        const QJsonObject selectionTimeline = toolPayload(selectionTimelineResponse);
        auto hasSelectedClip = [](const QJsonArray &tracks) {
            for (const QJsonValue &trackValue : tracks) {
                for (const QJsonValue &clipValue : trackValue.toObject()
                         .value(QStringLiteral("clips")).toArray()) {
                    if (clipValue.toObject().value(QStringLiteral("selected")).toBool(false))
                        return true;
                }
            }
            return false;
        };
        const bool videoSelectedInJson = hasSelectedClip(
            selectionTimeline.value(QStringLiteral("video")).toArray());
        const bool audioSelectedInJson = hasSelectedClip(
            selectionTimeline.value(QStringLiteral("audio")).toArray());
        const bool selectStateSynchronized = video0->isClipSelected(0)
            && audio0->isClipSelected(0)
            && projectInfoWindow.selectedVideoTrackIndex() == 0
            && projectInfoWindow.selectedVideoClipIndexTracked() == 0
            && videoSelectedInJson && audioSelectedInJson;
        const bool g46 = selectResult.value(QStringLiteral("ok")).toBool(false)
            && selectStateSynchronized;
        g46 ? pass("G46 select_clip synchronizes Timeline and MainWindow")
            : fail("G46 select_clip synchronizes Timeline and MainWindow",
                   QStringLiteral("selection was not synchronized across all state holders"));

        const QJsonObject invalidAfterSelect = callProjectInfoTool(
            49, QStringLiteral("select_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 5}
            });
        const bool g47 = toolResult(invalidAfterSelect)
            .value(QStringLiteral("isError")).toBool(false)
            && video0->isClipSelected(0) && audio0->isClipSelected(0)
            && projectInfoWindow.selectedVideoTrackIndex() == 0
            && projectInfoWindow.selectedVideoClipIndexTracked() == 0;
        g47 ? pass("G47 invalid selection preserves existing selection")
            : fail("G47 invalid selection preserves existing selection",
                   QStringLiteral("existing selection was changed by invalid input"));

        const QJsonObject clearResponse = callProjectInfoTool(
            50, QStringLiteral("clear_selection"), QJsonObject{});
        const bool g48 = toolPayload(clearResponse).value(QStringLiteral("ok")).toBool(false)
            && video0->selectedClip() < 0 && audio0->selectedClip() < 0
            && projectInfoWindow.selectedVideoTrackIndex() == -1
            && projectInfoWindow.selectedVideoClipIndexTracked() == -1;
        g48 ? pass("G48 clear_selection clears both state holders")
            : fail("G48 clear_selection clears both state holders",
                   QStringLiteral("selection remained in Timeline or MainWindow"));
        while (projectTimeline->videoTrackCount() < 2)
            projectTimeline->addVideoTrack();
        while (projectTimeline->audioTrackCount() < 2)
            projectTimeline->addAudioTrack();
        TimelineTrack *video1 = projectTimeline->videoTracks().at(1);
        TimelineTrack *audio1 = projectTimeline->audioTracks().at(1);
        auto saveTestUndoBaseline = [&]() {
            projectTimeline->undoManager()->clear();
            projectTimeline->undoManager()->saveState(
                projectTimeline->currentState(), QStringLiteral("MCP selftest baseline"));
        };

        video0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("A"), 0),
            makeTestClip(QStringLiteral("B"), 0),
            makeTestClip(QStringLiteral("C"), 0)
        });
        video1->setClips(QVector<ClipInfo>{});
        audio0->setClips(QVector<ClipInfo>{});
        audio1->setClips(QVector<ClipInfo>{});
        projectTimeline->clearSelection();
        saveTestUndoBaseline();
        const QJsonObject reorderResponse = callProjectInfoTool(
            51, QStringLiteral("move_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("newStartSec"), 5.0}
            });
        const QJsonObject reorderResult = toolPayload(reorderResponse);
        const bool reorderSucceeded = reorderResult.value(QStringLiteral("ok")).toBool(false)
            && reorderResult.value(QStringLiteral("actualStartSec")).toDouble(-1.0) == 5.0
            && video0->clipCount() == 3
            && video0->clips().at(0).displayName == QStringLiteral("B")
            && video0->clips().at(1).displayName == QStringLiteral("A")
            && video0->clips().at(2).displayName == QStringLiteral("C");
        const QJsonObject reorderUndo = callProjectInfoTool(
            52, QStringLiteral("undo"), QJsonObject{});
        const bool reorderUndoSucceeded = toolPayload(reorderUndo)
            .value(QStringLiteral("ok")).toBool(false)
            && video0->clips().at(0).displayName == QStringLiteral("A")
            && video0->clips().at(1).displayName == QStringLiteral("B")
            && video0->clips().at(2).displayName == QStringLiteral("C");
        const bool g49 = reorderSucceeded && reorderUndoSucceeded;
        g49 ? pass("G49 continuous clips can be reordered and undone")
            : fail("G49 continuous clips can be reordered and undone",
                   QStringLiteral("move_clip did not reorder or undo in one step"));

        video0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("blocked-A"), 0),
            makeTestClip(QStringLiteral("blocked-B"), 0)
        });
        saveTestUndoBaseline();
        const QJsonObject blockedResponse = callProjectInfoTool(
            53, QStringLiteral("move_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("newStartSec"), 2.0}
            });
        const QJsonObject blockedResult = toolPayload(blockedResponse);
        const double actualBlockedStart = blockedResult
            .value(QStringLiteral("actualStartSec")).toDouble(-1.0);
        const bool g50 = !toolResult(blockedResponse).value(QStringLiteral("isError")).toBool(false)
            && !blockedResult.value(QStringLiteral("ok")).toBool(true)
            && std::isfinite(actualBlockedStart) && actualBlockedStart >= 0.0
            && !blockedResult.value(QStringLiteral("reason")).toString().isEmpty()
            && video0->clips().at(0).displayName == QStringLiteral("blocked-A")
            && video0->clips().at(1).displayName == QStringLiteral("blocked-B");
        g50 ? pass("G50 blocked move reports actual start and reason")
            : fail("G50 blocked move reports actual start and reason",
                   QStringLiteral("overlapping move was clamped, errored, or mutated clips"));

        video0->setClips(QVector<ClipInfo>{makeTestClip(QStringLiteral("cross-track"), 0)});
        video1->setClips(QVector<ClipInfo>{});
        saveTestUndoBaseline();
        const QJsonObject crossTrackResponse = callProjectInfoTool(
            54, QStringLiteral("move_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("newStartSec"), 0.0},
                {QStringLiteral("newTrackIndex"), 1}
            });
        const QJsonObject crossTrackResult = toolPayload(crossTrackResponse);
        const bool g51 = crossTrackResult.value(QStringLiteral("ok")).toBool(false)
            && crossTrackResult.value(QStringLiteral("trackIndex")).toInt(-1) == 1
            && video0->clipCount() == 0 && video1->clipCount() == 1
            && video1->clips().first().displayName == QStringLiteral("cross-track");
        g51 ? pass("G51 move_clip supports cross-track movement")
            : fail("G51 move_clip supports cross-track movement",
                   QStringLiteral("clip was not moved to the requested track"));

        const ClipInfo linkedVideo = makeTestClip(QStringLiteral("linked-video"), 778);
        ClipInfo linkedAudio = linkedVideo;
        linkedAudio.filePath = QStringLiteral("linked-audio");
        linkedAudio.displayName = QStringLiteral("linked-audio");
        video0->setClips(QVector<ClipInfo>{linkedVideo});
        video1->setClips(QVector<ClipInfo>{});
        audio0->setClips(QVector<ClipInfo>{linkedAudio});
        audio1->setClips(QVector<ClipInfo>{});
        saveTestUndoBaseline();
        const QJsonObject linkedMoveResponse = callProjectInfoTool(
            55, QStringLiteral("move_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("newStartSec"), 0.0},
                {QStringLiteral("newTrackIndex"), 1}
            });
        const QJsonObject linkedMoveResult = toolPayload(linkedMoveResponse);
        const bool g52 = linkedMoveResult.value(QStringLiteral("ok")).toBool(false)
            && video0->clipCount() == 0 && video1->clipCount() == 1
            && audio0->clipCount() == 0 && audio1->clipCount() == 1
            && video1->clips().first().linkGroup == 778
            && audio1->clips().first().linkGroup == 778;
        g52 ? pass("G52 linked V/A clips move together")
            : fail("G52 linked V/A clips move together",
                   QStringLiteral("linked audio/video clips diverged during track move"));
    }
    if (!timelineReady) {
        fail("G45-G52 editor timeline MCP behavior", QStringLiteral("Timeline was not available"));
    }

    const bool selectClipFieldsPresent =
        !toolResult(successfulSelectResponse).value(QStringLiteral("isError")).toBool(false)
        && requiredOutputFieldsPresent(QStringLiteral("select_clip"),
                                       toolPayload(successfulSelectResponse));
    const bool g65 = projectInfoFieldsPresent && listCommandsFieldsPresent
        && selectClipFieldsPresent;
    g65 ? pass("G65 structuredContent matches outputSchema")
        : fail("G65 structuredContent matches outputSchema",
               QStringLiteral("a successful tool response omitted an outputSchema required key"));

    // get_frame は実際に libav の動画デコーダを通るため、リポジトリの
    // 実在する動画を import_media で V1/A1 に配置してから検証する。
    const QString frameAssetPath = QDir::current().absoluteFilePath(
        QStringLiteral("test_assets/e2e_clip.mp4"));
    const QFileInfo frameAssetInfo(frameAssetPath);
    const bool frameAssetReady = frameAssetInfo.exists()
        && frameAssetInfo.isFile();
    QJsonObject frameImportResponse;
    if (frameAssetReady && timelineReady) {
        frameImportResponse = callProjectInfoTool(
            56, QStringLiteral("import_media"), QJsonObject{
                {QStringLiteral("filePath"), frameAssetPath},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("startSec"), 0.0}
            });
    }
    const QJsonObject frameImportEnvelope = toolResult(frameImportResponse);
    const QJsonObject frameImportPayload = toolPayload(frameImportResponse);
    const int frameVideoClipCount = timelineReady
        ? projectTimeline->videoTracks().first()->clipCount() : -1;
    const int frameAudioClipCount = timelineReady
        ? projectTimeline->audioTracks().first()->clipCount() : -1;
    const bool frameFixtureReady = frameAssetReady && timelineReady
        && !frameImportEnvelope.value(QStringLiteral("isError")).toBool(true)
        && frameImportPayload.value(QStringLiteral("ok")).toBool(false)
        && frameImportPayload.value(QStringLiteral("clips")).toArray().size() == 2
        && frameVideoClipCount == 1 && frameAudioClipCount == 1
        && projectTimeline->videoTracks().first()->clips().first().filePath
               == frameAssetPath
        && projectTimeline->totalDuration() > 0.0;

    const QJsonObject frameResponse = callProjectInfoTool(
        57, QStringLiteral("get_frame"), QJsonObject{
            {QStringLiteral("timeSec"), 0.0}
        });
    const QJsonObject frameEnvelope = toolResult(frameResponse);
    const QJsonArray frameContent = frameEnvelope.value(QStringLiteral("content"))
        .toArray();
    const QJsonObject frameItem = frameContent.isEmpty()
        ? QJsonObject() : frameContent.first().toObject();
    const QJsonObject framePayload = toolPayload(frameResponse);
    const QByteArray framePng = QByteArray::fromBase64(
        frameItem.value(QStringLiteral("data")).toString().toLatin1());
    QImage decodedFrame;
    const bool frameDecoded = decodedFrame.loadFromData(framePng, "PNG");
    const bool g53 = frameFixtureReady
        && !frameEnvelope.value(QStringLiteral("isError")).toBool(true)
        && frameContent.size() == 1
        && frameItem.value(QStringLiteral("type")).toString() == QStringLiteral("image")
        && frameItem.value(QStringLiteral("mimeType")).toString()
               == QStringLiteral("image/png")
        && frameDecoded && decodedFrame.width() > 0 && decodedFrame.height() > 0
        && framePayload.value(QStringLiteral("ok")).toBool(false)
        && framePayload.value(QStringLiteral("width")).toInt(-1) == decodedFrame.width()
        && framePayload.value(QStringLiteral("height")).toInt(-1) == decodedFrame.height()
        && framePayload.value(QStringLiteral("byteSize")).toInt(-1)
               == framePng.size()
        && QJsonDocument(frameResponse).toJson(QJsonDocument::Compact).size()
               <= 1024 * 1024;
    g53 ? pass("G53 get_frame returns decodable PNG image")
        : fail("G53 get_frame returns decodable PNG image",
               QStringLiteral("get_frame did not return a valid non-empty PNG"));

    const QJsonObject narrowFrameResponse = callProjectInfoTool(
        58, QStringLiteral("get_frame"), QJsonObject{
            {QStringLiteral("timeSec"), 0.0},
            {QStringLiteral("maxWidth"), 32}
        });
    const QJsonObject narrowFrameEnvelope = toolResult(narrowFrameResponse);
    const QJsonArray narrowFrameContent = narrowFrameEnvelope
        .value(QStringLiteral("content")).toArray();
    const QJsonObject narrowFrameItem = narrowFrameContent.isEmpty()
        ? QJsonObject() : narrowFrameContent.first().toObject();
    const QJsonObject narrowFramePayload = toolPayload(narrowFrameResponse);
    const QByteArray narrowFramePng = QByteArray::fromBase64(
        narrowFrameItem.value(QStringLiteral("data")).toString().toLatin1());
    QImage decodedNarrowFrame;
    const bool narrowFrameDecoded = decodedNarrowFrame.loadFromData(
        narrowFramePng, "PNG");
    const bool g54 = g53
        && !narrowFrameEnvelope.value(QStringLiteral("isError")).toBool(true)
        && narrowFrameContent.size() == 1
        && narrowFrameDecoded && decodedNarrowFrame.width() > 0
        && decodedNarrowFrame.height() > 0
        && decodedNarrowFrame.width() <= 32
        && decodedNarrowFrame.width() < decodedFrame.width()
        && narrowFramePayload.value(QStringLiteral("ok")).toBool(false)
        && narrowFramePayload.value(QStringLiteral("width")).toInt(-1)
               == decodedNarrowFrame.width()
        && narrowFramePayload.value(QStringLiteral("height")).toInt(-1)
               == decodedNarrowFrame.height();
    g54 ? pass("G54 get_frame maxWidth shrinks image")
        : fail("G54 get_frame maxWidth shrinks image",
               QStringLiteral("maxWidth did not reduce the decoded frame width"));

    QTemporaryDir exportRoot;
    const bool exportRootReady = exportRoot.isValid();
    QJsonObject missingParentResponse;
    if (exportRootReady) {
        missingParentResponse = callProjectInfoTool(
            59, QStringLiteral("export_video"), QJsonObject{
                {QStringLiteral("outputPath"),
                 QDir(exportRoot.path()).filePath(
                     QStringLiteral("missing-parent/output.mp4"))}
            });
    }
    const QJsonObject missingParentEnvelope = toolResult(missingParentResponse);
    const QJsonObject missingParentPayload = toolPayload(missingParentResponse);
    const bool g55 = exportRootReady
        && missingParentEnvelope.value(QStringLiteral("isError")).toBool(false)
        && missingParentPayload.isEmpty();
    g55 ? pass("G55 export_video rejects missing parent directory")
        : fail("G55 export_video rejects missing parent directory",
               QStringLiteral("an output path below a missing directory was accepted"));

    const QString exportOutputPath = exportRootReady
        ? QDir(exportRoot.path()).filePath(QStringLiteral("output.mp4"))
        : QString();
    QElapsedTimer exportTimer;
    exportTimer.start();
    QJsonObject exportResponse;
    if (exportRootReady && timelineReady) {
        exportResponse = callProjectInfoTool(
            60, QStringLiteral("export_video"), QJsonObject{
                {QStringLiteral("outputPath"), exportOutputPath}
            });
    }
    const qint64 exportElapsedMs = exportTimer.elapsed();
    const QJsonObject exportEnvelope = toolResult(exportResponse);
    const QJsonObject exportPayload = toolPayload(exportResponse);
    const QString exportJobId = exportPayload.value(QStringLiteral("jobId")).toString();
    const bool g56 = exportRootReady && timelineReady
        && !exportEnvelope.value(QStringLiteral("isError")).toBool(true)
        && exportPayload.value(QStringLiteral("ok")).toBool(false)
        && !exportJobId.isEmpty()
        && exportPayload.value(QStringLiteral("status")).toString()
               == QStringLiteral("queued")
        && exportPayload.value(QStringLiteral("width")).toInt(-1)
               == projectInfoResult.value(QStringLiteral("width")).toInt(-2)
        && exportPayload.value(QStringLiteral("height")).toInt(-1)
               == projectInfoResult.value(QStringLiteral("height")).toInt(-2)
        && qFuzzyCompare(exportPayload.value(QStringLiteral("fps")).toDouble(-1.0),
                         projectInfoResult.value(QStringLiteral("fps")).toDouble(-2.0))
        && exportElapsedMs < 3000;
    g56 ? pass("G56 export_video returns jobId without waiting")
        : fail("G56 export_video returns jobId without waiting",
               QStringLiteral("export_video did not return a queued job promptly"));

    QJsonObject knownStatusResponse;
    if (!exportJobId.isEmpty()) {
        knownStatusResponse = callProjectInfoTool(
            61, QStringLiteral("get_export_status"), QJsonObject{
                {QStringLiteral("jobId"), exportJobId}
            });
    }
    const QJsonObject knownStatusEnvelope = toolResult(knownStatusResponse);
    const QJsonObject knownStatusPayload = toolPayload(knownStatusResponse);
    const QSet<QString> validExportStatuses{
        QStringLiteral("queued"), QStringLiteral("running"),
        QStringLiteral("done"), QStringLiteral("failed")
    };
    const int knownProgress = knownStatusPayload.value(QStringLiteral("progress"))
        .toInt(-1);
    const bool g57 = g56
        && !knownStatusEnvelope.value(QStringLiteral("isError")).toBool(true)
        && knownStatusPayload.value(QStringLiteral("ok")).toBool(false)
        && validExportStatuses.contains(
               knownStatusPayload.value(QStringLiteral("status")).toString())
        && knownProgress >= 0 && knownProgress <= 100;
    g57 ? pass("G57 get_export_status returns job state")
        : fail("G57 get_export_status returns job state",
               QStringLiteral("the queued export status was not readable"));

    const QJsonObject unknownStatusResponse = callProjectInfoTool(
        62, QStringLiteral("get_export_status"), QJsonObject{
            {QStringLiteral("jobId"), QStringLiteral("unknown-mcp-job")}
        });
    const QJsonObject unknownStatusEnvelope = toolResult(unknownStatusResponse);
    const QJsonObject unknownStatusPayload = toolPayload(unknownStatusResponse);
    const bool g58 = unknownStatusEnvelope.value(QStringLiteral("isError")).toBool(false)
        && unknownStatusPayload.isEmpty();
    g58 ? pass("G58 unknown export job is rejected")
        : fail("G58 unknown export job is rejected",
               QStringLiteral("an unknown jobId did not return isError"));

    const QJsonObject commandListResponse = callProjectInfoTool(
        37, QStringLiteral("list_commands"), QJsonObject{});
    const QJsonObject commandListResult = toolPayload(commandListResponse);
    const QJsonArray commands = commandListResult.value(QStringLiteral("commands")).toArray();
    QSet<QString> commandIds;
    const QSet<QString> validRisks{
        QStringLiteral("safe"), QStringLiteral("blocking"), QStringLiteral("quit")
    };
    bool commandIdsUnique = true;
    bool allRisksAssigned = true;
    bool exitIsQuit = false;
    bool searchMenuCommandFound = false;
    QString blockingId;
    QString quitId;
    for (const QJsonValue& value : commands) {
        const QJsonObject command = value.toObject();
        const QString id = command.value(QStringLiteral("id")).toString();
        const QString label = command.value(QStringLiteral("label")).toString();
        const QString risk = command.value(QStringLiteral("risk")).toString();
        commandIdsUnique = commandIdsUnique && !id.isEmpty() && !commandIds.contains(id);
        commandIds.insert(id);
        allRisksAssigned = allRisksAssigned && validRisks.contains(risk);
        if (risk == QStringLiteral("blocking") && blockingId.isEmpty())
            blockingId = id;
        if (risk == QStringLiteral("quit"))
            quitId = id;
        if (label.startsWith(QStringLiteral("終了")))
            exitIsQuit = risk == QStringLiteral("quit");
        if (command.value(QStringLiteral("menuPath")).toString()
                == QStringLiteral("検索")
            && id.startsWith(QStringLiteral("search."))) {
            searchMenuCommandFound = true;
        }
    }
    const bool g37 = commandIdsUnique
        && commandListResult.value(QStringLiteral("total")).toInt() == commands.size();
    g37 ? pass("G37 list_commands IDs are unique")
        : fail("G37 list_commands IDs are unique",
               QStringLiteral("duplicate, empty, or missing command IDs were returned"));

    const bool g38 = allRisksAssigned && !quitId.isEmpty() && exitIsQuit;
    g38 ? pass("G38 command risks are assigned")
        : fail("G38 command risks are assigned",
               QStringLiteral("risk is missing/invalid or the exit action is not quit"));

    QJsonObject blockingResponse;
    if (!blockingId.isEmpty()) {
        blockingResponse = callProjectInfoTool(
            39, QStringLiteral("run_command"), QJsonObject{
                {QStringLiteral("id"), blockingId}
            });
    }
    const QJsonObject blockingResult = blockingResponse
        .value(QStringLiteral("result")).toObject();
    const QJsonArray blockingContent = blockingResult.value(QStringLiteral("content"))
        .toArray();
    const QString blockingErrorText = blockingContent.isEmpty()
        ? QString()
        : blockingContent.first().toObject().value(QStringLiteral("text")).toString();
    const bool g39 = !blockingId.isEmpty()
        && blockingResult.value(QStringLiteral("isError")).toBool(false)
        && blockingErrorText.contains(QStringLiteral("allowBlocking:true"));
    g39 ? pass("G39 blocking command is rejected by default")
        : fail("G39 blocking command is rejected by default",
               QStringLiteral("a blocking command was not rejected without allowBlocking"));

    QJsonObject quitResponse;
    if (!quitId.isEmpty()) {
        quitResponse = callProjectInfoTool(
            40, QStringLiteral("run_command"), QJsonObject{
                {QStringLiteral("id"), quitId},
                {QStringLiteral("allowBlocking"), true}
            });
    }
    const QJsonObject quitResult = quitResponse.value(QStringLiteral("result")).toObject();
    const QJsonArray quitContent = quitResult.value(QStringLiteral("content")).toArray();
    const QString quitErrorText = quitContent.isEmpty()
        ? QString()
        : quitContent.first().toObject().value(QStringLiteral("text")).toString();
    const bool g40 = !quitId.isEmpty()
        && quitResult.value(QStringLiteral("isError")).toBool(false)
        && quitErrorText
               == QStringLiteral("このコマンドはエディタを終了させるため MCP からは実行できません。");
    g40 ? pass("G40 quit command is always rejected")
        : fail("G40 quit command is always rejected",
               QStringLiteral("quit was accepted with allowBlocking"));

    const bool g41 = searchMenuCommandFound;
    g41 ? pass("G41 search menu command is listed")
        : fail("G41 search menu command is listed",
               QStringLiteral("the 検索 menu command was not returned by list_commands"));

    const QHash<QString, QJsonObject> nullWindowWriteArguments{
        {QStringLiteral("split_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("timeSec"), 1.0}
        }},
        {QStringLiteral("delete_clip"), QJsonObject{{QStringLiteral("clipIndex"), 0}}},
        {QStringLiteral("move_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("newStartSec"), 0.0}
        }},
        {QStringLiteral("set_clip_property"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("value"), 1.0}
        }}
    };
    bool nullWindowWriteToolsSafe = true;
    for (const QString& toolName : changedToolNames) {
        const QJsonObject response = callWriteTool(
            31, toolName, nullWindowWriteArguments.value(toolName));
        nullWindowWriteToolsSafe = nullWindowWriteToolsSafe
            && response.value(QStringLiteral("result")).toObject()
                   .value(QStringLiteral("isError")).toBool(false);
    }
    nullWindowWriteToolsSafe ? pass("G31 null editor changed tools are safe")
                             : fail("G31 null editor changed tools are safe",
                                    QStringLiteral("a changed tool did not return isError"));

    const bool hadTimeoutEnvironment =
        qEnvironmentVariableIsSet("VEDITOR_MCP_TIMEOUT_MS");
    const QByteArray previousTimeoutEnvironment =
        qgetenv("VEDITOR_MCP_TIMEOUT_MS");
    qunsetenv("VEDITOR_MCP_TIMEOUT_MS");
    const bool defaultTimeout = McpStdioBridge::requestTimeoutMs() == 120000;
    qputenv("VEDITOR_MCP_TIMEOUT_MS", QByteArray("2500"));
    const bool overriddenTimeout = McpStdioBridge::requestTimeoutMs() == 2500;
    qputenv("VEDITOR_MCP_TIMEOUT_MS", QByteArray("999"));
    const bool tooShortFallsBack = McpStdioBridge::requestTimeoutMs() == 120000;
    if (hadTimeoutEnvironment)
        qputenv("VEDITOR_MCP_TIMEOUT_MS", previousTimeoutEnvironment);
    else
        qunsetenv("VEDITOR_MCP_TIMEOUT_MS");
    const bool g35 = defaultTimeout && overriddenTimeout && tooShortFallsBack;
    g35 ? pass("G35 stdio timeout default and environment override")
        : fail("G35 stdio timeout default and environment override",
               QStringLiteral("timeout selection is wrong"));

    bool noIdHadId = true;
    bool nullIdHadId = false;
    const QJsonValue noId = McpStdioBridge::extractRequestId(
        QByteArray("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/ping\"}"),
        &noIdHadId);
    const QJsonValue nullId = McpStdioBridge::extractRequestId(
        QByteArray("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}"),
        &nullIdHadId);
    const bool g36 = !noIdHadId && noId.isNull() && nullIdHadId && nullId.isNull();
    g36 ? pass("G36 request id absence versus null")
        : fail("G36 request id absence versus null",
               QStringLiteral("id presence was not distinguished"));

    server.stop();
    qInfo().noquote().nospace() << "[mcp] selftest end, passed=" << passed
                                << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
