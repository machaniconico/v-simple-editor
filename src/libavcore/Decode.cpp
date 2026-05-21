#include "Decode.h"

#include <cmath>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace libavcore {

namespace {

std::string ffmpegErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, buf, sizeof(buf)) < 0) {
        return std::to_string(err);
    }
    return std::string(buf);
}

bool validRational(AVRational r)
{
    return r.num > 0 && r.den > 0;
}

std::string codecNameFor(const AVCodecParameters* par)
{
    if (!par) return std::string();
    const char* name = avcodec_get_name(par->codec_id);
    return name ? std::string(name) : std::string();
}

int64_t streamDurationUs(const AVFormatContext* format, const AVStream* stream)
{
    if (stream && stream->duration != AV_NOPTS_VALUE && stream->duration >= 0
        && validRational(stream->time_base)) {
        const AVRational usTimeBase = {1, AV_TIME_BASE};
        return av_rescale_q(stream->duration, stream->time_base,
                            usTimeBase);
    }
    if (format && format->duration != AV_NOPTS_VALUE
        && format->duration >= 0) {
        return static_cast<int64_t>(format->duration);
    }
    return 0;
}

AVRational bestFrameRate(AVFormatContext* format, AVStream* stream)
{
    if (!stream) return AVRational{0, 1};

    const AVRational guessed = av_guess_frame_rate(format, stream, nullptr);
    if (validRational(guessed)) return guessed;
    if (validRational(stream->avg_frame_rate)) return stream->avg_frame_rate;
    if (validRational(stream->r_frame_rate)) return stream->r_frame_rate;
    return AVRational{0, 1};
}

} // namespace

MediaDecoder::MediaDecoder() = default;

MediaDecoder::~MediaDecoder()
{
    releaseAll();
}

AudioStreamProps::AudioStreamProps() = default;

AudioStreamProps::AudioStreamProps(const AudioStreamProps& other)
{
    *this = other;
}

AudioStreamProps& AudioStreamProps::operator=(const AudioStreamProps& other)
{
    if (this == &other) return *this;

    av_channel_layout_uninit(&channelLayout);
    channelLayout = AVChannelLayout{};

    sampleRate = other.sampleRate;
    channels = other.channels;
    sampleFormat = other.sampleFormat;
    timeBase = other.timeBase;
    codecName = other.codecName;

    if (other.channelLayout.nb_channels > 0) {
        av_channel_layout_copy(&channelLayout, &other.channelLayout);
    }

    return *this;
}

AudioStreamProps::AudioStreamProps(AudioStreamProps&& other) noexcept
{
    *this = std::move(other);
}

AudioStreamProps& AudioStreamProps::operator=(AudioStreamProps&& other) noexcept
{
    if (this == &other) return *this;

    av_channel_layout_uninit(&channelLayout);

    sampleRate = other.sampleRate;
    channels = other.channels;
    channelLayout = other.channelLayout;
    sampleFormat = other.sampleFormat;
    timeBase = other.timeBase;
    codecName = std::move(other.codecName);

    other.sampleRate = 0;
    other.channels = 0;
    other.channelLayout = AVChannelLayout{};
    other.sampleFormat = AV_SAMPLE_FMT_NONE;
    other.timeBase = AVRational{0, 1};

    return *this;
}

AudioStreamProps::~AudioStreamProps()
{
    av_channel_layout_uninit(&channelLayout);
}

void MediaDecoder::clearPacketQueue(std::deque<AVPacket*>& queue)
{
    for (AVPacket* pkt : queue) {
        av_packet_free(&pkt);
    }
    queue.clear();
}

void MediaDecoder::resetProps()
{
    m_videoProps = VideoStreamProps{};
    m_audioProps = AudioStreamProps{};
}

