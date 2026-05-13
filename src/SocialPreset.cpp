#include "SocialPreset.h"

namespace social {

// ---------------------------------------------------------------------------
// allPresets — built once, returned by value each call
// ---------------------------------------------------------------------------
QList<Preset> allPresets()
{
    static const QList<Preset> kPresets = {
        {
            QStringLiteral("instagram_reels"),
            QStringLiteral("Instagram Reels (9:16, 1080x1920)"),
            Platform::InstagramReels,
            QSize(1080, 1920),
            QStringLiteral("9:16"),
            30,
            6'000'000,
            128'000,
            QStringLiteral("h264"),
            QStringLiteral("aac"),
            QStringLiteral("mp4"),
            90,
            true
        },
        {
            QStringLiteral("instagram_feed_1_1"),
            QStringLiteral("Instagram Feed (1:1, 1080x1080)"),
            Platform::InstagramFeed11,
            QSize(1080, 1080),
            QStringLiteral("1:1"),
            30,
            5'000'000,
            128'000,
            QStringLiteral("h264"),
            QStringLiteral("aac"),
            QStringLiteral("mp4"),
            60,
            true
        },
        {
            QStringLiteral("instagram_feed_4_5"),
            QStringLiteral("Instagram Feed (4:5, 1080x1350)"),
            Platform::InstagramFeed45,
            QSize(1080, 1350),
            QStringLiteral("4:5"),
            30,
            5'000'000,
            128'000,
            QStringLiteral("h264"),
            QStringLiteral("aac"),
            QStringLiteral("mp4"),
            60,
            true
        },
        {
            QStringLiteral("tiktok"),
            QStringLiteral("TikTok (9:16, 1080x1920)"),
            Platform::TikTok,
            QSize(1080, 1920),
            QStringLiteral("9:16"),
            30,
            10'000'000,
            128'000,
            QStringLiteral("h264"),
            QStringLiteral("aac"),
            QStringLiteral("mp4"),
            180,
            true
        },
        {
            QStringLiteral("youtube_shorts"),
            QStringLiteral("YouTube Shorts (9:16, 1080x1920)"),
            Platform::YouTubeShorts,
            QSize(1080, 1920),
            QStringLiteral("9:16"),
            30,
            8'000'000,
            128'000,
            QStringLiteral("h264"),
            QStringLiteral("aac"),
            QStringLiteral("mp4"),
            60,
            true
        },
        {
            QStringLiteral("youtube_standard"),
            QStringLiteral("YouTube Standard (16:9, 1920x1080)"),
            Platform::YouTubeStandard,
            QSize(1920, 1080),
            QStringLiteral("16:9"),
            30,
            8'000'000,
            128'000,
            QStringLiteral("h264"),
            QStringLiteral("aac"),
            QStringLiteral("mp4"),
            0,
            false
        },
        {
            QStringLiteral("twitter"),
            QStringLiteral("Twitter (16:9, 1280x720)"),
            Platform::Twitter,
            QSize(1280, 720),
            QStringLiteral("16:9"),
            30,
            5'000'000,
            128'000,
            QStringLiteral("h264"),
            QStringLiteral("aac"),
            QStringLiteral("mp4"),
            140,
            false
        },
        {
            QStringLiteral("linkedin"),
            QStringLiteral("LinkedIn (16:9, 1920x1080)"),
            Platform::LinkedIn,
            QSize(1920, 1080),
            QStringLiteral("16:9"),
            30,
            5'000'000,
            128'000,
            QStringLiteral("h264"),
            QStringLiteral("aac"),
            QStringLiteral("mp4"),
            600,
            false
        },
        {
            QStringLiteral("facebook_feed"),
            QStringLiteral("Facebook Feed (16:9, 1920x1080)"),
            Platform::FacebookFeed,
            QSize(1920, 1080),
            QStringLiteral("16:9"),
            30,
            6'000'000,
            128'000,
            QStringLiteral("h264"),
            QStringLiteral("aac"),
            QStringLiteral("mp4"),
            0,
            false
        }
    };
    return kPresets;
}

// ---------------------------------------------------------------------------
// presetById — linear scan; returns empty Preset (id="") on miss
// ---------------------------------------------------------------------------
Preset presetById(const QString& id)
{
    for (const Preset& p : allPresets()) {
        if (p.id == id)
            return p;
    }
    return Preset{};
}

// ---------------------------------------------------------------------------
// presetIds — ids only
// ---------------------------------------------------------------------------
QStringList presetIds()
{
    QStringList ids;
    for (const Preset& p : allPresets())
        ids.append(p.id);
    return ids;
}

// ---------------------------------------------------------------------------
// platformDisplayName
// ---------------------------------------------------------------------------
QString platformDisplayName(Platform p)
{
    switch (p) {
    case Platform::InstagramReels:   return QStringLiteral("Instagram Reels");
    case Platform::InstagramFeed11:  return QStringLiteral("Instagram Feed 1:1");
    case Platform::InstagramFeed45:  return QStringLiteral("Instagram Feed 4:5");
    case Platform::TikTok:           return QStringLiteral("TikTok");
    case Platform::YouTubeShorts:    return QStringLiteral("YouTube Shorts");
    case Platform::YouTubeStandard:  return QStringLiteral("YouTube");
    case Platform::Twitter:          return QStringLiteral("Twitter");
    case Platform::LinkedIn:         return QStringLiteral("LinkedIn");
    case Platform::FacebookFeed:     return QStringLiteral("Facebook");
    case Platform::Custom:           return QStringLiteral("カスタム");
    }
    return QStringLiteral("カスタム");
}

// ---------------------------------------------------------------------------
// customPreset — arbitrary resolution/fps/bitrate with default codecs
// ---------------------------------------------------------------------------
Preset customPreset(const QSize& res, int fps, int vBitrate)
{
    const QString displayName = QStringLiteral("カスタム %1x%2 %3fps")
                                    .arg(res.width())
                                    .arg(res.height())
                                    .arg(fps);
    // Determine aspect ratio and vertical reframe flag
    const bool vertical = res.height() > res.width();

    return Preset{
        QStringLiteral("custom"),
        displayName,
        Platform::Custom,
        res,
        QStringLiteral("%1:%2").arg(res.width()).arg(res.height()),
        fps,
        vBitrate,
        128'000,
        QStringLiteral("h264"),
        QStringLiteral("aac"),
        QStringLiteral("mp4"),
        0,
        vertical
    };
}

} // namespace social
