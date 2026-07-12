#include "AIHighlight.h"
#include "WaveformGenerator.h"
#include "libavcore/Decode.h"
#include "libavcore/Encode.h"
#include <QByteArray>
#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <cmath>
#include <algorithm>
#include <limits>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace {

struct AvFrameDeleter {
    void operator()(AVFrame *frame) const
    {
        if (frame) av_frame_free(&frame);
    }
};

using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

struct SwrContextDeleter {
    void operator()(SwrContext *ctx) const
    {
        if (ctx) swr_free(&ctx);
    }
};

using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

struct ChannelLayoutGuard {
    AVChannelLayout layout = {};

    ~ChannelLayoutGuard()
    {
        av_channel_layout_uninit(&layout);
    }

    ChannelLayoutGuard() = default;
    ChannelLayoutGuard(const ChannelLayoutGuard&) = delete;
    ChannelLayoutGuard& operator=(const ChannelLayoutGuard&) = delete;
};

static double framePtsSeconds(const AVFrame *frame, AVRational timeBase)
{
    if (!frame) return std::numeric_limits<double>::quiet_NaN();

    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
        pts = frame->pts;
    if (pts == AV_NOPTS_VALUE)
        return std::numeric_limits<double>::quiet_NaN();

    return static_cast<double>(pts) * av_q2d(timeBase);
}

static int fpsFromVideoProps(const libavcore::VideoStreamProps &props)
{
    double fps = av_q2d(props.frameRate);
    if (!std::isfinite(fps) || fps <= 0.0)
        fps = 30.0;
    return qMax(1, static_cast<int>(std::round(fps)));
}

static AVSampleFormat firstSupportedAacSampleFormat()
{
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!encoder) return AV_SAMPLE_FMT_FLTP;

    const void *cfg = nullptr;
    int numCfg = 0;
    const int rc = avcodec_get_supported_config(
        nullptr, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &numCfg);
    if (rc >= 0 && cfg && numCfg > 0) {
        const AVSampleFormat *formats =
            static_cast<const AVSampleFormat*>(cfg);
        return formats[0];
    }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    const AVSampleFormat *legacy = encoder->sample_fmts;
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (legacy && legacy[0] != AV_SAMPLE_FMT_NONE)
        return legacy[0];
    return AV_SAMPLE_FMT_FLTP;
}

static bool copyOrDefaultChannelLayout(ChannelLayoutGuard &dst,
                                       const AVChannelLayout &src,
                                       int channels)
{
    if (src.nb_channels > 0)
        return av_channel_layout_copy(&dst.layout, &src) >= 0;

    if (channels <= 0) return false;
    av_channel_layout_default(&dst.layout, channels);
    return dst.layout.nb_channels > 0;
}

static bool makeDefaultChannelLayout(ChannelLayoutGuard &dst, int channels)
{
    if (channels <= 0) return false;
    av_channel_layout_default(&dst.layout, channels);
    return dst.layout.nb_channels > 0;
}

static QVector<Highlight> chronologicalHighlights(
    const QVector<Highlight> &highlights)
{
    QVector<Highlight> ordered = highlights;
    std::sort(ordered.begin(), ordered.end(),
              [](const Highlight &a, const Highlight &b) {
                  return a.startTime < b.startTime;
              });
    return ordered;
}

QMutex g_aiHighlightThreadsMutex;
QHash<const QObject*, QVector<QThread*>> g_aiHighlightThreads;

bool aiHighlightInterrupted()
{
    QThread *thread = QThread::currentThread();
    return thread && thread->isInterruptionRequested();
}

void unregisterAIHighlightThread(const QObject *owner, QThread *thread)
{
    QMutexLocker locker(&g_aiHighlightThreadsMutex);
    auto it = g_aiHighlightThreads.find(owner);
    if (it == g_aiHighlightThreads.end())
        return;

    it->removeAll(thread);
    if (it->isEmpty())
        g_aiHighlightThreads.erase(it);
}

void trackAIHighlightThread(AIHighlight *owner, QThread *thread)
{
    {
        QMutexLocker locker(&g_aiHighlightThreadsMutex);
        g_aiHighlightThreads[owner].append(thread);
    }

    QObject::connect(thread, &QThread::finished, [owner, thread]() {
        unregisterAIHighlightThread(owner, thread);
    });
    QObject::connect(owner, &QObject::destroyed, thread, [owner, thread]() {
        thread->requestInterruption();
        thread->wait();
        unregisterAIHighlightThread(owner, thread);
    }, Qt::DirectConnection);
    QObject::connect(thread, &QThread::finished, thread, &QThread::deleteLater);
}

