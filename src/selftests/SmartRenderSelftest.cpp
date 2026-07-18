// SmartRenderSelftest.cpp
// Headless selftest for the pure smart-render segment eligibility predicate.
// Run via: --selftest=smart-render

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtGlobal>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
}

#include "../ClipGeometry.h"
#include "../SmartRender.h"
#include "../libavcore/Concat.h"
#include "../libavcore/Encode.h"
#include "../libavcore/Probe.h"

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* envName = "VEDITOR_SMART_RENDER")
        : name(envName),
          had(qEnvironmentVariableIsSet(envName)),
          value(qgetenv(envName))
    {
    }

    ~EnvGuard()
    {
        if (had)
            qputenv(name.constData(), value);
        else
            qunsetenv(name.constData());
    }

    QByteArray name;
    bool had = false;
    QByteArray value;
};

struct InputCtxGuard {
    AVFormatContext* ctx = nullptr;
    ~InputCtxGuard()
    {
        if (ctx)
            avformat_close_input(&ctx);
    }
};

struct OutputCtxGuard {
    AVFormatContext* ctx = nullptr;
    bool fileOpened = false;

    ~OutputCtxGuard()
    {
        if (ctx) {
            if (fileOpened && !(ctx->oformat->flags & AVFMT_NOFILE)
                && ctx->pb) {
                avio_closep(&ctx->pb);
            }
            avformat_free_context(ctx);
        }
    }
};

QString ffmpegErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, buf, sizeof(buf)) < 0)
        return QString::number(err);
    return QString::fromUtf8(buf);
}

ClipInfo pristineClip(const QString& codec = QStringLiteral("h264"))
{
    ClipInfo clip;
    clip.filePath = QStringLiteral(
        "smartrender://clip?codec=%1&width=1920&height=1080&fps=30&pixfmt=yuv420p")
        .arg(codec);
    clip.displayName = QStringLiteral("smart-render-fixture");
    clip.duration = 1.0;
    clip.outPoint = 1.0;
    clip.speed = 1.0;
    clip.videoScale = 1.0;
    clip.videoDx = 0.0;
    clip.videoDy = 0.0;
    clip.rotation2DDegrees = 0.0;
    return clip;
}

smartrender::SegmentEligibility eligibleFor(const ClipInfo& clip,
                                            const QString& outputCodec = QStringLiteral("h264"),
                                            int outputWidth = 1920,
                                            int outputHeight = 1080,
                                            double outputFps = 30.0,
                                            bool hasEffects = false,
                                            bool hasColorCorrection = false,
                                            bool hasTransform = false,
                                            bool hasTransitions = false,
                                            bool hasSpeedChange = false,
                                            bool hasKeyframes = false,
                                            bool hasLayerStyle = false,
                                            bool hasTrackMatte = false,
                                            bool isOverlayLayer = false)
{
    return smartrender::canStreamCopy(clip,
                                      outputCodec,
                                      outputWidth,
                                      outputHeight,
                                      outputFps,
                                      hasEffects,
                                      hasColorCorrection,
                                      hasTransform,
                                      hasTransitions,
                                      hasSpeedChange,
                                      hasKeyframes,
                                      hasLayerStyle,
                                      hasTrackMatte,
                                      isOverlayLayer);
}

smartrender::PassThroughTimelineRequest passThroughRequest(
    const ClipInfo& clip = pristineClip())
{
    smartrender::PassThroughTimelineRequest request;
    request.videoTracks = {{clip}};
    request.outputPath = QStringLiteral("smart-render-output.mp4");
    request.outputCodec = QStringLiteral("h264");
    request.outputWidth = 1920;
    request.outputHeight = 1080;
    request.outputFps = 30.0;
    request.timelineDurationSec = clip.effectiveDuration();
    return request;
}