void MediaDecoder::releaseAll()
{
    clearPacketQueue(m_videoPackets);
    clearPacketQueue(m_audioPackets);

    if (m_readPacket) { av_packet_free(&m_readPacket); }
    if (m_videoFrame) { av_frame_free(&m_videoFrame); }
    if (m_audioFrame) { av_frame_free(&m_audioFrame); }
    if (m_videoCtx) { avcodec_free_context(&m_videoCtx); }
    if (m_audioCtx) { avcodec_free_context(&m_audioCtx); }
    if (m_format) { avformat_close_input(&m_format); }

    resetProps();

    m_videoStreamIndex = -1;
    m_audioStreamIndex = -1;
    m_inputEnded = true;
    m_videoFlushSent = false;
    m_audioFlushSent = false;
    m_videoEnded = true;
    m_audioEnded = true;
}

std::optional<std::string> MediaDecoder::openDecoderForStream(
    int streamIndex,
    AVCodecContext** outCtx,
    const char* label)
{
    if (!m_format || !outCtx || streamIndex < 0
        || static_cast<unsigned>(streamIndex) >= m_format->nb_streams) {
        return std::string("MediaDecoder: invalid ") + label + " stream";
    }

    AVStream* stream = m_format->streams[streamIndex];
    if (!stream || !stream->codecpar) {
        return std::string("MediaDecoder: missing codec parameters for ")
               + label + " stream";
    }

    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        return std::string("MediaDecoder: no decoder for ") + label
               + " codec '" + codecNameFor(stream->codecpar) + "'";
    }

    AVCodecContext* ctx = avcodec_alloc_context3(decoder);
    if (!ctx) {
        return std::string("MediaDecoder: failed to allocate ") + label
               + " decoder context";
    }

    int rc = avcodec_parameters_to_context(ctx, stream->codecpar);
    if (rc < 0) {
        avcodec_free_context(&ctx);
        return std::string("MediaDecoder: failed to copy ") + label
               + " codec parameters: " + ffmpegErrorString(rc);
    }
    ctx->pkt_timebase = stream->time_base;

    rc = avcodec_open2(ctx, decoder, nullptr);
    if (rc < 0) {
        avcodec_free_context(&ctx);
        return std::string("MediaDecoder: failed to open ") + label
               + " decoder: " + ffmpegErrorString(rc);
    }

    *outCtx = ctx;
    return std::nullopt;
}

