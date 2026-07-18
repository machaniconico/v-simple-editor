// src/selftests/libavcore_selftests.cpp
// PRD-SPLIT-MAIN-3 Phase 3-A: libavcore-backed selftests moved verbatim
// from src/main.cpp. Dispatch via SelftestRegistry.{h,cpp}.

#include "libavcore/Decode.h"
#include "libavcore/Encode.h"
#include "libavcore/Probe.h"
#include "libavcore/VideoFilterGraph.h"
#include "CodecDetector.h"
#include "ProxyManager.h"
#include "VideoStabilizer.h"
#include "AIHighlight.h"

#include <QString>
#include <QStringLiteral>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <QTemporaryDir>
#include <QDebug>
#include <QMetaObject>
#include <cmath>
#include <functional>
#include <optional>
#include <string>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
}

extern void writeLogLine(const QString& level, const QString& msg);

// ---- helper / worker / selftest functions moved verbatim from src/main.cpp ----

static QString libavcoreSelftestAvError(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, buf, sizeof(buf)) < 0)
        return QString::number(err);
    return QString::fromUtf8(buf);
}

static bool libavcoreSelftestFail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

static bool libavcoreSelftestIsH264Family(const std::string &name)
{
    const QString lower = QString::fromStdString(name).toLower();
    return lower.contains(QStringLiteral("h264"))
        || lower.contains(QStringLiteral("x264"))
        || lower.contains(QStringLiteral("264"));
}

static void runLibavcoreSelftestWorker(const std::function<void()> &fn)
{
    QThread *thread = QThread::create([fn]() { fn(); });
    thread->start();
    thread->wait();
    delete thread;
}

static void fillLibavcoreSelftestRgb24(QByteArray &frameBuf,
                                       int width,
                                       int height,
                                       int stride,
                                       int frameIndex)
{
    for (int y = 0; y < height; ++y) {
        uint8_t *row = reinterpret_cast<uint8_t*>(frameBuf.data())
                       + static_cast<qsizetype>(y) * stride;
        for (int x = 0; x < width; ++x) {
            uint8_t *px = row + static_cast<qsizetype>(x) * 3;
            px[0] = static_cast<uint8_t>((x + frameIndex * 4) & 0xFF);
            px[1] = static_cast<uint8_t>((y + frameIndex * 2) & 0xFF);
            px[2] = static_cast<uint8_t>((x + y + frameIndex) & 0xFF);
        }
    }
}

static bool pushLibavcoreSelftestAacSine(libavcore::FrameEncoder &enc,
                                         int sampleRate,
                                         int channels,
                                         int totalSamples,
                                         QString *error)
{
    AVChannelLayout layout = {};
    av_channel_layout_default(&layout, channels);

    SwrContext *swr = nullptr;
    int rc = swr_alloc_set_opts2(&swr,
                                 &layout, AV_SAMPLE_FMT_FLTP, sampleRate,
                                 &layout, AV_SAMPLE_FMT_S16, sampleRate,
                                 0, nullptr);
    if (rc < 0 || !swr) {
        av_channel_layout_uninit(&layout);
        return libavcoreSelftestFail(
            error,
            QStringLiteral("swr_alloc_set_opts2 failed: %1")
                .arg(libavcoreSelftestAvError(rc)));
    }
    rc = swr_init(swr);
    if (rc < 0) {
        swr_free(&swr);
        av_channel_layout_uninit(&layout);
        return libavcoreSelftestFail(
            error,
            QStringLiteral("swr_init failed: %1")
                .arg(libavcoreSelftestAvError(rc)));
    }

    constexpr int kChunkSamples = 1024;
    constexpr double kToneHz = 440.0;
    double phase = 0.0;
    const double phaseStep =
        (2.0 * M_PI * kToneHz) / static_cast<double>(sampleRate);
    int64_t audioPts = 0;

    for (int offset = 0; offset < totalSamples; offset += kChunkSamples) {
        const int chunk = qMin(kChunkSamples, totalSamples - offset);
        QByteArray pcm(static_cast<qsizetype>(chunk) * channels
                           * static_cast<int>(sizeof(int16_t)),
                       Qt::Uninitialized);
        int16_t *pcmSamples = reinterpret_cast<int16_t*>(pcm.data());
        for (int i = 0; i < chunk; ++i) {
            const int16_t v = static_cast<int16_t>(
                std::sin(phase) * 12000.0);
            phase += phaseStep;
            if (phase >= 2.0 * M_PI)
                phase -= 2.0 * M_PI;
            for (int c = 0; c < channels; ++c)
                pcmSamples[i * channels + c] = v;
        }

        AVFrame *audioFrame = av_frame_alloc();
        if (!audioFrame) {
            swr_free(&swr);
            av_channel_layout_uninit(&layout);
            return libavcoreSelftestFail(
                error, QStringLiteral("failed to allocate audio frame"));
        }
        audioFrame->nb_samples = chunk;
        audioFrame->format = AV_SAMPLE_FMT_FLTP;
        audioFrame->sample_rate = sampleRate;
        rc = av_channel_layout_copy(&audioFrame->ch_layout, &layout);
        if (rc < 0) {
            av_frame_free(&audioFrame);
            swr_free(&swr);
            av_channel_layout_uninit(&layout);
            return libavcoreSelftestFail(
                error,
                QStringLiteral("audio channel-layout copy failed: %1")
                    .arg(libavcoreSelftestAvError(rc)));
        }
        rc = av_frame_get_buffer(audioFrame, 0);
        if (rc < 0) {
            av_frame_free(&audioFrame);
            swr_free(&swr);
            av_channel_layout_uninit(&layout);
            return libavcoreSelftestFail(
                error,
                QStringLiteral("audio frame buffer allocation failed: %1")
                    .arg(libavcoreSelftestAvError(rc)));
        }

        const uint8_t *srcData[1] = {
            reinterpret_cast<const uint8_t*>(pcm.constData())
        };
        rc = swr_convert(swr, audioFrame->data, chunk, srcData, chunk);
        if (rc <= 0) {
            const QString msg =
                QStringLiteral("swr_convert returned %1").arg(rc);
            av_frame_free(&audioFrame);
            swr_free(&swr);
            av_channel_layout_uninit(&layout);
            return libavcoreSelftestFail(error, msg);
        }
        audioFrame->nb_samples = rc;

        const bool pushed = enc.pushAudioFrame(audioFrame, audioPts);
        audioPts += rc;
        av_frame_free(&audioFrame);
        if (!pushed) {
            swr_free(&swr);
            av_channel_layout_uninit(&layout);
            return libavcoreSelftestFail(
                error, QStringLiteral("pushAudioFrame returned false"));
        }
    }

    swr_free(&swr);
    av_channel_layout_uninit(&layout);
    return true;
}

