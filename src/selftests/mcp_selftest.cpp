#include "../ClipGeometry.h"
#include <QEventLoop>
#include <QElapsedTimer>
#include <QAction>
#include <QByteArray>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QColor>
#include <QCoreApplication>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QTemporaryFile>
#include <QStandardPaths>
#include <QTest>
#include <QTemporaryDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVector>
#include <QStringList>
#include <QSet>
#include <QDebug>
#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../clipanim/ClipAnim.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpHttpServer.h"
#include "../mcp/McpProtocol.h"
#include "../mcp/McpStdioBridge.h"
#include "../AudioExportMix.h"
#include "../mcp/McpToolRegistry.h"
#include "SelftestGate.h"
#include "../AiChatDock.h"
#include "../CaptionEditorDialog.h"
#include "../CaptionTrack.h"
#include "../MainWindow.h"
#include "../MusicRemix.h"
#include "../RenderQueue.h"
#include "../ShapeLayer.h"
#include "../ExportUserPresets.h"
#include "../LoudnessMaster.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/AudioExtract.h"
#include "../libavcore/Probe.h"
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
    selftest::GateTally tally;
    // Record operands as the original short-circuit expression evaluates them.
    // This preserves the gate predicate and avoids evaluating unsafe operands
    // after an earlier guard has failed. A skipped operand has no actual value.
    QHash<int, QStringList> gateOperands;
    auto observeTerm = [&](int gate, const char* name, bool actual) {
        gateOperands[gate].append(QString::fromUtf8(name)
                                  + (actual ? QStringLiteral("=true") : QStringLiteral("=false"))
                                  + QStringLiteral(" (expected true)"));
        return actual;
    };
    // Gate names start with "G<n> "; anything else is an explicit G0 FAIL.
    auto gateNumber = [](const char* name) {
        const QString text = QString::fromUtf8(name);
        if (!text.startsWith(QLatin1Char('G'))) return 0;
        int end = 1;
        while (end < text.size() && text.at(end).isDigit()) ++end;
        if (end == 1 || end >= text.size() || text.at(end) != QLatin1Char(' ')) return 0;
        return text.mid(1, end - 1).toInt();
    };
    auto pass = [&](const char* name) {
        const int n = gateNumber(name);
        gateOperands.remove(n);
        if (n > 0) {
            qInfo().noquote() << "[mcp] PASS" << name;
            tally.gate(n, true);
        } else {
            qWarning().noquote() << "[mcp] FAIL" << name << ": unparsable gate name";
            tally.gate(0, false, QByteArray(QByteArrayLiteral("unparsable gate name: ") + name).constData());
        }
    };
    auto fail = [&](const char* name, const QString& message) {
        const int n = gateNumber(name);
        const QStringList operands = gateOperands.take(n);
        const QString detail = operands.isEmpty()
            ? message : message + QStringLiteral("; ") + operands.join(QStringLiteral("; "));
        qWarning().noquote() << "[mcp] FAIL" << name << ":" << detail;
        tally.gate(n, false, detail.toUtf8().constData());
    };
    auto tallyObserved = [&](int n, bool ok, const char* message) {
        const QStringList operands = gateOperands.take(n);
        const QString detail = operands.isEmpty()
            ? QString::fromUtf8(message)
            : QString::fromUtf8(message) + QStringLiteral("; ")
                + operands.join(QStringLiteral("; "));
        return tally.gate(n, ok, detail.toUtf8().constData());
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
    const bool g1 = observeTerm(1, "initializeResult.value(QStringLiteral(\"protocolVersion\")).toString() == QStringLiteral(\"2025-06-18\")", static_cast<bool>(initializeResult.value(QStringLiteral("protocolVersion")).toString()
            == QStringLiteral("2025-06-18")))
        && observeTerm(1, "initializeResult.value(QStringLiteral(\"capabilities\")).toObject() .value(QStringLiteral(\"tools\")).isObject()", static_cast<bool>(initializeResult.value(QStringLiteral("capabilities")).toObject()
            .value(QStringLiteral("tools")).isObject()))
        && observeTerm(1, "initializeResult.value(QStringLiteral(\"serverInfo\")).toObject() .value(QStringLiteral(\"name\")).toString() == QStringLiteral(\"v-simple-editor\")", static_cast<bool>(initializeResult.value(QStringLiteral("serverInfo")).toObject()
            .value(QStringLiteral("name")).toString() == QStringLiteral("v-simple-editor")));
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
    const bool g70 = observeTerm(70, "supportedInitialize.value(QStringLiteral(\"result\")).toObject() .value(QStringLiteral(\"protocolVersion\")).toString() == QStringLiteral(\"2025-03-26\")", static_cast<bool>(supportedInitialize.value(QStringLiteral("result")).toObject()
                         .value(QStringLiteral("protocolVersion")).toString()
                         == QStringLiteral("2025-03-26")))
        && observeTerm(70, "unsupportedInitialize.value(QStringLiteral(\"result\")).toObject() .value(QStringLiteral(\"protocolVersion\")).toString() == QStringLiteral(\"2025-06-18\")", static_cast<bool>(unsupportedInitialize.value(QStringLiteral("result")).toObject()
               .value(QStringLiteral("protocolVersion")).toString()
               == QStringLiteral("2025-06-18")));
    g70 ? pass("G70 initialize version negotiation")
        : fail("G70 initialize version negotiation",
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
    const bool g6 = observeTerm(6, "hasEcho", static_cast<bool>(hasEcho)) && observeTerm(6, "hasErrorTool", static_cast<bool>(hasErrorTool)) && observeTerm(6, "schemasHeld", static_cast<bool>(schemasHeld));
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
    const bool g7 = observeTerm(7, "!successResult.value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!successResult.value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(7, "toolPayload(successCall) .value(QStringLiteral(\"value\")).toInt() == 42", static_cast<bool>(toolPayload(successCall)
            .value(QStringLiteral("value")).toInt() == 42));
    g7 ? pass("G7 successful tool call")
       : fail("G7 successful tool call", QStringLiteral("structured result was not preserved"));

    callParams.insert(QStringLiteral("name"), QStringLiteral("error-tool"));
    const QJsonObject errorCall = parseObject(protocol.handleMessage(compact(
        rpcRequest(5, QStringLiteral("tools/call"), callParams))));
    const QJsonObject errorResult = errorCall.value(QStringLiteral("result")).toObject();
    const QJsonArray errorContent = errorResult.value(QStringLiteral("content")).toArray();
    const bool g8 = observeTerm(8, "errorResult.value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(errorResult.value(QStringLiteral("isError")).toBool(false)))
        && observeTerm(8, "!errorCall.contains(QStringLiteral(\"error\"))", static_cast<bool>(!errorCall.contains(QStringLiteral("error"))))
        && observeTerm(8, "!errorContent.isEmpty()", static_cast<bool>(!errorContent.isEmpty()))
        && observeTerm(8, "errorContent.first().toObject().value(QStringLiteral(\"text\")).toString() == QStringLiteral(\"expected tool error\")", static_cast<bool>(errorContent.first().toObject().value(QStringLiteral("text")).toString()
            == QStringLiteral("expected tool error")));
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
    const bool g80Image = observeTerm(80, "!imageResult.value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!imageResult.value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(80, "imageContent.size() == 1", static_cast<bool>(imageContent.size() == 1))
        && observeTerm(80, "imageItem.value(QStringLiteral(\"type\")).toString() == QStringLiteral(\"image\")", static_cast<bool>(imageItem.value(QStringLiteral("type")).toString() == QStringLiteral("image")))
        && observeTerm(80, "imageItem.value(QStringLiteral(\"data\")).toString() == QStringLiteral(\"aGVsbG8=\")", static_cast<bool>(imageItem.value(QStringLiteral("data")).toString() == QStringLiteral("aGVsbG8=")))
        && observeTerm(80, "imageItem.value(QStringLiteral(\"mimeType\")).toString() == QStringLiteral(\"image/png\")", static_cast<bool>(imageItem.value(QStringLiteral("mimeType")).toString()
               == QStringLiteral("image/png")))
        && observeTerm(80, "imagePayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(imagePayload.value(QStringLiteral("ok")).toBool(false)));
    g80Image ? pass("G80 image content result shape")
             : fail("G80 image content result shape",
                    QStringLiteral("image content was not emitted in MCP shape"));

    mcp::McpHttpServer server(&registry);
    const bool serverStarted = server.start();
    if (!serverStarted) {
        fail("G11 HTTP bearer POST", QStringLiteral("server failed to start"));
        fail("G12 HTTP unauthorized", QStringLiteral("server failed to start"));
        fail("G13 HTTP query token", QStringLiteral("server failed to start"));
        fail("G14 HTTP GET rejected", QStringLiteral("server failed to start"));
        fail("G32 oversized HTTP header", QStringLiteral("server failed to start"));
        fail("G33 chunked transfer rejected", QStringLiteral("server failed to start"));
        fail("G34 duplicate content-length rejected", QStringLiteral("server failed to start"));
        fail("G66 HTTP Origin rejected", QStringLiteral("server failed to start"));
        fail("G67 HTTP localhost Origin allowed", QStringLiteral("server failed to start"));
        fail("G68 HTTP content-type rejected", QStringLiteral("server failed to start"));
        fail("G69 HTTP protocol header", QStringLiteral("server failed to start"));
        fail("G73 connection limit 503", QStringLiteral("server failed to start"));
        fail("G74 stdio bridge no head-of-line blocking / timeout",
             QStringLiteral("server failed to start"));
    } else {
        QNetworkAccessManager manager;
        const QUrl endpoint(server.endpointUrl());
        const QByteArray ping = compact(rpcRequest(8, QStringLiteral("ping")));

        const HttpResult bearer = post(&manager, endpoint, ping,
                                       server.token().toUtf8());
        const QJsonObject bearerJson = parseObject(bearer.body);
        const bool g11 = observeTerm(11, "bearer.status == 200", static_cast<bool>(bearer.status == 200))
            && observeTerm(11, "bearerJson.value(QStringLiteral(\"jsonrpc\")).toString() == QStringLiteral(\"2.0\")", static_cast<bool>(bearerJson.value(QStringLiteral("jsonrpc")).toString() == QStringLiteral("2.0")))
            && observeTerm(11, "bearerJson.value(QStringLiteral(\"result\")).isObject()", static_cast<bool>(bearerJson.value(QStringLiteral("result")).isObject()));
        g11 ? pass("G11 HTTP bearer POST")
            : fail("G11 HTTP bearer POST", QStringLiteral("status=%1").arg(bearer.status));

        const HttpResult noToken = post(&manager, endpoint, ping);
        const HttpResult badToken = post(&manager, endpoint, ping,
                                         QByteArray("wrong-token"));
        const bool g12 = observeTerm(12, "noToken.status == 401", static_cast<bool>(noToken.status == 401)) && observeTerm(12, "badToken.status == 401", static_cast<bool>(badToken.status == 401));
        g12 ? pass("G12 HTTP unauthorized")
            : fail("G12 HTTP unauthorized", QStringLiteral("noToken=%1 badToken=%2")
                    .arg(noToken.status).arg(badToken.status));

        QUrl queryEndpoint(endpoint);
        QUrlQuery query(queryEndpoint);
        query.addQueryItem(QStringLiteral("token"), server.token());
        queryEndpoint.setQuery(query);
        const HttpResult queryToken = post(&manager, queryEndpoint, ping);
        const QJsonObject queryJson = parseObject(queryToken.body);
        const bool g13 = observeTerm(13, "queryToken.status == 200", static_cast<bool>(queryToken.status == 200))
            && observeTerm(13, "queryJson.value(QStringLiteral(\"result\")).isObject()", static_cast<bool>(queryJson.value(QStringLiteral("result")).isObject()));
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
        const bool g66 = observeTerm(66, "evilOrigin.status == 403", static_cast<bool>(evilOrigin.status == 403)) && observeTerm(66, "nullOrigin.status == 403", static_cast<bool>(nullOrigin.status == 403));
        g66 ? pass("G66 HTTP Origin rejected")
            : fail("G66 HTTP Origin rejected",
                   QStringLiteral("evil=%1 null=%2")
                       .arg(evilOrigin.status).arg(nullOrigin.status));

        // G101: Host ヘッダが loopback 以外 (DNS リバインディング経由) なら 403。
        // loopback (ポート付き / IPv6 括弧付き) はそのまま通る。
        const HttpResult evilHost = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"Host", "evil.example:8765"}});
        const HttpResult portHost = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"Host", "localhost:8765"}});
        const HttpResult ipv6Host = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"Host", "[::1]:8765"}});
        const bool g101 = observeTerm(101, "evilHost.status == 403", static_cast<bool>(evilHost.status == 403)) && observeTerm(101, "portHost.status == 200", static_cast<bool>(portHost.status == 200))
            && observeTerm(101, "ipv6Host.status == 200", static_cast<bool>(ipv6Host.status == 200));
        g101 ? pass("G101 HTTP non-loopback Host rejected")
             : fail("G101 HTTP non-loopback Host rejected",
                    QStringLiteral("evil=%1 localhost:port=%2 [::1]:port=%3")
                        .arg(evilHost.status).arg(portHost.status).arg(ipv6Host.status));

        // G113: initialize の clientInfo を記録して通知する (どのクライアントが接続したか)。
        {
            const QByteArray initBody = QJsonDocument(QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), 77},
                {QStringLiteral("method"), QStringLiteral("initialize")},
                {QStringLiteral("params"), QJsonObject{
                    {QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")},
                    {QStringLiteral("capabilities"), QJsonObject{}},
                    {QStringLiteral("clientInfo"), QJsonObject{
                        {QStringLiteral("name"), QStringLiteral("codex-selftest")},
                        {QStringLiteral("version"), QStringLiteral("9.9")}}}}}
            }).toJson(QJsonDocument::Compact);
            QString seenName;
            QString seenVersion;
            const QMetaObject::Connection clientConnection = QObject::connect(
                &server, &mcp::McpHttpServer::clientInitialized,
                [&seenName, &seenVersion](const QString& name, const QString& version) {
                    seenName = name;
                    seenVersion = version;
                });
            const HttpResult initResult = post(
                &manager, endpoint, initBody, server.token().toUtf8(),
                QHash<QByteArray, QByteArray>{});
            QObject::disconnect(clientConnection);
            const bool g113 = observeTerm(113, "initResult.status == 200", static_cast<bool>(initResult.status == 200))
                && observeTerm(113, "server.lastClientName() == QStringLiteral(\"codex-selftest\")", static_cast<bool>(server.lastClientName() == QStringLiteral("codex-selftest")))
                && observeTerm(113, "server.lastClientVersion() == QStringLiteral(\"9.9\")", static_cast<bool>(server.lastClientVersion() == QStringLiteral("9.9")))
                && observeTerm(113, "seenName == QStringLiteral(\"codex-selftest\")", static_cast<bool>(seenName == QStringLiteral("codex-selftest")))
                && observeTerm(113, "seenVersion == QStringLiteral(\"9.9\")", static_cast<bool>(seenVersion == QStringLiteral("9.9")));
            g113 ? pass("G113 initialize clientInfo is recorded and announced")
                 : fail("G113 initialize clientInfo is recorded and announced",
                        QStringLiteral("status=%1 name=%2 version=%3 seen=%4")
                            .arg(initResult.status).arg(server.lastClientName(),
                                                        server.lastClientVersion(), seenName));
        }

        const HttpResult localhostOrigin = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"Origin", "http://localhost:3000"}});
        const bool g67 = observeTerm(67, "localhostOrigin.status == 200", static_cast<bool>(localhostOrigin.status == 200))
            && observeTerm(67, "localhostOrigin.allowOrigin == \"http://localhost:3000\"", static_cast<bool>(localhostOrigin.allowOrigin == "http://localhost:3000"));
        g67 ? pass("G67 HTTP localhost Origin allowed")
            : fail("G67 HTTP localhost Origin allowed",
                   QStringLiteral("status=%1 allowOrigin=%2")
                       .arg(localhostOrigin.status)
                       .arg(QString::fromLatin1(localhostOrigin.allowOrigin)));

        const HttpResult badContentType = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            {}, QByteArray("text/plain"));
        badContentType.status == 415
            ? pass("G68 HTTP content-type rejected")
            : fail("G68 HTTP content-type rejected",
                   QStringLiteral("status=%1").arg(badContentType.status));

        const HttpResult unsupportedVersion = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"MCP-Protocol-Version", "1999-01-01"}});
        const HttpResult supportedVersion = post(
            &manager, endpoint, ping, server.token().toUtf8(),
            QHash<QByteArray, QByteArray>{{"MCP-Protocol-Version", "2025-03-26"}});
        const bool g69 = observeTerm(69, "unsupportedVersion.status == 400", static_cast<bool>(unsupportedVersion.status == 400))
            && observeTerm(69, "supportedVersion.status == 200", static_cast<bool>(supportedVersion.status == 200));
        g69 ? pass("G69 HTTP protocol header")
            : fail("G69 HTTP protocol header",
                   QStringLiteral("unsupported=%1 supported=%2")
                       .arg(unsupportedVersion.status)
                       .arg(supportedVersion.status));

        auto activeServerConnections = [&server]() {
            int active = 0;
            for (QTcpSocket* socket : server.findChildren<QTcpSocket*>()) {
                if (socket->state() == QAbstractSocket::ConnectedState)
                    ++active;
            }
            return active;
        };
        const int activeConnectionsBeforeIdle = activeServerConnections();
        QList<QPointer<QTcpSocket>> idle;
        idle.reserve(mcp::McpHttpServer::maxConnections());
        for (int index = 0; index < mcp::McpHttpServer::maxConnections(); ++index) {
            QTcpSocket* socket = new QTcpSocket;
            idle.append(QPointer<QTcpSocket>(socket));
            socket->connectToHost(QHostAddress::LocalHost, server.port());
        }
        QElapsedTimer limitWaitTimer;
        limitWaitTimer.start();
        const bool limitReady = QTest::qWaitFor([&]() {
            return activeServerConnections() == mcp::McpHttpServer::maxConnections();
        }, 5000);
        const qint64 limitWaitMs = limitWaitTimer.elapsed();
        const QByteArray connectionLimitRequest(
            "POST /mcp HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 0\r\n\r\n");
        const int connectionLimitStatus = rawHttpStatus(
            server.port(), connectionLimitRequest);
        connectionLimitStatus == 503
            ? pass("G73 connection limit 503")
            : fail("G73 connection limit 503",
                   QStringLiteral("status=%1 (expected 503), activeConnections=%2 (expected %3), limitReady=%4 (expected true)")
                       .arg(connectionLimitStatus)
                       .arg(activeServerConnections())
                       .arg(mcp::McpHttpServer::maxConnections())
                       .arg(limitReady));
        for (const QPointer<QTcpSocket>& socket : idle) {
            if (!socket)
                continue;
            socket->close();
            socket->deleteLater();
        }
        QElapsedTimer closeWaitTimer;
        closeWaitTimer.start();
        const bool idleClosed = QTest::qWaitFor([&]() {
            return activeServerConnections() <= activeConnectionsBeforeIdle;
        }, 5000);
        const qint64 closeWaitMs = closeWaitTimer.elapsed();
        qInfo().noquote() << "[mcp] G73 wait detail: activeBefore=" << activeConnectionsBeforeIdle
                          << "limitReady=" << limitReady << "limitWaitMs=" << limitWaitMs
                          << "idleClosed=" << idleClosed << "closeWaitMs=" << closeWaitMs;
        if (!idleClosed)
            qWarning().noquote() << "[mcp] G73 idle socket cleanup timed out: activeConnections="
                                 << activeServerConnections()
                                 << "expectedAtMost=" << activeConnectionsBeforeIdle;

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
        const bool g74 = observeTerm(74, "firstResponseIsPing", static_cast<bool>(firstResponseIsPing)) && observeTerm(74, "slowRequestTimedOut", static_cast<bool>(slowRequestTimedOut))
            && observeTerm(74, "finalPingSucceeded", static_cast<bool>(finalPingSucceeded)) && observeTerm(74, "bridgeResponses.size() == 3", static_cast<bool>(bridgeResponses.size() == 3));
        g74 ? pass("G74 stdio bridge no head-of-line blocking / timeout")
            : fail("G74 stdio bridge no head-of-line blocking / timeout",
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
        allReadToolsListed = observeTerm(15, "allReadToolsListed", static_cast<bool>(allReadToolsListed)) && observeTerm(15, "found", static_cast<bool>(found));
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
        allReadToolsError = observeTerm(16, "allReadToolsError", static_cast<bool>(allReadToolsError))
            && observeTerm(16, "result.value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(result.value(QStringLiteral("isError")).toBool(false)));
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
        QStringLiteral("set_track_locked"),
        QStringLiteral("match_frame"),
        QStringLiteral("replace_clip"),
        QStringLiteral("relink_media"),
        QStringLiteral("set_clip_property"),
        QStringLiteral("dynamic_zoom"),
        QStringLiteral("set_clip_label"),
        QStringLiteral("trim_clip"),
        QStringLiteral("set_transition"),
        QStringLiteral("add_text_overlay"),
        QStringLiteral("add_caption"),
        QStringLiteral("apply_captions"),
        QStringLiteral("set_playhead"),
        QStringLiteral("undo"),
        QStringLiteral("redo"),
        QStringLiteral("music_remix"),
        QStringLiteral("dialogue_level")
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
        allWriteToolsListed = observeTerm(17, "allWriteToolsListed", static_cast<bool>(allWriteToolsListed)) && observeTerm(17, "found", static_cast<bool>(found));
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
        allWriteToolsError = observeTerm(18, "allWriteToolsError", static_cast<bool>(allWriteToolsError))
            && observeTerm(18, "response.value(QStringLiteral(\"result\")).toObject() .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(response.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("isError")).toBool(false)));
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
    const bool g21 = observeTerm(21, "missingSplitTime.value(QStringLiteral(\"result\")).toObject() .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(missingSplitTime.value(QStringLiteral("result")).toObject()
                         .value(QStringLiteral("isError")).toBool(false)))
        && observeTerm(21, "nanSplitTime.value(QStringLiteral(\"result\")).toObject() .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(nanSplitTime.value(QStringLiteral("result")).toObject()
               .value(QStringLiteral("isError")).toBool(false)));
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
    const bool g23 = observeTerm(23, "!compactJson.contains('\\n')", static_cast<bool>(!compactJson.contains('\n')))
        && observeTerm(23, "compactObject.value(QStringLiteral(\"jsonrpc\")).toString() == QStringLiteral(\"2.0\")", static_cast<bool>(compactObject.value(QStringLiteral("jsonrpc")).toString()
               == QStringLiteral("2.0")))
        && observeTerm(23, "compactObject.value(QStringLiteral(\"id\")).toInt() == 26", static_cast<bool>(compactObject.value(QStringLiteral("id")).toInt() == 26))
        && observeTerm(23, "compactObject.value(QStringLiteral(\"method\")).toString() == QStringLiteral(\"ping\")", static_cast<bool>(compactObject.value(QStringLiteral("method")).toString()
               == QStringLiteral("ping")));
    g23 ? pass("G23 JSON compact one-line framing")
        : fail("G23 JSON compact one-line framing",
               QStringLiteral("JSON was not compacted to one line"));

    const QJsonValue extractedId = McpStdioBridge::extractRequestId(compactJson);
    const QJsonValue malformedId = McpStdioBridge::extractRequestId(QByteArray("{"));
    const bool g24 = observeTerm(24, "extractedId.isDouble()", static_cast<bool>(extractedId.isDouble())) && observeTerm(24, "extractedId.toInt() == 26", static_cast<bool>(extractedId.toInt() == 26))
        && observeTerm(24, "malformedId.isNull()", static_cast<bool>(malformedId.isNull()));
    g24 ? pass("G24 request id extraction")
        : fail("G24 request id extraction",
               QStringLiteral("normal or malformed request id handling is wrong"));

    const QByteArray transportError = McpStdioBridge::makeTransportError(
        extractedId, QStringLiteral("editor not running"));
    const QJsonObject transportErrorObject = parseObject(transportError);
    const QJsonObject transportErrorDetails =
        transportErrorObject.value(QStringLiteral("error")).toObject();
    const bool g25 = observeTerm(25, "!transportError.contains('\\n')", static_cast<bool>(!transportError.contains('\n')))
        && observeTerm(25, "transportErrorObject.value(QStringLiteral(\"id\")).toInt() == 26", static_cast<bool>(transportErrorObject.value(QStringLiteral("id")).toInt() == 26))
        && observeTerm(25, "transportErrorDetails.value(QStringLiteral(\"code\")).toInt() == -32603", static_cast<bool>(transportErrorDetails.value(QStringLiteral("code")).toInt() == -32603))
        && observeTerm(25, "transportErrorDetails.value(QStringLiteral(\"message\")).toString() == QStringLiteral(\"editor not running\")", static_cast<bool>(transportErrorDetails.value(QStringLiteral("message")).toString()
               == QStringLiteral("editor not running")));
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
    const bool g26 = observeTerm(26, "!childEnvironment.contains(QStringLiteral(\"ANTHROPIC_API_KEY\"))", static_cast<bool>(!childEnvironment.contains(QStringLiteral("ANTHROPIC_API_KEY"))))
        && observeTerm(26, "!childEnvironment.contains(QStringLiteral(\"ANTHROPIC_AUTH_TOKEN\"))", static_cast<bool>(!childEnvironment.contains(QStringLiteral("ANTHROPIC_AUTH_TOKEN"))))
        && observeTerm(26, "childEnvironment.value(QStringLiteral(\"PATH\")) == QStringLiteral(\"keep-me\")", static_cast<bool>(childEnvironment.value(QStringLiteral("PATH")) == QStringLiteral("keep-me")));
    g26 ? pass("G26 child environment removes Anthropic credentials")
        : fail("G26 child environment removes Anthropic credentials",
               QStringLiteral("credential removal or PATH preservation failed"));

    const QStringList arguments = AiChatDock::buildArguments(
        QStringLiteral("/tmp/veditor-mcp.json"), QString());
    // プロンプトは argv に置かず stdin へ渡し、可変長の --allowedTools は最後に置く。
    const bool g27 = observeTerm(27, "arguments.contains(QStringLiteral(\"--strict-mcp-config\"))", static_cast<bool>(arguments.contains(QStringLiteral("--strict-mcp-config"))))
        && observeTerm(27, "arguments.indexOf(QStringLiteral(\"--output-format\")) >= 0", static_cast<bool>(arguments.indexOf(QStringLiteral("--output-format")) >= 0))
        && observeTerm(27, "arguments.value(arguments.indexOf(QStringLiteral(\"--output-format\")) + 1) == QStringLiteral(\"stream-json\")", static_cast<bool>(arguments.value(arguments.indexOf(QStringLiteral("--output-format")) + 1)
               == QStringLiteral("stream-json")))
        && observeTerm(27, "arguments.value(0) == QStringLiteral(\"-p\")", static_cast<bool>(arguments.value(0) == QStringLiteral("-p")))
        && observeTerm(27, "arguments.constLast() == QStringLiteral(\"mcp__veditor\")", static_cast<bool>(arguments.constLast() == QStringLiteral("mcp__veditor")));
    g27 ? pass("G27 AI chat CLI arguments")
        : fail("G27 AI chat CLI arguments",
               QStringLiteral("required stream-json arguments are wrong"));

    const QByteArray mcpConfig = AiChatDock::buildMcpConfig(
        9876, QStringLiteral("test-token"));
    const QJsonObject mcpConfigObject = parseObject(mcpConfig);
    const QJsonObject veditor = mcpConfigObject.value(QStringLiteral("mcpServers"))
        .toObject().value(QStringLiteral("veditor")).toObject();
    const bool g28 = observeTerm(28, "veditor.value(QStringLiteral(\"url\")).toString() == QStringLiteral(\"http://127.0.0.1:9876/mcp\")", static_cast<bool>(veditor.value(QStringLiteral("url")).toString()
                         == QStringLiteral("http://127.0.0.1:9876/mcp")))
        && observeTerm(28, "veditor.value(QStringLiteral(\"headers\")).toObject() .value(QStringLiteral(\"Authorization\")).toString() == QStringLiteral(\"Bearer test-token\")", static_cast<bool>(veditor.value(QStringLiteral("headers")).toObject()
               .value(QStringLiteral("Authorization")).toString()
               == QStringLiteral("Bearer test-token")));
    g28 ? pass("G28 AI chat MCP config JSON")
        : fail("G28 AI chat MCP config JSON",
               QStringLiteral("port or bearer token was not encoded correctly"));

    QTemporaryDir cliCommandDir;
    const QString fakeCmdPath = cliCommandDir.isValid()
        ? QDir(cliCommandDir.path()).filePath(QStringLiteral("claude.cmd"))
        : QString();
    bool fakeCmdReady = false;
    if (!fakeCmdPath.isEmpty()) {
        QFile fakeCmd(fakeCmdPath);
        fakeCmdReady = fakeCmd.open(QIODevice::WriteOnly);
        if (fakeCmdReady) {
            fakeCmd.write("@echo off\r\n");
            fakeCmd.close();
            fakeCmdReady = QFile::setPermissions(
                fakeCmdPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                    | QFileDevice::ExeOwner);
        }
    }
    const AiChatDock::CliCommand resolvedCmd = AiChatDock::resolveCliCommand(
        QStringLiteral("claude"), {cliCommandDir.path()});
    const bool g75 = observeTerm(75, "fakeCmdReady", static_cast<bool>(fakeCmdReady))
        && observeTerm(75, "QFileInfo(resolvedCmd.program).absoluteFilePath() == QFileInfo(fakeCmdPath).absoluteFilePath()", static_cast<bool>(QFileInfo(resolvedCmd.program).absoluteFilePath()
               == QFileInfo(fakeCmdPath).absoluteFilePath()))
        && observeTerm(75, "resolvedCmd.needsShell", static_cast<bool>(resolvedCmd.needsShell));
    g75 ? pass("G75 CLI resolver finds cmd shell script")
        : fail("G75 CLI resolver finds cmd shell script",
               QStringLiteral(".cmd was not resolved or was not marked for the shell"));

    QTemporaryDir cliExeDir;
    const QString fakeExePath = cliExeDir.isValid()
        ? QDir(cliExeDir.path()).filePath(QStringLiteral("claude.exe"))
        : QString();
    bool fakeExeReady = false;
    if (!fakeExePath.isEmpty()) {
        QFile fakeExe(fakeExePath);
        fakeExeReady = fakeExe.open(QIODevice::WriteOnly);
        if (fakeExeReady) {
            fakeExe.write("not a real executable\n");
            fakeExe.close();
            fakeExeReady = QFile::setPermissions(
                fakeExePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                    | QFileDevice::ExeOwner);
        }
    }
    const AiChatDock::CliCommand resolvedExe = AiChatDock::resolveCliCommand(
        QStringLiteral("claude.exe"), {cliExeDir.path()});
    const bool g76 = observeTerm(76, "fakeExeReady", static_cast<bool>(fakeExeReady)) && observeTerm(76, "!resolvedExe.program.isEmpty()", static_cast<bool>(!resolvedExe.program.isEmpty()))
        && observeTerm(76, "!resolvedExe.needsShell", static_cast<bool>(!resolvedExe.needsShell));
    g76 ? pass("G76 CLI resolver finds executable without shell")
        : fail("G76 CLI resolver finds executable without shell",
               QStringLiteral(".exe was not resolved as a direct executable"));

    QTemporaryDir missingCliDir;
    const AiChatDock::CliCommand missingCli = AiChatDock::resolveCliCommand(
        QStringLiteral("definitely-missing-cli-xyz"), {missingCliDir.path()});
    const bool g77 = observeTerm(77, "missingCliDir.isValid()", static_cast<bool>(missingCliDir.isValid())) && observeTerm(77, "missingCli.program.isEmpty()", static_cast<bool>(missingCli.program.isEmpty()))
        && observeTerm(77, "missingCli.searched.join(QStringLiteral(\"\\n\")) .contains(missingCliDir.path())", static_cast<bool>(missingCli.searched.join(QStringLiteral("\n"))
               .contains(missingCliDir.path())));
    g77 ? pass("G77 CLI resolver reports searched paths")
        : fail("G77 CLI resolver reports searched paths",
               QStringLiteral("a missing CLI did not report its search directory"));

    const QString shellLine = AiChatDock::buildShellCommandLine(
        QStringLiteral("C:\\p q\\claude.cmd"),
        {QStringLiteral("-p"), QStringLiteral("--mcp-config"),
         QStringLiteral("C:\\a&b|c^d.json")});
    const QString expectedShellLine = QStringLiteral(
        "\"C:\\p q\\claude.cmd\" \"-p\" \"--mcp-config\" \"C:\\a&b|c^d.json\"");
    bool metacharactersAreQuoted = true;
    int quoteDepth = 0;
    for (const QChar character : shellLine) {
        if (character == QLatin1Char('"'))
            quoteDepth = quoteDepth == 0 ? 1 : 0;
        else if (QStringLiteral("&|^").contains(character) && quoteDepth == 0)
            metacharactersAreQuoted = false;
    }
    QString shellError;
    const bool percentRejected = AiChatDock::buildShellCommandLine(
        QStringLiteral("claude.cmd"), {QStringLiteral("x%y")}, &shellError)
            .isEmpty() && !shellError.isEmpty();
    const bool quoteRejected = AiChatDock::buildShellCommandLine(
        QStringLiteral("claude.cmd"), {QStringLiteral("x\"y")}, &shellError)
            .isEmpty() && !shellError.isEmpty();
    const bool bangRejected = AiChatDock::buildShellCommandLine(
        QStringLiteral("claude.cmd"), {QStringLiteral("x!y")}, &shellError)
            .isEmpty() && !shellError.isEmpty();
    const bool g78 = observeTerm(78, "shellLine == expectedShellLine", static_cast<bool>(shellLine == expectedShellLine))
        && observeTerm(78, "shellLine.startsWith(QLatin1Char('\"'))", static_cast<bool>(shellLine.startsWith(QLatin1Char('"'))))
        && observeTerm(78, "shellLine.endsWith(QLatin1Char('\"'))", static_cast<bool>(shellLine.endsWith(QLatin1Char('"'))))
        && observeTerm(78, "metacharactersAreQuoted", static_cast<bool>(metacharactersAreQuoted))
        && observeTerm(78, "percentRejected", static_cast<bool>(percentRejected)) && observeTerm(78, "quoteRejected", static_cast<bool>(quoteRejected)) && observeTerm(78, "bangRejected", static_cast<bool>(bangRejected));
    g78 ? pass("G78 shell command quoting and rejection")
        : fail("G78 shell command quoting and rejection",
               QStringLiteral("cmd.exe command-line quoting or rejection is wrong"));

    const QStringList argumentsWithoutResume = AiChatDock::buildArguments(
        QStringLiteral("/tmp/c.json"), QString());
    const QStringList argumentsWithResume = AiChatDock::buildArguments(
        QStringLiteral("/tmp/c.json"), QStringLiteral("sess-1"));
    const int resumeIndex = argumentsWithResume.indexOf(QStringLiteral("--resume"));
    const int sessionIndex = argumentsWithResume.indexOf(QStringLiteral("sess-1"));
    const int allowedToolsIndex = argumentsWithResume.indexOf(
        QStringLiteral("--allowedTools"));
    const bool g79 = observeTerm(79, "argumentsWithoutResume.value(0) == QStringLiteral(\"-p\")", static_cast<bool>(argumentsWithoutResume.value(0) == QStringLiteral("-p")))
        && observeTerm(79, "!argumentsWithoutResume.contains(QStringLiteral(\"--resume\"))", static_cast<bool>(!argumentsWithoutResume.contains(QStringLiteral("--resume"))))
        && observeTerm(79, "argumentsWithoutResume.constLast() == QStringLiteral(\"mcp__veditor\")", static_cast<bool>(argumentsWithoutResume.constLast() == QStringLiteral("mcp__veditor")))
        && observeTerm(79, "resumeIndex >= 0", static_cast<bool>(resumeIndex >= 0)) && observeTerm(79, "sessionIndex == resumeIndex + 1", static_cast<bool>(sessionIndex == resumeIndex + 1))
        && observeTerm(79, "resumeIndex < allowedToolsIndex", static_cast<bool>(resumeIndex < allowedToolsIndex));
    g79 ? pass("G79 resume arguments precede allowed tools")
        : fail("G79 resume arguments precede allowed tools",
               QStringLiteral("--resume or stdin argument ordering is wrong"));

    const QStringList changedToolNames{
        QStringLiteral("split_clip"), QStringLiteral("delete_clip"),
        QStringLiteral("move_clip"), QStringLiteral("set_track_locked"),
        QStringLiteral("match_frame"), QStringLiteral("replace_clip"),
        QStringLiteral("relink_media"),
        QStringLiteral("set_clip_property"), QStringLiteral("dynamic_zoom"),
        QStringLiteral("set_clip_label"),
        QStringLiteral("trim_clip"), QStringLiteral("set_transition"),
        QStringLiteral("add_text_overlay")
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
        {QStringLiteral("set_track_locked"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("video")},
            {QStringLiteral("trackIndex"), 0}, {QStringLiteral("locked"), true},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("match_frame"), QJsonObject{
            {QStringLiteral("timeSec"), 0.0},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("replace_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0},
            {QStringLiteral("filePath"), QStringLiteral("missing.mp4")},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("relink_media"), QJsonObject{
            {QStringLiteral("mapping"), QJsonArray{
                QJsonObject{{QStringLiteral("from"), QStringLiteral("old.mp4")},
                            {QStringLiteral("to"), QStringLiteral("new.mp4")}}
            }},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("set_clip_property"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("value"), 1.0}, {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("dynamic_zoom"), QJsonObject{
            {QStringLiteral("clipIndex"), 0},
            {QStringLiteral("preset"), QStringLiteral("zoomIn")},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("set_clip_label"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("label"), QStringLiteral("red")},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("trim_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("edge"), QStringLiteral("in")},
            {QStringLiteral("timeSec"), 1.0}, {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("set_transition"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("type"), QStringLiteral("FadeOut")},
            {QStringLiteral("track"), QStringLiteral("video")}
        }},
        {QStringLiteral("add_text_overlay"), QJsonObject{
            {QStringLiteral("text"), QStringLiteral("text")},
            {QStringLiteral("startSec"), 0.0}, {QStringLiteral("endSec"), 1.0},
            {QStringLiteral("track"), QStringLiteral("video")}
        }}
    };
    bool unknownArgumentsRejected = true;
    for (const QString& toolName : changedToolNames) {
        const QJsonObject response = callWriteTool(
            29, toolName, unknownArgumentCases.value(toolName));
        const QJsonObject result = response.value(QStringLiteral("result")).toObject();
        unknownArgumentsRejected = observeTerm(29, "unknownArgumentsRejected", static_cast<bool>(unknownArgumentsRejected))
            && observeTerm(29, "result.value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(result.value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(29, "result.value(QStringLiteral(\"content\")).toArray().first() .toObject().value(QStringLiteral(\"text\")).toString() == QStringLiteral(\"unknown argument: track\")", static_cast<bool>(result.value(QStringLiteral("content")).toArray().first()
                   .toObject().value(QStringLiteral("text")).toString()
                   == QStringLiteral("unknown argument: track")));
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
    const bool g30 = observeTerm(30, "projectInfoResult.value(QStringLiteral(\"hasUnsavedChanges\")).isBool()", static_cast<bool>(projectInfoResult.value(QStringLiteral("hasUnsavedChanges")).isBool()))
        && observeTerm(30, "projectInfoResult.value(QStringLiteral(\"hasUnsavedChanges\")).toBool() == projectInfoWindow.isWindowModified()", static_cast<bool>(projectInfoResult.value(QStringLiteral("hasUnsavedChanges")).toBool()
            == projectInfoWindow.isWindowModified()));
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
    constexpr int kExpectedProjectInfoToolCount = 36 + 1 + 2 + 2 + 1 + 2 + 3 + 1 + 2 + 1 + 1 + 2 + 3 /* US-1711 */; // US-308: compare_project; US-312: render/decompose; US-400: color/LUT; US-406: track properties; US-509: sequences; US-511: track operations; US-605: group/ungroup; US-1201: connect/disconnect_clip; US-1711: set_clip_wiggle/set_clip_expression/get_clip_dynamics
    bool outputSchemasDeclared = projectInfoToolDescriptors.size()
        == kExpectedProjectInfoToolCount;
    for (const QJsonValue& value : projectInfoToolDescriptors) {
        outputSchemasDeclared = outputSchemasDeclared
            && value.toObject().value(QStringLiteral("outputSchema"))
                   .toObject().value(QStringLiteral("type")).toString()
                   == QStringLiteral("object");
    }
    const bool g71 = outputSchemasDeclared;
    g71 ? pass("G71 outputSchema declared")
        : fail("G71 outputSchema declared",
               QStringLiteral("expected %1 object output schemas, got %2")
                   .arg(kExpectedProjectInfoToolCount)
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
    const bool g42 = observeTerm(42, "saveWithoutPathResult.value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(saveWithoutPathResult.value(QStringLiteral("isError")).toBool(false)))
        && observeTerm(42, "toolErrorText(saveWithoutPath) == QStringLiteral(\"保存先のパスを指定してください\")", static_cast<bool>(toolErrorText(saveWithoutPath)
               == QStringLiteral("保存先のパスを指定してください")));
    g42 ? pass("G42 save without path is rejected non-interactively")
        : fail("G42 save without path is rejected non-interactively",
               QStringLiteral("save_project opened a dialog or returned a wrong error"));

    QTemporaryDir projectBehaviorDir;
    const bool projectBehaviorDirReady = projectBehaviorDir.isValid();
    const QString missingProjectPath = projectBehaviorDirReady
        ? QDir(projectBehaviorDir.path()).filePath(QStringLiteral("missing.vsep"))
        : QString();
    const QJsonObject missingProjectResponse = projectBehaviorDirReady
        ? callProjectInfoTool(
            81, QStringLiteral("open_project"), QJsonObject{
                {QStringLiteral("path"), missingProjectPath}
            })
        : QJsonObject();
    const bool g81 = observeTerm(81, "projectBehaviorDirReady", static_cast<bool>(projectBehaviorDirReady))
        && observeTerm(81, "toolResult(missingProjectResponse).value(QStringLiteral(\"isError\")) .toBool(false)", static_cast<bool>(toolResult(missingProjectResponse).value(QStringLiteral("isError"))
               .toBool(false)))
        && observeTerm(81, "toolErrorText(missingProjectResponse) == QStringLiteral(\"ファイルが見つかりません: %1\").arg(missingProjectPath)", static_cast<bool>(toolErrorText(missingProjectResponse)
               == QStringLiteral("ファイルが見つかりません: %1").arg(missingProjectPath)));
    g81 ? pass("G81 open_project missing file is rejected")
        : fail("G81 open_project missing file is rejected",
               QStringLiteral("missing project was accepted or returned a wrong error"));

    const QString roundtripProjectPath = projectBehaviorDirReady
        ? QDir(projectBehaviorDir.path()).filePath(QStringLiteral("roundtrip.vsep"))
        : QString();
    QJsonObject roundtripSaveResponse;
    QJsonObject roundtripOpenResponse;
    if (projectBehaviorDirReady) {
        roundtripSaveResponse = callProjectInfoTool(
            82, QStringLiteral("save_project"), QJsonObject{
                {QStringLiteral("path"), roundtripProjectPath}
            });
        roundtripOpenResponse = callProjectInfoTool(
            83, QStringLiteral("open_project"), QJsonObject{
                {QStringLiteral("path"), roundtripProjectPath}
            });
    }
    const QJsonObject roundtripSaveResult = toolResult(roundtripSaveResponse);
    const QJsonObject roundtripSavePayload = toolPayload(roundtripSaveResponse);
    const QJsonObject roundtripOpenResult = toolResult(roundtripOpenResponse);
    const QJsonObject roundtripOpenPayload = toolPayload(roundtripOpenResponse);
    const bool g82 = observeTerm(82, "projectBehaviorDirReady", static_cast<bool>(projectBehaviorDirReady))
        && observeTerm(82, "!roundtripSaveResult.value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!roundtripSaveResult.value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(82, "roundtripSavePayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(roundtripSavePayload.value(QStringLiteral("ok")).toBool(false)))
        && observeTerm(82, "requiredOutputFieldsPresent(QStringLiteral(\"save_project\"), roundtripSavePayload)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("save_project"),
                                       roundtripSavePayload)))
        && observeTerm(82, "roundtripSavePayload.value(QStringLiteral(\"path\")).toString() == roundtripProjectPath", static_cast<bool>(roundtripSavePayload.value(QStringLiteral("path")).toString()
               == roundtripProjectPath))
        && observeTerm(82, "QFileInfo(roundtripProjectPath).exists()", static_cast<bool>(QFileInfo(roundtripProjectPath).exists()))
        && observeTerm(82, "!roundtripOpenResult.value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!roundtripOpenResult.value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(82, "roundtripOpenPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(roundtripOpenPayload.value(QStringLiteral("ok")).toBool(false)))
        && observeTerm(82, "requiredOutputFieldsPresent(QStringLiteral(\"open_project\"), roundtripOpenPayload)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("open_project"),
                                       roundtripOpenPayload)))
        && observeTerm(82, "roundtripOpenPayload.value(QStringLiteral(\"path\")).toString() == roundtripProjectPath", static_cast<bool>(roundtripOpenPayload.value(QStringLiteral("path")).toString()
               == roundtripProjectPath))
        && observeTerm(82, "projectInfoWindow.projectDirectory() == QFileInfo(roundtripProjectPath).absolutePath()", static_cast<bool>(projectInfoWindow.projectDirectory()
               == QFileInfo(roundtripProjectPath).absolutePath()));
    g82 ? pass("G82 save_project/open_project roundtrip")
        : fail("G82 save_project/open_project roundtrip",
               QStringLiteral("save/open did not preserve the project path and response contract"));

    Timeline *projectTimeline = projectInfoWindow.findChild<Timeline *>(QStringLiteral("mainTimeline"));
    const QJsonObject missingImport = callProjectInfoTool(
        43, QStringLiteral("import_media"), QJsonObject{
            {QStringLiteral("filePath"),
             QStringLiteral("/definitely/missing/mcp-selftest-media.mp4")}
        });
    const bool g43 = observeTerm(43, "toolResult(missingImport).value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(missingImport).value(QStringLiteral("isError")).toBool(false)))
        && observeTerm(43, "toolErrorText(missingImport) == QStringLiteral(\"ファイルが見つかりません: /definitely/missing/mcp-selftest-media.mp4\")", static_cast<bool>(toolErrorText(missingImport)
               == QStringLiteral("ファイルが見つかりません: /definitely/missing/mcp-selftest-media.mp4")));
    g43 ? pass("G43 missing media import is rejected")
        : fail("G43 missing media import is rejected",
               QStringLiteral("missing media was accepted or changed the error text"));

    // import_media は動画/音声として開けないファイルを拒否する (G104) ので、
    // 取り込み + undo の検証にはリポジトリの実在する動画を使う。
    const QString importSourcePath = QDir::current().absoluteFilePath(
        QStringLiteral("test_assets/e2e_clip.mp4"));
    const bool importSourceReady = QFileInfo::exists(importSourcePath);
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
                {QStringLiteral("filePath"), importSourcePath},
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
    const bool g44 = observeTerm(44, "importSourceReady", static_cast<bool>(importSourceReady))
        && observeTerm(44, "successfulImportResult.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(successfulImportResult.value(QStringLiteral("ok")).toBool(false)))
        && observeTerm(44, "importResponseFields", static_cast<bool>(importResponseFields))
        && observeTerm(44, "importVideoCountAfter == importVideoCountBefore + 1", static_cast<bool>(importVideoCountAfter == importVideoCountBefore + 1))
        && observeTerm(44, "importAudioCountAfter == importAudioCountBefore + 1", static_cast<bool>(importAudioCountAfter == importAudioCountBefore + 1))
        && observeTerm(44, "importUndoRestored", static_cast<bool>(importUndoRestored));
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
        const bool invalidSelectRejected = observeTerm(45, "toolResult(invalidSelect) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidSelect)
            .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(45, "video0->selectedClip() < 0", static_cast<bool>(video0->selectedClip() < 0))
            && observeTerm(45, "audio0->selectedClip() < 0", static_cast<bool>(audio0->selectedClip() < 0));
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
        const bool g46 = observeTerm(46, "selectResult.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(selectResult.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(46, "selectStateSynchronized", static_cast<bool>(selectStateSynchronized));
        g46 ? pass("G46 select_clip synchronizes Timeline and MainWindow")
            : fail("G46 select_clip synchronizes Timeline and MainWindow",
                   QStringLiteral("selection was not synchronized across all state holders"));

        const QJsonObject invalidAfterSelect = callProjectInfoTool(
            49, QStringLiteral("select_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 5}
            });
        const bool g47 = observeTerm(47, "toolResult(invalidAfterSelect) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidAfterSelect)
            .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(47, "video0->isClipSelected(0)", static_cast<bool>(video0->isClipSelected(0))) && observeTerm(47, "audio0->isClipSelected(0)", static_cast<bool>(audio0->isClipSelected(0)))
            && observeTerm(47, "projectInfoWindow.selectedVideoTrackIndex() == 0", static_cast<bool>(projectInfoWindow.selectedVideoTrackIndex() == 0))
            && observeTerm(47, "projectInfoWindow.selectedVideoClipIndexTracked() == 0", static_cast<bool>(projectInfoWindow.selectedVideoClipIndexTracked() == 0));
        g47 ? pass("G47 invalid selection preserves existing selection")
            : fail("G47 invalid selection preserves existing selection",
                   QStringLiteral("existing selection was changed by invalid input"));

        const QJsonObject clearResponse = callProjectInfoTool(
            50, QStringLiteral("clear_selection"), QJsonObject{});
        const bool g48 = observeTerm(48, "toolPayload(clearResponse).value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(clearResponse).value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(48, "video0->selectedClip() < 0", static_cast<bool>(video0->selectedClip() < 0)) && observeTerm(48, "audio0->selectedClip() < 0", static_cast<bool>(audio0->selectedClip() < 0))
            && observeTerm(48, "projectInfoWindow.selectedVideoTrackIndex() == -1", static_cast<bool>(projectInfoWindow.selectedVideoTrackIndex() == -1))
            && observeTerm(48, "projectInfoWindow.selectedVideoClipIndexTracked() == -1", static_cast<bool>(projectInfoWindow.selectedVideoClipIndexTracked() == -1));
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

        // US-406 reserved G159-G160: track appearance is one undo; flags are not.
        {
            const QJsonObject original = projectTimeline->trackFlagsToJson();
            saveTestUndoBaseline();
            int requestId = 15900;
            auto callTrack = [&](const QJsonObject &values) {
                QJsonObject args = values;
                if (!args.contains(QStringLiteral("kind"))) args.insert(QStringLiteral("kind"), QStringLiteral("video"));
                if (!args.contains(QStringLiteral("trackIndex"))) args.insert(QStringLiteral("trackIndex"), 0);
                return callProjectInfoTool(++requestId, QStringLiteral("set_track_property"), args);
            };
            auto readTrack = [&]() {
                const auto tracks = toolPayload(callProjectInfoTool(++requestId, QStringLiteral("get_timeline"), {}))
                    .value(QStringLiteral("video")).toArray();
                return tracks.isEmpty() ? QJsonObject{} : tracks.at(0).toObject();
            };
            const quint64 serial = projectTimeline->undoManager()->saveSerial();
            const auto changed = toolPayload(callTrack(QJsonObject{
                {QStringLiteral("name"), QStringLiteral("MCP 映像")},
                {QStringLiteral("color"), QStringLiteral("#123abc")}}));
            const auto track = readTrack();
            bool g159 = observeTerm(159, "changed.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(changed.value(QStringLiteral("ok")).toBool()))
                && observeTerm(159, "changed.value(QStringLiteral(\"undoRecorded\")).toBool()", static_cast<bool>(changed.value(QStringLiteral("undoRecorded")).toBool()))
                && observeTerm(159, "requiredOutputFieldsPresent(QStringLiteral(\"set_track_property\"), changed)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("set_track_property"), changed)))
                && observeTerm(159, "track.value(QStringLiteral(\"name\")).toString() == QStringLiteral(\"MCP 映像\")", static_cast<bool>(track.value(QStringLiteral("name")).toString() == QStringLiteral("MCP 映像")))
                && observeTerm(159, "track.value(QStringLiteral(\"color\")).toString() == QStringLiteral(\"#123abc\")", static_cast<bool>(track.value(QStringLiteral("color")).toString() == QStringLiteral("#123abc")))
                && observeTerm(159, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1));
            const auto undone = toolPayload(callProjectInfoTool(++requestId, QStringLiteral("undo"), {}));
            g159 = observeTerm(159, "g159", static_cast<bool>(g159)) && observeTerm(159, "undone.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(undone.value(QStringLiteral("ok")).toBool()))
                && observeTerm(159, "projectTimeline->trackFlagsToJson() == original", static_cast<bool>(projectTimeline->trackFlagsToJson() == original));
            g159 ? pass("G159 track name/color readback and single undo")
                 : fail("G159 track name/color readback and single undo", QStringLiteral("appearance/undo mismatch"));
            const quint64 flagSerial = projectTimeline->undoManager()->saveSerial();
            const auto flags = toolPayload(callTrack(QJsonObject{
                {QStringLiteral("muted"), true}, {QStringLiteral("solo"), true}, {QStringLiteral("hidden"), true}}));
            const auto flagTrack = readTrack();
            bool g160 = observeTerm(160, "flags.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(flags.value(QStringLiteral("ok")).toBool()))
                && observeTerm(160, "!flags.value(QStringLiteral(\"undoRecorded\")).toBool()", static_cast<bool>(!flags.value(QStringLiteral("undoRecorded")).toBool()))
                && observeTerm(160, "flagTrack.value(QStringLiteral(\"muted\")).toBool()", static_cast<bool>(flagTrack.value(QStringLiteral("muted")).toBool()))
                && observeTerm(160, "flagTrack.value(QStringLiteral(\"solo\")).toBool()", static_cast<bool>(flagTrack.value(QStringLiteral("solo")).toBool()))
                && observeTerm(160, "flagTrack.value(QStringLiteral(\"hidden\")).toBool()", static_cast<bool>(flagTrack.value(QStringLiteral("hidden")).toBool()))
                && observeTerm(160, "projectTimeline->undoManager()->saveSerial() == flagSerial", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == flagSerial));
            const auto beforeInvalid = projectTimeline->trackFlagsToJson();
            for (const QJsonObject &invalid : {
                QJsonObject{{QStringLiteral("trackIndex"), 99999}},
                QJsonObject{{QStringLiteral("name"), QStringLiteral("must not apply")}, {QStringLiteral("color"), QStringLiteral("red")}},
                QJsonObject{{QStringLiteral("color"), QStringLiteral("#12345z")}},
                QJsonObject{{QStringLiteral("color"), QStringLiteral("#12345678")}},
                QJsonObject{{QStringLiteral("muted"), QStringLiteral("true")}}}) {
                const auto response = callTrack(invalid);
                g160 = observeTerm(160, "g160", static_cast<bool>(g160)) && observeTerm(160, "(response.contains(QStringLiteral(\"error\")) || toolResult(response).value(QStringLiteral(\"isError\")).toBool())", static_cast<bool>((response.contains(QStringLiteral("error"))
                    || toolResult(response).value(QStringLiteral("isError")).toBool())));
            }
            g160 = observeTerm(160, "g160", static_cast<bool>(g160)) && observeTerm(160, "projectTimeline->trackFlagsToJson() == beforeInvalid", static_cast<bool>(projectTimeline->trackFlagsToJson() == beforeInvalid))
                && observeTerm(160, "projectTimeline->undoManager()->saveSerial() == flagSerial", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == flagSerial));
            const auto mixed = toolPayload(callTrack(QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("audio")},
                {QStringLiteral("name"), QStringLiteral("MCP 音声")},
                {QStringLiteral("color"), QStringLiteral("#ABCDEF")},
                {QStringLiteral("muted"), true}, {QStringLiteral("solo"), true}}));
            const auto audioTracks = toolPayload(callProjectInfoTool(++requestId, QStringLiteral("get_timeline"), {}))
                .value(QStringLiteral("audio")).toArray();
            const auto audioTrack = audioTracks.isEmpty() ? QJsonObject{} : audioTracks.at(0).toObject();
            g160 = observeTerm(160, "g160", static_cast<bool>(g160)) && observeTerm(160, "mixed.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(mixed.value(QStringLiteral("ok")).toBool()))
                && observeTerm(160, "mixed.value(QStringLiteral(\"undoRecorded\")).toBool()", static_cast<bool>(mixed.value(QStringLiteral("undoRecorded")).toBool()))
                && observeTerm(160, "audioTrack.value(QStringLiteral(\"name\")).toString() == QStringLiteral(\"MCP 音声\")", static_cast<bool>(audioTrack.value(QStringLiteral("name")).toString() == QStringLiteral("MCP 音声")))
                && observeTerm(160, "audioTrack.value(QStringLiteral(\"color\")).toString() == QStringLiteral(\"#abcdef\")", static_cast<bool>(audioTrack.value(QStringLiteral("color")).toString() == QStringLiteral("#abcdef")));
            const auto mixedUndo = toolPayload(callProjectInfoTool(++requestId, QStringLiteral("undo"), {}));
            g160 = observeTerm(160, "g160", static_cast<bool>(g160)) && observeTerm(160, "mixedUndo.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(mixedUndo.value(QStringLiteral("ok")).toBool()))
                && observeTerm(160, "projectTimeline->trackAt(true, 0)->isMuted()", static_cast<bool>(projectTimeline->trackAt(true, 0)->isMuted()))
                && observeTerm(160, "projectTimeline->trackAt(true, 0)->isSolo()", static_cast<bool>(projectTimeline->trackAt(true, 0)->isSolo()))
                && observeTerm(160, "projectTimeline->trackAt(true, 0)->customName == original.value(QStringLiteral(\"audio\")).toArray().at(0).toObject().value(QStringLiteral(\"name\")).toString()", static_cast<bool>(projectTimeline->trackAt(true, 0)->customName
                    == original.value(QStringLiteral("audio")).toArray().at(0).toObject().value(QStringLiteral("name")).toString()));
            g160 ? pass("G160 track flags and invalid arguments")
                 : fail("G160 track flags and invalid arguments", QStringLiteral("flags/validation mismatch"));
            projectTimeline->applyTrackFlagsFromJson(original);
            saveTestUndoBaseline();
        }

        // US-400 reserved G156-G158: actual MCP writes, reads and single-step undo.
        {
            video0->setClips({makeTestClip(QStringLiteral("mcp-color.mp4"), 0)});
            video1->setClips({});
            audio0->setClips({});
            audio1->setClips({});
            projectTimeline->clearSelection();
            saveTestUndoBaseline();
            auto isToolErrorResponse = [&toolResult](const QJsonObject &response) {
                return toolResult(response).value(QStringLiteral("isError")).toBool(false);
            };
            int requestId = 15600;
            auto callColor = [&](const QJsonObject &values, bool reset = false) {
                return callProjectInfoTool(++requestId, QStringLiteral("set_color_correction"),
                    QJsonObject{{QStringLiteral("clipIndex"), 0},
                        {QStringLiteral("values"), values}, {QStringLiteral("reset"), reset}});
            };
            auto readClip = [&]() {
                const auto payload = toolPayload(callProjectInfoTool(++requestId,
                    QStringLiteral("get_timeline"), QJsonObject{}));
                const auto tracks = payload.value(QStringLiteral("video")).toArray();
                if (tracks.isEmpty()) return QJsonObject{};
                const auto clips = tracks.at(0).toObject().value(QStringLiteral("clips")).toArray();
                return clips.isEmpty() ? QJsonObject{} : clips.at(0).toObject();
            };
            auto undo = [&]() {
                return toolPayload(callProjectInfoTool(++requestId, QStringLiteral("undo"), QJsonObject{}))
                    .value(QStringLiteral("ok")).toBool();
            };
            // UI wheel values outside +/-1 must survive MCP readback and one undo.
            const QJsonObject values{{QStringLiteral("liftR"), 1.5},
                {QStringLiteral("gammaR"), 2.0}, {QStringLiteral("gammaG"), -3.3},
                {QStringLiteral("gainB"), -1.5},
                {QStringLiteral("exposure"), 1.5}};
            const quint64 serial = projectTimeline->undoManager()->saveSerial();
            const auto response = toolPayload(callColor(values));
            const auto readback = readClip();
            const auto correction = readback.value(QStringLiteral("colorCorrection")).toObject();
            bool g156 = observeTerm(156, "response.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(response.value(QStringLiteral("ok")).toBool()))
                && observeTerm(156, "requiredOutputFieldsPresent(QStringLiteral(\"set_color_correction\"), response)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("set_color_correction"), response)))
                && observeTerm(156, "correction.size() == 28", static_cast<bool>(correction.size() == 28)) && observeTerm(156, "!correction.contains(QStringLiteral(\"hueSatWarp\"))", static_cast<bool>(!correction.contains(QStringLiteral("hueSatWarp"))))
                && observeTerm(156, "response.value(QStringLiteral(\"colorCorrection\")).toObject() == correction", static_cast<bool>(response.value(QStringLiteral("colorCorrection")).toObject() == correction))
                && observeTerm(156, "readback.contains(QStringLiteral(\"lut\"))", static_cast<bool>(readback.contains(QStringLiteral("lut"))))
                && observeTerm(156, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1));
            for (const auto &descriptorValue : projectInfoToolDescriptors) {
                const auto descriptor = descriptorValue.toObject();
                if (descriptor.value(QStringLiteral("name")).toString() != QStringLiteral("get_timeline")) continue;
                const auto clipSchema = descriptor.value(QStringLiteral("outputSchema")).toObject()
                    .value(QStringLiteral("properties")).toObject().value(QStringLiteral("video")).toObject()
                    .value(QStringLiteral("items")).toObject().value(QStringLiteral("properties")).toObject()
                    .value(QStringLiteral("clips")).toObject().value(QStringLiteral("items")).toObject();
                const auto properties = clipSchema.value(QStringLiteral("properties")).toObject();
                const auto required = clipSchema.value(QStringLiteral("required")).toArray();
                g156 = observeTerm(156, "g156", static_cast<bool>(g156)) && observeTerm(156, "properties.contains(QStringLiteral(\"colorCorrection\"))", static_cast<bool>(properties.contains(QStringLiteral("colorCorrection"))))
                    && observeTerm(156, "properties.contains(QStringLiteral(\"lut\"))", static_cast<bool>(properties.contains(QStringLiteral("lut"))))
                    && observeTerm(156, "!required.contains(QStringLiteral(\"colorCorrection\"))", static_cast<bool>(!required.contains(QStringLiteral("colorCorrection"))))
                    && observeTerm(156, "!required.contains(QStringLiteral(\"lut\"))", static_cast<bool>(!required.contains(QStringLiteral("lut"))));
            }
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
                g156 = observeTerm(156, "g156", static_cast<bool>(g156)) && observeTerm(156, "correction.value(it.key()) == it.value()", static_cast<bool>(correction.value(it.key()) == it.value()));
            g156 = observeTerm(156, "undo()", static_cast<bool>(undo())) && observeTerm(156, "video0->clips().at(0).colorCorrection.isDefault()", static_cast<bool>(video0->clips().at(0).colorCorrection.isDefault())) && observeTerm(156, "g156", static_cast<bool>(g156));
            // Partial updates retain other fields; reset starts from defaults.
            callColor(values);
            callColor(QJsonObject{{QStringLiteral("brightness"), 12.0}});
            g156 = observeTerm(156, "g156", static_cast<bool>(g156)) && observeTerm(156, "video0->clips().at(0).colorCorrection.exposure == 1.5", static_cast<bool>(video0->clips().at(0).colorCorrection.exposure == 1.5));
            callColor(QJsonObject{{QStringLiteral("shadows"), 10.0}}, true);
            g156 = observeTerm(156, "g156", static_cast<bool>(g156)) && observeTerm(156, "video0->clips().at(0).colorCorrection.exposure == 0.0", static_cast<bool>(video0->clips().at(0).colorCorrection.exposure == 0.0))
                && observeTerm(156, "video0->clips().at(0).colorCorrection.brightness == 0.0", static_cast<bool>(video0->clips().at(0).colorCorrection.brightness == 0.0))
                && observeTerm(156, "video0->clips().at(0).colorCorrection.shadows == 10.0", static_cast<bool>(video0->clips().at(0).colorCorrection.shadows == 10.0));
            g156 = observeTerm(156, "undo()", static_cast<bool>(undo())) && observeTerm(156, "video0->clips().at(0).colorCorrection.brightness == 12.0", static_cast<bool>(video0->clips().at(0).colorCorrection.brightness == 12.0)) && observeTerm(156, "g156", static_cast<bool>(g156));
            g156 ? pass("G156 color correction readback and single undo")
                 : fail("G156 color correction readback and single undo", QStringLiteral("color/undo mismatch"));

            const auto beforeInvalid = readClip();
            const quint64 invalidSerial = projectTimeline->undoManager()->saveSerial();
            bool g157 = true;
            const QVector<QJsonObject> invalidValues{
                {{QStringLiteral("exposure"), 3.01}}, {{QStringLiteral("gamma"), 0.09}},
                {{QStringLiteral("brightness"), 101}}, {{QStringLiteral("hue"), -181}},
                {{QStringLiteral("liftR"), 2.01}}, {{QStringLiteral("gammaG"), -6.65}},
                {{QStringLiteral("gammaR"), 4.01}},
                {{QStringLiteral("logHighB"), 1.01}}, {{QStringLiteral("unknown"), 1}},
                {{QStringLiteral("contrast"), QStringLiteral("bad")}},
                {{QStringLiteral("brightness"), 15}, {QStringLiteral("unknown"), 1}}
            };
            for (const auto &invalid : invalidValues)
                g157 = observeTerm(157, "isToolErrorResponse(callColor(invalid))", static_cast<bool>(isToolErrorResponse(callColor(invalid)))) && observeTerm(157, "g157", static_cast<bool>(g157));
            g157 = observeTerm(157, "g157", static_cast<bool>(g157)) && observeTerm(157, "readClip() == beforeInvalid", static_cast<bool>(readClip() == beforeInvalid))
                && observeTerm(157, "projectTimeline->undoManager()->saveSerial() == invalidSerial", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == invalidSerial));
            const auto warned = toolPayload(callColor(QJsonObject{
                {QStringLiteral("hueSatWarp"), QJsonObject{{QStringLiteral("enabled"), true}}},
                {QStringLiteral("tint"), 25.0}}));
            const auto warnedColor = readClip().value(QStringLiteral("colorCorrection")).toObject();
            g157 = observeTerm(157, "g157", static_cast<bool>(g157)) && observeTerm(157, "warned.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(warned.value(QStringLiteral("ok")).toBool()))
                && observeTerm(157, "warned.value(QStringLiteral(\"warning\")).toString().contains(QStringLiteral(\"hueSatWarp\"))", static_cast<bool>(warned.value(QStringLiteral("warning")).toString().contains(QStringLiteral("hueSatWarp"))))
                && observeTerm(157, "warnedColor.value(QStringLiteral(\"tint\")).toDouble() == 25.0", static_cast<bool>(warnedColor.value(QStringLiteral("tint")).toDouble() == 25.0))
                && observeTerm(157, "!warnedColor.contains(QStringLiteral(\"hueSatWarp\"))", static_cast<bool>(!warnedColor.contains(QStringLiteral("hueSatWarp"))))
                && observeTerm(157, "video0->clips().at(0).colorCorrection.hueSatWarp.isDefault()", static_cast<bool>(video0->clips().at(0).colorCorrection.hueSatWarp.isDefault()));
            g157 = observeTerm(157, "undo()", static_cast<bool>(undo())) && observeTerm(157, "readClip() == beforeInvalid", static_cast<bool>(readClip() == beforeInvalid)) && observeTerm(157, "g157", static_cast<bool>(g157));
            g157 ? pass("G157 invalid color fields and hueSatWarp warning")
                 : fail("G157 invalid color fields and hueSatWarp warning", QStringLiteral("validation was not atomic"));

            QTemporaryDir lutDirectory;
            const QString lutPath = QDir(lutDirectory.path()).filePath(QStringLiteral("identity.cube"));
            QFile lutFile(lutPath);
            const QByteArray cube("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
            bool g158 = observeTerm(158, "lutDirectory.isValid()", static_cast<bool>(lutDirectory.isValid())) && observeTerm(158, "lutFile.open(QIODevice::WriteOnly)", static_cast<bool>(lutFile.open(QIODevice::WriteOnly)));
            if (g158) g158 = lutFile.write(cube) == cube.size();
            lutFile.close();
            auto applyLut = [&](const QString &path, double intensity) {
                return callProjectInfoTool(++requestId, QStringLiteral("apply_lut"), QJsonObject{
                    {QStringLiteral("clipIndex"), 0}, {QStringLiteral("filePath"), path},
                    {QStringLiteral("intensity"), intensity}});
            };
            saveTestUndoBaseline();
            const quint64 lutSerial = projectTimeline->undoManager()->saveSerial();
            const auto applied = toolPayload(applyLut(lutPath, 0.65));
            const QJsonObject expectedLut{{QStringLiteral("filePath"), lutPath},
                                         {QStringLiteral("intensity"), 0.65}};
            g158 = observeTerm(158, "g158", static_cast<bool>(g158)) && observeTerm(158, "applied.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(applied.value(QStringLiteral("ok")).toBool()))
                && observeTerm(158, "requiredOutputFieldsPresent(QStringLiteral(\"apply_lut\"), applied)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("apply_lut"), applied)))
                && observeTerm(158, "applied.value(QStringLiteral(\"lut\")).toObject() == expectedLut", static_cast<bool>(applied.value(QStringLiteral("lut")).toObject() == expectedLut))
                && observeTerm(158, "readClip().value(QStringLiteral(\"lut\")).toObject() == expectedLut", static_cast<bool>(readClip().value(QStringLiteral("lut")).toObject() == expectedLut))
                && observeTerm(158, "projectTimeline->undoManager()->saveSerial() == lutSerial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == lutSerial + 1));
            // Repeated UI writes retain the existing no-change guard.
            g158 = observeTerm(158, "projectTimeline->setClipLut(0, 0, lutPath, 0.65)", static_cast<bool>(projectTimeline->setClipLut(0, 0, lutPath, 0.65))) && observeTerm(158, "g158", static_cast<bool>(g158));
            g158 = observeTerm(158, "g158", static_cast<bool>(g158)) && observeTerm(158, "projectTimeline->undoManager()->saveSerial() == lutSerial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == lutSerial + 1));
            const auto missingLut = applyLut(lutPath + QStringLiteral(".missing"), 1.0);
            g158 = observeTerm(158, "isToolErrorResponse(missingLut)", static_cast<bool>(isToolErrorResponse(missingLut)))
                && observeTerm(158, "toolErrorText(missingLut).contains(QStringLiteral(\"LUT ファイルが見つかりません: \"))", static_cast<bool>(toolErrorText(missingLut).contains(QStringLiteral("LUT ファイルが見つかりません: ")))) && observeTerm(158, "g158", static_cast<bool>(g158));
            g158 = observeTerm(158, "isToolErrorResponse(applyLut(lutPath, 1.01))", static_cast<bool>(isToolErrorResponse(applyLut(lutPath, 1.01)))) && observeTerm(158, "g158", static_cast<bool>(g158));
            g158 = observeTerm(158, "g158", static_cast<bool>(g158)) && observeTerm(158, "projectTimeline->undoManager()->saveSerial() == lutSerial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == lutSerial + 1));
            const auto cleared = toolPayload(applyLut(QString(), 0.25));
            const auto clearedLut = readClip().value(QStringLiteral("lut")).toObject();
            g158 = observeTerm(158, "g158", static_cast<bool>(g158)) && observeTerm(158, "cleared.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(cleared.value(QStringLiteral("ok")).toBool()))
                && observeTerm(158, "clearedLut.value(QStringLiteral(\"filePath\")).toString().isEmpty()", static_cast<bool>(clearedLut.value(QStringLiteral("filePath")).toString().isEmpty()))
                && observeTerm(158, "clearedLut.value(QStringLiteral(\"intensity\")).toDouble() == 1.0", static_cast<bool>(clearedLut.value(QStringLiteral("intensity")).toDouble() == 1.0))
                && observeTerm(158, "projectTimeline->undoManager()->saveSerial() == lutSerial + 2", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == lutSerial + 2));
            g158 = observeTerm(158, "undo()", static_cast<bool>(undo())) && observeTerm(158, "readClip().value(QStringLiteral(\"lut\")).toObject() == expectedLut", static_cast<bool>(readClip().value(QStringLiteral("lut")).toObject() == expectedLut)) && observeTerm(158, "g158", static_cast<bool>(g158));
            g158 = observeTerm(158, "undo()", static_cast<bool>(undo())) && observeTerm(158, "video0->clips().at(0).lutFilePath.isEmpty()", static_cast<bool>(video0->clips().at(0).lutFilePath.isEmpty())) && observeTerm(158, "g158", static_cast<bool>(g158));
            const auto defaultLut = toolPayload(callProjectInfoTool(++requestId, QStringLiteral("apply_lut"),
                QJsonObject{{QStringLiteral("clipIndex"), 0}, {QStringLiteral("filePath"), lutPath}}));
            g158 = observeTerm(158, "g158", static_cast<bool>(g158)) && observeTerm(158, "defaultLut.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(defaultLut.value(QStringLiteral("ok")).toBool()))
                && observeTerm(158, "defaultLut.value(QStringLiteral(\"lut\")).toObject().value(QStringLiteral(\"intensity\")).toDouble() == 1.0", static_cast<bool>(defaultLut.value(QStringLiteral("lut")).toObject().value(QStringLiteral("intensity")).toDouble() == 1.0));
            g158 = observeTerm(158, "undo()", static_cast<bool>(undo())) && observeTerm(158, "video0->clips().at(0).lutFilePath.isEmpty()", static_cast<bool>(video0->clips().at(0).lutFilePath.isEmpty())) && observeTerm(158, "g158", static_cast<bool>(g158));
            g158 ? pass("G158 LUT apply clear and single-step undo")
                 : fail("G158 LUT apply clear and single-step undo", QStringLiteral("LUT/undo mismatch"));
        }

        // US-308 reserved G154-G155: compare the same saved/current pair as
        // the UI and prove comparison does not add undo states or dirty it.
        {
            QTemporaryDir diffDirectory;
            const QString diffPath = QDir(diffDirectory.path()).filePath(QStringLiteral("baseline.veditor"));
            video0->setClips({makeTestClip(QStringLiteral("project-diff-source.mp4"), 0)});
            video1->setClips({});
            audio0->setClips({});
            audio1->setClips({});
            projectTimeline->clearSelection();
            saveTestUndoBaseline();
            const auto saved = callProjectInfoTool(1540, QStringLiteral("save_project"),
                QJsonObject{{QStringLiteral("path"), diffPath}});
            const auto moved = callProjectInfoTool(1541, QStringLiteral("move_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}, {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0}, {QStringLiteral("newStartSec"), 2.0}});
            const auto volume = callProjectInfoTool(1542, QStringLiteral("set_clip_property"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}, {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0}, {QStringLiteral("property"), QStringLiteral("volume")},
                {QStringLiteral("value"), 0.5}});
            const quint64 serial = projectTimeline->undoManager()->saveSerial();
            const double playhead = projectTimeline->playheadPosition();
            const bool dirty = projectInfoWindow.isWindowModified();
            const auto comparison = callProjectInfoTool(1543, QStringLiteral("compare_project"),
                QJsonObject{{QStringLiteral("filePath"), diffPath}});
            const auto payload = toolPayload(comparison);
            int movedCount = 0, volumeCount = 0;
            for (const auto &value : payload.value(QStringLiteral("changes")).toArray()) {
                const auto change = value.toObject();
                if (change.value(QStringLiteral("type")).toString() == QStringLiteral("Moved")) ++movedCount;
                if (change.value(QStringLiteral("type")).toString() == QStringLiteral("PropertyChanged")
                    && change.value(QStringLiteral("path")).toString() == QStringLiteral("video[0].clips[0].volume")
                    && change.value(QStringLiteral("before")).toString() == QStringLiteral("1")
                    && change.value(QStringLiteral("after")).toString() == QStringLiteral("0.5")) ++volumeCount;
            }
            const auto summary = payload.value(QStringLiteral("summary")).toObject();
            const bool g154 = observeTerm(154, "diffDirectory.isValid()", static_cast<bool>(diffDirectory.isValid()))
                && observeTerm(154, "toolPayload(saved).value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(toolPayload(saved).value(QStringLiteral("ok")).toBool()))
                && observeTerm(154, "toolPayload(moved).value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(toolPayload(moved).value(QStringLiteral("ok")).toBool()))
                && observeTerm(154, "toolPayload(volume).value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(toolPayload(volume).value(QStringLiteral("ok")).toBool()))
                && observeTerm(154, "payload.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(payload.value(QStringLiteral("ok")).toBool()))
                && observeTerm(154, "requiredOutputFieldsPresent(QStringLiteral(\"compare_project\"), payload)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("compare_project"), payload)))
                && observeTerm(154, "payload.value(QStringLiteral(\"changes\")).toArray().size() == 2", static_cast<bool>(payload.value(QStringLiteral("changes")).toArray().size() == 2))
                && observeTerm(154, "movedCount == 1", static_cast<bool>(movedCount == 1)) && observeTerm(154, "volumeCount == 1", static_cast<bool>(volumeCount == 1))
                && observeTerm(154, "summary.value(QStringLiteral(\"moved\")).toInt() == 1", static_cast<bool>(summary.value(QStringLiteral("moved")).toInt() == 1))
                && observeTerm(154, "summary.value(QStringLiteral(\"changed\")).toInt() == 1", static_cast<bool>(summary.value(QStringLiteral("changed")).toInt() == 1))
                && observeTerm(154, "summary.value(QStringLiteral(\"added\")).toInt() == 0", static_cast<bool>(summary.value(QStringLiteral("added")).toInt() == 0))
                && observeTerm(154, "summary.value(QStringLiteral(\"removed\")).toInt() == 0", static_cast<bool>(summary.value(QStringLiteral("removed")).toInt() == 0))
                && observeTerm(154, "summary.value(QStringLiteral(\"trimmed\")).toInt() == 0", static_cast<bool>(summary.value(QStringLiteral("trimmed")).toInt() == 0))
                && observeTerm(154, "projectTimeline->undoManager()->saveSerial() == serial", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial))
                && observeTerm(154, "projectTimeline->playheadPosition() == playhead", static_cast<bool>(projectTimeline->playheadPosition() == playhead))
                && observeTerm(154, "projectInfoWindow.isWindowModified() == dirty", static_cast<bool>(projectInfoWindow.isWindowModified() == dirty));
            g154 ? pass("G154 compare_project detects move and volume")
                 : fail("G154 compare_project detects move and volume", QStringLiteral("wrong diff, summary, or read-only behavior"));

            const auto savedAgain = callProjectInfoTool(1550, QStringLiteral("save_project"),
                QJsonObject{{QStringLiteral("path"), diffPath}});
            const auto identical = toolPayload(callProjectInfoTool(1551, QStringLiteral("compare_project"),
                QJsonObject{{QStringLiteral("filePath"), diffPath}}));
            const auto missing = callProjectInfoTool(1552, QStringLiteral("compare_project"),
                QJsonObject{{QStringLiteral("filePath"), diffPath + QStringLiteral(".missing")}});
            const auto invalid = callProjectInfoTool(1553, QStringLiteral("compare_project"),
                QJsonObject{{QStringLiteral("filePath"), 42}});
            const bool g155 = observeTerm(155, "toolPayload(savedAgain).value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(toolPayload(savedAgain).value(QStringLiteral("ok")).toBool()))
                && observeTerm(155, "identical.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(identical.value(QStringLiteral("ok")).toBool()))
                && observeTerm(155, "identical.value(QStringLiteral(\"changes\")).isArray()", static_cast<bool>(identical.value(QStringLiteral("changes")).isArray()))
                && observeTerm(155, "identical.value(QStringLiteral(\"changes\")).toArray().isEmpty()", static_cast<bool>(identical.value(QStringLiteral("changes")).toArray().isEmpty()))
                && observeTerm(155, "identical.value(QStringLiteral(\"summary\")).toObject() == QJsonObject{ {\"added\", 0}, {\"removed\", 0}, {\"moved\", 0}, {\"trimmed\", 0}, {\"changed\", 0}}", static_cast<bool>(identical.value(QStringLiteral("summary")).toObject() == QJsonObject{
                    {"added", 0}, {"removed", 0}, {"moved", 0}, {"trimmed", 0}, {"changed", 0}}))
                && observeTerm(155, "toolResult(missing).value(QStringLiteral(\"isError\")).toBool()", static_cast<bool>(toolResult(missing).value(QStringLiteral("isError")).toBool()))
                && observeTerm(155, "toolResult(invalid).value(QStringLiteral(\"isError\")).toBool()", static_cast<bool>(toolResult(invalid).value(QStringLiteral("isError")).toBool()))
                && observeTerm(155, "projectTimeline->undoManager()->saveSerial() == serial", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial))
                && observeTerm(155, "!projectInfoWindow.isWindowModified()", static_cast<bool>(!projectInfoWindow.isWindowModified()));
            g155 ? pass("G155 compare_project unchanged and invalid inputs")
                 : fail("G155 compare_project unchanged and invalid inputs", QStringLiteral("nonempty diff or unsafe failure"));
            // Later save_project tests use this pre-existing temporary path.
            projectInfoWindow.m_projectFilePath = roundtripProjectPath;
        }

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
        const bool g49 = observeTerm(49, "reorderSucceeded", static_cast<bool>(reorderSucceeded)) && observeTerm(49, "reorderUndoSucceeded", static_cast<bool>(reorderUndoSucceeded));
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
        const bool g50 = observeTerm(50, "!toolResult(blockedResponse).value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(!toolResult(blockedResponse).value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(50, "!blockedResult.value(QStringLiteral(\"ok\")).toBool(true)", static_cast<bool>(!blockedResult.value(QStringLiteral("ok")).toBool(true)))
            && observeTerm(50, "std::isfinite(actualBlockedStart)", static_cast<bool>(std::isfinite(actualBlockedStart))) && observeTerm(50, "actualBlockedStart >= 0.0", static_cast<bool>(actualBlockedStart >= 0.0))
            && observeTerm(50, "!blockedResult.value(QStringLiteral(\"reason\")).toString().isEmpty()", static_cast<bool>(!blockedResult.value(QStringLiteral("reason")).toString().isEmpty()))
            && observeTerm(50, "video0->clips().at(0).displayName == QStringLiteral(\"blocked-A\")", static_cast<bool>(video0->clips().at(0).displayName == QStringLiteral("blocked-A")))
            && observeTerm(50, "video0->clips().at(1).displayName == QStringLiteral(\"blocked-B\")", static_cast<bool>(video0->clips().at(1).displayName == QStringLiteral("blocked-B")));
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
        const bool g51 = observeTerm(51, "crossTrackResult.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(crossTrackResult.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(51, "crossTrackResult.value(QStringLiteral(\"trackIndex\")).toInt(-1) == 1", static_cast<bool>(crossTrackResult.value(QStringLiteral("trackIndex")).toInt(-1) == 1))
            && observeTerm(51, "video0->clipCount() == 0", static_cast<bool>(video0->clipCount() == 0)) && observeTerm(51, "video1->clipCount() == 1", static_cast<bool>(video1->clipCount() == 1))
            && observeTerm(51, "video1->clips().first().displayName == QStringLiteral(\"cross-track\")", static_cast<bool>(video1->clips().first().displayName == QStringLiteral("cross-track")));
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
        const bool g52 = observeTerm(52, "linkedMoveResult.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(linkedMoveResult.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(52, "video0->clipCount() == 0", static_cast<bool>(video0->clipCount() == 0)) && observeTerm(52, "video1->clipCount() == 1", static_cast<bool>(video1->clipCount() == 1))
            && observeTerm(52, "audio0->clipCount() == 0", static_cast<bool>(audio0->clipCount() == 0)) && observeTerm(52, "audio1->clipCount() == 1", static_cast<bool>(audio1->clipCount() == 1))
            && observeTerm(52, "video1->clips().first().linkGroup == 778", static_cast<bool>(video1->clips().first().linkGroup == 778))
            && observeTerm(52, "audio1->clips().first().linkGroup == 778", static_cast<bool>(audio1->clips().first().linkGroup == 778));
        g52 ? pass("G52 linked V/A clips move together")
            : fail("G52 linked V/A clips move together",
                   QStringLiteral("linked audio/video clips diverged during track move"));

        auto timelineTrackObject = [](const QJsonObject &payload,
                                      const QString &kind, int trackIndex) {
            const QJsonArray tracks = payload.value(kind).toArray();
            return trackIndex >= 0 && trackIndex < tracks.size()
                ? tracks.at(trackIndex).toObject() : QJsonObject{};
        };

        const QJsonObject lockResponse = callProjectInfoTool(
            201, QStringLiteral("set_track_locked"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("locked"), true}
            });
        const QJsonObject lockedTimelineResponse = callProjectInfoTool(
            202, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}
            });
        const QJsonObject lockPayload = toolPayload(lockResponse);
        const QJsonObject lockedTimelinePayload = toolPayload(lockedTimelineResponse);
        const bool g114 = observeTerm(114, "!toolResult(lockResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(lockResponse)
                                .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(114, "lockPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(lockPayload.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(114, "lockPayload.value(QStringLiteral(\"locked\")).toBool(false)", static_cast<bool>(lockPayload.value(QStringLiteral("locked")).toBool(false)))
            && observeTerm(114, "requiredOutputFieldsPresent(QStringLiteral(\"set_track_locked\"), lockPayload)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("set_track_locked"),
                                           lockPayload)))
            && observeTerm(114, "video0->isLocked()", static_cast<bool>(video0->isLocked()))
            && observeTerm(114, "timelineTrackObject(lockedTimelinePayload, QStringLiteral(\"video\"), 0) .value(QStringLiteral(\"locked\")).toBool(false)", static_cast<bool>(timelineTrackObject(lockedTimelinePayload,
                                   QStringLiteral("video"), 0)
                   .value(QStringLiteral("locked")).toBool(false)));
        g114 ? pass("G114 set_track_locked is reflected by get_timeline")
             : fail("G114 set_track_locked is reflected by get_timeline",
                    QStringLiteral("V1 was not locked or get_timeline omitted locked=true"));

        const QJsonObject unlockResponse = callProjectInfoTool(
            203, QStringLiteral("set_track_locked"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("locked"), false}
            });
        const QJsonObject unlockedTimelineResponse = callProjectInfoTool(
            204, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}
            });
        const bool g115 = observeTerm(115, "toolPayload(unlockResponse) .value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(unlockResponse)
                                .value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(115, "!video0->isLocked()", static_cast<bool>(!video0->isLocked()))
            && observeTerm(115, "!timelineTrackObject(toolPayload(unlockedTimelineResponse), QStringLiteral(\"video\"), 0) .value(QStringLiteral(\"locked\")).toBool(true)", static_cast<bool>(!timelineTrackObject(toolPayload(unlockedTimelineResponse),
                                    QStringLiteral("video"), 0)
                    .value(QStringLiteral("locked")).toBool(true)));
        g115 ? pass("G115 set_track_locked unlocks the track")
             : fail("G115 set_track_locked unlocks the track",
                    QStringLiteral("V1 remained locked after locked=false"));

        const QJsonObject missingTrackLockResponse = callProjectInfoTool(
            205, QStringLiteral("set_track_locked"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), projectTimeline->videoTrackCount() + 10},
                {QStringLiteral("locked"), true}
            });
        const bool g116 = observeTerm(116, "toolResult(missingTrackLockResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(missingTrackLockResponse)
                                .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(116, "toolErrorText(missingTrackLockResponse) .contains(QStringLiteral(\"track index is out of range\"))", static_cast<bool>(toolErrorText(missingTrackLockResponse)
                   .contains(QStringLiteral("track index is out of range"))));
        g116 ? pass("G116 set_track_locked rejects a missing track")
             : fail("G116 set_track_locked rejects a missing track",
                    QStringLiteral("an out-of-range track was accepted"));

        video0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("locked-guard"), 0)
        });
        video1->setClips(QVector<ClipInfo>{});
        audio0->setClips(QVector<ClipInfo>{});
        audio1->setClips(QVector<ClipInfo>{});
        projectTimeline->clearSelection();
        saveTestUndoBaseline();
        callProjectInfoTool(206, QStringLiteral("set_track_locked"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("video")},
            {QStringLiteral("trackIndex"), 0},
            {QStringLiteral("locked"), true}
        });
        const QJsonObject lockedSplitResponse = callProjectInfoTool(
            207, QStringLiteral("split_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("timeSec"), 1.0}
            });
        const QJsonObject lockedDeleteResponse = callProjectInfoTool(
            208, QStringLiteral("delete_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0}
            });
        const QJsonObject lockedMoveResponse = callProjectInfoTool(
            209, QStringLiteral("move_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("newStartSec"), 1.0}
            });
        const auto lockedEditRejected = [&](const QJsonObject &response) {
            return toolResult(response).value(QStringLiteral("isError")).toBool(false)
                && toolErrorText(response) == QStringLiteral("track is locked");
        };
        const bool g117 = observeTerm(117, "lockedEditRejected(lockedSplitResponse)", static_cast<bool>(lockedEditRejected(lockedSplitResponse)))
            && observeTerm(117, "lockedEditRejected(lockedDeleteResponse)", static_cast<bool>(lockedEditRejected(lockedDeleteResponse)))
            && observeTerm(117, "lockedEditRejected(lockedMoveResponse)", static_cast<bool>(lockedEditRejected(lockedMoveResponse)))
            && observeTerm(117, "video0->clipCount() == 1", static_cast<bool>(video0->clipCount() == 1))
            && observeTerm(117, "video0->clips().first().displayName == QStringLiteral(\"locked-guard\")", static_cast<bool>(video0->clips().first().displayName == QStringLiteral("locked-guard")))
            && observeTerm(117, "!projectTimeline->undoManager()->canUndo()", static_cast<bool>(!projectTimeline->undoManager()->canUndo()));
        g117 ? pass("G117 locked tracks reject split/delete/move")
             : fail("G117 locked tracks reject split/delete/move",
                    QStringLiteral("a locked edit mutated V1 or added undo state"));
        callProjectInfoTool(210, QStringLiteral("set_track_locked"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("video")},
            {QStringLiteral("trackIndex"), 0},
            {QStringLiteral("locked"), false}
        });

        QJsonArray videoFlags;
        QJsonArray audioFlags;
        for (int index = 0; index < projectTimeline->videoTrackCount(); ++index) {
            videoFlags.append(index == 0
                ? QJsonObject{{QStringLiteral("locked"), true},
                              {QStringLiteral("hidden"), true}}
                : QJsonObject{});
        }
        for (int index = 0; index < projectTimeline->audioTrackCount(); ++index) {
            audioFlags.append(index == 0
                ? QJsonObject{{QStringLiteral("muted"), true},
                              {QStringLiteral("solo"), true}}
                : QJsonObject{});
        }
        projectTimeline->applyTrackFlagsFromJson(QJsonObject{
            {QStringLiteral("video"), videoFlags},
            {QStringLiteral("audio"), audioFlags}
        });

        const QString trackFlagsProjectPath = projectBehaviorDirReady
            ? QDir(projectBehaviorDir.path()).filePath(
                  QStringLiteral("track-flags-roundtrip.vsep"))
            : QString();
        const QString legacyTrackFlagsProjectPath = projectBehaviorDirReady
            ? QDir(projectBehaviorDir.path()).filePath(
                  QStringLiteral("track-flags-legacy.vsep"))
            : QString();
        QJsonObject trackFlagsSaveResponse;
        QJsonObject trackFlagsOpenResponse;
        QJsonDocument savedTrackFlagsDocument;
        bool savedFileRead = false;
        if (projectBehaviorDirReady) {
            trackFlagsSaveResponse = callProjectInfoTool(
                211, QStringLiteral("save_project"), QJsonObject{
                    {QStringLiteral("path"), trackFlagsProjectPath}
                });
            QFile savedFile(trackFlagsProjectPath);
            savedFileRead = savedFile.open(QIODevice::ReadOnly);
            if (savedFileRead)
                savedTrackFlagsDocument = QJsonDocument::fromJson(savedFile.readAll());
            projectTimeline->applyTrackFlagsFromJson(QJsonObject{});
            trackFlagsOpenResponse = callProjectInfoTool(
                212, QStringLiteral("open_project"), QJsonObject{
                    {QStringLiteral("path"), trackFlagsProjectPath}
                });
        }
        const QJsonObject reopenedTimelineResponse = callProjectInfoTool(
            213, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("all")}
            });
        const QJsonObject reopenedTimelinePayload = toolPayload(reopenedTimelineResponse);
        const QJsonObject restoredFlags = projectTimeline->trackFlagsToJson();
        const QJsonArray restoredVideoFlags = restoredFlags
            .value(QStringLiteral("video")).toArray();
        const QJsonArray restoredAudioFlags = restoredFlags
            .value(QStringLiteral("audio")).toArray();
        const QJsonObject restoredVideo0 = restoredVideoFlags.isEmpty()
            ? QJsonObject{} : restoredVideoFlags.first().toObject();
        const QJsonObject restoredAudio0 = restoredAudioFlags.isEmpty()
            ? QJsonObject{} : restoredAudioFlags.first().toObject();
        const QJsonObject savedRoot = savedTrackFlagsDocument.object();
        const QJsonObject savedFlags = savedRoot
            .value(QStringLiteral("trackFlags")).toObject();
        const bool flagsRoundtripped = projectBehaviorDirReady && savedFileRead
            && !toolResult(trackFlagsSaveResponse)
                    .value(QStringLiteral("isError")).toBool(true)
            && !toolResult(trackFlagsOpenResponse)
                    .value(QStringLiteral("isError")).toBool(true)
            && timelineTrackObject(reopenedTimelinePayload,
                                   QStringLiteral("video"), 0)
                   .value(QStringLiteral("locked")).toBool(false)
            && restoredVideoFlags.size() == projectTimeline->videoTrackCount()
            && restoredAudioFlags.size() == projectTimeline->audioTrackCount()
            && restoredVideo0.value(QStringLiteral("locked")).toBool(false)
            && restoredVideo0.value(QStringLiteral("hidden")).toBool(false)
            && restoredAudio0.value(QStringLiteral("muted")).toBool(false)
            && restoredAudio0.value(QStringLiteral("solo")).toBool(false)
            && savedFlags.value(QStringLiteral("video")).toArray().size()
                   == projectTimeline->videoTrackCount()
            && savedFlags.value(QStringLiteral("audio")).toArray().size()
                   == projectTimeline->audioTrackCount();

        const auto anyCheckedButton = [&projectInfoWindow](const QString &objectName,
                                                           const QString &text) {
            const QList<QPushButton *> buttons =
                projectInfoWindow.findChildren<QPushButton *>(objectName);
            for (const QPushButton *button : buttons) {
                if (button && button->isChecked() && button->text() == text)
                    return true;
            }
            return false;
        };
        const bool headersSynchronized =
            anyCheckedButton(QStringLiteral("timelineTrackLockButton"),
                             QString::fromUtf8("\xF0\x9F\x94\x92"))
            && anyCheckedButton(QStringLiteral("timelineTrackMuteButton"),
                                QString::fromUtf8("\xF0\x9F\x94\x87"))
            && anyCheckedButton(QStringLiteral("timelineTrackSoloButton"),
                                QStringLiteral("S"))
            && anyCheckedButton(QStringLiteral("timelineTrackHideButton"),
                                QString::fromUtf8("\xE2\x8A\x98"));

        bool legacyFileWritten = false;
        if (savedFileRead && !savedTrackFlagsDocument.isNull()) {
            QJsonObject legacyRoot = savedTrackFlagsDocument.object();
            legacyRoot.remove(QStringLiteral("trackFlags"));
            QFile legacyFile(legacyTrackFlagsProjectPath);
            if (legacyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                const QByteArray legacyJson = QJsonDocument(legacyRoot).toJson(
                    QJsonDocument::Indented);
                legacyFileWritten = legacyFile.write(legacyJson) == legacyJson.size();
            }
        }
        QJsonObject legacyOpenResponse;
        if (legacyFileWritten) {
            legacyOpenResponse = callProjectInfoTool(
                214, QStringLiteral("open_project"), QJsonObject{
                    {QStringLiteral("path"), legacyTrackFlagsProjectPath}
                });
        }
        const QJsonObject legacyFlags = projectTimeline->trackFlagsToJson();
        auto allFlagsFalse = [](const QJsonArray &tracks) {
            for (const QJsonValue &value : tracks) {
                const QJsonObject flags = value.toObject();
                if (flags.value(QStringLiteral("locked")).toBool(false)
                    || flags.value(QStringLiteral("muted")).toBool(false)
                    || flags.value(QStringLiteral("solo")).toBool(false)
                    || flags.value(QStringLiteral("hidden")).toBool(false)) {
                    return false;
                }
            }
            return true;
        };
        const bool legacyDefaults = legacyFileWritten
            && !toolResult(legacyOpenResponse)
                    .value(QStringLiteral("isError")).toBool(true)
            && allFlagsFalse(legacyFlags.value(QStringLiteral("video")).toArray())
            && allFlagsFalse(legacyFlags.value(QStringLiteral("audio")).toArray());
        const bool g118 = observeTerm(118, "flagsRoundtripped", static_cast<bool>(flagsRoundtripped)) && observeTerm(118, "headersSynchronized", static_cast<bool>(headersSynchronized)) && observeTerm(118, "legacyDefaults", static_cast<bool>(legacyDefaults));
        g118 ? pass("G118 track flags survive save/open and legacy defaults are false")
             : fail("G118 track flags survive save/open and legacy defaults are false",
                    QStringLiteral("flags, header state, array lengths, or legacy defaults diverged"));

        video0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("label-target"), 0),
            makeTestClip(QStringLiteral("label-default"), 0)
        });
        video1->setClips(QVector<ClipInfo>{});
        audio0->setClips(QVector<ClipInfo>{});
        audio1->setClips(QVector<ClipInfo>{});
        projectTimeline->applyTrackFlagsFromJson(QJsonObject{});
        projectTimeline->clearSelection();
        saveTestUndoBaseline();

        const QJsonObject setLabelResponse = callProjectInfoTool(
            215, QStringLiteral("set_clip_label"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("label"), QStringLiteral("red")}
            });
        const QJsonObject labelledTimelineResponse = callProjectInfoTool(
            216, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}
            });
        const QJsonArray labelledTimelineClips = timelineTrackObject(
            toolPayload(labelledTimelineResponse), QStringLiteral("video"), 0)
                .value(QStringLiteral("clips")).toArray();
        const QJsonObject setLabelPayload = toolPayload(setLabelResponse);
        const bool g119 = observeTerm(119, "!toolResult(setLabelResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(setLabelResponse)
                                .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(119, "setLabelPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(setLabelPayload.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(119, "setLabelPayload.value(QStringLiteral(\"label\")).toString() == QStringLiteral(\"red\")", static_cast<bool>(setLabelPayload.value(QStringLiteral("label")).toString()
                   == QStringLiteral("red")))
            && observeTerm(119, "requiredOutputFieldsPresent(QStringLiteral(\"set_clip_label\"), setLabelPayload)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("set_clip_label"),
                                           setLabelPayload)))
            && observeTerm(119, "video0->clips().at(0).label == ClipLabel::Red", static_cast<bool>(video0->clips().at(0).label == ClipLabel::Red))
            && observeTerm(119, "labelledTimelineClips.size() == 2", static_cast<bool>(labelledTimelineClips.size() == 2))
            && observeTerm(119, "labelledTimelineClips.at(0).toObject() .value(QStringLiteral(\"label\")).toString() == QStringLiteral(\"red\")", static_cast<bool>(labelledTimelineClips.at(0).toObject()
                   .value(QStringLiteral("label")).toString() == QStringLiteral("red")))
            && observeTerm(119, "labelledTimelineClips.at(1).toObject() .value(QStringLiteral(\"label\")).toString() == QStringLiteral(\"none\")", static_cast<bool>(labelledTimelineClips.at(1).toObject()
                   .value(QStringLiteral("label")).toString() == QStringLiteral("none")));
        g119 ? pass("G119 set_clip_label is reflected by get_timeline")
             : fail("G119 set_clip_label is reflected by get_timeline",
                    QStringLiteral("label mutation, response schema, or get_timeline.label diverged"));

        const QJsonObject invalidLabelResponse = callProjectInfoTool(
            217, QStringLiteral("set_clip_label"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("label"), QStringLiteral("ultraviolet")}
            });
        const bool g120 = observeTerm(120, "toolResult(invalidLabelResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidLabelResponse)
                                .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(120, "toolErrorText(invalidLabelResponse).contains( QStringLiteral(\"label must be one of\"))", static_cast<bool>(toolErrorText(invalidLabelResponse).contains(
                   QStringLiteral("label must be one of"))))
            && observeTerm(120, "video0->clips().at(0).label == ClipLabel::Red", static_cast<bool>(video0->clips().at(0).label == ClipLabel::Red));
        g120 ? pass("G120 set_clip_label rejects an invalid label")
             : fail("G120 set_clip_label rejects an invalid label",
                    QStringLiteral("invalid label was accepted or changed the clip"));

        const QJsonObject labelUndoResponse = callProjectInfoTool(
            218, QStringLiteral("undo"), QJsonObject{});
        const bool g121 = observeTerm(121, "toolPayload(labelUndoResponse) .value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(labelUndoResponse)
                                .value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(121, "video0->clips().at(0).label == ClipLabel::None", static_cast<bool>(video0->clips().at(0).label == ClipLabel::None))
            && observeTerm(121, "!projectTimeline->undoManager()->canUndo()", static_cast<bool>(!projectTimeline->undoManager()->canUndo()));
        g121 ? pass("G121 set_clip_label is reverted by one undo")
             : fail("G121 set_clip_label is reverted by one undo",
                    QStringLiteral("one undo did not restore the default label"));

        saveTestUndoBaseline();
        const QJsonObject roundtripLabelResponse = callProjectInfoTool(
            219, QStringLiteral("set_clip_label"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("label"), QStringLiteral("green")}
            });
        const QString labelProjectPath = projectBehaviorDirReady
            ? QDir(projectBehaviorDir.path()).filePath(
                  QStringLiteral("clip-label-roundtrip.vsep"))
            : QString();
        QJsonObject labelSaveResponse;
        QJsonObject labelOpenResponse;
        QJsonDocument savedLabelDocument;
        bool savedLabelFileRead = false;
        if (projectBehaviorDirReady) {
            labelSaveResponse = callProjectInfoTool(
                220, QStringLiteral("save_project"), QJsonObject{
                    {QStringLiteral("path"), labelProjectPath}
                });
            QFile labelFile(labelProjectPath);
            savedLabelFileRead = labelFile.open(QIODevice::ReadOnly);
            if (savedLabelFileRead)
                savedLabelDocument = QJsonDocument::fromJson(labelFile.readAll());

            QVector<ClipInfo> clearedLabels = video0->clips();
            for (ClipInfo &clip : clearedLabels)
                clip.label = ClipLabel::None;
            video0->setClips(clearedLabels);
            labelOpenResponse = callProjectInfoTool(
                221, QStringLiteral("open_project"), QJsonObject{
                    {QStringLiteral("path"), labelProjectPath}
                });
        }
        TimelineTrack *reopenedLabelTrack = projectTimeline->trackAt(false, 0);
        const QJsonArray savedLabelTracks = savedLabelDocument.object()
            .value(QStringLiteral("videoTracks")).toArray();
        const QJsonArray savedLabelClips = savedLabelTracks.isEmpty()
            ? QJsonArray{} : savedLabelTracks.first().toArray();
        const QJsonObject reopenedLabelTimeline = callProjectInfoTool(
            222, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}
            });
        const QJsonArray reopenedLabelClips = timelineTrackObject(
            toolPayload(reopenedLabelTimeline), QStringLiteral("video"), 0)
                .value(QStringLiteral("clips")).toArray();
        const bool g122 = observeTerm(122, "projectBehaviorDirReady", static_cast<bool>(projectBehaviorDirReady)) && observeTerm(122, "savedLabelFileRead", static_cast<bool>(savedLabelFileRead))
            && observeTerm(122, "!toolResult(roundtripLabelResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(roundtripLabelResponse)
                    .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(122, "!toolResult(labelSaveResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(labelSaveResponse)
                    .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(122, "!toolResult(labelOpenResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(labelOpenResponse)
                    .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(122, "savedLabelClips.size() == 2", static_cast<bool>(savedLabelClips.size() == 2))
            && observeTerm(122, "savedLabelClips.at(0).toObject() .value(QStringLiteral(\"label\")).toString() == QStringLiteral(\"green\")", static_cast<bool>(savedLabelClips.at(0).toObject()
                   .value(QStringLiteral("label")).toString() == QStringLiteral("green")))
            && observeTerm(122, "!savedLabelClips.at(1).toObject().contains(QStringLiteral(\"label\"))", static_cast<bool>(!savedLabelClips.at(1).toObject().contains(QStringLiteral("label"))))
            && observeTerm(122, "reopenedLabelTrack", static_cast<bool>(reopenedLabelTrack)) && observeTerm(122, "reopenedLabelTrack->clipCount() == 2", static_cast<bool>(reopenedLabelTrack->clipCount() == 2))
            && observeTerm(122, "reopenedLabelTrack->clips().at(0).label == ClipLabel::Green", static_cast<bool>(reopenedLabelTrack->clips().at(0).label == ClipLabel::Green))
            && observeTerm(122, "reopenedLabelTrack->clips().at(1).label == ClipLabel::None", static_cast<bool>(reopenedLabelTrack->clips().at(1).label == ClipLabel::None))
            && observeTerm(122, "reopenedLabelClips.size() == 2", static_cast<bool>(reopenedLabelClips.size() == 2))
            && observeTerm(122, "reopenedLabelClips.at(0).toObject() .value(QStringLiteral(\"label\")).toString() == QStringLiteral(\"green\")", static_cast<bool>(reopenedLabelClips.at(0).toObject()
                   .value(QStringLiteral("label")).toString() == QStringLiteral("green")))
            && observeTerm(122, "reopenedLabelClips.at(1).toObject() .value(QStringLiteral(\"label\")).toString() == QStringLiteral(\"none\")", static_cast<bool>(reopenedLabelClips.at(1).toObject()
                   .value(QStringLiteral("label")).toString() == QStringLiteral("none")));
        g122 ? pass("G122 clip labels survive save/open and None is omitted")
             : fail("G122 clip labels survive save/open and None is omitted",
                    QStringLiteral("project JSON or reopened timeline lost clip labels"));

        TimelineTrack *reverseVideoTrack = projectTimeline->trackAt(false, 0);
        TimelineTrack *reverseAudioTrack = projectTimeline->trackAt(true, 0);
        ClipInfo reverseVideoClip = makeTestClip(
            QStringLiteral("reverse-video"), 0);
        ClipInfo reverseAudioClip = makeTestClip(
            QStringLiteral("reverse-audio"), 0);
        reverseVideoClip.linkGroup = 12304;
        reverseAudioClip.linkGroup = 12304;
        if (reverseVideoTrack)
            reverseVideoTrack->setClips({reverseVideoClip});
        if (reverseAudioTrack)
            reverseAudioTrack->setClips({reverseAudioClip});
        saveTestUndoBaseline();

        const QJsonObject setReversedResponse = callProjectInfoTool(
            223, QStringLiteral("set_clip_property"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("property"), QStringLiteral("reversed")},
                {QStringLiteral("value"), true}
            });
        const QJsonObject reversedTimelineResponse = callProjectInfoTool(
            224, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("all")}
            });
        const QJsonObject reversedPayload = toolPayload(setReversedResponse);
        const QJsonArray reversedVideoClips = timelineTrackObject(
            toolPayload(reversedTimelineResponse), QStringLiteral("video"), 0)
                .value(QStringLiteral("clips")).toArray();
        const QJsonArray reversedAudioClips = timelineTrackObject(
            toolPayload(reversedTimelineResponse), QStringLiteral("audio"), 0)
                .value(QStringLiteral("clips")).toArray();
        const bool g123 = observeTerm(123, "reverseVideoTrack", static_cast<bool>(reverseVideoTrack)) && observeTerm(123, "reverseAudioTrack", static_cast<bool>(reverseAudioTrack))
            && observeTerm(123, "!toolResult(setReversedResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(setReversedResponse)
                    .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(123, "reversedPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(reversedPayload.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(123, "reversedPayload.value(QStringLiteral(\"property\")).toString() == QStringLiteral(\"reversed\")", static_cast<bool>(reversedPayload.value(QStringLiteral("property")).toString()
                   == QStringLiteral("reversed")))
            && observeTerm(123, "reversedPayload.value(QStringLiteral(\"value\")).isBool()", static_cast<bool>(reversedPayload.value(QStringLiteral("value")).isBool()))
            && observeTerm(123, "reversedPayload.value(QStringLiteral(\"value\")).toBool(false)", static_cast<bool>(reversedPayload.value(QStringLiteral("value")).toBool(false)))
            && observeTerm(123, "reversedPayload.value(QStringLiteral(\"linkedApplied\")).toBool(false)", static_cast<bool>(reversedPayload.value(QStringLiteral("linkedApplied")).toBool(false)))
            && observeTerm(123, "requiredOutputFieldsPresent(QStringLiteral(\"set_clip_property\"), reversedPayload)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("set_clip_property"),
                                           reversedPayload)))
            && observeTerm(123, "reverseVideoTrack->clips().first().reversed", static_cast<bool>(reverseVideoTrack->clips().first().reversed))
            && observeTerm(123, "reverseAudioTrack->clips().first().reversed", static_cast<bool>(reverseAudioTrack->clips().first().reversed))
            && observeTerm(123, "reversedVideoClips.size() == 1", static_cast<bool>(reversedVideoClips.size() == 1))
            && observeTerm(123, "reversedAudioClips.size() == 1", static_cast<bool>(reversedAudioClips.size() == 1))
            && observeTerm(123, "reversedVideoClips.first().toObject() .value(QStringLiteral(\"reversed\")).toBool(false)", static_cast<bool>(reversedVideoClips.first().toObject()
                   .value(QStringLiteral("reversed")).toBool(false)))
            && observeTerm(123, "reversedAudioClips.first().toObject() .value(QStringLiteral(\"reversed\")).toBool(false)", static_cast<bool>(reversedAudioClips.first().toObject()
                   .value(QStringLiteral("reversed")).toBool(false)));
        g123 ? pass("G123 reversed set is reflected by get_timeline")
             : fail("G123 reversed set is reflected by get_timeline",
                    QStringLiteral("boolean mutation, linked clip, schema, or timeline output diverged"));

        const QJsonObject reverseUndoResponse = callProjectInfoTool(
            225, QStringLiteral("undo"), QJsonObject{});
        const QJsonObject undoTimelineResponse = callProjectInfoTool(
            226, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("all")}
            });
        const QJsonArray undoVideoClips = timelineTrackObject(
            toolPayload(undoTimelineResponse), QStringLiteral("video"), 0)
                .value(QStringLiteral("clips")).toArray();
        const QJsonArray undoAudioClips = timelineTrackObject(
            toolPayload(undoTimelineResponse), QStringLiteral("audio"), 0)
                .value(QStringLiteral("clips")).toArray();
        const bool g124 = observeTerm(124, "toolPayload(reverseUndoResponse) .value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(reverseUndoResponse)
                                .value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(124, "reverseVideoTrack", static_cast<bool>(reverseVideoTrack)) && observeTerm(124, "reverseAudioTrack", static_cast<bool>(reverseAudioTrack))
            && observeTerm(124, "!reverseVideoTrack->clips().first().reversed", static_cast<bool>(!reverseVideoTrack->clips().first().reversed))
            && observeTerm(124, "!reverseAudioTrack->clips().first().reversed", static_cast<bool>(!reverseAudioTrack->clips().first().reversed))
            && observeTerm(124, "undoVideoClips.size() == 1", static_cast<bool>(undoVideoClips.size() == 1)) && observeTerm(124, "undoAudioClips.size() == 1", static_cast<bool>(undoAudioClips.size() == 1))
            && observeTerm(124, "!undoVideoClips.first().toObject() .value(QStringLiteral(\"reversed\")).toBool(true)", static_cast<bool>(!undoVideoClips.first().toObject()
                    .value(QStringLiteral("reversed")).toBool(true)))
            && observeTerm(124, "!undoAudioClips.first().toObject() .value(QStringLiteral(\"reversed\")).toBool(true)", static_cast<bool>(!undoAudioClips.first().toObject()
                    .value(QStringLiteral("reversed")).toBool(true)))
            && observeTerm(124, "!projectTimeline->undoManager()->canUndo()", static_cast<bool>(!projectTimeline->undoManager()->canUndo()));
        g124 ? pass("G124 reversed is reverted by one undo")
             : fail("G124 reversed is reverted by one undo",
                    QStringLiteral("one undo did not restore linked reverse flags"));

        // G144-G145: AUTO-ORIENT boolean property is visible and one-step undoable.
        ClipInfo autoOrientClip = makeTestClip(
            QStringLiteral("auto-orient-video"), 0);
        if (reverseVideoTrack)
            reverseVideoTrack->setClips({autoOrientClip});
        if (reverseAudioTrack)
            reverseAudioTrack->setClips(QVector<ClipInfo>{});
        saveTestUndoBaseline();

        const QJsonObject setAutoOrientResponse = callProjectInfoTool(
            301, QStringLiteral("set_clip_property"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("property"), QStringLiteral("autoOrient")},
                {QStringLiteral("value"), true}
            });
        const QJsonObject autoOrientTimelineResponse = callProjectInfoTool(
            302, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}
            });
        const QJsonObject autoOrientPayload = toolPayload(setAutoOrientResponse);
        const QJsonArray autoOrientClips = timelineTrackObject(
            toolPayload(autoOrientTimelineResponse), QStringLiteral("video"), 0)
                .value(QStringLiteral("clips")).toArray();
        const bool g144 = observeTerm(144, "reverseVideoTrack", static_cast<bool>(reverseVideoTrack))
            && observeTerm(144, "!toolResult(setAutoOrientResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(setAutoOrientResponse)
                    .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(144, "autoOrientPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(autoOrientPayload.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(144, "autoOrientPayload.value(QStringLiteral(\"property\")).toString() == QStringLiteral(\"autoOrient\")", static_cast<bool>(autoOrientPayload.value(QStringLiteral("property")).toString()
                   == QStringLiteral("autoOrient")))
            && observeTerm(144, "autoOrientPayload.value(QStringLiteral(\"value\")).isBool()", static_cast<bool>(autoOrientPayload.value(QStringLiteral("value")).isBool()))
            && observeTerm(144, "autoOrientPayload.value(QStringLiteral(\"value\")).toBool(false)", static_cast<bool>(autoOrientPayload.value(QStringLiteral("value")).toBool(false)))
            && observeTerm(144, "!autoOrientPayload.value(QStringLiteral(\"linkedApplied\")).toBool(true)", static_cast<bool>(!autoOrientPayload.value(QStringLiteral("linkedApplied")).toBool(true)))
            && observeTerm(144, "requiredOutputFieldsPresent(QStringLiteral(\"set_clip_property\"), autoOrientPayload)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("set_clip_property"),
                                           autoOrientPayload)))
            && observeTerm(144, "reverseVideoTrack->clips().first().autoOrientEnabled", static_cast<bool>(reverseVideoTrack->clips().first().autoOrientEnabled))
            && observeTerm(144, "autoOrientClips.size() == 1", static_cast<bool>(autoOrientClips.size() == 1))
            && observeTerm(144, "autoOrientClips.first().toObject() .value(QStringLiteral(\"autoOrient\")).toBool(false)", static_cast<bool>(autoOrientClips.first().toObject()
                   .value(QStringLiteral("autoOrient")).toBool(false)));
        g144 ? pass("G144 set_clip_property autoOrient is reflected by get_timeline")
             : fail("G144 set_clip_property autoOrient is reflected by get_timeline",
                    QStringLiteral("boolean mutation, schema, or timeline output diverged"));

        const QJsonObject autoOrientUndoResponse = callProjectInfoTool(
            303, QStringLiteral("undo"), QJsonObject{});
        const QJsonObject autoOrientUndoTimelineResponse = callProjectInfoTool(
            304, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}
            });
        const QJsonArray autoOrientUndoClips = timelineTrackObject(
            toolPayload(autoOrientUndoTimelineResponse),
            QStringLiteral("video"), 0).value(QStringLiteral("clips")).toArray();
        const bool g145 = observeTerm(145, "toolPayload(autoOrientUndoResponse) .value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(autoOrientUndoResponse)
                                .value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(145, "reverseVideoTrack", static_cast<bool>(reverseVideoTrack))
            && observeTerm(145, "!reverseVideoTrack->clips().first().autoOrientEnabled", static_cast<bool>(!reverseVideoTrack->clips().first().autoOrientEnabled))
            && observeTerm(145, "autoOrientUndoClips.size() == 1", static_cast<bool>(autoOrientUndoClips.size() == 1))
            && observeTerm(145, "!autoOrientUndoClips.first().toObject() .value(QStringLiteral(\"autoOrient\")).toBool(true)", static_cast<bool>(!autoOrientUndoClips.first().toObject()
                    .value(QStringLiteral("autoOrient")).toBool(true)))
            && observeTerm(145, "!projectTimeline->undoManager()->canUndo()", static_cast<bool>(!projectTimeline->undoManager()->canUndo()));
        g145 ? pass("G145 autoOrient is reverted by one undo")
             : fail("G145 autoOrient is reverted by one undo",
                    QStringLiteral("one undo did not restore the auto-orient flag"));

        // 後続の get_frame fixture は空の V1/A1 へ media を 1 件ずつ取り込む。
        // roundtrip で再読込したクリップと undo 履歴をここで隔離する。
        const QVector<TimelineTrack *> tracks =
            projectTimeline->videoTracks() + projectTimeline->audioTracks();
        for (TimelineTrack *track : tracks) {
            if (track)
                track->setClips(QVector<ClipInfo>{});
        }
        projectTimeline->applyTrackFlagsFromJson(QJsonObject{});
        projectTimeline->clearSelection();
        saveTestUndoBaseline();
    }
    if (!timelineReady) {
        fail("G45 select_clip rejects out-of-range index", QStringLiteral("Timeline was not available"));
        fail("G46 select_clip synchronizes Timeline and MainWindow", QStringLiteral("Timeline was not available"));
        fail("G47 invalid selection preserves existing selection", QStringLiteral("Timeline was not available"));
        fail("G48 clear_selection clears both state holders", QStringLiteral("Timeline was not available"));
        fail("G49 continuous clips can be reordered and undone", QStringLiteral("Timeline was not available"));
        fail("G50 blocked move reports actual start and reason", QStringLiteral("Timeline was not available"));
        fail("G51 move_clip supports cross-track movement", QStringLiteral("Timeline was not available"));
        fail("G52 linked V/A clips move together", QStringLiteral("Timeline was not available"));
        fail("G114 set_track_locked is reflected by get_timeline", QStringLiteral("Timeline was not available"));
        fail("G115 set_track_locked unlocks the track", QStringLiteral("Timeline was not available"));
        fail("G116 set_track_locked rejects a missing track", QStringLiteral("Timeline was not available"));
        fail("G117 locked tracks reject split/delete/move", QStringLiteral("Timeline was not available"));
        fail("G118 track flags survive save/open and legacy defaults are false", QStringLiteral("Timeline was not available"));
        fail("G119 set_clip_label is reflected by get_timeline", QStringLiteral("Timeline was not available"));
        fail("G120 set_clip_label rejects an invalid label", QStringLiteral("Timeline was not available"));
        fail("G121 set_clip_label is reverted by one undo", QStringLiteral("Timeline was not available"));
        fail("G122 clip labels survive save/open and None is omitted", QStringLiteral("Timeline was not available"));
        fail("G123 reversed set is reflected by get_timeline", QStringLiteral("Timeline was not available"));
        fail("G124 reversed is reverted by one undo", QStringLiteral("Timeline was not available"));
        fail("G144 set_clip_property autoOrient is reflected by get_timeline", QStringLiteral("Timeline was not available"));
        fail("G145 autoOrient is reverted by one undo", QStringLiteral("Timeline was not available"));
    }

    const QJsonObject originalTimecodeBurnIn =
        projectInfoResult.value(QStringLiteral("timecodeBurnIn")).toObject();
    const QJsonObject requestedTimecodeBurnIn{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("position"), QStringLiteral("topRight")},
        {QStringLiteral("fontSizePct"), 6},
        {QStringLiteral("showFrames"), false},
        {QStringLiteral("dropFrame"), true},
        {QStringLiteral("prefix"), QStringLiteral("MCP")},
        {QStringLiteral("showClipName"), true},
        {QStringLiteral("opacity"), 0.65}
    };
    const QJsonObject setProjectOptionResponse = callProjectInfoTool(
        227, QStringLiteral("set_project_option"), QJsonObject{
            {QStringLiteral("option"), QStringLiteral("timecodeBurnIn")},
            {QStringLiteral("value"), requestedTimecodeBurnIn}
        });
    const QJsonObject projectInfoAfterOption = callProjectInfoTool(
        228, QStringLiteral("get_project_info"), QJsonObject{});
    const QJsonObject setProjectOptionPayload = toolPayload(
        setProjectOptionResponse);
    const QJsonObject reflectedTimecodeBurnIn = toolPayload(
        projectInfoAfterOption).value(QStringLiteral("timecodeBurnIn"))
            .toObject();
    const bool g125 = observeTerm(125, "!toolResult(setProjectOptionResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(setProjectOptionResponse)
                            .value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(125, "setProjectOptionPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(setProjectOptionPayload.value(QStringLiteral("ok")).toBool(false)))
        && observeTerm(125, "setProjectOptionPayload.value(QStringLiteral(\"option\")).toString() == QStringLiteral(\"timecodeBurnIn\")", static_cast<bool>(setProjectOptionPayload.value(QStringLiteral("option")).toString()
               == QStringLiteral("timecodeBurnIn")))
        && observeTerm(125, "requiredOutputFieldsPresent(QStringLiteral(\"set_project_option\"), setProjectOptionPayload)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("set_project_option"),
                                       setProjectOptionPayload)))
        && observeTerm(125, "requiredOutputFieldsPresent(QStringLiteral(\"get_project_info\"), toolPayload(projectInfoAfterOption))", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("get_project_info"),
                                       toolPayload(projectInfoAfterOption))))
        && observeTerm(125, "reflectedTimecodeBurnIn.value(QStringLiteral(\"enabled\")).toBool(false)", static_cast<bool>(reflectedTimecodeBurnIn.value(QStringLiteral("enabled")).toBool(false)))
        && observeTerm(125, "reflectedTimecodeBurnIn.value(QStringLiteral(\"position\")).toString() == QStringLiteral(\"topRight\")", static_cast<bool>(reflectedTimecodeBurnIn.value(QStringLiteral("position")).toString()
               == QStringLiteral("topRight")))
        && observeTerm(125, "reflectedTimecodeBurnIn.value(QStringLiteral(\"fontSizePct\")).toInt() == 6", static_cast<bool>(reflectedTimecodeBurnIn.value(QStringLiteral("fontSizePct")).toInt()
               == 6))
        && observeTerm(125, "!reflectedTimecodeBurnIn.value(QStringLiteral(\"showFrames\")) .toBool(true)", static_cast<bool>(!reflectedTimecodeBurnIn.value(QStringLiteral("showFrames"))
                .toBool(true)))
        && observeTerm(125, "reflectedTimecodeBurnIn.value(QStringLiteral(\"dropFrame\")).toBool(false)", static_cast<bool>(reflectedTimecodeBurnIn.value(QStringLiteral("dropFrame")).toBool(false)))
        && observeTerm(125, "reflectedTimecodeBurnIn.value(QStringLiteral(\"prefix\")).toString() == QStringLiteral(\"MCP\")", static_cast<bool>(reflectedTimecodeBurnIn.value(QStringLiteral("prefix")).toString()
               == QStringLiteral("MCP")))
        && observeTerm(125, "reflectedTimecodeBurnIn.value(QStringLiteral(\"showClipName\")) .toBool(false)", static_cast<bool>(reflectedTimecodeBurnIn.value(QStringLiteral("showClipName"))
               .toBool(false)))
        && observeTerm(125, "std::fabs(reflectedTimecodeBurnIn.value(QStringLiteral(\"opacity\")) .toDouble() - 0.65) < 1e-9", static_cast<bool>(std::fabs(reflectedTimecodeBurnIn.value(QStringLiteral("opacity"))
                         .toDouble() - 0.65) < 1e-9));
    g125 ? pass("G125 set_project_option is reflected by get_project_info")
         : fail("G125 set_project_option is reflected by get_project_info",
                QStringLiteral("timecodeBurnIn mutation or output schema diverged"));

    const QJsonObject unknownProjectOptionResponse = callProjectInfoTool(
        229, QStringLiteral("set_project_option"), QJsonObject{
            {QStringLiteral("option"), QStringLiteral("unknownOption")},
            {QStringLiteral("value"), QJsonObject{}}
        });
    const QJsonObject projectInfoAfterUnknownOption = callProjectInfoTool(
        230, QStringLiteral("get_project_info"), QJsonObject{});
    const QJsonObject restoreProjectOptionResponse = callProjectInfoTool(
        231, QStringLiteral("set_project_option"), QJsonObject{
            {QStringLiteral("option"), QStringLiteral("timecodeBurnIn")},
            {QStringLiteral("value"), originalTimecodeBurnIn}
        });
    const QJsonObject projectInfoAfterRestore = callProjectInfoTool(
        232, QStringLiteral("get_project_info"), QJsonObject{});
    const bool g126 = observeTerm(126, "toolResult(unknownProjectOptionResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(unknownProjectOptionResponse)
                            .value(QStringLiteral("isError")).toBool(false)))
        && observeTerm(126, "toolErrorText(unknownProjectOptionResponse).contains( QStringLiteral(\"unknown project option\"))", static_cast<bool>(toolErrorText(unknownProjectOptionResponse).contains(
               QStringLiteral("unknown project option"))))
        && observeTerm(126, "toolPayload(projectInfoAfterUnknownOption) .value(QStringLiteral(\"timecodeBurnIn\")).toObject() == reflectedTimecodeBurnIn", static_cast<bool>(toolPayload(projectInfoAfterUnknownOption)
               .value(QStringLiteral("timecodeBurnIn")).toObject()
               == reflectedTimecodeBurnIn))
        && observeTerm(126, "!toolResult(restoreProjectOptionResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(restoreProjectOptionResponse)
                .value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(126, "toolPayload(projectInfoAfterRestore) .value(QStringLiteral(\"timecodeBurnIn\")).toObject() == originalTimecodeBurnIn", static_cast<bool>(toolPayload(projectInfoAfterRestore)
               .value(QStringLiteral("timecodeBurnIn")).toObject()
               == originalTimecodeBurnIn));
    g126 ? pass("G126 unknown project option is rejected")
         : fail("G126 unknown project option is rejected",
                QStringLiteral("unknown option was accepted or changed settings"));

    TimelineTrack *dynamicZoomTrack = timelineReady
        ? projectTimeline->trackAt(false, 0) : nullptr;
    if (dynamicZoomTrack) {
        dynamicZoomTrack->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("dynamic-zoom"), 0)
        });
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(),
            QStringLiteral("MCP dynamic zoom baseline"));

        const QJsonObject zoomResponse = callProjectInfoTool(
            233, QStringLiteral("dynamic_zoom"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("preset"), QStringLiteral("zoomIn")},
                {QStringLiteral("easing"), QStringLiteral("easeInOut")}
            });
        const QJsonObject zoomPayload = toolPayload(zoomResponse);
        const QJsonObject zoomCounts =
            zoomPayload.value(QStringLiteral("keyframeCounts")).toObject();
        QVector<ClipInfo> editedZoomClips = dynamicZoomTrack->clips();
        KeyframeTrack *editableScaleX = editedZoomClips.isEmpty()
            ? nullptr
            : editedZoomClips.first().keyframes.track(QStringLiteral("scaleX"));
        const double editedScale = 2.75;
        if (editableScaleX && editableScaleX->count() == 2) {
            const KeyframePoint last = editableScaleX->keyframes().last();
            editableScaleX->addKeyframe(
                last.time, editedScale, last.interpolation,
                last.bezX1, last.bezY1, last.bezX2, last.bezY2,
                last.hasSpatialTangent,
                last.spatialOutX, last.spatialOutY,
                last.spatialInX, last.spatialInY);
            dynamicZoomTrack->setClips(editedZoomClips);
        }
        const KeyframeManager& zoomKeyframes =
            dynamicZoomTrack->clips().first().keyframes;
        const KeyframeTrack *scaleX =
            zoomKeyframes.track(QStringLiteral("scaleX"));
        int storedZoomKeyframeCount = 0;
        for (const KeyframeTrack& track : zoomKeyframes.tracks())
            storedZoomKeyframeCount += track.count();
        // clipanim resolves the runtime motion.* names from the stored public
        // tracks at read time, so the playback value must follow the edited
        // public keyframe without any motion.* track existing.
        const double zoomEndTime = scaleX && scaleX->count() == 2
            ? scaleX->keyframes().last().time : 0.0;
        const double derivedEndScale = clipanim::effectiveTransformAt(
            dynamicZoomTrack->clips().first(), zoomEndTime).videoScale;
        const bool g127 = observeTerm(127, "!toolResult(zoomResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(zoomResponse)
                               .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(127, "zoomPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(zoomPayload.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(127, "zoomPayload.value(QStringLiteral(\"keyframeCount\")).toInt(-1) == 8", static_cast<bool>(zoomPayload.value(QStringLiteral("keyframeCount")).toInt(-1) == 8))
            && observeTerm(127, "zoomCounts.value(QStringLiteral(\"scaleX\")).toInt(-1) == 2", static_cast<bool>(zoomCounts.value(QStringLiteral("scaleX")).toInt(-1) == 2))
            && observeTerm(127, "scaleX", static_cast<bool>(scaleX)) && observeTerm(127, "scaleX->count() == 2", static_cast<bool>(scaleX->count() == 2))
            && observeTerm(127, "zoomKeyframes.tracks().size() == 4", static_cast<bool>(zoomKeyframes.tracks().size() == 4))
            && observeTerm(127, "storedZoomKeyframeCount == 8", static_cast<bool>(storedZoomKeyframeCount == 8))
            && observeTerm(127, "!zoomKeyframes.hasTrack(QStringLiteral(\"motion.position.x\"))", static_cast<bool>(!zoomKeyframes.hasTrack(QStringLiteral("motion.position.x"))))
            && observeTerm(127, "!zoomKeyframes.hasTrack(QStringLiteral(\"motion.position.y\"))", static_cast<bool>(!zoomKeyframes.hasTrack(QStringLiteral("motion.position.y"))))
            && observeTerm(127, "!zoomKeyframes.hasTrack(QStringLiteral(\"motion.scale\"))", static_cast<bool>(!zoomKeyframes.hasTrack(QStringLiteral("motion.scale"))))
            && observeTerm(127, "qAbs(derivedEndScale - scaleX->keyframes().last().value) < 1e-9", static_cast<bool>(qAbs(derivedEndScale - scaleX->keyframes().last().value) < 1e-9))
            && observeTerm(127, "qAbs(derivedEndScale - editedScale) < 1e-9", static_cast<bool>(qAbs(derivedEndScale - editedScale) < 1e-9))
            && observeTerm(127, "scaleX->keyframes().last().value > scaleX->keyframes().first().value", static_cast<bool>(scaleX->keyframes().last().value
                   > scaleX->keyframes().first().value));
        g127 ? pass("G127 dynamic_zoom zoomIn generates two scaleX keyframes")
             : fail("G127 dynamic_zoom zoomIn generates two scaleX keyframes",
                    QStringLiteral("preset response or generated scaleX track diverged"));

        const QJsonObject zoomUndo = callProjectInfoTool(
            234, QStringLiteral("undo"), QJsonObject{});
        const KeyframeManager& afterUndo =
            dynamicZoomTrack->clips().first().keyframes;
        const bool g128 = observeTerm(128, "toolPayload(zoomUndo) .value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(zoomUndo)
                                .value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(128, "!afterUndo.hasTrack(QStringLiteral(\"positionX\"))", static_cast<bool>(!afterUndo.hasTrack(QStringLiteral("positionX"))))
            && observeTerm(128, "!afterUndo.hasTrack(QStringLiteral(\"positionY\"))", static_cast<bool>(!afterUndo.hasTrack(QStringLiteral("positionY"))))
            && observeTerm(128, "!afterUndo.hasTrack(QStringLiteral(\"scaleX\"))", static_cast<bool>(!afterUndo.hasTrack(QStringLiteral("scaleX"))))
            && observeTerm(128, "!afterUndo.hasTrack(QStringLiteral(\"scaleY\"))", static_cast<bool>(!afterUndo.hasTrack(QStringLiteral("scaleY"))))
            && observeTerm(128, "!afterUndo.hasTrack(QStringLiteral(\"motion.position.x\"))", static_cast<bool>(!afterUndo.hasTrack(QStringLiteral("motion.position.x"))))
            && observeTerm(128, "!afterUndo.hasTrack(QStringLiteral(\"motion.position.y\"))", static_cast<bool>(!afterUndo.hasTrack(QStringLiteral("motion.position.y"))))
            && observeTerm(128, "!afterUndo.hasTrack(QStringLiteral(\"motion.scale\"))", static_cast<bool>(!afterUndo.hasTrack(QStringLiteral("motion.scale"))))
            && observeTerm(128, "!projectTimeline->undoManager()->canUndo()", static_cast<bool>(!projectTimeline->undoManager()->canUndo()));
        g128 ? pass("G128 undo removes dynamic_zoom keyframes in one step")
             : fail("G128 undo removes dynamic_zoom keyframes in one step",
                    QStringLiteral("one undo did not restore the keyframe-free clip"));

        const QJsonObject customRect{
            {QStringLiteral("cx"), 0.5}, {QStringLiteral("cy"), 0.5},
            {QStringLiteral("w"), 1.0}
        };
        const QJsonObject customEnd{
            {QStringLiteral("cx"), 0.6}, {QStringLiteral("cy"), 0.4},
            {QStringLiteral("w"), 0.5}, {QStringLiteral("h"), 0.25}
        };
        const QJsonObject customResponse = callProjectInfoTool(
            235, QStringLiteral("dynamic_zoom"), QJsonObject{
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("start"), customRect},
                {QStringLiteral("end"), customEnd},
                {QStringLiteral("easing"), QStringLiteral("linear")}
            });
        const KeyframeTrack *customScaleX =
            dynamicZoomTrack->clips().first().keyframes.track(
                QStringLiteral("scaleX"));
        const bool validCustomApplied = !toolResult(customResponse)
                                             .value(QStringLiteral("isError"))
                                             .toBool(true)
            && toolPayload(customResponse)
                   .value(QStringLiteral("keyframeCount")).toInt(-1) == 8
            && toolPayload(customResponse).value(QStringLiteral("warning"))
                   .toString().contains(
                       QStringLiteral("h はキャンバス比に固定"))
            && customScaleX && customScaleX->count() == 2
            && qAbs(customScaleX->keyframes().last().value - 2.0) < 1e-9;
        if (validCustomApplied)
            callProjectInfoTool(236, QStringLiteral("undo"), QJsonObject{});

        QJsonObject invalidStart = customRect;
        invalidStart.insert(QStringLiteral("w"), 0.0);
        const QJsonObject invalidRectResponse = callProjectInfoTool(
            237, QStringLiteral("dynamic_zoom"), QJsonObject{
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("start"), invalidStart},
                {QStringLiteral("end"), customEnd}
            });
        const bool g129 = observeTerm(129, "validCustomApplied", static_cast<bool>(validCustomApplied))
            && observeTerm(129, "toolResult(invalidRectResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidRectResponse)
                   .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(129, "toolErrorText(invalidRectResponse).contains(QStringLiteral(\"w\"))", static_cast<bool>(toolErrorText(invalidRectResponse).contains(QStringLiteral("w"))))
            && observeTerm(129, "!dynamicZoomTrack->clips().first().keyframes .hasTrack(QStringLiteral(\"scaleX\"))", static_cast<bool>(!dynamicZoomTrack->clips().first().keyframes
                    .hasTrack(QStringLiteral("scaleX"))))
            && observeTerm(129, "!projectTimeline->undoManager()->canUndo()", static_cast<bool>(!projectTimeline->undoManager()->canUndo()));
        g129 ? pass("G129 dynamic_zoom accepts custom frames and rejects w<=0")
             : fail("G129 dynamic_zoom accepts custom frames and rejects w<=0",
                    QStringLiteral("custom rect handling or invalid-rect guard diverged"));

        const QJsonObject ambiguousResponse = callProjectInfoTool(
            243, QStringLiteral("dynamic_zoom"), QJsonObject{
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("preset"), QStringLiteral("zoomIn")},
                {QStringLiteral("start"), customRect}
            });
        QJsonObject dynamicZoomInputSchema;
        for (const QJsonValue& value : projectInfoToolDescriptors) {
            const QJsonObject descriptor = value.toObject();
            if (descriptor.value(QStringLiteral("name")).toString()
                == QStringLiteral("dynamic_zoom")) {
                dynamicZoomInputSchema =
                    descriptor.value(QStringLiteral("inputSchema")).toObject();
                break;
            }
        }
        const bool g134 = observeTerm(134, "toolResult(ambiguousResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(ambiguousResponse)
                                .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(134, "toolErrorText(ambiguousResponse).contains( QStringLiteral(\"cannot be combined\"))", static_cast<bool>(toolErrorText(ambiguousResponse).contains(
                   QStringLiteral("cannot be combined"))))
            && observeTerm(134, "!dynamicZoomInputSchema.contains(QStringLiteral(\"oneOf\"))", static_cast<bool>(!dynamicZoomInputSchema.contains(QStringLiteral("oneOf"))))
            && observeTerm(134, "dynamicZoomInputSchema.value(QStringLiteral(\"description\")) .toString().contains(QStringLiteral(\"どちらか一方\"))", static_cast<bool>(dynamicZoomInputSchema.value(QStringLiteral("description"))
                   .toString().contains(QStringLiteral("どちらか一方"))))
            && observeTerm(134, "!dynamicZoomTrack->clips().first().keyframes .hasTrack(QStringLiteral(\"scaleX\"))", static_cast<bool>(!dynamicZoomTrack->clips().first().keyframes
                    .hasTrack(QStringLiteral("scaleX"))))
            && observeTerm(134, "!projectTimeline->undoManager()->canUndo()", static_cast<bool>(!projectTimeline->undoManager()->canUndo()));
        g134 ? pass("G134 dynamic_zoom rejects preset with start/end")
             : fail("G134 dynamic_zoom rejects preset with start/end",
                    QStringLiteral("ambiguous input or schema contract diverged"));

        dynamicZoomTrack->setClips(QVector<ClipInfo>{});
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(),
            QStringLiteral("MCP selftest baseline"));
    } else {
        fail("G127 dynamic_zoom zoomIn generates two scaleX keyframes",
             QStringLiteral("Timeline was not available"));
        fail("G128 undo removes dynamic_zoom keyframes in one step",
             QStringLiteral("Timeline was not available"));
        fail("G129 dynamic_zoom accepts custom frames and rejects w<=0",
             QStringLiteral("Timeline was not available"));
        fail("G134 dynamic_zoom rejects preset with start/end",
             QStringLiteral("Timeline was not available"));
    }

    QTemporaryDir relinkMediaDir;
    const QString relinkOldPath = relinkMediaDir.isValid()
        ? QDir(relinkMediaDir.path()).filePath(QStringLiteral("offline.mp4"))
        : QString();
    const QString relinkNewPath = relinkMediaDir.isValid()
        ? QDir(relinkMediaDir.path()).filePath(QStringLiteral("online.mp4"))
        : QString();
    TimelineTrack *relinkVideoTrack = timelineReady
        ? projectTimeline->trackAt(false, 0) : nullptr;
    TimelineTrack *relinkAudioTrack = timelineReady
        ? projectTimeline->trackAt(true, 0) : nullptr;
    const bool relinkFixtureReady = relinkVideoTrack && relinkAudioTrack
        && importSourceReady && relinkMediaDir.isValid()
        && QFile::copy(importSourcePath, relinkNewPath);
    if (relinkFixtureReady) {
        ClipInfo relinkVideoClip = makeTestClip(relinkOldPath, 935);
        relinkVideoClip.linkGroup = 935;
        ClipInfo relinkAudioClip = relinkVideoClip;
        relinkVideoTrack->setClips({relinkVideoClip});
        relinkAudioTrack->setClips({relinkAudioClip});
        projectInfoWindow.m_particleClipConfigs.clear();
        projectInfoWindow.m_particleClipConfigs.insert(
            relinkOldPath, ParticleEmitterConfig{});
        OverlayItem relinkOverlay;
        relinkOverlay.type = QStringLiteral("image");
        relinkOverlay.text = relinkOldPath;
        projectInfoWindow.m_projectOverlays = {relinkOverlay};
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(),
            QStringLiteral("MCP relink media baseline"));

        const QJsonArray mapping{
            QJsonObject{
                {QStringLiteral("from"), relinkOldPath},
                {QStringLiteral("to"), relinkNewPath}
            }
        };
        const QJsonObject relinkResponse = callProjectInfoTool(
            244, QStringLiteral("relink_media"), QJsonObject{
                {QStringLiteral("mapping"), mapping}
            });
        const bool sidecarsRelinked =
            projectInfoWindow.m_particleClipConfigs.contains(relinkNewPath)
            && !projectInfoWindow.m_particleClipConfigs.contains(relinkOldPath)
            && projectInfoWindow.m_projectOverlays.size() == 1
            && projectInfoWindow.m_projectOverlays.first().text == relinkNewPath;
        projectTimeline->undo();
        const bool sidecarsUndoRestored =
            !projectTimeline->canUndo()
            && projectInfoWindow.m_particleClipConfigs.contains(relinkOldPath)
            && !projectInfoWindow.m_particleClipConfigs.contains(relinkNewPath)
            && projectInfoWindow.m_projectOverlays.size() == 1
            && projectInfoWindow.m_projectOverlays.first().text == relinkOldPath;
        projectTimeline->redo();
        const bool sidecarsRedoRestored =
            projectInfoWindow.m_particleClipConfigs.contains(relinkNewPath)
            && !projectInfoWindow.m_particleClipConfigs.contains(relinkOldPath)
            && projectInfoWindow.m_projectOverlays.size() == 1
            && projectInfoWindow.m_projectOverlays.first().text == relinkNewPath;
        const bool g135 = observeTerm(135, "!toolResult(relinkResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(relinkResponse)
                                .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(135, "toolPayload(relinkResponse).value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(relinkResponse).value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(135, "toolPayload(relinkResponse).value(QStringLiteral(\"relinked\")).toInt() == 1", static_cast<bool>(toolPayload(relinkResponse).value(QStringLiteral("relinked")).toInt() == 1))
            && observeTerm(135, "relinkVideoTrack->clips().size() == 1", static_cast<bool>(relinkVideoTrack->clips().size() == 1))
            && observeTerm(135, "relinkAudioTrack->clips().size() == 1", static_cast<bool>(relinkAudioTrack->clips().size() == 1))
            && observeTerm(135, "relinkVideoTrack->clips().first().filePath == relinkNewPath", static_cast<bool>(relinkVideoTrack->clips().first().filePath == relinkNewPath))
            && observeTerm(135, "relinkAudioTrack->clips().first().filePath == relinkNewPath", static_cast<bool>(relinkAudioTrack->clips().first().filePath == relinkNewPath))
            && observeTerm(135, "relinkVideoTrack->clips().first().linkGroup == 935", static_cast<bool>(relinkVideoTrack->clips().first().linkGroup == 935))
            && observeTerm(135, "relinkAudioTrack->clips().first().linkGroup == 935", static_cast<bool>(relinkAudioTrack->clips().first().linkGroup == 935))
            && observeTerm(135, "sidecarsRelinked", static_cast<bool>(sidecarsRelinked)) && observeTerm(135, "sidecarsUndoRestored", static_cast<bool>(sidecarsUndoRestored)) && observeTerm(135, "sidecarsRedoRestored", static_cast<bool>(sidecarsRedoRestored));
        g135 ? pass("G135 relink_media applies a valid mapping")
             : fail("G135 relink_media applies a valid mapping",
                    QStringLiteral("valid mapping did not update the linked clips"));

        const QString invalidDestination = QDir(relinkMediaDir.path())
            .filePath(QStringLiteral("missing.mp4"));
        const QJsonObject invalidRelinkResponse = callProjectInfoTool(
            245, QStringLiteral("relink_media"), QJsonObject{
                {QStringLiteral("mapping"), QJsonArray{
                    QJsonObject{
                        {QStringLiteral("from"), relinkNewPath},
                        {QStringLiteral("to"), invalidDestination}
                    }
                }}
            });
        const bool g136 = observeTerm(136, "toolResult(invalidRelinkResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidRelinkResponse)
                                .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(136, "toolErrorText(invalidRelinkResponse).contains( QStringLiteral(\"ファイルが見つかりません\"))", static_cast<bool>(toolErrorText(invalidRelinkResponse).contains(
                   QStringLiteral("ファイルが見つかりません"))))
            && observeTerm(136, "relinkVideoTrack->clips().first().filePath == relinkNewPath", static_cast<bool>(relinkVideoTrack->clips().first().filePath == relinkNewPath))
            && observeTerm(136, "relinkAudioTrack->clips().first().filePath == relinkNewPath", static_cast<bool>(relinkAudioTrack->clips().first().filePath == relinkNewPath));
        g136 ? pass("G136 relink_media rejects a missing destination")
             : fail("G136 relink_media rejects a missing destination",
                    QStringLiteral("missing destination was accepted or mutated clips"));

        relinkVideoTrack->setClips({});
        relinkAudioTrack->setClips({});
        projectInfoWindow.m_particleClipConfigs.clear();
        projectInfoWindow.m_projectOverlays.clear();
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(),
            QStringLiteral("MCP selftest baseline"));
    } else {
        const QString reason = QStringLiteral(
            "Timeline or relink media fixture was not available");
        fail("G135 relink_media applies a valid mapping", reason);
        fail("G136 relink_media rejects a missing destination", reason);
    }

    // US-105: use a small in-memory-generated PCM WAV so the normal MCP path
    // exercises decodeAudio -> beatdetect -> Timeline mutation without
    // depending on the contents of a repository media fixture.
    QTemporaryFile musicRemixWav;
    musicRemixWav.setFileTemplate(
        QDir::tempPath() + QStringLiteral("/veditor-remix-XXXXXX.wav"));
    bool musicRemixWavReady = musicRemixWav.open();
    if (musicRemixWavReady) {
        const auto appendLe16 = [](QByteArray &bytes, quint16 value) {
            bytes.append(static_cast<char>(value & 0xff));
            bytes.append(static_cast<char>((value >> 8) & 0xff));
        };
        const auto appendLe32 = [](QByteArray &bytes, quint32 value) {
            bytes.append(static_cast<char>(value & 0xff));
            bytes.append(static_cast<char>((value >> 8) & 0xff));
            bytes.append(static_cast<char>((value >> 16) & 0xff));
            bytes.append(static_cast<char>((value >> 24) & 0xff));
        };
        constexpr int sampleRate = 16000;
        constexpr int sampleCount = sampleRate * 8;
        QByteArray pcm;
        pcm.reserve(sampleCount * 2);
        for (int i = 0; i < sampleCount; ++i) {
            const double t = static_cast<double>(i) / sampleRate;
            const int beatIndex = qRound((t - 0.2) / 0.5);
            const double beatTime = 0.2 + beatIndex * 0.5;
            const bool click = beatIndex >= 0 && beatIndex < 15
                && qAbs(t - beatTime) < 0.0125;
            const qint16 value = click && ((i % 2) == 0) ? 28000 : 0;
            appendLe16(pcm, static_cast<quint16>(value));
        }
        QByteArray wav;
        wav.append("RIFF", 4);
        appendLe32(wav, static_cast<quint32>(36 + pcm.size()));
        wav.append("WAVEfmt ", 8);
        appendLe32(wav, 16);
        appendLe16(wav, 1);
        appendLe16(wav, 1);
        appendLe32(wav, sampleRate);
        appendLe32(wav, sampleRate * 2);
        appendLe16(wav, 2);
        appendLe16(wav, 16);
        wav.append("data", 4);
        appendLe32(wav, static_cast<quint32>(pcm.size()));
        wav.append(pcm);
        musicRemixWavReady = musicRemixWav.write(wav) == wav.size();
        musicRemixWav.flush();
        musicRemixWav.close();
    }

    TimelineTrack *musicRemixTrack = timelineReady
        ? projectTimeline->trackAt(true, 0) : nullptr;
    const bool musicRemixFixtureReady = musicRemixWavReady && musicRemixTrack;
    if (musicRemixFixtureReady) {
        ClipInfo remixClip;
        remixClip.filePath = musicRemixWav.fileName();
        remixClip.displayName = QStringLiteral("remix-test.wav");
        remixClip.duration = 8.0;
        remixClip.outPoint = 8.0;
        musicRemixTrack->setClips(QVector<ClipInfo>{remixClip});
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(), QStringLiteral("MCP music remix baseline"));

        const QJsonObject musicRemixResponse = callProjectInfoTool(
            260, QStringLiteral("music_remix"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("audio")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("targetSec"), 6.0}
            });
        const QJsonObject musicRemixPayload = toolPayload(musicRemixResponse);
        const bool g137 = observeTerm(137, "!toolResult(musicRemixResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(musicRemixResponse)
                                .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(137, "musicRemixPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(musicRemixPayload.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(137, "musicRemixPayload.value(QStringLiteral(\"resultDuration\")).toDouble() > 0.0", static_cast<bool>(musicRemixPayload.value(QStringLiteral("resultDuration")).toDouble() > 0.0))
            && observeTerm(137, "musicRemixPayload.value(QStringLiteral(\"segmentCount\")).toInt() >= 2", static_cast<bool>(musicRemixPayload.value(QStringLiteral("segmentCount")).toInt() >= 2))
            && observeTerm(137, "musicRemixTrack->clipCount() >= 2", static_cast<bool>(musicRemixTrack->clipCount() >= 2));
        g137 ? pass("G137 music_remix normal case")
             : fail("G137 music_remix normal case",
                    QStringLiteral("decode, beat detection, or response failed: %1")
                        .arg(toolErrorText(musicRemixResponse)));

        const int remixCountAfter = musicRemixTrack->clipCount();
        const QJsonObject invalidMusicRemixResponse = callProjectInfoTool(
            261, QStringLiteral("music_remix"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("audio")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("targetSec"), 0.0}
            });
        const QJsonObject oversizedMusicRemixResponse = callProjectInfoTool(
            262, QStringLiteral("music_remix"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("audio")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("targetSec"), remix::kMaxTargetSec + 1.0}
            });
        const bool g138 = observeTerm(138, "toolResult(invalidMusicRemixResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidMusicRemixResponse)
                                .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(138, "toolErrorText(invalidMusicRemixResponse).contains( QStringLiteral(\"targetSec\"))", static_cast<bool>(toolErrorText(invalidMusicRemixResponse).contains(
                   QStringLiteral("targetSec"))))
            && observeTerm(138, "toolResult(oversizedMusicRemixResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(oversizedMusicRemixResponse)
                   .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(138, "musicRemixTrack->clipCount() == remixCountAfter", static_cast<bool>(musicRemixTrack->clipCount() == remixCountAfter));
        g138 ? pass("G138 music_remix rejects invalid target")
             : fail("G138 music_remix rejects invalid target",
                    QStringLiteral("invalid target was accepted or mutated the timeline"));

        QJsonObject musicRemixDescriptor;
        for (const QJsonValue &value : projectInfoToolDescriptors) {
            if (value.toObject().value(QStringLiteral("name")).toString()
                    == QStringLiteral("music_remix")) {
                musicRemixDescriptor = value.toObject();
                break;
            }
        }
        const QJsonObject musicInputSchema = musicRemixDescriptor
            .value(QStringLiteral("inputSchema")).toObject();
        const QJsonObject musicOutputSchema = musicRemixDescriptor
            .value(QStringLiteral("outputSchema")).toObject();
        const auto requiredHas = [](const QJsonObject &schema, const QString &key) {
            for (const QJsonValue &required : schema.value(QStringLiteral("required"))
                     .toArray()) {
                if (required.toString() == key)
                    return true;
            }
            return false;
        };
        const bool g139 = observeTerm(139, "!musicRemixDescriptor.isEmpty()", static_cast<bool>(!musicRemixDescriptor.isEmpty()))
            && observeTerm(139, "musicInputSchema.value(QStringLiteral(\"type\")).toString() == QStringLiteral(\"object\")", static_cast<bool>(musicInputSchema.value(QStringLiteral("type")).toString()
                   == QStringLiteral("object")))
            && observeTerm(139, "requiredHas(musicInputSchema, QStringLiteral(\"kind\"))", static_cast<bool>(requiredHas(musicInputSchema, QStringLiteral("kind"))))
            && observeTerm(139, "requiredHas(musicInputSchema, QStringLiteral(\"targetSec\"))", static_cast<bool>(requiredHas(musicInputSchema, QStringLiteral("targetSec"))))
            && observeTerm(139, "musicInputSchema.value(QStringLiteral(\"properties\")).toObject() .value(QStringLiteral(\"targetSec\")).toObject() .value(QStringLiteral(\"maximum\")).toDouble() == remix::kMaxTargetSec", static_cast<bool>(musicInputSchema.value(QStringLiteral("properties")).toObject()
                   .value(QStringLiteral("targetSec")).toObject()
                   .value(QStringLiteral("maximum")).toDouble()
                   == remix::kMaxTargetSec))
            && observeTerm(139, "musicOutputSchema.value(QStringLiteral(\"type\")).toString() == QStringLiteral(\"object\")", static_cast<bool>(musicOutputSchema.value(QStringLiteral("type")).toString()
                   == QStringLiteral("object")))
            && observeTerm(139, "requiredHas(musicOutputSchema, QStringLiteral(\"resultDuration\"))", static_cast<bool>(requiredHas(musicOutputSchema, QStringLiteral("resultDuration"))))
            && observeTerm(139, "requiredHas(musicOutputSchema, QStringLiteral(\"segmentCount\"))", static_cast<bool>(requiredHas(musicOutputSchema, QStringLiteral("segmentCount"))));
        g139 ? pass("G139 music_remix schema")
             : fail("G139 music_remix schema",
                    QStringLiteral("input or output schema is incomplete"));
        projectTimeline->undo();
    } else {
        const QString reason = QStringLiteral(
            "Timeline or generated WAV fixture was not available");
        fail("G137 music_remix normal case", reason);
        fail("G138 music_remix rejects invalid target", reason);
        fail("G139 music_remix schema", reason);
    }

    TimelineTrack *replaceVideoTrack = timelineReady
        ? projectTimeline->trackAt(false, 0) : nullptr;
    TimelineTrack *replaceAudioTrack = timelineReady
        ? projectTimeline->trackAt(true, 0) : nullptr;
    QTemporaryDir replacementMediaDir;
    const QString replacementMediaPath = replacementMediaDir.isValid()
        ? QDir(replacementMediaDir.path()).filePath(
              QStringLiteral("replacement.mp4"))
        : QString();
    const bool replacementFixtureReady = replaceVideoTrack && replaceAudioTrack
        && importSourceReady && replacementMediaDir.isValid()
        && QFile::copy(importSourcePath, replacementMediaPath);
    if (replacementFixtureReady) {
        ClipInfo replaceVideoClip = makeTestClip(importSourcePath, 930);
        replaceVideoClip.displayName = QStringLiteral("original.mp4");
        replaceVideoClip.inPoint = 1.0;
        replaceVideoClip.outPoint = 3.0;
        replaceVideoClip.leadInSec = 2.0;
        ClipInfo replaceAudioClip = replaceVideoClip;
        replaceVideoTrack->setClips(QVector<ClipInfo>{replaceVideoClip});
        replaceAudioTrack->setClips(QVector<ClipInfo>{replaceAudioClip});
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(),
            QStringLiteral("MCP replace clip baseline"));

        const QJsonObject replaceResponse = callProjectInfoTool(
            238, QStringLiteral("replace_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("filePath"), replacementMediaPath}
            });
        const QJsonObject replaceTimelineResponse = callProjectInfoTool(
            239, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("all")}
            });
        const QJsonObject replaceTimelinePayload =
            toolPayload(replaceTimelineResponse);
        const QJsonArray replacementVideoTracks =
            replaceTimelinePayload.value(QStringLiteral("video")).toArray();
        const QJsonArray replacementVideoClips = replacementVideoTracks.isEmpty()
            ? QJsonArray()
            : replacementVideoTracks.first().toObject()
                  .value(QStringLiteral("clips")).toArray();
        const QJsonObject replacementVideoJson = replacementVideoClips.isEmpty()
            ? QJsonObject() : replacementVideoClips.first().toObject();
        const bool g130 = observeTerm(130, "!toolResult(replaceResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(replaceResponse)
                                .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(130, "toolPayload(replaceResponse).value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(replaceResponse).value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(130, "replacementVideoJson.value(QStringLiteral(\"filePath\")).toString() == replacementMediaPath", static_cast<bool>(replacementVideoJson.value(QStringLiteral("filePath")).toString()
                   == replacementMediaPath))
            && observeTerm(130, "qAbs(replacementVideoJson.value(QStringLiteral(\"startSec\")).toDouble() - 2.0) < 1e-9", static_cast<bool>(qAbs(replacementVideoJson.value(QStringLiteral("startSec")).toDouble()
                    - 2.0) < 1e-9))
            && observeTerm(130, "qAbs(replacementVideoJson.value(QStringLiteral(\"durationSec\")).toDouble() - 2.0) < 1e-9", static_cast<bool>(qAbs(replacementVideoJson.value(QStringLiteral("durationSec")).toDouble()
                    - 2.0) < 1e-9))
            && observeTerm(130, "qAbs(replacementVideoJson.value(QStringLiteral(\"inPointSec\")).toDouble() - 1.0) < 1e-9", static_cast<bool>(qAbs(replacementVideoJson.value(QStringLiteral("inPointSec")).toDouble()
                    - 1.0) < 1e-9))
            && observeTerm(130, "replaceAudioTrack->clips().size() == 1", static_cast<bool>(replaceAudioTrack->clips().size() == 1))
            && observeTerm(130, "replaceAudioTrack->clips().first().filePath == replacementMediaPath", static_cast<bool>(replaceAudioTrack->clips().first().filePath == replacementMediaPath));
        g130 ? pass("G130 replace_clip changes filePath and preserves start/duration")
             : fail("G130 replace_clip changes filePath and preserves start/duration",
                    QStringLiteral("replacement response or preserved clip geometry diverged"));

        const QString missingReplacementPath = QDir(replacementMediaDir.path())
            .filePath(QStringLiteral("missing.mp4"));
        const QJsonObject missingReplaceResponse = callProjectInfoTool(
            240, QStringLiteral("replace_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("filePath"), missingReplacementPath}
            });
        const bool g131 = observeTerm(131, "toolResult(missingReplaceResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(missingReplaceResponse)
                                .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(131, "toolErrorText(missingReplaceResponse).contains( QStringLiteral(\"ファイルが見つかりません\"))", static_cast<bool>(toolErrorText(missingReplaceResponse).contains(
                   QStringLiteral("ファイルが見つかりません"))))
            && observeTerm(131, "replaceVideoTrack->clips().first().filePath == replacementMediaPath", static_cast<bool>(replaceVideoTrack->clips().first().filePath
                   == replacementMediaPath))
            && observeTerm(131, "replaceAudioTrack->clips().first().filePath == replacementMediaPath", static_cast<bool>(replaceAudioTrack->clips().first().filePath
                   == replacementMediaPath));
        g131 ? pass("G131 replace_clip rejects a missing file")
             : fail("G131 replace_clip rejects a missing file",
                    QStringLiteral("missing replacement was accepted or mutated clips"));

        const QJsonObject replaceUndoResponse = callProjectInfoTool(
            241, QStringLiteral("undo"), QJsonObject{});
        const bool g132 = observeTerm(132, "toolPayload(replaceUndoResponse) .value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(replaceUndoResponse)
                                .value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(132, "replaceVideoTrack->clips().size() == 1", static_cast<bool>(replaceVideoTrack->clips().size() == 1))
            && observeTerm(132, "replaceAudioTrack->clips().size() == 1", static_cast<bool>(replaceAudioTrack->clips().size() == 1))
            && observeTerm(132, "replaceVideoTrack->clips().first().filePath == importSourcePath", static_cast<bool>(replaceVideoTrack->clips().first().filePath == importSourcePath))
            && observeTerm(132, "replaceAudioTrack->clips().first().filePath == importSourcePath", static_cast<bool>(replaceAudioTrack->clips().first().filePath == importSourcePath))
            && observeTerm(132, "qAbs(replaceVideoTrack->clips().first().leadInSec - 2.0) < 1e-9", static_cast<bool>(qAbs(replaceVideoTrack->clips().first().leadInSec - 2.0) < 1e-9))
            && observeTerm(132, "qAbs(replaceVideoTrack->clips().first().effectiveDuration() - 2.0) < 1e-9", static_cast<bool>(qAbs(replaceVideoTrack->clips().first().effectiveDuration() - 2.0)
                   < 1e-9))
            && observeTerm(132, "qAbs(replaceVideoTrack->clips().first().inPoint - 1.0) < 1e-9", static_cast<bool>(qAbs(replaceVideoTrack->clips().first().inPoint - 1.0) < 1e-9))
            && observeTerm(132, "!projectTimeline->undoManager()->canUndo()", static_cast<bool>(!projectTimeline->undoManager()->canUndo()));
        g132 ? pass("G132 undo restores replace_clip in one step")
             : fail("G132 undo restores replace_clip in one step",
                    QStringLiteral("one undo did not restore the linked clip pair"));

        const QJsonObject matchFrameResponse = callProjectInfoTool(
            242, QStringLiteral("match_frame"), QJsonObject{
                {QStringLiteral("timeSec"), 3.0}
            });
        const QJsonObject matchFramePayload = toolPayload(matchFrameResponse);
        const bool g133 = observeTerm(133, "!toolResult(matchFrameResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(matchFrameResponse)
                                .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(133, "matchFramePayload.value(QStringLiteral(\"filePath\")).toString() == importSourcePath", static_cast<bool>(matchFramePayload.value(QStringLiteral("filePath")).toString()
                   == importSourcePath))
            && observeTerm(133, "qAbs(matchFramePayload.value(QStringLiteral(\"sourceSec\")).toDouble() - 2.0) < 1e-9", static_cast<bool>(qAbs(matchFramePayload.value(QStringLiteral("sourceSec")).toDouble()
                    - 2.0) < 1e-9))
            && observeTerm(133, "matchFramePayload.value(QStringLiteral(\"clipIndex\")).toInt(-1) == 0", static_cast<bool>(matchFramePayload.value(QStringLiteral("clipIndex")).toInt(-1) == 0));
        g133 ? pass("G133 match_frame maps timeline offset to sourceSec")
             : fail("G133 match_frame maps timeline offset to sourceSec",
                    QStringLiteral("match_frame did not return inPoint + offset"));

        replaceVideoTrack->setClips(QVector<ClipInfo>{});
        replaceAudioTrack->setClips(QVector<ClipInfo>{});
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(),
            QStringLiteral("MCP selftest baseline"));
    } else {
        const QString reason = QStringLiteral(
            "Timeline or replacement media fixture was not available");
        fail("G130 replace_clip changes filePath and preserves start/duration", reason);
        fail("G131 replace_clip rejects a missing file", reason);
        fail("G132 undo restores replace_clip in one step", reason);
        fail("G133 match_frame maps timeline offset to sourceSec", reason);
    }

    // G140-G141: dialogue_level generates an audio volume envelope and
    // rejects non-audio targets.
    if (projectTimeline && musicRemixWavReady) {
        TimelineTrack *dialogueTrack = projectTimeline->trackAt(true, 0);
        if (dialogueTrack) {
            ClipInfo dialogueClip;
            dialogueClip.filePath = musicRemixWav.fileName();
            dialogueClip.displayName = QStringLiteral("dialogue-level-test.wav");
            dialogueClip.duration = 8.0;
            dialogueClip.outPoint = 8.0;
            dialogueTrack->setClips(QVector<ClipInfo>{dialogueClip});
            projectTimeline->clearSelection();
            projectTimeline->undoManager()->clear();
            projectTimeline->undoManager()->saveState(
                projectTimeline->currentState(),
                QStringLiteral("MCP dialogue level baseline"));

            const QJsonObject dialogueResponse = callProjectInfoTool(
                263, QStringLiteral("dialogue_level"), QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("audio")},
                    {QStringLiteral("trackIndex"), 0},
                    {QStringLiteral("clipIndex"), 0},
                    {QStringLiteral("targetLufs"), -18.0}
                });
            const QJsonObject dialoguePayload = toolPayload(dialogueResponse);
            const int pointCount = dialoguePayload
                .value(QStringLiteral("pointCount")).toInt();
            const bool applied = !toolResult(dialogueResponse)
                                      .value(QStringLiteral("isError")).toBool(true)
                && dialoguePayload.value(QStringLiteral("ok")).toBool(false)
                && pointCount > 1
                && dialogueTrack->clips().first().volumeEnvelope.size()
                       == pointCount
                && dialoguePayload.value(QStringLiteral("measuredLufsMin")).toDouble()
                       <= dialoguePayload.value(QStringLiteral("measuredLufsMax")).toDouble();
            projectTimeline->undo();
            const bool oneUndo = !projectTimeline->canUndo()
                && dialogueTrack->clips().first().volumeEnvelope.isEmpty();
            const bool g140 = observeTerm(140, "applied", static_cast<bool>(applied)) && observeTerm(140, "oneUndo", static_cast<bool>(oneUndo));
            g140 ? pass("G140 dialogue_level normal case")
                 : fail("G140 dialogue_level normal case",
                        QStringLiteral("envelope generation, response, or undo failed: %1")
                            .arg(toolErrorText(dialogueResponse)));

            const QJsonObject nonAudioResponse = callProjectInfoTool(
                264, QStringLiteral("dialogue_level"), QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("video")},
                    {QStringLiteral("trackIndex"), 0},
                    {QStringLiteral("clipIndex"), 0}
                });
            const bool g141 = observeTerm(141, "toolResult(nonAudioResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(nonAudioResponse)
                                  .value(QStringLiteral("isError")).toBool(false)))
                && observeTerm(141, "dialogueTrack->clips().first().volumeEnvelope.isEmpty()", static_cast<bool>(dialogueTrack->clips().first().volumeEnvelope.isEmpty()));
            g141 ? pass("G141 dialogue_level rejects non-audio target")
                 : fail("G141 dialogue_level rejects non-audio target",
                        QStringLiteral("video target was accepted or mutated audio"));

            dialogueTrack->setClips(QVector<ClipInfo>{});
            projectTimeline->clearSelection();
            projectTimeline->undoManager()->clear();
            projectTimeline->undoManager()->saveState(
                projectTimeline->currentState(),
                QStringLiteral("MCP selftest baseline"));
        } else {
            fail("G140 dialogue_level normal case",
                 QStringLiteral("audio track was not available"));
            fail("G141 dialogue_level rejects non-audio target",
                 QStringLiteral("audio track was not available"));
        }
    } else {
        const QString reason = QStringLiteral(
            "Timeline or generated WAV fixture was not available");
        fail("G140 dialogue_level normal case", reason);
        fail("G141 dialogue_level rejects non-audio target", reason);
    }

    // G142-G143: AUDIO-XFADE adds an audio-only set_transition kind. Keep
    // these gates in the reserved range for US-107; video set_transition
    // coverage above remains unchanged.
    if (projectTimeline) {
        TimelineTrack *audioXfadeTrack = projectTimeline->trackAt(true, 0);
        if (audioXfadeTrack) {
            audioXfadeTrack->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("audio-xfade-A"), 0),
                makeTestClip(QStringLiteral("audio-xfade-B"), 0)
            });
            projectTimeline->clearSelection();
            projectTimeline->undoManager()->clear();
            projectTimeline->undoManager()->saveState(
                projectTimeline->currentState(),
                QStringLiteral("MCP audio-xfade baseline"));

            const QJsonObject audioCrossfade = callProjectInfoTool(
                246, QStringLiteral("set_transition"), QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("audio")},
                    {QStringLiteral("trackIndex"), 0},
                    {QStringLiteral("clipIndex"), 0},
                    {QStringLiteral("type"), QStringLiteral("CrossDissolve")},
                    {QStringLiteral("durationSec"), 1.25}
                });
            const QJsonObject audioCrossfadePayload = toolPayload(audioCrossfade);
            const bool audioCrossfadeApplied =
                !toolResult(audioCrossfade).value(QStringLiteral("isError")).toBool(true)
                && audioCrossfadePayload.value(QStringLiteral("ok")).toBool(false)
                && audioCrossfadePayload.value(QStringLiteral("kind")).toString()
                       == QStringLiteral("audio")
                && audioXfadeTrack->clips().at(0).trailOut.type
                       == TransitionType::CrossDissolve
                && audioXfadeTrack->clips().at(1).leadIn.type
                       == TransitionType::CrossDissolve
                && std::abs(audioXfadeTrack->clips().at(0).trailOut.duration - 1.25) < 1e-9;
            projectTimeline->undo();
            const bool audioCrossfadeUndo =
                !projectTimeline->canUndo()
                && audioXfadeTrack->clips().at(0).trailOut.type == TransitionType::None
                && audioXfadeTrack->clips().at(1).leadIn.type == TransitionType::None;
            const bool g142 = observeTerm(142, "audioCrossfadeApplied", static_cast<bool>(audioCrossfadeApplied)) && observeTerm(142, "audioCrossfadeUndo", static_cast<bool>(audioCrossfadeUndo));
            g142 ? pass("G142 audio set_transition applies CrossDissolve and one undo")
                 : fail("G142 audio set_transition applies CrossDissolve and one undo",
                        QStringLiteral("audio transition or undo did not match"));

            const QJsonObject invalidAudioType = callProjectInfoTool(
                247, QStringLiteral("set_transition"), QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("audio")},
                    {QStringLiteral("trackIndex"), 0},
                    {QStringLiteral("clipIndex"), 0},
                    {QStringLiteral("type"), QStringLiteral("WipeLeft")},
                    {QStringLiteral("durationSec"), 1.0}
                });
            const bool g143 = observeTerm(143, "toolResult(invalidAudioType) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidAudioType)
                                  .value(QStringLiteral("isError")).toBool(false)))
                && observeTerm(143, "audioXfadeTrack->clips().at(0).trailOut.type == TransitionType::None", static_cast<bool>(audioXfadeTrack->clips().at(0).trailOut.type == TransitionType::None))
                && observeTerm(143, "audioXfadeTrack->clips().at(1).leadIn.type == TransitionType::None", static_cast<bool>(audioXfadeTrack->clips().at(1).leadIn.type == TransitionType::None));
            g143 ? pass("G143 audio set_transition rejects non-audio transition types")
                 : fail("G143 audio set_transition rejects non-audio transition types",
                        QStringLiteral("an invalid audio transition type was accepted"));

            audioXfadeTrack->setClips(QVector<ClipInfo>{});
            projectTimeline->clearSelection();
            projectTimeline->undoManager()->clear();
            projectTimeline->undoManager()->saveState(
                projectTimeline->currentState(), QStringLiteral("MCP selftest baseline"));
        } else {
            fail("G142 audio set_transition applies CrossDissolve and one undo",
                 QStringLiteral("audio track was not available"));
            fail("G143 audio set_transition rejects non-audio transition types",
                 QStringLiteral("audio track was not available"));
        }
    } else {
        fail("G142 audio set_transition applies CrossDissolve and one undo",
             QStringLiteral("Timeline was not available"));
        fail("G143 audio set_transition rejects non-audio transition types",
             QStringLiteral("Timeline was not available"));
    }

    // US-302 reserves G150-G151: edge parameters and unsupported-type warning.
    if (timelineReady) {
        auto *track = projectTimeline->trackAt(false, 0);
        const auto saved = track->clips();
        auto *audioTrack = projectTimeline->trackAt(true, 0);
        const auto savedAudio = audioTrack->clips();
        const auto savedParents = projectTimeline->clipParentEntries();
        ClipInfo a = makeTestClip(QStringLiteral("edge-A"), 0);
        ClipInfo b = makeTestClip(QStringLiteral("edge-B"), 0);
        a.duration = b.duration = 10.0;
        a.inPoint = b.inPoint = 2.0;
        a.outPoint = b.outPoint = 7.0;
        track->setClips({a, b});
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(), QStringLiteral("MCP edge baseline"));
        const auto getEdge = [&]() {
            const auto tracks = toolPayload(callProjectInfoTool(
                350, QStringLiteral("get_timeline"), QJsonObject{})).value("video").toArray();
            if (tracks.isEmpty()) return QJsonObject{};
            const auto clips = tracks.at(0).toObject().value("clips").toArray();
            return clips.isEmpty() ? QJsonObject{} : clips.at(0).toObject().value("trailOut").toObject();
        };
        QJsonObject args{{QStringLiteral("kind"), QStringLiteral("video")},
                         {QStringLiteral("clipIndex"), 0},
                         {QStringLiteral("type"), QStringLiteral("WipeLeft")},
                         {QStringLiteral("softness"), 0.5},
                         {QStringLiteral("borderWidth"), 4.0},
                         {QStringLiteral("borderColor"), QStringLiteral("#ff0000")}};
        const auto serial = projectTimeline->undoManager()->saveSerial();
        const auto response = toolPayload(callProjectInfoTool(351, QStringLiteral("set_transition"), args));
        const auto edge = getEdge();
        bool g150 = observeTerm(150, "response.value(\"ok\").toBool()", static_cast<bool>(response.value("ok").toBool())) && observeTerm(150, "edge.value(\"softness\").toDouble() == 0.5", static_cast<bool>(edge.value("softness").toDouble() == 0.5))
            && observeTerm(150, "edge.value(\"borderWidth\").toDouble() == 4.0", static_cast<bool>(edge.value("borderWidth").toDouble() == 4.0))
            && observeTerm(150, "edge.value(\"borderColor\").toString() == QStringLiteral(\"#ff0000\")", static_cast<bool>(edge.value("borderColor").toString() == QStringLiteral("#ff0000")))
            && observeTerm(150, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1));
        projectTimeline->undo();
        g150 = observeTerm(150, "g150", static_cast<bool>(g150)) && observeTerm(150, "track->clips().first().trailOut.type == TransitionType::None", static_cast<bool>(track->clips().first().trailOut.type == TransitionType::None));
        g150 ? pass("G150 transition edge parameters and one undo")
             : fail("G150 transition edge parameters and one undo", QStringLiteral("edge values or undo did not match"));
        args["type"] = QStringLiteral("CrossDissolve");
        const auto ignored = toolPayload(callProjectInfoTool(352, QStringLiteral("set_transition"), args));
        const auto ignoredEdge = getEdge();
        const bool g151 = observeTerm(151, "ignored.value(\"ok\").toBool()", static_cast<bool>(ignored.value("ok").toBool()))
            && observeTerm(151, "ignored.value(\"warning\").toString() == QStringLiteral(\"この type ではソフトネスは無視されます\")", static_cast<bool>(ignored.value("warning").toString() == QStringLiteral("この type ではソフトネスは無視されます")))
            && observeTerm(151, "ignoredEdge.value(\"softness\").toDouble(-1.0) == 0.0", static_cast<bool>(ignoredEdge.value("softness").toDouble(-1.0) == 0.0))
            && observeTerm(151, "ignoredEdge.value(\"borderWidth\").toDouble(-1.0) == 0.0", static_cast<bool>(ignoredEdge.value("borderWidth").toDouble(-1.0) == 0.0))
            && observeTerm(151, "ignoredEdge.value(\"borderColor\").toString() == QStringLiteral(\"#ffffff\")", static_cast<bool>(ignoredEdge.value("borderColor").toString() == QStringLiteral("#ffffff")))
            && observeTerm(151, "track->clips().first().trailOut.hasDefaultEdgeParams()", static_cast<bool>(track->clips().first().trailOut.hasDefaultEdgeParams()));
        g151 ? pass("G151 unsupported transition ignores edge parameters")
             : fail("G151 unsupported transition ignores edge parameters", QStringLiteral("warning or stored defaults did not match"));
        projectTimeline->setClipParentEntries(savedParents);
        track->setClips(saved);
        audioTrack->setClips(savedAudio);
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(), QStringLiteral("MCP selftest baseline"));
    } else {
        fail("G150 transition edge parameters and one undo", QStringLiteral("Timeline tracks were not available"));
        fail("G151 unsupported transition ignores edge parameters", QStringLiteral("Timeline tracks were not available"));
    }

    // US-300 reserves G148-G149: identifiers and actual playback overlap.
    if (timelineReady) {
        auto* videoTrack = projectTimeline->trackAt(false, 0);
        auto* audioTrack = projectTimeline->trackAt(true, 0);
        const auto savedVideo = videoTrack->clips();
        const auto savedAudio = audioTrack->clips();
        const auto savedParents = projectTimeline->clipParentEntries();
        ClipInfo first = makeTestClip(QStringLiteral("overlap-A"), 0);
        ClipInfo second = makeTestClip(QStringLiteral("overlap-B"), 0);
        first.duration = second.duration = 10.0;
        first.inPoint = second.inPoint = 2.0;
        first.outPoint = second.outPoint = 7.0; // Handles for all alignments.
        const auto resetOverlapFixture = [&]() {
            videoTrack->setClips({first, second});
            audioTrack->setClips({first, second});
            projectTimeline->clearSelection();
            projectTimeline->undoManager()->clear();
            projectTimeline->undoManager()->saveState(
                projectTimeline->currentState(), QStringLiteral("MCP overlap baseline"));
        };
        const auto exportedClips = [&](const QString& kind) {
            const auto response = callProjectInfoTool(
                310, QStringLiteral("get_timeline"), QJsonObject{});
            const auto tracks = toolPayload(response).value(kind).toArray();
            return tracks.isEmpty() ? QJsonArray{} : tracks.at(0).toObject()
                .value(QStringLiteral("clips")).toArray();
        };
        const auto clipObject = [](const QJsonArray& clips, int index) {
            return index < clips.size() ? clips.at(index).toObject() : QJsonObject{};
        };
        const auto overlapSeconds = [](const QJsonArray& clips, int index,
                                       const QString& edge) {
            return index < clips.size() ? clips.at(index).toObject()
                .value(QStringLiteral("overlap")).toObject().value(edge).toDouble(-1.0)
                : -1.0;
        };
        QJsonObject arguments{
            {QStringLiteral("kind"), QStringLiteral("video")},
            {QStringLiteral("trackIndex"), 0}, {QStringLiteral("clipIndex"), 0},
            {QStringLiteral("type"), QStringLiteral("CrossDissolve")},
            {QStringLiteral("durationSec"), 1.0},
            {QStringLiteral("alignment"), QStringLiteral("Start")},
            {QStringLiteral("easing"), QStringLiteral("EaseIn")}
        };
        resetOverlapFixture();
        const auto videoResponse = callProjectInfoTool(
            311, QStringLiteral("set_transition"), arguments);
        const auto videoClips = exportedClips(QStringLiteral("video"));
        const auto trail = clipObject(videoClips, 0)
            .value(QStringLiteral("trailOut")).toObject();
        const auto lead = clipObject(videoClips, 1)
            .value(QStringLiteral("leadIn")).toObject();
        bool g148 = observeTerm(148, "!toolResult(videoResponse).value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(videoResponse).value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(148, "trail.value(QStringLiteral(\"alignment\")).toString() == QStringLiteral(\"Start\")", static_cast<bool>(trail.value(QStringLiteral("alignment")).toString() == QStringLiteral("Start")))
            && observeTerm(148, "trail.value(QStringLiteral(\"easing\")).toString() == QStringLiteral(\"EaseIn\")", static_cast<bool>(trail.value(QStringLiteral("easing")).toString() == QStringLiteral("EaseIn")))
            && observeTerm(148, "lead.value(QStringLiteral(\"alignment\")).toString() == QStringLiteral(\"Start\")", static_cast<bool>(lead.value(QStringLiteral("alignment")).toString() == QStringLiteral("Start")))
            && observeTerm(148, "lead.value(QStringLiteral(\"easing\")).toString() == QStringLiteral(\"EaseIn\")", static_cast<bool>(lead.value(QStringLiteral("easing")).toString() == QStringLiteral("EaseIn")))
            && observeTerm(148, "qAbs(overlapSeconds(videoClips, 0, QStringLiteral(\"trailOutSec\")) - 1.0) < 0.001", static_cast<bool>(qAbs(overlapSeconds(videoClips, 0, QStringLiteral("trailOutSec")) - 1.0) < 0.001))
            && observeTerm(148, "qAbs(overlapSeconds(videoClips, 1, QStringLiteral(\"leadInSec\")) - 1.0) < 0.001", static_cast<bool>(qAbs(overlapSeconds(videoClips, 1, QStringLiteral("leadInSec")) - 1.0) < 0.001))
            && observeTerm(148, "overlapSeconds(videoClips, 0, QStringLiteral(\"leadInSec\")) == 0.0", static_cast<bool>(overlapSeconds(videoClips, 0, QStringLiteral("leadInSec")) == 0.0))
            && observeTerm(148, "overlapSeconds(videoClips, 1, QStringLiteral(\"trailOutSec\")) == 0.0", static_cast<bool>(overlapSeconds(videoClips, 1, QStringLiteral("trailOutSec")) == 0.0))
            && observeTerm(148, "clipObject(videoClips, 1).value(QStringLiteral(\"startSec\")).toDouble() == 5.0", static_cast<bool>(clipObject(videoClips, 1).value(QStringLiteral("startSec")).toDouble() == 5.0));
        const auto serial = projectTimeline->undoManager()->saveSerial();
        for (const QString& key : {QStringLiteral("alignment"), QStringLiteral("easing")}) {
            auto invalid = arguments;
            invalid.insert(key, QStringLiteral("invalid"));
            const auto rejected = callProjectInfoTool(312, QStringLiteral("set_transition"), invalid);
            g148 = observeTerm(148, "g148", static_cast<bool>(g148)) && observeTerm(148, "toolResult(rejected).value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(rejected).value(QStringLiteral("isError")).toBool(false)))
                && observeTerm(148, "projectTimeline->undoManager()->saveSerial() == serial", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial));
        }
        projectTimeline->undo();
        g148 = observeTerm(148, "g148", static_cast<bool>(g148)) && observeTerm(148, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()))
            && observeTerm(148, "videoTrack->clips().first().trailOut.type == TransitionType::None", static_cast<bool>(videoTrack->clips().first().trailOut.type == TransitionType::None));
        g148 ? pass("G148 transition alignment/easing and video overlap")
             : fail("G148 transition alignment/easing and video overlap",
                    QStringLiteral("identifiers, overlap, invalid input, or undo did not match"));

        resetOverlapFixture();
        arguments.insert(QStringLiteral("kind"), QStringLiteral("audio"));
        const auto audioResponse = callProjectInfoTool(
            313, QStringLiteral("set_transition"), arguments);
        const auto audioClips = exportedClips(QStringLiteral("audio"));
        bool g149 = observeTerm(149, "!toolResult(audioResponse).value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(audioResponse).value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(149, "qAbs(overlapSeconds(audioClips, 0, QStringLiteral(\"trailOutSec\")) - 1.0) < 0.001", static_cast<bool>(qAbs(overlapSeconds(audioClips, 0, QStringLiteral("trailOutSec")) - 1.0) < 0.001))
            && observeTerm(149, "qAbs(overlapSeconds(audioClips, 1, QStringLiteral(\"leadInSec\")) - 1.0) < 0.001", static_cast<bool>(qAbs(overlapSeconds(audioClips, 1, QStringLiteral("leadInSec")) - 1.0) < 0.001))
            && observeTerm(149, "overlapSeconds(audioClips, 0, QStringLiteral(\"leadInSec\")) == 0.0", static_cast<bool>(overlapSeconds(audioClips, 0, QStringLiteral("leadInSec")) == 0.0))
            && observeTerm(149, "overlapSeconds(audioClips, 1, QStringLiteral(\"trailOutSec\")) == 0.0", static_cast<bool>(overlapSeconds(audioClips, 1, QStringLiteral("trailOutSec")) == 0.0))
            && observeTerm(149, "clipObject(audioClips, 0).value(QStringLiteral(\"trailOut\")).toObject() .value(QStringLiteral(\"alignment\")).toString() == QStringLiteral(\"Start\")", static_cast<bool>(clipObject(audioClips, 0).value(QStringLiteral("trailOut")).toObject()
                   .value(QStringLiteral("alignment")).toString() == QStringLiteral("Start")))
            && observeTerm(149, "clipObject(audioClips, 1).value(QStringLiteral(\"leadIn\")).toObject() .value(QStringLiteral(\"easing\")).toString() == QStringLiteral(\"EaseIn\")", static_cast<bool>(clipObject(audioClips, 1).value(QStringLiteral("leadIn")).toObject()
                   .value(QStringLiteral("easing")).toString() == QStringLiteral("EaseIn")))
            && observeTerm(149, "videoTrack->clips().first().trailOut.type == TransitionType::None", static_cast<bool>(videoTrack->clips().first().trailOut.type == TransitionType::None));
        projectTimeline->undo();
        g149 = observeTerm(149, "g149", static_cast<bool>(g149)) && observeTerm(149, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()))
            && observeTerm(149, "audioTrack->clips().first().trailOut.type == TransitionType::None", static_cast<bool>(audioTrack->clips().first().trailOut.type == TransitionType::None))
            && observeTerm(149, "audioTrack->clips().at(1).leadIn.type == TransitionType::None", static_cast<bool>(audioTrack->clips().at(1).leadIn.type == TransitionType::None));
        const auto noOverlap = exportedClips(QStringLiteral("audio"));
        g149 = observeTerm(149, "g149", static_cast<bool>(g149)) && observeTerm(149, "overlapSeconds(noOverlap, 0, QStringLiteral(\"trailOutSec\")) == 0.0", static_cast<bool>(overlapSeconds(noOverlap, 0, QStringLiteral("trailOutSec")) == 0.0))
            && observeTerm(149, "overlapSeconds(noOverlap, 1, QStringLiteral(\"leadInSec\")) == 0.0", static_cast<bool>(overlapSeconds(noOverlap, 1, QStringLiteral("leadInSec")) == 0.0));
        // Omitting both optional arguments retains Center / Linear.
        arguments.remove(QStringLiteral("alignment"));
        arguments.remove(QStringLiteral("easing"));
        const auto defaultAudioResponse = callProjectInfoTool(
            314, QStringLiteral("set_transition"), arguments);
        const auto defaultAudioClips = exportedClips(QStringLiteral("audio"));
        const auto defaultTrail = clipObject(defaultAudioClips, 0)
            .value(QStringLiteral("trailOut")).toObject();
        g149 = observeTerm(149, "g149", static_cast<bool>(g149))
            && observeTerm(149, "!toolResult(defaultAudioResponse).value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(defaultAudioResponse).value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(149, "defaultTrail.value(QStringLiteral(\"alignment\")).toString() == QStringLiteral(\"Center\")", static_cast<bool>(defaultTrail.value(QStringLiteral("alignment")).toString() == QStringLiteral("Center")))
            && observeTerm(149, "defaultTrail.value(QStringLiteral(\"easing\")).toString() == QStringLiteral(\"Linear\")", static_cast<bool>(defaultTrail.value(QStringLiteral("easing")).toString() == QStringLiteral("Linear")))
            && observeTerm(149, "qAbs(overlapSeconds(defaultAudioClips, 0, QStringLiteral(\"trailOutSec\")) - 1.0) < 0.001", static_cast<bool>(qAbs(overlapSeconds(defaultAudioClips, 0, QStringLiteral("trailOutSec")) - 1.0) < 0.001))
            && observeTerm(149, "qAbs(overlapSeconds(defaultAudioClips, 1, QStringLiteral(\"leadInSec\")) - 1.0) < 0.001", static_cast<bool>(qAbs(overlapSeconds(defaultAudioClips, 1, QStringLiteral("leadInSec")) - 1.0) < 0.001));
        projectTimeline->undo();
        g149 = observeTerm(149, "g149", static_cast<bool>(g149)) && observeTerm(149, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()))
            && observeTerm(149, "audioTrack->clips().first().trailOut.type == TransitionType::None", static_cast<bool>(audioTrack->clips().first().trailOut.type == TransitionType::None));
        // Enable the sequence model without replacing the live track objects.
        // Legacy calls (no alignment/easing) must preserve its special carrier.
        TimelineSequence inactiveSequence;
        inactiveSequence.id = QStringLiteral("mcp-overlap-inactive");
        inactiveSequence.name = QStringLiteral("MCP overlap inactive");
        inactiveSequence.videoTracks = {{second}};
        const bool sequenceAdded = projectTimeline->addSequence(inactiveSequence);
        g149 = observeTerm(149, "g149", static_cast<bool>(g149)) && observeTerm(149, "sequenceAdded", static_cast<bool>(sequenceAdded));
        const QString sequenceStoreKey = timeline_nesting::sequenceStoreParentKey();
        for (const QString& typeName : {QStringLiteral("CrossDissolve"),
                                       QStringLiteral("FadeIn"), QStringLiteral("FadeOut")}) {
            resetOverlapFixture();
            const auto parentsBefore = projectTimeline->clipParentEntries();
            const auto sequencesBefore = projectTimeline->sequences();
            const auto activeBefore = projectTimeline->activeSequenceId();
            const auto serialBefore = projectTimeline->undoManager()->saveSerial();
            arguments.insert(QStringLiteral("type"), typeName);
            const auto response = callProjectInfoTool(
                315, QStringLiteral("set_transition"), arguments);
            const auto parentsAfter = projectTimeline->clipParentEntries();
            const auto sequencesAfter = projectTimeline->sequences();
            bool sequencesPreserved = sequencesBefore.size() >= 2
                && sequencesAfter.size() == sequencesBefore.size();
            for (int index = 0; sequencesPreserved && index < sequencesBefore.size(); ++index) {
                sequencesPreserved = sequencesAfter.at(index).id == sequencesBefore.at(index).id
                    && sequencesAfter.at(index).name == sequencesBefore.at(index).name;
            }
            // The active sequence's clips legitimately change. Inactive
            // sequence payloads must remain identical, including their clips.
            const auto storeBefore = timeline_nesting::decodeSequenceStoreObject(
                parentsBefore.value(sequenceStoreKey)).value(QStringLiteral("sequences")).toArray();
            const auto storeAfter = timeline_nesting::decodeSequenceStoreObject(
                parentsAfter.value(sequenceStoreKey)).value(QStringLiteral("sequences")).toArray();
            sequencesPreserved = sequencesPreserved
                && storeBefore.size() == sequencesBefore.size()
                && storeAfter.size() == storeBefore.size();
            for (int index = 0; sequencesPreserved && index < storeBefore.size(); ++index) {
                if (sequencesBefore.at(index).id != activeBefore)
                    sequencesPreserved = index < storeAfter.size()
                        && storeAfter.at(index) == storeBefore.at(index);
            }
            g149 = observeTerm(149, "g149", static_cast<bool>(g149)) && observeTerm(149, "!toolResult(response).value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(response).value(QStringLiteral("isError")).toBool(true)))
                && observeTerm(149, "!parentsBefore.value(sequenceStoreKey).isEmpty()", static_cast<bool>(!parentsBefore.value(sequenceStoreKey).isEmpty()))
                && observeTerm(149, "!parentsAfter.value(sequenceStoreKey).isEmpty()", static_cast<bool>(!parentsAfter.value(sequenceStoreKey).isEmpty()))
                && observeTerm(149, "!activeBefore.isEmpty()", static_cast<bool>(!activeBefore.isEmpty()))
                && observeTerm(149, "projectTimeline->activeSequenceId() == activeBefore", static_cast<bool>(projectTimeline->activeSequenceId() == activeBefore))
                && observeTerm(149, "sequencesPreserved", static_cast<bool>(sequencesPreserved))
                && observeTerm(149, "projectTimeline->undoManager()->saveSerial() == serialBefore + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serialBefore + 1));
            projectTimeline->undo();
            g149 = observeTerm(149, "g149", static_cast<bool>(g149)) && observeTerm(149, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()))
                && observeTerm(149, "projectTimeline->clipParentEntries() == parentsBefore", static_cast<bool>(projectTimeline->clipParentEntries() == parentsBefore))
                && observeTerm(149, "projectTimeline->activeSequenceId() == activeBefore", static_cast<bool>(projectTimeline->activeSequenceId() == activeBefore))
                && observeTerm(149, "projectTimeline->sequences().size() == sequencesBefore.size()", static_cast<bool>(projectTimeline->sequences().size() == sequencesBefore.size()))
                && observeTerm(149, "audioTrack->clips().first().leadIn.type == TransitionType::None", static_cast<bool>(audioTrack->clips().first().leadIn.type == TransitionType::None))
                && observeTerm(149, "audioTrack->clips().first().trailOut.type == TransitionType::None", static_cast<bool>(audioTrack->clips().first().trailOut.type == TransitionType::None))
                && observeTerm(149, "audioTrack->clips().at(1).leadIn.type == TransitionType::None", static_cast<bool>(audioTrack->clips().at(1).leadIn.type == TransitionType::None));
        }
        g149 ? pass("G149 audio overlap and one undo")
             : fail("G149 audio overlap and one undo",
                    QStringLiteral("audio overlap, identifiers, isolation, sequence store, or undo did not match"));
        projectTimeline->setClipParentEntries(savedParents);
        videoTrack->setClips(savedVideo);
        audioTrack->setClips(savedAudio);
        projectTimeline->clearSelection();
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(), QStringLiteral("MCP selftest baseline"));
    } else {
        fail("G148 transition alignment/easing and video overlap",
             QStringLiteral("Timeline tracks were not available"));
        fail("G149 audio overlap and one undo",
             QStringLiteral("Timeline tracks were not available"));
    }

    const bool selectClipFieldsPresent =
        !toolResult(successfulSelectResponse).value(QStringLiteral("isError")).toBool(false)
        && requiredOutputFieldsPresent(QStringLiteral("select_clip"),
                                       toolPayload(successfulSelectResponse));
    const bool g72 = observeTerm(72, "projectInfoFieldsPresent", static_cast<bool>(projectInfoFieldsPresent)) && observeTerm(72, "listCommandsFieldsPresent", static_cast<bool>(listCommandsFieldsPresent))
        && observeTerm(72, "selectClipFieldsPresent", static_cast<bool>(selectClipFieldsPresent));
    g72 ? pass("G72 structuredContent matches outputSchema")
        : fail("G72 structuredContent matches outputSchema",
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
    const bool g53 = observeTerm(53, "frameFixtureReady", static_cast<bool>(frameFixtureReady))
        && observeTerm(53, "!frameEnvelope.value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!frameEnvelope.value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(53, "frameContent.size() == 1", static_cast<bool>(frameContent.size() == 1))
        && observeTerm(53, "frameItem.value(QStringLiteral(\"type\")).toString() == QStringLiteral(\"image\")", static_cast<bool>(frameItem.value(QStringLiteral("type")).toString() == QStringLiteral("image")))
        && observeTerm(53, "frameItem.value(QStringLiteral(\"mimeType\")).toString() == QStringLiteral(\"image/png\")", static_cast<bool>(frameItem.value(QStringLiteral("mimeType")).toString()
               == QStringLiteral("image/png")))
        && observeTerm(53, "frameDecoded", static_cast<bool>(frameDecoded)) && observeTerm(53, "decodedFrame.width() > 0", static_cast<bool>(decodedFrame.width() > 0)) && observeTerm(53, "decodedFrame.height() > 0", static_cast<bool>(decodedFrame.height() > 0))
        && observeTerm(53, "framePayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(framePayload.value(QStringLiteral("ok")).toBool(false)))
        && observeTerm(53, "framePayload.value(QStringLiteral(\"width\")).toInt(-1) == decodedFrame.width()", static_cast<bool>(framePayload.value(QStringLiteral("width")).toInt(-1) == decodedFrame.width()))
        && observeTerm(53, "framePayload.value(QStringLiteral(\"height\")).toInt(-1) == decodedFrame.height()", static_cast<bool>(framePayload.value(QStringLiteral("height")).toInt(-1) == decodedFrame.height()))
        && observeTerm(53, "framePayload.value(QStringLiteral(\"byteSize\")).toInt(-1) == framePng.size()", static_cast<bool>(framePayload.value(QStringLiteral("byteSize")).toInt(-1)
               == framePng.size()))
        && observeTerm(53, "QJsonDocument(frameResponse).toJson(QJsonDocument::Compact).size() <= 1024 * 1024", static_cast<bool>(QJsonDocument(frameResponse).toJson(QJsonDocument::Compact).size()
               <= 1024 * 1024));
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
    const bool g54 = observeTerm(54, "g53", static_cast<bool>(g53))
        && observeTerm(54, "!narrowFrameEnvelope.value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!narrowFrameEnvelope.value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(54, "narrowFrameContent.size() == 1", static_cast<bool>(narrowFrameContent.size() == 1))
        && observeTerm(54, "narrowFrameDecoded", static_cast<bool>(narrowFrameDecoded)) && observeTerm(54, "decodedNarrowFrame.width() > 0", static_cast<bool>(decodedNarrowFrame.width() > 0))
        && observeTerm(54, "decodedNarrowFrame.height() > 0", static_cast<bool>(decodedNarrowFrame.height() > 0))
        && observeTerm(54, "decodedNarrowFrame.width() <= 32", static_cast<bool>(decodedNarrowFrame.width() <= 32))
        && observeTerm(54, "decodedNarrowFrame.width() < decodedFrame.width()", static_cast<bool>(decodedNarrowFrame.width() < decodedFrame.width()))
        && observeTerm(54, "narrowFramePayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(narrowFramePayload.value(QStringLiteral("ok")).toBool(false)))
        && observeTerm(54, "narrowFramePayload.value(QStringLiteral(\"width\")).toInt(-1) == decodedNarrowFrame.width()", static_cast<bool>(narrowFramePayload.value(QStringLiteral("width")).toInt(-1)
               == decodedNarrowFrame.width()))
        && observeTerm(54, "narrowFramePayload.value(QStringLiteral(\"height\")).toInt(-1) == decodedNarrowFrame.height()", static_cast<bool>(narrowFramePayload.value(QStringLiteral("height")).toInt(-1)
               == decodedNarrowFrame.height()));
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
    const bool g55 = observeTerm(55, "exportRootReady", static_cast<bool>(exportRootReady))
        && observeTerm(55, "missingParentEnvelope.value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(missingParentEnvelope.value(QStringLiteral("isError")).toBool(false)))
        && observeTerm(55, "missingParentPayload.isEmpty()", static_cast<bool>(missingParentPayload.isEmpty()));
    g55 ? pass("G55 export_video rejects missing parent directory")
        : fail("G55 export_video rejects missing parent directory",
               QStringLiteral("an output path below a missing directory was accepted"));

    // US-415 G162: the synchronous branch must neither submit a job nor emit video.
    {
        bool g162 = observeTerm(162, "exportRootReady", static_cast<bool>(exportRootReady)) && observeTerm(162, "frameFixtureReady", static_cast<bool>(frameFixtureReady));
        auto *queueBefore = projectInfoWindow.m_renderQueue;
        const int jobsBefore = queueBefore ? queueBefore->jobs().size() : 0;
        for (const QString &container : {QStringLiteral("m4a"), QStringLiteral("wav")}) {
            if (!exportRootReady || !frameFixtureReady) break;
            const bool wav = container == QStringLiteral("wav");
            const QString audioPath = exportRoot.filePath(QStringLiteral("audio-only.") + container);
            const auto response = callProjectInfoTool(wav ? 1622 : 1620, QStringLiteral("export_video"), QJsonObject{
                {QStringLiteral("audioOnly"), true}, {QStringLiteral("outputPath"), audioPath},
                {QStringLiteral("width"), 640}});
            const auto payload = toolPayload(response);
            g162 &= observeTerm(162, "!toolResult(response).value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(response).value(QStringLiteral("isError")).toBool(true)))
                && observeTerm(162, "payload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(payload.value(QStringLiteral("ok")).toBool(false)))
                && observeTerm(162, "payload.value(QStringLiteral(\"outputPath\")).toString() == audioPath", static_cast<bool>(payload.value(QStringLiteral("outputPath")).toString() == audioPath))
                && observeTerm(162, "!payload.contains(QStringLiteral(\"jobId\"))", static_cast<bool>(!payload.contains(QStringLiteral("jobId"))))
                && observeTerm(162, "!payload.value(QStringLiteral(\"warnings\")).toArray().isEmpty()", static_cast<bool>(!payload.value(QStringLiteral("warnings")).toArray().isEmpty()))
                && observeTerm(162, "requiredOutputFieldsPresent(QStringLiteral(\"export_video\"), payload)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("export_video"), payload)))
                && observeTerm(162, "QFileInfo(audioPath).size() > 0", static_cast<bool>(QFileInfo(audioPath).size() > 0));
            AVFormatContext *format = nullptr;
            bool audio = false, video = false, expectedCodec = false;
            if (avformat_open_input(&format, audioPath.toUtf8().constData(), nullptr, nullptr) >= 0) {
                if (avformat_find_stream_info(format, nullptr) >= 0) {
                    for (unsigned int i = 0; i < format->nb_streams; ++i) {
                        audio |= format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
                        video |= format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
                        expectedCodec |= format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
                            && format->streams[i]->codecpar->codec_id
                                == (wav ? AV_CODEC_ID_PCM_S16LE : AV_CODEC_ID_AAC);
                    }
                }
                avformat_close_input(&format);
            }
            const auto duration = libavcore::probeDurationMicroseconds(audioPath.toStdString());
            g162 &= observeTerm(162, "audio", static_cast<bool>(audio)) && observeTerm(162, "!video", static_cast<bool>(!video)) && observeTerm(162, "expectedCodec", static_cast<bool>(expectedCodec)) && observeTerm(162, "duration.has_value()", static_cast<bool>(duration.has_value()))
                && observeTerm(162, "std::abs(double(duration.value_or(0)) / 1000000.0 - projectTimeline->totalDuration()) <= 0.1", static_cast<bool>(std::abs(double(duration.value_or(0)) / 1000000.0 - projectTimeline->totalDuration()) <= 0.1));
            const auto invalid = callProjectInfoTool(wav ? 1623 : 1621, QStringLiteral("export_video"), QJsonObject{
                {QStringLiteral("audioOnly"), QStringLiteral("true")}, {QStringLiteral("outputPath"), audioPath}});
            g162 &= toolResult(invalid).value(QStringLiteral("isError")).toBool(false);
        }
        g162 &= observeTerm(162, "projectInfoWindow.m_renderQueue == queueBefore", static_cast<bool>(projectInfoWindow.m_renderQueue == queueBefore))
            && observeTerm(162, "(!queueBefore || queueBefore->jobs().size() == jobsBefore)", static_cast<bool>((!queueBefore || queueBefore->jobs().size() == jobsBefore)));
        g162 ? pass("G162 synchronous audio-only export and video-setting warning")
             : fail("G162 synchronous audio-only export and video-setting warning",
                    QStringLiteral("音声ストリーム、AAC/PCM コーデック、尺、同期応答、警告、またはキュー非起動の検証に失敗"));
    }

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
    const bool g56 = observeTerm(56, "exportRootReady", static_cast<bool>(exportRootReady)) && observeTerm(56, "timelineReady", static_cast<bool>(timelineReady))
        && observeTerm(56, "!exportEnvelope.value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!exportEnvelope.value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(56, "exportPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(exportPayload.value(QStringLiteral("ok")).toBool(false)))
        && observeTerm(56, "!exportJobId.isEmpty()", static_cast<bool>(!exportJobId.isEmpty()))
        && observeTerm(56, "exportPayload.value(QStringLiteral(\"status\")).toString() == QStringLiteral(\"queued\")", static_cast<bool>(exportPayload.value(QStringLiteral("status")).toString()
               == QStringLiteral("queued")))
        && observeTerm(56, "exportPayload.value(QStringLiteral(\"width\")).toInt(-1) == projectInfoResult.value(QStringLiteral(\"width\")).toInt(-2)", static_cast<bool>(exportPayload.value(QStringLiteral("width")).toInt(-1)
               == projectInfoResult.value(QStringLiteral("width")).toInt(-2)))
        && observeTerm(56, "exportPayload.value(QStringLiteral(\"height\")).toInt(-1) == projectInfoResult.value(QStringLiteral(\"height\")).toInt(-2)", static_cast<bool>(exportPayload.value(QStringLiteral("height")).toInt(-1)
               == projectInfoResult.value(QStringLiteral("height")).toInt(-2)))
        && observeTerm(56, "qFuzzyCompare(exportPayload.value(QStringLiteral(\"fps\")).toDouble(-1.0), projectInfoResult.value(QStringLiteral(\"fps\")).toDouble(-2.0))", static_cast<bool>(qFuzzyCompare(exportPayload.value(QStringLiteral("fps")).toDouble(-1.0),
                         projectInfoResult.value(QStringLiteral("fps")).toDouble(-2.0))))
        && observeTerm(56, "exportElapsedMs < 3000", static_cast<bool>(exportElapsedMs < 3000));
    g56 ? pass("G56 export_video returns jobId without waiting")
        : fail("G56 export_video returns jobId without waiting",
               QStringLiteral("exportRootReady=%1 (expected true), timelineReady=%2 (expected true), "
                              "isError=%3 (expected false), ok=%4 (expected true), "
                              "jobId=[%5] (expected nonempty), status=[%6] (expected queued), "
                              "width=%7 (expected %8), height=%9 (expected %10), "
                              "fps=%11 (expected %12), elapsedMs=%13 (expected <3000)")
                   .arg(exportRootReady).arg(timelineReady)
                   .arg(exportEnvelope.value(QStringLiteral("isError")).toBool(true))
                   .arg(exportPayload.value(QStringLiteral("ok")).toBool(false))
                   .arg(exportJobId)
                   .arg(exportPayload.value(QStringLiteral("status")).toString())
                   .arg(exportPayload.value(QStringLiteral("width")).toInt(-1))
                   .arg(projectInfoResult.value(QStringLiteral("width")).toInt(-2))
                   .arg(exportPayload.value(QStringLiteral("height")).toInt(-1))
                   .arg(projectInfoResult.value(QStringLiteral("height")).toInt(-2))
                   .arg(exportPayload.value(QStringLiteral("fps")).toDouble(-1.0))
                   .arg(projectInfoResult.value(QStringLiteral("fps")).toDouble(-2.0))
                   .arg(exportElapsedMs));

    // G161: observe the actual RenderJob after the deferred MCP enqueue.
    // Clear captured jobs synchronously so this argument test starts no encoder.
    {
        bool g161 = observeTerm(161, "exportRootReady", static_cast<bool>(exportRootReady)) && observeTerm(161, "timelineReady", static_cast<bool>(timelineReady));
        const QString qualityPath = exportRoot.filePath(QStringLiteral("quality.mp4"));
        const QJsonObject qualityResponse = callProjectInfoTool(
            1610, QStringLiteral("export_video"), QJsonObject{
                {QStringLiteral("outputPath"), qualityPath},
                {QStringLiteral("rateControl"), QStringLiteral("crf")},
                {QStringLiteral("crf"), 20}});
        const QString qualityId = toolPayload(qualityResponse).value(QStringLiteral("jobId")).toString();
        const bool qualityIsError = toolResult(qualityResponse).value(QStringLiteral("isError")).toBool(true);
        g161 &= observeTerm(161, "!qualityId.isEmpty()", static_cast<bool>(!qualityId.isEmpty())) && observeTerm(161, "!qualityIsError", static_cast<bool>(!qualityIsError));
        const QList<QJsonObject> invalidArguments{
            {{QStringLiteral("rateControl"), QStringLiteral("invalid")}},
            {{QStringLiteral("rateControl"), 1}},
            {{QStringLiteral("crf"), 52}},
            {{QStringLiteral("crf"), -2}},
            {{QStringLiteral("crf"), 20.5}},
            {{QStringLiteral("crf"), QStringLiteral("20")}},
            {{QStringLiteral("videoCodec"), QStringLiteral("libsvtav1")}, {QStringLiteral("crf"), 64}}
        };
        int requestId = 1611;
        QStringList invalidDetail;
        for (auto args : invalidArguments) {
            const QString argsText = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
            args.insert(QStringLiteral("outputPath"), qualityPath);
            const auto response = callProjectInfoTool(requestId++, QStringLiteral("export_video"), args);
            const bool invErr = toolResult(response).value(QStringLiteral("isError")).toBool(false);
            const bool invPayloadEmpty = toolPayload(response).isEmpty();
            const bool rejected = invErr && invPayloadEmpty;
            invalidDetail << QStringLiteral("%1:%2:isError=%3(want 1):payloadEmpty=%4(want 1)")
                                 .arg(requestId - 1).arg(argsText).arg(invErr).arg(invPayloadEmpty);
            g161 &= rejected;
        }
        auto* queue = projectInfoWindow.findChild<RenderQueue*>(QStringLiteral("mainRenderQueue"));
        QHash<QString, QJsonObject> captured;
        if (queue && queue->parent() == &projectInfoWindow) {
            const auto connection = QObject::connect(queue, &RenderQueue::jobsChanged,
                queue, [&]() {
                    const auto jobs = queue->jobs();
                    if (jobs.isEmpty()) return;
                    for (const auto& job : jobs)
                        captured.insert(job.uuid, job.exportConfig);
                    queue->clear();
                });
            // The test-owned tool instance is allowed through the lifetime guard
            // only while delivering its queued callbacks.
            auto* previousTools = projectInfoWindow.m_mcpTools;
            projectInfoWindow.m_mcpTools = &projectInfoTools;
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            projectInfoWindow.m_mcpTools = previousTools;
            QObject::disconnect(connection);
        }
        const bool queueFound = queue != nullptr;
        const bool parentOk = queue && queue->parent() == &projectInfoWindow;
        const QString qualityRate = captured.value(qualityId).value(QStringLiteral("rateControl")).toString();
        const int qualityCrf = captured.value(qualityId).value(QStringLiteral("crf")).toInt(-1);
        const QString exportRate = captured.value(exportJobId).value(QStringLiteral("rateControl")).toString(QStringLiteral("bitrate"));
        const int exportCrf = captured.value(exportJobId).value(QStringLiteral("crf")).toInt(-1);
        g161 &= observeTerm(161, "captured.contains(qualityId)", static_cast<bool>(captured.contains(qualityId))) && observeTerm(161, "captured.contains(exportJobId)", static_cast<bool>(captured.contains(exportJobId)))
            && observeTerm(161, "qualityRate == QStringLiteral(\"crf\")", static_cast<bool>(qualityRate == QStringLiteral("crf"))) && observeTerm(161, "qualityCrf == 20", static_cast<bool>(qualityCrf == 20))
            && observeTerm(161, "exportRate == QStringLiteral(\"bitrate\")", static_cast<bool>(exportRate == QStringLiteral("bitrate"))) && observeTerm(161, "exportCrf == -1", static_cast<bool>(exportCrf == -1));
        g161 ? pass("G161 export_video rate control validation and queue wiring")
             : fail("G161 export_video rate control validation and queue wiring",
                    QStringLiteral("queueFound=%1 parentOk=%2 captured.size=%3 exportRootReady=%11 timelineReady=%12 "
                                   "qualityId.empty=%13 qualityIsError=%14(want 0) invalid=[%4] "
                                   "containsQuality=%5 containsExport=%6 quality.rateControl=%7(want crf) "
                                   "quality.crf=%8(want 20) export.rateControl=%9(want bitrate) export.crf=%10(want -1)")
                        .arg(queueFound).arg(parentOk).arg(captured.size()).arg(invalidDetail.join(QLatin1Char(' ')))
                        .arg(captured.contains(qualityId)).arg(captured.contains(exportJobId))
                        .arg(qualityRate).arg(qualityCrf).arg(exportRate).arg(exportCrf)
                        .arg(exportRootReady).arg(timelineReady).arg(qualityId.isEmpty()).arg(qualityIsError));
    }

    // US-503: capture queued jobs without launching an encoder (same observation as G161).
    {
        const bool hadEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
        const QByteArray previousDir = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
        QTemporaryDir presetDir;
        qputenv("VEDITOR_EXPORT_PRESET_DIR", presetDir.path().toUtf8());
        ExportConfig userConfig;
        userConfig.width = 1280; userConfig.height = 720; userConfig.fps = 24;
        userConfig.videoCodec = "libx265"; userConfig.videoBitrate = 17000;
        userConfig.rateControl = ExportConfig::RateControl::Crf; userConfig.crf = 18;
        const QString userName = QStringLiteral("MCP/ユーザー:プリセット");
        const bool presetDirValid = presetDir.isValid();
        const bool presetSaved = presetDirValid && ExportUserPresets::save({userName, userConfig});
        bool g164 = presetSaved;
        const auto missing = callProjectInfoTool(1640, QStringLiteral("export_video"), QJsonObject{
            {"outputPath", exportRoot.filePath("missing.mp4")}, {"preset", "US503-missing"}});
        const bool missingIsError = toolResult(missing).value("isError").toBool(false);
        const bool missingPayloadEmpty = toolPayload(missing).isEmpty();
        const bool missingMessage = QJsonDocument(missing).toJson().contains(
            QStringLiteral("プリセットが見つかりません: US503-missing").toUtf8());
        g164 &= observeTerm(164, "missingIsError", static_cast<bool>(missingIsError)) && observeTerm(164, "missingPayloadEmpty", static_cast<bool>(missingPayloadEmpty)) && observeTerm(164, "missingMessage", static_cast<bool>(missingMessage));
        const QString builtinName = QStringLiteral("YouTube (1440p H.264)");
        const auto builtin = callProjectInfoTool(1630, QStringLiteral("export_video"), QJsonObject{
            {"outputPath", exportRoot.filePath("preset-builtin.mp4")}, {"preset", builtinName},
            {"rateControl", "crf"}, {"crf", 20}});
        const auto user = callProjectInfoTool(1641, QStringLiteral("export_video"), QJsonObject{
            {"outputPath", exportRoot.filePath("preset-user.mp4")}, {"preset", userName}, {"audioBitrate", 256}});
        const QString builtinId = toolPayload(builtin).value("jobId").toString();
        const QString userId = toolPayload(user).value("jobId").toString();
        QHash<QString, QJsonObject> captured;
        auto *queue = projectInfoWindow.findChild<RenderQueue*>(QStringLiteral("mainRenderQueue"));
        if (queue && queue->parent() == &projectInfoWindow) {
            const auto connection = QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                const auto jobs = queue->jobs();
                if (jobs.isEmpty()) return;
                for (const auto &job : jobs) captured.insert(job.uuid, job.exportConfig);
                queue->clear();
            });
            auto *previousTools = projectInfoWindow.m_mcpTools;
            projectInfoWindow.m_mcpTools = &projectInfoTools;
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            projectInfoWindow.m_mcpTools = previousTools;
            QObject::disconnect(connection);
        }
        const auto b = captured.value(builtinId);
        const bool queueFound = queue != nullptr;
        const bool parentOk = queue && queue->parent() == &projectInfoWindow;
        const bool g163 = observeTerm(163, "exportRootReady", static_cast<bool>(exportRootReady)) && observeTerm(163, "timelineReady", static_cast<bool>(timelineReady)) && observeTerm(163, "!builtinId.isEmpty()", static_cast<bool>(!builtinId.isEmpty()))
            && observeTerm(163, "captured.contains(builtinId)", static_cast<bool>(captured.contains(builtinId))) && observeTerm(163, "toolPayload(builtin).value(\"preset\").toString() == builtinName", static_cast<bool>(toolPayload(builtin).value("preset").toString() == builtinName))
            && observeTerm(163, "b.value(\"width\").toInt() == qMax(2, projectInfoWindow.m_projectConfig.width)", static_cast<bool>(b.value("width").toInt() == qMax(2, projectInfoWindow.m_projectConfig.width)))
            && observeTerm(163, "b.value(\"height\").toInt() == qMax(2, projectInfoWindow.m_projectConfig.height)", static_cast<bool>(b.value("height").toInt() == qMax(2, projectInfoWindow.m_projectConfig.height)))
            && observeTerm(163, "b.value(\"videoCodec\").toString() == \"libx264\"", static_cast<bool>(b.value("videoCodec").toString() == "libx264")) && observeTerm(163, "b.value(\"videoBitrate\").toInt() == 16000", static_cast<bool>(b.value("videoBitrate").toInt() == 16000))
            && observeTerm(163, "b.value(\"rateControl\").toString() == \"crf\"", static_cast<bool>(b.value("rateControl").toString() == "crf")) && observeTerm(163, "b.value(\"crf\").toInt() == 20", static_cast<bool>(b.value("crf").toInt() == 20));
        const auto u = captured.value(userId);
        g164 &= observeTerm(164, "captured.size() == 2", static_cast<bool>(captured.size() == 2)) && observeTerm(164, "!userId.isEmpty()", static_cast<bool>(!userId.isEmpty())) && observeTerm(164, "captured.contains(userId)", static_cast<bool>(captured.contains(userId)))
            && observeTerm(164, "toolPayload(user).value(\"preset\").toString() == userName", static_cast<bool>(toolPayload(user).value("preset").toString() == userName))
            && observeTerm(164, "u.value(\"width\").toInt() == 1280", static_cast<bool>(u.value("width").toInt() == 1280)) && observeTerm(164, "u.value(\"height\").toInt() == 720", static_cast<bool>(u.value("height").toInt() == 720))
            && observeTerm(164, "u.value(\"fps\").toInt() == 24", static_cast<bool>(u.value("fps").toInt() == 24)) && observeTerm(164, "u.value(\"videoCodec\").toString() == \"libx265\"", static_cast<bool>(u.value("videoCodec").toString() == "libx265"))
            && observeTerm(164, "u.value(\"videoBitrate\").toInt() == 17000", static_cast<bool>(u.value("videoBitrate").toInt() == 17000)) && observeTerm(164, "u.value(\"audioBitrate\").toInt() == 256", static_cast<bool>(u.value("audioBitrate").toInt() == 256))
            && observeTerm(164, "u.value(\"rateControl\").toString() == \"crf\"", static_cast<bool>(u.value("rateControl").toString() == "crf")) && observeTerm(164, "u.value(\"crf\").toInt() == 18", static_cast<bool>(u.value("crf").toInt() == 18));
        g163 ? pass("G163 built-in export preset and explicit overrides")
             : fail("G163 built-in export preset and explicit overrides",
                    QStringLiteral("queueFound=%1 parentOk=%2 captured.size=%3 exportRootReady=%4 timelineReady=%5 "
                                   "builtinId.empty=%6 contains=%7 preset=%8(want %9) width=%10(want %11) height=%12(want %13) "
                                   "videoCodec=%14(want libx264) videoBitrate=%15(want 16000) rateControl=%16(want crf) crf=%17(want 20)")
                        .arg(queueFound).arg(parentOk).arg(captured.size()).arg(exportRootReady).arg(timelineReady)
                        .arg(builtinId.isEmpty()).arg(captured.contains(builtinId))
                        .arg(toolPayload(builtin).value("preset").toString(), builtinName)
                        .arg(b.value("width").toInt()).arg(qMax(2, projectInfoWindow.m_projectConfig.width))
                        .arg(b.value("height").toInt()).arg(qMax(2, projectInfoWindow.m_projectConfig.height))
                        .arg(b.value("videoCodec").toString(), QString::number(b.value("videoBitrate").toInt()))
                        .arg(b.value("rateControl").toString()).arg(b.value("crf").toInt()));
        g164 ? pass("G164 user export preset and missing preset rejection")
             : fail("G164 user export preset and missing preset rejection",
                    QStringLiteral("queueFound=%1 parentOk=%2 presetDirValid=%3 presetSaved=%4 missingIsError=%5(want 1) "
                                   "missingPayloadEmpty=%6(want 1) missingMessage=%7(want 1) captured.size=%8(want 2) "
                                   "userId.empty=%9 contains=%10 preset=%11(want %12) width=%13(want 1280) height=%14(want 720) "
                                   "fps=%15(want 24) videoCodec=%16(want libx265) videoBitrate=%17(want 17000) "
                                   "audioBitrate=%18(want 256) rateControl=%19(want crf) crf=%20(want 18)")
                        .arg(queueFound).arg(parentOk).arg(presetDirValid).arg(presetSaved).arg(missingIsError)
                        .arg(missingPayloadEmpty).arg(missingMessage).arg(captured.size())
                        .arg(userId.isEmpty()).arg(captured.contains(userId))
                        .arg(toolPayload(user).value("preset").toString(), userName)
                        .arg(u.value("width").toInt()).arg(u.value("height").toInt()).arg(u.value("fps").toInt())
                        .arg(u.value("videoCodec").toString()).arg(u.value("videoBitrate").toInt())
                        .arg(u.value("audioBitrate").toInt()).arg(u.value("rateControl").toString())
                        .arg(u.value("crf").toInt()));
        if (hadEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", previousDir);
        else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
    }

    // US-504 G165: validate before enqueue, then inspect the real deferred job.
    {
        bool ok = observeTerm(165, "exportRootReady", static_cast<bool>(exportRootReady)) && observeTerm(165, "timelineReady", static_cast<bool>(timelineReady));
        const QString path = exportRoot.filePath("alpha.mov");
        const QJsonObject valid{{"outputPath", path}, {"keepAlpha", true},
            {"videoCodec", "prores"}, {"proresProfile", 4}};
        const auto response = callProjectInfoTool(1650, QStringLiteral("export_video"), valid);
        const QString id = toolPayload(response).value("jobId").toString();
        const bool validIsError = toolResult(response).value("isError").toBool(true);
        ok &= observeTerm(165, "!id.isEmpty()", static_cast<bool>(!id.isEmpty())) && observeTerm(165, "!validIsError", static_cast<bool>(!validIsError));
        const QList<QJsonObject> invalid{
            {{"videoCodec", "h264"}}, {{"proresProfile", 2}},
            {{"keepAlpha", "true"}}, {{"keepAlpha", 1}},
            {{"proresProfile", -1}}, {{"proresProfile", 6}},
            {{"proresProfile", 4.5}}, {{"proresProfile", "4"}},
            {{"audioOnly", true}}
        };
        int requestId = 1651;
        QStringList invalidDetail;
        for (const auto &overrides : invalid) {
            auto args = valid;
            QStringList keys;
            for (auto it = overrides.begin(); it != overrides.end(); ++it) {
                args.insert(it.key(), it.value());
                keys << QStringLiteral("%1=%2").arg(it.key(), it.value().toVariant().toString());
            }
            const auto rejected = callProjectInfoTool(requestId++, QStringLiteral("export_video"), args);
            const bool invIsError = toolResult(rejected).value("isError").toBool(false);
            const bool invPayloadEmpty = toolPayload(rejected).isEmpty();
            invalidDetail << QStringLiteral("%1:isError=%2,payloadEmpty=%3")
                                 .arg(keys.join(QLatin1Char(','))).arg(invIsError).arg(invPayloadEmpty);
            ok &= observeTerm(165, "invIsError", static_cast<bool>(invIsError)) && observeTerm(165, "invPayloadEmpty", static_cast<bool>(invPayloadEmpty));
        }
        QHash<QString, QJsonObject> captured;
        auto *queue = projectInfoWindow.findChild<RenderQueue*>(QStringLiteral("mainRenderQueue"));
        if (queue && queue->parent() == &projectInfoWindow) {
            const auto connection = QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                const auto jobs = queue->jobs();
                if (jobs.isEmpty()) return;
                for (const auto &job : jobs) captured.insert(job.uuid, job.exportConfig);
                queue->clear();
            });
            auto *previousTools = projectInfoWindow.m_mcpTools;
            projectInfoWindow.m_mcpTools = &projectInfoTools;
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            projectInfoWindow.m_mcpTools = previousTools;
            QObject::disconnect(connection);
        }
        const bool queueFound = queue != nullptr;
        const bool parentOk = queue && queue->parent() == &projectInfoWindow;
        ok &= observeTerm(165, "captured.size() == 1", static_cast<bool>(captured.size() == 1)) && observeTerm(165, "captured.contains(id)", static_cast<bool>(captured.contains(id)))
            && observeTerm(165, "captured.value(id).value(\"keepAlpha\").toBool(false)", static_cast<bool>(captured.value(id).value("keepAlpha").toBool(false)))
            && observeTerm(165, "captured.value(id).value(\"proresProfile\").toInt(-1) == 4", static_cast<bool>(captured.value(id).value("proresProfile").toInt(-1) == 4));
        ok ? pass("G165 alpha export validation and queue wiring")
           : fail("G165 alpha export validation and queue wiring",
                  QStringLiteral("queueFound=%1 parentOk=%2 captured.size=%3(want 1) exportRootReady=%4 timelineReady=%5 id.empty=%6 "
                                 "validIsError=%7(want 0) invalid=[%8] contains=%9 "
                                 "keepAlpha=%10(want 1) proresProfile=%11(want 4)")
                      .arg(queueFound).arg(parentOk).arg(captured.size()).arg(exportRootReady).arg(timelineReady)
                      .arg(id.isEmpty()).arg(validIsError).arg(invalidDetail.join(QLatin1Char(' ')))
                      .arg(captured.contains(id)).arg(captured.value(id).value("keepAlpha").toBool(false))
                      .arg(captured.value(id).value("proresProfile").toInt(-1)));
    }

    // US-612 G174: the real MCP handler shares the alpha export policy.
    {
        bool ok = observeTerm(174, "exportRootReady", static_cast<bool>(exportRootReady)) && observeTerm(174, "timelineReady", static_cast<bool>(timelineReady));
        const QJsonObject valid{{"outputPath", exportRoot.filePath("alpha-vp9.webm")},
            {"keepAlpha", true}, {"videoCodec", "libvpx-vp9"}, {"audioCodec", "libopus"}};
        const auto response = callProjectInfoTool(1740, QStringLiteral("export_video"), valid);
        const QString id = toolPayload(response).value("jobId").toString();
        const bool validIsError = toolResult(response).value("isError").toBool(true);
        ok &= observeTerm(174, "!id.isEmpty()", static_cast<bool>(!id.isEmpty())) && observeTerm(174, "!validIsError", static_cast<bool>(!validIsError));
        auto invalid = valid;
        invalid.insert("videoCodec", "libx264");
        invalid.insert("outputPath", exportRoot.filePath("alpha-h264.mp4"));
        const auto rejected = callProjectInfoTool(1741, QStringLiteral("export_video"), invalid);
        const bool h264IsError = toolResult(rejected).value("isError").toBool(false);
        const bool h264PayloadEmpty = toolPayload(rejected).isEmpty();
        ok &= observeTerm(174, "h264IsError", static_cast<bool>(h264IsError)) && observeTerm(174, "h264PayloadEmpty", static_cast<bool>(h264PayloadEmpty));
        QHash<QString, QJsonObject> captured;
        auto *queue = projectInfoWindow.findChild<RenderQueue*>(QStringLiteral("mainRenderQueue"));
        if (queue && queue->parent() == &projectInfoWindow) {
            const auto connection = QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                const auto jobs = queue->jobs();
                if (jobs.isEmpty()) return;
                for (const auto &job : jobs) captured.insert(job.uuid, job.exportConfig);
                queue->clear();
            });
            auto *previousTools = projectInfoWindow.m_mcpTools;
            projectInfoWindow.m_mcpTools = &projectInfoTools;
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            projectInfoWindow.m_mcpTools = previousTools;
            QObject::disconnect(connection);
        }
        const bool queueFound = queue != nullptr;
        const bool parentOk = queue && queue->parent() == &projectInfoWindow;
        ok &= observeTerm(174, "captured.size() == 1", static_cast<bool>(captured.size() == 1)) && observeTerm(174, "captured.contains(id)", static_cast<bool>(captured.contains(id)))
            && observeTerm(174, "captured.value(id).value(\"keepAlpha\").toBool(false)", static_cast<bool>(captured.value(id).value("keepAlpha").toBool(false)))
            && observeTerm(174, "captured.value(id).value(\"videoCodec\").toString() == QStringLiteral(\"libvpx-vp9\")", static_cast<bool>(captured.value(id).value("videoCodec").toString() == QStringLiteral("libvpx-vp9")));
        ok ? pass("G174 VP9 alpha acceptance and H.264 rejection")
           : fail("G174 VP9 alpha acceptance and H.264 rejection",
                  QStringLiteral("queueFound=%1 parentOk=%2 captured.size=%3(want 1) exportRootReady=%4 timelineReady=%5 id.empty=%6 "
                                 "validIsError=%7(want 0) h264IsError=%8(want 1) h264PayloadEmpty=%9(want 1) contains=%10 "
                                 "keepAlpha=%11(want 1) videoCodec=%12(want libvpx-vp9)")
                      .arg(queueFound).arg(parentOk).arg(captured.size()).arg(exportRootReady).arg(timelineReady)
                      .arg(id.isEmpty()).arg(validIsError).arg(h264IsError).arg(h264PayloadEmpty)
                      .arg(captured.contains(id)).arg(captured.value(id).value("keepAlpha").toBool(false))
                      .arg(captured.value(id).value("videoCodec").toString()));
    }

    // US-614 G175: validate nested arguments and capture the actual queued job.
    {
        const QJsonObject metadata{{"title", "題名"}, {"author", "作成者"},
            {"copyright", "権利"}, {"comment", "コメント"}};
        QJsonObject args{{"outputPath", exportRoot.filePath("metadata.mp4")}, {"metadata", metadata}};
        const auto response = callProjectInfoTool(1750, QStringLiteral("export_video"), args);
        const QString id = toolPayload(response).value("jobId").toString();
        const bool validIsError = toolResult(response).value("isError").toBool(true);
        bool ok = observeTerm(175, "exportRootReady", static_cast<bool>(exportRootReady)) && observeTerm(175, "timelineReady", static_cast<bool>(timelineReady)) && observeTerm(175, "!id.isEmpty()", static_cast<bool>(!id.isEmpty())) && observeTerm(175, "!validIsError", static_cast<bool>(!validIsError));
        QStringList invalidDetail;
        for (const QJsonValue &invalid : {QJsonValue("invalid"), QJsonValue(QJsonObject{{"unknown", "x"}}),
                QJsonValue(QJsonObject{{"title", 42}})}) {
            args.insert("metadata", invalid);
            const bool rejected = toolResult(callProjectInfoTool(1751, QStringLiteral("export_video"), args))
                .value("isError").toBool(false);
            invalidDetail << QStringLiteral("metadata=%1:rejected=%2")
                .arg(invalid.isObject()
                         ? QString::fromUtf8(QJsonDocument(invalid.toObject()).toJson(QJsonDocument::Compact))
                         : invalid.toVariant().toString())
                .arg(rejected);
            ok &= rejected;
        }
        args.insert("metadata", metadata);
        args.insert("unknownMetadata", "x");
        const bool unknownRejected = toolResult(callProjectInfoTool(1752, QStringLiteral("export_video"), args))
            .value("isError").toBool(false);
        ok &= unknownRejected;
        QHash<QString, QJsonObject> captured;
        auto *queue = projectInfoWindow.findChild<RenderQueue*>(QStringLiteral("mainRenderQueue"));
        if (queue && queue->parent() == &projectInfoWindow) {
            const auto connection = QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                const auto jobs = queue->jobs();
                if (jobs.isEmpty()) return;
                for (const auto &job : jobs) captured.insert(job.uuid, job.exportConfig);
                queue->clear();
            });
            auto *previousTools = projectInfoWindow.m_mcpTools;
            projectInfoWindow.m_mcpTools = &projectInfoTools;
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            projectInfoWindow.m_mcpTools = previousTools;
            QObject::disconnect(connection);
        }
        const bool queueFound = queue != nullptr;
        const bool parentOk = queue && queue->parent() == &projectInfoWindow;
        ok &= observeTerm(175, "captured.size() == 1", static_cast<bool>(captured.size() == 1)) && observeTerm(175, "captured.contains(id)", static_cast<bool>(captured.contains(id)));
        QStringList metadataDetail;
        for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) {
            ok &= captured.value(id).value(it.key()) == it.value();
            metadataDetail << QStringLiteral("%1=%2(want %3)").arg(it.key(),
                captured.value(id).value(it.key()).toString(), it.value().toString());
        }
        ok ? pass("G175 export metadata")
           : fail("G175 export metadata",
                  QStringLiteral("queueFound=%1 parentOk=%2 captured.size=%3(want 1) exportRootReady=%4 timelineReady=%5 id.empty=%6 "
                                 "validIsError=%7(want 0) invalid=[%8] unknownMetadataRejected=%9(want 1) contains=%10 %11")
                      .arg(queueFound).arg(parentOk).arg(captured.size()).arg(exportRootReady).arg(timelineReady)
                      .arg(id.isEmpty()).arg(validIsError).arg(invalidDetail.join(QLatin1Char(' ')))
                      .arg(unknownRejected).arg(captured.contains(id)).arg(metadataDetail.join(QLatin1Char(' '))));
    }

    // US-616 G176: exercise the registered import tool and restore the fixture.
    {
        QTemporaryDir edlDir;
        const QString path = edlDir.filePath(QStringLiteral("import.edl"));
        QFile file(path);
        const QByteArray contents("TITLE: MCP EDL\nFCM: NON-DROP FRAME\n"
            "001 MISSING V C 00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n");
        bool ok = observeTerm(176, "edlDir.isValid()", static_cast<bool>(edlDir.isValid())) && observeTerm(176, "file.open(QIODevice::WriteOnly)", static_cast<bool>(file.open(QIODevice::WriteOnly)));
        if (ok) ok = file.write(contents) == contents.size();
        file.close();
        const QString beforeId = projectTimeline ? projectTimeline->activeSequenceId() : QString();
        const int beforeCount = projectTimeline ? projectTimeline->sequenceList().size() : 0;
        const auto response = callProjectInfoTool(1760, QStringLiteral("import_edl"), {{"filePath", path}});
        const bool created = !toolResult(response).value("isError").toBool(true);
        ok &= observeTerm(176, "created", static_cast<bool>(created)) && observeTerm(176, "projectTimeline", static_cast<bool>(projectTimeline)) && observeTerm(176, "projectTimeline->sequenceList().size() == beforeCount + 1", static_cast<bool>(projectTimeline->sequenceList().size() == beforeCount + 1))
            && observeTerm(176, "projectTimeline->activeSequenceId() != beforeId", static_cast<bool>(projectTimeline->activeSequenceId() != beforeId))
            && observeTerm(176, "toolPayload(response).value(\"unresolvedFiles\").toInt() == 1", static_cast<bool>(toolPayload(response).value("unresolvedFiles").toInt() == 1));
        const QString afterId = projectTimeline ? projectTimeline->activeSequenceId() : QString();
        const auto rejected = callProjectInfoTool(1761, QStringLiteral("import_edl"),
            {{"filePath", edlDir.filePath(QStringLiteral("absent.edl"))}});
        ok &= observeTerm(176, "toolResult(rejected).value(\"isError\").toBool(false)", static_cast<bool>(toolResult(rejected).value("isError").toBool(false)))
            && observeTerm(176, "projectTimeline", static_cast<bool>(projectTimeline)) && observeTerm(176, "projectTimeline->activeSequenceId() == afterId", static_cast<bool>(projectTimeline->activeSequenceId() == afterId));
        if (created && projectTimeline) projectTimeline->undo();
        ok &= observeTerm(176, "projectTimeline", static_cast<bool>(projectTimeline)) && observeTerm(176, "projectTimeline->activeSequenceId() == beforeId", static_cast<bool>(projectTimeline->activeSequenceId() == beforeId))
            && observeTerm(176, "projectTimeline->sequenceList().size() == beforeCount", static_cast<bool>(projectTimeline->sequenceList().size() == beforeCount));
        ok ? pass("G176 import_edl creates a sequence and rejects missing paths")
           : fail("G176 import_edl creates a sequence and rejects missing paths", QStringLiteral("import or undo mismatch"));
    }

    // US-617 G177: exercise the registered import tool and restore the fixture.
    {
        QTemporaryDir fcpxmlDir;
        const QString path = fcpxmlDir.filePath(QStringLiteral("import.fcpxml"));
        QFile file(path);
        const QByteArray contents("<fcpxml><resources><asset id=\"a\" src=\"missing.mov\"/></resources>"
            "<project name=\"MCP FCPXML\"><sequence><spine><asset-clip ref=\"a\" duration=\"1s\"/>"
            "</spine></sequence></project></fcpxml>");
        bool ok = observeTerm(177, "fcpxmlDir.isValid()", static_cast<bool>(fcpxmlDir.isValid())) && observeTerm(177, "file.open(QIODevice::WriteOnly)", static_cast<bool>(file.open(QIODevice::WriteOnly)));
        if (ok) ok = file.write(contents) == contents.size();
        file.close();
        const QString beforeId = projectTimeline ? projectTimeline->activeSequenceId() : QString();
        const int beforeCount = projectTimeline ? projectTimeline->sequenceList().size() : 0;
        const auto response = callProjectInfoTool(1770, QStringLiteral("import_fcpxml"), {{"filePath", path}});
        const bool created = !toolResult(response).value("isError").toBool(true);
        ok &= observeTerm(177, "created", static_cast<bool>(created)) && observeTerm(177, "projectTimeline", static_cast<bool>(projectTimeline)) && observeTerm(177, "projectTimeline->sequenceList().size() == beforeCount + 1", static_cast<bool>(projectTimeline->sequenceList().size() == beforeCount + 1))
            && observeTerm(177, "projectTimeline->activeSequenceId() != beforeId", static_cast<bool>(projectTimeline->activeSequenceId() != beforeId))
            && observeTerm(177, "toolPayload(response).value(\"unresolvedFiles\").toInt() == 1", static_cast<bool>(toolPayload(response).value("unresolvedFiles").toInt() == 1));
        const QString afterId = projectTimeline ? projectTimeline->activeSequenceId() : QString();
        QFile invalid(fcpxmlDir.filePath(QStringLiteral("invalid.fcpxml")));
        ok &= invalid.open(QIODevice::WriteOnly);
        ok &= invalid.write("<fcpxml><project>") == 17;
        invalid.close();
        const auto serial = projectTimeline ? projectTimeline->undoManager()->saveSerial() : 0;
        const auto rejected = callProjectInfoTool(1771, QStringLiteral("import_fcpxml"),
            {{"filePath", invalid.fileName()}});
        ok &= observeTerm(177, "toolResult(rejected).value(\"isError\").toBool(false)", static_cast<bool>(toolResult(rejected).value("isError").toBool(false)))
            && observeTerm(177, "projectTimeline", static_cast<bool>(projectTimeline)) && observeTerm(177, "projectTimeline->activeSequenceId() == afterId", static_cast<bool>(projectTimeline->activeSequenceId() == afterId))
            && observeTerm(177, "projectTimeline->undoManager()->saveSerial() == serial", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial));
        if (created && projectTimeline) projectTimeline->undo();
        ok &= observeTerm(177, "projectTimeline", static_cast<bool>(projectTimeline)) && observeTerm(177, "projectTimeline->activeSequenceId() == beforeId", static_cast<bool>(projectTimeline->activeSequenceId() == beforeId))
            && observeTerm(177, "projectTimeline->sequenceList().size() == beforeCount", static_cast<bool>(projectTimeline->sequenceList().size() == beforeCount));
        ok ? pass("G177 import_fcpxml creates a sequence and rejects malformed XML")
           : fail("G177 import_fcpxml creates a sequence and rejects malformed XML", QStringLiteral("import or undo mismatch"));
    }

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
    const bool g57 = observeTerm(57, "g56", static_cast<bool>(g56))
        && observeTerm(57, "!knownStatusEnvelope.value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!knownStatusEnvelope.value(QStringLiteral("isError")).toBool(true)))
        && observeTerm(57, "knownStatusPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(knownStatusPayload.value(QStringLiteral("ok")).toBool(false)))
        && observeTerm(57, "validExportStatuses.contains( knownStatusPayload.value(QStringLiteral(\"status\")).toString())", static_cast<bool>(validExportStatuses.contains(
               knownStatusPayload.value(QStringLiteral("status")).toString())))
        && observeTerm(57, "knownProgress >= 0", static_cast<bool>(knownProgress >= 0)) && observeTerm(57, "knownProgress <= 100", static_cast<bool>(knownProgress <= 100));
    g57 ? pass("G57 get_export_status returns job state")
        : fail("G57 get_export_status returns job state",
               QStringLiteral("the queued export status was not readable"));

    const QJsonObject unknownStatusResponse = callProjectInfoTool(
        62, QStringLiteral("get_export_status"), QJsonObject{
            {QStringLiteral("jobId"), QStringLiteral("unknown-mcp-job")}
        });
    const QJsonObject unknownStatusEnvelope = toolResult(unknownStatusResponse);
    const QJsonObject unknownStatusPayload = toolPayload(unknownStatusResponse);
    const bool g58 = observeTerm(58, "unknownStatusEnvelope.value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(unknownStatusEnvelope.value(QStringLiteral("isError")).toBool(false)))
        && observeTerm(58, "unknownStatusPayload.isEmpty()", static_cast<bool>(unknownStatusPayload.isEmpty()));
    g58 ? pass("G58 unknown export job is rejected")
        : fail("G58 unknown export job is rejected",
               QStringLiteral("an unknown jobId did not return isError"));

    TimelineTrack *captionVideo0 = (projectTimeline
        && !projectTimeline->videoTracks().isEmpty())
        ? projectTimeline->videoTracks().first() : nullptr;
    if (!captionVideo0) {
        fail("G59 add_caption without open editor",
             QStringLiteral("V1 track was not available"));
        fail("G60 apply_captions writes timeline overlays",
             QStringLiteral("V1 track was not available"));
        fail("G61 apply_captions rejects empty editor",
             QStringLiteral("V1 track was not available"));
        fail("G62 exclusive write guard rejects nested write",
             QStringLiteral("V1 track was not available"));
        fail("G63 run_command nests through the guard",
             QStringLiteral("V1 track was not available"));
        fail("G64 delete_clip resynchronizes selection",
             QStringLiteral("V1 track was not available"));
        fail("G65 set_playhead seeks the player",
             QStringLiteral("V1 track was not available"));
        fail("G83 set_clip_property changes and undoes a live clip",
             QStringLiteral("V1 track was not available"));
    } else {
        const bool noCaptionEditorBefore =
            projectInfoWindow.findChild<CaptionEditorDialog*>(QStringLiteral("captionEditorDialog")) == nullptr;
        const QJsonObject addCaptionResponse = callProjectInfoTool(
            63, QStringLiteral("add_caption"), QJsonObject{
                {QStringLiteral("text"), QStringLiteral("hello world")},
                {QStringLiteral("startSec"), 0.5},
                {QStringLiteral("endSec"), 1.5}
            });
        const QJsonObject addCaptionPayload = toolPayload(addCaptionResponse);
        CaptionEditorDialog *captionDialog =
            projectInfoWindow.findChild<CaptionEditorDialog*>(QStringLiteral("captionEditorDialog"));
        const bool addCaptionIsError = toolResult(addCaptionResponse).value(QStringLiteral("isError"))
                                           .toBool(false);
        const bool addCaptionOk = addCaptionPayload.value(QStringLiteral("ok")).toBool(false);
        const int addCaptionCount = addCaptionPayload.value(QStringLiteral("captionCount")).toInt(-1);
        const bool captionDialogFound = captionDialog != nullptr;
        const bool captionDialogVisible = captionDialog && captionDialog->isVisible();
        const bool g59 = observeTerm(59, "noCaptionEditorBefore", static_cast<bool>(noCaptionEditorBefore)) && observeTerm(59, "!addCaptionIsError", static_cast<bool>(!addCaptionIsError)) && observeTerm(59, "addCaptionOk", static_cast<bool>(addCaptionOk))
            && observeTerm(59, "addCaptionCount == 1", static_cast<bool>(addCaptionCount == 1)) && observeTerm(59, "captionDialogFound", static_cast<bool>(captionDialogFound)) && observeTerm(59, "!captionDialogVisible", static_cast<bool>(!captionDialogVisible));
        g59 ? pass("G59 add_caption without open editor")
            : fail("G59 add_caption without open editor",
                   QStringLiteral("noCaptionEditorBefore=%1 isError=%2(want 0) ok=%3(want 1) "
                                  "captionCount=%4(want 1) dialogFound=%5 visible=%6(want 0)")
                       .arg(noCaptionEditorBefore).arg(addCaptionIsError).arg(addCaptionOk)
                       .arg(addCaptionCount).arg(captionDialogFound).arg(captionDialogVisible));

        ClipInfo captionHost;
        captionHost.filePath = QStringLiteral("caption-host");
        captionHost.displayName = QStringLiteral("caption-host");
        captionHost.duration = 5.0;
        captionHost.outPoint = 5.0;
        captionVideo0->setClips(QVector<ClipInfo>{captionHost});
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(
            projectTimeline->currentState(), QStringLiteral("baseline"));

        const QJsonObject applyCaptionResponse = callProjectInfoTool(
            64, QStringLiteral("apply_captions"), QJsonObject{});
        const QJsonObject applyCaptionPayload = toolPayload(applyCaptionResponse);
        const QJsonObject captionsAfterApply = callProjectInfoTool(
            66, QStringLiteral("get_captions"), QJsonObject{});
        const QJsonObject captionsAfterApplyPayload = toolPayload(captionsAfterApply);
        const QJsonArray timelineCaptions = captionsAfterApplyPayload
            .value(QStringLiteral("timelineCaptions")).toArray();
        const bool timelineCaptionContents = timelineCaptions.size() == 2
            && timelineCaptions.at(0).toObject().value(QStringLiteral("text"))
                   .toString() == QStringLiteral("hello");
        const QJsonObject captionUndoResponse = callProjectInfoTool(
            67, QStringLiteral("undo"), QJsonObject{});
        const bool captionUndoRestored = toolPayload(captionUndoResponse)
            .value(QStringLiteral("ok")).toBool(false)
            && projectTimeline->generatedCaptionOverlays().isEmpty();
        const QJsonObject captionRedoResponse = callProjectInfoTool(
            68, QStringLiteral("redo"), QJsonObject{});
        const bool captionRedoRestored = toolPayload(captionRedoResponse)
            .value(QStringLiteral("ok")).toBool(false)
            && projectTimeline->generatedCaptionOverlays().size() == 2;
        const bool g60 = observeTerm(60, "!toolResult(applyCaptionResponse) .value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(applyCaptionResponse)
                              .value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(60, "applyCaptionPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(applyCaptionPayload.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(60, "applyCaptionPayload.value(QStringLiteral(\"appliedCount\")).toInt(-1) == 2", static_cast<bool>(applyCaptionPayload.value(QStringLiteral("appliedCount")).toInt(-1) == 2))
            && observeTerm(60, "projectTimeline->generatedCaptionOverlays().size() == 2", static_cast<bool>(projectTimeline->generatedCaptionOverlays().size() == 2))
            && observeTerm(60, "captionsAfterApplyPayload.value(QStringLiteral(\"timelineCaptionCount\")) .toInt(-1) == 2", static_cast<bool>(captionsAfterApplyPayload.value(QStringLiteral("timelineCaptionCount"))
                   .toInt(-1) == 2))
            && observeTerm(60, "timelineCaptionContents", static_cast<bool>(timelineCaptionContents))
            && observeTerm(60, "captionUndoRestored", static_cast<bool>(captionUndoRestored)) && observeTerm(60, "captionRedoRestored", static_cast<bool>(captionRedoRestored));
        g60 ? pass("G60 apply_captions writes timeline overlays")
            : fail("G60 apply_captions writes timeline overlays",
                   QStringLiteral("caption overlays, get_captions, or undo/redo did not match"));

        if (captionDialog)
            captionDialog->setTrack(caption::Track{});
        const QJsonObject emptyApplyResponse = callProjectInfoTool(
            69, QStringLiteral("apply_captions"), QJsonObject{});
        const QJsonObject refillCaptionResponse = callProjectInfoTool(
            70, QStringLiteral("add_caption"), QJsonObject{
                {QStringLiteral("text"), QStringLiteral("hello world")},
                {QStringLiteral("startSec"), 0.5},
                {QStringLiteral("endSec"), 1.5}
            });
        const bool refillCaptionSucceeded = toolPayload(refillCaptionResponse)
            .value(QStringLiteral("ok")).toBool(false)
            && toolPayload(refillCaptionResponse)
                   .value(QStringLiteral("captionCount")).toInt(-1) == 1;
        const bool g61 = observeTerm(61, "captionDialog != nullptr", static_cast<bool>(captionDialog != nullptr))
            && observeTerm(61, "toolResult(emptyApplyResponse).value(QStringLiteral(\"isError\")) .toBool(false)", static_cast<bool>(toolResult(emptyApplyResponse).value(QStringLiteral("isError"))
                   .toBool(false)))
            && observeTerm(61, "toolErrorText(emptyApplyResponse).contains(QStringLiteral(\"add_caption\"))", static_cast<bool>(toolErrorText(emptyApplyResponse).contains(QStringLiteral("add_caption"))))
            && observeTerm(61, "refillCaptionSucceeded", static_cast<bool>(refillCaptionSucceeded));
        g61 ? pass("G61 apply_captions rejects empty editor")
            : fail("G61 apply_captions rejects empty editor",
                   QStringLiteral("empty caption editor was not rejected or restored"));

        QString exclusiveError;
        const bool exclusiveStarted = projectInfoTools.beginExclusiveWrite(
            QStringLiteral("selftest-outer"), &exclusiveError);
        const QJsonObject nestedWriteResponse = callProjectInfoTool(
            71, QStringLiteral("set_playhead"), QJsonObject{
                {QStringLiteral("timeSec"), 0.0}
            });
        const QJsonObject readDuringWriteResponse = callProjectInfoTool(
            72, QStringLiteral("get_project_info"), QJsonObject{});
        projectInfoTools.endExclusiveWrite();
        const QJsonObject writeAfterReleaseResponse = callProjectInfoTool(
            73, QStringLiteral("set_playhead"), QJsonObject{
                {QStringLiteral("timeSec"), 0.0}
            });
        const bool g62 = observeTerm(62, "exclusiveStarted", static_cast<bool>(exclusiveStarted))
            && observeTerm(62, "toolResult(nestedWriteResponse).value(QStringLiteral(\"isError\")) .toBool(false)", static_cast<bool>(toolResult(nestedWriteResponse).value(QStringLiteral("isError"))
                   .toBool(false)))
            && observeTerm(62, "toolErrorText(nestedWriteResponse) .contains(QStringLiteral(\"別の操作を実行中\"))", static_cast<bool>(toolErrorText(nestedWriteResponse)
                   .contains(QStringLiteral("別の操作を実行中"))))
            && observeTerm(62, "!toolResult(readDuringWriteResponse).value(QStringLiteral(\"isError\")) .toBool(true)", static_cast<bool>(!toolResult(readDuringWriteResponse).value(QStringLiteral("isError"))
                   .toBool(true)))
            && observeTerm(62, "toolPayload(writeAfterReleaseResponse) .value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(writeAfterReleaseResponse)
                   .value(QStringLiteral("ok")).toBool(false)));
        g62 ? pass("G62 exclusive write guard rejects nested write")
            : fail("G62 exclusive write guard rejects nested write",
                   QStringLiteral("nested write was not rejected while a write was active"));

        const QJsonObject undoCommandListResponse = callProjectInfoTool(
            74, QStringLiteral("list_commands"), QJsonObject{});
        const QJsonArray undoCommands = toolPayload(undoCommandListResponse)
            .value(QStringLiteral("commands")).toArray();
        QAction *undoAction = nullptr;
        for (QAction *action : projectInfoWindow.findChildren<QAction*>()) {
            if (action && action->text() == QStringLiteral("元に戻す(&U)")) {
                undoAction = action;
                break;
            }
        }
        QString undoCommandId;
        for (const QJsonValue &value : undoCommands) {
            const QJsonObject command = value.toObject();
            if (command.value(QStringLiteral("label")).toString()
                    == QStringLiteral("元に戻す(&U)")) {
                undoCommandId = command.value(QStringLiteral("id")).toString();
                break;
            }
        }
        const bool canUndoBeforeRun = projectTimeline->canUndo();
        QJsonObject nestedRunCommandResponse;
        QJsonObject runCommandResponse;
        bool nestedRunCommandCalled = false;
        if (undoAction && !undoCommandId.isEmpty() && canUndoBeforeRun) {
            const QMetaObject::Connection connection = QObject::connect(
                undoAction, &QAction::triggered, &projectInfoWindow,
                [&]() {
                    nestedRunCommandCalled = true;
                    nestedRunCommandResponse = callProjectInfoTool(
                        65, QStringLiteral("set_playhead"), QJsonObject{
                            {QStringLiteral("timeSec"), 0.0}
                        });
                });
            runCommandResponse = callProjectInfoTool(
                75, QStringLiteral("run_command"), QJsonObject{
                    {QStringLiteral("id"), undoCommandId}
                });
            QObject::disconnect(connection);
        }
        const QJsonObject runCommandPayload = toolPayload(runCommandResponse);
        const bool g63 = observeTerm(63, "canUndoBeforeRun", static_cast<bool>(canUndoBeforeRun)) && observeTerm(63, "undoAction != nullptr", static_cast<bool>(undoAction != nullptr))
            && observeTerm(63, "!undoCommandId.isEmpty()", static_cast<bool>(!undoCommandId.isEmpty())) && observeTerm(63, "nestedRunCommandCalled", static_cast<bool>(nestedRunCommandCalled))
            && observeTerm(63, "!toolResult(runCommandResponse).value(QStringLiteral(\"isError\")) .toBool(true)", static_cast<bool>(!toolResult(runCommandResponse).value(QStringLiteral("isError"))
                   .toBool(true)))
            && observeTerm(63, "runCommandPayload.value(QStringLiteral(\"undoRecorded\")).isBool()", static_cast<bool>(runCommandPayload.value(QStringLiteral("undoRecorded")).isBool()))
            && observeTerm(63, "toolResult(nestedRunCommandResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(nestedRunCommandResponse)
                   .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(63, "toolErrorText(nestedRunCommandResponse) .contains(QStringLiteral(\"別の操作を実行中\"))", static_cast<bool>(toolErrorText(nestedRunCommandResponse)
                   .contains(QStringLiteral("別の操作を実行中"))));
        g63 ? pass("G63 run_command nests through the guard")
            : fail("G63 run_command nests through the guard",
                   QStringLiteral("run_command did not reject a nested write through the guard"));

        const ClipInfo clipA = makeTestClip(QStringLiteral("A"), 0);
        const ClipInfo clipB = makeTestClip(QStringLiteral("B"), 0);
        const ClipInfo clipC = makeTestClip(QStringLiteral("C"), 0);
        captionVideo0->setClips(QVector<ClipInfo>{clipA, clipB, clipC});
        projectTimeline->clearSelection();
        const QJsonObject selectLastResponse = callProjectInfoTool(
            76, QStringLiteral("select_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 2}
            });
        const QJsonObject deleteLastResponse = callProjectInfoTool(
            77, QStringLiteral("delete_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 2}
            });
        const bool deleteSelectionSynchronized =
            toolPayload(deleteLastResponse).value(QStringLiteral("ok")).toBool(false)
            && projectInfoWindow.selectedVideoClipIndexTracked() == -1
            && projectInfoWindow.selectedVideoTrackIndex() == -1
            && captionVideo0->selectedClip() < 0;
        const QJsonObject selectFirstResponse = callProjectInfoTool(
            78, QStringLiteral("select_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0}
            });
        const QJsonObject splitSelectedResponse = callProjectInfoTool(
            79, QStringLiteral("split_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("timeSec"), 2.5}
            });
        const bool splitSelectionSynchronized =
            toolPayload(splitSelectedResponse).value(QStringLiteral("ok")).toBool(false)
            && projectInfoWindow.selectedVideoTrackIndex() == 0
            && projectInfoWindow.selectedVideoClipIndexTracked() == 0
            && captionVideo0->isClipSelected(0);
        const bool g64 = observeTerm(64, "toolPayload(selectLastResponse) .value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(selectLastResponse)
                             .value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(64, "deleteSelectionSynchronized", static_cast<bool>(deleteSelectionSynchronized))
            && observeTerm(64, "toolPayload(selectFirstResponse).value(QStringLiteral(\"ok\")) .toBool(false)", static_cast<bool>(toolPayload(selectFirstResponse).value(QStringLiteral("ok"))
                   .toBool(false)))
            && observeTerm(64, "splitSelectionSynchronized", static_cast<bool>(splitSelectionSynchronized));
        g64 ? pass("G64 delete_clip resynchronizes selection")
            : fail("G64 delete_clip resynchronizes selection",
                   QStringLiteral("MainWindow, Timeline, or split selection state diverged"));

        const QJsonObject playheadResponse = callProjectInfoTool(
            80, QStringLiteral("set_playhead"), QJsonObject{
                {QStringLiteral("timeSec"), 1.25}
            });
        const QJsonObject playheadPayload = toolPayload(playheadResponse);
        const bool g65 = observeTerm(65, "!toolResult(playheadResponse).value(QStringLiteral(\"isError\")) .toBool(true)", static_cast<bool>(!toolResult(playheadResponse).value(QStringLiteral("isError"))
                              .toBool(true)))
            && observeTerm(65, "playheadPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(playheadPayload.value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(65, "playheadPayload.value(QStringLiteral(\"playheadSec\")).toDouble(-1.0) == 1.25", static_cast<bool>(playheadPayload.value(QStringLiteral("playheadSec")).toDouble(-1.0)
                   == 1.25))
            && observeTerm(65, "playheadPayload.value(QStringLiteral(\"playing\")).isBool()", static_cast<bool>(playheadPayload.value(QStringLiteral("playing")).isBool()))
            && observeTerm(65, "!playheadPayload.value(QStringLiteral(\"playing\")).toBool()", static_cast<bool>(!playheadPayload.value(QStringLiteral("playing")).toBool()))
            && observeTerm(65, "playheadPayload.value(QStringLiteral(\"previewSeekRequested\")) .toBool(false)", static_cast<bool>(playheadPayload.value(QStringLiteral("previewSeekRequested"))
                   .toBool(false)))
            && observeTerm(65, "projectTimeline->playheadPosition() == 1.25", static_cast<bool>(projectTimeline->playheadPosition() == 1.25));
        g65 ? pass("G65 set_playhead seeks the player")
            : fail("G65 set_playhead seeks the player",
                   QStringLiteral("set_playhead did not synchronize the timeline and preview"));

        const bool propertyTargetReady = !captionVideo0->clips().isEmpty();
        const double originalVolume = propertyTargetReady
            ? captionVideo0->clips().first().volume : -1.0;
        QJsonObject setPropertyResponse;
        QJsonObject setPropertyUndoResponse;
        if (propertyTargetReady) {
            projectTimeline->undoManager()->clear();
            projectTimeline->undoManager()->saveState(
                projectTimeline->currentState(), QStringLiteral("MCP selftest property baseline"));
            setPropertyResponse = callProjectInfoTool(
                84, QStringLiteral("set_clip_property"), QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("video")},
                    {QStringLiteral("trackIndex"), 0},
                    {QStringLiteral("clipIndex"), 0},
                    {QStringLiteral("property"), QStringLiteral("volume")},
                    {QStringLiteral("value"), 0.5}
                });
        }
        const QJsonObject setPropertyResult = toolResult(setPropertyResponse);
        const QJsonObject setPropertyPayload = toolPayload(setPropertyResponse);
        const double changedVolume = propertyTargetReady
            ? captionVideo0->clips().first().volume : -1.0;
        const bool propertyChanged = propertyTargetReady
            && !setPropertyResult.value(QStringLiteral("isError")).toBool(true)
            && setPropertyPayload.value(QStringLiteral("ok")).toBool(false)
            && setPropertyPayload.value(QStringLiteral("property")).toString()
                   == QStringLiteral("volume")
            && qFuzzyCompare(setPropertyPayload.value(QStringLiteral("value")).toDouble()
                                 + 1.0, 1.5)
            && qFuzzyCompare(changedVolume + 1.0, 1.5);
        if (propertyTargetReady) {
            setPropertyUndoResponse = callProjectInfoTool(
                85, QStringLiteral("undo"), QJsonObject{});
        }
        const QJsonObject setPropertyUndoPayload = toolPayload(setPropertyUndoResponse);
        const bool propertyUndoRestored = propertyTargetReady
            && setPropertyUndoPayload.value(QStringLiteral("ok")).toBool(false)
            && qFuzzyCompare(captionVideo0->clips().first().volume + 1.0,
                             originalVolume + 1.0);
        const bool g83 = observeTerm(83, "propertyChanged", static_cast<bool>(propertyChanged)) && observeTerm(83, "propertyUndoRestored", static_cast<bool>(propertyUndoRestored));
        g83 ? pass("G83 set_clip_property changes and undoes a live clip")
            : fail("G83 set_clip_property changes and undoes a live clip",
                   QStringLiteral("live volume change or undo restoration did not match"));
    }

    if (!projectTimeline || !captionVideo0) {
        fail("G84 trim_clip changes and undoes a live clip",
             QStringLiteral("V1 track was not available"));
        fail("G85 trim_clip rejects an invalid edge",
             QStringLiteral("V1 track was not available"));
        fail("G86 set_transition changes and undoes a live clip",
             QStringLiteral("V1 track was not available"));
        fail("G146 MorphCut is applied and reflected by get_timeline",
             QStringLiteral("V1 track was not available"));
        fail("G147 MorphCut is reverted by one undo",
             QStringLiteral("V1 track was not available"));
        fail("G87 set_transition rejects an invalid type",
             QStringLiteral("V1 track was not available"));
        fail("G88 add_text_overlay changes and undoes a live clip",
             QStringLiteral("V1 track was not available"));
        fail("G89 add_text_overlay rejects an invalid interval",
             QStringLiteral("V1 track was not available"));
        fail("G90 set_transition rejects clearing an absent transition",
             QStringLiteral("V1 track was not available"));
        fail("G91 trim_clip rejects in trim at clip end",
             QStringLiteral("V1 track was not available"));
        fail("G92 trim_clip rejects out trim beyond source end",
             QStringLiteral("V1 track was not available"));
        fail("G93 trim_clip rejects ripple:false",
             QStringLiteral("V1 track was not available"));
    } else {
        auto saveMcpLiveBaseline = [&]() {
            projectTimeline->clearSelection();
            projectTimeline->undoManager()->clear();
            projectTimeline->undoManager()->saveState(
                projectTimeline->currentState(), QStringLiteral("MCP selftest new tools baseline"));
        };

        captionVideo0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("trim-A"), 0),
            makeTestClip(QStringLiteral("trim-B"), 0)
        });
        saveMcpLiveBaseline();
        const QJsonObject trimResponse = callProjectInfoTool(
            86, QStringLiteral("trim_clip"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("edge"), QStringLiteral("in")},
                {QStringLiteral("timeSec"), 1.0}
            });
        const QJsonObject trimPayload = toolPayload(trimResponse);
        const bool trimChanged = !toolResult(trimResponse)
                                      .value(QStringLiteral("isError")).toBool(true)
            && trimPayload.value(QStringLiteral("ok")).toBool(false)
            && requiredOutputFieldsPresent(QStringLiteral("trim_clip"), trimPayload)
            && qAbs(trimPayload.value(QStringLiteral("startSec")).toDouble(-1.0) - 0.0) < 1e-9
            && qAbs(trimPayload.value(QStringLiteral("endSec")).toDouble(-1.0) - 4.0) < 1e-9
            && qAbs(captionVideo0->clips().first().inPoint - 1.0) < 1e-9;
        const QJsonObject trimUndoResponse = trimChanged
            ? callProjectInfoTool(87, QStringLiteral("undo"), QJsonObject{}) : QJsonObject();
        const bool trimUndoRestored = trimChanged
            && toolPayload(trimUndoResponse).value(QStringLiteral("ok")).toBool(false)
            && !captionVideo0->clips().isEmpty()
            && qAbs(captionVideo0->clips().first().inPoint) < 1e-9
            && qAbs(captionVideo0->clips().first().effectiveDuration() - 5.0) < 1e-9;
        const bool g84 = observeTerm(84, "trimChanged", static_cast<bool>(trimChanged)) && observeTerm(84, "trimUndoRestored", static_cast<bool>(trimUndoRestored));
        g84 ? pass("G84 trim_clip changes and undoes a live clip")
            : fail("G84 trim_clip changes and undoes a live clip",
                   QStringLiteral("trim output, live clip state, or undo restoration did not match"));

        const QJsonObject invalidTrimResponse = callProjectInfoTool(
            88, QStringLiteral("trim_clip"), QJsonObject{
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("edge"), QStringLiteral("middle")},
                {QStringLiteral("timeSec"), 1.0}
            });
        const bool g85 = observeTerm(85, "toolResult(invalidTrimResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidTrimResponse)
                             .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(85, "toolPayload(invalidTrimResponse).isEmpty()", static_cast<bool>(toolPayload(invalidTrimResponse).isEmpty()));
        g85 ? pass("G85 trim_clip rejects an invalid edge")
            : fail("G85 trim_clip rejects an invalid edge",
                   QStringLiteral("an invalid edge was accepted"));

        captionVideo0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("transition-A"), 0),
            makeTestClip(QStringLiteral("transition-B"), 0)
        });
        saveMcpLiveBaseline();
        const QJsonObject transitionResponse = callProjectInfoTool(
            89, QStringLiteral("set_transition"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("type"), QStringLiteral("FadeOut")},
                {QStringLiteral("durationSec"), 0.75}
            });
        const QJsonObject transitionPayload = toolPayload(transitionResponse);
        const bool transitionChanged = !toolResult(transitionResponse)
                                            .value(QStringLiteral("isError")).toBool(true)
            && transitionPayload.value(QStringLiteral("ok")).toBool(false)
            && requiredOutputFieldsPresent(QStringLiteral("set_transition"),
                                           transitionPayload)
            && transitionPayload.value(QStringLiteral("type")).toString()
                   == QStringLiteral("FadeOut")
            && qAbs(transitionPayload.value(QStringLiteral("durationSec"))
                        .toDouble(-1.0) - 0.75) < 1e-9
            && captionVideo0->clips().first().trailOut.type == TransitionType::FadeOut
            && qAbs(captionVideo0->clips().first().trailOut.duration - 0.75) < 1e-9
            && captionVideo0->clips().at(1).leadIn.type == TransitionType::FadeIn
            && captionVideo0->selectedClip() < 0
            && !projectTimeline->hasAnySelection();
        const QJsonObject transitionUndoResponse = transitionChanged
            ? callProjectInfoTool(90, QStringLiteral("undo"), QJsonObject{}) : QJsonObject();
        const bool transitionUndoRestored = transitionChanged
            && toolPayload(transitionUndoResponse).value(QStringLiteral("ok")).toBool(false)
            && captionVideo0->clips().first().trailOut.type == TransitionType::None
            && captionVideo0->clips().at(1).leadIn.type == TransitionType::None;
        const bool g86 = observeTerm(86, "transitionChanged", static_cast<bool>(transitionChanged)) && observeTerm(86, "transitionUndoRestored", static_cast<bool>(transitionUndoRestored));
        g86 ? pass("G86 set_transition changes and undoes a live clip")
            : fail("G86 set_transition changes and undoes a live clip",
                   QStringLiteral("transition output, pairing, selection, or undo restoration did not match"));

        // US-202 reserves G146-G147; use the same live pair and undo protocol.
        auto timelineTrackObject = [](const QJsonObject &payload,
                                      const QString &kind, int trackIndex) {
            const QJsonArray tracks = payload.value(kind).toArray();
            return trackIndex >= 0 && trackIndex < tracks.size()
                ? tracks.at(trackIndex).toObject() : QJsonObject{};
        };

        saveMcpLiveBaseline();
        const auto morphBefore = captionVideo0->clips();
        const QJsonObject morphResponse = callProjectInfoTool(
            305, QStringLiteral("set_transition"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("type"), QStringLiteral("MorphCut")},
                {QStringLiteral("durationSec"), 0.75}
            });
        const auto morphTimeline = callProjectInfoTool(
            306, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}
            });
        const QJsonArray morphClips = timelineTrackObject(
            toolPayload(morphTimeline), QStringLiteral("video"), 0)
                .value(QStringLiteral("clips")).toArray();
        const bool g146 = observeTerm(146, "!toolResult(morphResponse).value(QStringLiteral(\"isError\")).toBool(true)", static_cast<bool>(!toolResult(morphResponse).value(QStringLiteral("isError")).toBool(true)))
            && observeTerm(146, "toolPayload(morphResponse).value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(morphResponse).value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(146, "requiredOutputFieldsPresent(QStringLiteral(\"set_transition\"), toolPayload(morphResponse))", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("set_transition"), toolPayload(morphResponse))))
            && observeTerm(146, "captionVideo0->clips().size() == 2", static_cast<bool>(captionVideo0->clips().size() == 2))
            && observeTerm(146, "captionVideo0->clips().first().trailOut.type == TransitionType::MorphCut", static_cast<bool>(captionVideo0->clips().first().trailOut.type == TransitionType::MorphCut))
            && observeTerm(146, "captionVideo0->clips().at(1).leadIn.type == TransitionType::MorphCut", static_cast<bool>(captionVideo0->clips().at(1).leadIn.type == TransitionType::MorphCut))
            && observeTerm(146, "morphClips.size() == 2", static_cast<bool>(morphClips.size() == 2))
            && observeTerm(146, "morphClips.first().toObject().value(QStringLiteral(\"trailOut\")).toObject() .value(QStringLiteral(\"type\")).toString() == QStringLiteral(\"MorphCut\")", static_cast<bool>(morphClips.first().toObject().value(QStringLiteral("trailOut")).toObject()
                   .value(QStringLiteral("type")).toString() == QStringLiteral("MorphCut")))
            && observeTerm(146, "morphClips.at(1).toObject().value(QStringLiteral(\"leadIn\")).toObject() .value(QStringLiteral(\"type\")).toString() == QStringLiteral(\"MorphCut\")", static_cast<bool>(morphClips.at(1).toObject().value(QStringLiteral("leadIn")).toObject()
                   .value(QStringLiteral("type")).toString() == QStringLiteral("MorphCut")));
        g146 ? pass("G146 MorphCut is applied and reflected by get_timeline")
             : fail("G146 MorphCut is applied and reflected by get_timeline",
                    QStringLiteral("MCP mutation, mirrored transition or timeline output diverged"));
        const auto morphUndo = callProjectInfoTool(307, QStringLiteral("undo"), QJsonObject{});
        const auto morphUndoTimeline = callProjectInfoTool(
            308, QStringLiteral("get_timeline"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("video")}
            });
        const QJsonArray morphUndoClips = timelineTrackObject(
            toolPayload(morphUndoTimeline), QStringLiteral("video"), 0)
                .value(QStringLiteral("clips")).toArray();
        const bool g147 = observeTerm(147, "g146", static_cast<bool>(g146)) && observeTerm(147, "toolPayload(morphUndo).value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(morphUndo).value(QStringLiteral("ok")).toBool(false)))
            && observeTerm(147, "captionVideo0->clips().size() == morphBefore.size()", static_cast<bool>(captionVideo0->clips().size() == morphBefore.size()))
            && observeTerm(147, "captionVideo0->clips().first().trailOut.type == morphBefore.first().trailOut.type", static_cast<bool>(captionVideo0->clips().first().trailOut.type == morphBefore.first().trailOut.type))
            && observeTerm(147, "captionVideo0->clips().at(1).leadIn.type == morphBefore.at(1).leadIn.type", static_cast<bool>(captionVideo0->clips().at(1).leadIn.type == morphBefore.at(1).leadIn.type))
            && observeTerm(147, "morphUndoClips.size() == 2", static_cast<bool>(morphUndoClips.size() == 2))
            && observeTerm(147, "morphUndoClips.first().toObject().value(QStringLiteral(\"trailOut\")).toObject() .value(QStringLiteral(\"type\")).toString() == QStringLiteral(\"None\")", static_cast<bool>(morphUndoClips.first().toObject().value(QStringLiteral("trailOut")).toObject()
                   .value(QStringLiteral("type")).toString() == QStringLiteral("None")))
            && observeTerm(147, "morphUndoClips.at(1).toObject().value(QStringLiteral(\"leadIn\")).toObject() .value(QStringLiteral(\"type\")).toString() == QStringLiteral(\"None\")", static_cast<bool>(morphUndoClips.at(1).toObject().value(QStringLiteral("leadIn")).toObject()
                   .value(QStringLiteral("type")).toString() == QStringLiteral("None")))
            && observeTerm(147, "!projectTimeline->undoManager()->canUndo()", static_cast<bool>(!projectTimeline->undoManager()->canUndo()));
        g147 ? pass("G147 MorphCut is reverted by one undo")
             : fail("G147 MorphCut is reverted by one undo",
                    QStringLiteral("one undo did not restore both clips and history"));

        const QJsonObject invalidTransitionResponse = callProjectInfoTool(
            91, QStringLiteral("set_transition"), QJsonObject{
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("type"), QStringLiteral("NotATransition")}
            });
        const bool g87 = observeTerm(87, "toolResult(invalidTransitionResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidTransitionResponse)
                               .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(87, "toolPayload(invalidTransitionResponse).isEmpty()", static_cast<bool>(toolPayload(invalidTransitionResponse).isEmpty()));
        g87 ? pass("G87 set_transition rejects an invalid type")
            : fail("G87 set_transition rejects an invalid type",
                   QStringLiteral("an invalid TransitionType identifier was accepted"));

        captionVideo0->setClips(QVector<ClipInfo>{makeTestClip(QStringLiteral("text-A"), 0)});
        saveMcpLiveBaseline();
        const QJsonObject textResponse = callProjectInfoTool(
            92, QStringLiteral("add_text_overlay"), QJsonObject{
                {QStringLiteral("text"), QStringLiteral("MCP title")},
                {QStringLiteral("startSec"), 1.0},
                {QStringLiteral("endSec"), 3.0},
                {QStringLiteral("x"), 0.25},
                {QStringLiteral("y"), 0.75},
                {QStringLiteral("fontSize"), 24},
                {QStringLiteral("color"), QStringLiteral("#ff0000")}
            });
        const QJsonObject textPayload = toolPayload(textResponse);
        const bool textChanged = !toolResult(textResponse)
                                      .value(QStringLiteral("isError")).toBool(true)
            && textPayload.value(QStringLiteral("ok")).toBool(false)
            && requiredOutputFieldsPresent(QStringLiteral("add_text_overlay"), textPayload)
            && textPayload.value(QStringLiteral("index")).toInt(-1) == 0
            && captionVideo0->clips().first().textManager.count() == 1
            && captionVideo0->clips().first().textManager.overlays().first().text
                   == QStringLiteral("MCP title")
            && qAbs(captionVideo0->clips().first().textManager.overlays().first().startTime - 1.0)
                   < 1e-9
            && qAbs(captionVideo0->clips().first().textManager.overlays().first().endTime - 3.0)
                   < 1e-9
            && qAbs(captionVideo0->clips().first().textManager.overlays().first().x - 0.25)
                   < 1e-9
            && qAbs(captionVideo0->clips().first().textManager.overlays().first().y - 0.75)
                   < 1e-9
            && captionVideo0->clips().first().textManager.overlays().first().font.pointSize() == 24
            && captionVideo0->clips().first().textManager.overlays().first().color
                   == QColor(QStringLiteral("#ff0000"));
        const QJsonObject textUndoResponse = textChanged
            ? callProjectInfoTool(93, QStringLiteral("undo"), QJsonObject{}) : QJsonObject();
        const bool textUndoRestored = textChanged
            && toolPayload(textUndoResponse).value(QStringLiteral("ok")).toBool(false)
            && !captionVideo0->clips().isEmpty()
            && captionVideo0->clips().first().textManager.count() == 0;
        const bool g88 = observeTerm(88, "textChanged", static_cast<bool>(textChanged)) && observeTerm(88, "textUndoRestored", static_cast<bool>(textUndoRestored));
        g88 ? pass("G88 add_text_overlay changes and undoes a live clip")
            : fail("G88 add_text_overlay changes and undoes a live clip",
                   QStringLiteral("text output, persisted overlay, preview data, or undo restoration did not match"));

        const QJsonObject invalidTextResponse = callProjectInfoTool(
            94, QStringLiteral("add_text_overlay"), QJsonObject{
                {QStringLiteral("text"), QStringLiteral("invalid")},
                {QStringLiteral("startSec"), 2.0},
                {QStringLiteral("endSec"), 2.0}
            });
        const bool g89 = observeTerm(89, "toolResult(invalidTextResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(invalidTextResponse)
                            .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(89, "toolPayload(invalidTextResponse).isEmpty()", static_cast<bool>(toolPayload(invalidTextResponse).isEmpty()));
        g89 ? pass("G89 add_text_overlay rejects an invalid interval")
            : fail("G89 add_text_overlay rejects an invalid interval",
                   QStringLiteral("an interval with no positive duration was accepted"));

        captionVideo0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("transition-none"), 0)
        });
        saveMcpLiveBaseline();
        const QJsonObject noTransitionResponse = callProjectInfoTool(
            95, QStringLiteral("set_transition"), QJsonObject{
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("type"), QStringLiteral("None")}
            });
        const bool g90 = observeTerm(90, "toolResult(noTransitionResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(noTransitionResponse)
                             .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(90, "toolPayload(noTransitionResponse).isEmpty()", static_cast<bool>(toolPayload(noTransitionResponse).isEmpty()))
            && observeTerm(90, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()))
            && observeTerm(90, "captionVideo0->clips().first().leadIn.type == TransitionType::None", static_cast<bool>(captionVideo0->clips().first().leadIn.type == TransitionType::None))
            && observeTerm(90, "captionVideo0->clips().first().trailOut.type == TransitionType::None", static_cast<bool>(captionVideo0->clips().first().trailOut.type == TransitionType::None));
        g90 ? pass("G90 set_transition rejects clearing an absent transition")
            : fail("G90 set_transition rejects clearing an absent transition",
                   QStringLiteral("clearing an absent transition was accepted or added undo state"));

        captionVideo0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("trim-in-boundary"), 0)
        });
        saveMcpLiveBaseline();
        const QJsonObject trimInBoundaryResponse = callProjectInfoTool(
            96, QStringLiteral("trim_clip"), QJsonObject{
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("edge"), QStringLiteral("in")},
                {QStringLiteral("timeSec"), 5.0}
            });
        const bool g91 = observeTerm(91, "toolResult(trimInBoundaryResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(trimInBoundaryResponse)
                             .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(91, "toolPayload(trimInBoundaryResponse).isEmpty()", static_cast<bool>(toolPayload(trimInBoundaryResponse).isEmpty()))
            && observeTerm(91, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()))
            && observeTerm(91, "qAbs(captionVideo0->clips().first().inPoint) < 1e-9", static_cast<bool>(qAbs(captionVideo0->clips().first().inPoint) < 1e-9))
            && observeTerm(91, "qAbs(captionVideo0->clips().first().outPoint - 5.0) < 1e-9", static_cast<bool>(qAbs(captionVideo0->clips().first().outPoint - 5.0) < 1e-9));
        g91 ? pass("G91 trim_clip rejects in trim at clip end")
            : fail("G91 trim_clip rejects in trim at clip end",
                   QStringLiteral("edge=in accepted timeSec at or beyond the clip end"));

        captionVideo0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("trim-out-boundary"), 0)
        });
        saveMcpLiveBaseline();
        const QJsonObject trimOutBoundaryResponse = callProjectInfoTool(
            97, QStringLiteral("trim_clip"), QJsonObject{
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("edge"), QStringLiteral("out")},
                {QStringLiteral("timeSec"), 6.0}
            });
        const bool g92 = observeTerm(92, "toolResult(trimOutBoundaryResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(trimOutBoundaryResponse)
                             .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(92, "toolPayload(trimOutBoundaryResponse).isEmpty()", static_cast<bool>(toolPayload(trimOutBoundaryResponse).isEmpty()))
            && observeTerm(92, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()))
            && observeTerm(92, "qAbs(captionVideo0->clips().first().outPoint - 5.0) < 1e-9", static_cast<bool>(qAbs(captionVideo0->clips().first().outPoint - 5.0) < 1e-9));
        g92 ? pass("G92 trim_clip rejects out trim beyond source end")
            : fail("G92 trim_clip rejects out trim beyond source end",
                   QStringLiteral("edge=out accepted a timeSec beyond the source end"));

        captionVideo0->setClips(QVector<ClipInfo>{
            makeTestClip(QStringLiteral("trim-no-ripple"), 0)
        });
        saveMcpLiveBaseline();
        const QJsonObject noRippleResponse = callProjectInfoTool(
            98, QStringLiteral("trim_clip"), QJsonObject{
                {QStringLiteral("clipIndex"), 0},
                {QStringLiteral("edge"), QStringLiteral("in")},
                {QStringLiteral("timeSec"), 1.0},
                {QStringLiteral("ripple"), false}
            });
        const bool g93 = observeTerm(93, "toolResult(noRippleResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(noRippleResponse)
                             .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(93, "toolPayload(noRippleResponse).isEmpty()", static_cast<bool>(toolPayload(noRippleResponse).isEmpty()))
            && observeTerm(93, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()))
            && observeTerm(93, "qAbs(captionVideo0->clips().first().inPoint) < 1e-9", static_cast<bool>(qAbs(captionVideo0->clips().first().inPoint) < 1e-9))
            && observeTerm(93, "qAbs(captionVideo0->clips().first().outPoint - 5.0) < 1e-9", static_cast<bool>(qAbs(captionVideo0->clips().first().outPoint - 5.0) < 1e-9));
        g93 ? pass("G93 trim_clip rejects ripple:false")
            : fail("G93 trim_clip rejects ripple:false",
                   QStringLiteral("ripple:false was accepted by trim_clip"));

        // G94-G95: trim_clip は同じ linkGroup の音声クリップも同量トリムし、
        // 片方が境界違反なら両方とも変更前へ戻す (映像と音声の同期を壊さない)。
        TimelineTrack *linkedAudio0 = projectTimeline->audioTracks().isEmpty()
            ? nullptr : projectTimeline->audioTracks().first();
        if (!linkedAudio0) {
            fail("G94 trim_clip trims the linked audio clip and undo restores both",
                 QStringLiteral("A1 track was not available"));
            fail("G95 trim_clip rolls back when the linked audio cannot take the trim",
                 QStringLiteral("A1 track was not available"));
        } else {
            captionVideo0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("trim-linked-video"), 903)
            });
            linkedAudio0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("trim-linked-audio"), 903)
            });
            saveMcpLiveBaseline();
            const QJsonObject linkedTrimResponse = callProjectInfoTool(
                99, QStringLiteral("trim_clip"), QJsonObject{
                    {QStringLiteral("clipIndex"), 0},
                    {QStringLiteral("edge"), QStringLiteral("in")},
                    {QStringLiteral("timeSec"), 1.0}
                });
            const bool linkedTrimmed = !toolResult(linkedTrimResponse)
                                            .value(QStringLiteral("isError")).toBool(true)
                && toolPayload(linkedTrimResponse).value(QStringLiteral("ok")).toBool(false)
                && qAbs(captionVideo0->clips().first().inPoint - 1.0) < 1e-9
                && qAbs(linkedAudio0->clips().first().inPoint - 1.0) < 1e-9
                && qAbs(linkedAudio0->clips().first().effectiveDuration() - 4.0) < 1e-9;
            const QJsonObject linkedUndoResponse = linkedTrimmed
                ? callProjectInfoTool(100, QStringLiteral("undo"), QJsonObject{}) : QJsonObject();
            const bool linkedRestored = linkedTrimmed
                && toolPayload(linkedUndoResponse).value(QStringLiteral("ok")).toBool(false)
                && qAbs(captionVideo0->clips().first().inPoint) < 1e-9
                && !linkedAudio0->clips().isEmpty()
                && qAbs(linkedAudio0->clips().first().inPoint) < 1e-9
                && qAbs(linkedAudio0->clips().first().effectiveDuration() - 5.0) < 1e-9;
            const bool g94 = observeTerm(94, "linkedTrimmed", static_cast<bool>(linkedTrimmed)) && observeTerm(94, "linkedRestored", static_cast<bool>(linkedRestored));
            g94 ? pass("G94 trim_clip trims the linked audio clip and undo restores both")
                : fail("G94 trim_clip trims the linked audio clip and undo restores both",
                       QStringLiteral("linked audio did not follow the trim or undo did not restore both clips"));

            ClipInfo shortAudio = makeTestClip(QStringLiteral("trim-linked-short-audio"), 904);
            shortAudio.duration = 0.5;
            shortAudio.outPoint = 0.5;
            captionVideo0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("trim-linked-video-2"), 904)
            });
            linkedAudio0->setClips(QVector<ClipInfo>{shortAudio});
            saveMcpLiveBaseline();
            const QJsonObject rollbackResponse = callProjectInfoTool(
                101, QStringLiteral("trim_clip"), QJsonObject{
                    {QStringLiteral("clipIndex"), 0},
                    {QStringLiteral("edge"), QStringLiteral("in")},
                    {QStringLiteral("timeSec"), 1.0}
                });
            const bool g95 = observeTerm(95, "toolResult(rollbackResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(rollbackResponse)
                                 .value(QStringLiteral("isError")).toBool(false)))
                && observeTerm(95, "toolPayload(rollbackResponse).isEmpty()", static_cast<bool>(toolPayload(rollbackResponse).isEmpty()))
                && observeTerm(95, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()))
                && observeTerm(95, "qAbs(captionVideo0->clips().first().inPoint) < 1e-9", static_cast<bool>(qAbs(captionVideo0->clips().first().inPoint) < 1e-9))
                && observeTerm(95, "qAbs(captionVideo0->clips().first().outPoint - 5.0) < 1e-9", static_cast<bool>(qAbs(captionVideo0->clips().first().outPoint - 5.0) < 1e-9))
                && observeTerm(95, "qAbs(linkedAudio0->clips().first().inPoint) < 1e-9", static_cast<bool>(qAbs(linkedAudio0->clips().first().inPoint) < 1e-9));
            g95 ? pass("G95 trim_clip rolls back when the linked audio cannot take the trim")
                : fail("G95 trim_clip rolls back when the linked audio cannot take the trim",
                       QStringLiteral("a partially applied trim survived a linked-audio boundary failure"));
            linkedAudio0->setClips(QVector<ClipInfo>{});
        }

        // G96: 書き出し音声のパススルー (V1 先頭クリップの元音声を 0 秒から mux) が
        // 正しいのは「音声クリップが 1 つ・同じファイル・未トリム・等倍・音量 1」の
        // ときだけ。それ以外は ffmpeg でタイムライン通りのミックスを作る必要がある。
        if (!linkedAudio0) {
            fail("G96 export audio mix is required whenever passthrough would be wrong",
                 QStringLiteral("A1 track was not available"));
        } else {
            auto requiresMix = [&]() {
                return MainWindow::timelineRequiresExportAudioMix(projectTimeline);
            };
            // 先行ゲート (G51/G52) が A2 に残したクリップも正しく「ミックス要」と
            // 判定されるので、パススルー判定の前に A1 以外を空にして後で戻す。
            QVector<QPair<TimelineTrack *, QVector<ClipInfo>>> otherAudioClips;
            for (TimelineTrack *otherTrack : projectTimeline->audioTracks()) {
                if (!otherTrack || otherTrack == linkedAudio0)
                    continue;
                otherAudioClips.append(qMakePair(otherTrack, otherTrack->clips()));
                otherTrack->setClips(QVector<ClipInfo>{});
            }
            const ClipInfo sourceVideo = makeTestClip(QStringLiteral("mix-source"), 905);
            captionVideo0->setClips(QVector<ClipInfo>{sourceVideo});
            linkedAudio0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("mix-source"), 905)
            });
            const bool passthroughOk = !requiresMix();

            ClipInfo trimmedAudio = makeTestClip(QStringLiteral("mix-source"), 905);
            trimmedAudio.inPoint = 1.0;
            linkedAudio0->setClips(QVector<ClipInfo>{trimmedAudio});
            const bool trimmedNeedsMix = requiresMix();

            ClipInfo quietAudio = makeTestClip(QStringLiteral("mix-source"), 905);
            quietAudio.volume = 0.5;
            linkedAudio0->setClips(QVector<ClipInfo>{quietAudio});
            const bool volumeNeedsMix = requiresMix();

            linkedAudio0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("mix-source"), 905),
                makeTestClip(QStringLiteral("mix-source"), 906)
            });
            const bool twoClipsNeedMix = requiresMix();

            linkedAudio0->setClips(QVector<ClipInfo>{});
            const bool noAudioNeedsMix = requiresMix();

            linkedAudio0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("mix-source"), 905)
            });
            const bool wasMuted = linkedAudio0->isMuted();
            linkedAudio0->setMuted(true);
            const bool mutedNeedsMix = requiresMix();
            linkedAudio0->setMuted(wasMuted);
            linkedAudio0->setClips(QVector<ClipInfo>{});
            for (auto &entry : otherAudioClips)
                entry.first->setClips(entry.second);

            const bool g96 = observeTerm(96, "passthroughOk", static_cast<bool>(passthroughOk)) && observeTerm(96, "trimmedNeedsMix", static_cast<bool>(trimmedNeedsMix)) && observeTerm(96, "volumeNeedsMix", static_cast<bool>(volumeNeedsMix))
                && observeTerm(96, "twoClipsNeedMix", static_cast<bool>(twoClipsNeedMix)) && observeTerm(96, "noAudioNeedsMix", static_cast<bool>(noAudioNeedsMix)) && observeTerm(96, "mutedNeedsMix", static_cast<bool>(mutedNeedsMix));
            g96 ? pass("G96 export audio mix is required whenever passthrough would be wrong")
                : fail("G96 export audio mix is required whenever passthrough would be wrong",
                       QStringLiteral("passthrough=%1 trimmed=%2 volume=%3 twoClips=%4 noAudio=%5 muted=%6")
                           .arg(passthroughOk).arg(trimmedNeedsMix).arg(volumeNeedsMix)
                           .arg(twoClipsNeedMix).arg(noAudioNeedsMix).arg(mutedNeedsMix));
        }

        // G97: RenderQueue はプロジェクト JSON (.veditor / .vsep / .json) を音声ソース
        // として開かず、V1 クリップへフォールバックする。
        const bool g97 = observeTerm(97, "RenderQueue::isProjectFilePath(QStringLiteral(\"D:/x/project.veditor\"))", static_cast<bool>(RenderQueue::isProjectFilePath(QStringLiteral("D:/x/project.veditor"))))
            && observeTerm(97, "RenderQueue::isProjectFilePath(QStringLiteral(\"D:/x/project.vsep\"))", static_cast<bool>(RenderQueue::isProjectFilePath(QStringLiteral("D:/x/project.vsep"))))
            && observeTerm(97, "RenderQueue::isProjectFilePath(QStringLiteral(\"D:/x/PROJECT.JSON\"))", static_cast<bool>(RenderQueue::isProjectFilePath(QStringLiteral("D:/x/PROJECT.JSON"))))
            && observeTerm(97, "!RenderQueue::isProjectFilePath(QStringLiteral(\"D:/x/clip.mp4\"))", static_cast<bool>(!RenderQueue::isProjectFilePath(QStringLiteral("D:/x/clip.mp4"))))
            && observeTerm(97, "!RenderQueue::isProjectFilePath(QStringLiteral(\"D:/x/mix.m4a\"))", static_cast<bool>(!RenderQueue::isProjectFilePath(QStringLiteral("D:/x/mix.m4a"))))
            && observeTerm(97, "!RenderQueue::isProjectFilePath(QStringLiteral(\"D:/x/audio.wav\"))", static_cast<bool>(!RenderQueue::isProjectFilePath(QStringLiteral("D:/x/audio.wav"))))
            && observeTerm(97, "!RenderQueue::isProjectFilePath(QString())", static_cast<bool>(!RenderQueue::isProjectFilePath(QString())));
        g97 ? pass("G97 project files are never used as the export audio source")
            : fail("G97 project files are never used as the export audio source",
                   QStringLiteral("RenderQueue::isProjectFilePath misclassified a path"));

        // G98: export_video の音声引数はキュー投入前に検証される。
        const QString exportArgsPath = QDir::tempPath()
            + QStringLiteral("/mcp-selftest-export-args.mp4");
        const QJsonObject emptyAudioCodecResponse = callProjectInfoTool(
            102, QStringLiteral("export_video"), QJsonObject{
                {QStringLiteral("outputPath"), exportArgsPath},
                {QStringLiteral("audioCodec"), QStringLiteral("  ")}
            });
        const QJsonObject zeroAudioBitrateResponse = callProjectInfoTool(
            103, QStringLiteral("export_video"), QJsonObject{
                {QStringLiteral("outputPath"), exportArgsPath},
                {QStringLiteral("audioBitrate"), 0}
            });
        const bool g98 = observeTerm(98, "toolResult(emptyAudioCodecResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(emptyAudioCodecResponse)
                             .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(98, "toolPayload(emptyAudioCodecResponse).isEmpty()", static_cast<bool>(toolPayload(emptyAudioCodecResponse).isEmpty()))
            && observeTerm(98, "toolResult(zeroAudioBitrateResponse) .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(zeroAudioBitrateResponse)
                   .value(QStringLiteral("isError")).toBool(false)))
            && observeTerm(98, "toolPayload(zeroAudioBitrateResponse).isEmpty()", static_cast<bool>(toolPayload(zeroAudioBitrateResponse).isEmpty()))
            && observeTerm(98, "!QFileInfo::exists(exportArgsPath)", static_cast<bool>(!QFileInfo::exists(exportArgsPath)));
        g98 ? pass("G98 export_video validates audioCodec and audioBitrate before queuing")
            : fail("G98 export_video validates audioCodec and audioBitrate before queuing",
                   QStringLiteral("an invalid audio argument was accepted by export_video"));

        // G99-G100: 字幕エディタの一覧は remove_caption / clear_captions で直せる。
        {
            const QJsonObject clearedFirst = callProjectInfoTool(
                104, QStringLiteral("clear_captions"), QJsonObject{});
            callProjectInfoTool(105, QStringLiteral("add_caption"), QJsonObject{
                {QStringLiteral("text"), QStringLiteral("first")},
                {QStringLiteral("startSec"), 0.0}, {QStringLiteral("endSec"), 1.0}});
            callProjectInfoTool(106, QStringLiteral("add_caption"), QJsonObject{
                {QStringLiteral("text"), QStringLiteral("second")},
                {QStringLiteral("startSec"), 2.0}, {QStringLiteral("endSec"), 3.0}});
            const QJsonObject removed = callProjectInfoTool(
                107, QStringLiteral("remove_caption"), QJsonObject{{QStringLiteral("index"), 0}});
            const QJsonObject afterRemove = toolPayload(callProjectInfoTool(
                108, QStringLiteral("get_captions"), QJsonObject{}));
            const QJsonArray remaining = afterRemove.value(QStringLiteral("captions")).toArray();
            const QJsonObject removeOutOfRange = callProjectInfoTool(
                109, QStringLiteral("remove_caption"), QJsonObject{{QStringLiteral("index"), 5}});
            const bool g99 = observeTerm(99, "toolPayload(clearedFirst).value(QStringLiteral(\"captionCount\")).toInt(-1) == 0", static_cast<bool>(toolPayload(clearedFirst).value(QStringLiteral("captionCount")).toInt(-1) == 0))
                && observeTerm(99, "toolPayload(removed).value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(toolPayload(removed).value(QStringLiteral("ok")).toBool(false)))
                && observeTerm(99, "toolPayload(removed).value(QStringLiteral(\"captionCount\")).toInt(-1) == 1", static_cast<bool>(toolPayload(removed).value(QStringLiteral("captionCount")).toInt(-1) == 1))
                && observeTerm(99, "remaining.size() == 1", static_cast<bool>(remaining.size() == 1))
                && observeTerm(99, "remaining.first().toObject().value(QStringLiteral(\"text\")).toString() == QStringLiteral(\"second\")", static_cast<bool>(remaining.first().toObject().value(QStringLiteral("text")).toString()
                       == QStringLiteral("second")))
                && observeTerm(99, "toolResult(removeOutOfRange).value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(removeOutOfRange).value(QStringLiteral("isError")).toBool(false)))
                && observeTerm(99, "toolResult(removeOutOfRange).value(QStringLiteral(\"content\")).toArray() .first().toObject().value(QStringLiteral(\"text\")).toString() .contains(QStringLiteral(\"0..0\"))", static_cast<bool>(toolResult(removeOutOfRange).value(QStringLiteral("content")).toArray()
                       .first().toObject().value(QStringLiteral("text")).toString()
                       .contains(QStringLiteral("0..0"))));
            g99 ? pass("G99 remove_caption removes one caption and reports the valid range")
                : fail("G99 remove_caption removes one caption and reports the valid range",
                       QStringLiteral("caption list after remove_caption did not match"));

            const QJsonObject cleared = callProjectInfoTool(
                110, QStringLiteral("clear_captions"), QJsonObject{});
            const QJsonObject afterClear = toolPayload(callProjectInfoTool(
                111, QStringLiteral("get_captions"), QJsonObject{}));
            const QJsonObject applyEmpty = callProjectInfoTool(
                112, QStringLiteral("apply_captions"), QJsonObject{});
            const bool g100 = observeTerm(100, "toolPayload(cleared).value(QStringLiteral(\"captionCount\")).toInt(-1) == 0", static_cast<bool>(toolPayload(cleared).value(QStringLiteral("captionCount")).toInt(-1) == 0))
                && observeTerm(100, "afterClear.value(QStringLiteral(\"captions\")).toArray().isEmpty()", static_cast<bool>(afterClear.value(QStringLiteral("captions")).toArray().isEmpty()))
                && observeTerm(100, "toolResult(applyEmpty).value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(applyEmpty).value(QStringLiteral("isError")).toBool(false)));
            g100 ? pass("G100 clear_captions empties the editor list")
                 : fail("G100 clear_captions empties the editor list",
                        QStringLiteral("caption list was not empty after clear_captions"));
        }

        // G102: モーダルを開くが省略記号の無い「バージョン情報」は blocking。
        {
            const QJsonObject aboutList = toolPayload(callProjectInfoTool(
                113, QStringLiteral("list_commands"),
                QJsonObject{{QStringLiteral("query"), QStringLiteral("バージョン情報")}}));
            const QJsonArray aboutCommands = aboutList.value(QStringLiteral("commands")).toArray();
            bool aboutBlocking = !aboutCommands.isEmpty();
            for (const QJsonValue& value : aboutCommands) {
                if (value.toObject().value(QStringLiteral("risk")).toString()
                    != QStringLiteral("blocking"))
                    aboutBlocking = false;
            }
            aboutBlocking ? pass("G102 about dialog command is classified blocking")
                          : fail("G102 about dialog command is classified blocking",
                                 QStringLiteral("commands=%1").arg(
                                     QString::fromUtf8(QJsonDocument(aboutCommands).toJson(
                                         QJsonDocument::Compact))));
        }

        // G103: UndoManager::saveSerial は MAX_UNDO 到達後も増える (undoRecorded の根拠)。
        {
            UndoManager* undoManager = projectTimeline->undoManager();
            const quint64 serialBefore = undoManager->saveSerial();
            for (int i = 0; i < UndoManager::MAX_UNDO + 5; ++i)
                projectTimeline->saveUndoState(QStringLiteral("serial probe"));
            const bool g103 = observeTerm(103, "undoManager->saveSerial() == serialBefore + static_cast<quint64>(UndoManager::MAX_UNDO + 5)", static_cast<bool>(undoManager->saveSerial()
                    == serialBefore + static_cast<quint64>(UndoManager::MAX_UNDO + 5)))
                && observeTerm(103, "undoManager->currentIndex() == UndoManager::MAX_UNDO - 1", static_cast<bool>(undoManager->currentIndex() == UndoManager::MAX_UNDO - 1));
            g103 ? pass("G103 undo save serial keeps counting past MAX_UNDO")
                 : fail("G103 undo save serial keeps counting past MAX_UNDO",
                        QStringLiteral("serial=%1 index=%2")
                            .arg(undoManager->saveSerial()).arg(undoManager->currentIndex()));
            saveMcpLiveBaseline();
        }

        // G104: 動画/音声として開けないファイルは取り込まない (0 秒クリップを作らない)。
        {
            QTemporaryFile notMedia(QDir::tempPath() + QStringLiteral("/mcp-selftest-XXXXXX.txt"));
            bool importRejected = false;
            if (notMedia.open()) {
                notMedia.write("this is not a media file\n");
                notMedia.flush();
                const int videoBefore = captionVideo0->clipCount();
                const QJsonObject rejected = callProjectInfoTool(
                    114, QStringLiteral("import_media"),
                    QJsonObject{{QStringLiteral("filePath"), notMedia.fileName()}});
                importRejected = observeTerm(104, "toolResult(rejected).value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(rejected).value(QStringLiteral("isError")).toBool(false)))
                    && observeTerm(104, "toolResult(rejected).value(QStringLiteral(\"content\")).toArray() .first().toObject().value(QStringLiteral(\"text\")).toString() .contains(QStringLiteral(\"開けません\"))", static_cast<bool>(toolResult(rejected).value(QStringLiteral("content")).toArray()
                           .first().toObject().value(QStringLiteral("text")).toString()
                           .contains(QStringLiteral("開けません"))))
                    && observeTerm(104, "captionVideo0->clipCount() == videoBefore", static_cast<bool>(captionVideo0->clipCount() == videoBefore))
                    && observeTerm(104, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()));
            }
            importRejected ? pass("G104 import_media rejects a file that is not media")
                           : fail("G104 import_media rejects a file that is not media",
                                  QStringLiteral("a non-media file was imported or the error text was unclear"));
        }

        // G105: 空のタイムラインは export_video で即エラー (1 フレームの無意味な出力を作らない)。
        // 先行ゲートが V2/A2 に残したクリップも尺に数えるので、全トラックを空にして後で戻す。
        {
            QVector<QPair<TimelineTrack *, QVector<ClipInfo>>> everyTrackClips;
            const QVector<TimelineTrack *> everyTrack =
                projectTimeline->videoTracks() + projectTimeline->audioTracks();
            for (TimelineTrack *anyTrack : everyTrack) {
                if (!anyTrack)
                    continue;
                everyTrackClips.append(qMakePair(anyTrack, anyTrack->clips()));
                anyTrack->setClips(QVector<ClipInfo>{});
            }
            saveMcpLiveBaseline();
            const QJsonObject emptyExport = callProjectInfoTool(
                115, QStringLiteral("export_video"),
                QJsonObject{{QStringLiteral("outputPath"), exportArgsPath}});
            const bool g105 = observeTerm(105, "toolResult(emptyExport).value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(toolResult(emptyExport).value(QStringLiteral("isError")).toBool(false)))
                && observeTerm(105, "toolResult(emptyExport).value(QStringLiteral(\"content\")).toArray() .first().toObject().value(QStringLiteral(\"text\")).toString() .contains(QStringLiteral(\"空\"))", static_cast<bool>(toolResult(emptyExport).value(QStringLiteral("content")).toArray()
                       .first().toObject().value(QStringLiteral("text")).toString()
                       .contains(QStringLiteral("空"))))
                && observeTerm(105, "!QFileInfo::exists(exportArgsPath)", static_cast<bool>(!QFileInfo::exists(exportArgsPath)));
            g105 ? pass("G105 export_video rejects an empty timeline")
                 : fail("G105 export_video rejects an empty timeline",
                        QStringLiteral("an empty timeline was accepted for export (totalDuration=%1)")
                            .arg(projectTimeline->totalDuration()));
            for (auto &entry : everyTrackClips)
                entry.first->setClips(entry.second);
        }

        // G106: speed はリンクした音声にも適用され、1 回の undo で両方戻る。volume は片側だけ。
        if (!linkedAudio0) {
            fail("G106 set_clip_property speed follows the linked audio clip",
                 QStringLiteral("A1 track was not available"));
        } else {
            captionVideo0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("speed-video"), 907)});
            linkedAudio0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("speed-audio"), 907)});
            saveMcpLiveBaseline();
            const QJsonObject speedResponse = callProjectInfoTool(
                116, QStringLiteral("set_clip_property"), QJsonObject{
                    {QStringLiteral("clipIndex"), 0},
                    {QStringLiteral("property"), QStringLiteral("speed")},
                    {QStringLiteral("value"), 2.0}});
            const bool speedLinked = toolPayload(speedResponse).value(QStringLiteral("linkedApplied")).toBool(false)
                && qAbs(captionVideo0->clips().first().speed - 2.0) < 1e-9
                && qAbs(linkedAudio0->clips().first().speed - 2.0) < 1e-9;
            const QJsonObject speedUndo = speedLinked
                ? callProjectInfoTool(117, QStringLiteral("undo"), QJsonObject{}) : QJsonObject();
            const bool speedRestored = speedLinked
                && toolPayload(speedUndo).value(QStringLiteral("ok")).toBool(false)
                && qAbs(captionVideo0->clips().first().speed - 1.0) < 1e-9
                && qAbs(linkedAudio0->clips().first().speed - 1.0) < 1e-9;
            const QJsonObject volumeResponse = callProjectInfoTool(
                118, QStringLiteral("set_clip_property"), QJsonObject{
                    {QStringLiteral("clipIndex"), 0},
                    {QStringLiteral("property"), QStringLiteral("volume")},
                    {QStringLiteral("value"), 0.5}});
            const bool volumeOnlyVideo = !toolPayload(volumeResponse).value(QStringLiteral("linkedApplied")).toBool(true)
                && qAbs(captionVideo0->clips().first().volume - 0.5) < 1e-9
                && qAbs(linkedAudio0->clips().first().volume - 1.0) < 1e-9;
            const bool g106 = observeTerm(106, "speedLinked", static_cast<bool>(speedLinked)) && observeTerm(106, "speedRestored", static_cast<bool>(speedRestored)) && observeTerm(106, "volumeOnlyVideo", static_cast<bool>(volumeOnlyVideo));
            g106 ? pass("G106 set_clip_property speed follows the linked audio clip")
                 : fail("G106 set_clip_property speed follows the linked audio clip",
                        QStringLiteral("speedLinked=%1 restored=%2 volumeOnlyVideo=%3")
                            .arg(speedLinked).arg(speedRestored).arg(volumeOnlyVideo));
            linkedAudio0->setClips(QVector<ClipInfo>{});
        }

        // G107: select_clip は kind / trackIndex を省略でき、他ツールと同じ既定 (video / 0)。
        {
            captionVideo0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("select-default"), 0)});
            saveMcpLiveBaseline();
            const QJsonObject selectDefault = callProjectInfoTool(
                119, QStringLiteral("select_clip"), QJsonObject{{QStringLiteral("clipIndex"), 0}});
            const QJsonObject selectPayload = toolPayload(selectDefault);
            const bool g107 = observeTerm(107, "selectPayload.value(QStringLiteral(\"ok\")).toBool(false)", static_cast<bool>(selectPayload.value(QStringLiteral("ok")).toBool(false)))
                && observeTerm(107, "selectPayload.value(QStringLiteral(\"kind\")).toString() == QStringLiteral(\"video\")", static_cast<bool>(selectPayload.value(QStringLiteral("kind")).toString() == QStringLiteral("video")))
                && observeTerm(107, "selectPayload.value(QStringLiteral(\"trackIndex\")).toInt(-1) == 0", static_cast<bool>(selectPayload.value(QStringLiteral("trackIndex")).toInt(-1) == 0))
                && observeTerm(107, "captionVideo0->selectedClip() == 0", static_cast<bool>(captionVideo0->selectedClip() == 0));
            g107 ? pass("G107 select_clip defaults kind and trackIndex like the other clip tools")
                 : fail("G107 select_clip defaults kind and trackIndex like the other clip tools",
                        QStringLiteral("select_clip without kind/trackIndex did not select V1 clip 0"));
            projectTimeline->clearSelection();
        }

        // G108: add_text_overlay は区間と重なる V1 の全クリップに付く (clip 0 固定ではない)。
        {
            captionVideo0->setClips(QVector<ClipInfo>{
                makeTestClip(QStringLiteral("text-A"), 0),
                makeTestClip(QStringLiteral("text-B"), 0)});
            saveMcpLiveBaseline();
            const QJsonObject secondClipText = callProjectInfoTool(
                120, QStringLiteral("add_text_overlay"), QJsonObject{
                    {QStringLiteral("text"), QStringLiteral("late")},
                    {QStringLiteral("startSec"), 6.0}, {QStringLiteral("endSec"), 7.0}});
            const QJsonArray lateIndices = toolPayload(secondClipText)
                                               .value(QStringLiteral("clipIndices")).toArray();
            const bool lateOnSecond = lateIndices.size() == 1 && lateIndices.first().toInt(-1) == 1
                && captionVideo0->clips().at(0).textManager.count() == 0
                && captionVideo0->clips().at(1).textManager.count() == 1;
            const QJsonObject spanningText = callProjectInfoTool(
                121, QStringLiteral("add_text_overlay"), QJsonObject{
                    {QStringLiteral("text"), QStringLiteral("span")},
                    {QStringLiteral("startSec"), 4.0}, {QStringLiteral("endSec"), 6.0}});
            const QJsonArray spanIndices = toolPayload(spanningText)
                                               .value(QStringLiteral("clipIndices")).toArray();
            const bool spanOnBoth = spanIndices.size() == 2
                && captionVideo0->clips().at(0).textManager.count() == 1
                && captionVideo0->clips().at(1).textManager.count() == 2;
            const QJsonObject outsideText = callProjectInfoTool(
                122, QStringLiteral("add_text_overlay"), QJsonObject{
                    {QStringLiteral("text"), QStringLiteral("nowhere")},
                    {QStringLiteral("startSec"), 20.0}, {QStringLiteral("endSec"), 21.0}});
            const bool outsideRejected = toolResult(outsideText)
                                             .value(QStringLiteral("isError")).toBool(false)
                && captionVideo0->clips().at(1).textManager.count() == 2;
            const bool g108 = observeTerm(108, "lateOnSecond", static_cast<bool>(lateOnSecond)) && observeTerm(108, "spanOnBoth", static_cast<bool>(spanOnBoth)) && observeTerm(108, "outsideRejected", static_cast<bool>(outsideRejected));
            g108 ? pass("G108 add_text_overlay attaches to every V1 clip overlapping the interval")
                 : fail("G108 add_text_overlay attaches to every V1 clip overlapping the interval",
                        QStringLiteral("lateOnSecond=%1 spanOnBoth=%2 outsideRejected=%3")
                            .arg(lateOnSecond).arg(spanOnBoth).arg(outsideRejected));
        }

        // G109-G110: ステータスバー右端の「LLM に指示を出す」ボタンは MCP サーバ起動 +
        // AI チャット表示までを 1 クリックで行い、表示メニューで隠せる (設定に保存)。
        {
            QSettings prefSettings(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
            const QVariant autoStartBefore = prefSettings.value(QStringLiteral("mcpAutoStart"));
            const QVariant buttonVisibleBefore =
                prefSettings.value(QStringLiteral("llmAssistantButtonVisible"));

            // MainWindow の内部メンバは private なので、メニューの QAction / 子 QObject を
            // 実際の UI ツリーから引く (ユーザーが見るものと同じ経路)。
            auto actionByText = [&projectInfoWindow](const QString &text) -> QAction * {
                const QList<QAction *> actions = projectInfoWindow.findChildren<QAction *>();
                for (QAction *action : actions) {
                    if (action && action->text() == text)
                        return action;
                }
                return nullptr;
            };
            QAction *mcpToggle = actionByText(QStringLiteral("MCP サーバを有効にする"));
            QAction *aiChatAction = actionByText(QStringLiteral("AI チャット"));
            auto *assistantButton = projectInfoWindow.findChild<QToolButton *>(
                QStringLiteral("LlmAssistantButton"));
            const bool buttonPresent = observeTerm(109, "assistantButton", static_cast<bool>(assistantButton))
                && observeTerm(109, "assistantButton->text() == QStringLiteral(\"LLM に指示を出す\")", static_cast<bool>(assistantButton->text() == QStringLiteral("LLM に指示を出す")))
                && observeTerm(109, "assistantButton->parentWidget() == projectInfoWindow.statusBar()", static_cast<bool>(assistantButton->parentWidget() == projectInfoWindow.statusBar()))
                && observeTerm(109, "!assistantButton->isHidden()", static_cast<bool>(!assistantButton->isHidden()));
            bool toggleFound = false, toggleChecked = false, serverFound = false, serverRunning = false;
            bool chatActionFound = false, chatActionChecked = false, dockFound = false, dockShown = false;
            if (buttonPresent) {
                assistantButton->click();
                auto *server = projectInfoWindow.findChild<mcp::McpHttpServer *>(QStringLiteral("mcpHttpServer"));
                auto *dock = projectInfoWindow.findChild<AiChatDock *>(QStringLiteral("AiChatDock"));
                toggleFound = mcpToggle != nullptr;
                toggleChecked = mcpToggle && mcpToggle->isChecked();
                serverFound = server != nullptr;
                serverRunning = server && server->isRunning();
                chatActionFound = aiChatAction != nullptr;
                chatActionChecked = aiChatAction && aiChatAction->isChecked();
                dockFound = dock != nullptr;
                dockShown = dock && !dock->isHidden();
            }
            const bool assistantOpened = observeTerm(109, "toggleFound", static_cast<bool>(toggleFound)) && observeTerm(109, "toggleChecked", static_cast<bool>(toggleChecked)) && observeTerm(109, "serverFound", static_cast<bool>(serverFound)) && observeTerm(109, "serverRunning", static_cast<bool>(serverRunning))
                && observeTerm(109, "chatActionFound", static_cast<bool>(chatActionFound)) && observeTerm(109, "chatActionChecked", static_cast<bool>(chatActionChecked)) && observeTerm(109, "dockFound", static_cast<bool>(dockFound)) && observeTerm(109, "dockShown", static_cast<bool>(dockShown));
            if (aiChatAction)
                aiChatAction->setChecked(false);
            if (mcpToggle && mcpToggle->isChecked())
                mcpToggle->setChecked(false);
            const bool g109 = observeTerm(109, "buttonPresent", static_cast<bool>(buttonPresent)) && observeTerm(109, "assistantOpened", static_cast<bool>(assistantOpened));
            g109 ? pass("G109 status bar LLM button starts the MCP server and opens the AI chat")
                 : fail("G109 status bar LLM button starts the MCP server and opens the AI chat",
                        QStringLiteral("present=%1 opened=%2 toggleFound=%3 toggleChecked=%4 serverFound=%5 "
                                       "serverRunning=%6 chatActionFound=%7 chatActionChecked=%8 dockFound=%9 dockShown=%10")
                            .arg(buttonPresent).arg(assistantOpened).arg(toggleFound).arg(toggleChecked)
                            .arg(serverFound).arg(serverRunning).arg(chatActionFound).arg(chatActionChecked)
                            .arg(dockFound).arg(dockShown));

            QAction *toggle = actionByText(QStringLiteral("「LLM に指示を出す」ボタンを表示"));
            bool toggleWorks = observeTerm(110, "toggle", static_cast<bool>(toggle)) && observeTerm(110, "toggle->isCheckable()", static_cast<bool>(toggle->isCheckable())) && observeTerm(110, "toggle->isChecked()", static_cast<bool>(toggle->isChecked()))
                && observeTerm(110, "assistantButton", static_cast<bool>(assistantButton)) && observeTerm(110, "!assistantButton->isHidden()", static_cast<bool>(!assistantButton->isHidden()));
            if (toggleWorks) {
                toggle->setChecked(false);
                toggleWorks = observeTerm(110, "assistantButton->isHidden()", static_cast<bool>(assistantButton->isHidden()))
                    && observeTerm(110, "!prefSettings.value(QStringLiteral(\"llmAssistantButtonVisible\"), true).toBool()", static_cast<bool>(!prefSettings.value(QStringLiteral("llmAssistantButtonVisible"), true).toBool()));
                toggle->setChecked(true);
                toggleWorks = observeTerm(110, "toggleWorks", static_cast<bool>(toggleWorks)) && observeTerm(110, "!assistantButton->isHidden()", static_cast<bool>(!assistantButton->isHidden()))
                    && observeTerm(110, "prefSettings.value(QStringLiteral(\"llmAssistantButtonVisible\"), false).toBool()", static_cast<bool>(prefSettings.value(QStringLiteral("llmAssistantButtonVisible"), false).toBool()));
            }
            toggleWorks ? pass("G110 View menu toggles the LLM button and persists the choice")
                        : fail("G110 View menu toggles the LLM button and persists the choice",
                               QStringLiteral("the View menu toggle did not hide/show the button or persist it"));

            // G111: AI チャット Dock 下部の接続状態行と「接続 / 切断」ボタン。
            {
                if (aiChatAction)
                    aiChatAction->setChecked(true);
                auto *dock = projectInfoWindow.findChild<AiChatDock *>(QStringLiteral("AiChatDock"));
                auto *connectionLabel = dock
                    ? dock->findChild<QLabel *>(QStringLiteral("AiChatConnectionStatus")) : nullptr;
                auto *connectButton = dock
                    ? dock->findChild<QPushButton *>(QStringLiteral("AiChatConnectButton")) : nullptr;
                auto *server = projectInfoWindow.findChild<mcp::McpHttpServer *>(QStringLiteral("mcpHttpServer"));
                const bool dockFound111 = dock != nullptr;
                const bool labelFound = connectionLabel != nullptr;
                const bool buttonFound = connectButton != nullptr;
                const bool serverFound111 = server != nullptr;
                const bool serverRunningStopped = server && server->isRunning();
                const QString labelTextStopped = connectionLabel ? connectionLabel->text() : QString();
                const QString buttonTextStopped = connectButton ? connectButton->text() : QString();
                const bool stoppedShown = observeTerm(111, "connectionLabel", static_cast<bool>(connectionLabel)) && observeTerm(111, "connectButton", static_cast<bool>(connectButton))
                    && observeTerm(111, "!serverRunningStopped", static_cast<bool>(!serverRunningStopped))
                    && observeTerm(111, "labelTextStopped.contains(QStringLiteral(\"停止中\"))", static_cast<bool>(labelTextStopped.contains(QStringLiteral("停止中"))))
                    && observeTerm(111, "labelTextStopped.contains(QStringLiteral(\"CLI\"))", static_cast<bool>(labelTextStopped.contains(QStringLiteral("CLI"))))
                    && observeTerm(111, "buttonTextStopped == QStringLiteral(\"接続\")", static_cast<bool>(buttonTextStopped == QStringLiteral("接続")));
                bool connected = false;
                bool disconnected = false;
                bool mcpToggleConnected = false, mcpToggleDisconnected = false;
                bool serverRunningConnected = false, serverRunningDisconnected = false;
                QString labelTextConnected, buttonTextConnected, labelTextDisconnected, buttonTextDisconnected;
                if (stoppedShown) {
                    connectButton->click();
                    server = projectInfoWindow.findChild<mcp::McpHttpServer *>(QStringLiteral("mcpHttpServer"));
                    serverRunningConnected = server && server->isRunning();
                    labelTextConnected = connectionLabel->text();
                    buttonTextConnected = connectButton->text();
                    mcpToggleConnected = mcpToggle && mcpToggle->isChecked();
                    connected = observeTerm(111, "serverRunningConnected", static_cast<bool>(serverRunningConnected))
                        && observeTerm(111, "labelTextConnected.contains(QString::number(server->port()))", static_cast<bool>(labelTextConnected.contains(QString::number(server->port()))))
                        && observeTerm(111, "buttonTextConnected == QStringLiteral(\"切断\")", static_cast<bool>(buttonTextConnected == QStringLiteral("切断")))
                        && observeTerm(111, "mcpToggleConnected", static_cast<bool>(mcpToggleConnected));
                    connectButton->click();
                    serverRunningDisconnected = server && server->isRunning();
                    labelTextDisconnected = connectionLabel->text();
                    buttonTextDisconnected = connectButton->text();
                    mcpToggleDisconnected = mcpToggle && mcpToggle->isChecked();
                    disconnected = observeTerm(111, "!serverRunningDisconnected", static_cast<bool>(!serverRunningDisconnected))
                        && observeTerm(111, "labelTextDisconnected.contains(QStringLiteral(\"停止中\"))", static_cast<bool>(labelTextDisconnected.contains(QStringLiteral("停止中"))))
                        && observeTerm(111, "buttonTextDisconnected == QStringLiteral(\"接続\")", static_cast<bool>(buttonTextDisconnected == QStringLiteral("接続")))
                        && observeTerm(111, "mcpToggle", static_cast<bool>(mcpToggle)) && observeTerm(111, "!mcpToggleDisconnected", static_cast<bool>(!mcpToggleDisconnected));
                }
                if (aiChatAction)
                    aiChatAction->setChecked(false);
                const bool g111 = observeTerm(111, "stoppedShown", static_cast<bool>(stoppedShown)) && observeTerm(111, "connected", static_cast<bool>(connected)) && observeTerm(111, "disconnected", static_cast<bool>(disconnected));
                g111 ? pass("G111 AI chat dock shows the connection state and its button starts/stops the server")
                     : fail("G111 AI chat dock shows the connection state and its button starts/stops the server",
                            QStringLiteral("stoppedShown=%1 connected=%2 disconnected=%3 dockFound=%4 labelFound=%5 "
                                           "buttonFound=%6 serverFound=%7 serverRunning(stopped/connected/disconnected)=%8/%9/%10 "
                                           "label(stopped/connected/disconnected)=[%11]/[%12]/[%13] "
                                           "button(stopped/connected/disconnected)=[%14]/[%15]/[%16] mcpToggleFound=%17 mcpToggle(connected/disconnected)=%18(want 1)/%19(want 0)")
                                .arg(stoppedShown).arg(connected).arg(disconnected)
                                .arg(dockFound111).arg(labelFound).arg(buttonFound).arg(serverFound111)
                                .arg(serverRunningStopped).arg(serverRunningConnected).arg(serverRunningDisconnected)
                                .arg(labelTextStopped, labelTextConnected, labelTextDisconnected)
                                .arg(buttonTextStopped, buttonTextConnected, buttonTextDisconnected)
                                .arg(mcpToggle != nullptr).arg(mcpToggleConnected).arg(mcpToggleDisconnected));
            }

            // G112: codex exec の引数は stdio ブリッジを -c で渡し、cmd.exe 経由でも安全 (" を含まない)。
            {
                const QString editorPath = QStringLiteral("D:/x y/v-simple-editor.exe");
                const QStringList codexArgs = AiChatDock::buildCodexArguments(
                    8765, QStringLiteral("tok123"), editorPath, QString());
                const QStringList codexResume = AiChatDock::buildCodexArguments(
                    8765, QStringLiteral("tok123"), editorPath, QStringLiteral("thread-1"));
                bool noDoubleQuote = true;
                for (const QString& argument : codexArgs + codexResume) {
                    if (argument.contains(QLatin1Char('"')))
                        noDoubleQuote = false;
                }
                QString shellError;
                const QString shellLine = AiChatDock::buildShellCommandLine(
                    QStringLiteral("C:/npm/codex.cmd"), codexArgs, &shellError);
                const QString joined = codexArgs.join(QLatin1Char(' '));
                const bool g112 = observeTerm(112, "codexArgs.first() == QStringLiteral(\"exec\")", static_cast<bool>(codexArgs.first() == QStringLiteral("exec")))
                    && observeTerm(112, "codexArgs.contains(QStringLiteral(\"--json\"))", static_cast<bool>(codexArgs.contains(QStringLiteral("--json"))))
                    && observeTerm(112, "codexArgs.contains(QStringLiteral(\"--skip-git-repo-check\"))", static_cast<bool>(codexArgs.contains(QStringLiteral("--skip-git-repo-check"))))
                    && observeTerm(112, "joined.contains(QStringLiteral(\"mcp_servers.veditor.command='D:/x y/v-simple-editor.exe'\"))", static_cast<bool>(joined.contains(QStringLiteral("mcp_servers.veditor.command='D:/x y/v-simple-editor.exe'"))))
                    && observeTerm(112, "joined.contains(QStringLiteral(\"mcp_servers.veditor.args=['--mcp-stdio','--port','8765']\"))", static_cast<bool>(joined.contains(QStringLiteral("mcp_servers.veditor.args=['--mcp-stdio','--port','8765']"))))
                    && observeTerm(112, "joined.contains(QStringLiteral(\"mcp_servers.veditor.env={VEDITOR_MCP_TOKEN='tok123'}\"))", static_cast<bool>(joined.contains(QStringLiteral("mcp_servers.veditor.env={VEDITOR_MCP_TOKEN='tok123'}"))))
                    && observeTerm(112, "joined.contains(QStringLiteral(\"mcp_servers.veditor.default_tools_approval_mode='approve'\"))", static_cast<bool>(joined.contains(QStringLiteral("mcp_servers.veditor.default_tools_approval_mode='approve'"))))
                    && observeTerm(112, "codexArgs.last() == QStringLiteral(\"-\")", static_cast<bool>(codexArgs.last() == QStringLiteral("-")))
                    && observeTerm(112, "codexResume.at(1) == QStringLiteral(\"resume\")", static_cast<bool>(codexResume.at(1) == QStringLiteral("resume")))
                    && observeTerm(112, "codexResume.contains(QStringLiteral(\"thread-1\"))", static_cast<bool>(codexResume.contains(QStringLiteral("thread-1"))))
                    && observeTerm(112, "!codexResume.contains(QStringLiteral(\"-s\"))", static_cast<bool>(!codexResume.contains(QStringLiteral("-s"))))
                    && observeTerm(112, "noDoubleQuote", static_cast<bool>(noDoubleQuote)) && observeTerm(112, "!shellLine.isEmpty()", static_cast<bool>(!shellLine.isEmpty())) && observeTerm(112, "shellError.isEmpty()", static_cast<bool>(shellError.isEmpty()));
                g112 ? pass("G112 codex exec arguments carry the stdio bridge and pass the shell quoting rules")
                     : fail("G112 codex exec arguments carry the stdio bridge and pass the shell quoting rules",
                            QStringLiteral("args=%1 resume=%2 shellError=%3")
                                .arg(joined, codexResume.join(QLatin1Char(' ')), shellError));
            }

            if (autoStartBefore.isValid())
                prefSettings.setValue(QStringLiteral("mcpAutoStart"), autoStartBefore);
            else
                prefSettings.remove(QStringLiteral("mcpAutoStart"));
            if (buttonVisibleBefore.isValid())
                prefSettings.setValue(QStringLiteral("llmAssistantButtonVisible"), buttonVisibleBefore);
            else
                prefSettings.remove(QStringLiteral("llmAssistantButtonVisible"));
        }
    }

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
    const bool g37 = observeTerm(37, "commandIdsUnique", static_cast<bool>(commandIdsUnique))
        && observeTerm(37, "commandListResult.value(QStringLiteral(\"total\")).toInt() == commands.size()", static_cast<bool>(commandListResult.value(QStringLiteral("total")).toInt() == commands.size()));
    g37 ? pass("G37 list_commands IDs are unique")
        : fail("G37 list_commands IDs are unique",
               QStringLiteral("duplicate, empty, or missing command IDs were returned"));

    const bool g38 = observeTerm(38, "allRisksAssigned", static_cast<bool>(allRisksAssigned)) && observeTerm(38, "!quitId.isEmpty()", static_cast<bool>(!quitId.isEmpty())) && observeTerm(38, "exitIsQuit", static_cast<bool>(exitIsQuit));
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
    const bool g39 = observeTerm(39, "!blockingId.isEmpty()", static_cast<bool>(!blockingId.isEmpty()))
        && observeTerm(39, "blockingResult.value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(blockingResult.value(QStringLiteral("isError")).toBool(false)))
        && observeTerm(39, "blockingErrorText.contains(QStringLiteral(\"allowBlocking:true\"))", static_cast<bool>(blockingErrorText.contains(QStringLiteral("allowBlocking:true"))));
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
    const bool g40 = observeTerm(40, "!quitId.isEmpty()", static_cast<bool>(!quitId.isEmpty()))
        && observeTerm(40, "quitResult.value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(quitResult.value(QStringLiteral("isError")).toBool(false)))
        && observeTerm(40, "quitErrorText == QStringLiteral(\"このコマンドはエディタを終了させるため MCP からは実行できません。\")", static_cast<bool>(quitErrorText
               == QStringLiteral("このコマンドはエディタを終了させるため MCP からは実行できません。")));
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
        {QStringLiteral("set_track_locked"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("video")},
            {QStringLiteral("trackIndex"), 0}, {QStringLiteral("locked"), true}
        }},
        {QStringLiteral("match_frame"), QJsonObject{}},
        {QStringLiteral("replace_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0},
            {QStringLiteral("filePath"), QStringLiteral("missing.mp4")}
        }},
        {QStringLiteral("relink_media"), QJsonObject{
            {QStringLiteral("mapping"), QJsonArray{
                QJsonObject{{QStringLiteral("from"), QStringLiteral("old.mp4")},
                            {QStringLiteral("to"), QStringLiteral("new.mp4")}}
            }}
        }},
        {QStringLiteral("set_clip_property"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("property"), QStringLiteral("volume")},
            {QStringLiteral("value"), 1.0}
        }},
        {QStringLiteral("dynamic_zoom"), QJsonObject{
            {QStringLiteral("clipIndex"), 0},
            {QStringLiteral("preset"), QStringLiteral("zoomIn")}
        }},
        {QStringLiteral("trim_clip"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("edge"), QStringLiteral("in")},
            {QStringLiteral("timeSec"), 1.0}
        }},
        {QStringLiteral("set_transition"), QJsonObject{
            {QStringLiteral("clipIndex"), 0}, {QStringLiteral("type"), QStringLiteral("FadeOut")}
        }},
        {QStringLiteral("add_text_overlay"), QJsonObject{
            {QStringLiteral("text"), QStringLiteral("text")},
            {QStringLiteral("startSec"), 0.0}, {QStringLiteral("endSec"), 1.0}
        }}
    };
    bool nullWindowWriteToolsSafe = true;
    for (const QString& toolName : changedToolNames) {
        const QJsonObject response = callWriteTool(
            31, toolName, nullWindowWriteArguments.value(toolName));
        nullWindowWriteToolsSafe = observeTerm(31, "nullWindowWriteToolsSafe", static_cast<bool>(nullWindowWriteToolsSafe))
            && observeTerm(31, "response.value(QStringLiteral(\"result\")).toObject() .value(QStringLiteral(\"isError\")).toBool(false)", static_cast<bool>(response.value(QStringLiteral("result")).toObject()
                   .value(QStringLiteral("isError")).toBool(false)));
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
    const bool g35 = observeTerm(35, "defaultTimeout", static_cast<bool>(defaultTimeout)) && observeTerm(35, "overriddenTimeout", static_cast<bool>(overriddenTimeout)) && observeTerm(35, "tooShortFallsBack", static_cast<bool>(tooShortFallsBack));
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
    const bool g36 = observeTerm(36, "!noIdHadId", static_cast<bool>(!noIdHadId)) && observeTerm(36, "noId.isNull()", static_cast<bool>(noId.isNull())) && observeTerm(36, "nullIdHadId", static_cast<bool>(nullIdHadId)) && observeTerm(36, "nullId.isNull()", static_cast<bool>(nullId.isNull()));
    g36 ? pass("G36 request id absence versus null")
        : fail("G36 request id absence versus null",
               QStringLiteral("id presence was not distinguished"));

    if (!timelineReady) {
        fail("G159 track name/color readback and single undo", QStringLiteral("Timeline was not available"));
        fail("G160 track flags and invalid arguments", QStringLiteral("Timeline was not available"));
        fail("G156 color correction readback and single undo", QStringLiteral("Timeline was not available"));
        fail("G157 invalid color fields and hueSatWarp warning", QStringLiteral("Timeline was not available"));
        fail("G158 LUT apply clear and single-step undo", QStringLiteral("Timeline was not available"));
        fail("G154 compare_project detects move and volume", QStringLiteral("Timeline was not available"));
        fail("G155 compare_project unchanged and invalid inputs", QStringLiteral("Timeline was not available"));
    }
    // US-312 reserves G152-G153. Exercise the actual MCP handlers and renderer.
    if (timelineReady) {
        const auto savedState = projectTimeline->currentState();
        ClipInfo original{};
        original.filePath = QFileInfo(QStringLiteral("test_assets/e2e_clip.mp4")).absoluteFilePath();
        const auto duration = libavcore::probeDurationMicroseconds(original.filePath.toStdString());
        original.duration = duration ? double(*duration) / 1000000.0 : 0.0;
        original.inPoint = 1.0;
        original.outPoint = 2.0;
        original.effects.append(VideoEffect::createBrightnessContrast(12.0, 0.0));
        projectTimeline->restoreFromProject(QVector<QVector<ClipInfo>>{{original}},
            QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
        projectTimeline->undoManager()->clear();
        projectTimeline->saveUndoState(QStringLiteral("MCP焼き込み初期状態"));
        const QSize size(640, 360);
        const QImage before = tlrender::renderFrameAt(projectTimeline, 500000, size);
        // Same independent queue control as render-in-place G1, at the MCP
        // project's fps and source-native size. Measure the YUV/codec floor
        // instead of assuming a lossless RGB round trip on every encoder.
        QTemporaryDir controlOutput;
        QSize nativeSize;
        AVFormatContext *source = nullptr;
        if (avformat_open_input(&source, original.filePath.toUtf8().constData(), nullptr, nullptr) >= 0) {
            if (avformat_find_stream_info(source, nullptr) >= 0) {
                const int index = av_find_best_stream(source, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (index >= 0) {
                    const auto *parameters = source->streams[index]->codecpar;
                    nativeSize = QSize((parameters->width + 1) & ~1, (parameters->height + 1) & ~1);
                }
            }
            avformat_close_input(&source);
        }
        const double controlFps = toolPayload(callProjectInfoTool(451,
            QStringLiteral("get_project_info"), {})).value(QStringLiteral("fps")).toDouble();
        const QString controlPath = controlOutput.filePath(QStringLiteral("control.mp4"));
        const QString silentPath = controlOutput.filePath(QStringLiteral("silent.png"));
        QImage silent(2, 2, QImage::Format_RGB32);
        silent.fill(Qt::black);
        bool controlOk = false;
        QString controlError;
        if (controlOutput.isValid() && !nativeSize.isEmpty() && controlFps > 0.0 && silent.save(silentPath)) {
            RenderPreset preset{QStringLiteral("対照書き出し"), nativeSize.width(), nativeSize.height(),
                QStringLiteral("h264"), 100000000, QStringLiteral("mp4")};
            RenderJob job = RenderQueue::jobFromPreset(preset, controlPath, 0, 1000000);
            job.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            job.timeline = projectTimeline;
            job.projectFilePath = silentPath;
            job.exportConfig["fps"] = controlFps;
            RenderQueue queue;
            QEventLoop loop;
            bool done = false;
            QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
                [&](const QString &uuid, bool ok, const QString &message) {
                    if (uuid != job.uuid) return;
                    done = true;
                    controlOk = ok;
                    controlError = message;
                    loop.quit();
                });
            queue.addJob(job);
            queue.start();
            if (!done) loop.exec();
        }
        ClipInfo controlClip{};
        controlClip.filePath = controlPath;
        controlClip.duration = 1.0;
        Timeline controlTimeline;
        controlTimeline.restoreFromProject(QVector<QVector<ClipInfo>>{{controlClip}},
            QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
        const QImage control = tlrender::renderFrameAt(&controlTimeline, 500000, size);
        const QJsonObject target{{QStringLiteral("trackIndex"), 0}, {QStringLiteral("clipIndex"), 0}};
        QJsonObject renderArgs = target;
        renderArgs.insert(QStringLiteral("kind"), QStringLiteral("video"));
        const quint64 serial = projectTimeline->undoManager()->saveSerial();
        const auto rendered = toolPayload(callProjectInfoTool(452, QStringLiteral("render_in_place"), renderArgs));
        const QString path = rendered.value(QStringLiteral("outputPath")).toString();
        const auto timelineClip = [&]() {
            const auto payload = toolPayload(callProjectInfoTool(453, QStringLiteral("get_timeline"), {}));
            const auto tracks = payload.value(QStringLiteral("video")).toArray();
            const auto clips = tracks.isEmpty() ? QJsonArray{} : tracks.first().toObject().value(QStringLiteral("clips")).toArray();
            return clips.isEmpty() ? QJsonObject{} : clips.first().toObject();
        };
        const QJsonObject bakedClip = timelineClip();
        const QImage after = tlrender::renderFrameAt(projectTimeline, 500000, size);
        const auto imageMse = [](const QImage &before, const QImage &after) {
            double error = std::numeric_limits<double>::infinity();
            if (!before.isNull() && !after.isNull() && before.size() == after.size()) {
                const QImage a = before.convertToFormat(QImage::Format_RGB32);
                const QImage b = after.convertToFormat(QImage::Format_RGB32);
                double sum = 0.0;
                for (int y = 0; y < a.height(); ++y) {
                    const auto *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
                    const auto *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
                    for (int x = 0; x < a.width(); ++x) {
                        const double r = qRed(pa[x]) - qRed(pb[x]);
                        const double g = qGreen(pa[x]) - qGreen(pb[x]);
                        const double bl = qBlue(pa[x]) - qBlue(pb[x]);
                        sum += r*r + g*g + bl*bl;
                    }
                }
                error = sum / (3.0 * a.width() * a.height());
            }
            return error;
        };
        const double error = imageMse(before, after);
        const double floor = imageMse(before, control);
        const double residual = imageMse(control, after);
        constexpr double margin = 0.25; // Same measured-floor margin as G1.
        std::fprintf(stderr, "G152 fps=%.3f MSE=%.6f control=%.6f residual=%.6f margin=%.2f error=%s\n",
            controlFps, error, floor, residual, margin, qPrintable(controlError));
        const bool g152 = observeTerm(152, "duration", static_cast<bool>(duration)) && observeTerm(152, "*duration >= 2000000", static_cast<bool>(*duration >= 2000000))
            && observeTerm(152, "rendered.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(rendered.value(QStringLiteral("ok")).toBool()))
            && observeTerm(152, "rendered.value(QStringLiteral(\"replaced\")).toBool()", static_cast<bool>(rendered.value(QStringLiteral("replaced")).toBool()))
            && observeTerm(152, "rendered.value(QStringLiteral(\"linkedAudioReplaced\")).isBool()", static_cast<bool>(rendered.value(QStringLiteral("linkedAudioReplaced")).isBool()))
            && observeTerm(152, "!rendered.value(QStringLiteral(\"linkedAudioReplaced\")).toBool()", static_cast<bool>(!rendered.value(QStringLiteral("linkedAudioReplaced")).toBool()))
            && observeTerm(152, "QFileInfo::exists(path)", static_cast<bool>(QFileInfo::exists(path))) && observeTerm(152, "path.endsWith(QStringLiteral(\".mp4\"))", static_cast<bool>(path.endsWith(QStringLiteral(".mp4"))))
            && observeTerm(152, "path != original.filePath", static_cast<bool>(path != original.filePath)) && observeTerm(152, "bakedClip.value(QStringLiteral(\"filePath\")).toString() == path", static_cast<bool>(bakedClip.value(QStringLiteral("filePath")).toString() == path))
            && observeTerm(152, "bakedClip.value(QStringLiteral(\"effects\")).isArray()", static_cast<bool>(bakedClip.value(QStringLiteral("effects")).isArray()))
            && observeTerm(152, "bakedClip.value(QStringLiteral(\"effects\")).toArray().isEmpty()", static_cast<bool>(bakedClip.value(QStringLiteral("effects")).toArray().isEmpty()))
            && observeTerm(152, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1))
            && observeTerm(152, "controlOk", static_cast<bool>(controlOk)) && observeTerm(152, "std::isfinite(error)", static_cast<bool>(std::isfinite(error))) && observeTerm(152, "std::isfinite(floor)", static_cast<bool>(std::isfinite(floor)))
            && observeTerm(152, "error <= floor + margin", static_cast<bool>(error <= floor + margin)) && observeTerm(152, "residual <= margin", static_cast<bool>(residual <= margin));
        g152 ? pass("G152 render_in_place MCP picture and timeline")
             : fail("G152 render_in_place MCP picture and timeline",
                    QStringLiteral("MSE=%1 response=%2").arg(error).arg(QString::fromUtf8(compact(rendered))));
        if (!g152) {
            controlOutput.setAutoRemove(false);
            before.save(controlOutput.filePath(QStringLiteral("before.png")));
            after.save(controlOutput.filePath(QStringLiteral("after.png")));
            control.save(controlOutput.filePath(QStringLiteral("control.png")));
            std::fprintf(stderr, "G152 retained control=%s baked=%s\n", qPrintable(controlPath), qPrintable(path));
        }
        const auto decomposed = toolPayload(callProjectInfoTool(454, QStringLiteral("decompose_render_in_place"), target));
        const QJsonObject restored = timelineClip();
        bool g153 = observeTerm(153, "decomposed.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(decomposed.value(QStringLiteral("ok")).toBool()))
            && observeTerm(153, "restored.value(QStringLiteral(\"filePath\")).toString() == original.filePath", static_cast<bool>(restored.value(QStringLiteral("filePath")).toString() == original.filePath))
            && observeTerm(153, "restored.value(QStringLiteral(\"effects\")).toArray().size() == 1", static_cast<bool>(restored.value(QStringLiteral("effects")).toArray().size() == 1))
            && observeTerm(153, "restored.value(QStringLiteral(\"effects\")).toArray().first().toObject().value(QStringLiteral(\"param1\")).toDouble() == 12.0", static_cast<bool>(restored.value(QStringLiteral("effects")).toArray().first().toObject().value(QStringLiteral("param1")).toDouble() == 12.0));
        const auto undoDecompose = toolPayload(callProjectInfoTool(455, QStringLiteral("undo"), {}));
        g153 = observeTerm(153, "g153", static_cast<bool>(g153)) && observeTerm(153, "undoDecompose.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(undoDecompose.value(QStringLiteral("ok")).toBool()))
            && observeTerm(153, "timelineClip().value(QStringLiteral(\"filePath\")).toString() == path", static_cast<bool>(timelineClip().value(QStringLiteral("filePath")).toString() == path));
        const auto undoRender = toolPayload(callProjectInfoTool(456, QStringLiteral("undo"), {}));
        const auto undone = timelineClip();
        g153 = observeTerm(153, "g153", static_cast<bool>(g153)) && observeTerm(153, "undoRender.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(undoRender.value(QStringLiteral("ok")).toBool()))
            && observeTerm(153, "undone.value(QStringLiteral(\"filePath\")).toString() == original.filePath", static_cast<bool>(undone.value(QStringLiteral("filePath")).toString() == original.filePath))
            && observeTerm(153, "undone.value(QStringLiteral(\"effects\")) == restored.value(QStringLiteral(\"effects\"))", static_cast<bool>(undone.value(QStringLiteral("effects")) == restored.value(QStringLiteral("effects"))));
        g153 ? pass("G153 decompose and undo restore media and effects")
             : fail("G153 decompose and undo restore media and effects", QStringLiteral("復元または取り消しに失敗"));
        projectTimeline->restoreState(savedState);
        if (g152 && rendered.value(QStringLiteral("ok")).toBool() && path != original.filePath)
            QFile::remove(path);
    } else {
        fail("G152 render_in_place MCP picture and timeline", QStringLiteral("Timeline was not available"));
        fail("G153 decompose and undo restore media and effects", QStringLiteral("Timeline was not available"));
    }
    // US-509 reserved G166-G167: list and non-editing sequence switch.
    if (projectTimeline) {
        const auto savedState = projectTimeline->currentState();
        TimelineSequence root;
        root.id = QStringLiteral("main");
        root.name = QStringLiteral("メインシーケンス");
        root.videoTracks = savedState.videoTracks;
        root.audioTracks = savedState.audioTracks;
        TimelineSequence child;
        child.id = QStringLiteral("mcp-sequence-child");
        child.name = QStringLiteral("MCP 子シーケンス");
        child.videoTracks.append(QVector<ClipInfo>{});
        projectTimeline->setSequences(QVector<TimelineSequence>{root, child}, root.id);
        const auto listed = toolPayload(callProjectInfoTool(5090, QStringLiteral("get_sequences"), {}));
        const auto sequences = listed.value(QStringLiteral("sequences")).toArray();
        bool schemaRequired = false;
        for (const auto &value : projectInfoToolDescriptors) {
            const auto descriptor = value.toObject();
            if (descriptor.value(QStringLiteral("name")).toString() == QStringLiteral("get_sequences")) {
                const auto required = descriptor.value(QStringLiteral("outputSchema")).toObject()
                    .value(QStringLiteral("required")).toArray();
                schemaRequired = required.contains(QStringLiteral("ok"))
                    && required.contains(QStringLiteral("activeId"))
                    && required.contains(QStringLiteral("sequences"));
            }
        }
        const bool g166 = observeTerm(166, "listed.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(listed.value(QStringLiteral("ok")).toBool()))
            && observeTerm(166, "listed.value(QStringLiteral(\"activeId\")).toString() == root.id", static_cast<bool>(listed.value(QStringLiteral("activeId")).toString() == root.id))
            && observeTerm(166, "sequences.size() == 2", static_cast<bool>(sequences.size() == 2)) && observeTerm(166, "schemaRequired", static_cast<bool>(schemaRequired))
            && observeTerm(166, "sequences[1].toObject().value(QStringLiteral(\"id\")).toString() == child.id", static_cast<bool>(sequences[1].toObject().value(QStringLiteral("id")).toString() == child.id))
            && observeTerm(166, "sequences[1].toObject().value(QStringLiteral(\"name\")).toString() == child.name", static_cast<bool>(sequences[1].toObject().value(QStringLiteral("name")).toString() == child.name))
            && observeTerm(166, "sequences[1].toObject().value(QStringLiteral(\"videoTrackCount\")).toInt() == 1", static_cast<bool>(sequences[1].toObject().value(QStringLiteral("videoTrackCount")).toInt() == 1))
            && observeTerm(166, "sequences[1].toObject().value(QStringLiteral(\"clipCount\")).toInt(-1) == 0", static_cast<bool>(sequences[1].toObject().value(QStringLiteral("clipCount")).toInt(-1) == 0));
        g166 ? pass("G166 sequence list and schema")
             : fail("G166 sequence list and schema", QStringLiteral("sequence list/schema mismatch"));
        const auto serial = projectTimeline->undoManager()->saveSerial();
        const auto switched = toolPayload(callProjectInfoTool(5091, QStringLiteral("set_active_sequence"),
            QJsonObject{{"id", child.id}}));
        const auto missing = callProjectInfoTool(5092, QStringLiteral("set_active_sequence"),
            QJsonObject{{"id", QStringLiteral("missing-sequence")}});
        const auto noId = callProjectInfoTool(5093, QStringLiteral("set_active_sequence"), {});
        const auto after = toolPayload(callProjectInfoTool(5094, QStringLiteral("get_sequences"), {}));
        const auto activeTimeline = toolPayload(callProjectInfoTool(5095, QStringLiteral("get_timeline"), {}));
        const auto activeVideo = activeTimeline.value(QStringLiteral("video")).toArray();
        const bool g167 = observeTerm(167, "switched.value(QStringLiteral(\"ok\")).toBool()", static_cast<bool>(switched.value(QStringLiteral("ok")).toBool()))
            && observeTerm(167, "switched.value(QStringLiteral(\"activeId\")).toString() == child.id", static_cast<bool>(switched.value(QStringLiteral("activeId")).toString() == child.id))
            && observeTerm(167, "after.value(QStringLiteral(\"activeId\")).toString() == child.id", static_cast<bool>(after.value(QStringLiteral("activeId")).toString() == child.id))
            && observeTerm(167, "projectTimeline->activeSequenceId() == child.id", static_cast<bool>(projectTimeline->activeSequenceId() == child.id))
            && observeTerm(167, "!activeVideo.isEmpty()", static_cast<bool>(!activeVideo.isEmpty()))
            && observeTerm(167, "activeVideo[0].toObject().value(QStringLiteral(\"clips\")).toArray().isEmpty()", static_cast<bool>(activeVideo[0].toObject().value(QStringLiteral("clips")).toArray().isEmpty()))
            && observeTerm(167, "requiredOutputFieldsPresent(QStringLiteral(\"set_active_sequence\"), switched)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("set_active_sequence"), switched)))
            && observeTerm(167, "projectTimeline->undoManager()->saveSerial() == serial", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial))
            && observeTerm(167, "toolResult(missing).value(QStringLiteral(\"isError\")).toBool()", static_cast<bool>(toolResult(missing).value(QStringLiteral("isError")).toBool()))
            && observeTerm(167, "toolResult(noId).value(QStringLiteral(\"isError\")).toBool()", static_cast<bool>(toolResult(noId).value(QStringLiteral("isError")).toBool()));
        g167 ? pass("G167 sequence switch without undo and invalid id")
             : fail("G167 sequence switch without undo and invalid id", QStringLiteral("switch/undo/validation mismatch"));
        projectTimeline->restoreState(savedState);
    } else {
        fail("G166 sequence list and schema", QStringLiteral("Timeline was not available"));
        fail("G167 sequence switch without undo and invalid id", QStringLiteral("Timeline was not available"));
    }
    // US-511 reserved G168-G170: structural operations through the real MCP transport.
    if (projectTimeline) {
        const auto savedState = projectTimeline->currentState();
        int requestId = 51100;
        auto call = [&](const QString &name, const QJsonObject &args) {
            return callProjectInfoTool(++requestId, name, args);
        };
        auto tracks = [&]() {
            return toolPayload(call(QStringLiteral("get_timeline"), {})).value(QStringLiteral("video")).toArray();
        };
        auto baseline = [&]() {
            projectTimeline->undoManager()->clear();
            projectTimeline->undoManager()->saveState(projectTimeline->currentState(), QStringLiteral("基準"));
        };
        const QVector<QVector<ClipInfo>> empty{{}};
        Timeline cleanTimeline;
        projectTimeline->restoreState(cleanTimeline.currentState());
        baseline();
        auto serial = projectTimeline->undoManager()->saveSerial();
        const auto added = toolPayload(call(QStringLiteral("add_track"), {{"kind", "video"}}));
        bool g168 = observeTerm(168, "added.value(\"ok\").toBool()", static_cast<bool>(added.value("ok").toBool())) && observeTerm(168, "added.value(\"trackIndex\").toInt(-1) == 1", static_cast<bool>(added.value("trackIndex").toInt(-1) == 1))
            && observeTerm(168, "added.value(\"trackCount\").toInt() == 2", static_cast<bool>(added.value("trackCount").toInt() == 2)) && observeTerm(168, "tracks().size() == 2", static_cast<bool>(tracks().size() == 2))
            && observeTerm(168, "requiredOutputFieldsPresent(QStringLiteral(\"add_track\"), added)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("add_track"), added)))
            && observeTerm(168, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1));
        call(QStringLiteral("undo"), {});
        g168 = observeTerm(168, "g168", static_cast<bool>(g168)) && observeTerm(168, "tracks().size() == 1", static_cast<bool>(tracks().size() == 1)) && observeTerm(168, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()));
        serial = projectTimeline->undoManager()->saveSerial();
        const auto audioAdded = toolPayload(call(QStringLiteral("add_track"), {{"kind", "audio"}}));
        g168 = observeTerm(168, "g168", static_cast<bool>(g168)) && observeTerm(168, "audioAdded.value(\"ok\").toBool()", static_cast<bool>(audioAdded.value("ok").toBool())) && observeTerm(168, "audioAdded.value(\"trackCount\").toInt() == 2", static_cast<bool>(audioAdded.value("trackCount").toInt() == 2))
            && observeTerm(168, "projectTimeline->audioTrackCount() == 2", static_cast<bool>(projectTimeline->audioTrackCount() == 2))
            && observeTerm(168, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1));
        call(QStringLiteral("undo"), {});
        g168 = observeTerm(168, "g168", static_cast<bool>(g168)) && observeTerm(168, "projectTimeline->audioTrackCount() == 1", static_cast<bool>(projectTimeline->audioTrackCount() == 1)) && observeTerm(168, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()));
        const auto invalidAdd = call(QStringLiteral("add_track"), {{"kind", "invalid"}});
        g168 = observeTerm(168, "g168", static_cast<bool>(g168)) && observeTerm(168, "toolResult(invalidAdd).value(\"isError\").toBool()", static_cast<bool>(toolResult(invalidAdd).value("isError").toBool()));
        g168 ? pass("G168 add_track and single undo")
             : fail("G168 add_track and single undo", QStringLiteral("count/undo/schema mismatch"));

        ClipInfo middle, last;
        middle.filePath = QStringLiteral("mcp-track-middle.mov");
        middle.displayName = QStringLiteral("middle");
        middle.duration = middle.outPoint = 2.0;
        last = middle;
        last.filePath = QStringLiteral("mcp-track-last.mov");
        last.displayName = QStringLiteral("last");
        const QVector<QVector<ClipInfo>> rows{{}, {middle}, {last}};
        projectTimeline->restoreFromProject(rows, empty, 0.0, -1.0, -1.0, 100);
        baseline();
        const auto original = tracks();
        serial = projectTimeline->undoManager()->saveSerial();
        const auto removed = toolPayload(call(QStringLiteral("remove_track"), {{"kind", "video"}, {"trackIndex", 1}}));
        const auto afterRemove = tracks();
        bool g169 = observeTerm(169, "removed.value(\"ok\").toBool()", static_cast<bool>(removed.value("ok").toBool())) && observeTerm(169, "removed.value(\"trackCount\").toInt() == 2", static_cast<bool>(removed.value("trackCount").toInt() == 2))
            && observeTerm(169, "requiredOutputFieldsPresent(QStringLiteral(\"remove_track\"), removed)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("remove_track"), removed)))
            && observeTerm(169, "afterRemove.size() == 2", static_cast<bool>(afterRemove.size() == 2))
            && observeTerm(169, "afterRemove[1].toObject().value(\"clips\") == original[2].toObject().value(\"clips\")", static_cast<bool>(afterRemove[1].toObject().value("clips") == original[2].toObject().value("clips")))
            && observeTerm(169, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1));
        call(QStringLiteral("undo"), {});
        g169 = observeTerm(169, "g169", static_cast<bool>(g169)) && observeTerm(169, "tracks() == original", static_cast<bool>(tracks() == original)) && observeTerm(169, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()));
        projectTimeline->trackAt(false, 1)->setLocked(true);
        const auto locked = call(QStringLiteral("remove_track"), {{"kind", "video"}, {"trackIndex", 1}});
        g169 = observeTerm(169, "g169", static_cast<bool>(g169)) && observeTerm(169, "toolResult(locked).value(\"isError\").toBool()", static_cast<bool>(toolResult(locked).value("isError").toBool())) && observeTerm(169, "tracks().size() == 3", static_cast<bool>(tracks().size() == 3));
        projectTimeline->trackAt(false, 1)->setLocked(false);
        const auto lastTrack = call(QStringLiteral("remove_track"), {{"kind", "audio"}, {"trackIndex", 0}});
        g169 = observeTerm(169, "g169", static_cast<bool>(g169)) && observeTerm(169, "toolResult(lastTrack).value(\"isError\").toBool()", static_cast<bool>(toolResult(lastTrack).value("isError").toBool())) && observeTerm(169, "projectTimeline->audioTrackCount() == 1", static_cast<bool>(projectTimeline->audioTrackCount() == 1));
        g169 ? pass("G169 remove_track clips, undo and rejection")
             : fail("G169 remove_track clips, undo and rejection", QStringLiteral("remove/undo/validation mismatch"));

        projectTimeline->setTrackAppearance(false, 1, QStringLiteral("中央"), QColor(QStringLiteral("#123abc")));
        projectTimeline->setTrackAppearance(false, 2, QStringLiteral("末尾"), QColor(QStringLiteral("#abcdef")));
        baseline();
        const auto beforeMove = tracks();
        serial = projectTimeline->undoManager()->saveSerial();
        const auto moved = toolPayload(call(QStringLiteral("move_track"),
            {{"kind", "video"}, {"trackIndex", 1}, {"newTrackIndex", 2}}));
        const auto afterMove = tracks();
        bool g170 = observeTerm(170, "moved.value(\"ok\").toBool()", static_cast<bool>(moved.value("ok").toBool())) && observeTerm(170, "moved.value(\"trackIndex\").toInt(-1) == 2", static_cast<bool>(moved.value("trackIndex").toInt(-1) == 2))
            && observeTerm(170, "requiredOutputFieldsPresent(QStringLiteral(\"move_track\"), moved)", static_cast<bool>(requiredOutputFieldsPresent(QStringLiteral("move_track"), moved)))
            && observeTerm(170, "afterMove.size() == 3", static_cast<bool>(afterMove.size() == 3))
            && observeTerm(170, "afterMove[1].toObject().value(\"name\").toString() == QStringLiteral(\"末尾\")", static_cast<bool>(afterMove[1].toObject().value("name").toString() == QStringLiteral("末尾")))
            && observeTerm(170, "afterMove[2].toObject().value(\"name\").toString() == QStringLiteral(\"中央\")", static_cast<bool>(afterMove[2].toObject().value("name").toString() == QStringLiteral("中央")))
            && observeTerm(170, "afterMove[2].toObject().value(\"color\").toString() == QStringLiteral(\"#123abc\")", static_cast<bool>(afterMove[2].toObject().value("color").toString() == QStringLiteral("#123abc")))
            && observeTerm(170, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1));
        call(QStringLiteral("undo"), {});
        g170 = observeTerm(170, "g170", static_cast<bool>(g170)) && observeTerm(170, "tracks() == beforeMove", static_cast<bool>(tracks() == beforeMove)) && observeTerm(170, "!projectTimeline->canUndo()", static_cast<bool>(!projectTimeline->canUndo()));
        serial = projectTimeline->undoManager()->saveSerial();
        for (const QJsonObject &args : {QJsonObject{{"kind", "video"}, {"trackIndex", 1}, {"newTrackIndex", 3}},
                QJsonObject{{"kind", "video"}, {"trackIndex", 3}, {"newTrackIndex", 0}},
                QJsonObject{{"kind", "video"}, {"trackIndex", 1}, {"newTrackIndex", -1}},
                QJsonObject{{"kind", "video"}, {"trackIndex", 1}}}) {
            const auto rejected = call(QStringLiteral("move_track"), args);
            g170 = observeTerm(170, "g170", static_cast<bool>(g170)) && observeTerm(170, "toolResult(rejected).value(\"isError\").toBool()", static_cast<bool>(toolResult(rejected).value("isError").toBool()));
        }
        g170 = observeTerm(170, "g170", static_cast<bool>(g170)) && observeTerm(170, "tracks() == beforeMove", static_cast<bool>(tracks() == beforeMove)) && observeTerm(170, "serial == projectTimeline->undoManager()->saveSerial()", static_cast<bool>(serial == projectTimeline->undoManager()->saveSerial()));
        g170 ? pass("G170 move_track appearance, undo and bounds")
             : fail("G170 move_track appearance, undo and bounds", QStringLiteral("move/undo/validation mismatch"));
        projectTimeline->restoreState(savedState);
    } else {
        fail("G168 add_track and single undo", QStringLiteral("Timeline was not available"));
        fail("G169 remove_track clips, undo and rejection", QStringLiteral("Timeline was not available"));
        fail("G170 move_track appearance, undo and bounds", QStringLiteral("Timeline was not available"));
    }
    // US-600: the real transport must validate arguments and record one undo.
    if (projectTimeline) {
        const auto saved = projectTimeline->currentState();
        Timeline clean;
        projectTimeline->restoreState(clean.currentState());
        ClipInfo clip;
        clip.filePath = QStringLiteral("clip-enable-test.mp4");
        clip.duration = clip.outPoint = 1.0;
        projectTimeline->videoTracks().first()->setClips({clip});
        projectTimeline->audioTracks().first()->setClips({clip});
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(projectTimeline->currentState(), QStringLiteral("基準"));
        int requestId = 60000;
        auto call = [&](const QString &name, const QJsonObject &args) {
            return callProjectInfoTool(++requestId, name, args);
        };
        bool good = true;
        for (const QString &kind : {QStringLiteral("video"), QStringLiteral("audio")}) {
            const auto serial = projectTimeline->undoManager()->saveSerial();
            const auto response = toolPayload(call(QStringLiteral("set_clip_enabled"),
                {{"kind", kind}, {"clipIndex", 0}, {"enabled", false}}));
            good = observeTerm(171, "good", static_cast<bool>(good)) && observeTerm(171, "response.value(\"ok\").toBool()", static_cast<bool>(response.value("ok").toBool())) && observeTerm(171, "response.value(\"undoRecorded\").toBool()", static_cast<bool>(response.value("undoRecorded").toBool()))
                && observeTerm(171, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1));
            const auto tracks = toolPayload(call(QStringLiteral("get_timeline"), {}))
                .value(kind).toArray();
            const auto clips = tracks.isEmpty() ? QJsonArray{}
                : tracks.first().toObject().value("clips").toArray();
            good = observeTerm(171, "good", static_cast<bool>(good)) && observeTerm(171, "!clips.isEmpty()", static_cast<bool>(!clips.isEmpty())) && observeTerm(171, "clips.first().toObject().contains(\"enabled\")", static_cast<bool>(clips.first().toObject().contains("enabled")))
                && observeTerm(171, "!clips.first().toObject().value(\"enabled\").toBool(true)", static_cast<bool>(!clips.first().toObject().value("enabled").toBool(true)));
            projectTimeline->undo();
            const auto *track = kind == QStringLiteral("audio")
                ? projectTimeline->audioTracks().first() : projectTimeline->videoTracks().first();
            good = observeTerm(171, "good", static_cast<bool>(good)) && observeTerm(171, "track->clips().first().visible", static_cast<bool>(track->clips().first().visible));
        }
        for (const QJsonObject &args : {
                QJsonObject{{"clipIndex", 99}, {"enabled", false}},
                QJsonObject{{"clipIndex", 0}, {"enabled", "false"}}})
            good = observeTerm(171, "toolResult(call(QStringLiteral(\"set_clip_enabled\"), args)).value(\"isError\").toBool()", static_cast<bool>(toolResult(call(QStringLiteral("set_clip_enabled"), args)).value("isError").toBool())) && observeTerm(171, "good", static_cast<bool>(good));
        clip.filePath = clipgeom::nullObjectFilePath();
        clip.visible = false;
        projectTimeline->videoTracks().first()->setClips({clip});
        good = observeTerm(171, "toolResult(call(QStringLiteral(\"set_clip_enabled\"), {{\"clipIndex\", 0}, {\"enabled\", true}})).value(\"isError\").toBool()", static_cast<bool>(toolResult(call(QStringLiteral("set_clip_enabled"),
            {{"clipIndex", 0}, {"enabled", true}})).value("isError").toBool())) && observeTerm(171, "good", static_cast<bool>(good));
        good ? pass("G171 set_clip_enabled")
             : fail("G171 set_clip_enabled", QStringLiteral("enable/undo/schema/validation mismatch"));
        projectTimeline->restoreState(saved);
    } else {
        fail("G171 set_clip_enabled", QStringLiteral("Timeline was not available"));
    }
    // US-605: real transport validation and atomic grouping/ungrouping.
    if (projectTimeline) {
        const TimelineState saved = projectTimeline->currentState();
        ClipInfo clip;
        clip.filePath = QStringLiteral("clip-group-test.mp4");
        clip.duration = clip.outPoint = 2.0;
        projectTimeline->restoreFromProject(QVector<QVector<ClipInfo>>{{clip, clip}, {clip}},
            QVector<QVector<ClipInfo>>{{clip}}, 0.0, -1.0, -1.0, 100);
        projectTimeline->undoManager()->clear();
        projectTimeline->undoManager()->saveState(projectTimeline->currentState(), QStringLiteral("基準"));
        int requestId = 60500;
        auto call = [&](const QString &name, const QJsonArray &clips) {
            return callProjectInfoTool(++requestId, name, {{"clips", clips}});
        };
        const QJsonObject v1{{"kind", "video"}, {"trackIndex", 0}, {"clipIndex", 0}};
        const QJsonObject v2{{"kind", "video"}, {"trackIndex", 1}, {"clipIndex", 0}};
        const QJsonObject a1{{"kind", "audio"}, {"trackIndex", 0}, {"clipIndex", 0}};
        const QJsonArray targets{v1, v2, a1};
        const auto before = projectTimeline->undoManager()->saveSerial();
        bool good = true;
        for (const auto &invalid : {QJsonArray{}, QJsonArray{v1}, QJsonArray{v1, v1},
                QJsonArray{v1, QJsonObject{{"kind", "video"}, {"trackIndex", 0}, {"clipIndex", 1}}}})
            good = observeTerm(172, "toolResult(call(QStringLiteral(\"group_clips\"), invalid)).value(\"isError\").toBool()", static_cast<bool>(toolResult(call(QStringLiteral("group_clips"), invalid)).value("isError").toBool())) && observeTerm(172, "good", static_cast<bool>(good));
        good = observeTerm(172, "good", static_cast<bool>(good)) && observeTerm(172, "projectTimeline->undoManager()->saveSerial() == before", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == before));
        const auto grouped = toolPayload(call(QStringLiteral("group_clips"), targets));
        const int group = grouped.value("linkGroup").toInt();
        good = observeTerm(172, "good", static_cast<bool>(good)) && observeTerm(172, "grouped.value(\"ok\").toBool()", static_cast<bool>(grouped.value("ok").toBool())) && observeTerm(172, "group > 0", static_cast<bool>(group > 0))
            && observeTerm(172, "grouped.value(\"undoRecorded\").toBool()", static_cast<bool>(grouped.value("undoRecorded").toBool()))
            && observeTerm(172, "projectTimeline->undoManager()->saveSerial() == before + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == before + 1))
            && observeTerm(172, "projectTimeline->videoTracks()[0]->clips()[0].linkGroup == group", static_cast<bool>(projectTimeline->videoTracks()[0]->clips()[0].linkGroup == group))
            && observeTerm(172, "projectTimeline->videoTracks()[1]->clips()[0].linkGroup == group", static_cast<bool>(projectTimeline->videoTracks()[1]->clips()[0].linkGroup == group))
            && observeTerm(172, "projectTimeline->audioTracks()[0]->clips()[0].linkGroup == group", static_cast<bool>(projectTimeline->audioTracks()[0]->clips()[0].linkGroup == group))
            && observeTerm(172, "projectTimeline->videoTracks()[0]->clips()[1].linkGroup == 0", static_cast<bool>(projectTimeline->videoTracks()[0]->clips()[1].linkGroup == 0));
        good ? pass("G172 group_clips")
             : fail("G172 group_clips", QStringLiteral("group validation/identity/undo mismatch"));
        const auto serial = projectTimeline->undoManager()->saveSerial();
        good = toolResult(call(QStringLiteral("ungroup_clips"), QJsonArray{v1,
            QJsonObject{{"kind", "audio"}, {"trackIndex", 0}, {"clipIndex", 99}}}))
            .value("isError").toBool();
        good = observeTerm(173, "good", static_cast<bool>(good)) && observeTerm(173, "projectTimeline->undoManager()->saveSerial() == serial", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial))
            && observeTerm(173, "projectTimeline->videoTracks()[0]->clips()[0].linkGroup == group", static_cast<bool>(projectTimeline->videoTracks()[0]->clips()[0].linkGroup == group));
        const auto ungrouped = toolPayload(call(QStringLiteral("ungroup_clips"), targets));
        good = observeTerm(173, "good", static_cast<bool>(good)) && observeTerm(173, "ungrouped.value(\"ok\").toBool()", static_cast<bool>(ungrouped.value("ok").toBool())) && observeTerm(173, "ungrouped.value(\"undoRecorded\").toBool()", static_cast<bool>(ungrouped.value("undoRecorded").toBool()))
            && observeTerm(173, "ungrouped.value(\"linkGroup\").toInt(-1) == 0", static_cast<bool>(ungrouped.value("linkGroup").toInt(-1) == 0))
            && observeTerm(173, "projectTimeline->undoManager()->saveSerial() == serial + 1", static_cast<bool>(projectTimeline->undoManager()->saveSerial() == serial + 1))
            && observeTerm(173, "projectTimeline->videoTracks()[0]->clips()[0].linkGroup == 0", static_cast<bool>(projectTimeline->videoTracks()[0]->clips()[0].linkGroup == 0))
            && observeTerm(173, "projectTimeline->videoTracks()[1]->clips()[0].linkGroup == 0", static_cast<bool>(projectTimeline->videoTracks()[1]->clips()[0].linkGroup == 0))
            && observeTerm(173, "projectTimeline->audioTracks()[0]->clips()[0].linkGroup == 0", static_cast<bool>(projectTimeline->audioTracks()[0]->clips()[0].linkGroup == 0));
        projectTimeline->undo();
        good = observeTerm(173, "good", static_cast<bool>(good)) && observeTerm(173, "projectTimeline->videoTracks()[0]->clips()[0].linkGroup == group", static_cast<bool>(projectTimeline->videoTracks()[0]->clips()[0].linkGroup == group))
            && observeTerm(173, "projectTimeline->videoTracks()[1]->clips()[0].linkGroup == group", static_cast<bool>(projectTimeline->videoTracks()[1]->clips()[0].linkGroup == group))
            && observeTerm(173, "projectTimeline->audioTracks()[0]->clips()[0].linkGroup == group", static_cast<bool>(projectTimeline->audioTracks()[0]->clips()[0].linkGroup == group));
        good ? pass("G173 ungroup_clips")
             : fail("G173 ungroup_clips", QStringLiteral("ungroup validation/atomicity/undo mismatch"));
        projectTimeline->restoreState(saved);
    } else {
        fail("G172 group_clips", QStringLiteral("Timeline was not available"));
        fail("G173 ungroup_clips", QStringLiteral("Timeline was not available"));
    }
    // US-900 G178-G180: the MCP export_video mix (ducking_mix_*.m4a) is registered in the
    // job's tempFiles and removed by RenderQueue at job end. The job is read synchronously
    // from RenderQueue::jobsChanged (connected before the call) because removeTempFiles
    // drops the key at job end.
    {
        const QString ffmpegExe = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        const QString videoAsset = QDir::current().absoluteFilePath(
            QStringLiteral("test_assets/e2e_clip.mp4"));
        auto *queue = projectInfoWindow.findChild<RenderQueue*>(QStringLiteral("mainRenderQueue"));
        const bool g178Ready = timelineReady && queue && QFileInfo(videoAsset).isFile()
            && exportRootReady;
        if (!g178Ready) {
            const QString why = QStringLiteral("fixture missing: timeline=%1 queue=%2 asset=%3 exportRoot=%4")
                .arg(timelineReady).arg(queue != nullptr).arg(QFileInfo(videoAsset).isFile())
                .arg(exportRootReady);
            for (int n = 178; n <= 180; ++n)
                tally.gate(n, false, why.toUtf8().constData());
        } else if (ffmpegExe.isEmpty()) {
            for (int n = 178; n <= 180; ++n)
                tally.skip(n, "ffmpeg.exe unavailable");
        } else {
            const QString tonePath = exportRoot.filePath(QStringLiteral("us900-tone.wav"));
            std::vector<float> toneSamples(48000 * 2, 0.1f);
            QString toneError;
            const bool toneOk = libavcore::writeWavFloatInterleaved(tonePath, toneSamples, 48000, 2, &toneError);
            // A separate audio file makes the passthrough shortcut inapplicable, so a mix is built.
            ClipInfo videoClip;
            videoClip.filePath = videoAsset;
            videoClip.displayName = QStringLiteral("us900-video");
            videoClip.duration = videoClip.outPoint = 1.0;
            ClipInfo audioClip;
            audioClip.filePath = tonePath;
            audioClip.displayName = QStringLiteral("us900-audio");
            audioClip.duration = audioClip.outPoint = 1.0;
            TimelineTrack *videoTrack = projectTimeline->videoTracks().first();
            TimelineTrack *audioTrack = projectTimeline->audioTracks().first();
            const QVector<ClipInfo> savedVideoClips = videoTrack->clips();
            QVector<QPair<TimelineTrack *, QVector<ClipInfo>>> savedAudioClips;
            for (TimelineTrack *track : projectTimeline->audioTracks()) {
                savedAudioClips.append(qMakePair(track, track->clips()));
                track->setClips(QVector<ClipInfo>{});
            }
            videoTrack->setClips(QVector<ClipInfo>{videoClip});
            audioTrack->setClips(QVector<ClipInfo>{audioClip});
            queue->clear();
            auto *previousTools = projectInfoWindow.m_mcpTools;
            projectInfoWindow.m_mcpTools = &projectInfoTools;
            const QString mixDirPath = QDir::tempPath() + QStringLiteral("/v-simple-editor-audio-mix");

            // G178 / G179: one successful export_video call.
            RenderJob capturedJob;
            bool captured = false;
            bool mixExistedAtAdd = false;
            QString mixPath;
            const auto connection = QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                if (captured) return;
                const auto jobs = queue->jobs();
                if (jobs.isEmpty()) return;
                capturedJob = jobs.first();
                captured = true;
                const QJsonArray files = capturedJob.exportConfig.value(QStringLiteral("tempFiles")).toArray();
                for (const QJsonValue &file : files) {
                    const QString name = QFileInfo(file.toString()).fileName();
                    if (name.startsWith(QStringLiteral("ducking_mix_")) && name.endsWith(QStringLiteral(".m4a")))
                        mixPath = file.toString();
                }
                mixExistedAtAdd = !mixPath.isEmpty() && QFile::exists(mixPath);
            });
            const QJsonObject okResponse = callProjectInfoTool(1780, QStringLiteral("export_video"),
                QJsonObject{{QStringLiteral("outputPath"), exportRoot.filePath(QStringLiteral("us900-ok.mp4"))}});
            const QString okJobId = toolPayload(okResponse).value(QStringLiteral("jobId")).toString();
            const bool okIsError = toolResult(okResponse).value(QStringLiteral("isError")).toBool(true);
            const bool addedJob = QTest::qWaitFor([&]() { return captured; }, 60000);
            const auto jobTerminal = [&]() {
                for (const RenderJob &job : queue->jobs()) {
                    if (job.uuid != okJobId) continue;
                    return job.status == RenderJobStatus::Completed || job.status == RenderJobStatus::Failed
                        || job.status == RenderJobStatus::Cancelled;
                }
                return false;
            };
            const bool terminal = addedJob && QTest::qWaitFor(jobTerminal, 120000);
            QObject::disconnect(connection);
            const bool mixGoneAtEnd = !mixPath.isEmpty() && !QFile::exists(mixPath);
            const bool g178 = observeTerm(178, "toneOk", static_cast<bool>(toneOk)) && observeTerm(178, "!okIsError", static_cast<bool>(!okIsError)) && observeTerm(178, "!okJobId.isEmpty()", static_cast<bool>(!okJobId.isEmpty())) && observeTerm(178, "captured", static_cast<bool>(captured))
                && observeTerm(178, "capturedJob.uuid == okJobId", static_cast<bool>(capturedJob.uuid == okJobId)) && observeTerm(178, "mixExistedAtAdd", static_cast<bool>(mixExistedAtAdd)) && observeTerm(178, "terminal", static_cast<bool>(terminal)) && observeTerm(178, "mixGoneAtEnd", static_cast<bool>(mixGoneAtEnd));
            std::fprintf(stderr, "mcp G178 toneOk=%d isError=%d jobId=[%s] addedJob=%d captured=%d capturedUuid=[%s] mixPath=[%s] "
                "existedAtAdd=%d terminal=%d existsAtEnd=%d\n", toneOk, okIsError, qPrintable(okJobId),
                addedJob, captured, qPrintable(capturedJob.uuid), qPrintable(mixPath), mixExistedAtAdd, terminal,
                !mixPath.isEmpty() && QFile::exists(mixPath));
            tallyObserved(178, g178, "the export_video audio mix was not registered in tempFiles or survived the job end");

            const QJsonArray capturedTemp = capturedJob.exportConfig.value(QStringLiteral("tempFiles")).toArray();
            const bool tempHasMix = capturedTemp.contains(mixPath) && !mixPath.isEmpty();
            const bool hasSourceKey = capturedJob.exportConfig.contains(QStringLiteral("sourceProjectFilePath"));
            const QString sourceProject = capturedJob.exportConfig.value(QStringLiteral("sourceProjectFilePath")).toString();
            const QString queuePath = exportRoot.filePath(QStringLiteral("us900-queue.json"));
            bool saved = false;
            QByteArray savedBytes;
            {
                RenderQueue saveProbe;
                saveProbe.addJob(capturedJob);
                saved = saveProbe.saveQueue(queuePath);
            }
            {
                QFile savedFile(queuePath);
                if (savedFile.open(QIODevice::ReadOnly)) savedBytes = savedFile.readAll();
            }
            const QString mixName = QFileInfo(mixPath).fileName();
            const bool savedHasMix = !mixName.isEmpty() && savedBytes.contains(mixName.toUtf8());
            std::fprintf(stderr, "mcp G179 tempFilesHasMix=%d hasSourceKey=%d sourceProjectFilePath=[%s] "
                "projectFilePathAtAdd=[%s] saved=%d savedBytes=%lld savedHasMixName=%d mixName=[%s]\n",
                tempHasMix, hasSourceKey, qPrintable(sourceProject), qPrintable(capturedJob.projectFilePath),
                saved, static_cast<long long>(savedBytes.size()), savedHasMix, qPrintable(mixName));
            tallyObserved(179, observeTerm(179, "tempHasMix", static_cast<bool>(tempHasMix)) && observeTerm(179, "hasSourceKey", static_cast<bool>(hasSourceKey)) && observeTerm(179, "sourceProject.isEmpty()", static_cast<bool>(sourceProject.isEmpty()))
                && observeTerm(179, "capturedJob.projectFilePath == mixPath", static_cast<bool>(capturedJob.projectFilePath == mixPath)) && observeTerm(179, "saved", static_cast<bool>(saved)) && observeTerm(179, "!savedBytes.isEmpty()", static_cast<bool>(!savedBytes.isEmpty())) && observeTerm(179, "!savedHasMix", static_cast<bool>(!savedHasMix)),
                "the MCP mix was not tracked as a temp intermediate or leaked into the saved queue");
            queue->clear();

            // G180: regression guard for the existing failure cleanup in finishMix (not a
            // negative control for the US-900 change): a mix that fails after ffmpeg wrote
            // the file leaves no ducking_mix_* file behind and the job reports failed.
            const auto mixFiles = [&]() {
                QSet<QString> names;
                for (const QString &name : QDir(mixDirPath).entryList(QStringList{QStringLiteral("ducking_mix_*")},
                                                                      QDir::Files))
                    names.insert(name);
                return names;
            };
            const QSet<QString> mixBefore = mixFiles();
            exportaudio::failAfterWriteForTest = true;
            exportaudio::outputExistedBeforeCleanupForTest = false;
            const QJsonObject failResponse = callProjectInfoTool(1781, QStringLiteral("export_video"),
                QJsonObject{{QStringLiteral("outputPath"), exportRoot.filePath(QStringLiteral("us900-fail.mp4"))}});
            const QString failJobId = toolPayload(failResponse).value(QStringLiteral("jobId")).toString();
            QString failStatus, failError;
            int pollId = 1782;
            const bool failed = QTest::qWaitFor([&]() {
                const QJsonObject status = toolPayload(callProjectInfoTool(pollId++,
                    QStringLiteral("get_export_status"), QJsonObject{{QStringLiteral("jobId"), failJobId}}));
                failStatus = status.value(QStringLiteral("status")).toString();
                failError = status.value(QStringLiteral("error")).toString();
                return failStatus == QStringLiteral("failed");
            }, 60000);
            exportaudio::failAfterWriteForTest = false;
            const bool existedBeforeCleanup = exportaudio::outputExistedBeforeCleanupForTest;
            exportaudio::outputExistedBeforeCleanupForTest = false;
            QSet<QString> leftover = mixFiles();
            leftover.subtract(mixBefore);
            std::fprintf(stderr, "mcp G180 jobId=[%s] status=[%s] error=[%s] existedBeforeCleanup=%d "
                "newMixFiles=%lld jobsInQueue=%lld\n", qPrintable(failJobId), qPrintable(failStatus),
                qPrintable(failError), existedBeforeCleanup, static_cast<long long>(leftover.size()),
                static_cast<long long>(queue->jobs().size()));
            tallyObserved(180, observeTerm(180, "!failJobId.isEmpty()", static_cast<bool>(!failJobId.isEmpty())) && observeTerm(180, "failed", static_cast<bool>(failed)) && observeTerm(180, "existedBeforeCleanup", static_cast<bool>(existedBeforeCleanup)) && observeTerm(180, "leftover.isEmpty()", static_cast<bool>(leftover.isEmpty()))
                && observeTerm(180, "queue->jobs().isEmpty()", static_cast<bool>(queue->jobs().isEmpty())),
                "a failed export_video mix left a ducking_mix file or reached the queue");

            queue->clear();
            projectInfoWindow.m_mcpTools = previousTools;
            videoTrack->setClips(savedVideoClips);
            for (auto &entry : savedAudioClips)
                entry.first->setClips(entry.second);
        }
    }

    // US-909 G181-G187: export_video autoLoudness / targetLufs (explicit > preset > false),
    // the prepareJob / prepareExportAudioMix exclusion, and audioBitrate reaching the mix.
    {
        // G181: the export_video descriptor as of the base (before US-909), pinned verbatim.
        // The expected entry is that string plus two properties and one description sentence.
        static const char kBaseExportVideoDescriptor[] = R"US909({"description":"audioOnly=true は m4a / wav / mp3 の音声のみを同期で書き出し、ok と outputPath を返す (jobId なし)。映像設定は無視して warnings を返す。通常は現在のタイムラインを動画ファイルへ非同期で書き出す。tools/call はジョブ投入後すぐに jobId を返し、完了は get_export_status で確認する。width / height / fps の省略時は現在のプロジェクト設定を使い、videoBitrate / audioBitrate は kbps (既定 10000 / 192)、videoCodec / audioCodec は ffmpeg のエンコーダ名 (既定 libx264 / aac)。音声はトリム・分割・並べ替え・音量・ミュートを反映したタイムラインのミックスを ffmpeg で作ってから多重化する (ffmpeg が PATH に無いと単純な 1 クリップ構成以外は failed になる)。","inputSchema":{"additionalProperties":false,"properties":{"audioBitrate":{"description":"kbps。既定 192","minimum":1,"type":"integer"},"audioCodec":{"description":"ffmpeg の音声エンコーダ名。既定 aac","type":"string"},"audioOnly":{"default":false,"type":"boolean"},"crf":{"description":"品質。-1 は既定、H.264/H.265 は 0..51、AV1 は 0..63","maximum":63,"minimum":-1,"type":"integer"},"fps":{"exclusiveMinimum":0,"type":"number"},"height":{"minimum":2,"type":"integer"},"keepAlpha":{"default":false,"description":"ProRes 4444 / 4444 XQ または VP9 でアルファチャンネルを保持","type":"boolean"},"metadata":{"additionalProperties":false,"properties":{"author":{"type":"string"},"comment":{"type":"string"},"copyright":{"type":"string"},"title":{"type":"string"}},"type":"object"},"outputPath":{"type":"string"},"preset":{"type":"string"},"proresProfile":{"default":1,"description":"ProRes プロファイル。省略時は既存の既定値 1","maximum":5,"minimum":0,"type":"integer"},"rateControl":{"enum":["bitrate","crf"],"type":"string"},"videoBitrate":{"description":"kbps","minimum":1,"type":"integer"},"videoCodec":{"type":"string"},"width":{"minimum":2,"type":"integer"}},"required":["outputPath"],"type":"object"},"name":"export_video","outputSchema":{"oneOf":[{"required":["jobId","status","progress","outputPath","width","height","fps","videoCodec","videoBitrate"]},{"not":{"required":["jobId"]},"required":["outputPath"]}],"properties":{"audioBitrate":{"type":"integer"},"audioCodec":{"type":"string"},"fps":{"type":"number"},"height":{"type":"integer"},"jobId":{"type":"string"},"ok":{"type":"boolean"},"outputPath":{"type":"string"},"preset":{"type":"string"},"progress":{"type":"integer"},"status":{"enum":["queued"],"type":"string"},"videoBitrate":{"type":"integer"},"videoCodec":{"type":"string"},"warnings":{"items":{"type":"string"},"type":"array"},"width":{"type":"integer"}},"required":["ok"],"type":"object"}})US909";
        const QJsonObject baseDescriptor = QJsonDocument::fromJson(QByteArray(kBaseExportVideoDescriptor)).object();
        QJsonObject currentDescriptor;
        for (const QJsonValue &value : projectInfoToolDescriptors)
            if (value.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("export_video"))
                currentDescriptor = value.toObject();
        QJsonObject expectedDescriptor = baseDescriptor;
        const QString addedSentence = QStringLiteral("autoLoudness / targetLufs を省略すると、preset 指定時はそのプリセットの自動ラウドネス設定に従う (audioOnly=true では行わない)。");
        expectedDescriptor.insert(QStringLiteral("description"),
            baseDescriptor.value(QStringLiteral("description")).toString() + addedSentence);
        QJsonObject expectedSchema = baseDescriptor.value(QStringLiteral("inputSchema")).toObject();
        QJsonObject expectedProps = expectedSchema.value(QStringLiteral("properties")).toObject();
        expectedProps.insert(QStringLiteral("autoLoudness"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("boolean")},
            {QStringLiteral("description"), QStringLiteral("音声を自動ラウドネス正規化する。省略時は preset の設定に従い、preset が無ければ OFF。audioOnly=true では自動ラウドネスを行わず、true の指定はエラー")}});
        expectedProps.insert(QStringLiteral("targetLufs"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("minimum"), -70},
            {QStringLiteral("maximum"), -1},
            {QStringLiteral("description"), QStringLiteral("自動ラウドネスの目標 LUFS。省略時は preset の値、無ければ -14")}});
        expectedSchema.insert(QStringLiteral("properties"), expectedProps);
        expectedDescriptor.insert(QStringLiteral("inputSchema"), expectedSchema);
        QStringList diffKeys;
        for (const QString &key : expectedDescriptor.keys())
            if (expectedDescriptor.value(key) != currentDescriptor.value(key)) diffKeys << key;
        for (const QString &key : currentDescriptor.keys())
            if (!expectedDescriptor.contains(key)) diffKeys << key;
        const QJsonObject currentSchema = currentDescriptor.value(QStringLiteral("inputSchema")).toObject();
        QStringList propDiff;
        const QJsonObject currentProps = currentSchema.value(QStringLiteral("properties")).toObject();
        for (const QString &key : expectedProps.keys())
            if (expectedProps.value(key) != currentProps.value(key)) propDiff << key;
        for (const QString &key : currentProps.keys())
            if (!expectedProps.contains(key)) propDiff << key;
        const QJsonArray requiredNow = currentSchema.value(QStringLiteral("required")).toArray();
        const bool descriptorBaseSane = observeTerm(181, "!baseDescriptor.isEmpty()", static_cast<bool>(!baseDescriptor.isEmpty()))
            && observeTerm(181, "baseDescriptor.value(QStringLiteral(\"name\")).toString() == QStringLiteral(\"export_video\")", static_cast<bool>(baseDescriptor.value(QStringLiteral("name")).toString() == QStringLiteral("export_video")))
            && observeTerm(181, "!baseDescriptor.value(QStringLiteral(\"inputSchema\")).toObject().value(QStringLiteral(\"properties\")) .toObject().contains(QStringLiteral(\"autoLoudness\"))", static_cast<bool>(!baseDescriptor.value(QStringLiteral("inputSchema")).toObject().value(QStringLiteral("properties"))
                   .toObject().contains(QStringLiteral("autoLoudness"))));
        std::fprintf(stderr, "mcp G181 baseSane=%d equal=%d diffKeys=[%s] propDiff=[%s] required=%s outputSchemaSame=%d\n",
            descriptorBaseSane, expectedDescriptor == currentDescriptor, qPrintable(diffKeys.join(QLatin1Char(','))),
            qPrintable(propDiff.join(QLatin1Char(','))),
            QJsonDocument(requiredNow).toJson(QJsonDocument::Compact).constData(),
            baseDescriptor.value(QStringLiteral("outputSchema")) == currentDescriptor.value(QStringLiteral("outputSchema")));
        tallyObserved(181, observeTerm(181, "descriptorBaseSane", static_cast<bool>(descriptorBaseSane)) && observeTerm(181, "expectedDescriptor == currentDescriptor", static_cast<bool>(expectedDescriptor == currentDescriptor))
            && observeTerm(181, "requiredNow == QJsonArray{QStringLiteral(\"outputPath\")}", static_cast<bool>(requiredNow == QJsonArray{QStringLiteral("outputPath")}))
            && observeTerm(181, "baseDescriptor.value(QStringLiteral(\"outputSchema\")) == currentDescriptor.value(QStringLiteral(\"outputSchema\"))", static_cast<bool>(baseDescriptor.value(QStringLiteral("outputSchema")) == currentDescriptor.value(QStringLiteral("outputSchema")))),
            "the export_video descriptor differs from the pinned base plus the two US-909 properties and sentence");

        const QString ffmpegExe = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        const QString videoAsset = QDir::current().absoluteFilePath(
            QStringLiteral("test_assets/e2e_clip.mp4"));
        auto *queue = projectInfoWindow.findChild<RenderQueue*>(QStringLiteral("mainRenderQueue"));
        QTemporaryDir presetDir;
        const bool g182Ready = timelineReady && queue && QFileInfo(videoAsset).isFile()
            && exportRootReady && presetDir.isValid();
        if (!g182Ready) {
            const QString why = QStringLiteral("fixture missing: timeline=%1 queue=%2 asset=%3 exportRoot=%4 presetDir=%5")
                .arg(timelineReady).arg(queue != nullptr).arg(QFileInfo(videoAsset).isFile())
                .arg(exportRootReady).arg(presetDir.isValid());
            for (int n = 182; n <= 189; ++n)
                tally.gate(n, false, why.toUtf8().constData());
            tally.gate(194, false, why.toUtf8().constData());
        } else if (ffmpegExe.isEmpty()) {
            for (int n = 182; n <= 189; ++n)
                tally.skip(n, "ffmpeg.exe unavailable");
            tally.skip(194, "ffmpeg.exe unavailable");
        } else {
            const QString tonePath = exportRoot.filePath(QStringLiteral("us909-tone.wav"));
            std::vector<float> toneSamples(48000 * 2 * 4);
            for (size_t i = 0; i < toneSamples.size(); ++i)
                toneSamples[i] = 0.1f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * static_cast<double>(i / 2) / 48000.0));
            QString toneError;
            const bool toneOk = libavcore::writeWavFloatInterleaved(tonePath, toneSamples, 48000, 2, &toneError);
            ClipInfo videoClip;
            videoClip.filePath = videoAsset;
            videoClip.displayName = QStringLiteral("us909-video");
            videoClip.duration = videoClip.outPoint = 1.0;
            ClipInfo audioClip;
            audioClip.filePath = tonePath;
            audioClip.displayName = QStringLiteral("us909-audio");
            audioClip.duration = audioClip.outPoint = 4.0;
            TimelineTrack *videoTrack = projectTimeline->videoTracks().first();
            TimelineTrack *audioTrack = projectTimeline->audioTracks().first();
            const QVector<ClipInfo> savedVideoClips = videoTrack->clips();
            QVector<QPair<TimelineTrack *, QVector<ClipInfo>>> savedAudioClips;
            for (TimelineTrack *track : projectTimeline->audioTracks()) {
                savedAudioClips.append(qMakePair(track, track->clips()));
                track->setClips(QVector<ClipInfo>{});
            }
            videoTrack->setClips(QVector<ClipInfo>{videoClip});
            audioTrack->setClips(QVector<ClipInfo>{audioClip});
            queue->clear();
            auto *previousTools = projectInfoWindow.m_mcpTools;
            projectInfoWindow.m_mcpTools = &projectInfoTools;

            const bool hadPresetEnv = qEnvironmentVariableIsSet("VEDITOR_EXPORT_PRESET_DIR");
            const QByteArray oldPresetEnv = qgetenv("VEDITOR_EXPORT_PRESET_DIR");
            qputenv("VEDITOR_EXPORT_PRESET_DIR", presetDir.path().toUtf8());
            ExportUserPreset onPreset;
            onPreset.name = QStringLiteral("us909-on");
            onPreset.config.autoLoudness = true;
            onPreset.config.targetLufs = -16.0;
            ExportUserPreset offPreset;
            offPreset.name = QStringLiteral("us909-off");
            offPreset.config.autoLoudness = false;
            offPreset.config.targetLufs = -16.0;
            QString presetError;
            const bool presetsSaved = ExportUserPresets::save(onPreset, &presetError)
                && ExportUserPresets::save(offPreset, &presetError);

            struct Run {
                bool isError = true;
                QString errorText;
                QString jobId;
                bool captured = false;
                RenderJob job;
                QJsonArray tempFiles;
                bool mixInTemp = false;
                qint64 mixBytes = -1;
                qint64 mixDurationUs = -1;
                qint64 mixStreamBitRate = -1;
                bool mixIsAac = false;
                int measureDelta = -1;
                int mixDelta = -1;
                int silentMixDelta = -1;
                bool terminal = false;
                RenderJobStatus finalStatus = RenderJobStatus::Pending;
                qsizetype queueAfter = -1;
            };
            // Audio stream bit_rate from the container headers (not size / duration).
            const auto probeStreamBitRate = [](const QString &path) -> qint64 {
                AVFormatContext *format = nullptr;
                if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0)
                    return -1;
                qint64 rate = -1;
                if (avformat_find_stream_info(format, nullptr) >= 0) {
                    for (unsigned i = 0; i < format->nb_streams; ++i)
                        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                            rate = format->streams[i]->codecpar->bit_rate;
                }
                avformat_close_input(&format);
                return rate;
            };
            // Codec of the first audio stream (the .m4a extension names the container, not the codec).
            const auto firstAudioIsAac = [](const QString &path) -> bool {
                AVFormatContext *format = nullptr;
                if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0)
                    return false;
                bool aac = false;
                if (avformat_find_stream_info(format, nullptr) >= 0) {
                    for (unsigned i = 0; i < format->nb_streams; ++i) {
                        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                            aac = format->streams[i]->codecpar->codec_id == AV_CODEC_ID_AAC;
                            break;
                        }
                    }
                }
                avformat_close_input(&format);
                return aac;
            };
            int callId = 1900;
            // jobsChanged is connected before the call: the job's tempFiles and mix are gone at job end.
            auto runExport = [&](QJsonObject args, bool expectJob = true) {
                Run run;
                queue->clear();
                if (!args.contains(QStringLiteral("outputPath")))
                    args.insert(QStringLiteral("outputPath"),
                                exportRoot.filePath(QStringLiteral("us909-%1.mp4").arg(callId)));
                if (!args.contains(QStringLiteral("audioOnly"))) {
                    args.insert(QStringLiteral("width"), 320);
                    args.insert(QStringLiteral("height"), 180);
                    args.insert(QStringLiteral("fps"), 10);
                }
                const int queueBefore = queue->jobs().size();
                const int measureBefore = RenderQueue::autoLoudnessMeasurementCountForTest();
                const int mixBefore = projectInfoWindow.exportAudioMixCallCountForTest;
                const int silentMixBefore = loudnessauto::silentMixCallCountForTest();
                const auto connection = QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                    if (run.captured) return;
                    const auto jobs = queue->jobs();
                    if (jobs.isEmpty()) return;
                    run.job = jobs.first();
                    run.captured = true;
                    run.tempFiles = run.job.exportConfig.value(QStringLiteral("tempFiles")).toArray();
                    run.mixInTemp = !run.job.projectFilePath.isEmpty()
                        && run.tempFiles.contains(run.job.projectFilePath);
                    const QFileInfo mixInfo(run.job.projectFilePath);
                    if (mixInfo.isFile()) {
                        run.mixBytes = mixInfo.size();
                        const auto duration = libavcore::probeDurationMicroseconds(
                            run.job.projectFilePath.toStdString());
                        if (duration) run.mixDurationUs = *duration;
                        run.mixStreamBitRate = probeStreamBitRate(run.job.projectFilePath);
                        run.mixIsAac = firstAudioIsAac(run.job.projectFilePath);
                    }
                });
                const QJsonObject response = callProjectInfoTool(callId++, QStringLiteral("export_video"), args);
                run.isError = toolResult(response).value(QStringLiteral("isError")).toBool(true);
                run.jobId = toolPayload(response).value(QStringLiteral("jobId")).toString();
                if (run.isError) {
                    const QJsonArray content = toolResult(response).value(QStringLiteral("content")).toArray();
                    run.errorText = content.isEmpty() ? QString()
                        : content.first().toObject().value(QStringLiteral("text")).toString();
                }
                if (expectJob && !run.isError) {
                    (void)QTest::qWaitFor([&]() { return run.captured; }, 60000);
                    run.measureDelta = RenderQueue::autoLoudnessMeasurementCountForTest() - measureBefore;
                    run.mixDelta = projectInfoWindow.exportAudioMixCallCountForTest - mixBefore;
                    run.silentMixDelta = loudnessauto::silentMixCallCountForTest() - silentMixBefore;
                    run.terminal = run.captured && QTest::qWaitFor([&]() {
                        for (const RenderJob &job : queue->jobs()) {
                            if (job.uuid != run.jobId) continue;
                            run.finalStatus = job.status;
                            return job.status == RenderJobStatus::Completed || job.status == RenderJobStatus::Failed
                                || job.status == RenderJobStatus::Cancelled;
                        }
                        return false;
                    }, 120000);
                } else {
                    // Returns as soon as a (wrongly) deferred job step is observed; the deferral is QTimer::singleShot(0).
                    (void)QTest::qWaitFor([&]() {
                        return queue->jobs().size() != queueBefore
                            || RenderQueue::autoLoudnessMeasurementCountForTest() != measureBefore
                            || projectInfoWindow.exportAudioMixCallCountForTest != mixBefore;
                    }, 1000);
                    run.measureDelta = RenderQueue::autoLoudnessMeasurementCountForTest() - measureBefore;
                    run.mixDelta = projectInfoWindow.exportAudioMixCallCountForTest - mixBefore;
                    run.silentMixDelta = loudnessauto::silentMixCallCountForTest() - silentMixBefore;
                }
                QObject::disconnect(connection);
                run.queueAfter = queue->jobs().size();
                return run;
            };
            const auto lufsOf = [](const Run &run, const char *key) {
                return run.job.exportConfig.value(QLatin1String(key)).toDouble(std::numeric_limits<double>::quiet_NaN());
            };
            const auto autoOf = [](const Run &run) {
                return run.job.exportConfig.value(QStringLiteral("autoLoudness")).toBool(false);
            };
            const auto describe = [&](const char *tag, const Run &run) {
                std::fprintf(stderr, "mcp %s isError=%d err=[%s] captured=%d measureDelta=%d mixDelta=%d "
                    "hasAutoKey=%d autoLoudness=%d targetLufs=%g measuredLufs=%g mixInTemp=%d "
                    "mixIsAac=%d projectFilePath=[%s] terminal=%d status=%d\n", tag, run.isError, qPrintable(run.errorText),
                    run.captured, run.measureDelta, run.mixDelta,
                    run.job.exportConfig.contains(QStringLiteral("autoLoudness")), autoOf(run),
                    lufsOf(run, "targetLufs"), lufsOf(run, "measuredLufs"), run.mixInTemp,
                    run.mixIsAac, qPrintable(run.job.projectFilePath), run.terminal, static_cast<int>(run.finalStatus));
            };
            const auto finiteSane = [](double lufs) { return std::isfinite(lufs) && lufs > -30.0 && lufs < -18.0; };

            // G182: explicit autoLoudness=true goes through prepareJob, not prepareExportAudioMix.
            const Run explicitOn = runExport(QJsonObject{{QStringLiteral("autoLoudness"), true},
                                                         {QStringLiteral("targetLufs"), -14}});
            describe("G182", explicitOn);

            // G182 sub-case: autoLoudness resolved true on a timeline whose audio playback sequence is empty.
            // prepareJob makes the silent track itself (US-1002: not prepareExportAudioMix, mixDelta 0); without
            // it RenderQueue falls back to the V1 source audio (e2e_clip.mp4 has an audible AAC stream).
            audioTrack->setClips(QVector<ClipInfo>{});
            const QString silentOut = exportRoot.filePath(QStringLiteral("us909-emptyaudio.mp4"));
            const Run emptyAudio = runExport(QJsonObject{{QStringLiteral("autoLoudness"), true},
                {QStringLiteral("outputPath"), silentOut}});
            audioTrack->setClips(QVector<ClipInfo>{audioClip});
            describe("G182-empty", emptyAudio);
            const double silentLufs = emptyAudio.terminal ? loudness::measureIntegratedLufs(silentOut)
                                                          : std::numeric_limits<double>::quiet_NaN();
            const double sourceLufs = loudness::measureIntegratedLufs(videoAsset);
            const bool silentOk = observeTerm(182, "!emptyAudio.isError", static_cast<bool>(!emptyAudio.isError)) && observeTerm(182, "emptyAudio.captured", static_cast<bool>(emptyAudio.captured))
                && observeTerm(182, "emptyAudio.mixInTemp", static_cast<bool>(emptyAudio.mixInTemp)) && observeTerm(182, "emptyAudio.job.projectFilePath != videoAsset", static_cast<bool>(emptyAudio.job.projectFilePath != videoAsset))
                && observeTerm(182, "emptyAudio.mixDelta == 0", static_cast<bool>(emptyAudio.mixDelta == 0)) && observeTerm(182, "emptyAudio.silentMixDelta == 1", static_cast<bool>(emptyAudio.silentMixDelta == 1))
                && observeTerm(182, "emptyAudio.terminal", static_cast<bool>(emptyAudio.terminal)) && observeTerm(182, "emptyAudio.finalStatus == RenderJobStatus::Completed", static_cast<bool>(emptyAudio.finalStatus == RenderJobStatus::Completed))
                && observeTerm(182, "std::isfinite(sourceLufs)", static_cast<bool>(std::isfinite(sourceLufs))) && observeTerm(182, "sourceLufs > -40.0", static_cast<bool>(sourceLufs > -40.0))
                && observeTerm(182, "std::isfinite(silentLufs)", static_cast<bool>(std::isfinite(silentLufs))) && observeTerm(182, "silentLufs <= -70.0", static_cast<bool>(silentLufs <= -70.0));
            std::fprintf(stderr, "mcp G182-empty silentOk=%d outputLufs=%g sourceV1Lufs=%g mixDelta=%d silentMixDelta=%d "
                "measureDelta=%d projectFilePathIsV1=%d\n", silentOk, silentLufs, sourceLufs, emptyAudio.mixDelta,
                emptyAudio.silentMixDelta, emptyAudio.measureDelta, emptyAudio.job.projectFilePath == videoAsset);
            tallyObserved(182, observeTerm(182, "toneOk", static_cast<bool>(toneOk)) && observeTerm(182, "presetsSaved", static_cast<bool>(presetsSaved)) && observeTerm(182, "!explicitOn.isError", static_cast<bool>(!explicitOn.isError)) && observeTerm(182, "explicitOn.captured", static_cast<bool>(explicitOn.captured))
                && observeTerm(182, "explicitOn.measureDelta == 1", static_cast<bool>(explicitOn.measureDelta == 1)) && observeTerm(182, "finiteSane(lufsOf(explicitOn, \"measuredLufs\"))", static_cast<bool>(finiteSane(lufsOf(explicitOn, "measuredLufs"))))
                && observeTerm(182, "explicitOn.mixInTemp", static_cast<bool>(explicitOn.mixInTemp)) && observeTerm(182, "explicitOn.mixIsAac", static_cast<bool>(explicitOn.mixIsAac)) && observeTerm(182, "explicitOn.mixDelta == 0", static_cast<bool>(explicitOn.mixDelta == 0))
                && observeTerm(182, "explicitOn.terminal", static_cast<bool>(explicitOn.terminal)) && observeTerm(182, "explicitOn.finalStatus == RenderJobStatus::Completed", static_cast<bool>(explicitOn.finalStatus == RenderJobStatus::Completed))
                && observeTerm(182, "silentOk", static_cast<bool>(silentOk)),
                "autoLoudness=true did not measure once via prepareJob (or also called prepareExportAudioMix), "
                "or an empty audio sequence brought back the V1 source audio");

            // G183 / G184: omitted + no preset is the base path; omitted + ON preset is the US-909 change.
            const Run omitted = runExport(QJsonObject{});
            describe("G183", omitted);
            const bool g183Ext = observeTerm(183, "QFileInfo(omitted.job.projectFilePath).suffix().toLower() == QStringLiteral(\"m4a\")", static_cast<bool>(QFileInfo(omitted.job.projectFilePath).suffix().toLower() == QStringLiteral("m4a")))
                || observeTerm(183, "omitted.mixInTemp", static_cast<bool>(omitted.mixInTemp));
            tallyObserved(183, observeTerm(183, "!omitted.isError", static_cast<bool>(!omitted.isError)) && observeTerm(183, "omitted.captured", static_cast<bool>(omitted.captured))
                && observeTerm(183, "!omitted.job.exportConfig.contains(QStringLiteral(\"autoLoudness\"))", static_cast<bool>(!omitted.job.exportConfig.contains(QStringLiteral("autoLoudness"))))
                && observeTerm(183, "!omitted.job.exportConfig.contains(QStringLiteral(\"targetLufs\"))", static_cast<bool>(!omitted.job.exportConfig.contains(QStringLiteral("targetLufs"))))
                && observeTerm(183, "omitted.measureDelta == 0", static_cast<bool>(omitted.measureDelta == 0)) && observeTerm(183, "omitted.mixDelta == 1", static_cast<bool>(omitted.mixDelta == 1)) && observeTerm(183, "omitted.mixInTemp", static_cast<bool>(omitted.mixInTemp)) && observeTerm(183, "g183Ext", static_cast<bool>(g183Ext))
                && observeTerm(183, "omitted.terminal", static_cast<bool>(omitted.terminal)) && observeTerm(183, "omitted.finalStatus == RenderJobStatus::Completed", static_cast<bool>(omitted.finalStatus == RenderJobStatus::Completed)),
                "omitted autoLoudness without a preset left the base export path (key, measurement or mix call count changed)");
            const Run presetOn = runExport(QJsonObject{{QStringLiteral("preset"), QStringLiteral("us909-on")}});
            describe("G184", presetOn);
            tallyObserved(184, observeTerm(184, "!presetOn.isError", static_cast<bool>(!presetOn.isError)) && observeTerm(184, "presetOn.captured", static_cast<bool>(presetOn.captured)) && observeTerm(184, "presetOn.measureDelta == 1", static_cast<bool>(presetOn.measureDelta == 1))
                && observeTerm(184, "presetOn.mixDelta == 0", static_cast<bool>(presetOn.mixDelta == 0)) && observeTerm(184, "autoOf(presetOn)", static_cast<bool>(autoOf(presetOn)))
                && observeTerm(184, "lufsOf(presetOn, \"targetLufs\") == -16.0", static_cast<bool>(lufsOf(presetOn, "targetLufs") == -16.0)) && observeTerm(184, "finiteSane(lufsOf(presetOn, \"measuredLufs\"))", static_cast<bool>(finiteSane(lufsOf(presetOn, "measuredLufs"))))
                && observeTerm(184, "presetOn.mixInTemp", static_cast<bool>(presetOn.mixInTemp)) && observeTerm(184, "presetOn.terminal", static_cast<bool>(presetOn.terminal)) && observeTerm(184, "presetOn.finalStatus == RenderJobStatus::Completed", static_cast<bool>(presetOn.finalStatus == RenderJobStatus::Completed)),
                "an ON preset with autoLoudness omitted was not measured (behavior change not applied)");

            // G185: resolution rule explicit > preset > false.
            const Run caseA = runExport(QJsonObject{{QStringLiteral("preset"), QStringLiteral("us909-on")},
                                                    {QStringLiteral("autoLoudness"), false}});
            describe("G185a", caseA);
            const Run caseB = runExport(QJsonObject{{QStringLiteral("preset"), QStringLiteral("us909-off")},
                                                    {QStringLiteral("autoLoudness"), true}});
            describe("G185b", caseB);
            const Run caseC = runExport(QJsonObject{{QStringLiteral("targetLufs"), -20}});
            describe("G185c", caseC);
            const Run caseD = runExport(QJsonObject{{QStringLiteral("preset"), QStringLiteral("us909-on")},
                                                    {QStringLiteral("targetLufs"), -20}});
            describe("G185d", caseD);
            const bool a = observeTerm(185, "!caseA.isError", static_cast<bool>(!caseA.isError)) && observeTerm(185, "caseA.captured", static_cast<bool>(caseA.captured)) && observeTerm(185, "caseA.measureDelta == 0", static_cast<bool>(caseA.measureDelta == 0)) && observeTerm(185, "caseA.mixDelta == 1", static_cast<bool>(caseA.mixDelta == 1))
                && observeTerm(185, "!autoOf(caseA)", static_cast<bool>(!autoOf(caseA)))
                && observeTerm(185, "caseA.job.exportConfig.contains(QStringLiteral(\"autoLoudness\"))", static_cast<bool>(caseA.job.exportConfig.contains(QStringLiteral("autoLoudness"))))
                && observeTerm(185, "caseA.job.exportConfig.value(QStringLiteral(\"autoLoudness\")).isBool()", static_cast<bool>(caseA.job.exportConfig.value(QStringLiteral("autoLoudness")).isBool()));
            const bool b = observeTerm(185, "!caseB.isError", static_cast<bool>(!caseB.isError)) && observeTerm(185, "caseB.captured", static_cast<bool>(caseB.captured)) && observeTerm(185, "caseB.measureDelta == 1", static_cast<bool>(caseB.measureDelta == 1)) && observeTerm(185, "caseB.mixDelta == 0", static_cast<bool>(caseB.mixDelta == 0))
                && observeTerm(185, "autoOf(caseB)", static_cast<bool>(autoOf(caseB))) && observeTerm(185, "lufsOf(caseB, \"targetLufs\") == -16.0", static_cast<bool>(lufsOf(caseB, "targetLufs") == -16.0));
            const bool c = observeTerm(185, "!caseC.isError", static_cast<bool>(!caseC.isError)) && observeTerm(185, "caseC.captured", static_cast<bool>(caseC.captured)) && observeTerm(185, "caseC.measureDelta == 0", static_cast<bool>(caseC.measureDelta == 0)) && observeTerm(185, "caseC.mixDelta == 1", static_cast<bool>(caseC.mixDelta == 1))
                && observeTerm(185, "!autoOf(caseC)", static_cast<bool>(!autoOf(caseC))) && observeTerm(185, "lufsOf(caseC, \"targetLufs\") == -20.0", static_cast<bool>(lufsOf(caseC, "targetLufs") == -20.0));
            const bool d = observeTerm(185, "!caseD.isError", static_cast<bool>(!caseD.isError)) && observeTerm(185, "caseD.captured", static_cast<bool>(caseD.captured)) && observeTerm(185, "caseD.measureDelta == 1", static_cast<bool>(caseD.measureDelta == 1)) && observeTerm(185, "caseD.mixDelta == 0", static_cast<bool>(caseD.mixDelta == 0))
                && observeTerm(185, "autoOf(caseD)", static_cast<bool>(autoOf(caseD))) && observeTerm(185, "lufsOf(caseD, \"targetLufs\") == -20.0", static_cast<bool>(lufsOf(caseD, "targetLufs") == -20.0));
            std::fprintf(stderr, "mcp G185 a=%d b=%d c=%d d=%d\n", a, b, c, d);
            tallyObserved(185, observeTerm(185, "a", static_cast<bool>(a)) && observeTerm(185, "b", static_cast<bool>(b)) && observeTerm(185, "c", static_cast<bool>(c)) && observeTerm(185, "d", static_cast<bool>(d)),
                "the explicit > preset > false resolution of autoLoudness / targetLufs was violated");

            // G186: invalid values are rejected without queuing; audioOnly + preset-derived true is ignored.
            const QString audioOnlyPath = exportRoot.filePath(QStringLiteral("us909-audio.m4a"));
            const QList<QJsonObject> invalidArgs{
                QJsonObject{{QStringLiteral("targetLufs"), 0}},
                QJsonObject{{QStringLiteral("targetLufs"), -80}},
                QJsonObject{{QStringLiteral("autoLoudness"), QStringLiteral("yes")}},
                QJsonObject{{QStringLiteral("audioOnly"), true}, {QStringLiteral("autoLoudness"), true},
                            {QStringLiteral("outputPath"), audioOnlyPath}}};
            bool invalidOk = true;
            for (int i = 0; i < invalidArgs.size(); ++i) {
                const Run run = runExport(invalidArgs.at(i), false);
                std::fprintf(stderr, "mcp G186 invalid#%d isError=%d err=[%s] queueAfter=%lld measureDelta=%d mixDelta=%d\n",
                    i, run.isError, qPrintable(run.errorText), static_cast<long long>(run.queueAfter),
                    run.measureDelta, run.mixDelta);
                invalidOk = invalidOk && run.isError && !run.errorText.isEmpty() && run.queueAfter == 0
                    && run.measureDelta == 0 && run.mixDelta == 0;
            }
            // audioOnly + ON preset + omitted: decision 7 (Wave 10 exception) - MCP audioOnly never measures (base measured once).
            const Run audioOnlyPreset = runExport(QJsonObject{{QStringLiteral("audioOnly"), true},
                {QStringLiteral("preset"), QStringLiteral("us909-on")}, {QStringLiteral("outputPath"), audioOnlyPath}}, false);
            const bool audioOnlyPresetFile = QFileInfo(audioOnlyPath).isFile();
            std::fprintf(stderr, "mcp G186 audioOnly+ON preset isError=%d err=[%s] jobId=[%s] queueAfter=%lld measureDelta=%d outputExists=%d (decision 7: not measured; base measured)\n",
                audioOnlyPreset.isError, qPrintable(audioOnlyPreset.errorText), qPrintable(audioOnlyPreset.jobId),
                static_cast<long long>(audioOnlyPreset.queueAfter), audioOnlyPreset.measureDelta, audioOnlyPresetFile);
            // Explicit autoLoudness=false overrides the preset: not measured.
            const QString audioOnlyOffPath = exportRoot.filePath(QStringLiteral("us909-audio-off.m4a"));
            const Run audioOnlyOff = runExport(QJsonObject{{QStringLiteral("audioOnly"), true},
                {QStringLiteral("preset"), QStringLiteral("us909-on")}, {QStringLiteral("autoLoudness"), false},
                {QStringLiteral("outputPath"), audioOnlyOffPath}}, false);
            std::fprintf(stderr, "mcp G186 audioOnly+ON preset+autoLoudness=false isError=%d err=[%s] jobId=[%s] queueAfter=%lld measureDelta=%d outputExists=%d\n",
                audioOnlyOff.isError, qPrintable(audioOnlyOff.errorText), qPrintable(audioOnlyOff.jobId),
                static_cast<long long>(audioOnlyOff.queueAfter), audioOnlyOff.measureDelta, QFileInfo(audioOnlyOffPath).isFile());
            tallyObserved(186, observeTerm(186, "invalidOk", static_cast<bool>(invalidOk)) && observeTerm(186, "!audioOnlyPreset.isError", static_cast<bool>(!audioOnlyPreset.isError)) && observeTerm(186, "audioOnlyPreset.jobId.isEmpty()", static_cast<bool>(audioOnlyPreset.jobId.isEmpty()))
                && observeTerm(186, "audioOnlyPreset.queueAfter == 0", static_cast<bool>(audioOnlyPreset.queueAfter == 0)) && observeTerm(186, "audioOnlyPreset.measureDelta == 0", static_cast<bool>(audioOnlyPreset.measureDelta == 0)) && observeTerm(186, "audioOnlyPresetFile", static_cast<bool>(audioOnlyPresetFile))
                && observeTerm(186, "!audioOnlyOff.isError", static_cast<bool>(!audioOnlyOff.isError)) && observeTerm(186, "audioOnlyOff.jobId.isEmpty()", static_cast<bool>(audioOnlyOff.jobId.isEmpty())) && observeTerm(186, "audioOnlyOff.queueAfter == 0", static_cast<bool>(audioOnlyOff.queueAfter == 0))
                && observeTerm(186, "audioOnlyOff.measureDelta == 0", static_cast<bool>(audioOnlyOff.measureDelta == 0)) && observeTerm(186, "QFileInfo(audioOnlyOffPath).isFile()", static_cast<bool>(QFileInfo(audioOnlyOffPath).isFile())),
                "an invalid autoLoudness / targetLufs queued a job, or MCP audioOnly + ON preset measured (decision 7: not measured; base measured) / audioOnly + explicit false measured");

            // G187: audioBitrate reaches the mix. The audio stream bit_rate is read from the m4a headers.
            // Noise (a tone would not make the AAC encoder spend its bitrate), on the audio track for these two runs only.
            const QString noisePath = exportRoot.filePath(QStringLiteral("us909-noise.wav"));
            std::vector<float> noiseSamples(48000 * 2 * 4);
            unsigned noiseState = 12345u;
            for (float &v : noiseSamples) {
                noiseState = noiseState * 1664525u + 1013904223u;
                v = (static_cast<float>(noiseState >> 8) / 8388608.0f - 1.0f) * 0.3f;
            }
            QString noiseError;
            const bool noiseOk = libavcore::writeWavFloatInterleaved(noisePath, noiseSamples, 48000, 2, &noiseError);
            ClipInfo noiseClip = audioClip;
            noiseClip.filePath = noisePath;
            noiseClip.displayName = QStringLiteral("us909-noise");
            audioTrack->setClips(QVector<ClipInfo>{noiseClip});
            const Run bitrate96 = runExport(QJsonObject{{QStringLiteral("audioBitrate"), 96}});
            const Run bitrate192 = runExport(QJsonObject{{QStringLiteral("audioBitrate"), 192}});
            audioTrack->setClips(QVector<ClipInfo>{audioClip});
            const double rate96 = static_cast<double>(bitrate96.mixStreamBitRate) / 1000.0;
            const double rate192 = static_cast<double>(bitrate192.mixStreamBitRate) / 1000.0;
            std::fprintf(stderr, "mcp G187 noiseOk=%d 96: isError=%d mixInTemp=%d bit_rate=%lld bytes=%lld durationUs=%lld | 192: isError=%d mixInTemp=%d bit_rate=%lld bytes=%lld durationUs=%lld\n",
                noiseOk, bitrate96.isError, bitrate96.mixInTemp, static_cast<long long>(bitrate96.mixStreamBitRate),
                static_cast<long long>(bitrate96.mixBytes), static_cast<long long>(bitrate96.mixDurationUs),
                bitrate192.isError, bitrate192.mixInTemp, static_cast<long long>(bitrate192.mixStreamBitRate),
                static_cast<long long>(bitrate192.mixBytes), static_cast<long long>(bitrate192.mixDurationUs));
            tallyObserved(187, observeTerm(187, "noiseOk", static_cast<bool>(noiseOk)) && observeTerm(187, "!bitrate96.isError", static_cast<bool>(!bitrate96.isError)) && observeTerm(187, "bitrate96.mixInTemp", static_cast<bool>(bitrate96.mixInTemp)) && observeTerm(187, "rate96 > 81.6", static_cast<bool>(rate96 > 81.6)) && observeTerm(187, "rate96 < 110.4", static_cast<bool>(rate96 < 110.4))
                && observeTerm(187, "!bitrate192.isError", static_cast<bool>(!bitrate192.isError)) && observeTerm(187, "bitrate192.mixInTemp", static_cast<bool>(bitrate192.mixInTemp)) && observeTerm(187, "rate192 > 163.2", static_cast<bool>(rate192 > 163.2)) && observeTerm(187, "rate192 < 220.8", static_cast<bool>(rate192 < 220.8))
                && observeTerm(187, "rate192 > 1.4 * rate96", static_cast<bool>(rate192 > 1.4 * rate96)),
                "the MCP audioBitrate did not set the mix AAC stream bit_rate (about 96 / 192 kbps)");

            // G188 (US-1002): a PCM mix failure inside prepareJob rejects the export_video job (addJob ->
            // get_export_status failed + error, nothing queued). Control: hook None, same call -> queued.
            {
                const QJsonObject onArgs{{QStringLiteral("autoLoudness"), true},
                                         {QStringLiteral("outputPath"), exportRoot.filePath(QStringLiteral("us1002-fail.mp4"))}};
                queue->clear();
                int jobsSeen = 0;
                const auto seen = QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                    jobsSeen += static_cast<int>(queue->jobs().size());
                });
                loudnessauto::setForcedFailureForTest(loudnessauto::ForcedFailure::PcmMix);
                const QJsonObject failResponse = callProjectInfoTool(callId++, QStringLiteral("export_video"), onArgs);
                const bool failCallError = toolResult(failResponse).value(QStringLiteral("isError")).toBool(true);
                const QString failJobId = toolPayload(failResponse).value(QStringLiteral("jobId")).toString();
                QString failStatus, failError;
                const bool failed = !failJobId.isEmpty() && QTest::qWaitFor([&]() {
                    const QJsonObject status = toolPayload(callProjectInfoTool(callId++,
                        QStringLiteral("get_export_status"), QJsonObject{{QStringLiteral("jobId"), failJobId}}));
                    failStatus = status.value(QStringLiteral("status")).toString();
                    failError = status.value(QStringLiteral("error")).toString();
                    return failStatus == QStringLiteral("failed");
                }, 60000);
                loudnessauto::setForcedFailureForTest(loudnessauto::ForcedFailure::None);
                QObject::disconnect(seen);
                const qsizetype jobsAfterFail = queue->jobs().size();
                const Run control = runExport(onArgs);
                describe("G188-control", control);
                std::fprintf(stderr, "mcp G188 isError=%d jobId=[%s] status=[%s] error=[%s] jobsSeen=%d jobsAfter=%lld "
                    "| control captured=%d status=%d\n", failCallError, qPrintable(failJobId), qPrintable(failStatus),
                    qPrintable(failError), jobsSeen, static_cast<long long>(jobsAfterFail), control.captured,
                    static_cast<int>(control.finalStatus));

                // G188-emptyV1: V1 empty, video only on an upper track, empty audio sequence. With V1 empty and video
                // on an upper track the ordinary export makes a silent track (US-1109), so prepareJob must too: one silent mix, one audio stream.
                // The upper clip is a shape clip: with V1 empty the renderer draws a frame only for an upper shape.
                const bool addedUpper = projectTimeline->videoTracks().size() < 2;
                if (addedUpper) projectTimeline->addVideoTrack(false);
                TimelineTrack *upperTrack = projectTimeline->videoTracks().at(1);
                const QVector<ClipInfo> savedUpperClips = upperTrack->clips();
                ShapeFill upperFill;
                upperFill.color = Qt::white;
                ShapeStroke upperStroke;
                upperStroke.enabled = false;
                Shape upperShape = ShapeLayer::createRectangle(QSizeF(64, 64), upperFill, upperStroke);
                upperShape.position = QPointF(160, 90);
                ClipInfo upperClip;
                upperClip.displayName = QStringLiteral("us1002-upper-shape");
                upperClip.duration = upperClip.outPoint = 1.0;
                upperClip.shapes = {upperShape};
                videoTrack->setClips(QVector<ClipInfo>{});
                upperTrack->setClips(QVector<ClipInfo>{upperClip});
                audioTrack->setClips(QVector<ClipInfo>{});
                const QString emptyV1Out = exportRoot.filePath(QStringLiteral("us1002-emptyv1.mp4"));
                const Run emptyV1 = runExport(QJsonObject{{QStringLiteral("autoLoudness"), true},
                    {QStringLiteral("outputPath"), emptyV1Out}});
                upperTrack->setClips(savedUpperClips);
                videoTrack->setClips(QVector<ClipInfo>{videoClip});
                audioTrack->setClips(QVector<ClipInfo>{audioClip});
                if (addedUpper) projectTimeline->removeTrack(false, projectTimeline->videoTracks().size() - 1);
                describe("G188-emptyV1", emptyV1);
                QString emptyV1JobError;
                for (const RenderJob &job : queue->jobs())
                    if (job.uuid == emptyV1.jobId) emptyV1JobError = job.error.isEmpty() ? job.errorMessage : job.error;
                int emptyV1AudioStreams = -1;
                AVFormatContext *emptyV1Format = nullptr;
                if (emptyV1.terminal
                    && avformat_open_input(&emptyV1Format, emptyV1Out.toUtf8().constData(), nullptr, nullptr) >= 0) {
                    if (avformat_find_stream_info(emptyV1Format, nullptr) >= 0) {
                        emptyV1AudioStreams = 0;
                        for (unsigned i = 0; i < emptyV1Format->nb_streams; ++i)
                            if (emptyV1Format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                                ++emptyV1AudioStreams;
                    }
                    avformat_close_input(&emptyV1Format);
                }
                int emptyV1M4aTemps = 0;
                for (const QJsonValue &temp : emptyV1.tempFiles)
                    if (temp.toString().endsWith(QStringLiteral(".m4a"), Qt::CaseInsensitive)) ++emptyV1M4aTemps;
                const bool emptyV1HasError = emptyV1.job.exportConfig.contains(QStringLiteral("loudnessAutoError"));
                const bool emptyV1Ok = observeTerm(188, "!emptyV1.isError", static_cast<bool>(!emptyV1.isError)) && observeTerm(188, "emptyV1.captured", static_cast<bool>(emptyV1.captured)) && observeTerm(188, "emptyV1.terminal", static_cast<bool>(emptyV1.terminal))
                    && observeTerm(188, "emptyV1.finalStatus == RenderJobStatus::Completed", static_cast<bool>(emptyV1.finalStatus == RenderJobStatus::Completed))
                    && observeTerm(188, "emptyV1.mixDelta == 0", static_cast<bool>(emptyV1.mixDelta == 0)) && observeTerm(188, "emptyV1.silentMixDelta == 1", static_cast<bool>(emptyV1.silentMixDelta == 1)) && observeTerm(188, "!emptyV1HasError", static_cast<bool>(!emptyV1HasError))
                    && observeTerm(188, "emptyV1.mixInTemp", static_cast<bool>(emptyV1.mixInTemp)) && observeTerm(188, "emptyV1M4aTemps == 1", static_cast<bool>(emptyV1M4aTemps == 1)) && observeTerm(188, "emptyV1AudioStreams == 1", static_cast<bool>(emptyV1AudioStreams == 1));
                std::fprintf(stderr, "mcp G188-emptyV1 ok=%d isError=%d captured=%d terminal=%d status=%d mixDelta=%d "
                    "silentMixDelta=%d loudnessAutoError=%d[%s] mixInTemp=%d m4aTemps=%d tempFiles=%lld "
                    "projectFilePath=[%s] outputAudioStreams=%d addedUpper=%d jobError=[%s]\n", emptyV1Ok, emptyV1.isError,
                    emptyV1.captured, emptyV1.terminal, static_cast<int>(emptyV1.finalStatus), emptyV1.mixDelta,
                    emptyV1.silentMixDelta, emptyV1HasError,
                    qPrintable(emptyV1.job.exportConfig.value(QStringLiteral("loudnessAutoError")).toString()),
                    emptyV1.mixInTemp, emptyV1M4aTemps, static_cast<long long>(emptyV1.tempFiles.size()),
                    qPrintable(emptyV1.job.projectFilePath), emptyV1AudioStreams, addedUpper, qPrintable(emptyV1JobError));
                tallyObserved(188, observeTerm(188, "!failCallError", static_cast<bool>(!failCallError)) && observeTerm(188, "failed", static_cast<bool>(failed)) && observeTerm(188, "!failError.isEmpty()", static_cast<bool>(!failError.isEmpty())) && observeTerm(188, "jobsSeen == 0", static_cast<bool>(jobsSeen == 0)) && observeTerm(188, "jobsAfterFail == 0", static_cast<bool>(jobsAfterFail == 0))
                    && observeTerm(188, "!control.isError", static_cast<bool>(!control.isError)) && observeTerm(188, "control.captured", static_cast<bool>(control.captured)) && observeTerm(188, "control.measureDelta == 1", static_cast<bool>(control.measureDelta == 1))
                    && observeTerm(188, "control.terminal", static_cast<bool>(control.terminal)) && observeTerm(188, "control.finalStatus == RenderJobStatus::Completed", static_cast<bool>(control.finalStatus == RenderJobStatus::Completed)) && observeTerm(188, "emptyV1Ok", static_cast<bool>(emptyV1Ok)),
                    "a forced PCM mix failure under autoLoudness=true did not end as failed (or the control was not "
                    "queued), or an empty V1 export (video on an upper track) did not get its silent audio track");
            }

            // G194 (US-1312): V1 visible with no clip and an ordinary video clip on V2. export_video
            // completes and the output has video frames (HEAD: the renderer returned a null frame and the
            // job failed). Side check: with no video clip on any track export_video is still refused.
            {
                const bool addedUpper = projectTimeline->videoTracks().size() < 2;
                if (addedUpper) projectTimeline->addVideoTrack(false);
                TimelineTrack *upperTrack = projectTimeline->videoTracks().at(1);
                const QVector<ClipInfo> savedUpperClips = upperTrack->clips();
                const bool v1Visible = !videoTrack->isHidden();
                videoTrack->setClips(QVector<ClipInfo>{});
                upperTrack->setClips(QVector<ClipInfo>{videoClip});
                const QString emptyV1UpperOut = exportRoot.filePath(QStringLiteral("us1312-v1-empty-upper.mp4"));
                const Run upperRun = runExport(QJsonObject{{QStringLiteral("outputPath"), emptyV1UpperOut}});
                QString upperJobError;
                for (const RenderJob &job : queue->jobs())
                    if (job.uuid == upperRun.jobId) upperJobError = job.error.isEmpty() ? job.errorMessage : job.error;
                int upperVideoFrames = -1;
                AVFormatContext *upperFormat = nullptr;
                if (upperRun.terminal
                    && avformat_open_input(&upperFormat, emptyV1UpperOut.toUtf8().constData(), nullptr, nullptr) >= 0) {
                    if (avformat_find_stream_info(upperFormat, nullptr) >= 0) {
                        const int videoStream = av_find_best_stream(upperFormat, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                        if (videoStream >= 0) {
                            upperVideoFrames = 0;
                            AVPacket *packet = av_packet_alloc();
                            while (packet && av_read_frame(upperFormat, packet) >= 0) {
                                if (packet->stream_index == videoStream) ++upperVideoFrames;
                                av_packet_unref(packet);
                            }
                            av_packet_free(&packet);
                        }
                    }
                    avformat_close_input(&upperFormat);
                }
                upperTrack->setClips(QVector<ClipInfo>{});
                const Run allEmptyRun = runExport(QJsonObject{{QStringLiteral("outputPath"),
                    exportRoot.filePath(QStringLiteral("us1312-all-empty.mp4"))}}, false);
                upperTrack->setClips(savedUpperClips);
                videoTrack->setClips(QVector<ClipInfo>{videoClip});
                if (addedUpper) projectTimeline->removeTrack(false, projectTimeline->videoTracks().size() - 1);
                std::fprintf(stderr, "mcp G194 v1Visible=%d isError=%d err=[%s] captured=%d terminal=%d status=%d "
                    "jobError=[%s] outputVideoFrames=%d | allEmpty isError=%d err=[%s] queueAfter=%lld addedUpper=%d "
                    "(expect visible, ok, Completed=%d, frames > 0, all-empty refused, queue 0)\n",
                    v1Visible, upperRun.isError, qPrintable(upperRun.errorText), upperRun.captured, upperRun.terminal,
                    static_cast<int>(upperRun.finalStatus), qPrintable(upperJobError), upperVideoFrames,
                    allEmptyRun.isError, qPrintable(allEmptyRun.errorText),
                    static_cast<long long>(allEmptyRun.queueAfter), addedUpper,
                    static_cast<int>(RenderJobStatus::Completed));
                tallyObserved(194, observeTerm(194, "V1 visible", v1Visible)
                    && observeTerm(194, "export_video ok", !upperRun.isError)
                    && observeTerm(194, "job captured", upperRun.captured)
                    && observeTerm(194, "job terminal", upperRun.terminal)
                    && observeTerm(194, "job Completed", upperRun.finalStatus == RenderJobStatus::Completed)
                    && observeTerm(194, "output video frames > 0", upperVideoFrames > 0)
                    && observeTerm(194, "all-empty export refused", allEmptyRun.isError)
                    && observeTerm(194, "all-empty nothing queued", allEmptyRun.queueAfter == 0),
                    "export_video of a timeline whose visible V1 holds no clip (ordinary video on V2) did not "
                    "complete with video frames, or a timeline without any video clip was not refused");
            }

            // G189 (US-1127): export_video autoLoudness=true prepares the audio on a worker, so a client on
            // another thread (through the HTTP transport) gets get_export_status answered while the 500 ms
            // preparation runs. Terms are printed; the control blocks the GUI thread on purpose and must be
            // seen as slow by the same client, so a client that cannot observe blocking cannot pass the gate.
            {
                using SteadyClock = std::chrono::steady_clock;
                const auto nowNs = []() {
                    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                        SteadyClock::now().time_since_epoch()).count();
                };
                mcp::McpHttpServer exportServer(&projectInfoRegistry);
                const bool exportServerStarted = exportServer.start();
                struct StatusReply {
                    std::atomic<bool> done{false};
                    std::atomic<long long> sentNs{0};
                    std::atomic<long long> repliedNs{0};
                    QString status;
                    QString body;
                };
                // One get_export_status over the transport, blocking sockets on a non-GUI thread.
                // The GUI thread must keep running its event loop meanwhile.
                const auto askStatus = [&](const QString &jobId, int waitBeforeMs, StatusReply &reply) {
                    const QUrl url(exportServer.endpointUrl());
                    const QByteArray token = exportServer.token().toUtf8();
                    const QByteArray body = compact(rpcRequest(
                        7000, QStringLiteral("tools/call"), QJsonObject{
                            {QStringLiteral("name"), QStringLiteral("get_export_status")},
                            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("jobId"), jobId}}}}));
                    return std::thread([&reply, url, token, body, waitBeforeMs, nowNs]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(waitBeforeMs));
                        QTcpSocket socket;
                        socket.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(url.port()));
                        reply.sentNs = nowNs();
                        QByteArray response;
                        if (socket.waitForConnected(10000)) {
                            QByteArray request = "POST " + url.path().toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "Content-Type: application/json\r\nAuthorization: Bearer " + token
                                + "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size())
                                + "\r\n\r\n" + body;
                            socket.write(request);
                            while (socket.state() == QAbstractSocket::ConnectedState
                                   && socket.waitForReadyRead(10000)) {
                                response += socket.readAll();
                                const int headerEnd = response.indexOf("\r\n\r\n");
                                if (headerEnd >= 0) {
                                    const QByteArray head = response.left(headerEnd).toLower();
                                    const int lenAt = head.indexOf("content-length:");
                                    if (lenAt >= 0) {
                                        int lenEnd = head.indexOf("\r\n", lenAt);
                                        if (lenEnd < 0) lenEnd = head.size();
                                        const int contentLength = head.mid(lenAt + 15, lenEnd - lenAt - 15).trimmed().toInt();
                                        if (response.size() - (headerEnd + 4) >= contentLength) break;
                                    }
                                }
                            }
                            response += socket.readAll();
                        }
                        reply.repliedNs = nowNs();
                        const int headerEnd = response.indexOf("\r\n\r\n");
                        reply.body = QString::fromUtf8(headerEnd >= 0 ? response.mid(headerEnd + 4) : response);
                        const QJsonObject payload = parseObject(reply.body.toUtf8())
                            .value(QStringLiteral("result")).toObject()
                            .value(QStringLiteral("structuredContent")).toObject();
                        reply.status = payload.value(QStringLiteral("status")).toString();
                        reply.done = true;
                    });
                };

                bool controlSlow = false;
                double controlMs = -1.0;
                QString controlStatus;
                bool preparingOk = false;
                bool doneOk = false;
                double latencyMs = -1.0;
                double jobAddedAfterMs = -1.0;
                double repliedBeforeAddMs = -1.0;
                QString duringStatus, finalStatusText;
                if (exportServerStarted) {
                    // Control: the same client against a GUI thread that is blocked for 400 ms.
                    StatusReply controlReply;
                    std::thread controlClient = askStatus(QStringLiteral("g189-control"), 50, controlReply);
                    QTimer::singleShot(0, &projectInfoWindow, []() { QThread::msleep(400); });
                    (void)QTest::qWaitFor([&]() { return controlReply.done.load(); }, 20000);
                    controlClient.join();
                    controlMs = static_cast<double>(controlReply.repliedNs - controlReply.sentNs) / 1.0e6;
                    controlStatus = controlReply.status;
                    controlSlow = controlReply.done && controlMs > 100.0;

                    queue->clear();
                    std::atomic<long long> addedNs{0};
                    const auto added = QObject::connect(queue, &RenderQueue::jobsChanged, queue, [&]() {
                        if (!queue->jobs().isEmpty() && addedNs.load() == 0) addedNs = nowNs();
                    });
                    audioexport::setPrepareDelayForTest(500);
                    const long long callNs = nowNs();
                    const QJsonObject response = callProjectInfoTool(callId++, QStringLiteral("export_video"),
                        QJsonObject{{QStringLiteral("autoLoudness"), true}, {QStringLiteral("width"), 320},
                                    {QStringLiteral("height"), 180}, {QStringLiteral("fps"), 10},
                                    {QStringLiteral("outputPath"), exportRoot.filePath(QStringLiteral("us1127-async.mp4"))}});
                    const QString jobId = toolPayload(response).value(QStringLiteral("jobId")).toString();
                    StatusReply reply;
                    // 50 ms: the deferred step (singleShot(0)) has started preparing by then.
                    std::thread client = askStatus(jobId, 50, reply);
                    (void)QTest::qWaitFor([&]() { return reply.done.load(); }, 20000);
                    client.join();
                    audioexport::setPrepareDelayForTest(0);
                    latencyMs = static_cast<double>(reply.repliedNs - reply.sentNs) / 1.0e6;
                    duringStatus = reply.status;
                    // Preparing was still running when the reply came back: the job was added after it.
                    (void)QTest::qWaitFor([&]() { return addedNs.load() != 0; }, 60000);
                    jobAddedAfterMs = static_cast<double>(addedNs - callNs) / 1.0e6;
                    repliedBeforeAddMs = static_cast<double>(addedNs - reply.repliedNs) / 1.0e6;
                    preparingOk = !jobId.isEmpty() && reply.done && latencyMs < 100.0
                        && duringStatus == QStringLiteral("queued") && addedNs.load() > reply.repliedNs
                        && jobAddedAfterMs > 400.0;
                    doneOk = !jobId.isEmpty() && QTest::qWaitFor([&]() {
                        finalStatusText = toolPayload(callProjectInfoTool(callId++,
                            QStringLiteral("get_export_status"), QJsonObject{{QStringLiteral("jobId"), jobId}}))
                            .value(QStringLiteral("status")).toString();
                        return finalStatusText == QStringLiteral("done") || finalStatusText == QStringLiteral("failed");
                    }, 120000) && finalStatusText == QStringLiteral("done");
                    QObject::disconnect(added);
                    queue->clear();
                    exportServer.stop();
                }
                std::fprintf(stderr, "mcp G189 serverStarted=%d control: latencyMs=%.1f slow=%d status=[%s] | "
                    "during: latencyMs=%.1f status=[%s] jobAddedAfterCallMs=%.1f repliedBeforeAddMs=%.1f preparing=%d | "
                    "final status=[%s] done=%d\n", exportServerStarted, controlMs, controlSlow, qPrintable(controlStatus),
                    latencyMs, qPrintable(duringStatus), jobAddedAfterMs, repliedBeforeAddMs, preparingOk,
                    qPrintable(finalStatusText), doneOk);
                tallyObserved(189, observeTerm(189, "exportServerStarted", static_cast<bool>(exportServerStarted))
                    && observeTerm(189, "controlSlow", static_cast<bool>(controlSlow))
                    && observeTerm(189, "preparingOk", static_cast<bool>(preparingOk))
                    && observeTerm(189, "doneOk", static_cast<bool>(doneOk)),
                    "get_export_status over the transport was not answered within 100 ms as queued while export_video "
                    "prepared its auto loudness audio (500 ms), or the export did not end done, or the blocked-GUI "
                    "control was not seen as slow");
            }

            queue->clear();
            if (hadPresetEnv) qputenv("VEDITOR_EXPORT_PRESET_DIR", oldPresetEnv);
            else qunsetenv("VEDITOR_EXPORT_PRESET_DIR");
            projectInfoWindow.m_mcpTools = previousTools;
            videoTrack->setClips(savedVideoClips);
            for (auto &entry : savedAudioClips)
                entry.first->setClips(entry.second);
        }
    }

    // G190 / G191 (US-1201): connect_clip connects a V2 clip to a V1 clip and
    // get_timeline reports the connection; a V2 anchor is rejected unchanged.
    {
        Timeline* connectTimeline = projectInfoWindow.m_timeline;
        ClipInfo anchor;
        anchor.filePath = QStringLiteral("/selftests/mcp-connect-anchor.mp4");
        anchor.duration = anchor.outPoint = 10.0;
        ClipInfo connected = anchor;
        connected.filePath = QStringLiteral("/selftests/mcp-connect-x.mp4");
        connected.duration = connected.outPoint = 2.0;
        connected.leadInSec = 4.0;
        ClipInfo other = connected;
        other.filePath = QStringLiteral("/selftests/mcp-connect-y.mp4");
        other.leadInSec = 1.0;
        connectTimeline->restoreFromProject(
            QVector<QVector<ClipInfo>>{{anchor}, {connected, other}},
            QVector<QVector<ClipInfo>>{{}}, 0.0, -1.0, -1.0, 100);
        const QString anchorUid = connectTimeline->trackAt(false, 0)->clips().at(0).uid;
        const quint64 serialBefore = connectTimeline->undoManager()->saveSerial();
        const QJsonObject connectPayload = toolPayload(callProjectInfoTool(
            1901, QStringLiteral("connect_clip"),
            QJsonObject{{"trackIndex", 1}, {"clipIndex", 0}, {"anchorClipIndex", 0}}));
        const quint64 serialAfter = connectTimeline->undoManager()->saveSerial();
        const QJsonObject timelinePayload = toolPayload(callProjectInfoTool(
            1902, QStringLiteral("get_timeline"), QJsonObject{{"kind", "video"}}));
        const QJsonObject reported = timelinePayload.value(QStringLiteral("video")).toArray()
            .at(1).toObject().value(QStringLiteral("clips")).toArray().at(0).toObject();
        const QJsonObject unconnected = timelinePayload.value(QStringLiteral("video")).toArray()
            .at(1).toObject().value(QStringLiteral("clips")).toArray().at(1).toObject();
        std::fprintf(stderr, "mcp G190 ok=%d anchorUid=[%s] reported=[%s] offset=%.6f "
                     "unconnectedHasKey=%d serialDelta=%llu warnings=%lld\n",
                     connectPayload.value(QStringLiteral("ok")).toBool(), qPrintable(anchorUid),
                     qPrintable(reported.value(QStringLiteral("anchorUid")).toString()),
                     reported.value(QStringLiteral("anchorOffsetSec")).toDouble(),
                     unconnected.contains(QStringLiteral("anchorUid")),
                     static_cast<unsigned long long>(serialAfter - serialBefore),
                     static_cast<long long>(connectPayload.value(QStringLiteral("warnings")).toArray().size()));
        tallyObserved(190, observeTerm(190, "connect ok", connectPayload.value(QStringLiteral("ok")).toBool())
            && observeTerm(190, "reported anchorUid", !anchorUid.isEmpty()
                && reported.value(QStringLiteral("anchorUid")).toString() == anchorUid)
            && observeTerm(190, "reported offset 4", std::abs(reported.value(QStringLiteral("anchorOffsetSec")).toDouble() - 4.0) < 1e-6)
            && observeTerm(190, "unconnected clip omits the keys", !unconnected.contains(QStringLiteral("anchorUid"))
                && !unconnected.contains(QStringLiteral("anchorOffsetSec")))
            && observeTerm(190, "serial +1", serialAfter - serialBefore == 1),
            "connect_clip did not connect the V2 clip, get_timeline did not report anchorUid / "
            "anchorOffsetSec, or the undo serial did not grow by one");

        const quint64 rejectBefore = connectTimeline->undoManager()->saveSerial();
        const QJsonObject rejected = toolResult(callProjectInfoTool(
            1903, QStringLiteral("connect_clip"),
            QJsonObject{{"trackIndex", 1}, {"clipIndex", 1}, {"anchorTrackIndex", 1},
                        {"anchorClipIndex", 0}}));
        const ClipInfo afterReject = connectTimeline->trackAt(false, 1)->clips().at(1);
        const QString rejectText = rejected.value(QStringLiteral("content")).toArray()
            .at(0).toObject().value(QStringLiteral("text")).toString();
        std::fprintf(stderr, "mcp G191 isError=%d text=[%s] anchorUid=[%s] serialDelta=%llu\n",
                     rejected.value(QStringLiteral("isError")).toBool(), qPrintable(rejectText),
                     qPrintable(afterReject.anchorUid),
                     static_cast<unsigned long long>(connectTimeline->undoManager()->saveSerial() - rejectBefore));
        tallyObserved(191, observeTerm(191, "isError", rejected.value(QStringLiteral("isError")).toBool(false))
            && observeTerm(191, "unchanged", afterReject.anchorUid.isEmpty())
            && observeTerm(191, "serial unchanged", connectTimeline->undoManager()->saveSerial() == rejectBefore),
            "connect_clip accepted a V2 anchor or changed the timeline / undo serial when rejecting it");
    }

    // G192 (US-1214): delete_clip of the anchor deletes its connected V2+A2
    // group; get_timeline then lists neither while an unconnected V2 clip stays.
    {
        Timeline* deleteTimeline = projectInfoWindow.m_timeline;
        ClipInfo anchor;
        anchor.filePath = QStringLiteral("/selftests/mcp-delete-anchor.mp4");
        anchor.duration = anchor.outPoint = 10.0;
        ClipInfo connected = anchor;
        connected.filePath = QStringLiteral("/selftests/mcp-delete-x.mp4");
        connected.duration = connected.outPoint = 1.0;
        connected.leadInSec = 2.0;
        connected.linkGroup = 192;
        ClipInfo other = connected;
        other.filePath = QStringLiteral("/selftests/mcp-delete-y.mp4");
        other.linkGroup = 0;
        other.leadInSec = 3.0;
        ClipInfo connectedAudio = connected;
        connectedAudio.filePath = QStringLiteral("/selftests/mcp-delete-xa.wav");
        deleteTimeline->restoreFromProject(
            QVector<QVector<ClipInfo>>{{anchor}, {connected, other}},
            QVector<QVector<ClipInfo>>{{}, {connectedAudio}}, 0.0, -1.0, -1.0, 100);
        deleteTimeline->undoManager()->clear();
        deleteTimeline->saveUndoState(QStringLiteral("mcp G192 initial"));
        const QString anchorUid = deleteTimeline->trackAt(false, 0)->clips().at(0).uid;
        const bool connectedBefore = deleteTimeline->connectClip(TrackKind::Video, 1, 0, anchorUid)
            && deleteTimeline->trackAt(true, 1)->clips().at(0).anchorUid == anchorUid;
        const QJsonObject deletePayload = toolPayload(callProjectInfoTool(
            1921, QStringLiteral("delete_clip"), QJsonObject{{"trackIndex", 0}, {"clipIndex", 0}}));
        const QJsonObject timelinePayload = toolPayload(callProjectInfoTool(
            1922, QStringLiteral("get_timeline"), QJsonObject{}));
        const QJsonArray v2Clips = timelinePayload.value(QStringLiteral("video")).toArray()
            .at(1).toObject().value(QStringLiteral("clips")).toArray();
        const QJsonArray a2Clips = timelinePayload.value(QStringLiteral("audio")).toArray()
            .at(1).toObject().value(QStringLiteral("clips")).toArray();
        QStringList v2Paths;
        for (const QJsonValue &clip : v2Clips)
            v2Paths.append(clip.toObject().value(QStringLiteral("filePath")).toString());
        std::fprintf(stderr, "mcp G192 connectedBefore=%d delete ok=%d remainingClipCount=%d keys=[%s] "
                     "v2Clips=%lld [%s] (expect only mcp-delete-y) a2Clips=%lld (expect 0)\n",
                     connectedBefore, deletePayload.value(QStringLiteral("ok")).toBool(),
                     deletePayload.value(QStringLiteral("remainingClipCount")).toInt(-1),
                     qPrintable(QStringList(deletePayload.keys()).join(',')),
                     static_cast<long long>(v2Clips.size()), qPrintable(v2Paths.join(',')),
                     static_cast<long long>(a2Clips.size()));
        tallyObserved(192, observeTerm(192, "connected before", connectedBefore)
            && observeTerm(192, "delete ok", deletePayload.value(QStringLiteral("ok")).toBool())
            && observeTerm(192, "remainingClipCount 0",
                           deletePayload.value(QStringLiteral("remainingClipCount")).toInt(-1) == 0)
            && observeTerm(192, "response keys unchanged",
                           QStringList(deletePayload.keys()) == QStringList({QStringLiteral("ok"),
                                                                              QStringLiteral("remainingClipCount")}))
            && observeTerm(192, "unconnected V2 clip kept", v2Clips.size() == 1
                && v2Paths.value(0).endsWith(QStringLiteral("mcp-delete-y.mp4")))
            && observeTerm(192, "connected A2 clip deleted", a2Clips.isEmpty()),
            "delete_clip of the anchor kept its connected group, deleted the unconnected clip, or "
            "changed its response");
    }

    // G193 (US-1306): import_media honours a locked track. A1 locked + explicit trackIndex 0 is
    // refused with the drop-path wording and the timeline keeps its clip counts.
    {
        Timeline* lockTimeline = projectInfoWindow.m_timeline;
        ClipInfo lockedV;
        lockedV.filePath = QStringLiteral("/selftests/mcp-lock-v.mp4");
        lockedV.duration = lockedV.outPoint = 2.0;
        ClipInfo lockedA = lockedV;
        lockedA.filePath = QStringLiteral("/selftests/mcp-lock-a.wav");
        lockTimeline->restoreFromProject(
            QVector<QVector<ClipInfo>>{{lockedV}}, QVector<QVector<ClipInfo>>{{lockedA}},
            0.0, -1.0, -1.0, 100);
        const QString lockImportPath = QDir::current().absoluteFilePath(
            QStringLiteral("test_assets/e2e_clip.mp4"));
        const QJsonObject lockResponse = callProjectInfoTool(
            1931, QStringLiteral("set_track_locked"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("audio")},
                {QStringLiteral("trackIndex"), 0}, {QStringLiteral("locked"), true}});
        const bool lockSet = toolPayload(lockResponse).value(QStringLiteral("ok")).toBool(false)
            && lockTimeline->trackAt(true, 0)->isLocked();
        const QJsonObject importResponse = callProjectInfoTool(
            1932, QStringLiteral("import_media"), QJsonObject{
                {QStringLiteral("filePath"), lockImportPath},
                {QStringLiteral("trackIndex"), 0}, {QStringLiteral("startSec"), 10.0}});
        const bool importIsError = toolResult(importResponse).value(QStringLiteral("isError")).toBool(false);
        const QString importError = toolErrorText(importResponse);
        const int v1Count = lockTimeline->trackAt(false, 0)->clipCount();
        const int a1Count = lockTimeline->trackAt(true, 0)->clipCount();
        const QJsonObject lockedReadback = toolPayload(callProjectInfoTool(
            1933, QStringLiteral("get_timeline"), {}));
        const QJsonArray readbackVideo = lockedReadback.value(QStringLiteral("video")).toArray();
        const QJsonArray readbackAudio = lockedReadback.value(QStringLiteral("audio")).toArray();
        const int mcpV1Clips = readbackVideo.isEmpty() ? -1
            : readbackVideo.at(0).toObject().value(QStringLiteral("clips")).toArray().size();
        const int mcpA1Clips = readbackAudio.isEmpty() ? -1
            : readbackAudio.at(0).toObject().value(QStringLiteral("clips")).toArray().size();
        std::fprintf(stderr, "mcp G193 lockSet=%d import isError=%d error=[%s] V1 clips=%d A1 clips=%d "
                     "get_timeline V1 clips=%d A1 clips=%d "
                     "(expect isError, locked wording, 1, 1, 1, 1)\n", lockSet, importIsError,
                     qPrintable(importError), v1Count, a1Count, mcpV1Clips, mcpA1Clips);
        tallyObserved(193, observeTerm(193, "A1 locked", lockSet)
            && observeTerm(193, "import refused", importIsError)
            && observeTerm(193, "locked wording",
                           importError.contains(QStringLiteral("ロック中のトラックには配置できません")))
            && observeTerm(193, "V1 clip count unchanged", v1Count == 1)
            && observeTerm(193, "A1 clip count unchanged", a1Count == 1)
            && observeTerm(193, "get_timeline video[0].clips unchanged", mcpV1Clips == 1)
            && observeTerm(193, "get_timeline audio[0].clips unchanged", mcpA1Clips == 1),
            "import_media placed a clip on a locked track or reported a different error");
    }

    // G195-G199 (US-1711): camera auto-orient / 3D-layer switch / wiggle / expressions through MCP.
    // HEAD has none of the three tools nor the two set_clip_property values, so every gate fails there.
    {
        Timeline* dynTimeline = projectInfoWindow.m_timeline;
        int dynId = 19500;
        const auto resetDynClips = [&]() {
            ClipInfo dynV;
            dynV.filePath = QStringLiteral("/selftests/mcp-dyn-v.mp4");
            dynV.duration = dynV.outPoint = 4.0;
            dynV.is3DLayer = true;
            dynV.layer3D.positionZ = 50.0;
            dynV.layer3D.rotationX = 10.0;
            dynV.layer3D.rotationY = 20.0;
            dynV.layer3D.rotationZ = 30.0;
            ClipInfo dynA = dynV;
            dynA.filePath = QStringLiteral("/selftests/mcp-dyn-a.wav");
            dynA.is3DLayer = false;
            dynA.layer3D = Layer3DTransform();
            dynTimeline->restoreFromProject(
                QVector<QVector<ClipInfo>>{{dynV}}, QVector<QVector<ClipInfo>>{{dynA}},
                0.0, -1.0, -1.0, 100);
            dynTimeline->applyTrackFlagsFromJson(QJsonObject{});
            dynTimeline->undoManager()->clear();
            dynTimeline->undoManager()->saveState(
                dynTimeline->currentState(), QStringLiteral("MCP selftest baseline"));
        };
        const auto call = [&](const QString& name, const QJsonObject& args) {
            return callProjectInfoTool(++dynId, name, args);
        };
        const auto vArgs = [](const QJsonObject& extra) {
            QJsonObject args{{QStringLiteral("trackIndex"), 0}, {QStringLiteral("clipIndex"), 0}};
            for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
                args.insert(it.key(), it.value());
            return args;
        };
        const auto isErr = [&](const QJsonObject& response) {
            return toolResult(response).value(QStringLiteral("isError")).toBool(false);
        };
        const auto dynamics = [&]() {
            return toolPayload(call(QStringLiteral("get_clip_dynamics"), vArgs({})));
        };
        const auto setProp = [&](const QString& property, bool value) {
            return call(QStringLiteral("set_clip_property"),
                        vArgs({{QStringLiteral("property"), property}, {QStringLiteral("value"), value}}));
        };
        const auto undoOnce = [&]() { return call(QStringLiteral("undo"), QJsonObject{}); };

        // G195: autoOrientToCamera true -> get_clip_dynamics / get_timeline true, one undo -> false;
        // is3DLayer true -> false zeroes positionZ / rotationX/Y/Z, one undo restores them.
        {
            resetDynClips();
            const QJsonObject setCam = toolPayload(setProp(QStringLiteral("autoOrientToCamera"), true));
            const bool camOn = dynamics().value(QStringLiteral("autoOrientToCamera")).toBool(false);
            const QJsonObject timelinePayload = toolPayload(call(QStringLiteral("get_timeline"),
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("video")}}));
            const bool timelineCamOn = timelinePayload.value(QStringLiteral("video")).toArray().at(0).toObject()
                .value(QStringLiteral("clips")).toArray().at(0).toObject()
                .value(QStringLiteral("autoOrientToCamera")).toBool(false);
            undoOnce();
            const bool camOff = !dynamics().value(QStringLiteral("autoOrientToCamera")).toBool(true);
            const QJsonObject set3d = toolPayload(setProp(QStringLiteral("is3DLayer"), false));
            const ClipInfo flat = dynTimeline->trackAt(false, 0)->clips().first();
            const bool dyn3dOff = !dynamics().value(QStringLiteral("is3DLayer")).toBool(true);
            undoOnce();
            const ClipInfo restored = dynTimeline->trackAt(false, 0)->clips().first();
            std::fprintf(stderr, "mcp G195 camOn=%d timelineCamOn=%d camOff=%d is3D(resp)=%d flat: is3D=%d z=%g rx=%g ry=%g rz=%g "
                         "restored: is3D=%d z=%g rx=%g ry=%g rz=%g (expect 1,1,1,0, 0,0,0,0,0, 1,50,10,20,30)\n",
                         camOn, timelineCamOn, camOff, set3d.value(QStringLiteral("is3DLayer")).toBool(true),
                         flat.is3DLayer, flat.layer3D.positionZ, flat.layer3D.rotationX, flat.layer3D.rotationY,
                         flat.layer3D.rotationZ, restored.is3DLayer, restored.layer3D.positionZ,
                         restored.layer3D.rotationX, restored.layer3D.rotationY, restored.layer3D.rotationZ);
            tallyObserved(195, observeTerm(195, "autoOrientToCamera set ok",
                                           setCam.value(QStringLiteral("ok")).toBool(false))
                && observeTerm(195, "dynamics autoOrientToCamera true", camOn)
                && observeTerm(195, "get_timeline autoOrientToCamera true", timelineCamOn)
                && observeTerm(195, "one undo -> false", camOff)
                && observeTerm(195, "is3DLayer response false",
                               set3d.contains(QStringLiteral("is3DLayer"))
                                   && !set3d.value(QStringLiteral("is3DLayer")).toBool(true))
                && observeTerm(195, "layer3D zeroed", !flat.is3DLayer && flat.layer3D.positionZ == 0.0
                               && flat.layer3D.rotationX == 0.0 && flat.layer3D.rotationY == 0.0
                               && flat.layer3D.rotationZ == 0.0)
                && observeTerm(195, "dynamics is3DLayer false", dyn3dOff)
                && observeTerm(195, "one undo restores layer3D", restored.is3DLayer
                               && restored.layer3D.positionZ == 50.0 && restored.layer3D.rotationX == 10.0
                               && restored.layer3D.rotationY == 20.0 && restored.layer3D.rotationZ == 30.0),
                "autoOrientToCamera / is3DLayer through set_clip_property did not round-trip or undo");
        }

        // G196: set_clip_wiggle round trip, unknown key refused (state unchanged), octaves 7 -> 6, one undo.
        {
            resetDynClips();
            const QJsonObject wiggleArg{
                {QStringLiteral("enabled"), true}, {QStringLiteral("seed"), 7}, {QStringLiteral("octaves"), 7},
                {QStringLiteral("positionFrequency"), 3.0},
                {QStringLiteral("positionAmplitude"), QJsonObject{{QStringLiteral("x"), 4.0}, {QStringLiteral("y"), -2.0}}}};
            const QJsonObject setResp = call(QStringLiteral("set_clip_wiggle"),
                                             vArgs({{QStringLiteral("wiggle"), wiggleArg}}));
            const QJsonObject stored = dynamics().value(QStringLiteral("wiggle")).toObject();
            const wiggle::WiggleParams direct = dynTimeline->trackAt(false, 0)->clips().first().wiggle;
            const QJsonObject badResp = call(QStringLiteral("set_clip_wiggle"),
                vArgs({{QStringLiteral("wiggle"), QJsonObject{{QStringLiteral("enabled"), true},
                                                              {QStringLiteral("bogusKey"), 1}}}}));
            const QString badError = toolErrorText(badResp);
            const wiggle::WiggleParams afterBad = dynTimeline->trackAt(false, 0)->clips().first().wiggle;
            undoOnce();
            const wiggle::WiggleParams afterUndo = dynTimeline->trackAt(false, 0)->clips().first().wiggle;
            std::fprintf(stderr, "mcp G196 setErr=%d stored: enabled=%d seed=%d octaves=%d px=%g py=%g pf=%g "
                         "bad isError=%d error=[%s] afterBad.octaves=%d afterUndo.enabled=%d "
                         "(expect 0, 1,7,6,4,-2,3, 1, unknown wiggle key, 6, 0)\n",
                         isErr(setResp), stored.value(QStringLiteral("enabled")).toBool(),
                         stored.value(QStringLiteral("seed")).toInt(-1),
                         stored.value(QStringLiteral("octaves")).toInt(-1),
                         stored.value(QStringLiteral("positionAmplitude")).toObject().value(QStringLiteral("x")).toDouble(),
                         stored.value(QStringLiteral("positionAmplitude")).toObject().value(QStringLiteral("y")).toDouble(),
                         stored.value(QStringLiteral("positionFrequency")).toDouble(),
                         isErr(badResp), qPrintable(badError), afterBad.octaves, afterUndo.enabled);
            tallyObserved(196, observeTerm(196, "set ok", !isErr(setResp)
                                           && toolPayload(setResp).value(QStringLiteral("ok")).toBool(false))
                && observeTerm(196, "octaves clamped 7 -> 6",
                               stored.value(QStringLiteral("octaves")).toInt(-1) == 6 && direct.octaves == 6)
                && observeTerm(196, "round trip", stored.value(QStringLiteral("enabled")).toBool(false)
                               && stored.value(QStringLiteral("seed")).toInt(-1) == 7
                               && stored.value(QStringLiteral("positionFrequency")).toDouble() == 3.0
                               && stored.value(QStringLiteral("positionAmplitude")).toObject()
                                      .value(QStringLiteral("x")).toDouble() == 4.0
                               && stored.value(QStringLiteral("positionAmplitude")).toObject()
                                      .value(QStringLiteral("y")).toDouble() == -2.0)
                && observeTerm(196, "unknown key refused", isErr(badResp)
                               && badError.contains(QStringLiteral("unknown wiggle key: bogusKey")))
                && observeTerm(196, "refused call left the wiggle", afterBad.enabled && afterBad.octaves == 6)
                && observeTerm(196, "one undo disables", !afterUndo.enabled),
                "set_clip_wiggle did not round-trip, clamp octaves, refuse unknown keys or undo in one step");
        }

        // G197: set_clip_expression set / clear / invalid propPath.
        {
            resetDynClips();
            const QJsonObject setResp = call(QStringLiteral("set_clip_expression"),
                vArgs({{QStringLiteral("propPath"), QStringLiteral("transform.position.x")},
                       {QStringLiteral("code"), QStringLiteral("time * 10")}}));
            const QJsonObject setExprs = dynamics().value(QStringLiteral("expressions")).toObject();
            const QJsonObject badResp = call(QStringLiteral("set_clip_expression"),
                vArgs({{QStringLiteral("propPath"), QStringLiteral("transform.bogus")},
                       {QStringLiteral("code"), QStringLiteral("1")}}));
            const QString badError = toolErrorText(badResp);
            const QJsonObject afterBad = dynamics().value(QStringLiteral("expressions")).toObject();
            const QJsonObject clearResp = call(QStringLiteral("set_clip_expression"),
                vArgs({{QStringLiteral("propPath"), QStringLiteral("transform.position.x")},
                       {QStringLiteral("code"), QStringLiteral("   ")}}));
            const QJsonObject cleared = dynamics().value(QStringLiteral("expressions")).toObject();
            std::fprintf(stderr, "mcp G197 set=[%s] bad isError=%d error=[%s] afterBad keys=%d "
                         "clear cleared=%d after keys=%d (expect time * 10, 1, Unknown property path: transform.bogus, 1, 1, 0)\n",
                         qPrintable(setExprs.value(QStringLiteral("transform.position.x")).toString()),
                         isErr(badResp), qPrintable(badError), static_cast<int>(afterBad.size()),
                         toolPayload(clearResp).value(QStringLiteral("cleared")).toBool(false), static_cast<int>(cleared.size()));
            tallyObserved(197, observeTerm(197, "set ok", !isErr(setResp))
                && observeTerm(197, "expression stored",
                               setExprs.value(QStringLiteral("transform.position.x")).toString()
                                   == QStringLiteral("time * 10"))
                && observeTerm(197, "invalid propPath refused", isErr(badResp)
                               && badError.contains(QStringLiteral("Unknown property path: transform.bogus")))
                && observeTerm(197, "refused call left the binding", afterBad.size() == 1)
                && observeTerm(197, "blank code clears", !isErr(clearResp)
                               && toolPayload(clearResp).value(QStringLiteral("cleared")).toBool(false)
                               && cleared.isEmpty()),
                "set_clip_expression did not set, clear on blank code, or refuse an unknown propPath");
        }

        // G198: audio clips / audio track index are refused by all four entry points; clip untouched.
        {
            resetDynClips();
            const QJsonObject audioSel{{QStringLiteral("kind"), QStringLiteral("audio")}};
            const QJsonObject r1 = call(QStringLiteral("set_clip_property"),
                vArgs({{QStringLiteral("kind"), QStringLiteral("audio")},
                       {QStringLiteral("property"), QStringLiteral("is3DLayer")}, {QStringLiteral("value"), true}}));
            const QJsonObject r2 = call(QStringLiteral("set_clip_wiggle"),
                vArgs({{QStringLiteral("kind"), QStringLiteral("audio")},
                       {QStringLiteral("wiggle"), QJsonObject{{QStringLiteral("enabled"), true}}}}));
            const QJsonObject r3 = call(QStringLiteral("set_clip_expression"),
                vArgs({{QStringLiteral("kind"), QStringLiteral("audio")},
                       {QStringLiteral("propPath"), QStringLiteral("transform.scale")}, {QStringLiteral("code"), QStringLiteral("2")}}));
            const QJsonObject r4 = call(QStringLiteral("get_clip_dynamics"),
                vArgs({{QStringLiteral("kind"), QStringLiteral("audio")}}));
            const QJsonObject r5 = call(QStringLiteral("set_clip_wiggle"),
                vArgs({{QStringLiteral("trackIndex"), 5},
                       {QStringLiteral("wiggle"), QJsonObject{{QStringLiteral("enabled"), true}}}}));
            const ClipInfo audioClip = dynTimeline->trackAt(true, 0)->clips().first();
            const ClipInfo videoClip = dynTimeline->trackAt(false, 0)->clips().first();
            const QString e1 = toolErrorText(r1), e2 = toolErrorText(r2), e3 = toolErrorText(r3),
                          e4 = toolErrorText(r4), e5 = toolErrorText(r5);
            std::fprintf(stderr, "mcp G198 errors: [%s] | [%s] | [%s] | [%s] | out-of-range track [%s] "
                         "audio is3D=%d wiggle=%d video wiggle=%d (expect 1 audio-clip wording x4, track index is out of range, 0,0,0)\n",
                         qPrintable(e1), qPrintable(e2), qPrintable(e3), qPrintable(e4), qPrintable(e5),
                         audioClip.is3DLayer, audioClip.wiggle.enabled, videoClip.wiggle.enabled);
            const QString wording = QStringLiteral("音声クリップ");
            tallyObserved(198, observeTerm(198, "set_clip_property refused", isErr(r1) && e1.contains(wording))
                && observeTerm(198, "set_clip_wiggle refused", isErr(r2) && e2.contains(wording))
                && observeTerm(198, "set_clip_expression refused", isErr(r3) && e3.contains(wording))
                && observeTerm(198, "get_clip_dynamics refused", isErr(r4) && e4.contains(wording))
                && observeTerm(198, "track index out of range", isErr(r5)
                               && e5.contains(QStringLiteral("track index is out of range")))
                && observeTerm(198, "nothing changed", !audioClip.is3DLayer && !audioClip.wiggle.enabled
                               && !videoClip.wiggle.enabled),
                "an audio clip or an out-of-range track index was accepted by a dynamics tool");
        }

        // G199: a locked video track refuses every dynamics write; reads still work; clip untouched.
        {
            resetDynClips();
            const bool locked = toolPayload(call(QStringLiteral("set_track_locked"),
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("video")}, {QStringLiteral("trackIndex"), 0},
                            {QStringLiteral("locked"), true}})).value(QStringLiteral("ok")).toBool(false)
                && dynTimeline->trackAt(false, 0)->isLocked();
            const QJsonObject r1 = setProp(QStringLiteral("autoOrientToCamera"), true);
            const QJsonObject r2 = setProp(QStringLiteral("is3DLayer"), false);
            const QJsonObject r3 = call(QStringLiteral("set_clip_wiggle"),
                vArgs({{QStringLiteral("wiggle"), QJsonObject{{QStringLiteral("enabled"), true}}}}));
            const QJsonObject r4 = call(QStringLiteral("set_clip_expression"),
                vArgs({{QStringLiteral("propPath"), QStringLiteral("transform.scale")}, {QStringLiteral("code"), QStringLiteral("2")}}));
            const QJsonObject readBack = dynamics();
            const ClipInfo lockedClip = dynTimeline->trackAt(false, 0)->clips().first();
            const QString e1 = toolErrorText(r1), e2 = toolErrorText(r2), e3 = toolErrorText(r3), e4 = toolErrorText(r4);
            std::fprintf(stderr, "mcp G199 locked=%d errors: [%s] | [%s] | [%s] | [%s] clip: cam=%d is3D=%d z=%g wiggle=%d exprs=%d "
                         "read is3DLayer=%d (expect 1, locked wording x4, 0,1,50,0,0, 1)\n",
                         locked, qPrintable(e1), qPrintable(e2), qPrintable(e3), qPrintable(e4),
                         lockedClip.autoOrientToCamera, lockedClip.is3DLayer, lockedClip.layer3D.positionZ,
                         lockedClip.wiggle.enabled, static_cast<int>(lockedClip.expressions.boundPaths().size()),
                         readBack.value(QStringLiteral("is3DLayer")).toBool(false));
            const QString wording = QStringLiteral("ロック中");
            tallyObserved(199, observeTerm(199, "V1 locked", locked)
                && observeTerm(199, "autoOrientToCamera refused", isErr(r1) && e1.contains(wording))
                && observeTerm(199, "is3DLayer refused", isErr(r2) && e2.contains(wording))
                && observeTerm(199, "set_clip_wiggle refused", isErr(r3) && e3.contains(wording))
                && observeTerm(199, "set_clip_expression refused", isErr(r4) && e4.contains(wording))
                && observeTerm(199, "clip untouched", !lockedClip.autoOrientToCamera && lockedClip.is3DLayer
                               && lockedClip.layer3D.positionZ == 50.0 && !lockedClip.wiggle.enabled
                               && lockedClip.expressions.boundPaths().isEmpty())
                && observeTerm(199, "get_clip_dynamics still reads", readBack.value(QStringLiteral("is3DLayer")).toBool(false)),
                "a dynamics write changed a locked track or its error did not say the track is locked");
            dynTimeline->applyTrackFlagsFromJson(QJsonObject{});
        }
    }

    server.stop();
    qInfo().noquote().nospace() << "[mcp] selftest end";
    return tally.finish("mcp");
}
