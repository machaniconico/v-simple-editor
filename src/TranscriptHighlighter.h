#pragma once

#include "CaptionTrack.h"
#include "AIHighlight.h"

#include <QString>
#include <QVector>
#include <QList>
#include <functional>

namespace transcripthl {

struct HighlightRequest {
    QList<caption::Clip> transcript;
    int targetCount = 10;
    QString provider = "anthropic";
    QString model;
};

using SendFn = std::function<QString(const QString& prompt, QString* err)>;

class TranscriptHighlighter {
public:
    static QString buildPrompt(const QList<caption::Clip>& transcript, int targetCount);
    static QString extractJsonObject(const QString& raw, QString* err = nullptr);
    static QVector<Highlight> parseHighlights(const QString& json, QString* err = nullptr);
    static QVector<Highlight> detectOffline(const QList<caption::Clip>& transcript, int targetCount);
    QVector<Highlight> detect(const HighlightRequest& req, SendFn sender = nullptr, QString* err = nullptr);
};

} // namespace transcripthl
