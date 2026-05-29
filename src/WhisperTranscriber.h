#pragma once

#include "SpeechRecognizer.h"
#include "CaptionTrack.h"

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
    speech::RecognizeResult raw;
    bool success = false;
    QString error;
};

class WhisperTranscriber {
public:
    TranscribeOutcome transcribe(const TranscribeRequest& req);
    static caption::Track toCaptionTrack(const QList<speech::Segment>& segs);
};

} // namespace whisper