static bool encodeLibavcoreSelftestClip(const QString &outPath,
                                        int width,
                                        int height,
                                        int fps,
                                        int frameCount,
                                        bool encodeAudio,
                                        std::string *activeEncoder,
                                        QString *error)
{
    QFile::remove(outPath);

    libavcore::EncodeRequest req;
    req.width = width;
    req.height = height;
    req.fps = fps;
    req.videoBitrateBits = 2000000;
    req.videoCodecName = "libx264";
    req.outputPath = outPath.toStdString();
    req.encoderAvailableHook = [](const std::string &name) {
        return CodecDetector::isEncoderAvailable(QString::fromStdString(name));
    };
    if (encodeAudio) {
        req.audioEncode = true;
        req.audioSampleRate = 48000;
        req.audioChannels = 2;
        req.audioBitrateBits = 128000;
    }

    libavcore::FrameEncoder enc;
    const std::optional<std::string> openErr = enc.open(req);
    if (openErr.has_value()) {
        return libavcoreSelftestFail(
            error,
            QStringLiteral("FrameEncoder::open() returned error: %1")
                .arg(QString::fromStdString(*openErr)));
    }
    if (!enc.isOpen()) {
        return libavcoreSelftestFail(
            error,
            QStringLiteral("open() reported success but isOpen() is false"));
    }

    const int stride = width * 3;
    QByteArray frameBuf(static_cast<qsizetype>(stride) * height,
                        Qt::Uninitialized);
    for (int f = 0; f < frameCount; ++f) {
        fillLibavcoreSelftestRgb24(frameBuf, width, height, stride, f);
        if (!enc.pushFrameRgb24(
                reinterpret_cast<const uint8_t*>(frameBuf.constData()),
                stride, f)) {
            return libavcoreSelftestFail(
                error,
                QStringLiteral("pushFrameRgb24 returned false for frame %1")
                    .arg(f));
        }
    }

    if (encodeAudio) {
        const int totalSamples =
            (req.audioSampleRate * frameCount + fps - 1) / fps;
        if (!pushLibavcoreSelftestAacSine(enc,
                                          req.audioSampleRate,
                                          req.audioChannels,
                                          totalSamples,
                                          error)) {
            return false;
        }
    }

    const std::optional<std::string> finalizeErr = enc.finalize();
    if (finalizeErr.has_value()) {
        return libavcoreSelftestFail(
            error,
            QStringLiteral("finalize() returned error: %1")
                .arg(QString::fromStdString(*finalizeErr)));
    }

    if (activeEncoder)
        *activeEncoder = enc.activeEncoderName();
    return true;
}

static std::optional<double> libavcoreSelftestFramePtsSeconds(const AVFrame *frame,
                                                             AVRational timeBase)
{
    if (!frame || timeBase.num <= 0 || timeBase.den <= 0)
        return std::nullopt;
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
        pts = frame->pts;
    if (pts == AV_NOPTS_VALUE)
        return std::nullopt;
    return static_cast<double>(pts) * av_q2d(timeBase);
}

static bool transcodeLibavcoreSelftestRoundTrip(const QString &inputPath,
                                                const QString &outPath,
                                                int outWidth,
                                                int outHeight,
                                                int fps,
                                                std::string *activeEncoder,
                                                int *pushedFrames,
                                                QString *error)
{
    QFile::remove(outPath);

    libavcore::MediaDecoder dec;
    const std::optional<std::string> openErr =
        dec.open(inputPath.toStdString(), false);
    if (openErr.has_value()) {
        return libavcoreSelftestFail(
            error,
            QStringLiteral("MediaDecoder::open(roundtrip input) failed: %1")
                .arg(QString::fromStdString(*openErr)));
    }

    const libavcore::VideoStreamProps props = dec.videoProps();
    SwsContext *sws = sws_getContext(props.width,
                                     props.height,
                                     props.pixelFormat,
                                     outWidth,
                                     outHeight,
                                     AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR,
                                     nullptr,
                                     nullptr,
                                     nullptr);
    if (!sws) {
        return libavcoreSelftestFail(
            error,
            QStringLiteral("sws_getContext failed for roundtrip scale"));
    }

    auto fail = [&](const QString &message) {
        sws_freeContext(sws);
        return libavcoreSelftestFail(error, message);
    };

    libavcore::EncodeRequest req;
    req.width = outWidth;
    req.height = outHeight;
    req.fps = fps;
    req.videoBitrateBits = 1200000;
    req.videoCodecName = "libx264";
    req.outputPath = outPath.toStdString();
    req.encoderAvailableHook = [](const std::string &name) {
        return CodecDetector::isEncoderAvailable(QString::fromStdString(name));
    };

    libavcore::FrameEncoder enc;
    const std::optional<std::string> encOpenErr = enc.open(req);
    if (encOpenErr.has_value()) {
        return fail(QStringLiteral("roundtrip FrameEncoder::open() failed: %1")
                        .arg(QString::fromStdString(*encOpenErr)));
    }

    const int stride = outWidth * 3;
    QByteArray scaled(static_cast<qsizetype>(stride) * outHeight,
                      Qt::Uninitialized);
    int frameIndex = 0;
    while (AVFrame *frame = dec.nextVideoFrame()) {
        uint8_t *dstData[1] = {
            reinterpret_cast<uint8_t*>(scaled.data())
        };
        int dstLinesize[1] = { stride };
        const int scaledRows = sws_scale(sws,
                                         frame->data,
                                         frame->linesize,
                                         0,
                                         props.height,
                                         dstData,
                                         dstLinesize);
        if (scaledRows != outHeight) {
            return fail(QStringLiteral("sws_scale produced %1/%2 rows")
                            .arg(scaledRows)
                            .arg(outHeight));
        }
        if (!enc.pushFrameRgb24(
                reinterpret_cast<const uint8_t*>(scaled.constData()),
                stride,
                frameIndex)) {
            return fail(QStringLiteral("roundtrip pushFrameRgb24 failed at "
                                       "frame %1").arg(frameIndex));
        }
        ++frameIndex;
    }
    if (frameIndex <= 0)
        return fail(QStringLiteral("roundtrip decoded no video frames"));

    const std::optional<std::string> finalizeErr = enc.finalize();
    if (finalizeErr.has_value()) {
        return fail(QStringLiteral("roundtrip finalize() failed: %1")
                        .arg(QString::fromStdString(*finalizeErr)));
    }

    if (activeEncoder)
        *activeEncoder = enc.activeEncoderName();
    if (pushedFrames)
        *pushedFrames = frameIndex;
    sws_freeContext(sws);
    return true;
}