std::optional<std::string> MediaDecoder::open(const std::string& path,
                                              bool wantAudio)
{
    releaseAll();

    if (path.empty()) {
        return std::string("MediaDecoder: input path is empty");
    }

    int rc = avformat_open_input(&m_format, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        releaseAll();
        return std::string("MediaDecoder: cannot open input '") + path
               + "': " + ffmpegErrorString(rc);
    }

    rc = avformat_find_stream_info(m_format, nullptr);
    if (rc < 0) {
        releaseAll();
        return std::string("MediaDecoder: cannot read stream info for '")
               + path + "': " + ffmpegErrorString(rc);
    }

    for (unsigned i = 0; i < m_format->nb_streams; ++i) {
        AVStream* stream = m_format->streams[i];
        if (!stream || !stream->codecpar) continue;

        if (m_videoStreamIndex < 0
            && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = static_cast<int>(i);
        } else if (wantAudio && m_audioStreamIndex < 0
                   && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            m_audioStreamIndex = static_cast<int>(i);
        }
    }

    if (m_videoStreamIndex < 0) {
        releaseAll();
        return std::string("MediaDecoder: no video stream in input '")
               + path + "'";
    }

    if (auto err = openDecoderForStream(m_videoStreamIndex, &m_videoCtx,
                                        "video")) {
        releaseAll();
        return err;
    }

    if (wantAudio && m_audioStreamIndex >= 0) {
        if (auto err = openDecoderForStream(m_audioStreamIndex, &m_audioCtx,
                                            "audio")) {
            releaseAll();
            return err;
        }
    } else {
        m_audioStreamIndex = -1;
    }

    m_readPacket = av_packet_alloc();
    m_videoFrame = av_frame_alloc();
    if (m_audioCtx) {
        m_audioFrame = av_frame_alloc();
    }
    if (!m_readPacket || !m_videoFrame || (m_audioCtx && !m_audioFrame)) {
        releaseAll();
        return std::string("MediaDecoder: failed to allocate decode buffers");
    }

    AVStream* videoStream = m_format->streams[m_videoStreamIndex];
    m_videoProps.width = m_videoCtx->width;
    m_videoProps.height = m_videoCtx->height;
    m_videoProps.frameRate = bestFrameRate(m_format, videoStream);
    m_videoProps.pixelFormat = m_videoCtx->pix_fmt;
    m_videoProps.timeBase = videoStream->time_base;
    m_videoProps.durationUs = streamDurationUs(m_format, videoStream);
    m_videoProps.codecName = codecNameFor(videoStream->codecpar);

    if (m_audioCtx) {
        AVStream* audioStream = m_format->streams[m_audioStreamIndex];
        m_audioProps.sampleRate = m_audioCtx->sample_rate;
        m_audioProps.channels = m_audioCtx->ch_layout.nb_channels;
        m_audioProps.sampleFormat = m_audioCtx->sample_fmt;
        m_audioProps.timeBase = audioStream->time_base;
        m_audioProps.codecName = codecNameFor(audioStream->codecpar);

        if (m_audioCtx->ch_layout.nb_channels > 0) {
            rc = av_channel_layout_copy(&m_audioProps.channelLayout,
                                        &m_audioCtx->ch_layout);
            if (rc < 0) {
                releaseAll();
                return std::string("MediaDecoder: failed to copy audio "
                                   "channel layout: ")
                       + ffmpegErrorString(rc);
            }
        }
    }

    m_inputEnded = false;
    m_videoFlushSent = false;
    m_audioFlushSent = (m_audioCtx == nullptr);
    m_videoEnded = false;
    m_audioEnded = (m_audioCtx == nullptr);

    return std::nullopt;
}

bool MediaDecoder::queuePacket(std::deque<AVPacket*>& queue,
                               const AVPacket* pkt)
{
    AVPacket* queued = av_packet_alloc();
    if (!queued) return false;

    const int rc = av_packet_ref(queued, pkt);
    if (rc < 0) {
        av_packet_free(&queued);
        return false;
    }

    queue.push_back(queued);
    return true;
}

void MediaDecoder::dispatchPacketToQueue(const AVPacket* pkt)
{
    if (!pkt) return;
    if (pkt->stream_index == m_videoStreamIndex && m_videoCtx) {
        if (!queuePacket(m_videoPackets, pkt)) {
            m_videoEnded = true;
        }
    } else if (pkt->stream_index == m_audioStreamIndex && m_audioCtx) {
        if (!queuePacket(m_audioPackets, pkt)) {
            m_audioEnded = true;
        }
    }
}

bool MediaDecoder::sendPacketToDecoder(AVCodecContext* ctx,
                                       AVPacket* pkt,
                                       bool video)
{
    if (!ctx || !pkt) return false;

    const int rc = avcodec_send_packet(ctx, pkt);
    if (rc == 0) return true;
    if (rc == AVERROR_EOF) {
        if (video) {
            m_videoEnded = true;
        } else {
            m_audioEnded = true;
        }
    }
    return false;
}

