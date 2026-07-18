// TextBasedEdit: 文字起こし駆動編集の純粋エンジン実装。
// 詳細・マージ規則は src/TextBasedEdit.h を参照。

#include "TextBasedEdit.h"

#include <algorithm>
#include <limits>

namespace textedit {

QVector<int> search(const QList<caption::Clip>& transcript, const QString& query)
{
    QVector<int> result;
    const QString needle = query.trimmed();
    if (needle.isEmpty()) {
        // 空クエリは全 index。
        result.reserve(transcript.size());
        for (int i = 0; i < transcript.size(); ++i)
            result.append(i);
        return result;
    }

    for (int i = 0; i < transcript.size(); ++i) {
        if (transcript.at(i).text.contains(needle, Qt::CaseInsensitive))
            result.append(i);
    }
    return result;
}

QVector<TimeRange> deletionRanges(const QList<caption::Clip>& transcript,
                                  const QSet<int>& deletedIndices)
{
    // 1. 有効な削除対象 clip の [startMs,endMs) を収集 (範囲外 / 空 clip は無視)。
    QVector<TimeRange> ranges;
    ranges.reserve(deletedIndices.size());
    for (int idx : deletedIndices) {
        if (idx < 0 || idx >= transcript.size())
            continue;
        const caption::Clip& c = transcript.at(idx);
        if (c.endMs <= c.startMs)
            continue;
        ranges.append(TimeRange{ c.startMs, c.endMs });
    }
    if (ranges.isEmpty())
        return ranges;

    // 2. startMs 昇順 (同 start なら endMs 昇順) でソート。
    std::sort(ranges.begin(), ranges.end(),
              [](const TimeRange& a, const TimeRange& b) {
                  if (a.startMs != b.startMs)
                      return a.startMs < b.startMs;
                  return a.endMs < b.endMs;
              });

    // 3. 隣接 / 重複 / 微小ギャップ (<= kMergeGapMs) を結合。
    QVector<TimeRange> merged;
    merged.reserve(ranges.size());
    merged.append(ranges.first());
    for (int i = 1; i < ranges.size(); ++i) {
        TimeRange& last = merged.last();
        const TimeRange& cur = ranges.at(i);
        const qint64 mergeEnd =
            (last.endMs > std::numeric_limits<qint64>::max() - kMergeGapMs)
                ? std::numeric_limits<qint64>::max()
                : last.endMs + kMergeGapMs;
        if (cur.startMs <= mergeEnd) {
            // 結合: end を伸ばす (重複・包含も正しく扱う)。
            if (cur.endMs > last.endMs)
                last.endMs = cur.endMs;
        } else {
            merged.append(cur);
        }
    }
    return merged;
}

QVector<TimeRange> keptRanges(const QList<caption::Clip>& transcript,
                              const QSet<int>& deletedIndices,
                              qint64 totalDurationMs)
{
    QVector<TimeRange> kept;
    if (totalDurationMs <= 0)
        return kept;

    const QVector<TimeRange> deleted = deletionRanges(transcript, deletedIndices);

    qint64 cursor = 0;
    for (const TimeRange& d : deleted) {
        // [0,total) でクランプした削除区間。
        const qint64 ds = std::max<qint64>(0, d.startMs);
        const qint64 de = std::min<qint64>(totalDurationMs, d.endMs);
        if (de <= ds)
            continue;        // タイムライン外 (はみ出し全体) は無視。
        if (ds > cursor)
            kept.append(TimeRange{ cursor, ds });
        if (de > cursor)
            cursor = de;
    }
    if (cursor < totalDurationMs)
        kept.append(TimeRange{ cursor, totalDurationMs });

    return kept;
}

qint64 totalDeletedMs(const QVector<TimeRange>& ranges)
{
    qint64 total = 0;
    for (const TimeRange& r : ranges) {
        if (r.endMs > r.startMs)
            total += r.endMs - r.startMs;
    }
    return total;
}

QString concatenatedText(const QList<caption::Clip>& transcript,
                         const QString& separator)
{
    QString out;
    for (int i = 0; i < transcript.size(); ++i) {
        if (i > 0)
            out += separator;
        out += transcript.at(i).text;
    }
    return out;
}

QVector<WordSpan> textSpans(const QList<caption::Clip>& transcript,
                            const QString& separator)
{
    QVector<WordSpan> spans;
    spans.reserve(transcript.size());
    int cursor = 0;
    for (int i = 0; i < transcript.size(); ++i) {
        if (i > 0)
            cursor += separator.size();
        const int start = cursor;
        const int len   = transcript.at(i).text.size();
        spans.append(WordSpan{ i, start, start + len });
        cursor += len;
    }
    return spans;
}

} // namespace textedit
