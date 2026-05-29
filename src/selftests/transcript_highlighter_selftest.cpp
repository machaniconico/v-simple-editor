#include <QDebug>
#include <QString>
#include <QVector>
#include <QList>

#include "../TranscriptHighlighter.h"
#include "../CaptionTrack.h"
#include "../AIHighlight.h"

namespace {

QList<caption::Clip> makeTranscript()
{
    return {
        {0, 1500, QStringLiteral("Opening setup with a quiet introduction"), QString()},
        {1500, 3250, QStringLiteral("The guest reveals the surprising result"), QString()},
        {3250, 5200, QStringLiteral("Everyone reacts and explains why it matters"), QString()}
    };
}

bool nearlyEqual(double lhs, double rhs)
{
    const double diff = lhs > rhs ? lhs - rhs : rhs - lhs;
    return diff < 0.0001;
}

bool isValidHighlightRange(const Highlight& highlight)
{
    return highlight.startTime < highlight.endTime
        && highlight.score >= 0.0
        && highlight.score <= 1.0;
}

} // namespace

int runTranscriptHighlighterSelftest()
{
    qInfo().noquote() << "[transcript-highlighter] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[transcript-highlighter] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[transcript-highlighter] FAIL" << name << ":" << msg; };

    using transcripthl::HighlightRequest;
    using transcripthl::TranscriptHighlighter;

    const QList<caption::Clip> transcript = makeTranscript();

    // G1: buildPrompt
    const QString prompt = TranscriptHighlighter::buildPrompt(transcript, 3);
    bool promptOk = prompt.contains(QStringLiteral("\"highlights\""))
        && prompt.contains(QStringLiteral("[00:00.000 -> 00:01.500]"))
        && prompt.contains(QStringLiteral("[00:01.500 -> 00:03.250]"))
        && prompt.contains(QStringLiteral("[00:03.250 -> 00:05.200]"));
    for (const caption::Clip& clip : transcript)
        promptOk = promptOk && prompt.contains(clip.text);
    if (promptOk) {
        pass("G1 buildPrompt includes transcript text timestamps and highlights JSON key");
    } else {
        fail("G1 buildPrompt", QStringLiteral("prompt missing expected text timestamp or JSON key"));
    }

    // G2: mock detect
    HighlightRequest request;
    request.transcript = transcript;
    request.targetCount = 2;
    const QString expectedDetectPrompt = TranscriptHighlighter::buildPrompt(transcript, request.targetCount);
    bool senderSawPrompt = false;
    QString detectError;
    const QVector<Highlight> detected = TranscriptHighlighter().detect(
        request,
        [&](const QString& sentPrompt, QString* err) {
            if (err)
                err->clear();
            senderSawPrompt = sentPrompt == expectedDetectPrompt;
            return QStringLiteral("{\"highlights\":["
                                  "{\"start\":1.5,\"end\":3.0,\"score\":0.91,\"reason\":\"reveal\"},"
                                  "{\"start\":3.25,\"end\":5.0,\"score\":0.73,\"reason\":\"reaction\"}"
                                  "]}");
        },
        &detectError);
    bool detectedOk = senderSawPrompt && detectError.isEmpty() && detected.size() == 2;
    for (const Highlight& highlight : detected)
        detectedOk = detectedOk && isValidHighlightRange(highlight);
    if (detectedOk) {
        pass("G2 mock detect returns expected valid highlights without network");
    } else {
        fail("G2 mock detect", QStringLiteral("count=%1 senderPrompt=%2 error=%3")
                .arg(detected.size())
                .arg(senderSawPrompt)
                .arg(detectError));
    }

    // G3: parseHighlights
    QString parseError;
    const QVector<Highlight> parsed = TranscriptHighlighter::parseHighlights(
        QStringLiteral("{\"highlights\":["
                       "{\"start\":0.5,\"end\":1.25,\"score\":0.4,\"reason\":\"first\"},"
                       "{\"start\":2.0,\"end\":4.5,\"score\":0.8,\"reason\":\"second\"}"
                       "]}"),
        &parseError);
    const bool parsedOk = parseError.isEmpty()
        && parsed.size() == 2
        && nearlyEqual(parsed.at(0).startTime, 0.5)
        && nearlyEqual(parsed.at(0).endTime, 1.25)
        && nearlyEqual(parsed.at(0).score, 0.4)
        && parsed.at(0).description == QStringLiteral("first")
        && parsed.at(0).type == HighlightType::SpeechSegment
        && nearlyEqual(parsed.at(1).startTime, 2.0)
        && nearlyEqual(parsed.at(1).endTime, 4.5)
        && nearlyEqual(parsed.at(1).score, 0.8)
        && parsed.at(1).description == QStringLiteral("second");
    if (parsedOk) {
        pass("G3 parseHighlights preserves count values type and reasons");
    } else {
        fail("G3 parseHighlights", QStringLiteral("count=%1 error=%2")
                .arg(parsed.size())
                .arg(parseError));
    }

    // G4: extractJsonObject edge cases
    QString extractError;
    const QString fenced = TranscriptHighlighter::extractJsonObject(
        QStringLiteral("```json\n{\"highlights\":[]}\n```"),
        &extractError);
    const bool fencedOk = extractError.isEmpty() && fenced == QStringLiteral("{\"highlights\":[]}");

    const QString prose = TranscriptHighlighter::extractJsonObject(
        QStringLiteral("Here is the result: {\"highlights\":[{\"start\":1,\"end\":2}]} Thanks."),
        &extractError);
    const bool proseOk = extractError.isEmpty()
        && prose == QStringLiteral("{\"highlights\":[{\"start\":1,\"end\":2}]}");

    const QString nested = TranscriptHighlighter::extractJsonObject(
        QStringLiteral("prefix {\"a\":{\"b\":1}} suffix"),
        &extractError);
    const bool nestedOk = extractError.isEmpty() && nested == QStringLiteral("{\"a\":{\"b\":1}}");

    const QString stringBrace = TranscriptHighlighter::extractJsonObject(
        QStringLiteral("prefix {\"t\":\"a}b\"} suffix"),
        &extractError);
    const bool stringBraceOk = extractError.isEmpty() && stringBrace == QStringLiteral("{\"t\":\"a}b\"}");

    if (fencedOk && proseOk && nestedOk && stringBraceOk) {
        pass("G4 extractJsonObject handles fence prose nested braces and string braces");
    } else {
        fail("G4 extractJsonObject edge", QStringLiteral("fence=%1 prose=%2 nested=%3 stringBrace=%4 error=%5")
                .arg(fencedOk)
                .arg(proseOk)
                .arg(nestedOk)
                .arg(stringBraceOk)
                .arg(extractError));
    }

    // G5: fallback
    QString fallbackError;
    const QString noJson = TranscriptHighlighter::extractJsonObject(
        QStringLiteral("plain prose without an object"),
        &fallbackError);
    const bool noJsonOk = noJson.isEmpty() && !fallbackError.isEmpty();

    QString invalidJsonError;
    const QVector<Highlight> invalidParsed = TranscriptHighlighter::parseHighlights(
        QStringLiteral("{\"highlights\":["),
        &invalidJsonError);
    const bool invalidJsonOk = invalidParsed.isEmpty() && !invalidJsonError.isEmpty();

    QString failingDetectError;
    const QVector<Highlight> failingDetected = TranscriptHighlighter().detect(
        request,
        [](const QString&, QString* err) {
            if (err)
                *err = QStringLiteral("net fail");
            return QString();
        },
        &failingDetectError);
    const bool failingDetectOk = failingDetected.isEmpty()
        && failingDetectError == QStringLiteral("net fail");

    if (noJsonOk && invalidJsonOk && failingDetectOk) {
        pass("G5 fallback paths return empty results and errors without crashing");
    } else {
        fail("G5 fallback", QStringLiteral("noJson=%1 invalidJson=%2 failingDetect=%3 detectError=%4")
                .arg(noJsonOk)
                .arg(invalidJsonOk)
                .arg(failingDetectOk)
                .arg(failingDetectError));
    }

    // G6: output validation
    HighlightRequest clampRequest;
    clampRequest.transcript = {
        {2000, 3500, QStringLiteral("Middle section begins"), QString()},
        {3500, 6000, QStringLiteral("Middle section ends"), QString()}
    };
    clampRequest.targetCount = 2;
    QString clampError;
    const QVector<Highlight> clamped = TranscriptHighlighter().detect(
        clampRequest,
        [](const QString&, QString* err) {
            if (err)
                err->clear();
            return QStringLiteral("{\"highlights\":["
                                  "{\"start\":-10,\"end\":4.0,\"score\":0.5,\"reason\":\"early\"},"
                                  "{\"start\":5.0,\"end\":99,\"score\":1.3,\"reason\":\"late\"},"
                                  "{\"start\":3.0,\"end\":4.0,\"score\":0.2,\"reason\":\"extra\"}"
                                  "]}");
        },
        &clampError);
    bool clampedOk = clampError.isEmpty() && clamped.size() <= clampRequest.targetCount;
    for (const Highlight& highlight : clamped) {
        clampedOk = clampedOk
            && highlight.startTime >= 2.0
            && highlight.startTime <= 6.0
            && highlight.endTime >= 2.0
            && highlight.endTime <= 6.0
            && highlight.score >= 0.0
            && highlight.score <= 1.0;
    }
    if (clampedOk) {
        pass("G6 detect clamps output to transcript range and targetCount");
    } else {
        fail("G6 output validation", QStringLiteral("count=%1 target=%2 error=%3")
                .arg(clamped.size())
                .arg(clampRequest.targetCount)
                .arg(clampError));
    }

    qInfo().noquote().nospace() << "[transcript-highlighter] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
