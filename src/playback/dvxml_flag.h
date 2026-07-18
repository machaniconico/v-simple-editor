#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace dvxml {

inline bool enabledFromEnvValue(const QByteArray& v) { return v == QByteArrayLiteral("1"); }
inline bool enabledFromEnv() { return enabledFromEnvValue(qgetenv("VEDITOR_DV_XML")); }

} // namespace dvxml