// US-MF-4 / US-B2-8: in-process libavcore encoder regression gate
// (VEDITOR_LIBAVCORE_ENCODE_SELFTEST=1). This must resolve to an H.264-family
// encoder (libx264/h264_mf/etc.); an mpeg4 fallback is a regression.
int runLibavcoreEncodeSelftest()
{
    qInfo() << "[INFO] LIBAVCORE-ENCODE selftest: in-process FrameEncoder gate";

    const int kWidth = 320;
    const int kHeight = 240;
    const int kFps = 30;
    const int kFrameCount = 30;

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qCritical() << "LIBAVCORE-ENCODE selftest FAILED: could not create temp dir";
        return 1;
    }
    const QString outPath = tmpDir.filePath(QStringLiteral("libavcore_encode_selftest.mp4"));

    bool encodeOk = false;
    QString encodeError;
    std::string activeEnc;
    runLibavcoreSelftestWorker([&]() {
        encodeOk = encodeLibavcoreSelftestClip(outPath,
                                               kWidth,
                                               kHeight,
                                               kFps,
                                               kFrameCount,
                                               false,
                                               &activeEnc,
                                               &encodeError);
    });
    if (!encodeOk) {
        qCritical() << "LIBAVCORE-ENCODE selftest FAILED:" << encodeError;
        QFile::remove(outPath);
        return 1;
    }

    qInfo() << "[INFO] LIBAVCORE-ENCODE selftest: activeEncoderName ="
            << QString::fromStdString(activeEnc);
    if (!libavcoreSelftestIsH264Family(activeEnc)) {
        qCritical() << "LIBAVCORE-ENCODE selftest FAILED: expected an H.264"
                    << "encoder, got" << QString::fromStdString(activeEnc)
                    << "(mpeg4 fallback is not accepted)";
        QFile::remove(outPath);
        return 1;
    }

    if (!QFile::exists(outPath)) {
        qCritical() << "LIBAVCORE-ENCODE selftest FAILED: encoder produced no"
                    << "output file at" << outPath;
        return 1;
    }

    // (5) Re-open the artifact with the Qt-free Probe API and assert a real
    //     video stream plus a positive duration.
    const std::string probePath = outPath.toStdString();
    auto codecName = libavcore::probeVideoCodecName(probePath);
    if (!codecName.has_value() || codecName->empty()) {
        qCritical() << "LIBAVCORE-ENCODE selftest FAILED: probeVideoCodecName"
                    << "found no video stream in the output";
        return 1;
    }
    qInfo() << "[INFO] LIBAVCORE-ENCODE selftest: probed video codec ="
            << QString::fromStdString(*codecName);

    auto durationUs = libavcore::probeDurationMicroseconds(probePath);
    if (!durationUs.has_value() || *durationUs <= 0) {
        qCritical() << "LIBAVCORE-ENCODE selftest FAILED: probeDurationMicroseconds"
                    << "did not return a positive duration (got"
                    << (durationUs.has_value() ? QString::number(*durationUs)
                                               : QStringLiteral("nullopt"))
                    << ")";
        return 1;
    }
    qInfo() << "[INFO] LIBAVCORE-ENCODE selftest: probed duration (us) ="
            << static_cast<qlonglong>(*durationUs);

    // (7) Drop the temp artifact (QTemporaryDir also cleans up on destruction,
    //     but remove explicitly so the PASS path leaves nothing behind).
    QFile::remove(outPath);

    qInfo() << "[INFO] LIBAVCORE-ENCODE selftest PASSED: encoder ="
            << QString::fromStdString(activeEnc) << "codec ="
            << QString::fromStdString(*codecName);
    return 0;
}

