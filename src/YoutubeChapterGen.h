#pragma once
#include <QList>
#include <QString>

struct ChapterHighlight {
    double startSec = 0;
    QString title;  // 空なら "シーンN" にフォールバック
};

class YoutubeChapterGen {
public:
    // 'M:SS' または 'H:MM:SS' フォーマット
    // useHours=true: '0:00:00' / 'H:MM:SS' (zero-pad min/sec)
    // useHours=false: '0:00' / 'M:SS' (plain min)
    static QString formatTimestamp(double seconds, bool useHours);

    // YouTube 互換 chapter テキスト構築。ルール:
    //   - 1 行目は必ず '0:00' 始まり (first highlight start > 0 なら '0:00 イントロ' を挿入)
    //   - videoDuration >= 3600 or any highlight start >= 3600 で H:MM:SS、else M:SS
    //   - title 空なら 'シーンN' (1-indexed)
    static QString generateChapterText(
        const QList<ChapterHighlight>& highlights,
        double videoDurationSec = 0.0);

    // chapter テキストを UTF-8 で path に書き込む。親 dir 自動作成
    static bool writeChapterFile(
        const QList<ChapterHighlight>& highlights,
        const QString& outputPath,
        double videoDurationSec = 0.0);
};