void postProgressChanged(const QPointer<AIHighlight> &owner, int percent)
{
    AIHighlight *target = owner.data();
    if (!target)
        return;

    QMetaObject::invokeMethod(target, [owner, percent]() {
        if (owner)
            emit owner->progressChanged(percent);
    }, Qt::QueuedConnection);
}

void postAnalysisComplete(const QPointer<AIHighlight> &owner, const QVector<Highlight> &highlights)
{
    AIHighlight *target = owner.data();
    if (!target)
        return;

    QMetaObject::invokeMethod(target, [owner, highlights]() {
        if (owner)
            emit owner->analysisComplete(highlights);
    }, Qt::QueuedConnection);
}

void postExportComplete(const QPointer<AIHighlight> &owner, bool success, const QString &message)
{
    AIHighlight *target = owner.data();
    if (!target)
        return;

    QMetaObject::invokeMethod(target, [owner, success, message]() {
        if (owner)
            emit owner->exportComplete(success, message);
    }, Qt::QueuedConnection);
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

AIHighlight::AIHighlight(QObject *parent) : QObject(parent) {}

// ---------------------------------------------------------------------------
// analyze — full async pipeline
// ---------------------------------------------------------------------------

void AIHighlight::analyze(const QString &filePath, const HighlightConfig &config)
{
    QPointer<AIHighlight> owner(this);
    auto *thread = QThread::create([owner, filePath, config]() {
        postProgressChanged(owner, 0);

        // Pass 1: Audio energy (0-30%)
        auto audioScores = analyzeAudioEnergy(filePath, config);
        if (aiHighlightInterrupted())
            return;
        postProgressChanged(owner, 30);

        // Pass 2: Motion activity (30-60%)
        auto motionScores = analyzeMotionActivity(filePath, config);
        if (aiHighlightInterrupted())
            return;
        postProgressChanged(owner, 60);

        // Pass 3: Scene changes (60-80%)
        auto sceneScores = analyzeSceneChanges(filePath, config);
        if (aiHighlightInterrupted())
            return;
        postProgressChanged(owner, 80);

        // Combine and select (80-100%)
        auto allHighlights = combineScores(audioScores, motionScores, sceneScores, config);
        auto selected = selectTopHighlights(allHighlights, config);
        if (aiHighlightInterrupted())
            return;
        postProgressChanged(owner, 100);

        postAnalysisComplete(owner, selected);
    });
    trackAIHighlightThread(this, thread);
    thread->start();
}

// ---------------------------------------------------------------------------
// analyzeAudioEnergy — RMS energy in sliding windows
// (reuses WaveformGenerator's decodeAudio pattern)
// ---------------------------------------------------------------------------

QVector<ScoredSegment> AIHighlight::analyzeAudioEnergy(const QString &filePath,
                                                        const HighlightConfig &config)
{
    QVector<ScoredSegment> scores;

    // Generate high-resolution waveform (100 peaks/sec for fine granularity)
    WaveformData waveform = WaveformGenerator::generate(filePath, 100);
    if (waveform.isEmpty())
        return scores;

    // Compute RMS energy in sliding windows
    int windowPeaks = static_cast<int>(config.audioWindowSize * waveform.peaksPerSecond);
    if (windowPeaks < 1) windowPeaks = 1;

    double secondsPerPeak = 1.0 / waveform.peaksPerSecond;

    for (int i = 0; i <= waveform.peaks.size() - windowPeaks; i += windowPeaks / 2) {
        double sumSq = 0.0;
        for (int j = i; j < i + windowPeaks && j < waveform.peaks.size(); ++j) {
            double val = waveform.peaks[j];
            sumSq += val * val;
        }
        double rms = std::sqrt(sumSq / windowPeaks);

        ScoredSegment seg;
        seg.time = (i + windowPeaks / 2.0) * secondsPerPeak;
        seg.score = rms;
        scores.append(seg);
    }

    normalizeScores(scores);
    return scores;
}

// ---------------------------------------------------------------------------
// analyzeMotionActivity — frame differencing at reduced fps
// (reuses AutoEdit/MotionTracker frame decode pattern)
// ---------------------------------------------------------------------------

QVector<ScoredSegment> AIHighlight::analyzeMotionActivity(const QString &filePath,
                                                           const HighlightConfig &config)
{
    QVector<ScoredSegment> scores;

    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return scores;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return scores;
    }

    int videoIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        return scores;
    }

    auto *codecpar = fmtCtx->streams[videoIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmtCtx); return scores; }

    AVCodecContext *decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decCtx, codecpar);
    if (avcodec_open2(decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return scores;
    }

    // Scale to small grayscale for fast comparison (same as AutoEdit pattern)
    int cmpW = 160, cmpH = 90;
    SwsContext *swsCtx = sws_getContext(
        decCtx->width, decCtx->height, decCtx->pix_fmt,
        cmpW, cmpH, AV_PIX_FMT_GRAY8,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return scores;
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    QVector<uint8_t> prevFrame(cmpW * cmpH, 0);
    QVector<uint8_t> currFrame(cmpW * cmpH, 0);
    bool hasPrev = false;

    AVStream *stream = fmtCtx->streams[videoIdx];
    double fps = av_q2d(stream->avg_frame_rate);
    if (fps <= 0.0) fps = 25.0;

    // Skip frames to achieve target sample fps
    int skipInterval = qMax(1, static_cast<int>(fps / config.motionSampleFps));
    int frameCount = 0;

    while (!aiHighlightInterrupted() && av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != videoIdx) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (!aiHighlightInterrupted() && avcodec_receive_frame(decCtx, frame) == 0) {
                frameCount++;
                if (frameCount % skipInterval != 0) continue;

                // Scale to grayscale
                uint8_t *dest[1] = { currFrame.data() };
                int destLinesize[1] = { cmpW };
                sws_scale(swsCtx, frame->data, frame->linesize, 0,
                          frame->height, dest, destLinesize);

                if (hasPrev) {
                    // Compute mean absolute pixel difference
                    double totalDiff = 0.0;
                    int pixelCount = cmpW * cmpH;
                    for (int i = 0; i < pixelCount; ++i)
                        totalDiff += std::abs(currFrame[i] - prevFrame[i]);
                    double avgDiff = totalDiff / (pixelCount * 255.0);

                    double time = frame->pts * av_q2d(stream->time_base);
                    ScoredSegment seg;
                    seg.time = time;
                    seg.score = avgDiff;
                    scores.append(seg);
                }

                std::swap(prevFrame, currFrame);
                hasPrev = true;
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    if (swsCtx) sws_freeContext(swsCtx);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    normalizeScores(scores);
    return scores;
}

// ---------------------------------------------------------------------------
// analyzeSceneChanges — same frame differencing with higher threshold
// (reuses AutoEdit::detectSceneChanges pattern)
// ---------------------------------------------------------------------------

QVector<ScoredSegment> AIHighlight::analyzeSceneChanges(const QString &filePath,
                                                         const HighlightConfig &config)
{
    QVector<ScoredSegment> scores;

    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return scores;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return scores;
    }

    int videoIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        return scores;
    }

    auto *codecpar = fmtCtx->streams[videoIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmtCtx); return scores; }

    AVCodecContext *decCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(decCtx, codecpar);
    if (avcodec_open2(decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return scores;
    }

    int cmpW = 160, cmpH = 90;
    SwsContext *swsCtx = sws_getContext(
        decCtx->width, decCtx->height, decCtx->pix_fmt,
        cmpW, cmpH, AV_PIX_FMT_GRAY8,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return scores;
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    QVector<uint8_t> prevFrame(cmpW * cmpH, 0);
    QVector<uint8_t> currFrame(cmpW * cmpH, 0);
    bool hasPrev = false;
    int frameCount = 0;

    AVStream *stream = fmtCtx->streams[videoIdx];
    // Check every 5 frames (same as AutoEdit default)
    int checkInterval = 5;

    while (!aiHighlightInterrupted() && av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != videoIdx) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (!aiHighlightInterrupted() && avcodec_receive_frame(decCtx, frame) == 0) {
                frameCount++;
                if (frameCount % checkInterval != 0) continue;

                uint8_t *dest[1] = { currFrame.data() };
                int destLinesize[1] = { cmpW };
                sws_scale(swsCtx, frame->data, frame->linesize, 0,
                          frame->height, dest, destLinesize);

                if (hasPrev) {
                    double totalDiff = 0.0;
                    int pixelCount = cmpW * cmpH;
                    for (int i = 0; i < pixelCount; ++i)
                        totalDiff += std::abs(currFrame[i] - prevFrame[i]);
                    double avgDiff = totalDiff / (pixelCount * 255.0);

                    // Scene change threshold — only record significant transitions
                    // Score is 0 for small diffs, ramps up for large diffs
                    double sceneThreshold = 0.15;
                    double sceneScore = (avgDiff > sceneThreshold)
                        ? qMin((avgDiff - sceneThreshold) / (1.0 - sceneThreshold), 1.0)
                        : 0.0;

                    if (sceneScore > 0.0) {
                        double time = frame->pts * av_q2d(stream->time_base);
                        ScoredSegment seg;
                        seg.time = time;
                        seg.score = sceneScore;
                        scores.append(seg);
                    }
                }

                std::swap(prevFrame, currFrame);
                hasPrev = true;
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);
    if (swsCtx) sws_freeContext(swsCtx);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);

    normalizeScores(scores);
    return scores;
}

