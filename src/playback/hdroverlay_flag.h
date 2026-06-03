#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace hdroverlay {

inline QByteArray enabledFromEnvValue()
{
    return qgetenv("VEDITOR_HDR_OVERLAY");
}

inline bool enabledFromEnv()
{
    return enabledFromEnvValue() == QByteArrayLiteral("1");
}

inline bool wantRgba64(bool flag, bool clipIsHdr)
{
    return flag && clipIsHdr;
}

} // namespace hdroverlay
