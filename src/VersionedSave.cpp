#include "VersionedSave.h"

#include <QFileInfo>

#include <algorithm>
#include <limits>

namespace {

struct PathParts {
    QString prefix;
    QString stem;
    QString suffix;
};

PathParts splitPath(const QString &path)
{
    const qsizetype slash = std::max(path.lastIndexOf(QLatin1Char('/')),
                                     path.lastIndexOf(QLatin1Char('\\')));
    const QString prefix = slash >= 0 ? path.left(slash + 1) : QString();
    const QString fileName = slash >= 0 ? path.mid(slash + 1) : path;
    if (fileName.isEmpty())
        return {};

    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        return {prefix, fileName.left(dot), fileName.mid(dot)};
    return {prefix, fileName, QString()};
}

QString paddedNumber(qlonglong value, int width)
{
    return QString::number(value).rightJustified(width, QLatin1Char('0'));
}

} // namespace

namespace versionedsave {

QString nextVersionedPath(const QString &currentPath, const PathExists &exists)
{
    const PathParts parts = splitPath(currentPath);
    if (parts.stem.isEmpty())
        return QString();

    qsizetype digitStart = parts.stem.size();
    while (digitStart > 0 && parts.stem.at(digitStart - 1).isDigit())
        --digitStart;

    QString prefixStem;
    int width = 3;
    qlonglong nextNumber = 2;
    if (digitStart < parts.stem.size()) {
        bool ok = false;
        const qlonglong parsedNumber = parts.stem.mid(digitStart).toLongLong(&ok);
        if (!ok || parsedNumber == std::numeric_limits<qlonglong>::max())
            return QString();
        nextNumber = parsedNumber + 1;
        prefixStem = parts.stem.left(digitStart);
        width = static_cast<int>(parts.stem.size() - digitStart);
    } else {
        prefixStem = parts.stem + QStringLiteral("_v");
    }

    for (;;) {
        const QString candidate = parts.prefix
            + prefixStem
            + paddedNumber(nextNumber, width)
            + parts.suffix;
        if (!exists || !exists(candidate))
            return candidate;
        if (nextNumber == std::numeric_limits<qlonglong>::max())
            return QString();
        ++nextNumber;
    }
}

QString nextVersionedPath(const QString &currentPath)
{
    return nextVersionedPath(currentPath, [](const QString &path) {
        return QFileInfo::exists(path);
    });
}

} // namespace versionedsave