// ---------------------------------------------------------------------------
// normalizeScores — scale all scores to 0.0-1.0
// ---------------------------------------------------------------------------

void AIHighlight::normalizeScores(QVector<ScoredSegment> &segments)
{
    if (segments.isEmpty()) return;

    double maxScore = 0.0;
    for (const auto &s : segments)
        maxScore = qMax(maxScore, s.score);

    if (maxScore < 1e-10) return;

    for (auto &s : segments)
        s.score /= maxScore;
}

// ---------------------------------------------------------------------------
// combineScores — weighted merge of all metrics into highlight candidates
// ---------------------------------------------------------------------------

QVector<Highlight> AIHighlight::combineScores(const QVector<ScoredSegment> &audioScores,
                                               const QVector<ScoredSegment> &motionScores,
                                               const QVector<ScoredSegment> &sceneScores,
                                               const HighlightConfig &config)
{
    // Build a unified time grid at 1-second resolution
    double maxTime = 0.0;
    for (const auto &s : audioScores)  maxTime = qMax(maxTime, s.time);
    for (const auto &s : motionScores) maxTime = qMax(maxTime, s.time);
    for (const auto &s : sceneScores)  maxTime = qMax(maxTime, s.time);

    if (maxTime <= 0.0) return {};

    int gridSize = static_cast<int>(std::ceil(maxTime)) + 1;
    QVector<double> audioGrid(gridSize, 0.0);
    QVector<double> motionGrid(gridSize, 0.0);
    QVector<double> sceneGrid(gridSize, 0.0);

    // Map scored segments onto the time grid (nearest second)
    auto mapToGrid = [gridSize](const QVector<ScoredSegment> &segs, QVector<double> &grid) {
        for (const auto &s : segs) {
            int idx = static_cast<int>(std::round(s.time));
            if (idx >= 0 && idx < gridSize)
                grid[idx] = qMax(grid[idx], s.score);
        }
    };

    mapToGrid(audioScores, audioGrid);
    mapToGrid(motionScores, motionGrid);
    mapToGrid(sceneScores, sceneGrid);

    // Weighted combination into unified scores
    QVector<ScoredSegment> combinedScores(gridSize);
    for (int i = 0; i < gridSize; ++i) {
        combinedScores[i].time = static_cast<double>(i);
        combinedScores[i].score = audioGrid[i] * config.audioWeight
                                + motionGrid[i] * config.motionWeight
                                + sceneGrid[i] * config.sceneWeight;
    }

    return buildHighlightsFromScores(combinedScores, config);
}

