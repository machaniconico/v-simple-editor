#include <QDebug>
#include <QString>
#include <QVector>

#include "../CommandSearch.h"

namespace {

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

} // namespace

int runCommandSearchSelftest()
{
    qInfo().noquote() << "[command-search] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[command-search] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[command-search] FAIL" << name << ":" << msg; };

    const QVector<cmdsearch::CommandEntry> entries = {
        { QStringLiteral("export"), QStringLiteral("動画を書き出し"), QStringLiteral("export render エクスポート 出力") },
        { QStringLiteral("cut"), QStringLiteral("自動ジャンプカット"), QStringLiteral("無音 カット silence") },
        { QStringLiteral("whisper"), QStringLiteral("動画を文字起こし"), QStringLiteral("transcribe 字幕 whisper") },
        { QStringLiteral("render_help"), QStringLiteral("レンダー設定"), QStringLiteral("動画を書き出し export quality") },
        { QStringLiteral("markers"), QStringLiteral("マーカーを追加"), QStringLiteral("chapter bookmark 目印") }
    };

    const int exportIndex = 0;
    const int whisperIndex = 2;
    const int keywordOnlyWriteIndex = 3;

    // G1: empty query
    const QVector<int> emptyResults = cmdsearch::rankMatches(entries, QString());
    bool emptyOk = emptyResults.size() == entries.size();
    for (int i = 0; emptyOk && i < entries.size(); ++i) {
        emptyOk = emptyResults.at(i) == i;
    }
    if (emptyOk) {
        pass("G1 empty query returns all indexes in original order");
    } else {
        fail("G1 empty query", QStringLiteral("indexes=[%1] expectedCount=%2")
                .arg(indexesToString(emptyResults))
                .arg(entries.size()));
    }

    // G2: title partial match
    const QVector<int> titleResults = cmdsearch::rankMatches(entries, QStringLiteral("書き出し"));
    const bool titleOk = !titleResults.isEmpty()
        && titleResults.contains(exportIndex)
        && titleResults.indexOf(exportIndex) <= 1;
    if (titleOk) {
        pass("G2 title partial match finds export near top");
    } else {
        fail("G2 title partial match", QStringLiteral("indexes=[%1] exportIndex=%2")
                .arg(indexesToString(titleResults))
                .arg(exportIndex));
    }

    // G3: keyword match
    const QVector<int> keywordResults = cmdsearch::rankMatches(entries, QStringLiteral("transcribe"));
    const bool keywordOk = keywordResults.contains(whisperIndex);
    if (keywordOk) {
        pass("G3 keyword-only query matches whisper");
    } else {
        fail("G3 keyword match", QStringLiteral("indexes=[%1] whisperIndex=%2")
                .arg(indexesToString(keywordResults))
                .arg(whisperIndex));
    }

    // G4: title exact match ranks before keyword-only match
    const QVector<int> rankResults = cmdsearch::rankMatches(entries, QStringLiteral("動画を書き出し"));
    const int exportRank = rankResults.indexOf(exportIndex);
    const int keywordOnlyRank = rankResults.indexOf(keywordOnlyWriteIndex);
    const bool rankOk = exportRank >= 0
        && keywordOnlyRank >= 0
        && exportRank < keywordOnlyRank;
    if (rankOk) {
        pass("G4 exact title match ranks before keyword-only match");
    } else {
        fail("G4 rank order", QStringLiteral("indexes=[%1] exportRank=%2 keywordOnlyRank=%3")
                .arg(indexesToString(rankResults))
                .arg(exportRank)
                .arg(keywordOnlyRank));
    }

    // G5: case-insensitive keyword match
    const QVector<int> caseResults = cmdsearch::rankMatches(entries, QStringLiteral("EXPORT"));
    const bool caseOk = caseResults.contains(exportIndex);
    if (caseOk) {
        pass("G5 case-insensitive query matches export keyword");
    } else {
        fail("G5 case-insensitive", QStringLiteral("indexes=[%1] exportIndex=%2")
                .arg(indexesToString(caseResults))
                .arg(exportIndex));
    }

    // G6: no match
    const QVector<int> noMatchResults = cmdsearch::rankMatches(entries, QStringLiteral("zzzznomatch"));
    if (noMatchResults.isEmpty()) {
        pass("G6 no-match query returns empty results");
    } else {
        fail("G6 no match", QStringLiteral("indexes=[%1]").arg(indexesToString(noMatchResults)));
    }

    qInfo().noquote().nospace() << "[command-search] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
