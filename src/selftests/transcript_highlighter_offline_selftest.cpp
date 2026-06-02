#include <QDebug>
#include <QList>
#include <QString>
#include <QVector>

#include "../AIHighlight.h"
#include "../CaptionTrack.h"
#include "../TranscriptHighlighter.h"

namespace {

bool nearlyEqual(double lhs, double rhs)
{
    const double diff = lhs > rhs ? lhs - rhs : rhs - lhs;
    return diff < 0.0001;
}

bool sameHighlights(const QVector<Highlight>& lhs, const QVector<Highlight>& rhs)
{
    if (lhs.size() != rhs.size())
        return false;

    for (int i = 0; i < lhs.size(); ++i) {
        if (!nearlyEqual(lhs.at(i).startTime, rhs.at(i).startTime)
            || !nearlyEqual(lhs.at(i).endTime, rhs.at(i).endTime)
            || !nearlyEqual(lhs.at(i).score, rhs.at(i).score)
            || lhs.at(i).type != rhs.at(i).type
            || lhs.at(i).description != rhs.at(i).description) {
            return false;
        }
    }
    return true;
}

bool sortedByScoreThenStart(const QVector<Highlight>& highlights)
{
    for (int i = 1; i < highlights.size(); ++i) {
        const Highlight& prev = highlights.at(i - 1);
        const Highlight& cur = highlights.at(i);
        if (cur.score > prev.score)
            return false;
        if (nearlyEqual(cur.score, prev.score) && cur.startTime < prev.startTime)
            return false;
    }
    return true;
}

bool scoresInRange(const QVector<Highlight>& highlights)
{
    for (const Highlight& highlight : highlights) {
        if (highlight.score < 0.0 || highlight.score > 1.0)
            return false;
    }
    return true;
}

} // namespace

int runTranscriptHighlighterOfflineSelftest()
{
    qInfo().noquote() << "[transcript-highlighter-offline] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[transcript-highlighter-offline] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[transcript-highlighter-offline] FAIL" << name << ":" << msg; };

    using transcripthl::HighlightRequest;
    using transcripthl::TranscriptHighlighter;

    const QList<caption::Clip> transcript = {
        {0, 1200, QStringLiteral("Quiet setup for the discussion"), QString()},
        {1200, 2600, QStringLiteral("The guest reveals a surprising result!"), QString()},
        {2600, 4300, QStringLiteral("A dense explanation of the important solution and why it matters?"), QString()},
        {4300, 6100, QStringLiteral("Short wrap"), QString()}
    };

    const QVector<Highlight> first = TranscriptHighlighter::detectOffline(transcript, 3);
    const QVector<Highlight> second = TranscriptHighlighter::detectOffline(transcript, 3);
    if (sameHighlights(first, second)) {
        pass("G1 deterministic same input same output");
    } else {
        fail("G1 deterministic", QStringLiteral("first=%1 second=%2").arg(first.size()).arg(second.size()));
    }

    const QVector<Highlight> limited = TranscriptHighlighter::detectOffline(transcript, 2);
    if (limited.size() == 2) {
        pass("G2 targetCount respected");
    } else {
        fail("G2 targetCount", QStringLiteral("count=%1").arg(limited.size()));
    }

    if (!first.isEmpty() && scoresInRange(first) && sortedByScoreThenStart(first)) {
        pass("G3 scores in range and sorted by score then start");
    } else {
        fail("G3 score/sort", QStringLiteral("count=%1 inRange=%2 sorted=%3")
                .arg(first.size())
                .arg(scoresInRange(first))
                .arg(sortedByScoreThenStart(first)));
    }

    const QList<caption::Clip> tiedTranscript = {
        {1000, 2000, QStringLiteral("same neutral words"), QString()},
        {3000, 4000, QStringLiteral("same neutral words"), QString()}
    };
    const QVector<Highlight> tied = TranscriptHighlighter::detectOffline(tiedTranscript, 2);
    const bool tieOk = tied.size() == 2
        && nearlyEqual(tied.at(0).score, tied.at(1).score)
        && tied.at(0).startTime < tied.at(1).startTime;
    if (tieOk) {
        pass("G4 equal scores tie-break by start time ascending");
    } else {
        fail("G4 tie-break", QStringLiteral("count=%1").arg(tied.size()));
    }

    const QList<caption::Clip> clampTranscript = {
        {1000, 5000, QStringLiteral("normal reference range"), QString()},
        {6000, 500, QStringLiteral("surprising important reveal!!!"), QString()}
    };
    const QVector<Highlight> clamped = TranscriptHighlighter::detectOffline(clampTranscript, 2);
    bool clampOk = !clamped.isEmpty();
    for (const Highlight& highlight : clamped) {
        clampOk = clampOk
            && highlight.startTime >= 1.0
            && highlight.startTime <= 5.0
            && highlight.endTime >= 1.0
            && highlight.endTime <= 5.0
            && highlight.endTime >= highlight.startTime;
    }
    if (clampOk) {
        pass("G5 start/end clamped to transcript range");
    } else {
        fail("G5 clamp", QStringLiteral("count=%1").arg(clamped.size()));
    }

    const QList<caption::Clip> keywordTranscript = {
        {0, 2000, QStringLiteral("plain everyday sentence with ordinary pacing"), QString()},
        {2000, 4000, QStringLiteral("critical surprising reveal with important result"), QString()}
    };
    const QVector<Highlight> keywordHits = TranscriptHighlighter::detectOffline(keywordTranscript, 2);
    const bool keywordOk = keywordHits.size() == 2
        && nearlyEqual(keywordHits.at(0).startTime, 2.0)
        && keywordHits.at(0).score > keywordHits.at(1).score;
    if (keywordOk) {
        pass("G6 salient keywords raise rank");
    } else {
        fail("G6 keyword sensitivity", QStringLiteral("count=%1").arg(keywordHits.size()));
    }

    HighlightRequest offlineRequest;
    offlineRequest.transcript = transcript;
    offlineRequest.targetCount = 2;
    offlineRequest.provider = QStringLiteral("offline");
    QString detectError;
    const QVector<Highlight> offlineDetected = TranscriptHighlighter().detect(offlineRequest, nullptr, &detectError);
    const bool detectOfflineOk = detectError.isEmpty()
        && sameHighlights(offlineDetected, TranscriptHighlighter::detectOffline(transcript, 2));
    if (detectOfflineOk) {
        pass("G7 detect provider offline dispatches to offline scorer");
    } else {
        fail("G7 detect offline provider", QStringLiteral("count=%1 error=%2")
                .arg(offlineDetected.size())
                .arg(detectError));
    }

    const bool emptyOk = TranscriptHighlighter::detectOffline({}, 3).isEmpty()
        && TranscriptHighlighter::detectOffline(transcript, 0).isEmpty();
    if (emptyOk) {
        pass("G8 empty transcript and zero target return empty");
    } else {
        fail("G8 empty input", QStringLiteral("unexpected non-empty result"));
    }

    qInfo().noquote().nospace() << "[transcript-highlighter-offline] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