// ---------------------------------------------------------------------------
// buildHighlightsFromScores — convert time-series scores into highlight regions
// ---------------------------------------------------------------------------

QVector<Highlight> AIHighlight::buildHighlightsFromScores(
    const QVector<ScoredSegment> &combinedScores,
    const HighlightConfig &config)
{
    QVector<Highlight> highlights;
    if (combinedScores.isEmpty()) return highlights;

    // Threshold: consider a second "interesting" if combined score exceeds median
    QVector<double> allScores;
    allScores.reserve(combinedScores.size());
    for (const auto &s : combinedScores)
        allScores.append(s.score);
    std::sort(allScores.begin(), allScores.end());

    double threshold = allScores[allScores.size() * 3 / 4]; // 75th percentile
    if (threshold < 0.1) threshold = 0.1;

    // Find contiguous regions above threshold
    bool inRegion = false;
    double regionStart = 0.0;
    double regionMaxScore = 0.0;

    for (int i = 0; i < combinedScores.size(); ++i) {
        bool above = combinedScores[i].score >= threshold;

        if (above && !inRegion) {
            inRegion = true;
            regionStart = combinedScores[i].time;
            regionMaxScore = combinedScores[i].score;
        } else if (above && inRegion) {
            regionMaxScore = qMax(regionMaxScore, combinedScores[i].score);
        } else if (!above && inRegion) {
            inRegion = false;
            double regionEnd = combinedScores[i].time;
            double duration = regionEnd - regionStart;

            // Pad short regions to minimum duration
            if (duration < config.minHighlightDuration) {
                double pad = (config.minHighlightDuration - duration) / 2.0;
                regionStart = qMax(0.0, regionStart - pad);
                regionEnd = regionStart + config.minHighlightDuration;
            }

            // Truncate long regions
            if (regionEnd - regionStart > config.maxHighlightDuration) {
                regionEnd = regionStart + config.maxHighlightDuration;
            }

            Highlight h;
            h.startTime = regionStart;
            h.endTime = regionEnd;
            h.score = regionMaxScore;
            h.type = HighlightType::Combined;
            highlights.append(h);
        }
    }

    // Handle trailing region
    if (inRegion) {
        double regionEnd = combinedScores.last().time;
        double duration = regionEnd - regionStart;

        if (duration < config.minHighlightDuration) {
            double pad = (config.minHighlightDuration - duration) / 2.0;
            regionStart = qMax(0.0, regionStart - pad);
            regionEnd = regionStart + config.minHighlightDuration;
        }
        if (regionEnd - regionStart > config.maxHighlightDuration)
            regionEnd = regionStart + config.maxHighlightDuration;

        Highlight h;
        h.startTime = regionStart;
        h.endTime = regionEnd;
        h.score = regionMaxScore;
        h.type = HighlightType::Combined;
        highlights.append(h);
    }

    return highlights;
}

