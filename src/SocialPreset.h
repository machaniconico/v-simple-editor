#pragma once
#include <QString>
#include <QSize>
#include <QList>
#include <QStringList>

namespace social {

enum class Platform {
    InstagramReels,
    InstagramFeed11,
    InstagramFeed45,
    TikTok,
    YouTubeShorts,
    YouTubeStandard,
    Twitter,
    LinkedIn,
    FacebookFeed,
    Custom
};

struct Preset {
    QString  id;
    QString  displayName;
    Platform platform;
    QSize    resolution;
    QString  aspectRatio;
    int      targetFps;
    int      videoBitrateBps;
    int      audioBitrateBps;
    QString  videoCodec;
    QString  audioCodec;
    QString  containerFormat;
    int      maxDurationSec;
    bool     requiresVerticalReframe;
};

QList<Preset> allPresets();
Preset        presetById(const QString& id);
QStringList   presetIds();
QString       platformDisplayName(Platform p);
Preset        customPreset(const QSize& res, int fps, int vBitrate);

} // namespace social
