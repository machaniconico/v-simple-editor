// caption-edit: 字幕の分割 / 結合 (caption::splitClip / mergeClips と CaptionEditorDialog のボタン)。
#include "SelftestGate.h"

#include "../CaptionEditorDialog.h"
#include "../CaptionTrack.h"

#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTableWidget>

#include <iostream>
#include <string>

namespace {

caption::Clip makeClip(qint64 startMs, qint64 endMs, const QString &text, const QString &actor = QString())
{
    caption::Clip c;
    c.startMs = startMs;
    c.endMs = endMs;
    c.text = text;
    c.actor = actor;
    return c;
}

caption::Word makeWord(qint64 startMs, qint64 endMs, const QString &text)
{
    caption::Word w;
    w.startMs = startMs;
    w.endMs = endMs;
    w.text = text;
    return w;
}

std::string u8(const QString &s) { return std::string(s.toUtf8().constData()); }

void selectRows(QTableWidget *table, int first, int last)
{
    table->clearSelection();
    for (int r = first; r <= last; ++r)
        table->selectionModel()->select(table->model()->index(r, 0),
                                        QItemSelectionModel::Select | QItemSelectionModel::Rows);
}

QString rowTime(const QTableWidget *t, int row)
{
    return t->item(row, 0)->text() + QStringLiteral("-") + t->item(row, 1)->text();
}

} // namespace

