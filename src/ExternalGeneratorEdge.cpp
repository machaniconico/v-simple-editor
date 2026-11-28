#include "ExternalGeneratorEdge.h"
#include "ExternalGenerator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>

#include <cstring>

namespace extgen {
namespace edge {

namespace {

quint16 pickFreeLocalPort()
{
    QTcpServer srv;
    if (srv.listen(QHostAddress::LocalHost, 0)) {
        const quint16 port = srv.serverPort();
        srv.close();
        return port;
    }
    return 0;
}

bool httpGetLocal(quint16 port, const QString &path, QByteArray *bodyOut, int timeoutMs)
{
    QTcpSocket sock;
    sock.connectToHost(QHostAddress::LocalHost, port);
    if (!sock.waitForConnected(qMin(timeoutMs, 1500)))
        return false;
    const QByteArray req = QStringLiteral(
        "GET %1 HTTP/1.1\r\nHost: 127.0.0.1:%2\r\nConnection: close\r\n\r\n")
        .arg(path).arg(port).toUtf8();
    sock.write(req);
    sock.flush();
    QByteArray raw;
    QElapsedTimer timer;
    timer.start();
    while (sock.state() != QAbstractSocket::UnconnectedState && timer.elapsed() < timeoutMs) {
        if (sock.waitForReadyRead(200))
            raw.append(sock.readAll());
    }
    raw.append(sock.readAll());
    const int sep = raw.indexOf("\r\n\r\n");
    if (sep < 0)
        return false;
    if (bodyOut)
        *bodyOut = raw.mid(sep + 4);
    return true;
}

bool cdpSendAndWait(QTcpSocket &ws,
                    QByteArray &rxBuf,
                    int id,
                    const QString &method,
                    const QJsonObject &params,
                    QJsonObject *resultOut,
                    int timeoutMs)
{
    QJsonObject msg;
    msg[QStringLiteral("id")] = id;
    msg[QStringLiteral("method")] = method;
    if (!params.isEmpty())
        msg[QStringLiteral("params")] = params;
    const QByteArray payload = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    const QByteArray frame = buildClientTextFrame(payload, static_cast<quint32>(0x5647454Eu ^ id));
    if (ws.write(frame) != frame.size())
        return false;
    ws.flush();

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        int consumed = 0;
        int opcode = 0;
        QByteArray rxPayload;
        while (parseServerFrame(rxBuf, &rxPayload, &opcode, &consumed)) {
            rxBuf.remove(0, consumed);
            if (opcode == 0x8) // Close
                return false;
            if (opcode == 0x9) { // Ping -> Pong
                QByteArray pong;
                pong.append(static_cast<char>(0x8A));
                pong.append(static_cast<char>(0x80));
                pong.append(4, '\0');
                ws.write(pong);
                ws.flush();
                continue;
            }
            if (opcode != 0x1 && opcode != 0x0)
                continue;
            const QJsonDocument doc = QJsonDocument::fromJson(rxPayload);
            if (!doc.isObject())
                continue;
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("id")).toInt(-1) == id) {
                if (obj.contains(QStringLiteral("error")))
                    return false;
                if (resultOut)
                    *resultOut = obj.value(QStringLiteral("result")).toObject();
                return true;
            }
        }
        if (!ws.waitForReadyRead(100)) {
            if (ws.state() != QAbstractSocket::ConnectedState)
                return false;
        }
        rxBuf.append(ws.readAll());
    }
    return false;
}

} // namespace

