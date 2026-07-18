#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace hdrmatte16 {

inline bool enabledFromEnvValue(const QString& v)
{
    return v == QStringLiteral("1");
}

inline bool enabledFromEnv()
{
    return enabledFromEnvValue(QString::fromLatin1(qgetenv("VEDITOR_HDR_MATTE16")));
}

inline bool matte16Applicable(bool flagEnabled, bool hasMatte, int layerCount, bool allRgba64)
{
    return flagEnabled && hasMatte && layerCount >= 2 && allRgba64;
}

} // namespace hdrmatte16
