#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace hdrexport16 {

inline bool enabledFromEnvValue(const QString& v)
{
    return v == QStringLiteral("1");
}

inline bool enabledFromEnv()
{
    return enabledFromEnvValue(QString::fromLatin1(qgetenv("VEDITOR_HDR_EXPORT16")));
}

} // namespace hdrexport16