bool encodeTinyMp4(const QString& path, QString* error)
{
    static constexpr int kWidth = 16;
    static constexpr int kHeight = 16;
    static constexpr int kFps = 4;
    static constexpr int kFrames = 4;
    static constexpr int kStride = kWidth * 3;

    libavcore::EncodeRequest request;
    request.width = kWidth;
    request.height = kHeight;
    request.fps = kFps;
    request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 200000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (const auto openErr = encoder.open(request)) {
        if (error)
            *error = QStringLiteral("FrameEncoder::open failed: ")
                + QString::fromStdString(*openErr);
        return false;
    }

    QByteArray frame(kStride * kHeight, Qt::Uninitialized);
    for (int f = 0; f < kFrames; ++f) {
        uchar* data = reinterpret_cast<uchar*>(frame.data());
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int off = y * kStride + x * 3;
                data[off + 0] = static_cast<uchar>((x * 16 + f * 11) & 0xff);
                data[off + 1] = static_cast<uchar>((y * 16 + f * 17) & 0xff);
                data[off + 2] = static_cast<uchar>((x * y + f * 23) & 0xff);
            }
        }
        if (!encoder.pushFrameRgb24(
                reinterpret_cast<const uint8_t*>(frame.constData()),
                kStride,
                f)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrameRgb24 failed");
            return false;
        }
    }

    if (const auto finalizeErr = encoder.finalize()) {
        if (error)
            *error = QStringLiteral("FrameEncoder::finalize failed: ")
                + QString::fromStdString(*finalizeErr);
        return false;
    }

    return QFile::exists(path);
}

bool writePacket(AVFormatContext* fmt,
                 int streamIndex,
                 int64_t pts,
                 int64_t duration,
                 int size,
                 uchar seed,
                 QString* error)
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        if (error)
            *error = QStringLiteral("failed to allocate packet");
        return false;
    }

    const int rc = av_new_packet(pkt, size);
    if (rc < 0) {
        av_packet_free(&pkt);
        if (error)
            *error = QStringLiteral("failed to allocate packet payload: ")
                + ffmpegErrorString(rc);
        return false;
    }

    std::memset(pkt->data, seed, static_cast<size_t>(size));
    pkt->stream_index = streamIndex;
    pkt->pts = pts;
    pkt->dts = pts;
    pkt->duration = duration;
    pkt->pos = -1;

    const int writeRc = av_interleaved_write_frame(fmt, pkt);
    av_packet_free(&pkt);
    if (writeRc < 0) {
        if (error)
            *error = QStringLiteral("failed to write packet: ")
                + ffmpegErrorString(writeRc);
        return false;
    }
    return true;
}

bool createAvOffsetFixture(const QString& path, int64_t audioDelayUs,
                           QString* error)
{
    static constexpr int kWidth = 16;
    static constexpr int kHeight = 16;
    static constexpr int kFps = 30;
    static constexpr int kVideoFrames = 30;
    static constexpr int kVideoPacketBytes = kWidth * kHeight * 3;
    static constexpr int kSampleRate = 48000;
    static constexpr int kAudioSamplesPerPacket = 1024;
    static constexpr int kAudioPackets = 24;
    static constexpr int kAudioPacketBytes = kAudioSamplesPerPacket * 2;

    OutputCtxGuard out;
    int rc = avformat_alloc_output_context2(&out.ctx,
                                            nullptr,
                                            "nut",
                                            path.toUtf8().constData());
    if (rc < 0 || !out.ctx) {
        if (error)
            *error = QStringLiteral("failed to allocate nut muxer: ")
                + ffmpegErrorString(rc);
        return false;
    }

    AVStream* video = avformat_new_stream(out.ctx, nullptr);
    AVStream* audio = avformat_new_stream(out.ctx, nullptr);
    if (!video || !audio) {
        if (error)
            *error = QStringLiteral("failed to create fixture streams");
        return false;
    }

    video->time_base = AVRational{1, kFps};
    video->avg_frame_rate = AVRational{kFps, 1};
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    video->codecpar->format = AV_PIX_FMT_RGB24;
    video->codecpar->width = kWidth;
    video->codecpar->height = kHeight;
    video->codecpar->bit_rate =
        static_cast<int64_t>(kVideoPacketBytes) * 8 * kFps;

    audio->time_base = AVRational{1, kSampleRate};
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audio->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    audio->codecpar->sample_rate = kSampleRate;
    audio->codecpar->bits_per_coded_sample = 16;
    audio->codecpar->block_align = 2;
    audio->codecpar->bit_rate = kSampleRate * 16;
    av_channel_layout_default(&audio->codecpar->ch_layout, 1);

    if (!(out.ctx->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&out.ctx->pb, path.toUtf8().constData(),
                       AVIO_FLAG_WRITE);
        if (rc < 0) {
            if (error)
                *error = QStringLiteral("failed to open fixture output: ")
                    + ffmpegErrorString(rc);
            return false;
        }
        out.fileOpened = true;
    }

    rc = avformat_write_header(out.ctx, nullptr);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("failed to write fixture header: ")
                + ffmpegErrorString(rc);
        return false;
    }

    const int64_t audioDelaySamples =
        av_rescale_q(audioDelayUs,
                     AVRational{1, AV_TIME_BASE},
                     audio->time_base);
    int videoFrame = 0;
    int audioPacket = 0;
    while (videoFrame < kVideoFrames || audioPacket < kAudioPackets) {
        const int64_t videoUs =
            videoFrame < kVideoFrames
                ? av_rescale_q(videoFrame,
                               video->time_base,
                               AVRational{1, AV_TIME_BASE})
                : std::numeric_limits<int64_t>::max();
        const int64_t audioUs =
            audioPacket < kAudioPackets
                ? av_rescale_q(audioDelaySamples
                                   + static_cast<int64_t>(audioPacket)
                                         * kAudioSamplesPerPacket,
                               audio->time_base,
                               AVRational{1, AV_TIME_BASE})
                : std::numeric_limits<int64_t>::max();

        if (videoUs <= audioUs) {
            if (!writePacket(out.ctx,
                             video->index,
                             videoFrame,
                             1,
                             kVideoPacketBytes,
                             static_cast<uchar>(0x20 + videoFrame),
                             error)) {
                return false;
            }
            ++videoFrame;
        } else {
            if (!writePacket(out.ctx,
                             audio->index,
                             audioDelaySamples
                                 + static_cast<int64_t>(audioPacket)
                                       * kAudioSamplesPerPacket,
                             kAudioSamplesPerPacket,
                             kAudioPacketBytes,
                             static_cast<uchar>(0x80 + audioPacket),
                             error)) {
                return false;
            }
            ++audioPacket;
        }
    }

    rc = av_write_trailer(out.ctx);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("failed to write fixture trailer: ")
                + ffmpegErrorString(rc);
        return false;
    }

    return true;
}

