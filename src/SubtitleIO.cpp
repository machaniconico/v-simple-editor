#include "SubtitleIO.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QStringList>
#include <QStringConverter>

namespace subtitle {

// ---------------------------------------------------------------------------
// Timestamp helpers
// ---------------------------------------------------------------------------

QString formatSrtTimestamp(qint64 ms)
{
    const qint64 h   = ms / 3600000;
    const qint64 m   = (ms % 3600000) / 60000;
    const qint64 s   = (ms % 60000) / 1000;
    const qint64 mss = ms % 1000;
    return QString("%1:%2:%3,%4")
        .arg(h,   2, 10, QChar('0'))
        .arg(m,   2, 10, QChar('0'))
        .arg(s,   2, 10, QChar('0'))
        .arg(mss, 3, 10, QChar('0'));
}

QString formatVttTimestamp(qint64 ms)
{
    const qint64 h   = ms / 3600000;
    const qint64 m   = (ms % 3600000) / 60000;
    const qint64 s   = (ms % 60000) / 1000;
    const qint64 mss = ms % 1000;
    return QString("%1:%2:%3.%4")
        .arg(h,   2, 10, QChar('0'))
        .arg(m,   2, 10, QChar('0'))
        .arg(s,   2, 10, QChar('0'))
        .arg(mss, 3, 10, QChar('0'));
}

// Parse "HH:MM:SS,mmm"
qint64 parseSrtTimestamp(const QString& s)
{
    // Accept HH:MM:SS,mmm
    static const QRegularExpression re(
        R"(^(\d+):(\d{2}):(\d{2}),(\d{3})$)");
    const QRegularExpressionMatch m = re.match(s.trimmed());
    if (!m.hasMatch())
        return -1;
    const qint64 h   = m.captured(1).toLongLong();
    const qint64 min = m.captured(2).toLongLong();
    const qint64 sec = m.captured(3).toLongLong();
    const qint64 ms  = m.captured(4).toLongLong();
    return (h * 3600 + min * 60 + sec) * 1000 + ms;
}

// Parse "HH:MM:SS.mmm"
qint64 parseVttTimestamp(const QString& s)
{
    // Accept HH:MM:SS.mmm  or  MM:SS.mmm
    static const QRegularExpression re(
        R"(^(?:(\d+):)?(\d{2}):(\d{2})\.(\d{3})$)");
    const QRegularExpressionMatch m = re.match(s.trimmed());
    if (!m.hasMatch())
        return -1;
    const qint64 h   = m.captured(1).isEmpty() ? 0 : m.captured(1).toLongLong();
    const qint64 min = m.captured(2).toLongLong();
    const qint64 sec = m.captured(3).toLongLong();
    const qint64 ms  = m.captured(4).toLongLong();
    return (h * 3600 + min * 60 + sec) * 1000 + ms;
}

// ---------------------------------------------------------------------------
// Internal block parser
// ---------------------------------------------------------------------------

// Parse a list of non-empty lines as a single SRT/VTT cue block.
// sep: ',' for SRT, '.' for VTT
// Returns a valid Clip on success, invalid Clip on failure.
static caption::Clip parseBlock(const QStringList& lines, QChar sep, bool lenientSrtMs = false)
{
    caption::Clip clip;

    // Find the timestamp line (contains " --> ")
    int tsLine = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].contains(" --> ")) {
            tsLine = i;
            break;
        }
    }
    if (tsLine < 0)
        return clip;  // no timestamp found

    const QString& tsStr = lines[tsLine];
    const int arrowIdx = tsStr.indexOf(" --> ");
    if (arrowIdx < 0)
        return clip;

    const QString startStr = tsStr.left(arrowIdx).trimmed();
    // VTT may have cue settings after the end timestamp; take first token only
    const QString afterArrow = tsStr.mid(arrowIdx + 5).trimmed();
    const QString endStr = afterArrow.split(QRegularExpression("\\s+")).first().trimmed();

    qint64 startMs = -1;
    qint64 endMs   = -1;
    if (sep == ',') {
        // Import accepts a '.' millisecond separator; parseSrtTimestamp stays strict.
        auto lenient = [lenientSrtMs](QString t) {
            if (lenientSrtMs)
                t.replace('.', ',');
            return t;
        };
        startMs = parseSrtTimestamp(lenient(startStr));
        endMs   = parseSrtTimestamp(lenient(endStr));
    } else {
        startMs = parseVttTimestamp(startStr);
        endMs   = parseVttTimestamp(endStr);
    }

    if (startMs < 0 || endMs < 0 || endMs <= startMs)
        return clip;

    // Text: all lines after the timestamp line
    QStringList textLines;
    for (int i = tsLine + 1; i < lines.size(); ++i)
        textLines.append(lines[i]);

    if (textLines.isEmpty())
        return clip;

    clip.startMs = startMs;
    clip.endMs   = endMs;
    clip.text    = textLines.join('\n');
    return clip;
}

