#include "McpStdioBridge.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr int kDefaultRequestTimeoutMs = 120000;

QJsonValue nullJsonValue()
{
    return QJsonValue(QJsonValue::Null);
}

void setBinaryMode()
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

void writeStdoutLine(const QByteArray& line)
{
    std::cout.write(line.constData(), static_cast<std::streamsize>(line.size()));
    std::cout.put('\n');
    std::cout.flush();
}

class BridgeSession : public QObject
{
public:
    BridgeSession(const QUrl& endpoint, const QByteArray& token, int timeoutMs)
        : m_endpoint(endpoint)
        , m_token(token)
        , m_timeoutMs(timeoutMs)
    {
    }

    void handleLine(QByteArray line)
    {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty())
            return;

        bool hadRequestId = false;
        const QJsonValue requestId = McpStdioBridge::extractRequestId(
            line, &hadRequestId);

        QNetworkRequest request(m_endpoint);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
        request.setRawHeader("Authorization", "Bearer " + m_token);
        QNetworkReply* reply = m_manager.post(request, line);
        ++m_inFlight;

        QTimer* timeout = new QTimer(reply);
        timeout->setSingleShot(true);
        QObject::connect(timeout, &QTimer::timeout, reply, [reply]() {
            reply->setProperty("timedOut", true);
            if (reply->isRunning())
                reply->abort();
        });
        QObject::connect(reply, &QNetworkReply::finished, this,
                         [this, reply, requestId, hadRequestId]() {
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            bool handled = false;

            if (hadRequestId) {
                if (status == 202) {
                    handled = true;
                } else if (status == 200) {
                    const QByteArray compactResponse =
                        McpStdioBridge::compactJson(body);
                    if (!compactResponse.isEmpty()) {
                        writeStdoutLine(compactResponse);
                        handled = true;
                    }
                }

                if (!handled) {
                    if (status == 401) {
                        writeStdoutLine(McpStdioBridge::makeTransportError(
                            requestId, QStringLiteral("invalid token")));
                    } else if (reply->property("timedOut").toBool()) {
                        // abort() changes QNetworkReply::error(), so the
                        // explicit timeout marker must be checked first.
                        writeStdoutLine(McpStdioBridge::makeTransportError(
                            requestId,
                            QStringLiteral("editor did not respond in time")));
                    } else if (reply->error()
                               == QNetworkReply::ConnectionRefusedError) {
                        writeStdoutLine(McpStdioBridge::makeTransportError(
                            requestId, QStringLiteral("editor not running")));
                    } else {
                        // Unexpected HTTP/network errors are reported without
                        // terminating the bridge, but are kept distinct from
                        // connection refusal.
                        writeStdoutLine(McpStdioBridge::makeTransportError(
                            requestId,
                            QStringLiteral("unexpected editor response")));
                    }
                }
            }

            --m_inFlight;
            reply->deleteLater();
            maybeQuit();
        });
        timeout->start(m_timeoutMs);
    }

    void stdinClosed()
    {
        m_stdinClosed = true;
        maybeQuit();
    }

private:
    void maybeQuit()
    {
        if (m_stdinClosed && m_inFlight == 0)
            QCoreApplication::quit();
    }

    QNetworkAccessManager m_manager;
    QUrl m_endpoint;
    QByteArray m_token;
    int m_timeoutMs = 0;
    int m_inFlight = 0;
    bool m_stdinClosed = false;
};

} // namespace

QByteArray McpStdioBridge::compactJson(const QByteArray& json)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || document.isNull())
        return QByteArray();
    return document.toJson(QJsonDocument::Compact);
}

QJsonValue McpStdioBridge::extractRequestId(const QByteArray& line, bool *hadId)
{
    if (hadId)
        *hadId = false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return nullJsonValue();

    const QJsonObject request = document.object();
    if (!request.contains(QStringLiteral("id")))
        return nullJsonValue();
    if (hadId)
        *hadId = true;
    return request.value(QStringLiteral("id"));
}

int McpStdioBridge::requestTimeoutMs()
{
    bool ok = false;
    const int configured = qEnvironmentVariable(
        "VEDITOR_MCP_TIMEOUT_MS").toInt(&ok);
    return ok && configured >= 1000 ? configured : kDefaultRequestTimeoutMs;
}

