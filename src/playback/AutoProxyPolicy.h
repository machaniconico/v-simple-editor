#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace playback {

struct AutoProxyClip {
    QString filePath;
    int     width      = 0;
    int     height     = 0;
    QString codec;
    bool    proxyReady = false;
};

struct AutoProxyConfig {
    bool        enabled             = true;
    int         trackThreshold      = 2;
    int         heavyWidthThreshold = 2560;
    QStringList heavyCodecs         = { "av1", "hevc", "h265", "vp9", "prores" };
};

struct AutoProxyPlan {
    QStringList useProxyFor;   // heavy clips whose proxy is ready
    QStringList generateFor;   // heavy clips that need proxy generation
    bool        anyProxyActive = false;
};

class AutoProxyPolicy {
public:
    static AutoProxyPlan decide(const QVector<AutoProxyClip>& clips,
                                const AutoProxyConfig&        cfg = {});
};

} // namespace playback
