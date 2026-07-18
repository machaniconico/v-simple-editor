#include "WhisperTranscriber.h"

#include "libavcore/AudioExtract.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QStringList>
#include <QTemporaryDir>
#include <QtGlobal>
#include <cmath>
#include <limits>

namespace whisper {

namespace {

QString modelIdForRecognizer(const speech::Recognizer& recognizer)
{
    if (recognizer.name() == QStringLiteral("Stub"))
        return QStringLiteral("stub");
    return QStringLiteral("base");
}

struct TimeRange {
    qint64 startMs = 0;
    qint64 endMs = 0;
    bool valid = false;
};

qint64 roundedMs(double value)
{
    return static_cast<qint64>(std::llround(value));
}

bool numberValue(const QJsonValue& value, double* out)
{
    if (!value.isDouble())
        return false;
    const double v = value.toDouble();
    if (!std::isfinite(v))
        return false;
    *out = v;
    return true;
}

bool timestampStringMs(const QString& raw, qint64* out)
{
    QString text = raw.trimmed();
    if (text.isEmpty())
        return false;

    text.replace(QLatin1Char(','), QLatin1Char('.'));
    const QStringList parts = text.split(QLatin1Char(':'));
    bool ok = false;
    double seconds = 0.0;

    if (parts.size() == 3) {
        const int hours = parts.at(0).toInt(&ok);
        if (!ok)
            return false;
        const int minutes = parts.at(1).toInt(&ok);
        if (!ok)
            return false;
        const double sec = parts.at(2).toDouble(&ok);
        if (!ok)
            return false;
        seconds = hours * 3600.0 + minutes * 60.0 + sec;
    } else if (parts.size() == 2) {
        const int minutes = parts.at(0).toInt(&ok);
        if (!ok)
            return false;
        const double sec = parts.at(1).toDouble(&ok);
        if (!ok)
            return false;
        seconds = minutes * 60.0 + sec;
    } else if (parts.size() == 1) {
        seconds = parts.at(0).toDouble(&ok);
        if (!ok)
            return false;
    } else {
        return false;
    }

    *out = roundedMs(seconds * 1000.0);
    return true;
}

bool fieldMs(const QJsonValue& value, bool secondsUnit, qint64* out)
{
    double number = 0.0;
    if (numberValue(value, &number)) {
        *out = secondsUnit ? roundedMs(number * 1000.0) : roundedMs(number);
        return true;
    }
    if (value.isString())
        return timestampStringMs(value.toString(), out);
    return false;
}

TimeRange rangeFromOffsetObject(const QJsonObject& obj)
{
    const QJsonObject offsets = obj.value(QStringLiteral("offsets")).toObject();
    if (offsets.isEmpty())
        return {};

    qint64 fromMs = 0;
    qint64 toMs = 0;
    const bool ok = fieldMs(offsets.value(QStringLiteral("from")), false, &fromMs)
        && fieldMs(offsets.value(QStringLiteral("to")), false, &toMs);
    return { fromMs, toMs, ok && toMs > fromMs };
}

TimeRange rangeFromTimestampObject(const QJsonObject& obj)
{
    const QJsonObject timestamps = obj.value(QStringLiteral("timestamps")).toObject();
    if (timestamps.isEmpty())
        return {};

    qint64 fromMs = 0;
    qint64 toMs = 0;
    const bool ok = fieldMs(timestamps.value(QStringLiteral("from")), true, &fromMs)
        && fieldMs(timestamps.value(QStringLiteral("to")), true, &toMs);
    return { fromMs, toMs, ok && toMs > fromMs };
}

TimeRange inferLegacyTokenRange(double rawStart, double rawEnd, qint64 segmentStartMs, qint64 segmentEndMs)
{
    struct Candidate {
        qint64 startMs;
        qint64 endMs;
        int score;
    };

    const Candidate candidates[] = {
        { roundedMs(rawStart * 1000.0), roundedMs(rawEnd * 1000.0), 0 },
        { roundedMs(rawStart),          roundedMs(rawEnd),          0 },
        { roundedMs(rawStart * 10.0),   roundedMs(rawEnd * 10.0),   0 },
    };

    Candidate best = candidates[0];
    best.score = -1000000;
    for (Candidate candidate : candidates) {
        int score = 0;
        if (candidate.endMs > candidate.startMs)
            score += 100;
        if (candidate.startMs >= segmentStartMs)
            score += 20;
        else
            score -= 20;
        if (candidate.endMs <= segmentEndMs)
            score += 20;
        else
            score -= 20;
        if (candidate.startMs <= segmentEndMs && candidate.endMs >= segmentStartMs)
            score += 10;
        candidate.score = score;
        if (candidate.score > best.score)
            best = candidate;
    }

    return { best.startMs, best.endMs, best.endMs > best.startMs };
}

TimeRange rangeFromObject(const QJsonObject& obj,
                          qint64 segmentStartMs,
                          qint64 segmentEndMs,
                          bool segmentRange)
{
    TimeRange range = rangeFromOffsetObject(obj);
    if (range.valid)
        return range;

    range = rangeFromTimestampObject(obj);
    if (range.valid)
        return range;

    if (obj.contains(QStringLiteral("start")) && obj.contains(QStringLiteral("end"))) {
        qint64 startMs = 0;
        qint64 endMs = 0;
        const bool ok = fieldMs(obj.value(QStringLiteral("start")), true, &startMs)
            && fieldMs(obj.value(QStringLiteral("end")), true, &endMs);
        return { startMs, endMs, ok && endMs > startMs };
    }

    if (obj.contains(QStringLiteral("from")) && obj.contains(QStringLiteral("to"))) {
        qint64 startMs = 0;
        qint64 endMs = 0;
        const bool ok = fieldMs(obj.value(QStringLiteral("from")), segmentRange, &startMs)
            && fieldMs(obj.value(QStringLiteral("to")), segmentRange, &endMs);
        return { startMs, endMs, ok && endMs > startMs };
    }

    if (obj.contains(QStringLiteral("t0")) && obj.contains(QStringLiteral("t1"))) {
        double start = 0.0;
        double end = 0.0;
        if (numberValue(obj.value(QStringLiteral("t0")), &start)
            && numberValue(obj.value(QStringLiteral("t1")), &end)) {
            if (segmentRange && segmentEndMs == std::numeric_limits<qint64>::max()) {
                const qint64 startMs = roundedMs(start * 10.0);
                const qint64 endMs = roundedMs(end * 10.0);
                return { startMs, endMs, endMs > startMs };
            }
            return inferLegacyTokenRange(start, end, segmentStartMs, segmentEndMs);
        }
    }

    return {};
}

QString normalizedTokenText(const QString& raw)
{
    QString text = raw;
    text.replace(QString::fromUtf8("\xE2\x96\x81"), QLatin1String(" "));
    text.replace(QString::fromUtf8("\xC4\xA0"), QLatin1String(" "));
    return text.trimmed();
}

QString wordTextFromObject(const QJsonObject& obj)
{
    if (obj.contains(QStringLiteral("word")))
        return normalizedTokenText(obj.value(QStringLiteral("word")).toString());
    if (obj.contains(QStringLiteral("text")))
        return normalizedTokenText(obj.value(QStringLiteral("text")).toString());
    if (obj.contains(QStringLiteral("token")))
        return normalizedTokenText(obj.value(QStringLiteral("token")).toString());
    return QString();
}

double confidenceFromObject(const QJsonObject& obj, double fallback)
{
    const QStringList keys = {
        QStringLiteral("confidence"),
        QStringLiteral("probability"),
        QStringLiteral("prob"),
        QStringLiteral("p")
    };
    for (const QString& key : keys) {
        double value = 0.0;
        if (numberValue(obj.value(key), &value))
            return qBound(0.0, value, 1.0);
    }
    return fallback;
}

void appendWord(QList<speech::Word>* words,
                const QString& text,
                qint64 startMs,
                qint64 endMs,
                double confidence,
                qint64 segmentStartMs,
                qint64 segmentEndMs)
{
    const QString cleanText = text.trimmed();
    if (cleanText.isEmpty() || segmentEndMs <= segmentStartMs)
        return;

    startMs = qBound(segmentStartMs, startMs, segmentEndMs);
    endMs = qBound(segmentStartMs, endMs, segmentEndMs);
    if (!words->isEmpty() && startMs < words->last().endMs)
        startMs = words->last().endMs;
    if (endMs <= startMs)
        return;

    speech::Word word;
    word.startMs = startMs;
    word.endMs = endMs;
    word.text = cleanText;
    word.confidence = confidence;
    words->append(word);
}

QList<speech::Word> wordsFromWordArray(const QJsonArray& wordArray,
                                       qint64 segmentStartMs,
                                       qint64 segmentEndMs)
{
    QList<speech::Word> words;
    for (const QJsonValue& value : wordArray) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();
        const QString text = wordTextFromObject(obj);
        const TimeRange range = rangeFromObject(obj, segmentStartMs, segmentEndMs, false);
        if (!range.valid)
            continue;
        appendWord(&words,
                   text,
                   range.startMs,
                   range.endMs,
                   confidenceFromObject(obj, 1.0),
                   segmentStartMs,
                   segmentEndMs);
    }
    return words;
}

bool hasExplicitWordBoundary(const QString& rawText)
{
    return rawText.startsWith(QLatin1Char(' '))
        || rawText.startsWith(QLatin1Char('\t'))
        || rawText.startsWith(QLatin1Char('\n'))
        || rawText.startsWith(QString::fromUtf8("\xE2\x96\x81"))
        || rawText.startsWith(QString::fromUtf8("\xC4\xA0"));
}

bool isAsciiWordPiece(const QString& text)
{
    if (text.isEmpty())
        return false;
    for (const QChar ch : text) {
        if (ch.unicode() >= 128)
            return false;
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('\'') && ch != QLatin1Char('-'))
            return false;
    }
    return true;
}