bool firstAudioVideoDeltaUs(const QString& path, int64_t* deltaUs,
                            QString* error)
{
    InputCtxGuard in;
    int rc = avformat_open_input(&in.ctx, path.toUtf8().constData(),
                                 nullptr, nullptr);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("failed to open probe input: ")
                + ffmpegErrorString(rc);
        return false;
    }
    rc = avformat_find_stream_info(in.ctx, nullptr);
    if (rc < 0) {
        if (error)
            *error = QStringLiteral("failed to read probe stream info: ")
                + ffmpegErrorString(rc);
        return false;
    }

    int videoIndex = -1;
    int audioIndex = -1;
    for (unsigned i = 0; i < in.ctx->nb_streams; ++i) {
        const AVCodecParameters* par = in.ctx->streams[i]->codecpar;
        if (!par)
            continue;
        if (videoIndex < 0 && par->codec_type == AVMEDIA_TYPE_VIDEO)
            videoIndex = static_cast<int>(i);
        if (audioIndex < 0 && par->codec_type == AVMEDIA_TYPE_AUDIO)
            audioIndex = static_cast<int>(i);
    }
    if (videoIndex < 0 || audioIndex < 0) {
        if (error)
            *error = QStringLiteral("probe input missing audio/video streams");
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        if (error)
            *error = QStringLiteral("failed to allocate probe packet");
        return false;
    }

    int64_t firstVideoUs = AV_NOPTS_VALUE;
    int64_t firstAudioUs = AV_NOPTS_VALUE;
    while ((rc = av_read_frame(in.ctx, pkt)) >= 0) {
        if ((pkt->stream_index == videoIndex
             || pkt->stream_index == audioIndex)
            && ((pkt->pts != AV_NOPTS_VALUE)
                || (pkt->dts != AV_NOPTS_VALUE))) {
            const int64_t ts =
                pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            const int64_t tsUs =
                av_rescale_q(ts,
                             in.ctx->streams[pkt->stream_index]->time_base,
                             AVRational{1, AV_TIME_BASE});
            if (pkt->stream_index == videoIndex
                && firstVideoUs == AV_NOPTS_VALUE) {
                firstVideoUs = tsUs;
            } else if (pkt->stream_index == audioIndex
                       && firstAudioUs == AV_NOPTS_VALUE) {
                firstAudioUs = tsUs;
            }
        }
        av_packet_unref(pkt);
        if (firstVideoUs != AV_NOPTS_VALUE
            && firstAudioUs != AV_NOPTS_VALUE) {
            break;
        }
    }
    av_packet_free(&pkt);

    if (firstVideoUs == AV_NOPTS_VALUE || firstAudioUs == AV_NOPTS_VALUE) {
        if (error)
            *error = QStringLiteral("probe input did not expose first A/V pts");
        return false;
    }

    *deltaUs = firstAudioUs - firstVideoUs;
    return true;
}

