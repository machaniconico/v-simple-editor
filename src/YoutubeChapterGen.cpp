#include "YoutubeChapterGen.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QList>
#include <QString>
#include <QTextStream>

QString YoutubeChapterGen::formatTimestamp(double seconds, bool useHours)
{
    int total = qMax(0, static_cast<int>(qRound(seconds)));
    if (useHours) {
        int h = total / 3600;
        int m = (total % 3600) / 60;
        int s = total % 60;
        return QString::asprintf("%d:%02d:%02d", h, m, s);
    }
    int total_m = total / 60;
    int s = total % 60;
    return QString::asprintf("%d:%02d", total_m, s);
}

QString YoutubeChapterGen::generateChapterText(
    const QList<ChapterHighlight>& highlights,
    double videoDurationSec)
{
    if (highlights.isEmpty())
        return QString();

    // YouTube は概要欄チャプターを時刻昇順で要求する (非昇順は無効扱い)。
    // 入力順に依存せず startSec で安定ソートしてから組み立てる
    // (同時刻は入力順を保持)。
    QList<ChapterHighlight> sorted = highlights;
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const ChapterHighlight& a, const ChapterHighlight& b) {
            return a.startSec < b.startSec;
        });

    bool useHours = (videoDurationSec >= 3600.0);
    if (!useHours) {
        for (const ChapterHighlight& h : sorted) {
            if (h.startSec >= 3600.0) {
                useHours = true;
                break;
            }
        }
    }

    QStringList lines;

    if (sorted.first().startSec > 0.0) {
        lines.append(formatTimestamp(0.0, useHours) + QStringLiteral(" イントロ"));
    }

    for (int i = 0; i < sorted.size(); ++i) {
        const ChapterHighlight& h = sorted.at(i);
        const QString title = h.title.isEmpty()
            ? QStringLiteral("シーン%1").arg(i + 1)
            : h.title;
        lines.append(formatTimestamp(h.startSec, useHours) + QStringLiteral(" ") + title);
    }

    return lines.join(QStringLiteral("\n"));
}

bool YoutubeChapterGen::writeChapterFile(
    const QList<ChapterHighlight>& highlights,
    const QString& outputPath,
    double videoDurationSec)
{
    const QString text = generateChapterText(highlights, videoDurationSec);

    QFileInfo fi(outputPath);
    QDir dir = fi.absoluteDir();
    if (!dir.mkpath(QStringLiteral(".")))
        return false;

    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << text;
    return true;
}
