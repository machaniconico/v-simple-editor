#pragma once

#include <QString>

namespace rcpause {

inline constexpr bool kDefaultPauseOnRightClick = false;

inline QString pauseOnRightClickKey()
{
    return QStringLiteral("preview/pauseOnRightClick");
}

inline bool resolvePauseOnRightClick(const QString& storedValue, bool defaultVal)
{
    const QString value = storedValue.trimmed().toLower();
    if (value.isEmpty())
        return defaultVal;
    if (value == QStringLiteral("true") || value == QStringLiteral("1"))
        return true;
    if (value == QStringLiteral("false") || value == QStringLiteral("0"))
        return false;
    return defaultVal;
}

} // namespace rcpause
