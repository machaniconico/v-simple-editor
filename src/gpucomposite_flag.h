#pragma once

#include <QByteArray>

inline bool resolveGpuCompositeEnabled(bool envPresent,
                                       const QByteArray& envValue,
                                       bool settingsValue)
{
    return envPresent ? (envValue == "1") : settingsValue;
}