QString findBrowserExecutable()
{
    const QString envPath = qEnvironmentVariable("VEDITOR_GENERATOR_BROWSER").trimmed();
    if (!envPath.isEmpty()) {
        if (QFileInfo::exists(envPath))
            return QFileInfo(envPath).absoluteFilePath();
        return {};
    }

    QStringList candidates;
#ifdef Q_OS_WIN
    const QString pf86 = qEnvironmentVariable("ProgramFiles(x86)");
    const QString pf = qEnvironmentVariable("ProgramFiles");
    const QString localApp = qEnvironmentVariable("LOCALAPPDATA");
    if (!pf86.isEmpty())
        candidates << (pf86 + QStringLiteral("/Microsoft/Edge/Application/msedge.exe"));
    if (!pf.isEmpty())
        candidates << (pf + QStringLiteral("/Microsoft/Edge/Application/msedge.exe"));
    if (!localApp.isEmpty())
        candidates << (localApp + QStringLiteral("/Microsoft/Edge/Application/msedge.exe"));
    candidates << QStringLiteral("C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe")
               << QStringLiteral("C:/Program Files/Microsoft/Edge/Application/msedge.exe");
#endif
    for (const QString &cand : candidates) {
        if (QFileInfo::exists(cand))
            return QFileInfo(cand).absoluteFilePath();
    }
    for (const char *bin : {"msedge", "microsoft-edge", "google-chrome", "chromium"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(bin));
        if (!found.isEmpty())
            return found;
    }
    return {};
}

QByteArray buildClientTextFrame(const QByteArray &payload, quint32 maskKey)
{
    QByteArray out;
    out.reserve(payload.size() + 14);
    out.append(static_cast<char>(0x81)); // FIN=1, opcode=1 (text)
    const quint64 len = static_cast<quint64>(payload.size());
    if (len < 126) {
        out.append(static_cast<char>(0x80u | len));
    } else if (len <= 0xFFFFu) {
        out.append(static_cast<char>(0x80u | 126u));
        out.append(static_cast<char>((len >> 8) & 0xFFu));
        out.append(static_cast<char>(len & 0xFFu));
    } else {
        out.append(static_cast<char>(0x80u | 127u));
        for (int shift = 56; shift >= 0; shift -= 8)
            out.append(static_cast<char>((len >> shift) & 0xFFu));
    }
    const unsigned char mask[4] = {
        static_cast<unsigned char>((maskKey >> 24) & 0xFFu),
        static_cast<unsigned char>((maskKey >> 16) & 0xFFu),
        static_cast<unsigned char>((maskKey >> 8) & 0xFFu),
        static_cast<unsigned char>(maskKey & 0xFFu)
    };
    out.append(reinterpret_cast<const char *>(mask), 4);
    for (int i = 0; i < payload.size(); ++i)
        out.append(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i & 3]));
    return out;
}

bool parseServerFrame(const QByteArray &buffer,
                      QByteArray *payloadOut,
                      int *opcodeOut,
                      int *consumedBytesOut)
{
    if (buffer.size() < 2)
        return false;
    const unsigned char b0 = static_cast<unsigned char>(buffer[0]);
    const unsigned char b1 = static_cast<unsigned char>(buffer[1]);
    const int opcode = b0 & 0x0Fu;
    const bool masked = (b1 & 0x80u) != 0;
    quint64 len = b1 & 0x7Fu;
    int offset = 2;
    if (len == 126) {
        if (buffer.size() < offset + 2)
            return false;
        len = (static_cast<quint64>(static_cast<unsigned char>(buffer[offset])) << 8)
            | static_cast<quint64>(static_cast<unsigned char>(buffer[offset + 1]));
        offset += 2;
    } else if (len == 127) {
        if (buffer.size() < offset + 8)
            return false;
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | static_cast<quint64>(static_cast<unsigned char>(buffer[offset + i]));
        offset += 8;
    }
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (buffer.size() < offset + 4)
            return false;
        std::memcpy(mask, buffer.constData() + offset, 4);
        offset += 4;
    }
    if (static_cast<quint64>(buffer.size() - offset) < len)
        return false;

    QByteArray payload = buffer.mid(offset, static_cast<int>(len));
    if (masked) {
        for (int i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i & 3]);
    }
    if (payloadOut)
        *payloadOut = payload;
    if (opcodeOut)
        *opcodeOut = opcode;
    if (consumedBytesOut)
        *consumedBytesOut = offset + static_cast<int>(len);
    return true;
}

