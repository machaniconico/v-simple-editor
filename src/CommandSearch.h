#pragma once

#include <QString>
#include <QVector>

namespace cmdsearch {

struct CommandEntry {
    QString id;
    QString title;
    QString keywords;
};

QVector<int> rankMatches(const QVector<CommandEntry>& entries, const QString& query);

} // namespace cmdsearch