// Split a full file text into blocks separated by blank lines.
static QList<QStringList> splitBlocks(const QString& text)
{
    QList<QStringList> blocks;
    QStringList current;

    const QStringList lines = text.split('\n');
    for (QString line : lines) {
        // Normalize CRLF (exportSrt/exportVtt in text mode write "\r\r\n" on Windows)
        while (line.endsWith('\r'))
            line.chop(1);

        if (line.trimmed().isEmpty()) {
            if (!current.isEmpty()) {
                blocks.append(current);
                current.clear();
            }
        } else {
            current.append(line);
        }
    }
    if (!current.isEmpty())
        blocks.append(current);

    return blocks;
}

// Decode raw subtitle bytes: BOM (UTF-8 / UTF-16LE / UTF-16BE), then strict
// UTF-8, then Shift_JIS, then the system codec. The BOM is not part of the text.
static QString decodeSubtitleBytes(const QByteArray &raw, QString *encodingUsed)
{
    if (raw.startsWith("\xEF\xBB\xBF")) {
        *encodingUsed = QStringLiteral("UTF-8");
        QStringDecoder dec(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        return dec(raw.mid(3));
    }
    if (raw.startsWith("\xFF\xFE")) {
        *encodingUsed = QStringLiteral("UTF-16LE");
        QStringDecoder dec(QStringConverter::Utf16LE, QStringConverter::Flag::Stateless);
        return dec(raw.mid(2));
    }
    if (raw.startsWith("\xFE\xFF")) {
        *encodingUsed = QStringLiteral("UTF-16BE");
        QStringDecoder dec(QStringConverter::Utf16BE, QStringConverter::Flag::Stateless);
        return dec(raw.mid(2));
    }
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString strict = utf8(raw);
    if (!utf8.hasError()) {
        *encodingUsed = QStringLiteral("UTF-8");
        return strict;
    }
    // Built by name: QStringConverter::encodingForName("Shift_JIS") is empty in this build.
    QStringDecoder sjis("Shift_JIS");
    if (sjis.isValid()) {
        *encodingUsed = QStringLiteral("Shift_JIS");
        return sjis(raw);
    }
    *encodingUsed = QStringLiteral("System");
    QStringDecoder sys(QStringConverter::System);
    return sys(raw);
}

// Read a subtitle file as raw bytes and decode it; adds the fallback warning.
static bool readSubtitleFile(const QString &path, ImportResult &result, QString &content)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.success = false;
        result.error   = QString("Cannot open file: %1").arg(path);
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();

    content = decodeSubtitleBytes(raw, &result.encodingUsed);
    if (result.encodingUsed == QLatin1String("Shift_JIS") || result.encodingUsed == QLatin1String("System"))
        result.warnings << QStringLiteral("文字コードを %1 として読みました").arg(result.encodingUsed);
    return true;
}

