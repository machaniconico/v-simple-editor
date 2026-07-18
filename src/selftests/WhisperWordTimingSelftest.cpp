#include <QDebug>
#include <QString>
#include <QtGlobal>

#include "../SubtitleGenerator.h"
#include "../WhisperTranscriber.h"

namespace {

QString validateWordBoundaries(const QList<speech::Segment>& segments)
{
    if (segments.isEmpty())
        return QStringLiteral("segments are empty");

    for (const speech::Segment& segment : segments) {
        qint64 previousEndMs = segment.startMs;
        for (const speech::Word& word : segment.words) {
            if (word.text.trimmed().isEmpty())
                return QStringLiteral("empty word text");
            if (word.startMs < segment.startMs || word.endMs > segment.endMs) {
                return QStringLiteral("word %1 outside segment %2-%3")
                    .arg(word.text)
                    .arg(segment.startMs)
                    .arg(segment.endMs);
            }
            if (word.startMs < previousEndMs || word.endMs <= word.startMs) {
                return QStringLiteral("non-monotonic word %1 %2-%3 previousEnd=%4")
                    .arg(word.text)
                    .arg(word.startMs)
                    .arg(word.endMs)
                    .arg(previousEndMs);
            }
            previousEndMs = word.endMs;
        }
    }

    return QString();
}

} // namespace

int runWhisperWordTimingSelftest()
{
    qInfo().noquote() << "[whisper-word-timing] selftest start";
    int passed = 0;
    int failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[whisper-word-timing] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[whisper-word-timing] FAIL" << name << ":" << msg;
    };
    auto check = [&](const char* name, bool ok, const QString& msg = QString()) {
        if (ok)
            pass(name);
        else
            fail(name, msg);
    };

    const QByteArray fixture = R"json({
        "result": { "language": "en" },
        "transcription": [
            {
                "offsets": { "from": 1000, "to": 3300 },
                "text": " karaoke words",
                "tokens": [
                    { "text": " karaoke", "offsets": { "from": 1000, "to": 1900 }, "p": 0.98 },
                    { "text": " words",   "offsets": { "from": 1900, "to": 3200 }, "p": 0.97 }
                ]
            },
            {
                "offsets": { "from": 3300, "to": 4500 },
                "text": " again",
                "words": [
                    { "word": "again", "start": 3.3, "end": 4.5, "probability": 0.96 }
                ]
            },
            {
                "start": 4.5,
                "end": 5.5,
                "text": " legacy",
                "words": [
                    { "word": "legacy", "t0": 450, "t1": 550, "probability": 0.95 }
                ]
            }
        ]
    })json";

    QString language;
    QString error;
    const QList<speech::Segment> segments =
        whisper::WhisperTranscriber::parseWhisperJsonSegments(fixture, &language, &error);

    check("G1 fixture parses without error",
          error.isEmpty() && segments.size() == 3 && language == QStringLiteral("en"),
          QStringLiteral("error='%1' segments=%2 language='%3'")
              .arg(error)
              .arg(segments.size())
              .arg(language));

    check("G2 token and word timings populate Segment.words",
          segments.size() == 3
              && segments.at(0).words.size() == 2
              && segments.at(0).words.at(0).text == QStringLiteral("karaoke")
              && segments.at(0).words.at(0).startMs == 1000
              && segments.at(0).words.at(0).endMs == 1900
              && segments.at(0).words.at(1).text == QStringLiteral("words")
              && segments.at(1).words.size() == 1
              && segments.at(1).words.at(0).startMs == 3300
              && segments.at(1).words.at(0).endMs == 4500
              && segments.at(2).words.size() == 1
              && segments.at(2).words.at(0).startMs == 4500
              && segments.at(2).words.at(0).endMs == 5500,
          segments.isEmpty()
              ? QStringLiteral("no segments")
              : QStringLiteral("seg0Words=%1 seg1Words=%2")
                    .arg(segments.at(0).words.size())
                    .arg(segments.size() > 1 ? segments.at(1).words.size() : -1));

    const QString boundaryError = validateWordBoundaries(segments);
    check("G3 word timings are monotonic and inside segment bounds",
          boundaryError.isEmpty(),
          boundaryError);

    const QVector<SubtitleSegment> subtitleSegments =
        whisper::WhisperTranscriber::toSubtitleSegments(segments, language);
    check("G4 SubtitleWord conversion preserves word text and seconds",
          subtitleSegments.size() == 3
              && subtitleSegments.at(0).words.size() == 2
              && subtitleSegments.at(0).words.at(0).text == QStringLiteral("karaoke")
              && qAbs(subtitleSegments.at(0).words.at(0).startTime - 1.0) < 0.0001
              && qAbs(subtitleSegments.at(0).words.at(1).endTime - 3.2) < 0.0001,
          QStringLiteral("subtitleSegments=%1").arg(subtitleSegments.size()));

    const QByteArray noWordFixture = R"json({
        "language": "en",
        "segments": [
            { "start": 0.0, "end": 2.0, "text": "plain segment" }
        ]
    })json";
    const QList<speech::Segment> noWordSegments =
        whisper::WhisperTranscriber::parseWhisperJsonSegments(noWordFixture, nullptr, &error);
    const caption::Track track = whisper::WhisperTranscriber::toCaptionTrack(noWordSegments);
    const QVector<SubtitleSegment> noWordSubtitleSegments =
        whisper::WhisperTranscriber::toSubtitleSegments(noWordSegments, QStringLiteral("en"));
    check("G5 no-word path stays segment-level compatible",
          error.isEmpty()
              && noWordSegments.size() == 1
              && noWordSegments.at(0).words.isEmpty()
              && track.clipCount() == 1
              && track.clipAt(0).startMs == 0
              && track.clipAt(0).endMs == 2000
              && track.clipAt(0).text == QStringLiteral("plain segment")
              && noWordSubtitleSegments.size() == 1
              && noWordSubtitleSegments.at(0).words.isEmpty(),
          QStringLiteral("error='%1' segments=%2 clips=%3")
              .arg(error)
              .arg(noWordSegments.size())
              .arg(track.clipCount()));

    qInfo().noquote() << "[whisper-word-timing] selftest done: passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
