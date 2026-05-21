#pragma once

// ===========================================================================
// libavcore::Decode — Qt-free in-process media decoder.
//
// Shared decoder for callers migrating away from QProcess(ffmpeg.exe). This
// header mirrors Probe.h/Concat.h style: pure C++/libav, ZERO Qt dependencies,
// std::optional<std::string> errors, and internal RAII cleanup.
//
// MediaDecoder demuxes one input, opens the first video stream (required) and
// optionally the first audio stream. nextVideoFrame()/nextAudioFrame() return
// borrowed AVFrame pointers valid until the next call for the same stream,
// seek(), open(), or destruction.
//
// Operational note: if only video is needed, call open(path, false) so audio
// packets are discarded immediately. If both streams are needed, consume
// nextVideoFrame() and nextAudioFrame() in alternation; packets for the other
// opened stream are queued internally until consumed.
// ===========================================================================

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

namespace libavcore {

struct VideoStreamProps {
    int width = 0;
    int height = 0;
    AVRational frameRate = {0, 1};
    AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;
    AVRational timeBase = {0, 1};
    int64_t durationUs = 0;
    std::string codecName;
};

struct AudioStreamProps {
    AudioStreamProps();
    AudioStreamProps(const AudioStreamProps& other);
    AudioStreamProps& operator=(const AudioStreamProps& other);
    AudioStreamProps(AudioStreamProps&& other) noexcept;
    AudioStreamProps& operator=(AudioStreamProps&& other) noexcept;
    ~AudioStreamProps();

    int sampleRate = 0;
    int channels = 0;
    AVChannelLayout channelLayout = {};
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_NONE;
    AVRational timeBase = {0, 1};
    std::string codecName;
};

class MediaDecoder {
public:
    MediaDecoder();
    ~MediaDecoder();

    MediaDecoder(const MediaDecoder&) = delete;
    MediaDecoder& operator=(const MediaDecoder&) = delete;

    // Open input and decoder contexts. Video is required; audio is optional.
    // Passing wantAudio=false ignores the audio stream and never queues audio
    // packets while reading.
    std::optional<std::string> open(const std::string& path,
                                    bool wantAudio = true);

    bool hasVideo() const { return m_videoCtx != nullptr; }
    bool hasAudio() const { return m_audioCtx != nullptr; }

    // Valid after a successful open(). audioProps() is meaningful only when
    // hasAudio() is true.
    VideoStreamProps videoProps() const { return m_videoProps; }
    AudioStreamProps audioProps() const { return m_audioProps; }

    // Decode the next frame for the requested stream. The returned AVFrame is
    // borrowed and owned by MediaDecoder; copy or ref it if it must outlive the
    // next call for that stream. Returns nullptr on EOF, missing stream, or a
    // fatal decode error.
    AVFrame* nextVideoFrame();
    AVFrame* nextAudioFrame();

    // Seek to the keyframe at or before seconds using the video stream time
    // base, then flush both decoders and clear queued packets/frames. The next
    // returned video frame may be earlier than seconds; callers that require
    // exact alignment must decode-and-discard frames whose pts is before the
    // target timestamp.
    std::optional<std::string> seek(double seconds);

    bool videoEnded() const { return m_videoEnded; }
    bool audioEnded() const { return m_audioEnded; }

private:
    std::optional<std::string> openDecoderForStream(int streamIndex,
                                                    AVCodecContext** outCtx,
                                                    const char* label);
    AVFrame* nextFrame(bool video);
    bool sendPacketToDecoder(AVCodecContext* ctx, AVPacket* pkt, bool video);
    bool queuePacket(std::deque<AVPacket*>& queue, const AVPacket* pkt);
    void dispatchPacketToQueue(const AVPacket* pkt);
    void clearPacketQueue(std::deque<AVPacket*>& queue);
    void resetDecodeStateAfterSeek();
    void resetProps();
    void releaseAll();

    AVFormatContext* m_format = nullptr;
    AVCodecContext* m_videoCtx = nullptr;
    AVCodecContext* m_audioCtx = nullptr;
    AVPacket* m_readPacket = nullptr;
    AVFrame* m_videoFrame = nullptr;
    AVFrame* m_audioFrame = nullptr;

    std::deque<AVPacket*> m_videoPackets;
    std::deque<AVPacket*> m_audioPackets;

    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;
    bool m_inputEnded = true;
    bool m_videoFlushSent = false;
    bool m_audioFlushSent = false;
    bool m_videoEnded = true;
    bool m_audioEnded = true;

    VideoStreamProps m_videoProps;
    AudioStreamProps m_audioProps;
};

} // namespace libavcore