bool isAsciiPunctuationOnly(const QString& text)
{
    if (text.isEmpty())
        return false;
    for (const QChar ch : text) {
        if (ch.unicode() >= 128)
            return false;
        if (ch.isLetterOrNumber() || ch.isSpace())
            return false;
    }
    return true;
}

bool shouldMergeTokenPiece(const QString& pendingText, const QString& text)
{
    return (isAsciiWordPiece(pendingText) && isAsciiWordPiece(text))
        || isAsciiPunctuationOnly(text);
}

QList<speech::Word> wordsFromTokenArray(const QJsonArray& tokenArray,
                                        qint64 segmentStartMs,
                                        qint64 segmentEndMs)
{
    QList<speech::Word> words;
    QString pendingText;
    qint64 pendingStartMs = 0;
    qint64 pendingEndMs = 0;
    double pendingConfidence = 1.0;
    bool hasPending = false;

    auto flushPending = [&]() {
        if (!hasPending)
            return;
        appendWord(&words,
                   pendingText,
                   pendingStartMs,
                   pendingEndMs,
                   pendingConfidence,
                   segmentStartMs,
                   segmentEndMs);
        pendingText.clear();
        pendingStartMs = 0;
        pendingEndMs = 0;
        pendingConfidence = 1.0;
        hasPending = false;
    };

    for (const QJsonValue& value : tokenArray) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();
        const QString rawText = obj.contains(QStringLiteral("word"))
            ? obj.value(QStringLiteral("word")).toString()
            : (obj.contains(QStringLiteral("text"))
                ? obj.value(QStringLiteral("text")).toString()
                : obj.value(QStringLiteral("token")).toString());
        const QString text = normalizedTokenText(rawText);
        if (text.isEmpty())
            continue;

        const TimeRange range = rangeFromObject(obj, segmentStartMs, segmentEndMs, false);
        if (!range.valid)
            continue;

        if (hasPending
            && (hasExplicitWordBoundary(rawText) || !shouldMergeTokenPiece(pendingText, text))) {
            flushPending();
        }

        if (!hasPending) {
            pendingText = text;
            pendingStartMs = range.startMs;
            pendingEndMs = range.endMs;
            pendingConfidence = confidenceFromObject(obj, 1.0);
            hasPending = true;
        } else {
            pendingText += text;
            pendingEndMs = qMax(pendingEndMs, range.endMs);
            pendingConfidence = qMin(pendingConfidence, confidenceFromObject(obj, pendingConfidence));
        }
    }

    flushPending();
    return words;
}

