#include "SubtitleIO.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QStringList>
#include <QStringConverter>

#include <algorithm>

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

// A timestamp line: "start --> end" anchored per line. ASCII digits only ([0-9], no
// Unicode properties); \h is PCRE2 horizontal whitespace (tab, U+00A0, U+3000, ...).
static const QRegularExpression kTimestampLine(QStringLiteral(
    "^\\h*(?:[0-9]+:)?[0-9]{1,2}:[0-9]{2}[.,][0-9]{1,3}\\h*-->\\h*(?:[0-9]+:)?[0-9]{1,2}:[0-9]{2}[.,][0-9]{1,3}"));
// The arrow alone, used to split a timestamp line and as the loose cue-block candidate test.
static const QRegularExpression kArrow(QStringLiteral("\\h*-->\\h*"));

// Import-only timestamp parser: 1-3 fraction digits, right-padded with zeros
// (",5" = 500, ",50" = 500, ",005" = 5). SRT needs hours and accepts '.' or ','; VTT
// hours are optional and the separator is '.'. The public parsers stay strict.
static qint64 parseTimestampLenient(const QString& s, QChar sep)
{
    static const QRegularExpression srt(
        QStringLiteral("^([0-9]+):([0-9]{2}):([0-9]{2})[.,]([0-9]{1,3})$"));
    static const QRegularExpression vtt(
        QStringLiteral("^(?:([0-9]+):)?([0-9]{2}):([0-9]{2})\\.([0-9]{1,3})$"));
    const QRegularExpressionMatch m = (sep == ',' ? srt : vtt).match(s.trimmed());
    if (!m.hasMatch())
        return -1;
    const qint64 h   = m.captured(1).toLongLong();
    const qint64 min = m.captured(2).toLongLong();
    const qint64 sec = m.captured(3).toLongLong();
    const qint64 ms  = m.captured(4).leftJustified(3, '0').toLongLong();
    return (h * 3600 + min * 60 + sec) * 1000 + ms;
}

// Parse a list of non-empty lines as a single SRT/VTT cue block.
// sep: ',' for SRT, '.' for VTT
// Returns a valid Clip on success, invalid Clip on failure.
static caption::Clip parseBlock(const QStringList& lines, QChar sep)
{
    caption::Clip clip;

    // Find the timestamp line (anchored "start --> end", line by line)
    int tsLine = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (kTimestampLine.match(lines[i]).hasMatch()) {
            tsLine = i;
            break;
        }
    }
    if (tsLine < 0)
        return clip;  // no timestamp found

    const QString& tsStr = lines[tsLine];
    const QRegularExpressionMatch arrow = kArrow.match(tsStr);
    if (!arrow.hasMatch())
        return clip;

    const QString startStr = tsStr.left(arrow.capturedStart()).trimmed();
    // VTT may have cue settings after the end timestamp; take first token only
    const QString afterArrow = tsStr.mid(arrow.capturedEnd()).trimmed();
    const QString endStr = afterArrow.split(QRegularExpression("\\h+")).first().trimmed();

    const qint64 startMs = parseTimestampLenient(startStr, sep);
    const qint64 endMs   = parseTimestampLenient(endStr, sep);

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

// Decode raw subtitle bytes: BOM (UTF-8 / UTF-16LE / UTF-16BE), BOM-less UTF-16, then strict
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
    // BOM-less UTF-16: an ASCII first line has NULs on one byte parity only (a strict
    // UTF-8 / Shift_JIS read would accept those bytes silently). Decode an even length.
    {
        int n = int(qMin<qsizetype>(raw.size(), 64));
        // Only the first line: a cue's text may follow within 64 bytes (kanji such as
        // U+65E5 have a byte >= 0x80, U+3000 a NUL on the other parity). The pair that
        // holds the LF is kept.
        for (int i = 0; i < n; ++i) {
            if (raw[i] == '\n') {
                n = qMin(n, (i + 2) & ~1);
                break;
            }
        }
        int nul[2] = {0, 0};
        bool ascii[2] = {true, true};
        for (int i = 0; i < n; ++i) {
            const unsigned char c = static_cast<unsigned char>(raw[i]);
            if (c == 0)
                ++nul[i & 1];
            else if (c >= 0x80)
                ascii[i & 1] = false;
        }
        const bool le = nul[1] > 0 && nul[0] == 0 && ascii[0];
        const bool be = nul[0] > 0 && nul[1] == 0 && ascii[1];
        if (le || be) {
            *encodingUsed = QString::fromUtf8(le ? "UTF-16LE (BOM なし)" : "UTF-16BE (BOM なし)");
            QStringDecoder dec(le ? QStringConverter::Utf16LE : QStringConverter::Utf16BE,
                               QStringConverter::Flag::Stateless);
            return dec(raw.left(raw.size() & ~qsizetype(1)));
        }
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
    else if (result.encodingUsed.startsWith(QLatin1String("UTF-16"))
             && result.encodingUsed.endsWith(QString::fromUtf8("(BOM なし)"))) {
        QString w = QStringLiteral("文字コードを %1 として読みました").arg(result.encodingUsed);
        if (raw.size() & 1)
            w += QString::fromUtf8("。末尾 1 byte を無視しました");
        result.warnings << w;
    }
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

        if (std::any_of(block.begin(), block.end(), [](const QString &l) { return l.contains(kArrow); }))
            ++candidates;
        const caption::Clip clip = parseBlock(lines, ',');
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

        // Strip optional cue identifier line (no arrow)
        QStringList lines = block;
        if (!lines.isEmpty() && !lines.first().contains(kArrow))
            lines.removeFirst();

        if (lines.isEmpty())
            continue;

        if (std::any_of(block.begin(), block.end(), [](const QString &l) { return l.contains(kArrow); }))
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