bool realRemuxGate(QString* error)
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        if (error)
            *error = QStringLiteral("QTemporaryDir failed");
        return false;
    }

    const QString inputPath = dir.filePath(QStringLiteral("input.mp4"));
    const QString outputPath = dir.filePath(QStringLiteral("output.mp4"));
    if (!encodeTinyMp4(inputPath, error))
        return false;

    if (const auto copyErr = libavcore::concatCopy(
            {inputPath.toStdString()}, outputPath.toStdString())) {
        if (error)
            *error = QStringLiteral("concatCopy failed: ")
                + QString::fromStdString(*copyErr);
        return false;
    }

    const auto inputCodec =
        libavcore::probeVideoCodecName(inputPath.toStdString());
    const auto outputCodec =
        libavcore::probeVideoCodecName(outputPath.toStdString());
    if (!inputCodec.has_value() || !outputCodec.has_value()
        || *inputCodec != *outputCodec) {
        if (error) {
            *error = QStringLiteral("codec mismatch after remux: in=%1 out=%2")
                .arg(inputCodec.has_value()
                         ? QString::fromStdString(*inputCodec)
                         : QStringLiteral("<none>"))
                .arg(outputCodec.has_value()
                         ? QString::fromStdString(*outputCodec)
                         : QStringLiteral("<none>"));
        }
        return false;
    }

    const auto inputDuration =
        libavcore::probeDurationMicroseconds(inputPath.toStdString());
    const auto outputDuration =
        libavcore::probeDurationMicroseconds(outputPath.toStdString());
    if (!inputDuration.has_value() || !outputDuration.has_value()
        || std::llabs(*inputDuration - *outputDuration) > 500000) {
        if (error) {
            *error = QStringLiteral("duration mismatch after remux: in=%1 out=%2")
                .arg(inputDuration.has_value()
                         ? QString::number(*inputDuration)
                         : QStringLiteral("<none>"))
                .arg(outputDuration.has_value()
                         ? QString::number(*outputDuration)
                         : QStringLiteral("<none>"));
        }
        return false;
    }

    return true;
}

bool avOffsetPreservationGate(QString* error)
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        if (error)
            *error = QStringLiteral("QTemporaryDir failed");
        return false;
    }

    static constexpr int64_t kAudioDelayUs = 500000;
    static constexpr int64_t kOnePacketToleranceUs = 35000;
    const QString inputPath = dir.filePath(QStringLiteral("av-offset-input.nut"));
    const QString outputPath = dir.filePath(QStringLiteral("av-offset-output.nut"));

    if (!createAvOffsetFixture(inputPath, kAudioDelayUs, error))
        return false;

    int64_t inputDeltaUs = 0;
    if (!firstAudioVideoDeltaUs(inputPath, &inputDeltaUs, error))
        return false;
    if (std::llabs(inputDeltaUs - kAudioDelayUs) > kOnePacketToleranceUs) {
        if (error) {
            *error = QStringLiteral("fixture A/V offset mismatch: expected=%1 actual=%2")
                .arg(static_cast<qlonglong>(kAudioDelayUs))
                .arg(static_cast<qlonglong>(inputDeltaUs));
        }
        return false;
    }

    if (const auto copyErr = libavcore::concatCopy(
            {inputPath.toStdString()}, outputPath.toStdString())) {
        if (error)
            *error = QStringLiteral("concatCopy failed: ")
                + QString::fromStdString(*copyErr);
        return false;
    }

    int64_t outputDeltaUs = 0;
    if (!firstAudioVideoDeltaUs(outputPath, &outputDeltaUs, error))
        return false;
    if (std::llabs(outputDeltaUs - inputDeltaUs) > kOnePacketToleranceUs) {
        if (error) {
            *error = QStringLiteral("A/V offset was not preserved: input=%1 output=%2")
                .arg(static_cast<qlonglong>(inputDeltaUs))
                .arg(static_cast<qlonglong>(outputDeltaUs));
        }
        return false;
    }

    return true;
}

