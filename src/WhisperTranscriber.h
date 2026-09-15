#pragma once

#include "SpeechRecognizer.h"
#include "CaptionTrack.h"
#include "SubtitleGenerator.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

class QSettings;

namespace whisperpath {

struct Resolution {
    QString executablePath;
    QStringList candidatePaths;
};

// Deterministic resolution seam used by selftests. Values supplied by callers
// are already considered usable; existingKnownLocations controls which fixed
// candidates are eligible.
Resolution resolveWhisperCli(QSettings& settings,
                             const QString& environmentPath,
                             const QString& pathExecutable,
                             const QStringList& knownLocations,
                             const QStringList& existingKnownLocations);

// Runtime resolver: settings -> environment -> PATH -> known locations.
Resolution resolveWhisperCli();
QStringList whisperCliCandidatePaths();

} // namespace whisperpath

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
