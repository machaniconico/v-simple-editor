#pragma once

#include <QString>

#include <functional>

namespace versionedsave {

using PathExists = std::function<bool(const QString &)>;

QString nextVersionedPath(const QString &currentPath, const PathExists &exists);
QString nextVersionedPath(const QString &currentPath);

} // namespace versionedsave
