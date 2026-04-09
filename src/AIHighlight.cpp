#include "AIHighlight.h"
#include "WaveformGenerator.h"
#include <QThread>
#include <QTemporaryFile>
#include <QProcess>
#include <QDir>
#include <cmath>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

AIHighlight::AIHighlight(QObject *parent) : QObject(parent) {}

// ---------------------------------------------------------------------------
// analyze — full async pipeline
// ---------------------------------------------------------------------------

void AIHighlight::analyze(const QString &filePath, const HighlightConfig &config)
{
    auto *thread = QThread::create([this, filePath, config]() {
        emit progressChanged(0);

        // Pass 1: Audio energy (0-30%)
        auto audioScores = analyzeAudioEnergy(filePath, config);
        emit progressChanged(30);

        // Pass 2: Motion activity (30-60%)
        auto motionScores = analyzeMotionActivity(filePath, config);
        emit progressChanged(60);

        // Pass 3: Scene changes (60-80%)
        auto sceneScores = analyzeSceneChanges(filePath, config);
        emit progressChanged(80);

        // Combine and select (80-100%)
        auto allHighlights = combineScores(audioScores, motionScores, sceneScores, config);
        auto selected = selectTopHighlights(allHighlights, config);
        emit progressChanged(100);

        emit analysisComplete(selected);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
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

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != videoIdx) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (avcodec_receive_frame(decCtx, frame) == 0) {
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

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    QVector<uint8_t> prevFrame(cmpW * cmpH, 0);
    QVector<uint8_t> currFrame(cmpW * cmpH, 0);
    bool hasPrev = false;
    int frameCount = 0;

    AVStream *stream = fmtCtx->streams[videoIdx];
    // Check every 5 frames (same as AutoEdit default)
    int checkInterval = 5;

    while (av_read_frame(fmtCtx, packet) >= 0) {
        if (packet->stream_index != videoIdx) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(decCtx, packet) == 0) {
            while (avcodec_receive_frame(decCtx, frame) == 0) {
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
// exportHighlightReel — concatenate highlights via FFmpeg process
// ---------------------------------------------------------------------------

void AIHighlight::exportHighlightReel(const QString &inputPath, const QString &outputPath,
                                       const QVector<Highlight> &highlights)
{
    auto *thread = QThread::create([this, inputPath, outputPath, highlights]() {
        if (highlights.isEmpty()) {
            emit exportComplete(false, "No highlights to export");
            return;
        }

        // Create temporary concat filter script
        // Uses FFmpeg's trim + concat filter for frame-accurate cutting
        QTemporaryFile concatFile;
        concatFile.setAutoRemove(true);
        if (!concatFile.open()) {
            emit exportComplete(false, "Failed to create temporary file");
            return;
        }

        // Build FFmpeg complex filter for trimming and concatenation
        // Format: [0:v]trim=start=S:end=E,setpts=PTS-STARTPTS[v0]; ... concat
        QString filterVideo, filterAudio;
        QString concatInputsV, concatInputsA;

        for (int i = 0; i < highlights.size(); ++i) {
            const auto &h = highlights[i];
            QString vi = QString("v%1").arg(i);
            QString ai = QString("a%1").arg(i);

            filterVideo += QString("[0:v]trim=start=%1:end=%2,setpts=PTS-STARTPTS[%3]; ")
                .arg(h.startTime, 0, 'f', 3)
                .arg(h.endTime, 0, 'f', 3)
                .arg(vi);

            filterAudio += QString("[0:a]atrim=start=%1:end=%2,asetpts=PTS-STARTPTS[%3]; ")
                .arg(h.startTime, 0, 'f', 3)
                .arg(h.endTime, 0, 'f', 3)
                .arg(ai);

            concatInputsV += QString("[%1]").arg(vi);
            concatInputsA += QString("[%1]").arg(ai);
        }

        int n = highlights.size();
        QString complexFilter = filterVideo + filterAudio
            + concatInputsV + concatInputsA
            + QString("concat=n=%1:v=1:a=1[outv][outa]").arg(n);

        QStringList args;
        args << "-y"
             << "-i" << inputPath
             << "-filter_complex" << complexFilter
             << "-map" << "[outv]"
             << "-map" << "[outa]"
             << "-c:v" << "libx264"
             << "-preset" << "medium"
             << "-crf" << "23"
             << "-c:a" << "aac"
             << "-b:a" << "192k"
             << outputPath;

        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", args);

        if (!ffmpeg.waitForStarted(5000)) {
            emit exportComplete(false, "Failed to start ffmpeg");
            return;
        }

        ffmpeg.waitForFinished(-1); // wait indefinitely

        bool success = (ffmpeg.exitCode() == 0);
        QString message = success
            ? QString("Highlight reel exported: %1").arg(outputPath)
            : QString("Export failed: %1").arg(QString::fromUtf8(ffmpeg.readAllStandardError()));

        emit exportComplete(success, message);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
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
