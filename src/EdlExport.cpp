#include "EdlExport.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QtGlobal>

#include <cmath>

namespace edl {

namespace {

// frameRate を最寄りの整数 fps へ丸める (29.97→30, 59.94→60, 23.976→24)。
// ドロップフレーム判定とタイムコードのフレーム桁幅に使う。
int nominalFps(double frameRate)
{
    if (frameRate <= 0.0)
        return 30;
    return static_cast<int>(std::lround(frameRate));
}

// この frameRate がドロップフレーム計算の対象か (29.97 / 59.94 系)。
// 整数 fps やそれ以外は常に NDF 扱いにする。
bool isDropFrameRate(double frameRate)
{
    // 29.97 ≈ 30000/1001, 59.94 ≈ 60000/1001。整数 30/60 は NDF として扱う。
    constexpr double kNtsc2997 = 30000.0 / 1001.0;
    constexpr double kNtsc5994 = 60000.0 / 1001.0;
    constexpr double kTolerance = 0.005;
    return (std::fabs(frameRate - kNtsc2997) < kTolerance)
        || (std::fabs(frameRate - kNtsc5994) < kTolerance);
}

// 1 分あたりドロップするフレーム数 (29.97→2, 59.94→4)。
int dropPerMinute(int fps)
{
    // 29.97 系 (fps≈30) は毎分 2、59.94 系 (fps≈60) は毎分 4。
    return (fps >= 48) ? 4 : 2;
}

QString sanitizedReelStem(const QString& text)
{
    QString out;
    for (const QChar c : text) {
        const char ch = c.toLatin1();
        if (ch >= 'a' && ch <= 'z') {
            out += QLatin1Char(static_cast<char>(ch - 'a' + 'A'));
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            out += QLatin1Char(ch);
        }

        if (out.size() >= 8)
            break;
    }
    return out;
}

QString normalizedReelName(const QString& text)
{
    const QString reel = sanitizedReelStem(text);
    return reel.isEmpty() ? QStringLiteral("AX") : reel;
}

QString normalizedTrackType(const QString& text)
{
    QString out;
    for (const QChar c : text.trimmed().toUpper()) {
        if (!c.isSpace())
            out += c;
        if (out.size() >= 4)
            break;
    }
    return out.isEmpty() ? QStringLiteral("V") : out;
}

QString singleLineText(QString text)
{
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.trimmed();
}

} // namespace

// ---------------------------------------------------------------------------
qint64 secToFrames(double sec, double frameRate)
{
    if (!std::isfinite(sec) || !std::isfinite(frameRate)
        || sec <= 0.0 || frameRate <= 0.0) {
        return 0;
    }
    return static_cast<qint64>(std::llround(sec * frameRate));
}

// ---------------------------------------------------------------------------
QString framesToTimecode(qint64 frames, double frameRate, bool dropFrame)
{
    if (frames < 0)
        frames = 0;

    const int fps = nominalFps(frameRate);
    const bool df = dropFrame && isDropFrameRate(frameRate);

    qint64 effectiveFrames = frames;

    if (df) {
        // 標準 SMPTE ドロップフレーム計算 (David Heidelberger アルゴリズム)。
        // 実フレーム番号 (= タイムコードが表示すべき値) へ dropCount を加算して
        // 名目 fps で割れるようにする。毎分先頭 dropCount フレーム分の番号を
        // スキップし、ただし 10 分目はスキップしない。
        const int dropCount = dropPerMinute(fps);
        const qint64 framesPerMin = static_cast<qint64>(fps) * 60;          // 名目 (1800 / 3600)
        const qint64 framesPer10Min = framesPerMin * 10 - dropCount * 9;    // 実 (17982 / 35964)

        const qint64 d = frames / framesPer10Min;   // 完了した 10 分ブロック数
        const qint64 m = frames % framesPer10Min;   // ブロック内余り (実フレーム)

        // ブロック内: 先頭 1 分 (実 framesPerMin-dropCount... ではなく) の扱いを
        // m>dropCount で分岐。各 10 分ブロックは 9 回のドロップ、ブロック内の
        // 各分 (先頭 1 分を除く) で 1 回のドロップ。
        if (m > dropCount) {
            effectiveFrames = frames
                + dropCount * 9 * d
                + dropCount * ((m - dropCount) / (framesPerMin - dropCount));
        } else {
            effectiveFrames = frames + dropCount * 9 * d;
        }
    }

    const int ff = static_cast<int>(effectiveFrames % fps);
    const qint64 totalSeconds = effectiveFrames / fps;
    const int ss = static_cast<int>(totalSeconds % 60);
    const int mm = static_cast<int>((totalSeconds / 60) % 60);
    const int hh = static_cast<int>((totalSeconds / 3600) % 24);

    const QChar sep = df ? QLatin1Char(';') : QLatin1Char(':');

    return QStringLiteral("%1:%2:%3%4%5")
        .arg(hh, 2, 10, QLatin1Char('0'))
        .arg(mm, 2, 10, QLatin1Char('0'))
        .arg(ss, 2, 10, QLatin1Char('0'))
        .arg(sep)
        .arg(ff, 2, 10, QLatin1Char('0'));
}

// ---------------------------------------------------------------------------
QString toCmx3600(const EdlDocument& doc)
{
    QString out;
    const QString title = singleLineText(doc.title).isEmpty()
        ? QStringLiteral("VEDITOR EDL")
        : singleLineText(doc.title);
    const bool dropFrame = doc.dropFrame && isDropFrameRate(doc.frameRate);
    out += QStringLiteral("TITLE: ") + title + QLatin1Char('\n');
    out += dropFrame ? QStringLiteral("FCM: DROP FRAME\n\n")
                     : QStringLiteral("FCM: NON-DROP FRAME\n\n");

    for (const EdlEvent& ev : doc.events) {
        const QString srcIn  = framesToTimecode(ev.srcInFrames,  doc.frameRate, dropFrame);
        const QString srcOut = framesToTimecode(ev.srcOutFrames, doc.frameRate, dropFrame);
        const QString recIn  = framesToTimecode(ev.recInFrames,  doc.frameRate, dropFrame);
        const QString recOut = framesToTimecode(ev.recOutFrames, doc.frameRate, dropFrame);

        const QString eventNumber =
            QStringLiteral("%1").arg(qMax(0, ev.number), 3, 10, QLatin1Char('0'));
        const QString reelField =
            normalizedReelName(ev.reel).leftJustified(8, QLatin1Char(' '));
        const QString trackField =
            normalizedTrackType(ev.trackType).leftJustified(6, QLatin1Char(' '));

        const QChar transition = ev.transition.isNull()
            ? QLatin1Char('C')
            : ev.transition.toUpper();
        QString transitionField(transition);
        if (transition != QLatin1Char('C') && ev.transitionFrames > 0) {
            transitionField += QLatin1Char(' ');
            transitionField += QStringLiteral("%1")
                .arg(ev.transitionFrames, 3, 10, QLatin1Char('0'));
        }
        transitionField = transitionField.left(8).leftJustified(8, QLatin1Char(' '));

        const QString line = eventNumber
            + QStringLiteral("  ") + reelField
            + QLatin1Char(' ') + trackField
            + transitionField
            + QLatin1Char(' ') + srcIn
            + QLatin1Char(' ') + srcOut
            + QLatin1Char(' ') + recIn
            + QLatin1Char(' ') + recOut;

        out += line;
        out += QLatin1Char('\n');

        const QString clipName = singleLineText(ev.clipName);
        if (!clipName.isEmpty()) {
            out += QStringLiteral("* FROM CLIP NAME: ") + clipName + QLatin1Char('\n');
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
EdlDocument fromClips(const QVector<ClipInfo>& clips,
                      const QString& title,
                      double frameRate,
                      bool dropFrame)
{
    EdlDocument doc;
    doc.title = title.isEmpty() ? QStringLiteral("VEDITOR EDL") : title;
    doc.frameRate = (frameRate > 0.0) ? frameRate : 29.97;
    doc.dropFrame = dropFrame;

    double timelineSec = 0.0;   // タイムライン (レコード) 上の現在位置 (秒)
    int number = 1;

    for (const ClipInfo& clip : clips) {
        // レコード側: leadInSec のギャップを進めてから、このクリップの
        // 実尺 (effectiveDuration) を載せる。
        timelineSec += qMax(0.0, clip.leadInSec);
        const double recStart = timelineSec;
        const double srcInSec = qMax(0.0, clip.inPoint);
        const double srcOutCandidate = (clip.outPoint > 0.0) ? clip.outPoint : clip.duration;
        const double srcOutSec = qMax(srcInSec, srcOutCandidate);
        const double speed = (std::isfinite(clip.speed) && clip.speed > 0.0)
            ? clip.speed
            : 1.0;
        const double recDur = qMax(0.0, (srcOutSec - srcInSec) / speed);
        const double recEnd = recStart + recDur;
        timelineSec = recEnd;

        EdlEvent ev;
        ev.number = number++;

        // reel: displayName の英数字を大文字化し最大 8 文字へ整形。空なら "AX"。
        QString reel = sanitizedReelStem(clip.displayName);
        if (reel.isEmpty())
            reel = sanitizedReelStem(QFileInfo(clip.filePath).completeBaseName());
        ev.reel = reel.isEmpty() ? QStringLiteral("AX") : reel;

        ev.trackType = QStringLiteral("V");
        ev.transition = QLatin1Char('C');
        ev.transitionFrames = 0;
        ev.clipName = !clip.displayName.isEmpty()
            ? clip.displayName
            : QFileInfo(clip.filePath).fileName();

        ev.srcInFrames  = secToFrames(srcInSec,  doc.frameRate);
        ev.srcOutFrames = secToFrames(srcOutSec, doc.frameRate);
        ev.recInFrames  = secToFrames(recStart,  doc.frameRate);
        ev.recOutFrames = secToFrames(recEnd,    doc.frameRate);

        doc.events.push_back(ev);
    }

    return doc;
}

// ---------------------------------------------------------------------------
QJsonObject toJson(const EdlDocument& doc)
{
    QJsonObject obj;
    obj[QStringLiteral("title")] = doc.title;
    obj[QStringLiteral("frameRate")] = doc.frameRate;
    obj[QStringLiteral("dropFrame")] = doc.dropFrame;

    QJsonArray events;
    for (const EdlEvent& ev : doc.events) {
        QJsonObject e;
        e[QStringLiteral("number")] = ev.number;
        e[QStringLiteral("reel")] = ev.reel;
        e[QStringLiteral("trackType")] = ev.trackType;
        e[QStringLiteral("transition")] = QString(ev.transition);
        e[QStringLiteral("transitionFrames")] = ev.transitionFrames;
        e[QStringLiteral("clipName")] = ev.clipName;
        e[QStringLiteral("srcInFrames")]  = static_cast<double>(ev.srcInFrames);
        e[QStringLiteral("srcOutFrames")] = static_cast<double>(ev.srcOutFrames);
        e[QStringLiteral("recInFrames")]  = static_cast<double>(ev.recInFrames);
        e[QStringLiteral("recOutFrames")] = static_cast<double>(ev.recOutFrames);
        events.push_back(e);
    }
    obj[QStringLiteral("events")] = events;
    return obj;
}

// ---------------------------------------------------------------------------
EdlDocument fromJson(const QJsonObject& obj)
{
    EdlDocument doc;
    if (obj.contains(QStringLiteral("title")))
        doc.title = obj.value(QStringLiteral("title")).toString(doc.title);
    if (obj.contains(QStringLiteral("frameRate")))
        doc.frameRate = obj.value(QStringLiteral("frameRate")).toDouble(doc.frameRate);
    if (obj.contains(QStringLiteral("dropFrame")))
        doc.dropFrame = obj.value(QStringLiteral("dropFrame")).toBool(doc.dropFrame);

    const QJsonArray events = obj.value(QStringLiteral("events")).toArray();
    doc.events.reserve(events.size());
    for (const QJsonValue& v : events) {
        const QJsonObject e = v.toObject();
        EdlEvent ev;
        ev.number = e.value(QStringLiteral("number")).toInt(ev.number);
        ev.reel = e.value(QStringLiteral("reel")).toString(ev.reel);
        ev.trackType = e.value(QStringLiteral("trackType")).toString(ev.trackType);
        const QString tr = e.value(QStringLiteral("transition")).toString(QStringLiteral("C"));
        ev.transition = tr.isEmpty() ? QLatin1Char('C') : tr.at(0);
        ev.transitionFrames = e.value(QStringLiteral("transitionFrames")).toInt(ev.transitionFrames);
        ev.clipName = e.value(QStringLiteral("clipName")).toString();
        ev.srcInFrames  = static_cast<qint64>(e.value(QStringLiteral("srcInFrames")).toDouble(0));
        ev.srcOutFrames = static_cast<qint64>(e.value(QStringLiteral("srcOutFrames")).toDouble(0));
        ev.recInFrames  = static_cast<qint64>(e.value(QStringLiteral("recInFrames")).toDouble(0));
        ev.recOutFrames = static_cast<qint64>(e.value(QStringLiteral("recOutFrames")).toDouble(0));
        doc.events.push_back(ev);
    }
    return doc;
}

} // namespace edl