QByteArray McpStdioBridge::makeTransportError(const QJsonValue& id,
                                               const QString& message)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), -32603);
    error.insert(QStringLiteral("message"), message);

    QJsonObject response;
    response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    response.insert(QStringLiteral("id"), id.isUndefined() ? nullJsonValue() : id);
    response.insert(QStringLiteral("error"), error);
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

int McpStdioBridge::run(quint16 port, const QString& token)
{
    int applicationArgc = 1;
    char applicationName[] = "v-simple-editor";
    char* applicationArgv[] = {applicationName, nullptr};
    QCoreApplication application(applicationArgc, applicationArgv);

    setBinaryMode();

    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/mcp").arg(port));
    const QString environmentToken = qEnvironmentVariable("VEDITOR_MCP_TOKEN");
    const QByteArray tokenBytes = (environmentToken.isEmpty()
                                   ? token : environmentToken).toUtf8();
    const int timeoutMs = requestTimeoutMs();

    BridgeSession session(endpoint, tokenBytes, timeoutMs);
    std::thread reader([&session]() {
        std::string inputLine;
        while (std::getline(std::cin, inputLine)) {
            const QByteArray line = QByteArray::fromStdString(inputLine);
            QMetaObject::invokeMethod(
                &session,
                [&session, line]() { session.handleLine(line); },
                Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(
            &session, [&session]() { session.stdinClosed(); },
            Qt::QueuedConnection);
    });

    application.exec();
    reader.join();

    return 0;
}

std::optional<int> dispatchMcpStdioPreQApplication(int argc, char* argv[])
{
    bool enabled = false;
    bool hasPort = false;
    bool hasToken = false;
    quint16 port = 0;
    QString token;
    bool cliTokenWasUsed = false;

    auto usageError = []() -> std::optional<int> {
        std::fprintf(stderr,
                     "usage: v-simple-editor --mcp-stdio --port <N> [--token <T>]\n");
        return 2;
    };

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--mcp-stdio")) {
            enabled = true;
            continue;
        }

        QString value;
        if (arg == QStringLiteral("--port") || arg == QStringLiteral("--token")) {
            if (i + 1 >= argc)
                return enabled ? usageError() : std::nullopt;
            value = QString::fromLocal8Bit(argv[++i]);
            if (value.startsWith(QLatin1Char('-')))
                return enabled ? usageError() : std::nullopt;
            if (arg == QStringLiteral("--port")) {
                bool ok = false;
                const uint parsed = value.toUInt(&ok);
                if (!ok || parsed == 0 || parsed > 65535)
                    return enabled ? usageError() : std::nullopt;
                port = static_cast<quint16>(parsed);
                hasPort = true;
            } else {
                token = value;
                hasToken = !token.isEmpty();
                cliTokenWasUsed = true;
            }
            continue;
        }

        if (arg.startsWith(QStringLiteral("--port="))) {
            value = arg.mid(QStringLiteral("--port=").size());
            bool ok = false;
            const uint parsed = value.toUInt(&ok);
            if (!ok || parsed == 0 || parsed > 65535)
                return enabled ? usageError() : std::nullopt;
            port = static_cast<quint16>(parsed);
            hasPort = true;
            continue;
        }

        if (arg.startsWith(QStringLiteral("--token="))) {
            token = arg.mid(QStringLiteral("--token=").size());
            hasToken = !token.isEmpty();
            cliTokenWasUsed = true;
            continue;
        }
    }

    if (!enabled)
        return std::nullopt;
    const QString environmentToken = qEnvironmentVariable("VEDITOR_MCP_TOKEN");
    if (!environmentToken.isEmpty()) {
        token = environmentToken;
        hasToken = true;
    }
    if (!hasPort || !hasToken)
        return usageError();

    if (cliTokenWasUsed)
        std::fprintf(stderr,
                     "--token はプロセス一覧から見えます。VEDITOR_MCP_TOKEN の使用を推奨します。\n");

    return McpStdioBridge::run(port, token);
}