QString detectedLanguageFromRoot(const QJsonObject& root)
{
    const QString language = root.value(QStringLiteral("language")).toString();
    if (!language.isEmpty())
        return language;
    return root.value(QStringLiteral("result")).toObject().value(QStringLiteral("language")).toString();
}

QJsonArray segmentArrayFromRoot(const QJsonObject& root)
{
    const QJsonValue segments = root.value(QStringLiteral("segments"));
    if (segments.isArray())
        return segments.toArray();
    const QJsonValue transcription = root.value(QStringLiteral("transcription"));
    if (transcription.isArray())
        return transcription.toArray();
    return {};
}

QList<speech::Segment> parseSegmentsFromDocument(const QJsonDocument& doc,
                                                 QString* detectedLanguage,
                                                 QString* error)
{
    QList<speech::Segment> segments;
    if (!doc.isObject()) {
        if (error)
            *error = QStringLiteral("JSON root is not an object");
        return segments;
    }

    const QJsonObject root = doc.object();
    if (detectedLanguage)
        *detectedLanguage = detectedLanguageFromRoot(root);

    const QJsonArray jsonSegments = segmentArrayFromRoot(root);
    for (const QJsonValue& value : jsonSegments) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();
        const TimeRange segmentRange = rangeFromObject(obj, 0, std::numeric_limits<qint64>::max(), true);
        if (!segmentRange.valid)
            continue;

        speech::Segment segment;
        segment.startMs = segmentRange.startMs;
        segment.endMs = segmentRange.endMs;
        segment.text = obj.value(QStringLiteral("text")).toString().trimmed();

        if (obj.contains(QStringLiteral("confidence"))) {
            segment.confidence = confidenceFromObject(obj, segment.confidence);
        } else if (obj.contains(QStringLiteral("avg_logprob"))) {
            double logprob = 0.0;
            if (numberValue(obj.value(QStringLiteral("avg_logprob")), &logprob))
                segment.confidence = qBound(0.0, std::exp(logprob), 1.0);
        }

        if (obj.value(QStringLiteral("words")).isArray()) {
            segment.words = wordsFromWordArray(obj.value(QStringLiteral("words")).toArray(),
                                               segment.startMs,
                                               segment.endMs);
        }
        if (segment.words.isEmpty() && obj.value(QStringLiteral("tokens")).isArray()) {
            segment.words = wordsFromTokenArray(obj.value(QStringLiteral("tokens")).toArray(),
                                                segment.startMs,
                                                segment.endMs);
        }

        if (!segment.text.isEmpty())
            segments.append(segment);
    }

    return segments;
}

