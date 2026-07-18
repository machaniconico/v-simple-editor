#pragma once
// TextBasedEdit: 文字起こし駆動編集の純粋エンジン (Premiere テキストベース編集 / Descript 相当)。
//
// QObject / QWidget を使わない純粋・決定的なロジックのみ。headless (QApplication 不要) で
// テスト可能。UI (TextBasedEditDialog) はこのエンジンを呼び出すだけにする。
//
// 前提 (v1 仮定): トランスクリプトはタイムライン(シーケンス)音声から生成され、
// caption::Clip の startMs/endMs はタイムライン時刻(ms)に対応する。よって削除対象 clip の
// [startMs,endMs) はそのままタイムライン上の削除区間になる。

#include <QString>
#include <QList>
#include <QVector>
#include <QSet>
#include <QJsonObject>
#include "CaptionTrack.h"

namespace textedit {

// タイムライン上の半開区間 [startMs, endMs)。
struct TimeRange {
    qint64 startMs = 0;
    qint64 endMs   = 0;

    qint64 durationMs() const { return endMs - startMs; }
    bool   isEmpty()    const { return endMs <= startMs; }
};

// 連結テキスト中で各 clip が占める文字範囲 [charStart, charEnd)。
// UI で単語クリック→clip 特定に使う (任意)。
struct WordSpan {
    int clipIndex = -1;
    int charStart = 0;
    int charEnd   = 0;
};

// text 部分一致 (case-insensitive) でマッチした clip index 列を返す。
// 空クエリは全 index を返す。マッチは入力順 (昇順) で重複なし。
QVector<int> search(const QList<caption::Clip>& transcript, const QString& query);

// 削除対象 index の clip の [startMs,endMs) を集め、昇順マージ済み範囲列にする。
// マージ規則:
//   1. index 範囲外 (負 / clip 数以上) は無視。
//   2. startMs でソートしてから走査。
//   3. 隣接 / 重複 / 微小ギャップ (次の startMs - 現在の endMs <= kMergeGapMs) は 1 範囲に結合。
//   4. endMs <= startMs の不正/空 clip は無視。
QVector<TimeRange> deletionRanges(const QList<caption::Clip>& transcript,
                                  const QSet<int>& deletedIndices);

// deletionRanges の補集合: [0, totalDurationMs) から削除範囲を引いた残り (タイムライン再構成の確認用)。
// totalDurationMs <= 0 のときは空。削除範囲が [0,total) をはみ出す場合は [0,total) でクランプ。
QVector<TimeRange> keptRanges(const QList<caption::Clip>& transcript,
                              const QSet<int>& deletedIndices,
                              qint64 totalDurationMs);

// 範囲列の合計長 (ms)。
qint64 totalDeletedMs(const QVector<TimeRange>& ranges);

// 全 clip の text を separator で連結 (UI のトランスクリプト表示用)。
QString concatenatedText(const QList<caption::Clip>& transcript,
                         const QString& separator = QStringLiteral(" "));

// concatenatedText と同じ連結規則で、各 clip の文字範囲 [charStart,charEnd) を返す。
// span[i].clipIndex == i。separator はスパンに含めない (clip 本文のみ)。
QVector<WordSpan> textSpans(const QList<caption::Clip>& transcript,
                            const QString& separator = QStringLiteral(" "));

// 微小ギャップ結合のしきい値 (ms)。次の startMs - 現在の endMs がこれ以下なら結合。
constexpr qint64 kMergeGapMs = 1;

} // namespace textedit
