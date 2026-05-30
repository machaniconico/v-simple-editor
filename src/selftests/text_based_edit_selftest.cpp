#include <QDebug>
#include <QList>
#include <QSet>
#include <QString>
#include <QVector>

#include "../CaptionTrack.h"
#include "../TextBasedEdit.h"

namespace {

caption::Clip makeClip(qint64 startMs, qint64 endMs, const QString& text,
                       const QString& actor = QString())
{
    caption::Clip c;
    c.startMs = startMs;
    c.endMs   = endMs;
    c.text    = text;
    c.actor   = actor;
    return c;
}

QString indexesToString(const QVector<int>& indexes)
{
    QString text;
    for (int i = 0; i < indexes.size(); ++i) {
        if (i > 0) {
            text += QStringLiteral(",");
        }
        text += QString::number(indexes.at(i));
    }
    return text;
}

QString rangesToString(const QVector<textedit::TimeRange>& ranges)
{
    QString text;
    for (int i = 0; i < ranges.size(); ++i) {
        if (i > 0) {
            text += QStringLiteral(",");
        }
        text += QStringLiteral("[%1,%2)")
                    .arg(ranges.at(i).startMs)
                    .arg(ranges.at(i).endMs);
    }
    return text;
}

} // namespace

int runTextBasedEditSelftest()
{
    qInfo().noquote() << "[text-based-edit] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[text-based-edit] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[text-based-edit] FAIL" << name << ":" << msg; };

    // 共通トランスクリプト: 4 clip、隣接 (gap=0) と非隣接 (gap>kMergeGapMs) を混在。
    //   c0 [0,1000)    "Hello world"
    //   c1 [1000,2000) "this is a test"   (c0 と隣接: end0==start1)
    //   c2 [3000,4000) "another Line"     (c1 と非隣接: 1000ms gap)
    //   c3 [4000,5000) "final clip"       (c2 と隣接)
    const QList<caption::Clip> transcript = {
        makeClip(0, 1000, QStringLiteral("Hello world")),
        makeClip(1000, 2000, QStringLiteral("this is a test")),
        makeClip(3000, 4000, QStringLiteral("another Line")),
        makeClip(4000, 5000, QStringLiteral("final clip"))
    };
    const qint64 kTotal = 5000;

    // G1: search 部分一致 + 空クエリ全件
    {
        const QVector<int> hit = textedit::search(transcript, QStringLiteral("test"));
        const bool hitOk = hit.size() == 1 && hit.first() == 1;

        const QVector<int> all = textedit::search(transcript, QString());
        bool allOk = all.size() == transcript.size();
        for (int i = 0; allOk && i < transcript.size(); ++i) {
            allOk = all.at(i) == i;
        }

        // trim 後に空になるクエリも全件
        const QVector<int> spaces = textedit::search(transcript, QStringLiteral("   "));
        bool spacesOk = spaces.size() == transcript.size();
        for (int i = 0; spacesOk && i < transcript.size(); ++i) {
            spacesOk = spaces.at(i) == i;
        }

        if (hitOk && allOk && spacesOk) {
            pass("G1 search partial match + empty/whitespace query returns all");
        } else {
            fail("G1 search", QStringLiteral("hit=[%1] all=[%2] spaces=[%3]")
                    .arg(indexesToString(hit))
                    .arg(indexesToString(all))
                    .arg(indexesToString(spaces)));
        }
    }

    // G2: search case-insensitive
    {
        const QVector<int> upper = textedit::search(transcript, QStringLiteral("HELLO"));
        const QVector<int> lower = textedit::search(transcript, QStringLiteral("line"));
        const bool ok = upper.size() == 1 && upper.first() == 0
                     && lower.size() == 1 && lower.first() == 2;
        if (ok) {
            pass("G2 search case-insensitive");
        } else {
            fail("G2 search case", QStringLiteral("upper=[%1] lower=[%2]")
                    .arg(indexesToString(upper))
                    .arg(indexesToString(lower)));
        }
    }

    // G3: deletionRanges 単一 index → その clip の [startMs,endMs)
    {
        const QVector<textedit::TimeRange> r =
            textedit::deletionRanges(transcript, QSet<int>{2});
        const bool ok = r.size() == 1
                     && r.first().startMs == 3000
                     && r.first().endMs == 4000;
        if (ok) {
            pass("G3 deletionRanges single index → that clip span");
        } else {
            fail("G3 deletionRanges single", rangesToString(r));
        }
    }

    // G4: deletionRanges 隣接 2 index が 1 範囲に結合 (c0 end==c1 start)
    {
        const QVector<textedit::TimeRange> r =
            textedit::deletionRanges(transcript, QSet<int>{0, 1});
        const bool ok = r.size() == 1
                     && r.first().startMs == 0
                     && r.first().endMs == 2000;
        if (ok) {
            pass("G4 deletionRanges adjacent indices merge into one range");
        } else {
            fail("G4 deletionRanges adjacent", rangesToString(r));
        }
    }

    // G5: deletionRanges 非隣接は別範囲のまま昇順 (c0 と c2: 1000ms gap)
    {
        const QVector<textedit::TimeRange> r =
            textedit::deletionRanges(transcript, QSet<int>{2, 0});
        const bool ok = r.size() == 2
                     && r.at(0).startMs == 0    && r.at(0).endMs == 1000
                     && r.at(1).startMs == 3000 && r.at(1).endMs == 4000;
        if (ok) {
            pass("G5 deletionRanges non-adjacent stay separate, ascending");
        } else {
            fail("G5 deletionRanges non-adjacent", rangesToString(r));
        }
    }

    // G6: deletionRanges 範囲外/重複 index の安全処理
    //     -1 と 99 (範囲外) は無視、重複した 3 は 1 度だけ。c3 [4000,5000)。
    {
        const QVector<textedit::TimeRange> r =
            textedit::deletionRanges(transcript, QSet<int>{-1, 99, 3});
        const bool ok = r.size() == 1
                     && r.first().startMs == 4000
                     && r.first().endMs == 5000;

        // 空 index 集合 → 空範囲
        const QVector<textedit::TimeRange> empty =
            textedit::deletionRanges(transcript, QSet<int>{});
        const bool emptyOk = empty.isEmpty();

        if (ok && emptyOk) {
            pass("G6 deletionRanges ignores out-of-range/duplicate, empty set → empty");
        } else {
            fail("G6 deletionRanges safety", QStringLiteral("r=%1 emptyCount=%2")
                    .arg(rangesToString(r))
                    .arg(empty.size()));
        }
    }

    // G7: keptRanges 補集合 (先頭/中間/末尾削除で残りが妥当)
    {
        // 先頭削除 c0 → 残り [1000,5000)
        const QVector<textedit::TimeRange> head =
            textedit::keptRanges(transcript, QSet<int>{0}, kTotal);
        const bool headOk = head.size() == 1
                         && head.first().startMs == 1000
                         && head.first().endMs == kTotal;

        // 中間削除 c1 [1000,2000) → 残り [0,1000) と [2000,5000)
        const QVector<textedit::TimeRange> mid =
            textedit::keptRanges(transcript, QSet<int>{1}, kTotal);
        const bool midOk = mid.size() == 2
                        && mid.at(0).startMs == 0    && mid.at(0).endMs == 1000
                        && mid.at(1).startMs == 2000 && mid.at(1).endMs == kTotal;

        // 末尾削除 c3 [4000,5000) → 残り [0,4000)
        const QVector<textedit::TimeRange> tail =
            textedit::keptRanges(transcript, QSet<int>{3}, kTotal);
        const bool tailOk = tail.size() == 1
                         && tail.first().startMs == 0
                         && tail.first().endMs == 4000;

        if (headOk && midOk && tailOk) {
            pass("G7 keptRanges complement for head/middle/tail deletion");
        } else {
            fail("G7 keptRanges", QStringLiteral("head=%1 mid=%2 tail=%3")
                    .arg(rangesToString(head))
                    .arg(rangesToString(mid))
                    .arg(rangesToString(tail)));
        }
    }

    // G8: totalDeletedMs マージ後範囲の合計長
    {
        // c0+c1 が結合 [0,2000) + c3 [4000,5000) = 2000 + 1000 = 3000
        const QVector<textedit::TimeRange> r =
            textedit::deletionRanges(transcript, QSet<int>{0, 1, 3});
        const qint64 total = textedit::totalDeletedMs(r);
        if (total == 3000) {
            pass("G8 totalDeletedMs sums merged range lengths");
        } else {
            fail("G8 totalDeletedMs", QStringLiteral("ranges=%1 total=%2")
                    .arg(rangesToString(r))
                    .arg(total));
        }
    }

    // G9: concatenatedText / textSpans の整合
    //     各 span [charStart,charEnd) が連結文字列上で対応 clip の text と一致。
    {
        const QString joined = textedit::concatenatedText(transcript);
        const QVector<textedit::WordSpan> spans = textedit::textSpans(transcript);

        bool ok = spans.size() == transcript.size();
        for (int i = 0; ok && i < spans.size(); ++i) {
            const textedit::WordSpan& s = spans.at(i);
            if (s.clipIndex != i || s.charStart < 0 || s.charEnd > joined.size()
                || s.charEnd < s.charStart) {
                ok = false;
                break;
            }
            const QString slice = joined.mid(s.charStart, s.charEnd - s.charStart);
            if (slice != transcript.at(i).text) {
                ok = false;
            }
        }
        // 連結文字列に separator が入っていること (先頭以外)
        const bool joinOk = joined.startsWith(QStringLiteral("Hello world"))
                         && joined.contains(QStringLiteral("Hello world this is a test"));

        if (ok && joinOk) {
            pass("G9 concatenatedText/textSpans agree (span slices == clip text)");
        } else {
            fail("G9 concat/spans", QStringLiteral("joined='%1' spanCount=%2")
                    .arg(joined)
                    .arg(spans.size()));
        }
    }

    // G10: 空トランスクリプト / 全削除の境界
    {
        const QList<caption::Clip> empty;

        const QVector<int> es = textedit::search(empty, QStringLiteral("x"));
        const QVector<textedit::TimeRange> ed =
            textedit::deletionRanges(empty, QSet<int>{0, 1});
        const QVector<textedit::TimeRange> ek =
            textedit::keptRanges(empty, QSet<int>{}, 0);
        const QString ec = textedit::concatenatedText(empty);
        const QVector<textedit::WordSpan> esp = textedit::textSpans(empty);
        const bool emptyOk = es.isEmpty() && ed.isEmpty() && ek.isEmpty()
                          && ec.isEmpty() && esp.isEmpty();

        // 全削除 → kept は空 (タイムライン全体が削除される)
        const QVector<textedit::TimeRange> allDel =
            textedit::deletionRanges(transcript, QSet<int>{0, 1, 2, 3});
        // c0+c1 結合 [0,2000)、c2+c3 結合 [3000,5000) の 2 範囲
        const bool allDelOk = allDel.size() == 2
                           && allDel.at(0).startMs == 0    && allDel.at(0).endMs == 2000
                           && allDel.at(1).startMs == 3000 && allDel.at(1).endMs == 5000;

        const QVector<textedit::TimeRange> allKept =
            textedit::keptRanges(transcript, QSet<int>{0, 1, 2, 3}, kTotal);
        // kept = [0,5000) から削除 [0,2000)+[3000,5000) を引く → [2000,3000)
        const bool allKeptOk = allKept.size() == 1
                            && allKept.first().startMs == 2000
                            && allKept.first().endMs == 3000;

        // totalDurationMs <= 0 → keptRanges 空
        const QVector<textedit::TimeRange> zeroTotal =
            textedit::keptRanges(transcript, QSet<int>{}, 0);
        const bool zeroOk = zeroTotal.isEmpty();

        if (emptyOk && allDelOk && allKeptOk && zeroOk) {
            pass("G10 empty transcript / full-deletion / zero-total boundaries");
        } else {
            fail("G10 boundaries", QStringLiteral("emptyOk=%1 allDel=%2 allKept=%3 zeroOk=%4")
                    .arg(emptyOk)
                    .arg(rangesToString(allDel))
                    .arg(rangesToString(allKept))
                    .arg(zeroOk));
        }
    }

    qInfo().noquote().nospace() << "[text-based-edit] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
