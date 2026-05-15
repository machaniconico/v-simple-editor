#include "TwitchStreamConfig.h"

namespace twitch::stream {

// ---------------------------------------------------------------------------
// serverHost
// ---------------------------------------------------------------------------

QString serverHost(StreamServer s)
{
    switch (s) {
    case StreamServer::USWest: return QStringLiteral("live-sjc");
    case StreamServer::USEast: return QStringLiteral("live-iad");
    case StreamServer::EU:     return QStringLiteral("live-fra");
    case StreamServer::Asia:   return QStringLiteral("live-tyo");
    case StreamServer::Auto:   return QStringLiteral("live");
    }
    return QStringLiteral("live");
}

// ---------------------------------------------------------------------------
// buildFfmpegCommand
// ---------------------------------------------------------------------------

QStringList buildFfmpegCommand(const StreamConfig &config,
                               const QString      &inputFile)
{
    const QString rtmpUrl = QString("rtmp://%1.twitch.tv/app/%2")
                                .arg(serverHost(config.server), config.streamKey);

    QStringList args;
    args << QStringLiteral("ffmpeg")
         << QStringLiteral("-re")
         << QStringLiteral("-i") << inputFile
         << QStringLiteral("-c:v") << QStringLiteral("libx264")
         << QStringLiteral("-preset") << QStringLiteral("veryfast")
         << QStringLiteral("-maxrate") << QString("%1k").arg(config.bitrate)
         << QStringLiteral("-bufsize")  << QString("%1k").arg(config.bitrate * 2)
         << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
         << QStringLiteral("-g")       << QString::number(config.framerate)
         << QStringLiteral("-c:a")     << QStringLiteral("aac")
         << QStringLiteral("-b:a")     << QString("%1k").arg(config.audioBitrate)
         << QStringLiteral("-ar")      << QStringLiteral("44100")
         << QStringLiteral("-f")       << QStringLiteral("flv")
         << rtmpUrl;

    return args;
}

} // namespace twitch::stream
