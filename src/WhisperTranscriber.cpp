#include "WhisperTranscriber.h"

#include "libavcore/AudioExtract.h"

#include <QTemporaryDir>

namespace whisper {

namespace {

QString modelIdForRecognizer(const speech::Recognizer& recognizer)
{
    if (recognizer.name() == QStringLiteral("Stub"))
        return QStringLiteral("stub");
    return QStringLiteral("base");
}

} // namespace

TranscribeOutcome WhisperTranscriber::transcribe(const TranscribeRequest& req)
{
    TranscribeOutcome outcome;

    const auto recognizer = speech::recognizerByName(req.recognizerName);
    if (!recognizer) {
        outcome.success = false;
        outcome.error = QStringLiteral("recognizer not found");
        return outcome;
    }

    const auto recognizeFrom = [&](const QString& audioPath) {
        speech::RecognizeParams params;
        params.audioPath = audioPath;
        params.language = req.language;
        params.modelId = modelIdForRecognizer(*recognizer);
        params.maxDurationMs = req.maxDurationMs;

        outcome.raw = recognizer->recognize(params);
        outcome.track = toCaptionTrack(outcome.raw.segments);
        outcome.success = outcome.raw.success;
        outcome.error = outcome.raw.error;
    };

    if (recognizer->name() == QStringLiteral("Stub")) {
        recognizeFrom(req.mediaPath);
        return outcome;
    }

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        outcome.success = false;
        outcome.error = QStringLiteral("failed to create temporary directory");
        return outcome;
    }

    const QString wavPath = tempDir.filePath(QStringLiteral("transcribe.wav"));
    QString extractError;
    if (!libavcore::extractAudioToWav(req.mediaPath, wavPath, 16000, &extractError)) {
        outcome.success = false;
        outcome.error = extractError.isEmpty()
            ? QStringLiteral("failed to extract audio")
            : extractError;
        return outcome;
    }

    recognizeFrom(wavPath);
    return outcome;
}

caption::Track WhisperTranscriber::toCaptionTrack(const QList<speech::Segment>& segs)
{
    caption::Track track;
    for (const speech::Segment& seg : segs) {
        if (seg.text.isEmpty())
            continue;

        caption::Clip clip;
        clip.startMs = seg.startMs;
        clip.endMs = seg.endMs;
        clip.text = seg.text;
        clip.actor = QString();
        track.addClip(clip);
    }
    track.sortByStart();
    return track;
}

} // namespace whisper
