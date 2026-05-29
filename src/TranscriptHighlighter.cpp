#include "TranscriptHighlighter.h"

#include "CredentialStore.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QUrl>

#include <exception>

namespace transcripthl {

namespace {

QString formatTimestamp(qint64 ms)
{
    const qint64 safeMs = qMax<qint64>(0, ms);
    const qint64 totalSeconds = safeMs / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    const qint64 millis = safeMs % 1000;
    return QStringLiteral("%1:%2.%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(millis, 3, 10, QLatin1Char('0'));
}

QString findBalancedJsonObject(const QString& text)
{
    const int start = text.indexOf(QLatin1Char('{'));
    if (start < 0)
        return QString();

    bool inString = false;
    bool escaped = false;
    int depth = 0;

    for (int i = start; i < text.size(); ++i) {
        const QChar ch = text.at(i);

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }

        if (ch == QLatin1Char('"')) {
            inString = true;
        } else if (ch == QLatin1Char('{')) {
            ++depth;
        } else if (ch == QLatin1Char('}')) {
            --depth;
            if (depth == 0)
                return text.mid(start, i - start + 1);
        }
    }

    return QString();
}

QString anthropicResponseText(const QByteArray& payload)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return QString::fromUtf8(payload);

    const QJsonObject root = doc.object();
    QStringList parts;
    const QJsonArray content = root.value(QStringLiteral("content")).toArray();
    for (const QJsonValue& itemValue : content) {
        const QJsonObject item = itemValue.toObject();
        const QString text = item.value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            parts.append(text);
    }
    if (!parts.isEmpty())
        return parts.join(QLatin1Char('\n'));

    const QString text = root.value(QStringLiteral("text")).toString();
    if (!text.isEmpty())
        return text;

    return QString::fromUtf8(payload);
}

QString sendAnthropicPrompt(const HighlightRequest& req, const QString& prompt, QString* err)
{
    const QString apiKey = creds::CredentialStore::get(
        "ANTHROPIC_API_KEY", QStringLiteral("apiKeys/anthropic"));
    if (apiKey.trimmed().isEmpty()) {
        if (err)
            *err = QStringLiteral("no api key");
        return QString();
    }

    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
    request.setRawHeader("x-api-key", apiKey.trimmed().toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QString model = req.model.trimmed().isEmpty()
        ? QStringLiteral("claude-3-5-sonnet-20241022")
        : req.model.trimmed();

    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("max_tokens"), 1000);

    QJsonArray messages;
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), prompt);
    messages.append(message);
    body.insert(QStringLiteral("messages"), messages);

    QEventLoop loop;
    QNetworkReply* reply = network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QByteArray response = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString errorString = reply->errorString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError || status >= 400) {
        if (err) {
            *err = status > 0
                ? QStringLiteral("network error %1: %2").arg(status).arg(errorString)
                : QStringLiteral("network error: %1").arg(errorString);
        }
        return QString();
    }

    return anthropicResponseText(response);
}

} // namespace

QString TranscriptHighlighter::buildPrompt(const QList<caption::Clip>& transcript, int targetCount)
{
    QString prompt;
    prompt += QStringLiteral("You are selecting transcript-based video highlights.\n");
    prompt += QStringLiteral("Choose the most engaging %1 moments from the transcript.\n").arg(qMax(0, targetCount));
    prompt += QStringLiteral("Return only this JSON and nothing else: {\"highlights\":[{\"start\":<sec>,\"end\":<sec>,\"score\":<0-1>,\"reason\":\"...\"}]}\n\n");
    prompt += QStringLiteral("Transcript:\n");

    for (const caption::Clip& clip : transcript) {
        prompt += QStringLiteral("[%1 -> %2] %3\n")
            .arg(formatTimestamp(clip.startMs),
                 formatTimestamp(clip.endMs),
                 clip.text);
    }

    return prompt;
}