QString buildSetFrameExpression(int frameIndex, double fps, const QJsonObject &params)
{
    const QString paramsJson = QString::fromUtf8(
        QJsonDocument(params).toJson(QJsonDocument::Compact));
    return QStringLiteral(
        "(async () => {"
        "  const f = %1, fps = %2, params = %3;"
        "  if (typeof window.setFrame === 'function') { await window.setFrame(f, fps, params); }"
        "  await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));"
        "  return true;"
        "})()")
        .arg(frameIndex)
        .arg(QString::number(fps > 0.0 ? fps : 30.0, 'f', 4))
        .arg(paramsJson);
}

RenderOutcome renderFrameViaEdgeCdp(const GeneratorSpec &spec,
                                    int frameIndex,
                                    QSize size,
                                    int timeoutMs)
{
    RenderOutcome out;
    const QString browser = findBrowserExecutable();
    if (browser.isEmpty()) {
        out.missingRenderer = true;
        out.statusMessage = QStringLiteral("Edge/Chromium ブラウザが見つかりません");
        return out;
    }
    if (spec.entryPath.isEmpty() || !QFileInfo::exists(spec.entryPath)) {
        out.statusMessage = QStringLiteral("HTML エントリファイルが見つかりません: %1").arg(spec.entryPath);
        return out;
    }
    const quint16 port = pickFreeLocalPort();
    if (port == 0) {
        out.statusMessage = QStringLiteral("CDP ローカルポートの確保に失敗しました");
        return out;
    }

    QTemporaryDir profileDir;
    if (!profileDir.isValid()) {
        out.statusMessage = QStringLiteral("一時プロファイル作成に失敗しました");
        return out;
    }

    const QUrl fileUrl = QUrl::fromLocalFile(QFileInfo(spec.entryPath).absoluteFilePath());
    QStringList args;
    args << QStringLiteral("--headless=new")
         << QStringLiteral("--disable-gpu")
         << QStringLiteral("--no-first-run")
         << QStringLiteral("--no-default-browser-check")
         << QStringLiteral("--hide-scrollbars")
         << QStringLiteral("--remote-debugging-port=%1").arg(port)
         << QStringLiteral("--window-size=%1,%2").arg(qMax(16, size.width())).arg(qMax(16, size.height()))
         << QStringLiteral("--user-data-dir=%1").arg(profileDir.path())
         << fileUrl.toString();

    QProcess proc;
    proc.start(browser, args);
    if (!proc.waitForStarted(3000)) {
        out.missingRenderer = true;
        out.statusMessage = QStringLiteral("Edge headless の起動に失敗しました");
        return out;
    }

    auto cleanup = [&]() {
        if (proc.state() != QProcess::NotRunning) {
            proc.terminate();
            if (!proc.waitForFinished(1000)) {
                proc.kill();
                proc.waitForFinished(1000);
            }
        }
    };

    // Wait for DevTools /json endpoint
    QElapsedTimer timer;
    timer.start();
    QString wsPath;
    while (timer.elapsed() < timeoutMs) {
        QByteArray jsonBody;
        if (httpGetLocal(port, QStringLiteral("/json"), &jsonBody, 1000)) {
            const QJsonDocument doc = QJsonDocument::fromJson(jsonBody);
            if (doc.isArray()) {
                for (const QJsonValue &v : doc.array()) {
                    const QJsonObject o = v.toObject();
                    if (o.value(QStringLiteral("type")).toString() == QStringLiteral("page")) {
                        const QUrl wsUrl(o.value(QStringLiteral("webSocketDebuggerUrl")).toString());
                        if (wsUrl.isValid() && !wsUrl.path().isEmpty()) {
                            wsPath = wsUrl.path();
                            break;
                        }
                    }
                }
            }
        }
        if (!wsPath.isEmpty())
            break;
        proc.waitForFinished(100);
    }

    if (wsPath.isEmpty()) {
        cleanup();
        out.statusMessage = QStringLiteral("Edge CDP ターゲットへの接続がタイムアウトしました");
        return out;
    }

    // Connect WebSocket directly via QTcpSocket (zero external dependency)
    QTcpSocket ws;
    ws.connectToHost(QHostAddress::LocalHost, port);
    if (!ws.waitForConnected(2000)) {
        cleanup();
        out.statusMessage = QStringLiteral("Edge CDP WebSocket 接続失敗");
        return out;
    }
    const QByteArray wsKey = QByteArrayLiteral("dGhlIHNhbXBsZSBub25jZQ==");
    const QByteArray upgrade = QStringLiteral(
        "GET %1 HTTP/1.1\r\n"
        "Host: 127.0.0.1:%2\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %3\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n")
        .arg(wsPath).arg(port).arg(QString::fromLatin1(wsKey)).toUtf8();
    ws.write(upgrade);
    ws.flush();

    QByteArray rxBuf;
    int hdrEnd = -1;
    while (timer.elapsed() < timeoutMs) {
        if (ws.waitForReadyRead(150)) {
            rxBuf.append(ws.readAll());
            hdrEnd = rxBuf.indexOf("\r\n\r\n");
            if (hdrEnd >= 0)
                break;
        }
    }
    if (hdrEnd < 0 || !rxBuf.left(hdrEnd).contains("101")) {
        cleanup();
        out.statusMessage = QStringLiteral("Edge CDP WebSocket ハンドシェイク失敗");
        return out;
    }
    rxBuf.remove(0, hdrEnd + 4);

    // 1. Set exact viewport size
    QJsonObject metrics;
    metrics[QStringLiteral("width")] = qMax(16, size.width());
    metrics[QStringLiteral("height")] = qMax(16, size.height());
    metrics[QStringLiteral("deviceScaleFactor")] = 1;
    metrics[QStringLiteral("mobile")] = false;
    cdpSendAndWait(ws, rxBuf, 1, QStringLiteral("Emulation.setDeviceMetricsOverride"),
                   metrics, nullptr, qMax(500, timeoutMs - int(timer.elapsed())));

    // 2. Allow transparent background for alpha compositing
    QJsonObject clearBg;
    QJsonObject rgba;
    rgba[QStringLiteral("r")] = 0;
    rgba[QStringLiteral("g")] = 0;
    rgba[QStringLiteral("b")] = 0;
    rgba[QStringLiteral("a")] = 0;
    clearBg[QStringLiteral("color")] = rgba;
    cdpSendAndWait(ws, rxBuf, 2, QStringLiteral("Emulation.setDefaultBackgroundColorOverride"),
                   clearBg, nullptr, qMax(500, timeoutMs - int(timer.elapsed())));

    // 3. Evaluate window.setFrame(frameIndex, fps, params)
    QJsonObject evalParams;
    evalParams[QStringLiteral("expression")] = buildSetFrameExpression(frameIndex, spec.fps, spec.params);
    evalParams[QStringLiteral("awaitPromise")] = true;
    cdpSendAndWait(ws, rxBuf, 3, QStringLiteral("Runtime.evaluate"),
                   evalParams, nullptr, qMax(500, timeoutMs - int(timer.elapsed())));

    // 4. Capture PNG screenshot
    QJsonObject shotParams;
    shotParams[QStringLiteral("format")] = QStringLiteral("png");
    shotParams[QStringLiteral("fromSurface")] = true;
    QJsonObject shotResult;
    const bool shotOk = cdpSendAndWait(ws, rxBuf, 4, QStringLiteral("Page.captureScreenshot"),
                                       shotParams, &shotResult,
                                       qMax(500, timeoutMs - int(timer.elapsed())));
    cleanup();

    if (!shotOk) {
        out.statusMessage = QStringLiteral("Edge CDP スクリーンショット取得に失敗しました");
        return out;
    }
    const QByteArray pngBytes = QByteArray::fromBase64(
        shotResult.value(QStringLiteral("data")).toString().toLatin1());
    QImage img;
    if (!img.loadFromData(pngBytes, "PNG")) {
        out.statusMessage = QStringLiteral("Edge CDP PNG デコードに失敗しました");
        return out;
    }
    if (img.size() != size)
        img = img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    out.image = img.convertToFormat(QImage::Format_RGBA8888);
    out.ok = !out.image.isNull();
    return out;
}

} // namespace edge
} // namespace extgen