bool recognizeWhisperCppFullJson(const QString& audioPath,
                                 const TranscribeRequest& req,
                                 speech::RecognizeResult* result,
                                 QString* error)
{
    const QString cliPath = QStandardPaths::findExecutable(QStringLiteral("whisper-cli"));
    if (cliPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("whisper-cli binary not found in PATH");
        return false;
    }

    QTemporaryDir outputDir;
    if (!outputDir.isValid()) {
        if (error)
            *error = QStringLiteral("failed to create temporary directory");
        return false;
    }

    const QString outputPrefix = outputDir.filePath(QStringLiteral("result"));
    QStringList args;
    args << QStringLiteral("-f") << audioPath
         << QStringLiteral("--output-json")
         << QStringLiteral("--output-json-full")
         << QStringLiteral("-of") << outputPrefix;
    if (!req.language.isEmpty() && req.language != QStringLiteral("auto"))
        args << QStringLiteral("-l") << req.language;

    QElapsedTimer timer;
    timer.start();

    QProcess proc;
    proc.start(cliPath, args);
    if (!proc.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("whisper-cli failed to start");
        return false;
    }
    if (!proc.waitForFinished(5 * 60 * 1000)) {
        proc.kill();
        if (error)
            *error = QStringLiteral("whisper-cli timed out");
        return false;
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return false;
    }

    QByteArray rawJson;
    QFile jsonFile(outputPrefix + QStringLiteral(".json"));
    if (jsonFile.open(QIODevice::ReadOnly))
        rawJson = jsonFile.readAll();
    if (rawJson.trimmed().isEmpty())
        rawJson = proc.readAllStandardOutput();
    if (rawJson.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("whisper-cli did not produce JSON output");
        return false;
    }

    QString parseError;
    QString detectedLanguage;
    const QList<speech::Segment> segments =
        WhisperTranscriber::parseWhisperJsonSegments(rawJson, &detectedLanguage, &parseError);
    if (!parseError.isEmpty()) {
        if (error)
            *error = parseError;
        return false;
    }

    result->segments = segments;
    result->detectedLanguage = detectedLanguage.isEmpty() ? req.language : detectedLanguage;
    result->processingTimeMs = timer.elapsed();
    result->success = true;
    result->error.clear();
    return true;
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
        outcome.subtitleSegments = toSubtitleSegments(outcome.raw.segments, outcome.raw.detectedLanguage);
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

    if (recognizer->name() == QStringLiteral("Whisper.cpp CLI")) {
        QString wordTimingError;
        speech::RecognizeResult wordTimingResult;
        if (recognizeWhisperCppFullJson(wavPath, req, &wordTimingResult, &wordTimingError)) {
            outcome.raw = wordTimingResult;
            outcome.track = toCaptionTrack(outcome.raw.segments);
            outcome.subtitleSegments = toSubtitleSegments(outcome.raw.segments, outcome.raw.detectedLanguage);
            outcome.success = true;
            outcome.error.clear();
            return outcome;
        }
    }

    recognizeFrom(wavPath);
    return outcome;
}