QString TranscriptHighlighter::extractJsonObject(const QString& raw, QString* err)
{
    if (err)
        err->clear();

    const QString fenceMarker = QStringLiteral("```");
    int fenceStart = raw.indexOf(fenceMarker);
    while (fenceStart >= 0) {
        int contentStart = fenceStart + fenceMarker.size();
        const int lineEnd = raw.indexOf(QLatin1Char('\n'), contentStart);
        if (lineEnd >= 0) {
            const QString language = raw.mid(contentStart, lineEnd - contentStart).trimmed();
            if (language.isEmpty() || language.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0)
                contentStart = lineEnd + 1;
        }

        const int fenceEnd = raw.indexOf(fenceMarker, contentStart);
        if (fenceEnd < 0)
            break;

        const QString fenced = raw.mid(contentStart, fenceEnd - contentStart).trimmed();
        const QString object = findBalancedJsonObject(fenced);
        if (!object.isEmpty())
            return object;

        fenceStart = raw.indexOf(fenceMarker, fenceEnd + fenceMarker.size());
    }

    const QString object = findBalancedJsonObject(raw);
    if (!object.isEmpty())
        return object;

    if (err)
        *err = QStringLiteral("no json object found");
    return QString();
}

QVector<Highlight> TranscriptHighlighter::parseHighlights(const QString& json, QString* err)
{
    if (err)
        err->clear();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err)
            *err = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("json root is not an object");
        return QVector<Highlight>();
    }

    const QJsonArray array = doc.object().value(QStringLiteral("highlights")).toArray();
    QVector<Highlight> highlights;
    highlights.reserve(array.size());

    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        Highlight highlight;
        highlight.startTime = object.value(QStringLiteral("start")).toDouble();
        highlight.endTime = object.value(QStringLiteral("end")).toDouble();
        highlight.score = qBound(0.0, object.value(QStringLiteral("score")).toDouble(), 1.0);
        highlight.type = HighlightType::SpeechSegment;
        highlight.description = object.value(QStringLiteral("reason")).toString();
        highlights.append(highlight);
    }

    return highlights;
}

QVector<Highlight> TranscriptHighlighter::detect(const HighlightRequest& req, SendFn sender, QString* err)
{
    if (err)
        err->clear();

    const int targetCount = qMax(0, req.targetCount);
    if (targetCount == 0)
        return QVector<Highlight>();

    const QString prompt = buildPrompt(req.transcript, targetCount);

    QString sendError;
    QString raw;
    try {
        if (sender) {
            raw = sender(prompt, &sendError);
        } else {
            if (!req.provider.trimmed().isEmpty()
                && req.provider.compare(QStringLiteral("anthropic"), Qt::CaseInsensitive) != 0) {
                if (err)
                    *err = QStringLiteral("unsupported provider");
                return QVector<Highlight>();
            }
            raw = sendAnthropicPrompt(req, prompt, &sendError);
        }
    } catch (const std::exception& ex) {
        if (err)
            *err = QStringLiteral("highlight transport exception: %1").arg(QString::fromUtf8(ex.what()));
        return QVector<Highlight>();
    } catch (...) {
        if (err)
            *err = QStringLiteral("highlight transport exception");
        return QVector<Highlight>();
    }

    if (!sendError.isEmpty()) {
        if (err)
            *err = sendError;
        return QVector<Highlight>();
    }

    QString extractError;
    const QString json = extractJsonObject(raw, &extractError);
    if (json.isEmpty()) {
        if (err)
            *err = extractError.isEmpty() ? QStringLiteral("empty llm response") : extractError;
        return QVector<Highlight>();
    }

    QString parseError;
    QVector<Highlight> highlights = parseHighlights(json, &parseError);
    if (!parseError.isEmpty()) {
        if (err)
            *err = parseError;
        return QVector<Highlight>();
    }

    qint64 minStartMs = 0;
    qint64 maxEndMs = 0;
    bool haveRange = false;
    for (const caption::Clip& clip : req.transcript) {
        if (!haveRange) {
            minStartMs = clip.startMs;
            maxEndMs = clip.endMs;
            haveRange = true;
        } else {
            minStartMs = qMin(minStartMs, clip.startMs);
            maxEndMs = qMax(maxEndMs, clip.endMs);
        }
    }

    if (haveRange) {
        const double minStart = minStartMs / 1000.0;
        const double maxEnd = maxEndMs / 1000.0;
        for (Highlight& highlight : highlights) {
            highlight.startTime = qBound(minStart, highlight.startTime, maxEnd);
            highlight.endTime = qBound(minStart, highlight.endTime, maxEnd);
            if (highlight.endTime < highlight.startTime)
                highlight.endTime = highlight.startTime;
        }
    }

    if (highlights.size() > targetCount)
        highlights.resize(targetCount);

    return highlights;
}

} // namespace transcripthl
