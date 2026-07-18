#include <QDebug>
#include <QString>
#include <QStringList>

#include "../YoutubeChapterGen.h"

int runYoutubeChapterSelftest()
{
    qInfo().noquote() << "[youtube-chapter] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[youtube-chapter] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[youtube-chapter] FAIL" << name << ":" << msg; };

    // G1: M:SS formatting uses plain minutes and zero-padded seconds
    const QString oneMinuteFive = YoutubeChapterGen::formatTimestamp(65.0, false);
    const QString fiveSeconds = YoutubeChapterGen::formatTimestamp(5.0, false);
    if (oneMinuteFive == QStringLiteral("1:05")
     && fiveSeconds == QStringLiteral("0:05")) {
        pass("G1 formatTimestamp M:SS");
    } else {
        fail("G1 formatTimestamp M:SS",
             QStringLiteral("got: %1, %2").arg(oneMinuteFive, fiveSeconds));
    }

    // G2: H:MM:SS formatting uses zero-padded minutes and seconds
    const QString oneHourOneMinuteFive = YoutubeChapterGen::formatTimestamp(3665.0, true);
    if (oneHourOneMinuteFive == QStringLiteral("1:01:05")) {
        pass("G2 formatTimestamp H:MM:SS");
    } else {
        fail("G2 formatTimestamp H:MM:SS",
             QStringLiteral("got: %1").arg(oneHourOneMinuteFive));
    }

    // G3: start>0 のチャプターを *非昇順* かつ distinct な時刻で渡し、
    // (a) 1 行目に '0:00 イントロ' が挿入され、(b) module が startSec 昇順に
    // 並べ替える (後半 40s が 前半 20s の後ろに来る) ことを distinct な表示
    // 時刻 (0:20 / 0:40) で検証する。両時刻が別表示なので順序検証が vacuous
    // にならず、ソート挙動を確実に exercise する。
    QList<ChapterHighlight> highlights;
    highlights.append(ChapterHighlight{40.0, QStringLiteral("後半")});
    highlights.append(ChapterHighlight{20.0, QStringLiteral("前半")});

    const bool inputNotAscending = highlights.at(0).startSec > highlights.at(1).startSec;
    const QString chapterText = YoutubeChapterGen::generateChapterText(highlights, 120.0);
    const QString expected = QStringLiteral("0:00 イントロ\n0:20 前半\n0:40 後半");
    const QStringList lines = chapterText.split(QStringLiteral("\n"));
    if (inputNotAscending
     && chapterText == expected
     && lines.size() == 3
     && lines.at(0).startsWith(QStringLiteral("0:00"))
     && lines.at(1).startsWith(QStringLiteral("0:20"))
     && lines.at(2).startsWith(QStringLiteral("0:40"))) {
        pass("G3 generateChapterText intro and ascending sort");
    } else {
        fail("G3 generateChapterText intro/sort",
             QStringLiteral("got: %1").arg(chapterText));
    }

    qInfo().noquote().nospace() << "[youtube-chapter] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
