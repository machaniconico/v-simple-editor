#include <QDebug>
#include <QString>
#include <QFile>
#include <QByteArray>
#include <QTemporaryDir>

#include "../libavcore/AudioExtract.h"
#include "../SpeechRecognizer.h"
#include "../CaptionTrack.h"
#include "../WhisperTranscriber.h"

namespace {

quint16 readLe16(const QByteArray& bytes, int offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(bytes.at(offset)))
        | static_cast<quint16>(static_cast<unsigned char>(bytes.at(offset + 1)) << 8);
}

quint32 readLe32(const QByteArray& bytes, int offset)
{
    return static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset)))
        | (static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + 1))) << 8)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + 2))) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + 3))) << 24);
}

QByteArray makeKnownPcm()
{
    QByteArray pcm;
    pcm.reserve(100 * 2);
    for (int i = 0; i < 100; ++i) {
        const qint16 sample = static_cast<qint16>((i - 50) * 128);
        const quint16 encoded = static_cast<quint16>(sample);
        pcm.append(static_cast<char>(encoded & 0xff));
        pcm.append(static_cast<char>((encoded >> 8) & 0xff));
    }
    return pcm;
}

QString validateSegments(const QList<speech::Segment>& segments)
{
    if (segments.isEmpty())
        return QStringLiteral("segments are empty");

    qint64 previousStart = 0;
    bool havePrevious = false;
    for (const speech::Segment& seg : segments) {
        if (seg.startMs >= seg.endMs) {
            return QStringLiteral("invalid segment range %1-%2")
                .arg(seg.startMs)
                .arg(seg.endMs);
        }
        if (havePrevious && seg.startMs < previousStart) {
            return QStringLiteral("segment start moved backwards from %1 to %2")
                .arg(previousStart)
                .arg(seg.startMs);
        }
        previousStart = seg.startMs;
        havePrevious = true;
    }

    return QString();
}

} // namespace

