#pragma once

#include <QString>
#include <QStringList>
#include <QSize>

// ---------------------------------------------------------------------------
// TwitchStreamConfig — Sprint 20 US-TWI-1
// Twitch ライブ配信設定と ffmpeg コマンド生成ユーティリティ。
// ---------------------------------------------------------------------------

namespace twitch::stream {

enum class StreamServer {
    USWest,
    USEast,
    EU,
    Asia,
    Auto
};

struct StreamConfig {
    QString      streamKey;
    StreamServer server     = StreamServer::Auto;
    int          bitrate    = 6000;
    QSize        resolution { 1920, 1080 };
    int          framerate  = 60;
    int          audioBitrate = 160;
};

/// Returns the RTMP ingest hostname prefix for the given server region.
QString serverHost(StreamServer s);

/// Builds an ffmpeg command-line as a QStringList for streaming to Twitch.
QStringList buildFfmpegCommand(const StreamConfig &config,
                               const QString      &inputFile);

} // namespace twitch::stream
