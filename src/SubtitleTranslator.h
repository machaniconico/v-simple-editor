#pragma once
#include "CaptionTrack.h"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QNetworkAccessManager>

class QSettings;

namespace subxlat {

enum class Provider { Stub, GoogleV2, DeepL };

struct TranslateConfig {
    Provider provider  = Provider::Stub;
    QString  apiKey;
    QString  sourceLang = "auto";
    QString  targetLang = "ja";

    static TranslateConfig defaultConfig();
    static TranslateConfig defaultConfig(QSettings &settings,
                                         const QString &environmentKey);
};

Provider providerFromSettings(QSettings &settings,
                              Provider fallback = Provider::Stub);
void saveDialogSettings(QSettings &settings,
                        Provider provider,
                        const QString &apiKey,
                        bool saveApiKey);

class TranslatorClient : public QObject {
    Q_OBJECT
public:
    explicit TranslatorClient(QObject *parent = nullptr);

    void translateTrack(const caption::Track &track, const TranslateConfig &cfg);

signals:
    void translateProgress(int percent);
    void translateFinished(const caption::Track &translated);
    void translateFailed(const QString &error);

private:
    QPointer<QNetworkAccessManager> m_nam;

    void translateStub(const caption::Track &track, const TranslateConfig &cfg);
    void translateGoogleV2(const caption::Track &track, const TranslateConfig &cfg);
    void translateDeepL(const caption::Track &track, const TranslateConfig &cfg);
};

} // namespace subxlat