// Cue blocks that parsed to nothing: all of them is an error, some is a warning.
static void reportSkipped(ImportResult &result, int candidates)
{
    const int skipped = candidates - int(result.clips.size());
    if (candidates > 0 && result.clips.isEmpty()) {
        result.success = false;
        result.error   = QStringLiteral("字幕ブロックを 1 つも読めませんでした (%1 ブロック)").arg(candidates);
        return;
    }
    result.success = true;
    if (skipped > 0)
        result.warnings << QStringLiteral("%1 ブロックを読み飛ばしました").arg(skipped);
}

// ---------------------------------------------------------------------------
// importSrt
// ---------------------------------------------------------------------------

ImportResult importSrt(const QString& path)
{
    ImportResult result;

    QString content;
    if (!readSubtitleFile(path, result, content))
        return result;

    const QList<QStringList> blocks = splitBlocks(content);

    int candidates = 0;
    for (const QStringList& block : blocks) {
        // A block whose first non-empty line is purely numeric is a sequence number;
        // strip it before parsing so parseBlock can find the timestamp line.
        QStringList lines = block;
        if (!lines.isEmpty()) {
            bool ok = false;
            lines.first().trimmed().toLongLong(&ok);
            if (ok)
                lines.removeFirst();
        }
        if (lines.isEmpty())
            continue;

        if (block.join('\n').contains(" --> "))
            ++candidates;
        const caption::Clip clip = parseBlock(lines, ',', true);
        if (clip.isValid())
            result.clips.append(clip);
    }

    reportSkipped(result, candidates);
    return result;
}

// ---------------------------------------------------------------------------
// importVtt
// ---------------------------------------------------------------------------

ImportResult importVtt(const QString& path)
{
    ImportResult result;

    QString content;
    if (!readSubtitleFile(path, result, content))
        return result;

    const QList<QStringList> blocks = splitBlocks(content);

    int candidates = 0;
    bool firstBlock = true;
    for (const QStringList& block : blocks) {
        if (block.isEmpty())
            continue;

        // First block: check/skip WEBVTT header
        if (firstBlock) {
            firstBlock = false;
            if (block.first().startsWith("WEBVTT"))
                continue;
            // Tolerant: no WEBVTT header — note it but continue
            result.warnings << QStringLiteral("WEBVTT ヘッダがありません");
        }

        // Skip NOTE / STYLE / REGION blocks
        const QString& firstLine = block.first();
        if (firstLine.startsWith("NOTE") ||
            firstLine.startsWith("STYLE") ||
            firstLine.startsWith("REGION"))
            continue;

        // Strip optional cue identifier line (no " --> ")
        QStringList lines = block;
        if (!lines.isEmpty() && !lines.first().contains(" --> "))
            lines.removeFirst();

        if (lines.isEmpty())
            continue;

        if (block.join('\n').contains(" --> "))
            ++candidates;
        const caption::Clip clip = parseBlock(lines, '.');
        if (clip.isValid())
            result.clips.append(clip);
    }

    reportSkipped(result, candidates);
    return result;
}

// ---------------------------------------------------------------------------
// exportSrt
// ---------------------------------------------------------------------------

bool exportSrt(const QString& path, const QList<caption::Clip>& clips)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    int index = 1;
    for (const caption::Clip& clip : clips) {
        out << index << "\r\n";
        out << formatSrtTimestamp(clip.startMs) << " --> "
            << formatSrtTimestamp(clip.endMs)   << "\r\n";
        out << clip.text << "\r\n";
        out << "\r\n";
        ++index;
    }

    file.close();
    return true;
}

// ---------------------------------------------------------------------------
// exportVtt
// ---------------------------------------------------------------------------

bool exportVtt(const QString& path, const QList<caption::Clip>& clips)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    out << "WEBVTT\r\n\r\n";

    for (const caption::Clip& clip : clips) {
        out << formatVttTimestamp(clip.startMs) << " --> "
            << formatVttTimestamp(clip.endMs)   << "\r\n";
        out << clip.text << "\r\n";
        out << "\r\n";
    }

    file.close();
    return true;
}

} // namespace subtitle
