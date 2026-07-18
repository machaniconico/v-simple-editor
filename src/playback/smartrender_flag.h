#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace smartrender {

inline bool enabledFromEnvValue(const QString& v)
{
    return v == QStringLiteral("1");
}

inline bool enabledFromEnv()
{
    return enabledFromEnvValue(QString::fromLatin1(qgetenv("VEDITOR_SMART_RENDER")));
}

} // namespace smartrender