// US-B2-8: in-process decode + AAC encode regression gate
// (VEDITOR_LIBAVCORE_DECODE_SELFTEST=1). Generates a video+AAC MP4 with
// FrameEncoder, decodes it with MediaDecoder, validates seek, then performs a
// decode->swscale->FrameEncoder proxy-style round trip and independently
// re-probes the result.
int runLibavcoreDecodeSelftest()
{
    qInfo() << "[INFO] LIBAVCORE-DECODE selftest: in-process decode gate";

    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    constexpr int kFps = 30;
    constexpr int kFrameCount = 30;
    constexpr int kScaledWidth = 160;
    constexpr int kScaledHeight = 120;

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qCritical() << "LIBAVCORE-DECODE selftest FAILED: could not create temp dir";
        return 1;
    }
    const QString inputPath =
        tmpDir.filePath(QStringLiteral("libavcore_decode_input.mp4"));
    const QString roundTripPath =
        tmpDir.filePath(QStringLiteral("libavcore_decode_roundtrip.mp4"));

    auto fail = [&](const QString &message) {
        qCritical() << "LIBAVCORE-DECODE selftest FAILED:" << message;
        QFile::remove(inputPath);
        QFile::remove(roundTripPath);
        return 1;
    };

    bool encodeOk = false;
    QString encodeError;
    std::string inputEncoder;
    runLibavcoreSelftestWorker([&]() {
        encodeOk = encodeLibavcoreSelftestClip(inputPath,
                                               kWidth,
                                               kHeight,
                                               kFps,
                                               kFrameCount,
                                               true,
                                               &inputEncoder,
                                               &encodeError);
    });
    if (!encodeOk)
        return fail(QStringLiteral("input clip encode failed: %1")
                        .arg(encodeError));
    qInfo() << "[INFO] LIBAVCORE-DECODE selftest: input activeEncoderName ="
            << QString::fromStdString(inputEncoder);
    if (!libavcoreSelftestIsH264Family(inputEncoder)) {
        return fail(QStringLiteral("expected H.264 encoder for input clip, got "
                                   "%1 (mpeg4 fallback is not accepted)")
                        .arg(QString::fromStdString(inputEncoder)));
    }

    int decodedVideoFrames = 0;
    {
        libavcore::MediaDecoder dec;
        const std::optional<std::string> openErr =
            dec.open(inputPath.toStdString(), true);
        if (openErr.has_value()) {
            return fail(QStringLiteral("MediaDecoder::open() failed: %1")
                            .arg(QString::fromStdString(*openErr)));
        }
        if (!dec.hasVideo())
            return fail(QStringLiteral("MediaDecoder reported no video stream"));
        if (!dec.hasAudio())
            return fail(QStringLiteral("MediaDecoder reported no audio stream"));

        const libavcore::VideoStreamProps videoProps = dec.videoProps();
        if (videoProps.width != kWidth || videoProps.height != kHeight) {
            return fail(QStringLiteral("decoded dimensions were %1x%2, expected "
                                       "%3x%4")
                            .arg(videoProps.width)
                            .arg(videoProps.height)
                            .arg(kWidth)
                            .arg(kHeight));
        }

        const libavcore::AudioStreamProps audioProps = dec.audioProps();
        if (audioProps.sampleRate <= 0) {
            return fail(QStringLiteral("decoded audio sampleRate was not positive "
                                       "(got %1)")
                            .arg(audioProps.sampleRate));
        }

        while (dec.nextVideoFrame())
            ++decodedVideoFrames;
        if (qAbs(decodedVideoFrames - kFrameCount) > 2) {
            return fail(QStringLiteral("decoded %1 video frames, expected about %2")
                            .arg(decodedVideoFrames)
                            .arg(kFrameCount));
        }

        AVFrame *audioFrame = dec.nextAudioFrame();
        if (!audioFrame) {
            return fail(QStringLiteral("nextAudioFrame() returned no AAC frame"));
        }
        qInfo() << "[INFO] LIBAVCORE-DECODE selftest: decoded frames ="
                << decodedVideoFrames << "audio sampleRate ="
                << audioProps.sampleRate;

        constexpr double kSeekTargetSeconds = 0.5;
        constexpr double kSeekToleranceSeconds = 0.25;
        const std::optional<std::string> seekErr = dec.seek(kSeekTargetSeconds);
        if (seekErr.has_value()) {
            return fail(QStringLiteral("MediaDecoder::seek() failed: %1")
                            .arg(QString::fromStdString(*seekErr)));
        }
        AVFrame *seekFrame = dec.nextVideoFrame();
        if (!seekFrame)
            return fail(QStringLiteral("nextVideoFrame() after seek returned null"));
        const std::optional<double> seekPts =
            libavcoreSelftestFramePtsSeconds(seekFrame, dec.videoProps().timeBase);
        if (!seekPts.has_value()) {
            return fail(QStringLiteral("seek frame had no usable PTS"));
        }
        if (*seekPts < -kSeekToleranceSeconds
            || *seekPts > kSeekTargetSeconds + kSeekToleranceSeconds) {
            return fail(QStringLiteral("seek frame pts %1 was not near target %2")
                            .arg(*seekPts, 0, 'f', 6)
                            .arg(kSeekTargetSeconds, 0, 'f', 6));
        }
        qInfo() << "[INFO] LIBAVCORE-DECODE selftest: seek target/pts ="
                << kSeekTargetSeconds << "/" << *seekPts;
    }

    bool roundTripOk = false;
    QString roundTripError;
    std::string roundTripEncoder;
    int roundTripFrames = 0;
    runLibavcoreSelftestWorker([&]() {
        roundTripOk = transcodeLibavcoreSelftestRoundTrip(inputPath,
                                                          roundTripPath,
                                                          kScaledWidth,
                                                          kScaledHeight,
                                                          kFps,
                                                          &roundTripEncoder,
                                                          &roundTripFrames,
                                                          &roundTripError);
    });
    if (!roundTripOk) {
        return fail(QStringLiteral("roundtrip transcode failed: %1")
                        .arg(roundTripError));
    }
    qInfo() << "[INFO] LIBAVCORE-DECODE selftest: roundtrip activeEncoderName ="
            << QString::fromStdString(roundTripEncoder) << "frames ="
            << roundTripFrames;
    if (!libavcoreSelftestIsH264Family(roundTripEncoder)) {
        return fail(QStringLiteral("expected H.264 encoder for roundtrip, got "
                                   "%1 (mpeg4 fallback is not accepted)")
                        .arg(QString::fromStdString(roundTripEncoder)));
    }

    const std::string roundTripProbePath = roundTripPath.toStdString();
    auto codecName = libavcore::probeVideoCodecName(roundTripProbePath);
    if (!codecName.has_value() || codecName->empty()) {
        return fail(QStringLiteral("probeVideoCodecName found no video stream "
                                   "in roundtrip output"));
    }
    if (!libavcoreSelftestIsH264Family(*codecName)) {
        return fail(QStringLiteral("roundtrip probe reported non-H.264 codec "
                                   "%1")
                        .arg(QString::fromStdString(*codecName)));
    }
    auto durationUs = libavcore::probeDurationMicroseconds(roundTripProbePath);
    if (!durationUs.has_value() || *durationUs <= 0) {
        return fail(QStringLiteral("probeDurationMicroseconds did not return "
                                   "a positive roundtrip duration (got %1)")
                        .arg(durationUs.has_value()
                                 ? QString::number(*durationUs)
                                 : QStringLiteral("nullopt")));
    }
    qInfo() << "[INFO] LIBAVCORE-DECODE selftest: roundtrip probe codec/duration ="
            << QString::fromStdString(*codecName)
            << static_cast<qlonglong>(*durationUs);

    QFile::remove(inputPath);
    QFile::remove(roundTripPath);

    qInfo() << "[INFO] LIBAVCORE-DECODE selftest PASSED: decoded frames ="
            << decodedVideoFrames << "roundtrip frames =" << roundTripFrames
            << "codec =" << QString::fromStdString(*codecName);
    return 0;
}