bool readErrorPropagationGate(QString* error)
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        if (error)
            *error = QStringLiteral("QTemporaryDir failed");
        return false;
    }

    const QString inputPath = dir.filePath(QStringLiteral("read-error-input.nut"));
    const QString outputPath = dir.filePath(QStringLiteral("read-error-output.nut"));
    if (!createAvOffsetFixture(inputPath, 250000, error))
        return false;

    EnvGuard envGuard("VEDITOR_CONCAT_TEST_READ_ERROR");
    qputenv("VEDITOR_CONCAT_TEST_READ_ERROR", "1");
    const auto copyErr = libavcore::concatCopy(
        {inputPath.toStdString()}, outputPath.toStdString());
    if (!copyErr) {
        if (error)
            *error = QStringLiteral("concatCopy succeeded despite injected read error");
        return false;
    }

    return true;
}

} // namespace

int runSmartRenderSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int g, const char* desc, bool ok) {
        std::printf("[smart-render] %s G%d %s\n",
                    ok ? "PASS" : "FAIL", g, desc);
        ok ? ++passed : ++failed;
    };

    EnvGuard envGuard;

    {
        const smartrender::SegmentEligibility result = eligibleFor(pristineClip());
        check(1, "pristine matching clip is eligible", result.eligible);
    }

    auto checkRejected = [&](const char* desc,
                             const smartrender::SegmentEligibility& result) {
        check(2, desc, !result.eligible && !result.reason.isEmpty());
    };

    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasEffects rejects", eligibleFor(clip, QStringLiteral("h264"),
                                                        1920, 1080, 30.0, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasColorCorrection rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, true));
    }
    {
        ClipInfo clip = pristineClip();
        clip.videoScale = 1.01;
        checkRejected("non-identity transform rejects", eligibleFor(clip));
    }
    {
        ClipInfo clip = pristineClip();
        clip.speed = 0.5;
        checkRejected("speed change rejects", eligibleFor(clip));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasTransitions rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasKeyframes rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, false, false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasLayerStyle rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, false, false, false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("hasTrackMatte rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, false, false, false, false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("isOverlayLayer rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 30.0,
                                  false, false, false, false, false, false, false,
                                  false, true));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("codec mismatch rejects", eligibleFor(clip, QStringLiteral("hevc")));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("resolution width mismatch rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1280));
    }
    {
        const ClipInfo clip = pristineClip();
        checkRejected("fps mismatch rejects",
                      eligibleFor(clip, QStringLiteral("h264"), 1920, 1080, 24.0));
    }

    qunsetenv("VEDITOR_SMART_RENDER");
    check(3, "env unset keeps smart render disabled by default",
          !smartrender::enabledFromEnv());

    {
        const ClipInfo clip = pristineClip(QString());
        const smartrender::SegmentEligibility result = eligibleFor(clip);
        check(4, "empty source codec is conservative false",
              !result.eligible && !result.reason.isEmpty());
    }

    auto checkPassThroughRejected =
        [&](int g,
            const char* desc,
            const smartrender::PassThroughTimelineRequest& request) {
            const smartrender::PassThroughEligibility result =
                smartrender::timelinePassThrough(request);
            check(g, desc, !result.eligible && !result.reason.isEmpty());
        };

    {
        auto request = passThroughRequest();
        request.videoTracks[0].append(pristineClip());
        request.timelineDurationSec = 2.0;
        checkPassThroughRejected(5, "timelinePassThrough rejects multiple clips",
                                 request);
    }
    {
        ClipInfo clip = pristineClip();
        clip.inPoint = 0.1;
        clip.outPoint = 1.0;
        checkPassThroughRejected(6, "timelinePassThrough rejects trims",
                                 passThroughRequest(clip));
    }
    {
        ClipInfo clip = pristineClip();
        clip.leadInSec = 0.5;
        auto request = passThroughRequest(clip);
        request.timelineDurationSec = 1.5;
        checkPassThroughRejected(7, "timelinePassThrough rejects non-zero start",
                                 request);
    }
    {
        ClipInfo clip = pristineClip();
        clip.volume = 0.5;
        checkPassThroughRejected(8, "timelinePassThrough rejects audio processing",
                                 passThroughRequest(clip));
    }
    {
        auto request = passThroughRequest();
        request.jobEndUs = 500000;
        checkPassThroughRejected(9, "timelinePassThrough rejects partial export range",
                                 request);
    }
    {
        auto request = passThroughRequest();
        request.outputCodec = QStringLiteral("hevc");
        const smartrender::PassThroughEligibility result =
            smartrender::timelinePassThrough(request);
        check(10, "timelinePassThrough propagates canStreamCopy rejection",
              !result.eligible && result.reason.contains(QStringLiteral("codec")));
    }
    {
        const smartrender::PassThroughEligibility result =
            smartrender::timelinePassThrough(passThroughRequest());
        check(11, "timelinePassThrough accepts whole pristine timeline",
              result.eligible);
    }
    {
        QString error;
        const bool ok = realRemuxGate(&error);
        if (!ok)
            std::printf("[smart-render] real remux detail: %s\n",
                        error.toUtf8().constData());
        check(12, "concatCopy single-input remux preserves codec and duration",
              ok);
    }
    {
        auto request = passThroughRequest();
        request.videoTracks = {{}, {pristineClip()}};
        checkPassThroughRejected(13, "timelinePassThrough rejects clip off track 0",
                                 request);
    }
    {
        auto request = passThroughRequest();
        request.audioTracks = {{pristineClip()}};
        checkPassThroughRejected(14, "timelinePassThrough rejects separate audio clips",
                                 request);
    }
    {
        ClipInfo clip = pristineClip();
        EnhancedTextOverlay overlay;
        overlay.text = QStringLiteral("subtitle");
        clip.textManager.addOverlay(overlay);
        checkPassThroughRejected(15, "timelinePassThrough rejects text overlays",
                                 passThroughRequest(clip));
    }
    {
        auto request = passThroughRequest();
        request.hasAdjustmentLayers = true;
        checkPassThroughRejected(16, "timelinePassThrough rejects adjustment layers",
                                 request);
    }
    {
        auto request = passThroughRequest();
        TimelineTrackMatteEntry entry;
        entry.matteType = TrackMatteType::AlphaMatte;
        entry.matteSourceClipId = trackMatteClipKey(1, 0);
        request.trackMatteEntries.insert(trackMatteClipKey(0, 0), entry);
        checkPassThroughRejected(17, "timelinePassThrough rejects track matte entries",
                                 request);
    }
    {
        auto request = passThroughRequest(pristineClip(QStringLiteral("vp9")));
        request.outputPath = QStringLiteral("smart-render-output.mp4");
        request.outputCodec = QStringLiteral("vp9");
        checkPassThroughRejected(18, "timelinePassThrough rejects unsafe container/codec",
                                 request);
    }
    {
        auto request = passThroughRequest();
        request.hdrExport16Enabled = true;
        checkPassThroughRejected(19, "timelinePassThrough rejects HDR export flags",
                                 request);
    }
    {
        ClipInfo clip = pristineClip();
        clip.filePath = clipgeom::nullObjectFilePath();
        checkPassThroughRejected(20, "timelinePassThrough rejects generated null objects",
                                 passThroughRequest(clip));
    }
    {
        auto request = passThroughRequest();
        request.exportMarkedRangeOnly = true;
        request.hasMarkedRange = true;
        request.markedIn = 0.25;
        request.markedOut = 0.75;
        checkPassThroughRejected(21, "timelinePassThrough rejects marked partial range",
                                 request);
    }
    {
        auto request = passThroughRequest();
        request.hasMarkedRange = true;
        request.markedIn = 0.25;
        request.markedOut = 0.75;
        const smartrender::PassThroughEligibility result =
            smartrender::timelinePassThrough(request);
        check(22, "timelinePassThrough ignores inactive marked range",
              result.eligible);
    }

    {
        ClipInfo clip = pristineClip();
        clip.filePath = QStringLiteral(
            "smartrender://clip?codec=h264&width=1920&height=1080&fps=30"
            "&pixfmt=yuv420p&audio=pcm_s16le");
        checkPassThroughRejected(23,
                                 "timelinePassThrough rejects unsafe audio codec for mp4",
                                 passThroughRequest(clip));
    }
    {
        ClipInfo clip = pristineClip();
        clip.filePath = QStringLiteral(
            "smartrender://clip?codec=h264&width=1920&height=1080&fps=30"
            "&pixfmt=yuv420p&audio=aac");
        const smartrender::PassThroughEligibility result =
            smartrender::timelinePassThrough(passThroughRequest(clip));
        check(24, "timelinePassThrough accepts safe aac audio in mp4",
              result.eligible);
    }
    {
        QString error;
        const bool ok = avOffsetPreservationGate(&error);
        if (!ok)
            std::printf("[smart-render] A/V offset detail: %s\n",
                        error.toUtf8().constData());
        check(25, "concatCopy preserves relative A/V offset", ok);
    }
    {
        QString error;
        const bool ok = readErrorPropagationGate(&error);
        if (!ok)
            std::printf("[smart-render] read error detail: %s\n",
                        error.toUtf8().constData());
        check(26, "concatCopy reports injected read errors", ok);
    }

    std::printf("[smart-render] summary: passed=%d failed=%d\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
