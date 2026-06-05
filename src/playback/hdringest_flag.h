#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

// Stage10 — ingest-time ColorMeta auto-population gate.
//
// When OFF (default), Timeline::addClip leaves the freshly-created clip's
// colorMeta at its SDR default exactly as before, so import behaviour and
// ProjectFile serialization stay byte-identical. When ON, the addClip probe
// derives per-clip ColorMeta from the source codecpar (primaries / transfer /
// bit depth), which is the load-bearing link that makes real HDR sources
// activate the Stage1–9b HDR pipeline instead of being treated as SDR.
namespace hdringest {

inline bool enabledFromEnvValue(const QString& v)
{
    return v == QStringLiteral("1");
}

inline bool enabledFromEnv()
{
    return enabledFromEnvValue(QString::fromLatin1(qgetenv("VEDITOR_HDR_INGEST")));
}

// Diagnostic trace gate (VEDITOR_HDR_TRACE) — independent of the ingest gate so
// the per-clip ColorMeta and per-frame branch decisions can be observed even
// while diagnosing why the pipeline stays dormant.
inline bool traceEnabledFromEnvValue(const QString& v)
{
    return v == QStringLiteral("1");
}

inline bool traceEnabledFromEnv()
{
    return traceEnabledFromEnvValue(QString::fromLatin1(qgetenv("VEDITOR_HDR_TRACE")));
}

} // namespace hdringest
