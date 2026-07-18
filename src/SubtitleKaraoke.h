#pragma once

#include <QVector>

#include "SubtitleGenerator.h"

namespace subtitlekaraoke {

// timeSec の時点で「現在発話中」の word の index を返す。
// 規則: startTime <= timeSec < endTime を満たす最初の word の index。
// 該当なし(語間/開始前/終了後)は -1。
int activeWordIndex(const QVector<SubtitleWord>& words, double timeSec);

// timeSec までに「発話を開始した」word 数 (startTime <= timeSec の個数)。
// プログレッシブ塗り用。0..words.size()。
int spokenWordCount(const QVector<SubtitleWord>& words, double timeSec);

} // namespace subtitlekaraoke