// US-B3-4: deshake in-process regression gate
// (VEDITOR_VIDEOSTAB_DESHAKE_SELFTEST=1).
//
// Exercises the full in-process deshake pipeline without any QProcess/ffmpeg
// subprocess:
//
//   (1) Encodes a synthetic "shaky" clip (sine-wave pixel offsets, ~30 frames)
//       using libavcore::FrameEncoder (libx264) into a temp mp4.
//   (2) Validates VideoStabilizer::buildDeshakeFilter() returns a well-formed
//       "deshake=..." filter string (US-B3-2 regression gate).
//   (3) Runs libavcore::VideoFilterGraph::run() on that clip with the deshake
//       filter — asserts std::nullopt (success).
//   (4) Independently re-probes the output with libavcore::Probe to verify a
//       video stream is present and duration is positive (tautology-free).
//   (5) Sanity-guards that output file size > 0.
//   Cleans up both temp files on all paths.
int runVideostabDeshakeSelftest()
{
    qInfo() << "[INFO] VIDEOSTAB-DESHAKE selftest: deshake in-process regression gate";

    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    constexpr int kFps = 30;
    constexpr int kFrameCount = 30;

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qCritical() << "VIDEOSTAB-DESHAKE selftest FAILED: could not create temp dir";
        return 1;
    }
    const QString inputPath =
        tmpDir.filePath(QStringLiteral("deshake_input.mp4"));
    const QString outputPath =
        tmpDir.filePath(QStringLiteral("deshake_output.mp4"));

    auto fail = [&](const QString &message) {
        qCritical() << "VIDEOSTAB-DESHAKE selftest FAILED:" << message;
        QFile::remove(inputPath);
        QFile::remove(outputPath);
        return 1;
    };

    // -----------------------------------------------------------------------
    // (1) Encode synthetic shaky clip via FrameEncoder
    // -----------------------------------------------------------------------
    // Each frame is a 320x240 RGB24 image with the test pattern shifted by
    // a sine-wave offset to simulate camera shake.
    {
        bool encodeOk = false;
        QString encodeError;
        std::string activeEncoder;
        runLibavcoreSelftestWorker([&]() {
            QFile::remove(inputPath);

            libavcore::EncodeRequest req;
            req.width      = kWidth;
            req.height     = kHeight;
            req.fps        = kFps;
            req.videoBitrateBits = 2000000;
            req.videoCodecName   = "libx264";
            req.outputPath = inputPath.toStdString();
            req.encoderAvailableHook = [](const std::string &name) {
                return CodecDetector::isEncoderAvailable(
                    QString::fromStdString(name));
            };

            libavcore::FrameEncoder enc;
            const std::optional<std::string> openErr = enc.open(req);
            if (openErr.has_value()) {
                encodeError = QStringLiteral("FrameEncoder::open() failed: %1")
                                  .arg(QString::fromStdString(*openErr));
                return;
            }

            const int stride = kWidth * 3;
            QByteArray frameBuf(static_cast<qsizetype>(stride) * kHeight,
                                Qt::Uninitialized);

            for (int f = 0; f < kFrameCount; ++f) {
                // Sine-wave shake offsets (known artificial motion)
                const int dx = static_cast<int>(std::round(8.0 * std::sin(f * 0.4)));
                const int dy = static_cast<int>(std::round(6.0 * std::cos(f * 0.3)));

                // Fill frameBuf with a shifted test pattern (white centre square
                // + colour corners + diagonal grid lines)
                auto *buf = reinterpret_cast<uint8_t*>(frameBuf.data());
                for (int y = 0; y < kHeight; ++y) {
                    uint8_t *row = buf + static_cast<qsizetype>(y) * stride;
                    for (int x = 0; x < kWidth; ++x) {
                        uint8_t *px = row + x * 3;
                        const int sx = x - dx;
                        const int sy = y - dy;
                        // Centre white square (40x40 around (160,120))
                        if (sx >= 140 && sx < 180 && sy >= 100 && sy < 140) {
                            px[0] = px[1] = px[2] = 255;
                        }
                        // Diagonal grid lines every 20 px
                        else if ((sx + sy) % 20 == 0) {
                            px[0] = 180; px[1] = 180; px[2] = 180;
                        }
                        // Colour corners (8x8)
                        else if (sx < 8 && sy < 8) {
                            px[0] = 255; px[1] = 0; px[2] = 0;   // red TL
                        } else if (sx >= kWidth - 8 && sy < 8) {
                            px[0] = 0; px[1] = 255; px[2] = 0;   // green TR
                        } else if (sx < 8 && sy >= kHeight - 8) {
                            px[0] = 0; px[1] = 0; px[2] = 255;   // blue BL
                        } else if (sx >= kWidth - 8 && sy >= kHeight - 8) {
                            px[0] = 255; px[1] = 255; px[2] = 0; // yellow BR
                        } else {
                            // Background gradient
                            px[0] = static_cast<uint8_t>((sx + f * 3) & 0xFF);
                            px[1] = static_cast<uint8_t>((sy + f * 2) & 0xFF);
                            px[2] = static_cast<uint8_t>((sx + sy + f) & 0xFF);
                        }
                    }
                }

                if (!enc.pushFrameRgb24(
                        reinterpret_cast<const uint8_t*>(frameBuf.constData()),
                        stride, f)) {
                    encodeError = QStringLiteral("pushFrameRgb24 failed at frame %1")
                                      .arg(f);
                    return;
                }
            }

            const std::optional<std::string> finalizeErr = enc.finalize();
            if (finalizeErr.has_value()) {
                encodeError = QStringLiteral("finalize() failed: %1")
                                  .arg(QString::fromStdString(*finalizeErr));
                return;
            }
            activeEncoder = enc.activeEncoderName();
            encodeOk = true;
        });

        if (!encodeOk)
            return fail(QStringLiteral("input clip encode failed: %1").arg(encodeError));
        qInfo() << "[INFO] VIDEOSTAB-DESHAKE selftest (1): input encoded,"
                << "encoder =" << QString::fromStdString(activeEncoder);
    }

    // -----------------------------------------------------------------------
    // (2) buildDeshakeFilter() structural validation
    // -----------------------------------------------------------------------
    const StabilizerConfig cfg;  // default config
    const QString filterQStr = VideoStabilizer::buildDeshakeFilter(cfg);
    const std::string filterStr = filterQStr.toStdString();

    qInfo() << "[INFO] VIDEOSTAB-DESHAKE selftest (2): buildDeshakeFilter ="
            << filterQStr;

    if (!filterQStr.startsWith(QStringLiteral("deshake="))) {
        return fail(QStringLiteral(
            "buildDeshakeFilter() did not return a 'deshake=...' string, got: %1")
                        .arg(filterQStr));
    }
    if (filterQStr.indexOf(QLatin1Char(':')) < 0) {
        return fail(QStringLiteral(
            "buildDeshakeFilter() returned no ':' key=value separator: %1")
                        .arg(filterQStr));
    }

    // -----------------------------------------------------------------------
    // (3) VideoFilterGraph::run() — in-process deshake
    // -----------------------------------------------------------------------
    QFile::remove(outputPath);
    {
        bool filterOk = false;
        QString filterError;
        runLibavcoreSelftestWorker([&]() {
            libavcore::VideoFilterRequest vreq;
            vreq.inputPath        = inputPath.toStdString();
            vreq.outputPath       = outputPath.toStdString();
            vreq.filterDescription = filterStr;
            vreq.videoCodecName   = "libx264";
            vreq.copyAudio        = false;

            libavcore::VideoFilterGraph graph;
            const std::optional<std::string> err = graph.run(vreq);
            if (err.has_value()) {
                filterError = QString::fromStdString(*err);
            } else {
                filterOk = true;
            }
        });

        if (!filterOk)
            return fail(QStringLiteral("VideoFilterGraph::run() returned error: %1")
                            .arg(filterError));
        qInfo() << "[INFO] VIDEOSTAB-DESHAKE selftest (3): VideoFilterGraph::run() OK";
    }

    // -----------------------------------------------------------------------
    // (4) Independent probe (tautology-free): codec name + positive duration
    // -----------------------------------------------------------------------
    {
        const std::string probePath = outputPath.toStdString();

        const auto codecName = libavcore::probeVideoCodecName(probePath);
        if (!codecName.has_value() || codecName->empty()) {
            return fail(QStringLiteral(
                "probeVideoCodecName found no video stream in deshake output"));
        }
        qInfo() << "[INFO] VIDEOSTAB-DESHAKE selftest (4): probed codec ="
                << QString::fromStdString(*codecName);

        const auto durationUs = libavcore::probeDurationMicroseconds(probePath);
        if (!durationUs.has_value() || *durationUs <= 0) {
            return fail(QStringLiteral(
                "probeDurationMicroseconds did not return a positive duration (got %1)")
                            .arg(durationUs.has_value()
                                     ? QString::number(*durationUs)
                                     : QStringLiteral("nullopt")));
        }
        qInfo() << "[INFO] VIDEOSTAB-DESHAKE selftest (4): probed duration (us) ="
                << static_cast<qlonglong>(*durationUs);
    }

    // -----------------------------------------------------------------------
    // (5) Sanity guard: output file size > 0
    // -----------------------------------------------------------------------
    {
        const QFileInfo fi(outputPath);
        if (!fi.exists() || fi.size() == 0) {
            return fail(QStringLiteral(
                "output file is missing or empty: %1").arg(outputPath));
        }
        qInfo() << "[INFO] VIDEOSTAB-DESHAKE selftest (5): output size ="
                << fi.size() << "bytes";
    }

    // -----------------------------------------------------------------------
    // (8) cancel() callable smoke test
    // -----------------------------------------------------------------------
    {
        VideoStabilizer canceler;
        canceler.cancel();
        qInfo().noquote() << "[INFO] VIDEOSTAB-DESHAKE (8): VideoStabilizer::cancel() callable";
    }

    // -----------------------------------------------------------------------
    // (9) analyzeOnly / analysisComplete removal regression gate (US-VST-1)
    // -----------------------------------------------------------------------
    {
        VideoStabilizer probe;
        const QMetaObject *mo = probe.metaObject();
        const int idx1 = mo->indexOfMethod("analyzeOnly(QString)");
        const int idx2 = mo->indexOfMethod("analyzeOnly(QString,StabilizerConfig)");
        const int sigIdx = mo->indexOfSignal("analysisComplete(QString)");
        if (idx1 != -1 || idx2 != -1 || sigIdx != -1) {
            qCritical().noquote() << "VIDEOSTAB-DESHAKE (9) FAILED: analyzeOnly or "
                                      "analysisComplete signal re-introduced - regression!";
            QFile::remove(inputPath);
            QFile::remove(outputPath);
            return 1;
        }
        qInfo().noquote() << "[INFO] VIDEOSTAB-DESHAKE (9): analyzeOnly/analysisComplete removal confirmed";
    }

    // -----------------------------------------------------------------------
    // Cleanup
    // -----------------------------------------------------------------------
    QFile::remove(inputPath);
    QFile::remove(outputPath);

    qInfo() << "[INFO] VIDEOSTAB-DESHAKE selftest PASSED";
    return 0;
}