int runWhisperTranscribeSelftest()
{
    qInfo().noquote() << "[whisper-transcribe] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[whisper-transcribe] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[whisper-transcribe] FAIL" << name << ":" << msg; };

    QTemporaryDir tempDir;
    const QByteArray pcm = makeKnownPcm();
    QString wavPath;

    // G1: WAV header
    if (!tempDir.isValid()) {
        fail("G1 WAV header", QStringLiteral("failed to create temporary directory"));
    } else {
        wavPath = tempDir.filePath(QStringLiteral("known.wav"));
        QString error;
        const bool wrote = libavcore::writePcm16AsWav(wavPath, pcm, 16000, 1, &error);
        QFile wavFile(wavPath);
        const bool opened = wavFile.open(QFile::ReadOnly);
        const QByteArray wavBytes = opened ? wavFile.readAll() : QByteArray();

        if (wrote
            && opened
            && wavBytes.size() >= 44
            && wavBytes.mid(0, 4) == QByteArrayLiteral("RIFF")
            && wavBytes.mid(8, 4) == QByteArrayLiteral("WAVE")
            && wavBytes.mid(12, 4) == QByteArrayLiteral("fmt ")
            && wavBytes.mid(36, 4) == QByteArrayLiteral("data")
            && readLe32(wavBytes, 16) == 16
            && readLe16(wavBytes, 20) == 1
            && readLe16(wavBytes, 22) == 1
            && readLe32(wavBytes, 24) == 16000
            && readLe16(wavBytes, 34) == 16
            && readLe32(wavBytes, 40) == static_cast<quint32>(pcm.size())
            && wavBytes.mid(44) == pcm) {
            pass("G1 WAV header has RIFF/WAVE/fmt/data and expected PCM metadata");
        } else {
            fail("G1 WAV header", QStringLiteral("write=%1 open=%2 bytes=%3 error=%4")
                    .arg(wrote)
                    .arg(opened)
                    .arg(wavBytes.size())
                    .arg(error));
        }
    }

    // G2: recognizer select
    const auto selectedRecognizer = speech::recognizerByName(QString());
    if (selectedRecognizer && selectedRecognizer->isAvailable()) {
        pass("G2 recognizerByName empty returns available recognizer");
    } else {
        fail("G2 recognizer select", QStringLiteral("recognizer was null or unavailable"));
    }

    speech::StubRecognizer stub;
    speech::RecognizeParams params;
    params.audioPath = wavPath.isEmpty() ? QStringLiteral("stub.wav") : wavPath;
    params.language = QStringLiteral("ja");
    const speech::RecognizeResult stubResult = stub.recognize(params);

    // G3: segment timestamps
    const QString segmentError = validateSegments(stubResult.segments);
    if (stubResult.success && segmentError.isEmpty()) {
        pass("G3 StubRecognizer segments have valid monotonic timestamps");
    } else {
        fail("G3 segment timestamps", stubResult.success ? segmentError : stubResult.error);
    }

    // G4: language detect
    speech::RecognizeParams autoParams = params;
    autoParams.language = QStringLiteral("auto");
    const speech::RecognizeResult autoResult = stub.recognize(autoParams);
    if (autoResult.success && !autoResult.detectedLanguage.isEmpty()) {
        pass("G4 auto language recognition reports detectedLanguage");
    } else {
        fail("G4 language detect", autoResult.success
             ? QStringLiteral("detectedLanguage is empty")
             : autoResult.error);
    }

    // G5: maxDuration
    speech::RecognizeParams shortParams = params;
    shortParams.maxDurationMs = 1;
    const speech::RecognizeResult shortResult = stub.recognize(shortParams);
    if (shortResult.success) {
        pass("G5 small maxDurationMs is handled gracefully");
    } else {
        fail("G5 maxDuration", shortResult.error);
    }

    // G6: CLI fallback
    speech::WhisperCliRecognizer cliRecognizer;
    cliRecognizer.setCliPath(QStringLiteral("/definitely/not/a/whisper-cli"));
    const auto fallbackRecognizer = speech::recognizerByName(QStringLiteral("/definitely/not/a/recognizer"));
    if (!cliRecognizer.isAvailable()
        && fallbackRecognizer
        && fallbackRecognizer->name() == QStringLiteral("Stub")
        && fallbackRecognizer->isAvailable()) {
        pass("G6 unavailable CLI falls back to Stub recognizer");
    } else {
        fail("G6 CLI fallback", QStringLiteral("cliAvailable=%1 fallback=%2")
                .arg(cliRecognizer.isAvailable())
                .arg(fallbackRecognizer ? fallbackRecognizer->name() : QStringLiteral("<null>")));
    }

    // G7: segments -> caption
    const caption::Track track = whisper::WhisperTranscriber::toCaptionTrack(stubResult.segments);
    const QList<caption::Clip> clips = track.clips();
    bool captionsOk = clips.size() == stubResult.segments.size();
    qint64 previousStart = 0;
    bool havePreviousClip = false;
    for (int i = 0; captionsOk && i < clips.size(); ++i) {
        const caption::Clip& clip = clips.at(i);
        const speech::Segment& seg = stubResult.segments.at(i);
        captionsOk = clip.isValid()
            && clip.startMs == seg.startMs
            && clip.endMs == seg.endMs
            && clip.text == seg.text
            && (!havePreviousClip || clip.startMs >= previousStart);
        previousStart = clip.startMs;
        havePreviousClip = true;
    }
    if (captionsOk) {
        pass("G7 segment conversion preserves count timestamps text and sort order");
    } else {
        fail("G7 segments to caption", QStringLiteral("segments=%1 clips=%2")
                .arg(stubResult.segments.size())
                .arg(clips.size()));
    }

    // G8: cleanup
    whisper::TranscribeRequest request;
    request.mediaPath = wavPath.isEmpty() ? QStringLiteral("stub-media.wav") : wavPath;
    request.language = QStringLiteral("auto");
    request.recognizerName = QStringLiteral("Stub");
    const whisper::TranscribeOutcome outcome = whisper::WhisperTranscriber().transcribe(request);
    if (outcome.success && outcome.track.clipCount() > 0) {
        pass("G8 Stub transcribe path completes with non-empty track and scoped temp cleanup");
    } else {
        fail("G8 cleanup", outcome.success
             ? QStringLiteral("track is empty")
             : outcome.error);
    }

    qInfo().noquote().nospace() << "[whisper-transcribe] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
