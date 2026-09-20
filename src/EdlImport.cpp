#include "EdlImport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace edl {
// Invert the exporter's monotonic timecode mapping. This deliberately shares
// its drop-frame rules, including skipped labels, instead of duplicating them.
bool timecodeToFrames(const QString &text, double rate, bool drop, qint64 *frames)
{
    static const QRegularExpression pattern(QStringLiteral("^\\d{2}:([0-5]\\d):([0-5]\\d)[:;](\\d{2})$"));
    const auto match = pattern.match(text);
    if (!frames || !std::isfinite(rate) || rate < 1.0 || rate > 100.0
        || !match.hasMatch() || text.left(2).toInt() > 23
        || match.captured(3).toInt() >= std::lround(rate)) return false;
    QString target = text;
    target.replace(';', ':');
    qint64 low = 0, high = secToFrames(86400.0, std::round(rate)) - 1;
    // A DF day is shorter than a nominal day; find its final frame first.
    if (framesToTimecode(high, rate, drop).startsWith(QStringLiteral("00:"))) {
        qint64 lastDay = secToFrames(82800.0, rate), nextDay = high;
        while (lastDay + 1 < nextDay) {
            const qint64 mid = lastDay + (nextDay - lastDay) / 2;
            if (framesToTimecode(mid, rate, drop).startsWith(QStringLiteral("00:"))) nextDay = mid;
            else lastDay = mid;
        }
        high = lastDay;
    }
    while (low <= high) {
        const qint64 mid = low + (high - low) / 2;
        QString label = framesToTimecode(mid, rate, drop);
        label.replace(';', ':');
        const int cmp = QString::compare(label, target);
        if (cmp == 0) { *frames = mid; return true; }
        if (cmp < 0) low = mid + 1; else high = mid - 1;
    }
    return false;
}

ImportResult parseEdl(const QString &text, double frameRate)
{
    ImportResult result;
    result.document.frameRate = frameRate;
    result.document.dropFrame = false;
    if (!std::isfinite(frameRate) || frameRate < 1.0 || frameRate > 100.0) {
        result.error = QStringLiteral("フレームレートが不正です。");
        return result;
    }
    int commentEvent = -1, lineNumber = 0;
    for (QString line : text.split('\n')) {
        ++lineNumber;
        line = line.trimmed();
        if (line.startsWith(QChar(0xfeff))) line.remove(0, 1);
        if (line.isEmpty()) continue;
        auto warn = [&] { result.warnings.append(QStringLiteral("行 %1: 未対応または不正な行: %2").arg(lineNumber).arg(line)); };
        if (line.startsWith(QStringLiteral("TITLE:"))) {
            result.document.title = line.mid(6).trimmed(); continue;
        }
        if (line == QStringLiteral("FCM: DROP FRAME") || line == QStringLiteral("FCM: NON-DROP FRAME")) {
            result.document.dropFrame = line == QStringLiteral("FCM: DROP FRAME"); continue;
        }
        if (line.startsWith(QStringLiteral("* FROM CLIP NAME:")) && commentEvent >= 0) {
            result.document.events[commentEvent].clipName = line.mid(17).trimmed(); continue;
        }
        // Unsupported lines (including M2 and other comments) do not detach
        // FROM CLIP NAME from the last accepted event.
        const auto fields = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        bool numberOk = false;
        EdlEvent event;
        event.number = fields.value(0).toInt(&numberOk);
        const bool dissolve = fields.value(3) == QStringLiteral("D");
        const int offset = dissolve ? 5 : 4;
        if (!numberOk || event.number < 0 || fields.size() != offset + 4
            || (fields[2] != QStringLiteral("V") && fields[2] != QStringLiteral("A"))
            || (!dissolve && fields[3] != QStringLiteral("C"))) { warn(); continue; }
        event.reel = fields[1]; event.trackType = fields[2];
        event.transition = dissolve ? QLatin1Char('D') : QLatin1Char('C');
        bool durationOk = true;
        if (dissolve) event.transitionFrames = fields[4].toInt(&durationOk);
        qint64 *values[] = {&event.srcInFrames, &event.srcOutFrames, &event.recInFrames, &event.recOutFrames};
        bool valid = durationOk && event.transitionFrames >= 0;
        for (int i = 0; i < 4; ++i)
            valid &= timecodeToFrames(fields[offset + i], frameRate, result.document.dropFrame, values[i]);
        if (!valid || event.srcOutFrames < event.srcInFrames || event.recOutFrames < event.recInFrames) { warn(); continue; }
        result.document.events.append(event);
        commentEvent = result.document.events.size() - 1;
    }
    if (result.document.events.isEmpty()) result.error = QStringLiteral("対応する EDL イベントがありません。");
    return result;
}