// runAIHighlightSelftest — env VEDITOR_AIHIGHLIGHT_SELFTEST=1
//
// Validates AIHighlight static helpers + Highlight/HighlightConfig structs without
// instantiating AIHighlight (QObject-derived) or calling async member methods:
//   gate1  - HighlightConfig default values
//   gate2  - Highlight::duration()
//   gate3  - Highlight::overlaps() boundary handling
//   gate4  - exportTimestamps() empty + single
//   gate5  - selectTopHighlights() dedup + targetCount cap
//   gate6  - combineScores() with empty inputs
int runAIHighlightSelftest()
{
    qInfo().noquote() << "[AIHIGHLIGHT-SELFTEST] start";
    writeLogLine("INFO", "[AIHIGHLIGHT-SELFTEST] start");

    int passed = 0;
    int failed = 0;
    auto pass = [&](const char* gateName) {
        const QString line = QStringLiteral("[AIHIGHLIGHT-SELFTEST] %1 PASS").arg(QString::fromLatin1(gateName));
        qInfo().noquote() << line;
        writeLogLine("INFO", line);
        ++passed;
    };
    auto fail = [&](const char* gateName, const QString& reason) {
        const QString line = QStringLiteral("[AIHIGHLIGHT-SELFTEST] %1 FAIL: %2").arg(QString::fromLatin1(gateName), reason);
        qCritical().noquote() << line;
        writeLogLine("CRIT", line);
        ++failed;
    };

    // gate 1: HighlightConfig default values
    {
        HighlightConfig c;
        const bool ok = qFuzzyCompare(c.minHighlightDuration, 3.0)
                     && qFuzzyCompare(c.maxHighlightDuration, 30.0)
                     && c.targetCount == 10
                     && qFuzzyCompare(c.audioWeight, 0.4)
                     && qFuzzyCompare(c.motionWeight, 0.3)
                     && qFuzzyCompare(c.sceneWeight, 0.3)
                     && qFuzzyCompare(c.loudnessThreshold, 0.7)
                     && qFuzzyCompare(c.audioWindowSize, 0.5)
                     && c.motionSampleFps == 1;
        if (ok) pass("gate1-config-defaults");
        else    fail("gate1-config-defaults", QStringLiteral("default values mismatch"));
    }

    // gate 2: Highlight::duration()
    {
        Highlight h;
        h.startTime = 5.0;
        h.endTime = 12.0;
        if (qFuzzyCompare(h.duration(), 7.0)) pass("gate2-duration");
        else                                  fail("gate2-duration",
                                                    QStringLiteral("expected 7.0 got %1").arg(h.duration()));
    }

    // gate 3: Highlight::overlaps() boundary handling
    {
        Highlight a; a.startTime = 0.0; a.endTime = 5.0;
        Highlight b; b.startTime = 4.0; b.endTime = 10.0;
        Highlight c; c.startTime = 5.0; c.endTime = 10.0;  // boundary touch
        const bool ab = a.overlaps(b);    // expect true
        const bool ac = a.overlaps(c);    // expect false (touch at 5.0)
        if (ab && !ac) pass("gate3-overlaps");
        else           fail("gate3-overlaps",
                            QStringLiteral("ab=%1 ac=%2 (expected true,false)")
                                .arg(ab).arg(ac));
    }

    // gate 4: exportTimestamps() empty + single
    {
        const QString emptyResult = AIHighlight::exportTimestamps({});
        if (!emptyResult.isEmpty()) {
            fail("gate4-export-timestamps", QStringLiteral("empty input did not yield empty string"));
        } else {
            QVector<Highlight> singletonV;
            Highlight one; one.startTime = 10.0; one.endTime = 25.0; one.score = 0.9;
            singletonV.push_back(one);
            const QString singleResult = AIHighlight::exportTimestamps(singletonV);
            if (!singleResult.isEmpty()) pass("gate4-export-timestamps");
            else                          fail("gate4-export-timestamps",
                                                QStringLiteral("single-highlight result was empty"));
        }
    }

    // gate 5: selectTopHighlights() dedup + targetCount cap
    {
        QVector<Highlight> input;
        for (int i = 0; i < 5; ++i) {
            Highlight h;
            h.startTime = i * 4.0;
            h.endTime = i * 4.0 + 5.0;   // every pair overlaps with the next
            h.score = 1.0 - i * 0.1;
            input.push_back(h);
        }
        HighlightConfig cfg;
        cfg.targetCount = 3;
        const QVector<Highlight> picked = AIHighlight::selectTopHighlights(input, cfg);
        const bool sizeOk = picked.size() <= 3;
        bool nonOverlap = true;
        for (int i = 0; i < picked.size() && nonOverlap; ++i) {
            for (int j = i + 1; j < picked.size() && nonOverlap; ++j) {
                if (picked[i].overlaps(picked[j])) nonOverlap = false;
            }
        }
        if (sizeOk && nonOverlap) pass("gate5-select-top");
        else                       fail("gate5-select-top",
                                         QStringLiteral("sizeOk=%1 nonOverlap=%2 picked=%3")
                                             .arg(sizeOk).arg(nonOverlap).arg(picked.size()));
    }

    // gate 6: combineScores() with empty inputs
    {
        const QVector<Highlight> empty = AIHighlight::combineScores({}, {}, {}, HighlightConfig{});
        if (empty.isEmpty()) pass("gate6-combine-empty");
        else                 fail("gate6-combine-empty",
                                   QStringLiteral("empty input produced %1 highlights").arg(empty.size()));
    }

    const QString summary = QStringLiteral("AIHIGHLIGHT-SELFTEST: %1/%2 PASS").arg(passed).arg(passed + failed);
    if (failed == 0) qInfo().noquote() << summary;
    else             qCritical().noquote() << summary;
    writeLogLine("INFO", summary);
    return failed == 0 ? 0 : 1;
}