// ---------------------------------------------------------------------------
// selectTopHighlights — greedy non-overlapping selection by score
// ---------------------------------------------------------------------------

QVector<Highlight> AIHighlight::selectTopHighlights(const QVector<Highlight> &allHighlights,
                                                     const HighlightConfig &config)
{
    if (allHighlights.isEmpty()) return {};

    // Sort by score descending
    QVector<Highlight> sorted = allHighlights;
    std::sort(sorted.begin(), sorted.end(), [](const Highlight &a, const Highlight &b) {
        return a.score > b.score;
    });

    // Greedy selection: pick highest-scoring, skip overlapping
    QVector<Highlight> selected;
    for (const auto &candidate : sorted) {
        if (selected.size() >= config.targetCount)
            break;

        bool overlaps = false;
        for (const auto &existing : selected) {
            if (candidate.overlaps(existing)) {
                overlaps = true;
                break;
            }
        }

        if (!overlaps) {
            Highlight h = candidate;
            // Generate description
            int minutes = static_cast<int>(h.startTime) / 60;
            int seconds = static_cast<int>(h.startTime) % 60;
            h.description = QString("Highlight at %1:%2 (score: %3)")
                .arg(minutes, 2, 10, QChar('0'))
                .arg(seconds, 2, 10, QChar('0'))
                .arg(h.score, 0, 'f', 2);
            selected.append(h);
        }
    }

    // Sort selected by time for chronological order
    std::sort(selected.begin(), selected.end(), [](const Highlight &a, const Highlight &b) {
        return a.startTime < b.startTime;
    });

    return selected;
}

// ---------------------------------------------------------------------------
// exportHighlightReel — concatenate highlights via in-process libavcore
// ---------------------------------------------------------------------------

