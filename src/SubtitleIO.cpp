#include "SubtitleIO.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QStringList>

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
static caption::Clip parseBlock(const QStringList& lines, QChar sep)
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
    const QString endStr = afterArrow.split(' ').first().trimmed();

    qint64 startMs = -1;
    qint64 endMs   = -1;
    if (sep == ',') {
        startMs = parseSrtTimestamp(startStr);
        endMs   = parseSrtTimestamp(endStr);
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
        // Normalize CRLF
        if (line.endsWith('\r'))
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

// ---------------------------------------------------------------------------
// importSrt
// ---------------------------------------------------------------------------

ImportResult importSrt(const QString& path)
{
    ImportResult result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.success = false;
        result.error   = QString("Cannot open file: %1").arg(path);
        return result;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    const QString content = in.readAll();
    file.close();

    const QList<QStringList> blocks = splitBlocks(content);

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

        const caption::Clip clip = parseBlock(lines, ',');
        if (clip.isValid())
            result.clips.append(clip);
    }

    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
// importVtt
// ---------------------------------------------------------------------------

ImportResult importVtt(const QString& path)
{
    ImportResult result;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.success = false;
        result.error   = QString("Cannot open file: %1").arg(path);
        return result;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    const QString content = in.readAll();
    file.close();

    const QList<QStringList> blocks = splitBlocks(content);

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
            result.error += "Warning: missing WEBVTT header. ";
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

        const caption::Clip clip = parseBlock(lines, '.');
        if (clip.isValid())
            result.clips.append(clip);
    }

    result.success = true;
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