void resolveClips(ImportResult &result, const QString &directory)
{
    result.videoTracks = {QVector<ClipInfo>{}};
    result.audioTracks = {QVector<ClipInfo>{}};
    result.resolvedFiles = result.unresolvedFiles = 0;
    const QDir dir(directory);
    const auto files = dir.entryInfoList(QDir::Files, QDir::Name);
    QSet<QString> resolved, missing;
    auto events = result.document.events;
    std::stable_sort(events.begin(), events.end(), [](const EdlEvent &a, const EdlEvent &b) { return a.recInFrames < b.recInFrames; });
    double videoEnd = 0.0, audioEnd = 0.0;
    for (const auto &event : events) {
        // Zero-duration C events are the outgoing reel of a CMX dissolve pair.
        if (event.recOutFrames == event.recInFrames || event.srcOutFrames == event.srcInFrames) continue;
        const bool audio = event.trackType == QStringLiteral("A");
        auto &track = audio ? result.audioTracks[0] : result.videoTracks[0];
        double &end = audio ? audioEnd : videoEnd;
        const double fps = result.document.frameRate;
        const double start = event.recInFrames / fps;
        if (start + 1e-8 < end) {
            result.warnings.append(QStringLiteral("イベント %1: 重複するレコード範囲を読み飛ばしました。").arg(event.number)); continue;
        }
        QString path;
        for (const QString &name : {event.clipName, event.reel}) {
            if (name.isEmpty()) continue;
            const QFileInfo direct(dir.absoluteFilePath(name));
            if (direct.isFile()) { path = direct.absoluteFilePath(); break; }
            QStringList matches;
            for (const auto &file : files) {
                if (file.fileName().compare(name, Qt::CaseInsensitive) == 0
                    || file.completeBaseName().compare(name, Qt::CaseInsensitive) == 0)
                    matches.append(file.absoluteFilePath());
            }
            if (matches.size() == 1) { path = matches.first(); break; }
        }
        const QString name = event.clipName.isEmpty() ? event.reel : event.clipName;
        if (path.isEmpty()) {
            path = dir.absoluteFilePath(name);
            if (!missing.contains(path)) {
                missing.insert(path);
                result.warnings.append(QStringLiteral("オフライン素材: %1").arg(path));
            }
        } else resolved.insert(path);
        ClipInfo clip;
        clip.filePath = path; // Ordinary missing media path: existing relink APIs can repair it.
        clip.displayName = name;
        clip.inPoint = event.srcInFrames / fps;
        clip.outPoint = clip.duration = event.srcOutFrames / fps;
        clip.speed = double(event.srcOutFrames - event.srcInFrames) / double(event.recOutFrames - event.recInFrames);
        clip.leadInSec = qMax(0.0, start - end);
        if (event.transition == QLatin1Char('D') && !track.isEmpty()
            && qAbs(start - end) < 1e-8) {
            clip.leadIn.type = TransitionType::CrossDissolve;
            clip.leadIn.duration = qMin(event.transitionFrames / fps, (event.recOutFrames - event.recInFrames) / fps);
            clip.leadIn.alignment = TransitionAlignment::Start;
            auto &outgoing = track.last();
            outgoing.trailOut = clip.leadIn;
            // CMX dissolves start at the incoming record IN. Preserve the cut
            // (outPoint), but reserve source handles for the outgoing overlap.
            outgoing.duration = qMax(outgoing.duration,
                outgoing.outPoint + clip.leadIn.duration * outgoing.speed);
        }
        track.append(clip);
        end = event.recOutFrames / fps;
    }
    result.resolvedFiles = resolved.size(); result.unresolvedFiles = missing.size();
    if (result.videoTracks[0].isEmpty() && result.audioTracks[0].isEmpty())
        result.error = QStringLiteral("読み込めるクリップがありません。");
}

ImportResult readEdl(const QString &filePath, double frameRate)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        ImportResult result; result.error = QStringLiteral("EDL を開けません: %1").arg(filePath); return result;
    }
    auto result = parseEdl(QString::fromUtf8(file.readAll()), frameRate);
    if (result.error.isEmpty()) resolveClips(result, QFileInfo(filePath).absolutePath());
    if (result.document.title.trimmed().isEmpty()) result.document.title = QFileInfo(filePath).completeBaseName();
    return result;
}
} // namespace edl