QList<speech::Segment> WhisperTranscriber::parseWhisperJsonSegments(const QByteArray& json,
                                                                    QString* detectedLanguage,
                                                                    QString* error)
{
    if (detectedLanguage)
        detectedLanguage->clear();
    if (error)
        error->clear();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || doc.isNull()) {
        if (error)
            *error = QStringLiteral("failed to parse JSON output: %1").arg(parseError.errorString());
        return {};
    }

    return parseSegmentsFromDocument(doc, detectedLanguage, error);
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

QVector<SubtitleSegment> WhisperTranscriber::toSubtitleSegments(const QList<speech::Segment>& segs,
                                                               const QString& language)
{
    QVector<SubtitleSegment> segments;
    segments.reserve(segs.size());

    for (const speech::Segment& seg : segs) {
        if (seg.text.isEmpty())
            continue;

        SubtitleSegment subtitle;
        subtitle.startTime = static_cast<double>(seg.startMs) / 1000.0;
        subtitle.endTime = static_cast<double>(seg.endMs) / 1000.0;
        subtitle.text = seg.text;
        subtitle.language = language;
        subtitle.confidence = seg.confidence;

        qint64 previousEndMs = seg.startMs;
        for (const speech::Word& sourceWord : seg.words) {
            if (sourceWord.text.trimmed().isEmpty())
                continue;
            if (sourceWord.startMs < seg.startMs || sourceWord.endMs > seg.endMs)
                continue;
            if (sourceWord.startMs < previousEndMs || sourceWord.endMs <= sourceWord.startMs)
                continue;

            SubtitleWord word;
            word.text = sourceWord.text.trimmed();
            word.startTime = static_cast<double>(sourceWord.startMs) / 1000.0;
            word.endTime = static_cast<double>(sourceWord.endMs) / 1000.0;
            subtitle.words.append(word);
            previousEndMs = sourceWord.endMs;
        }

        segments.append(subtitle);
    }

    return segments;
}

} // namespace whisper
