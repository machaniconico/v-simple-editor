#include "CommandSearch.h"

#include <QStringList>

#include <algorithm>

namespace cmdsearch {

QVector<int> rankMatches(const QVector<CommandEntry>& entries, const QString& query)
{
    const QString normalizedQuery = query.trimmed().toLower();
    QVector<int> result;
    result.reserve(entries.size());

    if (normalizedQuery.isEmpty()) {
        for (int i = 0; i < entries.size(); ++i) {
            result.append(i);
        }
        return result;
    }

    struct RankedMatch {
        int index = 0;
        int score = 0;
    };

    QVector<RankedMatch> matches;
    matches.reserve(entries.size());

    const QStringList tokens = normalizedQuery.simplified().split(' ', Qt::SkipEmptyParts);

    for (int i = 0; i < entries.size(); ++i) {
        const CommandEntry& entry = entries.at(i);
        const QString title = entry.title.toLower();
        const QString keywords = entry.keywords.toLower();
        const QString searchableText = title + QLatin1Char(' ') + keywords;

        int score = 0;
        if (title == normalizedQuery) {
            score = 5;
        } else if (title.startsWith(normalizedQuery)) {
            score = 4;
        } else if (title.contains(normalizedQuery)) {
            score = 3;
        } else if (keywords.contains(normalizedQuery)) {
            score = 2;
        } else {
            bool allTokensMatch = !tokens.isEmpty();
            for (const QString& token : tokens) {
                if (!searchableText.contains(token)) {
                    allTokensMatch = false;
                    break;
                }
            }
            if (allTokensMatch) {
                score = 1;
            }
        }

        if (score > 0) {
            matches.append({i, score});
        }
    }

    std::stable_sort(matches.begin(), matches.end(), [](const RankedMatch& lhs, const RankedMatch& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.index < rhs.index;
    });

    result.reserve(matches.size());
    for (const RankedMatch& match : matches) {
        result.append(match.index);
    }
    return result;
}

} // namespace cmdsearch
