#include "SpeechRecognizer.h"

#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QUuid>

namespace speech {

namespace {

bool looksLikeModelPath(const QString& modelId)
{
    if (modelId.isEmpty())
        return false;
    const QFileInfo fi(modelId);
    return fi.exists()
           || modelId.contains(QLatin1Char('/'))
           || modelId.contains(QLatin1Char('\\'))
           || modelId.endsWith(QStringLiteral(".bin"), Qt::CaseInsensitive);
}

void appendOpenAiSegments(const QJsonArray& segs, RecognizeResult& res)
{
    for (const QJsonValue& val : segs) {
        if (!val.isObject())
            continue;
        const QJsonObject obj = val.toObject();
        Segment seg;
        seg.startMs    = static_cast<qint64>(obj.value(QStringLiteral("start")).toDouble() * 1000.0);
        seg.endMs      = static_cast<qint64>(obj.value(QStringLiteral("end")).toDouble()   * 1000.0);
        seg.text       = obj.value(QStringLiteral("text")).toString().trimmed();
        seg.confidence = 0.95;
        if (!seg.text.isEmpty())
            res.segments << seg;
    }
}

void appendWhisperCppSegments(const QJsonArray& transcription, RecognizeResult& res)
{
    for (const QJsonValue& val : transcription) {
        if (!val.isObject())
            continue;
        const QJsonObject obj = val.toObject();
        const QJsonObject offsets = obj.value(QStringLiteral("offsets")).toObject();
        Segment seg;
        seg.startMs = static_cast<qint64>(offsets.value(QStringLiteral("from")).toDouble());
        seg.endMs   = static_cast<qint64>(offsets.value(QStringLiteral("to")).toDouble());
        seg.text    = obj.value(QStringLiteral("text")).toString().trimmed();
        seg.confidence = 0.95;
        if (!seg.text.isEmpty())
            res.segments << seg;
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────
// StubRecognizer
// ─────────────────────────────────────────────────────────────

QString StubRecognizer::name() const
{
    return QStringLiteral("Stub");
}

QStringList StubRecognizer::supportedLanguages() const
{
    return { QStringLiteral("auto"), QStringLiteral("ja"), QStringLiteral("en") };
}

bool StubRecognizer::supportsLanguage(const QString& lang) const
{
    return supportedLanguages().contains(lang);
}

RecognizeResult StubRecognizer::recognize(const RecognizeParams& params)
{
    RecognizeResult res;

    if (params.audioPath.isEmpty()) {
        res.success = false;
        res.error = QStringLiteral("audioPath is empty");
        return res;
    }

    res.success = true;
    res.detectedLanguage = params.language.isEmpty()
        ? QStringLiteral("auto")
        : params.language;
    res.processingTimeMs = 0;

    Segment s1;
    s1.startMs = 0;
    s1.endMs = 2000;
    s1.text = QStringLiteral("これは Stub 認識結果のサンプル 1 です。");
    s1.confidence = 0.9;

    Segment s2;
    s2.startMs = 2000;
    s2.endMs = 4000;
    s2.text = QStringLiteral("Stub 認識結果のサンプル 2。実 ASR エンジン未統合。");
    s2.confidence = 0.85;

    Segment s3;
    s3.startMs = 4000;
    s3.endMs = 6000;
    s3.text = QStringLiteral("Stub recognizer placeholder segment 3.");
    s3.confidence = 0.8;

    res.segments << s1 << s2 << s3;
    return res;
}

// ─────────────────────────────────────────────────────────────
// WhisperCliRecognizer
// ─────────────────────────────────────────────────────────────

bool WhisperCliRecognizer::isAvailable() const
{
    QProcess probe;
    probe.start(m_cliPath, { QStringLiteral("--help") });
    if (!probe.waitForStarted(1500)) {
        return false;
    }
    if (!probe.waitForFinished(1500)) {
        probe.kill();
        return false;
    }
    if (probe.error() == QProcess::FailedToStart) {
        return false;
    }
    const QString out = QString::fromUtf8(probe.readAllStandardOutput())
                        + QString::fromUtf8(probe.readAllStandardError());
    return probe.exitCode() == 0 && out.contains(QStringLiteral("whisper"), Qt::CaseInsensitive);
}

QStringList WhisperCliRecognizer::supportedLanguages() const
{
    return {
        QStringLiteral("auto"), QStringLiteral("ja"), QStringLiteral("en"),
        QStringLiteral("zh"),   QStringLiteral("ko"), QStringLiteral("es"),
        QStringLiteral("fr"),   QStringLiteral("de"), QStringLiteral("pt"),
        QStringLiteral("ru")
    };
}

RecognizeResult WhisperCliRecognizer::recognize(const RecognizeParams& params)
{
    RecognizeResult res;

    if (!isAvailable()) {
        res.success = false;
        res.error = QStringLiteral("whisper-cli binary not found in PATH");
        return res;
    }

    const QString lang    = params.language.isEmpty() ? QStringLiteral("auto") : params.language;
    const QString modelId = params.modelId.isEmpty()  ? QStringLiteral("base") : params.modelId;
    const QString outBase = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("veditor_whisper_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QString outJson = outBase + QStringLiteral(".json");

    QStringList args;
    args << QStringLiteral("-f") << params.audioPath
         << QStringLiteral("-oj")
         << QStringLiteral("-of") << outBase;
    if (lang != QStringLiteral("auto"))
        args << QStringLiteral("-l") << lang;
    if (looksLikeModelPath(modelId))
        args << QStringLiteral("-m") << modelId;

    QElapsedTimer timer;
    timer.start();

    QProcess proc;
    proc.start(m_cliPath, args);
    if (!proc.waitForStarted(5000)) {
        res.success = false;
        res.error = QStringLiteral("whisper-cli failed to start");
        return res;
    }
    // 5 minute timeout
    if (!proc.waitForFinished(5 * 60 * 1000)) {
        proc.kill();
        res.success = false;
        res.error = QStringLiteral("whisper-cli timed out");
        QFile::remove(outJson);
        return res;
    }

    res.processingTimeMs = timer.elapsed();

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        res.success = false;
        res.error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (res.error.isEmpty())
            res.error = QStringLiteral("whisper-cli failed");
        QFile::remove(outJson);
        return res;
    }

    QFile jsonFile(outJson);
    if (!jsonFile.open(QIODevice::ReadOnly)) {
        res.success = false;
        res.error = QStringLiteral("failed to read JSON output");
        QFile::remove(outJson);
        return res;
    }
    const QByteArray raw = jsonFile.readAll();
    jsonFile.close();
    QFile::remove(outJson);

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        res.success = false;
        res.error = QStringLiteral("failed to parse JSON output");
        return res;
    }

    const QJsonObject root = doc.object();
    appendOpenAiSegments(root.value(QStringLiteral("segments")).toArray(), res);
    if (res.segments.isEmpty())
        appendWhisperCppSegments(root.value(QStringLiteral("transcription")).toArray(), res);

    res.success = true;
    res.detectedLanguage = params.language;
    return res;
}

// ─────────────────────────────────────────────────────────────
// availableRecognizers / recognizerByName
// ─────────────────────────────────────────────────────────────

QList<QSharedPointer<Recognizer>> availableRecognizers()
{
    QList<QSharedPointer<Recognizer>> list;

    list.append(QSharedPointer<Recognizer>(new StubRecognizer()));

    auto whisper = QSharedPointer<WhisperCliRecognizer>(new WhisperCliRecognizer());
    if (whisper->isAvailable()) {
        list.append(whisper);
    }

    return list;
}

QSharedPointer<Recognizer> recognizerByName(const QString& name)
{
    const QList<QSharedPointer<Recognizer>> all = availableRecognizers();
    for (const QSharedPointer<Recognizer>& r : all) {
        if (r->name() == name)
            return r;
    }
    return QSharedPointer<Recognizer>(new StubRecognizer());
}

} // namespace speech
