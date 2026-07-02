#pragma once

#include "SpeechRecognizer.h"
#include "CaptionTrack.h"
#include "SubtitleGenerator.h"

#include <QByteArray>
#include <QString>

namespace whisper {

struct TranscribeRequest {
    QString mediaPath;
    QString language = "auto";
    QString recognizerName;
    qint64 maxDurationMs = 0;
};

struct TranscribeOutcome {
    caption::Track track;
    QVector<SubtitleSegment> subtitleSegments;
    speech::RecognizeResult raw;
    bool success = false;
    QString error;
};

class WhisperTranscriber {
public:
    TranscribeOutcome transcribe(const TranscribeRequest& req);
    static QList<speech::Segment> parseWhisperJsonSegments(const QByteArray& json,
                                                           QString* detectedLanguage = nullptr,
                                                           QString* error = nullptr);
    static caption::Track toCaptionTrack(const QList<speech::Segment>& segs);
    static QVector<SubtitleSegment> toSubtitleSegments(const QList<speech::Segment>& segs,
                                                       const QString& language = QString());
};

} // namespace whisper