void AIHighlight::exportHighlightReel(const QString &inputPath, const QString &outputPath,
                                       const QVector<Highlight> &highlights)
{
    QPointer<AIHighlight> owner(this);
    auto *thread = QThread::create([owner, inputPath, outputPath, highlights]() {
        if (highlights.isEmpty()) {
            postExportComplete(owner, false, "No highlights to export");
            return;
        }

        QString failureMessage;
        const bool success = [&]() -> bool {
            libavcore::MediaDecoder decoder;
            const QByteArray inputUtf8 = inputPath.toUtf8();
            if (auto err = decoder.open(inputUtf8.constData(), true)) {
                failureMessage = QString::fromStdString(*err);
                return false;
            }

            const libavcore::VideoStreamProps videoProps = decoder.videoProps();
            if (videoProps.width <= 0 || videoProps.height <= 0) {
                failureMessage = QStringLiteral("invalid input video dimensions");
                return false;
            }

            libavcore::EncodeRequest request;
            request.width = videoProps.width;
            request.height = videoProps.height;
            request.fps = fpsFromVideoProps(videoProps);
            request.videoBitrateBits = std::max<int64_t>(
                4000000,
                static_cast<int64_t>(request.width)
                    * static_cast<int64_t>(request.height)
                    * static_cast<int64_t>(request.fps) / 6);
            request.outputPath = outputPath.toUtf8().constData();
            request.videoCodecName = "libx264";

            const bool hasAudio = decoder.hasAudio();
            libavcore::AudioStreamProps audioProps;
            ChannelLayoutGuard audioInLayout;
            ChannelLayoutGuard audioOutLayout;
            AVSampleFormat audioOutSampleFmt = AV_SAMPLE_FMT_NONE;

            if (hasAudio) {
                audioProps = decoder.audioProps();
                if (audioProps.sampleRate <= 0 || audioProps.channels <= 0
                    || audioProps.sampleFormat == AV_SAMPLE_FMT_NONE) {
                    failureMessage = QStringLiteral("invalid input audio stream");
                    return false;
                }
                if (!copyOrDefaultChannelLayout(audioInLayout,
                                                audioProps.channelLayout,
                                                audioProps.channels)
                    || !makeDefaultChannelLayout(audioOutLayout,
                                                audioProps.channels)) {
                    failureMessage = QStringLiteral("invalid input audio channel layout");
                    return false;
                }

                request.audioEncode = true;
                request.audioSampleRate = audioProps.sampleRate;
                request.audioChannels = audioProps.channels;
                request.audioBitrateBits = 192000;
                audioOutSampleFmt = firstSupportedAacSampleFormat();
            }

            libavcore::FrameEncoder encoder;
            if (auto err = encoder.open(request)) {
                failureMessage = QString::fromStdString(*err);
                return false;
            }

            AVPixelFormat targetPixFmt = encoder.outputPixelFormat();

            AvFramePtr videoOutFrame(av_frame_alloc());
            if (!videoOutFrame) {
                failureMessage = QStringLiteral("failed to allocate output video frame");
                return false;
            }
            videoOutFrame->format = targetPixFmt;
            videoOutFrame->width = request.width;
            videoOutFrame->height = request.height;
            if (av_frame_get_buffer(videoOutFrame.get(), 0) < 0) {
                failureMessage = QStringLiteral("failed to allocate output video buffer");
                return false;
            }

            SwsContext *swsCtx = nullptr;
            SwrContext *swrRaw = nullptr;
            if (hasAudio) {
                const int rc = swr_alloc_set_opts2(
                    &swrRaw,
                    &audioOutLayout.layout,
                    audioOutSampleFmt,
                    request.audioSampleRate,
                    &audioInLayout.layout,
                    audioProps.sampleFormat,
                    audioProps.sampleRate,
                    0,
                    nullptr);
                if (rc < 0 || !swrRaw || swr_init(swrRaw) < 0) {
                    if (swrRaw) swr_free(&swrRaw);
                    failureMessage = QStringLiteral("failed to initialize audio resampler");
                    return false;
                }
            }
            SwrContextPtr swrCtx(swrRaw);

            int64_t videoPts = 0;
            int64_t audioPts = 0;
            bool wroteVideo = false;
            int swsSrcWidth = 0;
            int swsSrcHeight = 0;
            AVPixelFormat swsSrcPixFmt = AV_PIX_FMT_NONE;
            const QVector<Highlight> orderedHighlights =
                chronologicalHighlights(highlights);

            auto pushVideo = [&](AVFrame *frame) -> bool {
                AVPixelFormat srcPixFmt =
                    static_cast<AVPixelFormat>(frame->format);
                if (srcPixFmt == AV_PIX_FMT_NONE)
                    srcPixFmt = videoProps.pixelFormat;

                if (!swsCtx
                    || swsSrcWidth != frame->width
                    || swsSrcHeight != frame->height
                    || swsSrcPixFmt != srcPixFmt) {
                    if (swsCtx) {
                        sws_freeContext(swsCtx);
                        swsCtx = nullptr;
                    }
                    swsCtx = sws_getContext(frame->width,
                                            frame->height,
                                            srcPixFmt,
                                            request.width,
                                            request.height,
                                            targetPixFmt,
                                            SWS_BILINEAR,
                                            nullptr,
                                            nullptr,
                                            nullptr);
                    if (!swsCtx) {
                        failureMessage = QStringLiteral(
                            "failed to initialize video scaler");
                        return false;
                    }
                    swsSrcWidth = frame->width;
                    swsSrcHeight = frame->height;
                    swsSrcPixFmt = srcPixFmt;
                }

                if (av_frame_make_writable(videoOutFrame.get()) < 0) {
                    failureMessage = QStringLiteral("failed to prepare output video frame");
                    return false;
                }

                sws_scale(swsCtx,
                          frame->data,
                          frame->linesize,
                          0,
                          frame->height,
                          videoOutFrame->data,
                          videoOutFrame->linesize);

                if (!encoder.pushFrameNative(videoOutFrame.get(), videoPts++)) {
                    failureMessage = QStringLiteral("encoder video frame push failed");
                    return false;
                }
                wroteVideo = true;
                return true;
            };

            auto pushAudio = [&](AVFrame *frame) -> bool {
                if (!swrCtx) return true;

                const int outSamples =
                    swr_get_out_samples(swrCtx.get(), frame->nb_samples);
                if (outSamples < 0) {
                    failureMessage = QStringLiteral("failed to size audio resampler output");
                    return false;
                }
                if (outSamples == 0) return true;

                AvFramePtr audioOutFrame(av_frame_alloc());
                if (!audioOutFrame) {
                    failureMessage = QStringLiteral("failed to allocate output audio frame");
                    return false;
                }
                audioOutFrame->format = audioOutSampleFmt;
                audioOutFrame->sample_rate = request.audioSampleRate;
                audioOutFrame->nb_samples = outSamples;
                if (av_channel_layout_copy(&audioOutFrame->ch_layout,
                                           &audioOutLayout.layout) < 0) {
                    failureMessage = QStringLiteral("failed to copy output audio layout");
                    return false;
                }
                if (av_frame_get_buffer(audioOutFrame.get(), 0) < 0) {
                    failureMessage = QStringLiteral("failed to allocate output audio buffer");
                    return false;
                }

                const int converted = swr_convert(
                    swrCtx.get(),
                    audioOutFrame->extended_data,
                    outSamples,
                    const_cast<const uint8_t**>(frame->extended_data),
                    frame->nb_samples);
                if (converted < 0) {
                    failureMessage = QStringLiteral("audio resample failed");
                    return false;
                }
                if (converted == 0) return true;

                audioOutFrame->nb_samples = converted;
                if (!encoder.pushAudioFrame(audioOutFrame.get(), audioPts)) {
                    failureMessage = QStringLiteral("encoder audio frame push failed");
                    return false;
                }
                audioPts += converted;
                return true;
            };

            auto flushAudio = [&]() -> bool {
                if (!swrCtx) return true;

                while (true) {
                    const int outSamples = swr_get_out_samples(swrCtx.get(), 0);
                    if (outSamples < 0) {
                        failureMessage = QStringLiteral(
                            "failed to size audio resampler flush");
                        return false;
                    }
                    if (outSamples == 0) return true;

                    AvFramePtr audioOutFrame(av_frame_alloc());
                    if (!audioOutFrame) {
                        failureMessage = QStringLiteral(
                            "failed to allocate audio flush frame");
                        return false;
                    }
                    audioOutFrame->format = audioOutSampleFmt;
                    audioOutFrame->sample_rate = request.audioSampleRate;
                    audioOutFrame->nb_samples = outSamples;
                    if (av_channel_layout_copy(&audioOutFrame->ch_layout,
                                               &audioOutLayout.layout) < 0) {
                        failureMessage = QStringLiteral(
                            "failed to copy output audio layout");
                        return false;
                    }
                    if (av_frame_get_buffer(audioOutFrame.get(), 0) < 0) {
                        failureMessage = QStringLiteral(
                            "failed to allocate audio flush buffer");
                        return false;
                    }

                    const int converted = swr_convert(
                        swrCtx.get(),
                        audioOutFrame->extended_data,
                        outSamples,
                        nullptr,
                        0);
                    if (converted < 0) {
                        failureMessage = QStringLiteral("audio resampler flush failed");
                        return false;
                    }
                    if (converted == 0) return true;

                    audioOutFrame->nb_samples = converted;
                    if (!encoder.pushAudioFrame(audioOutFrame.get(), audioPts)) {
                        failureMessage = QStringLiteral(
                            "encoder audio frame push failed");
                        return false;
                    }
                    audioPts += converted;
                }
            };

            for (const Highlight &highlight : orderedHighlights) {
                if (!std::isfinite(highlight.startTime)
                    || !std::isfinite(highlight.endTime)
                    || highlight.endTime <= highlight.startTime) {
                    continue;
                }

                if (auto err = decoder.seek(highlight.startTime)) {
                    failureMessage = QString::fromStdString(*err);
                    if (swsCtx) sws_freeContext(swsCtx);
                    return false;
                }

                bool videoDone = false;
                bool audioDone = !hasAudio;
                while (!videoDone || !audioDone) {
                    // US-123: bail out promptly when the owning AIHighlight is
                    // being destroyed so trackAIHighlightThread's interruption +
                    // thread->wait() in the destroyed handler does not block for
                    // the full export duration.
                    if (aiHighlightInterrupted()) {
                        if (swsCtx) sws_freeContext(swsCtx);
                        return false;
                    }
                    bool advanced = false;

                    if (!videoDone) {
                        AVFrame *frame = decoder.nextVideoFrame();
                        if (!frame) {
                            videoDone = true;
                        } else {
                            advanced = true;
                            const double pts =
                                framePtsSeconds(frame, videoProps.timeBase);
                            if (std::isfinite(pts)) {
                                if (pts >= highlight.endTime) {
                                    videoDone = true;
                                } else if (pts >= highlight.startTime) {
                                    if (!pushVideo(frame)) {
                                        if (swsCtx) sws_freeContext(swsCtx);
                                        return false;
                                    }
                                }
                            }
                        }
                    }

                    if (!audioDone) {
                        AVFrame *frame = decoder.nextAudioFrame();
                        if (!frame) {
                            audioDone = true;
                        } else {
                            advanced = true;
                            const double pts =
                                framePtsSeconds(frame, audioProps.timeBase);
                            if (std::isfinite(pts)) {
                                if (pts >= highlight.endTime) {
                                    audioDone = true;
                                } else if (pts >= highlight.startTime) {
                                    if (!pushAudio(frame)) {
                                        if (swsCtx) sws_freeContext(swsCtx);
                                        return false;
                                    }
                                }
                            }
                        }
                    }

                    if (!advanced) break;
                }
            }

            if (swsCtx) {
                sws_freeContext(swsCtx);
                swsCtx = nullptr;
            }

            if (!wroteVideo) {
                failureMessage = QStringLiteral(
                    "no video frames matched highlight ranges");
                return false;
            }

            if (!flushAudio())
                return false;

            if (auto err = encoder.finalize()) {
                failureMessage = QString::fromStdString(*err);
                return false;
            }
            return true;
        }();

        const QString message = success
            ? QString("Highlight reel exported: %1").arg(outputPath)
            : QString("Export failed: %1").arg(failureMessage);
        postExportComplete(owner, success, message);
    });
    trackAIHighlightThread(this, thread);
    thread->start();
}

// ---------------------------------------------------------------------------
// exportTimestamps — human-readable timestamp list
// ---------------------------------------------------------------------------

QString AIHighlight::exportTimestamps(const QVector<Highlight> &highlights)
{
    QString result;

    for (int i = 0; i < highlights.size(); ++i) {
        const auto &h = highlights[i];

        auto formatTime = [](double secs) -> QString {
            int totalSec = static_cast<int>(secs);
            int hours = totalSec / 3600;
            int minutes = (totalSec % 3600) / 60;
            int seconds = totalSec % 60;
            if (hours > 0)
                return QString("%1:%2:%3")
                    .arg(hours).arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
            return QString("%1:%2")
                .arg(minutes).arg(seconds, 2, 10, QChar('0'));
        };

        result += QString("#%1  %2 - %3  [score: %4]")
            .arg(i + 1, 2)
            .arg(formatTime(h.startTime))
            .arg(formatTime(h.endTime))
            .arg(h.score, 0, 'f', 2);

        if (!h.description.isEmpty())
            result += QString("  %1").arg(h.description);

        result += "\n";
    }

    return result;
}
