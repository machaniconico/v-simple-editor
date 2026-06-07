#include "SubtitleKaraoke.h"

namespace subtitlekaraoke {

int activeWordIndex(const QVector<SubtitleWord>& words, double timeSec)
{
    for (int i = 0; i < words.size(); ++i) {
        const SubtitleWord& word = words[i];
        if (word.endTime <= word.startTime)
            continue;
        if (word.startTime <= timeSec && timeSec < word.endTime)
            return i;
    }
    return -1;
}

int spokenWordCount(const QVector<SubtitleWord>& words, double timeSec)
{
    int count = 0;
    for (const SubtitleWord& word : words) {
        if (word.startTime <= timeSec)
            ++count;
    }
    return count;
}

} // namespace subtitlekaraoke
