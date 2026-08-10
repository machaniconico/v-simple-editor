#include "SubtitleTranslator.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QProcessEnvironment>

namespace subxlat {

namespace {

void replaceTranslatedText(caption::Clip *clip, const QString &translatedText)
{
    if (!clip || clip->text == translatedText)
        return;
    clip->text = translatedText;
    clip->words.clear();
}

} // namespace

// ---------------------------------------------------------------------------
// TranslateConfig
// ---------------------------------------------------------------------------

TranslateConfig TranslateConfig::defaultConfig()
{
    TranslateConfig cfg;
    // env var takes priority
    const QString envKey = QProcessEnvironment::systemEnvironment()
                               .value(QStringLiteral("VEDITOR_TRANSLATE_KEY"));
    if (!envKey.isEmpty()) {
        cfg.apiKey = envKey;
        return cfg;
    }
    // fall back to QSettings
    QSettings settings;
    settings.beginGroup(QStringLiteral("translate"));
    cfg.apiKey = settings.value(QStringLiteral("api_key")).toString();
    settings.endGroup();
    return cfg;
}

// ---------------------------------------------------------------------------
// TranslatorClient
// ---------------------------------------------------------------------------

TranslatorClient::TranslatorClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void TranslatorClient::translateTrack(const caption::Track &track,
                                      const TranslateConfig &cfg)
{
    switch (cfg.provider) {
    case Provider::Stub:
        translateStub(track, cfg);
        break;
    case Provider::GoogleV2:
        translateGoogleV2(track, cfg);
        break;
    case Provider::DeepL:
        translateDeepL(track, cfg);
        break;
    }
}

// ---------------------------------------------------------------------------
// Stub — offline, synchronous
// ---------------------------------------------------------------------------

void TranslatorClient::translateStub(const caption::Track &track,
                                     const TranslateConfig &cfg)
{
    caption::Track result;
    const int total = track.clipCount();
    for (int i = 0; i < total; ++i) {
        caption::Clip c = track.clipAt(i);
        replaceTranslatedText(
            &c, QStringLiteral("[") + cfg.targetLang + QStringLiteral("] ") + c.text);
        result.addClip(c);
        if (total > 0)
            emit translateProgress((i + 1) * 100 / total);
    }
    emit translateFinished(result);
}

// ---------------------------------------------------------------------------
// Google Translate V2
// ---------------------------------------------------------------------------

void TranslatorClient::translateGoogleV2(const caption::Track &track,
                                         const TranslateConfig &cfg)
{
    if (cfg.apiKey.isEmpty()) {
        emit translateFailed(QStringLiteral("Google Translate API key is empty"));
        return;
    }

    // Build one batch request with all clip texts
    const QList<caption::Clip> clips = track.clips();
    if (clips.isEmpty()) {
        emit translateFinished(track);
        return;
    }

    const QUrl u(QStringLiteral("https://translation.googleapis.com/language/translate/v2?key=")
                 + cfg.apiKey);
    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));

    QJsonArray qArr;
    for (const caption::Clip &c : clips)
        qArr.append(c.text);

    QJsonObject body;
    body[QStringLiteral("q")]      = qArr;
    body[QStringLiteral("target")] = cfg.targetLang;
    if (cfg.sourceLang != QLatin1String("auto"))
        body[QStringLiteral("source")] = cfg.sourceLang;

    QNetworkReply *reply = m_nam->post(req, QJsonDocument(body).toJson());

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, clips]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit translateFailed(reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray translations =
            doc[QStringLiteral("data")][QStringLiteral("translations")].toArray();

        if (translations.isEmpty()) {
            emit translateFailed(QStringLiteral("Google Translate returned empty translations"));
            return;
        }

        caption::Track result;
        const int total = clips.size();
        for (int i = 0; i < total; ++i) {
            caption::Clip c = clips.at(i);
            if (i < translations.size()) {
                replaceTranslatedText(
                    &c,
                    translations.at(i)
                        .toObject()[QStringLiteral("translatedText")]
                        .toString());
            }
            result.addClip(c);
            emit translateProgress((i + 1) * 100 / total);
        }
        emit translateFinished(result);
    });
}

// ---------------------------------------------------------------------------
// DeepL
// ---------------------------------------------------------------------------

void TranslatorClient::translateDeepL(const caption::Track &track,
                                      const TranslateConfig &cfg)
{
    if (cfg.apiKey.isEmpty()) {
        emit translateFailed(QStringLiteral("DeepL API key is empty"));
        return;
    }

    const QList<caption::Clip> clips = track.clips();
    if (clips.isEmpty()) {
        emit translateFinished(track);
        return;
    }

    const QUrl u(QStringLiteral("https://api-free.deepl.com/v2/translate"));
    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("auth_key"), cfg.apiKey);
    query.addQueryItem(QStringLiteral("target_lang"), cfg.targetLang.toUpper());
    if (cfg.sourceLang != QLatin1String("auto"))
        query.addQueryItem(QStringLiteral("source_lang"), cfg.sourceLang.toUpper());
    for (const caption::Clip &c : clips)
        query.addQueryItem(QStringLiteral("text"), c.text);

    QNetworkReply *reply = m_nam->post(req, query.toString(QUrl::FullyEncoded).toUtf8());

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, clips]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit translateFailed(reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray translations =
            doc[QStringLiteral("translations")].toArray();

        if (translations.isEmpty()) {
            emit translateFailed(QStringLiteral("DeepL returned empty translations"));
            return;
        }

        caption::Track result;
        const int total = clips.size();
        for (int i = 0; i < total; ++i) {
            caption::Clip c = clips.at(i);
            if (i < translations.size()) {
                replaceTranslatedText(
                    &c,
                    translations.at(i)
                        .toObject()[QStringLiteral("text")]
                        .toString());
            }
            result.addClip(c);
            emit translateProgress((i + 1) * 100 / total);
        }
        emit translateFinished(result);
    });
}

} // namespace subxlat