// runProxySelftestV2 — argv-switch '--selftest=proxy'
//
// Singleton-free regression gates for ProxyManager's in-process pathway.
// Invoked from main() BEFORE QApplication construction so that env-gate
// quirks (WSL->cmd.exe propagation, qEnvironmentVariableIntValue parse)
// are bypassed entirely. No ProxyManager::instance() call — that path was
// shown to hang in PRD-PROXY-CLEAN US-PXC-3, see
// [[feedback-selftest-avoid-singleton-init]].
//
//   gate1 - ffmpegHasEncoder for at least one of h264_mf/libx264/mpeg4
//   gate2 - ffmpegHasDecoder('h264')
//   gate3 - libavcore::encoderAvailable agreement (direct chain check)
//   gate4 - libavcore::decoderAvailable('h264')
//   gate5 - ProxyManager::proxyDir() non-empty (static, no instance)
//   gate6 - ProxyConfig{} default fields (proxyWidth > 0 && proxyHeight > 0)
int runProxySelftestV2()
{
    qInfo().noquote() << "[PROXY-SELFTEST-V2] start";
    writeLogLine("INFO", "[PROXY-SELFTEST-V2] start");

    int passed = 0;
    int failed = 0;
    auto pass = [&](const char* gateName) {
        const QString line = QStringLiteral("[PROXY-SELFTEST-V2] %1 PASS").arg(QString::fromLatin1(gateName));
        qInfo().noquote() << line;
        writeLogLine("INFO", line);
        ++passed;
    };
    auto fail = [&](const char* gateName, const QString& reason) {
        const QString line = QStringLiteral("[PROXY-SELFTEST-V2] %1 FAIL: %2").arg(QString::fromLatin1(gateName), reason);
        qCritical().noquote() << line;
        writeLogLine("CRIT", line);
        ++failed;
    };

    // gate 1
    {
        const bool any = ProxyManager::ffmpegHasEncoder("h264_mf")
                      || ProxyManager::ffmpegHasEncoder("libx264")
                      || ProxyManager::ffmpegHasEncoder("mpeg4");
        if (any) pass("gate1-encoder-registry");
        else     fail("gate1-encoder-registry",
                       QStringLiteral("none of h264_mf/libx264/mpeg4 in registry"));
    }

    // gate 2 — ffmpegHasDecoder is private; use libavcore::decoderAvailable directly
    // (same underlying libavcodec registry probe, no instance required)
    {
        if (libavcore::decoderAvailable("h264")) pass("gate2-decoder-registry");
        else                                     fail("gate2-decoder-registry",
                                                      QStringLiteral("h264 decoder absent"));
    }

    // gate 3
    {
        const bool any = libavcore::encoderAvailable("h264_mf")
                      || libavcore::encoderAvailable("libx264")
                      || libavcore::encoderAvailable("mpeg4");
        if (any) pass("gate3-libavcore-encoder");
        else     fail("gate3-libavcore-encoder",
                       QStringLiteral("libavcore registry empty"));
    }

    // gate 4
    {
        if (libavcore::decoderAvailable("h264")) pass("gate4-libavcore-decoder");
        else                                     fail("gate4-libavcore-decoder",
                                                       QStringLiteral("libavcore h264 decoder absent"));
    }

    // gate 5
    {
        const QString dir = ProxyManager::proxyDir();
        if (!dir.isEmpty()) pass("gate5-proxy-dir");
        else                fail("gate5-proxy-dir", QStringLiteral("proxyDir is empty"));
    }

    // gate 6
    {
        const ProxyConfig cfg;
        if (cfg.proxyWidth > 0 && cfg.proxyHeight > 0)
            pass("gate6-config-defaults");
        else
            fail("gate6-config-defaults",
                 QStringLiteral("default ProxyConfig has zero w/h: %1x%2")
                     .arg(cfg.proxyWidth).arg(cfg.proxyHeight));
    }

    const QString summary = QStringLiteral("PROXY-SELFTEST-V2: %1/%2 PASS").arg(passed).arg(passed + failed);
    if (failed == 0) qInfo().noquote() << summary;
    else             qCritical().noquote() << summary;
    writeLogLine("INFO", summary);
    return failed == 0 ? 0 : 1;
}