AVFrame* MediaDecoder::nextFrame(bool video)
{
    AVCodecContext* ctx = video ? m_videoCtx : m_audioCtx;
    AVFrame* frame = video ? m_videoFrame : m_audioFrame;
    bool& ended = video ? m_videoEnded : m_audioEnded;
    bool& flushSent = video ? m_videoFlushSent : m_audioFlushSent;
    std::deque<AVPacket*>& ownQueue =
        video ? m_videoPackets : m_audioPackets;
    const int targetStream = video ? m_videoStreamIndex : m_audioStreamIndex;

    if (!ctx || !frame || ended) return nullptr;

    av_frame_unref(frame);

    while (true) {
        int rc = avcodec_receive_frame(ctx, frame);
        if (rc == 0) {
            return frame;
        }
        if (rc == AVERROR_EOF) {
            ended = true;
            return nullptr;
        }
        if (rc != AVERROR(EAGAIN)) {
            ended = true;
            return nullptr;
        }

        if (!ownQueue.empty()) {
            AVPacket* pkt = ownQueue.front();
            ownQueue.pop_front();
            const bool sent = sendPacketToDecoder(ctx, pkt, video);
            av_packet_free(&pkt);
            if (!sent) {
                ended = true;
                return nullptr;
            }
            continue;
        }

        if (m_inputEnded) {
            if (!flushSent) {
                rc = avcodec_send_packet(ctx, nullptr);
                if (rc == 0 || rc == AVERROR_EOF) {
                    flushSent = true;
                    continue;
                }
                if (rc != AVERROR(EAGAIN)) {
                    ended = true;
                    return nullptr;
                }
            } else {
                ended = true;
                return nullptr;
            }
            continue;
        }

        rc = av_read_frame(m_format, m_readPacket);
        if (rc < 0) {
            av_packet_unref(m_readPacket);
            m_inputEnded = true;
            continue;
        }

        if (m_readPacket->stream_index == targetStream) {
            const bool sent = sendPacketToDecoder(ctx, m_readPacket, video);
            av_packet_unref(m_readPacket);
            if (!sent) {
                ended = true;
                return nullptr;
            }
        } else {
            dispatchPacketToQueue(m_readPacket);
            av_packet_unref(m_readPacket);
        }
    }
}

AVFrame* MediaDecoder::nextVideoFrame()
{
    return nextFrame(true);
}

AVFrame* MediaDecoder::nextAudioFrame()
{
    return nextFrame(false);
}

void MediaDecoder::resetDecodeStateAfterSeek()
{
    if (m_videoCtx) avcodec_flush_buffers(m_videoCtx);
    if (m_audioCtx) avcodec_flush_buffers(m_audioCtx);
    if (m_readPacket) av_packet_unref(m_readPacket);
    if (m_videoFrame) av_frame_unref(m_videoFrame);
    if (m_audioFrame) av_frame_unref(m_audioFrame);
    clearPacketQueue(m_videoPackets);
    clearPacketQueue(m_audioPackets);

    m_inputEnded = false;
    m_videoFlushSent = false;
    m_audioFlushSent = (m_audioCtx == nullptr);
    m_videoEnded = (m_videoCtx == nullptr);
    m_audioEnded = (m_audioCtx == nullptr);
}

std::optional<std::string> MediaDecoder::seek(double seconds)
{
    if (!m_format || !m_videoCtx || m_videoStreamIndex < 0) {
        return std::string("MediaDecoder: seek called before open");
    }
    if (!std::isfinite(seconds)) {
        return std::string("MediaDecoder: seek target is not finite");
    }
    if (seconds < 0.0) seconds = 0.0;

    AVStream* videoStream = m_format->streams[m_videoStreamIndex];
    const int64_t targetUs = static_cast<int64_t>(
        seconds * static_cast<double>(AV_TIME_BASE));
    const AVRational usTimeBase = {1, AV_TIME_BASE};
    const int64_t targetTs =
        av_rescale_q(targetUs, usTimeBase, videoStream->time_base);

    int rc = av_seek_frame(m_format, m_videoStreamIndex, targetTs,
                           AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        rc = av_seek_frame(m_format, -1, targetUs, AVSEEK_FLAG_BACKWARD);
    }
    if (rc < 0) {
        return std::string("MediaDecoder: seek failed: ")
               + ffmpegErrorString(rc);
    }

    resetDecodeStateAfterSeek();
    return std::nullopt;
}

} // namespace libavcore