int runCaptionEditSelftest()
{
    selftest::GateTally tally;

    // G1: splitClip の境界。
    {
        const caption::Clip clip = makeClip(1000, 3000, QStringLiteral("abcdefgh"), QStringLiteral("A"));
        const bool atStart = !caption::splitClip(clip, 1000).has_value();
        const bool atEnd = !caption::splitClip(clip, 3000).has_value();
        const bool below = !caption::splitClip(clip, 500).has_value();
        const bool above = !caption::splitClip(clip, 4000).has_value();
        const auto parts = caption::splitClip(clip, 2000);
        const bool inner = parts.has_value()
            && parts->first.startMs == 1000 && parts->first.endMs == 2000
            && parts->second.startMs == 2000 && parts->second.endMs == 3000;
        const bool actors = parts.has_value()
            && parts->first.actor == QStringLiteral("A") && parts->second.actor == QStringLiteral("A");
        const std::string d = std::string("atStart=") + (atStart ? "nullopt" : "value")
            + " atEnd=" + (atEnd ? "nullopt" : "value") + " below=" + (below ? "nullopt" : "value")
            + " above=" + (above ? "nullopt" : "value") + " inner=" + (inner ? "ok" : "bad")
            + " actors=" + (actors ? "dup" : "bad");
        tally.gate(1, atStart && atEnd && below && above && inner && actors, d);
    }

    // G2: words 付き / words 無しの分割 (text の前後を stderr へ)。
    {
        caption::Clip withWords = makeClip(0, 4000, QStringLiteral("hello big world today"));
        withWords.words = {makeWord(0, 900, QStringLiteral("hello")), makeWord(1000, 1900, QStringLiteral("big")),
                           makeWord(2000, 2900, QStringLiteral("world")), makeWord(3000, 3900, QStringLiteral("today"))};
        const auto wp = caption::splitClip(withWords, 2000);
        const bool wordsOk = wp.has_value()
            && wp->first.words.size() == 2 && wp->second.words.size() == 2
            && wp->first.text == QStringLiteral("hello big") && wp->second.text == QStringLiteral("world today");
        if (wp)
            std::cerr << "G2 words: before='" << u8(withWords.text) << "' after='" << u8(wp->first.text)
                      << "' | '" << u8(wp->second.text) << "'\n";

        const caption::Clip plain = makeClip(0, 4000, QStringLiteral("abcdefghij"));
        const auto pp = caption::splitClip(plain, 1000);   // 比 0.25 -> 10 * 0.25 = 2.5 -> 3 文字
        const bool ratioOk = pp.has_value() && pp->first.text == QStringLiteral("abc")
            && pp->second.text == QStringLiteral("defghij") && pp->first.words.isEmpty();
        if (pp)
            std::cerr << "G2 ratio: before='" << u8(plain.text) << "' after='" << u8(pp->first.text)
                      << "' | '" << u8(pp->second.text) << "'\n";

        // (a) 語の途中で分割: 両側の全語が側の範囲内に収まる。
        caption::Clip cross = makeClip(0, 4000, QStringLiteral("hello world"));
        cross.words = {makeWord(0, 900, QStringLiteral("hello")), makeWord(1000, 2000, QStringLiteral("world"))};
        const auto cp = caption::splitClip(cross, 1400);
        bool crossOk = cp.has_value();
        if (cp) {
            for (const caption::Word &w : cp->first.words)
                crossOk = crossOk && w.startMs >= cp->first.startMs && w.endMs <= cp->first.endMs;
            for (const caption::Word &w : cp->second.words)
                crossOk = crossOk && w.startMs >= cp->second.startMs && w.endMs <= cp->second.endMs;
            crossOk = crossOk && !cp->first.words.isEmpty() && !cp->second.words.isEmpty();
            std::cerr << "G2 cross: before='" << u8(cross.text) << "' after='" << u8(cp->first.text)
                      << "' | '" << u8(cp->second.text) << "'\n";
        }

        // (b) 最後の語より後ろで分割: char-ratio に落ちず nullopt。
        const auto tail = caption::splitClip(cross, 3000);
        const bool tailOk = !tail.has_value();
        std::cerr << "G2 tail: before='" << u8(cross.text) << "' after="
                  << (tail ? u8(tail->first.text) + " | " + u8(tail->second.text) : std::string("nullopt")) << "\n";

        // サロゲートペアを割らない (比 0.5 -> 4 * 0.5 = 2 の位置が 😀 の 2 つの UTF-16 の間)。
        const caption::Clip emoji = makeClip(0, 1000, QString::fromUtf8("a\xF0\x9F\x98\x80""b"));
        const auto ep = caption::splitClip(emoji, 500);
        bool emojiOk = ep.has_value();
        if (ep) {
            emojiOk = !ep->first.text.isEmpty() && !ep->second.text.isEmpty()
                && !ep->first.text.back().isHighSurrogate() && !ep->second.text.front().isLowSurrogate()
                && ep->first.text + ep->second.text == emoji.text;
            std::cerr << "G2 emoji: before='" << u8(emoji.text) << "' after='" << u8(ep->first.text)
                      << "' | '" << u8(ep->second.text) << "'\n";
        }
        tally.gate(2, wordsOk && ratioOk && crossOk && tailOk && emojiOk,
                   std::string("words=") + (wordsOk ? "ok" : "bad") + " ratio=" + (ratioOk ? "ok" : "bad")
                   + " cross=" + (crossOk ? "ok" : "bad") + " tail=" + (tailOk ? "ok" : "bad")
                   + " emoji=" + (emojiOk ? "ok" : "bad"));
    }

    // G3: mergeClips の条件。
    {
        caption::Clip a = makeClip(0, 1000, QStringLiteral("one"), QStringLiteral("A"));
        a.words = {makeWord(0, 900, QStringLiteral("one"))};
        caption::Clip b = makeClip(1500, 3000, QStringLiteral("two"), QStringLiteral("A"));
        b.words = {makeWord(1500, 2900, QStringLiteral("two"))};
        const auto m = caption::mergeClips(a, b);
        const bool mergedOk = m.has_value() && m->startMs == 0 && m->endMs == 3000
            && m->text == QStringLiteral("one two") && m->words.size() == 2 && m->actor == QStringLiteral("A");
        const bool touching = caption::mergeClips(a, makeClip(1000, 2000, QStringLiteral("t"), QStringLiteral("A"))).has_value();
        const bool overlap = !caption::mergeClips(a, makeClip(900, 2000, QStringLiteral("t"), QStringLiteral("A"))).has_value();
        const bool actorDiff = !caption::mergeClips(a, makeClip(1500, 2000, QStringLiteral("t"), QStringLiteral("B"))).has_value();
        tally.gate(3, mergedOk && touching && overlap && actorDiff,
                   std::string("merged=") + (mergedOk ? "ok" : "bad") + " touching=" + (touching ? "value" : "nullopt")
                   + " overlap=" + (overlap ? "nullopt" : "value") + " actorDiff=" + (actorDiff ? "nullopt" : "value"));
    }

    // ダイアログ側の準備: 3 行 (0-1000 / 1000-3000 / 5000-6000)。
    auto fill = [](CaptionEditorDialog &dlg) {
        caption::Track track;
        track.addClip(makeClip(0, 1000, QStringLiteral("first line")));
        track.addClip(makeClip(1000, 3000, QStringLiteral("second line here")));
        track.addClip(makeClip(5000, 6000, QStringLiteral("third")));
        dlg.setTrack(track);
    };

    // G4: 分割ボタン。
    {
        CaptionEditorDialog dlg;
        fill(dlg);
        auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("captionClipTable"));
        auto *splitBtn = dlg.findChild<QPushButton *>(QStringLiteral("captionSplitButton"));
        auto *splitSpin = dlg.findChild<QSpinBox *>(QStringLiteral("captionSplitMsSpin"));
        bool ok = table && splitBtn && splitSpin;
        std::string d = ok ? "" : "widget missing";
        if (ok) {
            table->selectRow(1);
            const int mid = splitSpin->value();
            const bool midOk = mid == 2000;
            splitSpin->setValue(1500);
            const bool enabled = splitBtn->isEnabled();
            const int before = table->rowCount();
            splitBtn->click();
            const int after = table->rowCount();
            const bool rowsOk = after == before + 1 && rowTime(table, 1) == QStringLiteral("1000-1500")
                && rowTime(table, 2) == QStringLiteral("1500-3000");
            ok = midOk && enabled && rowsOk;
            d = "mid=" + std::to_string(mid) + " enabled=" + (enabled ? "1" : "0") + " rows=" + std::to_string(before)
                + "->" + std::to_string(after) + " r1=" + u8(rowTime(table, 1)) + " r2=" + u8(rowTime(table, 2));
        }
        tally.gate(4, ok, d);
    }

    // G5: 結合ボタン (隣接 2 行)。
    {
        CaptionEditorDialog dlg;
        fill(dlg);
        auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("captionClipTable"));
        auto *mergeBtn = dlg.findChild<QPushButton *>(QStringLiteral("captionMergeButton"));
        bool ok = table && mergeBtn;
        std::string d = ok ? "" : "widget missing";
        if (ok) {
            selectRows(table, 0, 1);
            const bool enabled = mergeBtn->isEnabled();
            const int before = table->rowCount();
            mergeBtn->click();
            const int after = table->rowCount();
            const bool rowsOk = after == before - 1 && rowTime(table, 0) == QStringLiteral("0-3000")
                && table->item(0, 2)->text() == QStringLiteral("first line second line here");
            ok = enabled && rowsOk;
            d = "enabled=" + std::string(enabled ? "1" : "0") + " rows=" + std::to_string(before) + "->"
                + std::to_string(after) + " r0=" + u8(rowTime(table, 0)) + " text='" + u8(table->item(0, 2)->text()) + "'";
        }
        tally.gate(5, ok, d);
    }

    // G6: 負の対照 (ボタン無効 / 押しても行数不変)。
    {
        CaptionEditorDialog dlg;
        fill(dlg);
        auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("captionClipTable"));
        auto *splitBtn = dlg.findChild<QPushButton *>(QStringLiteral("captionSplitButton"));
        auto *mergeBtn = dlg.findChild<QPushButton *>(QStringLiteral("captionMergeButton"));
        auto *splitSpin = dlg.findChild<QSpinBox *>(QStringLiteral("captionSplitMsSpin"));
        bool ok = table && splitBtn && mergeBtn && splitSpin;
        std::string d = ok ? "" : "widget missing";
        if (ok) {
            const int rows = table->rowCount();
            auto probe = [&](bool &disabledAndUnchanged) {
                const bool wasEnabled = mergeBtn->isEnabled();
                mergeBtn->click();
                disabledAndUnchanged = !wasEnabled && table->rowCount() == rows;
            };
            table->clearSelection();
            bool zero = false;
            probe(zero);
            table->selectRow(0);
            bool one = false;
            probe(one);
            // 非隣接 2 行 (0 と 2)。
            table->clearSelection();
            for (int r : {0, 2})
                table->selectionModel()->select(table->model()->index(r, 0),
                                                QItemSelectionModel::Select | QItemSelectionModel::Rows);
            bool nonAdjacent = false;
            probe(nonAdjacent);
            // 分割位置が範囲外 (行 1 = 1000-3000 に対し 500 / 3000)。
            table->selectRow(1);
            splitSpin->setValue(500);
            const bool lowDisabled = !splitBtn->isEnabled();
            splitSpin->setValue(3000);
            const bool highDisabled = !splitBtn->isEnabled();
            splitBtn->click();
            const bool unchanged = table->rowCount() == rows;
            splitSpin->setValue(2000);
            const bool positive = splitBtn->isEnabled();   // 有効側が反応する対照
            ok = zero && one && nonAdjacent && lowDisabled && highDisabled && unchanged && positive;
            d = std::string("zero=") + (zero ? "1" : "0") + " one=" + (one ? "1" : "0") + " nonAdjacent="
                + (nonAdjacent ? "1" : "0") + " low=" + (lowDisabled ? "1" : "0") + " high=" + (highDisabled ? "1" : "0")
                + " unchanged=" + (unchanged ? "1" : "0") + " positive=" + (positive ? "1" : "0");
        }
        tally.gate(6, ok, d);
    }

    return tally.finish("caption-edit");
}
