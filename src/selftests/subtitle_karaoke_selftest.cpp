// Subtitle karaoke headless selftest.
//
// QApplication 不要。subtitlekaraoke:: の純粋 word 選択ロジックを検証する。

#include <QDebug>
#include <QString>
#include <QVector>

#include "../SubtitleKaraoke.h"

int runSubtitleKaraokeSelftest()
{
    qInfo().noquote() << "[subtitle-karaoke] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) {
        ++passed;
        qInfo().noquote() << "[subtitle-karaoke] PASS" << name;
    };
    auto fail = [&](const char* name, const QString& msg) {
        ++failed;
        qWarning().noquote() << "[subtitle-karaoke] FAIL" << name << ":" << msg;
    };
    auto check = [&](const char* name, bool ok, const QString& msg = QString()) {
        if (ok)
            pass(name);
        else
            fail(name, msg);
    };

    const QVector<SubtitleWord> words = {
        { QStringLiteral("a"), 0.0, 1.0 },
        { QStringLiteral("b"), 1.0, 2.0 },
        { QStringLiteral("c"), 2.0, 3.0 },
    };

    check("G1 t=0.5 selects word 0",
          subtitlekaraoke::activeWordIndex(words, 0.5) == 0,
          QStringLiteral("active=%1").arg(subtitlekaraoke::activeWordIndex(words, 0.5)));

    check("G2 t=1.5 selects word 1",
          subtitlekaraoke::activeWordIndex(words, 1.5) == 1,
          QStringLiteral("active=%1").arg(subtitlekaraoke::activeWordIndex(words, 1.5)));

    check("G3 t=2.9 selects word 2",
          subtitlekaraoke::activeWordIndex(words, 2.9) == 2,
          QStringLiteral("active=%1").arg(subtitlekaraoke::activeWordIndex(words, 2.9)));

    check("G4 half-open boundary t=1.0 selects word 1",
          subtitlekaraoke::activeWordIndex(words, 1.0) == 1,
          QStringLiteral("active=%1").arg(subtitlekaraoke::activeWordIndex(words, 1.0)));

    check("G5 t=3.5 after all words selects none",
          subtitlekaraoke::activeWordIndex(words, 3.5) == -1,
          QStringLiteral("active=%1").arg(subtitlekaraoke::activeWordIndex(words, 3.5)));

    check("G6 spokenWordCount uses inclusive start boundaries",
          subtitlekaraoke::spokenWordCount(words, -0.1) == 0
              && subtitlekaraoke::spokenWordCount(words, 0.0) == 1
              && subtitlekaraoke::spokenWordCount(words, 1.0) == 2
              && subtitlekaraoke::spokenWordCount(words, 5.0) == 3,
          QStringLiteral("counts=%1,%2,%3,%4")
              .arg(subtitlekaraoke::spokenWordCount(words, -0.1))
              .arg(subtitlekaraoke::spokenWordCount(words, 0.0))
              .arg(subtitlekaraoke::spokenWordCount(words, 1.0))
              .arg(subtitlekaraoke::spokenWordCount(words, 5.0)));

    {
        const QVector<SubtitleWord> empty;
        check("G7 empty words select none and count zero",
              subtitlekaraoke::activeWordIndex(empty, 0.0) == -1
                  && subtitlekaraoke::spokenWordCount(empty, 0.0) == 0,
              QStringLiteral("active=%1 count=%2")
                  .arg(subtitlekaraoke::activeWordIndex(empty, 0.0))
                  .arg(subtitlekaraoke::spokenWordCount(empty, 0.0)));
    }

    {
        const QVector<SubtitleWord> invalid = {
            { QStringLiteral("bad"), 2.0, 1.0 },
        };
        check("G8 invalid word timing is never active",
              subtitlekaraoke::activeWordIndex(invalid, 2.0) == -1,
              QStringLiteral("active=%1")
                  .arg(subtitlekaraoke::activeWordIndex(invalid, 2.0)));
    }

    qInfo().noquote() << "[subtitle-karaoke] selftest done: passed=" << passed
                      << "failed=" << failed;
    return failed == 0 ? 0 : 1;
}
