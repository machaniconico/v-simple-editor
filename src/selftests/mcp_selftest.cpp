#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QStringList>
#include <QDebug>

#include <limits>
#include <stdexcept>

#include "../mcp/McpEditorTools.h"
#include "../mcp/McpHttpServer.h"
#include "../mcp/McpProtocol.h"
#include "../mcp/McpStdioBridge.h"
#include "../mcp/McpToolRegistry.h"
#include "../AiChatDock.h"
#include "../MainWindow.h"

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
                const QByteArray& body, const QByteArray& token = {})
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    if (!token.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + token);

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
        && successResult.value(QStringLiteral("structuredContent")).toObject()
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
    }

    mcp::McpEditorTools nullWindowTools(nullptr, &registry);
    nullWindowTools.registerReadTools();

    const QJsonObject readToolsList = parseObject(protocol.handleMessage(compact(
        rpcRequest(9, QStringLiteral("tools/list")))));
    const QJsonArray readTools = readToolsList.value(QStringLiteral("result"))
        .toObject().value(QStringLiteral("tools")).toArray();
    const QStringList expectedReadToolNames{
        QStringLiteral("get_project_info"),
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
    const QJsonObject projectInfoResult = projectInfoResponse
        .value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("structuredContent")).toObject();
    const bool g30 = !projectInfoResult.contains(QStringLiteral("hasUnsavedChanges"))
        || (projectInfoResult.value(QStringLiteral("hasUnsavedChanges")).isBool()
            && projectInfoResult.value(QStringLiteral("hasUnsavedChanges")).toBool()
                == projectInfoWindow.isWindowModified());
    g30 ? pass("G30 project unsaved state is truthful")
        : fail("G30 project unsaved state is truthful",
               QStringLiteral("hasUnsavedChanges was missing a boolean/state contract"));

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