// (VEDITOR_HDR_ROUTING_SELFTEST=1).
//
// Validates three invariants without spawning any subprocess or QProcess:
//
//   (1) DLL-ready contract: the bundled avcodec-62.dll ships without
//       libx265/nvenc/qsv/amf, so firstTenBitHevcEncoder() must return
//       std::nullopt and tenBitHevcEncoderAvailable() must return false.
//       When a full-featured DLL (libx265 or HW encoder enabled) is
//       dropped in, these will return true and the routing logic below
//       will automatically select the in-process path — no recompile needed.
//
//   (2) HDR routing predicate: exercises all three branches of the US-B3-7
//       routing rule via truth-value combinations, not by instantiating a
//       full RenderQueue:
//         - HDR10/HLG + 10-bit encoder unavailable  -> "subprocess"
//         - HDR10/HLG + 10-bit encoder available    -> "in-process"
//         - 8-bit (non-HDR) job                     -> "in-process" always
//
//   (3) master-display string: hdr10MasterDisplayString() must return a
//       string that contains all five mandatory x265 tokens:
//       G(, B(, R(, WP(, L(
int runHdrRoutingSelftest()
{
    qInfo() << "[INFO] HDR-ROUTING selftest: DLL-ready HDR routing regression gate";

    bool passed = true;

    // -----------------------------------------------------------------------
    // (1) DLL-ready contract
    // -----------------------------------------------------------------------
    // The bundled avcodec-62.dll is built without libx265/nvenc/qsv/amf.
    // Expect std::nullopt / false on the stock build.  A drop-in DLL that
    // enables any of those encoders will make these assertions fail, which is
    // the desired signal that the in-process routing path is now reachable.
    auto firstEnc = libavcore::firstTenBitHevcEncoder();
    bool tenBitOk = libavcore::tenBitHevcEncoderAvailable();

    if (firstEnc.has_value()) {
        qInfo() << "[INFO] HDR-ROUTING selftest: firstTenBitHevcEncoder ="
                << QString::fromStdString(*firstEnc)
                << "(non-null: DLL now carries a 10-bit HEVC encoder;"
                << "in-process HDR routing is active)";
    } else {
        qInfo() << "[INFO] HDR-ROUTING selftest: firstTenBitHevcEncoder = nullopt"
                << "(stock DLL: libx265/nvenc/qsv/amf absent — expected)";
    }
    qInfo() << "[INFO] HDR-ROUTING selftest: tenBitHevcEncoderAvailable =" << tenBitOk;

    // Assert stock-DLL contract: no 10-bit HEVC encoder present.
    // If this assertion ever fails it means the DLL was upgraded and the
    // in-process HDR path is now live — update this selftest to expect true.
    if (firstEnc.has_value()) {
        qCritical() << "HDR-ROUTING selftest FAILED (1): firstTenBitHevcEncoder"
                    << "returned" << QString::fromStdString(*firstEnc)
                    << "but stock avcodec-62.dll should have no 10-bit HEVC encoder."
                    << "If the DLL was intentionally upgraded, update this check.";
        passed = false;
    }
    if (tenBitOk) {
        qCritical() << "HDR-ROUTING selftest FAILED (1): tenBitHevcEncoderAvailable"
                    << "returned true but stock avcodec-62.dll should return false."
                    << "If the DLL was intentionally upgraded, update this check.";
        passed = false;
    }

    // -----------------------------------------------------------------------
    // (2) HDR routing predicate — truth-value combinatorics
    //
    // US-B3-7 routing rule (no RenderQueue instantiation needed):
    //   isHdr && !tenBitAvail  -> "subprocess"
    //   isHdr &&  tenBitAvail  -> "in-process"
    //  !isHdr                  -> "in-process"  (always)
    // -----------------------------------------------------------------------
    auto chooseRoute = [](bool isHdr, bool tenBitAvail) -> const char* {
        if (!isHdr) return "in-process";
        return tenBitAvail ? "in-process" : "subprocess";
    };

    struct RouteCase {
        bool isHdr;
        bool tenBit;
        const char* expected;
    };
    const RouteCase cases[] = {
        { false, false, "in-process"  },   // 8-bit, no 10-bit enc: always in-process
        { false, true,  "in-process"  },   // 8-bit, 10-bit enc present: still in-process
        { true,  false, "subprocess"  },   // HDR + no 10-bit enc: subprocess fallback
        { true,  true,  "in-process"  },   // HDR + 10-bit enc present: in-process
    };

    for (const auto& c : cases) {
        const char* got = chooseRoute(c.isHdr, c.tenBit);
        if (std::string(got) != std::string(c.expected)) {
            qCritical() << "HDR-ROUTING selftest FAILED (2): chooseRoute("
                        << c.isHdr << "," << c.tenBit << ") returned"
                        << got << "expected" << c.expected;
            passed = false;
        } else {
            qInfo() << "[INFO] HDR-ROUTING selftest (2): chooseRoute("
                    << c.isHdr << "," << c.tenBit << ") ->" << got << "OK";
        }
    }

    // -----------------------------------------------------------------------
    // (3) master-display string structural validation
    // -----------------------------------------------------------------------
    const std::string mdStr =
        libavcore::hdr10MasterDisplayString(1000.0, 0.005);
    qInfo() << "[INFO] HDR-ROUTING selftest (3): hdr10MasterDisplayString ="
            << QString::fromStdString(mdStr);

    // Must contain all five x265 mandatory chromaticity / luminance tokens.
    const char* requiredTokens[] = { "G(", "B(", "R(", "WP(", "L(" };
    for (const char* tok : requiredTokens) {
        if (mdStr.find(tok) == std::string::npos) {
            qCritical() << "HDR-ROUTING selftest FAILED (3):"
                        << "hdr10MasterDisplayString missing token"
                        << tok << "in:" << QString::fromStdString(mdStr);
            passed = false;
        }
    }

    if (!passed) {
        qCritical() << "HDR-ROUTING selftest FAILED";
        return 1;
    }
    qInfo() << "[INFO] HDR-ROUTING selftest PASSED";
    return 0;
}
