#include "CreatorTools.h"
#include "BeatDetect.h"
#include "ExternalGenerator.h"
#include "Timeline.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace creatortools {

namespace {

constexpr double kPi = 3.14159265358979323846;

QVector<float> decodeMonoWindow(const QString &filePath,
                                double startSec,
                                double durationSec,
                                int targetSampleRate)
{
    QVector<float> out;
    if (filePath.isEmpty() || durationSec <= 0.0 || targetSampleRate <= 0)
        return out;

    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return out;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return out;
    }
    const int streamIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIdx < 0) {
        avformat_close_input(&fmt);
        return out;
    }
    AVCodecParameters *par = fmt->streams[streamIdx]->codecpar;
    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *codec = dec ? avcodec_alloc_context3(dec) : nullptr;
    if (!codec || avcodec_parameters_to_context(codec, par) < 0 || avcodec_open2(codec, dec, nullptr) < 0) {
        if (codec) avcodec_free_context(&codec);
        avformat_close_input(&fmt);
        return out;
    }

    AVChannelLayout monoLayout;
    av_channel_layout_default(&monoLayout, 1);
    SwrContext *swr = nullptr;
    if (swr_alloc_set_opts2(&swr, &monoLayout, AV_SAMPLE_FMT_FLT, targetSampleRate,
                            &codec->ch_layout, codec->sample_fmt, codec->sample_rate,
                            0, nullptr) < 0 || !swr || swr_init(swr) < 0) {
        av_channel_layout_uninit(&monoLayout);
        if (swr) swr_free(&swr);
        avcodec_free_context(&codec);
        avformat_close_input(&fmt);
        return out;
    }
    av_channel_layout_uninit(&monoLayout);

    const double tb = av_q2d(fmt->streams[streamIdx]->time_base);
    const double endSec = startSec + durationSec;
    if (startSec > 0.0 && tb > 0.0) {
        av_seek_frame(fmt, streamIdx, static_cast<int64_t>(startSec / tb), AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec);
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    const int maxSamples = qMax(64, static_cast<int>(std::ceil(durationSec * targetSampleRate)));
    out.reserve(maxSamples);
    QVector<float> tmp;
    double cursorSec = startSec;
    bool done = false;

    auto consumeFrame = [&](AVFrame *f) {
        double fStart = (f->pts != AV_NOPTS_VALUE && tb > 0.0) ? static_cast<double>(f->pts) * tb : cursorSec;
        const double fDur = static_cast<double>(f->nb_samples) / qMax(1, codec->sample_rate);
        const double fEnd = fStart + fDur;
        cursorSec = fEnd;
        if (fEnd <= startSec) return;
        if (fStart >= endSec) { done = true; return; }
        const int maxOut = swr_get_out_samples(swr, f->nb_samples);
        if (maxOut <= 0) return;
        tmp.resize(maxOut);
        uint8_t *planes[1] = { reinterpret_cast<uint8_t *>(tmp.data()) };
        const int got = swr_convert(swr, planes, maxOut,
                                    const_cast<const uint8_t **>(f->extended_data), f->nb_samples);
        if (got <= 0) return;
        int s0 = 0, s1 = got;
        if (fStart < startSec)
            s0 = qBound(0, static_cast<int>(std::round((startSec - fStart) * targetSampleRate)), got);
        if (fEnd > endSec) {
            s1 = qBound(s0, static_cast<int>(std::round((endSec - fStart) * targetSampleRate)), got);
            done = true;
        }
        for (int i = s0; i < s1 && out.size() < maxSamples; ++i)
            out.append(tmp[i]);
        if (out.size() >= maxSamples)
            done = true;
    };

    while (!done && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == streamIdx && avcodec_send_packet(codec, pkt) == 0) {
            while (!done && avcodec_receive_frame(codec, frame) == 0) {
                consumeFrame(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&codec);
    avformat_close_input(&fmt);
    return out;
}

QVector<float> sampleTimelineAudioWindow(const Timeline *timeline, double timeSec, int sampleRate, int sampleCount)
{
    if (!timeline || sampleCount <= 0)
        return {};
    const double winSec = static_cast<double>(sampleCount) / sampleRate;
    for (const TimelineTrack *tr : timeline->audioTracks()) {
        if (!tr || tr->isMuted()) continue;
        double cursor = 0.0;
        for (const ClipInfo &c : tr->clips()) {
            cursor += qMax(0.0, c.leadInSec);
            const double dur = qMax(0.0, c.effectiveDuration());
            if (c.visible && !c.filePath.isEmpty() && timeSec >= cursor && timeSec < cursor + dur) {
                const double local = timeSec - cursor;
                const double srcSec = c.inPoint + local * (c.speed > 0.0 ? c.speed : 1.0);
                QVector<float> pcm = decodeMonoWindow(c.filePath, srcSec, winSec, sampleRate);
                if (!pcm.isEmpty()) {
                    const float gain = static_cast<float>(qBound(0.0, c.volume, 4.0));
                    for (float &v : pcm) v *= gain;
                    return pcm;
                }
            }
            cursor += dur;
        }
    }
    return {};
}

} // namespace

CollageResult applySplitScreenCollage(Timeline *timeline,
                                      CollagePreset preset,
                                      double timelineSec)
{
    CollageResult res;
    if (!timeline) {
        res.summary = QStringLiteral("タイムラインが無効です。");
        return res;
    }

    struct Slot {
        double scale = 0.5;
        double dx = 0.0;
        double dy = 0.0;
    };
    QVector<Slot> layoutSlots;
    QString presetName;
    switch (preset) {
    case CollagePreset::TwoUpHorizontal:
        presetName = QStringLiteral("2分割 (左右)");
        layoutSlots = { {0.48, -0.25, 0.0}, {0.48, 0.25, 0.0} };
        break;
    case CollagePreset::TwoUpVertical:
        presetName = QStringLiteral("2分割 (上下)");
        layoutSlots = { {0.48, 0.0, -0.25}, {0.48, 0.0, 0.25} };
        break;
    case CollagePreset::ThreeUpMainLeft:
        presetName = QStringLiteral("3分割 (左メイン+右2)");
        layoutSlots = { {0.48, -0.25, 0.0}, {0.46, 0.25, -0.25}, {0.46, 0.25, 0.25} };
        break;
    case CollagePreset::FourUpGrid:
        presetName = QStringLiteral("4分割 (2x2 グリッド)");
        layoutSlots = {
            {0.48, -0.25, -0.25},
            {0.48,  0.25, -0.25},
            {0.48, -0.25,  0.25},
            {0.48,  0.25,  0.25}
        };
        break;
    }

    const QVector<TimelineTrack *> tracks = timeline->videoTracks();
    for (int i = 0; i < layoutSlots.size() && i < tracks.size(); ++i) {
        TimelineTrack *tr = tracks[i];
        if (!tr || tr->isLocked() || tr->clips().isEmpty())
            continue;
        QVector<ClipInfo> clips = tr->clips();
        int targetClip = -1;
        if (timelineSec < 0.0) {
            targetClip = 0;
        } else {
            double cursor = 0.0;
            for (int c = 0; c < clips.size(); ++c) {
                cursor += qMax(0.0, clips[c].leadInSec);
                const double dur = qMax(0.0, clips[c].effectiveDuration());
                if (timelineSec >= cursor && timelineSec < cursor + dur) {
                    targetClip = c;
                    break;
                }
                cursor += dur;
            }
            if (targetClip < 0)
                targetClip = 0;
        }
        clips[targetClip].videoScale = layoutSlots[i].scale;
        clips[targetClip].videoDx = layoutSlots[i].dx;
        clips[targetClip].videoDy = layoutSlots[i].dy;
        clips[targetClip].rotation2DDegrees = 0.0;
        tr->setClips(clips);
        ++res.clipsUpdated;
    }

    if (res.clipsUpdated == 0) {
        res.summary = QStringLiteral("画面分割を適用できる動画クリップがありません。");
        return res;
    }
    timeline->saveUndoState(QStringLiteral("画面分割コラージュ (%1)").arg(presetName));
    res.ok = true;
    res.summary = QStringLiteral("画面分割コラージュ (%1) を %2 トラックへ適用しました")
        .arg(presetName).arg(res.clipsUpdated);
    return res;
}

VisualizerStyle visualizerStyleFromString(const QString &name)
{
    const QString s = name.trimmed().toLower();
    if (s == QStringLiteral("mirror"))   return VisualizerStyle::MirrorBars;
    if (s == QStringLiteral("circular")) return VisualizerStyle::CircularRing;
    if (s == QStringLiteral("waveform")) return VisualizerStyle::WaveformLine;
    return VisualizerStyle::SpectrumBars;
}

QString visualizerStyleToString(VisualizerStyle style)
{
    switch (style) {
    case VisualizerStyle::MirrorBars:   return QStringLiteral("mirror");
    case VisualizerStyle::CircularRing: return QStringLiteral("circular");
    case VisualizerStyle::WaveformLine: return QStringLiteral("waveform");
    case VisualizerStyle::SpectrumBars:
    default:                            return QStringLiteral("bars");
    }
}

QVector<double> computeSpectrumBands(const QVector<float> &monoPcm, int bandCount)
{
    const int bands = qBound(4, bandCount, 128);
    QVector<double> mags(bands, 0.0);
    if (monoPcm.isEmpty())
        return mags;

    const int N = qMin(512, monoPcm.size());
    const double sr = 48000.0;
    for (int b = 0; b < bands; ++b) {
        const double frac = (b + 0.5) / bands;
        const double freq = 60.0 * std::pow(200.0, frac);
        const double omega = 2.0 * kPi * freq / sr;
        double re = 0.0, im = 0.0;
        for (int n = 0; n < N; ++n) {
            const double win = 0.5 * (1.0 - std::cos(2.0 * kPi * n / qMax(1, N - 1)));
            const double x = static_cast<double>(monoPcm[n]) * win;
            const double ang = omega * n;
            re += x * std::cos(ang);
            im -= x * std::sin(ang);
        }
        const double amp = std::sqrt(re * re + im * im) / (N * 0.25);
        mags[b] = qBound(0.0, std::log1p(amp * 8.0) / std::log1p(8.0), 1.0);
    }
    return mags;
}

QImage renderAudioVisualizerFrame(const QJsonObject &params,
                                  double clipLocalSec,
                                  QSize size,
                                  const Timeline *timeline)
{
    const QSize sz = (size.isValid() && !size.isEmpty()) ? size : QSize(640, 360);
    QImage img(sz, QImage::Format_RGBA8888);
    const bool transparentBg = params.value(QStringLiteral("transparent")).toBool(true);
    img.fill(transparentBg ? QColor(0, 0, 0, 0) : QColor(14, 18, 28, 255));

    const VisualizerStyle style = visualizerStyleFromString(
        params.value(QStringLiteral("style")).toString(QStringLiteral("bars")));
    const int barCount = qBound(8, params.value(QStringLiteral("bars")).toInt(32), 96);
    const QColor c1(params.value(QStringLiteral("color1")).toString(QStringLiteral("#4cc9f0")));
    const QColor c2(params.value(QStringLiteral("color2")).toString(QStringLiteral("#f72585")));

    QVector<float> pcm = sampleTimelineAudioWindow(timeline, clipLocalSec, 48000, 512);
    QVector<double> bands;
    if (!pcm.isEmpty()) {
        bands = computeSpectrumBands(pcm, barCount);
    } else {
        bands.resize(barCount);
        for (int i = 0; i < barCount; ++i) {
            const double env = std::sin(kPi * (i + 0.5) / barCount);
            const double wave = 0.5 + 0.5 * std::sin(clipLocalSec * 6.0 + i * 0.45);
            bands[i] = qBound(0.05, 0.12 + 0.72 * env * wave, 1.0);
        }
    }

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    auto lerpColor = [&](double t) {
        const double u = qBound(0.0, t, 1.0);
        return QColor(
            qRound(c1.red() + (c2.red() - c1.red()) * u),
            qRound(c1.green() + (c2.green() - c1.green()) * u),
            qRound(c1.blue() + (c2.blue() - c1.blue()) * u),
            245);
    };

    const int W = sz.width();
    const int H = sz.height();

    if (style == VisualizerStyle::SpectrumBars || style == VisualizerStyle::MirrorBars) {
        const double marginX = W * 0.08;
        const double usableW = W - marginX * 2.0;
        const double step = usableW / barCount;
        const double barW = qMax(2.0, step * 0.68);
        for (int i = 0; i < barCount; ++i) {
            const double val = bands[i];
            const double x = marginX + i * step + (step - barW) * 0.5;
            p.setBrush(lerpColor(static_cast<double>(i) / qMax(1, barCount - 1)));
            p.setPen(Qt::NoPen);
            if (style == VisualizerStyle::SpectrumBars) {
                const double h = qMax(4.0, val * H * 0.72);
                p.drawRoundedRect(QRectF(x, H * 0.86 - h, barW, h), barW * 0.25, barW * 0.25);
            } else {
                const double halfH = qMax(3.0, val * H * 0.36);
                p.drawRoundedRect(QRectF(x, H * 0.5 - halfH, barW, halfH * 2.0), barW * 0.25, barW * 0.25);
            }
        }
    } else if (style == VisualizerStyle::CircularRing) {
        const QPointF center(W * 0.5, H * 0.5);
        const double baseR = qMin(W, H) * 0.22;
        const double maxSpike = qMin(W, H) * 0.22;
        for (int i = 0; i < barCount; ++i) {
            const double ang = 2.0 * kPi * i / barCount - kPi * 0.5;
            const double r0 = baseR;
            const double r1 = baseR + qMax(4.0, bands[i] * maxSpike);
            QPen pen(lerpColor(static_cast<double>(i) / qMax(1, barCount - 1)),
                     qMax(2.0, qMin(W, H) * 0.012), Qt::SolidLine, Qt::RoundCap);
            p.setPen(pen);
            p.drawLine(QPointF(center.x() + std::cos(ang) * r0, center.y() + std::sin(ang) * r0),
                       QPointF(center.x() + std::cos(ang) * r1, center.y() + std::sin(ang) * r1));
        }
    } else {
        QPainterPath path;
        const int pts = qMax(32, barCount * 2);
        for (int i = 0; i < pts; ++i) {
            const double u = static_cast<double>(i) / (pts - 1);
            const double x = W * 0.06 + u * (W * 0.88);
            double amp = 0.0;
            if (!pcm.isEmpty()) {
                const int idx = qBound(0, static_cast<int>(u * (pcm.size() - 1)), pcm.size() - 1);
                amp = qBound(-1.0, static_cast<double>(pcm[idx]) * 1.5, 1.0);
            } else {
                const int bi = qBound(0, static_cast<int>(u * (barCount - 1)), barCount - 1);
                amp = ((i % 2 == 0) ? 1.0 : -1.0) * bands[bi] * 0.8;
            }
            const double y = H * 0.5 - amp * (H * 0.34);
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        QPen pen(c1, qMax(2.5, H * 0.01), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    return img;
}

QString buildVisualizerGeneratorUri(VisualizerStyle style,
                                    const QColor &primaryColor,
                                    const QColor &secondaryColor,
                                    int barCount,
                                    double fps,
                                    bool transparentBg)
{
    extgen::GeneratorSpec spec;
    spec.renderer = QStringLiteral("visualizer");
    spec.fps = fps;
    spec.params[QStringLiteral("style")] = visualizerStyleToString(style);
    spec.params[QStringLiteral("color1")] = primaryColor.name(QColor::HexRgb);
    spec.params[QStringLiteral("color2")] = secondaryColor.name(QColor::HexRgb);
    spec.params[QStringLiteral("bars")] = qBound(8, barCount, 96);
    spec.params[QStringLiteral("transparent")] = transparentBg;
    return extgen::buildGeneratorUri(spec);
}

BeatCutResult autoCutClipToBeats(Timeline *timeline,
                                 int videoTrackIdx,
                                 int videoClipIdx,
                                 const QVector<double> &beatTimesSec,
                                 int everyNBeats,
                                 double minSegmentSec)
{
    BeatCutResult res;
    if (!timeline) {
        res.summary = QStringLiteral("タイムラインが無効です。");
        return res;
    }
    const QVector<TimelineTrack *> tracks = timeline->videoTracks();
    if (videoTrackIdx < 0 || videoTrackIdx >= tracks.size() || !tracks[videoTrackIdx]) {
        res.summary = QStringLiteral("動画トラックが見つかりません。");
        return res;
    }
    TimelineTrack *track = tracks[videoTrackIdx];
    if (track->isLocked()) {
        res.summary = QStringLiteral("動画トラックがロックされています。");
        return res;
    }
    QVector<ClipInfo> clips = track->clips();
    if (videoClipIdx < 0 || videoClipIdx >= clips.size()) {
        res.summary = QStringLiteral("対象動画クリップが見つかりません。");
        return res;
    }

    double clipStart = 0.0;
    for (int i = 0; i < videoClipIdx; ++i)
        clipStart += qMax(0.0, clips[i].leadInSec) + qMax(0.0, clips[i].effectiveDuration());
    clipStart += qMax(0.0, clips[videoClipIdx].leadInSec);
    const ClipInfo orig = clips[videoClipIdx];
    const double clipDur = qMax(0.0, orig.effectiveDuration());
    const double clipEnd = clipStart + clipDur;

    const int stride = qMax(1, everyNBeats);
    QVector<double> cutTimes;
    double lastCut = clipStart;
    for (int i = 0; i < beatTimesSec.size(); i += stride) {
        const double t = beatTimesSec[i];
        if (t > lastCut + minSegmentSec && t < clipEnd - minSegmentSec) {
            cutTimes.append(t);
            lastCut = t;
        }
    }
    res.beatCount = beatTimesSec.size();
    if (cutTimes.isEmpty()) {
        res.summary = QStringLiteral("クリップ範囲内に有効なビート分割点がありません。");
        return res;
    }

    const double speed = (orig.speed > 0.0) ? orig.speed : 1.0;
    QVector<ClipInfo> pieces;
    double segStart = clipStart;
    for (int i = 0; i <= cutTimes.size(); ++i) {
        const double segEnd = (i < cutTimes.size()) ? cutTimes[i] : clipEnd;
        const double localStart = segStart - clipStart;
        const double localEnd = segEnd - clipStart;
        ClipInfo piece = orig;
        piece.uid.clear();
        piece.leadInSec = (i == 0) ? orig.leadInSec : 0.0;
        piece.inPoint = orig.inPoint + localStart * speed;
        piece.outPoint = orig.inPoint + localEnd * speed;
        pieces.append(piece);
        segStart = segEnd;
    }

    QVector<ClipInfo> updated;
    updated.reserve(clips.size() - 1 + pieces.size());
    for (int i = 0; i < videoClipIdx; ++i)
        updated.append(clips[i]);
    for (const ClipInfo &p : pieces)
        updated.append(p);
    for (int i = videoClipIdx + 1; i < clips.size(); ++i)
        updated.append(clips[i]);

    track->setClips(updated);
    timeline->saveUndoState(QStringLiteral("ビート同期自動カット (%1 分割)").arg(cutTimes.size()));

    res.ok = true;
    res.cutsApplied = cutTimes.size();
    res.summary = QStringLiteral("ビート同期自動カット完了: %1 箇所で分割しました").arg(cutTimes.size());
    return res;
}

BeatCutResult detectBeatsAndAutoCut(Timeline *timeline,
                                    int videoTrackIdx,
                                    int videoClipIdx,
                                    int audioTrackIdx,
                                    int audioClipIdx,
                                    int everyNBeats,
                                    double sensitivity)
{
    BeatCutResult res;
    if (!timeline)
        return res;
    const auto aTracks = timeline->audioTracks();
    if (audioTrackIdx < 0 || audioTrackIdx >= aTracks.size() || !aTracks[audioTrackIdx]) {
        res.summary = QStringLiteral("音声トラックが見つかりません。");
        return res;
    }
    const auto aClips = aTracks[audioTrackIdx]->clips();
    if (audioClipIdx < 0 || audioClipIdx >= aClips.size()) {
        res.summary = QStringLiteral("ビート検出対象の音声クリップがありません。");
        return res;
    }
    double aStart = 0.0;
    for (int i = 0; i < audioClipIdx; ++i)
        aStart += qMax(0.0, aClips[i].leadInSec) + qMax(0.0, aClips[i].effectiveDuration());
    aStart += qMax(0.0, aClips[audioClipIdx].leadInSec);
    const ClipInfo &ac = aClips[audioClipIdx];
    const double dur = qMax(0.0, ac.effectiveDuration());
    const int sr = 22050;
    const QVector<float> pcm = decodeMonoWindow(ac.filePath, ac.inPoint, dur, sr);
    if (pcm.isEmpty()) {
        res.summary = QStringLiteral("音声データのデコードに失敗しました。");
        return res;
    }
    beatdetect::Config cfg;
    cfg.sensitivity = sensitivity;
    const beatdetect::Result bres = beatdetect::detectBeats(pcm, sr, cfg);
    QVector<double> tlBeats;
    tlBeats.reserve(bres.beatTimesSec.size());
    for (double t : bres.beatTimesSec)
        tlBeats.append(aStart + t);

    res = autoCutClipToBeats(timeline, videoTrackIdx, videoClipIdx, tlBeats, everyNBeats);
    res.detectedBpm = bres.bpm;
    if (res.ok && res.detectedBpm > 0.0) {
        res.summary = QStringLiteral("ビート同期自動カット完了 (推定 %1 BPM): %2 箇所で分割")
            .arg(res.detectedBpm, 0, 'f', 1).arg(res.cutsApplied);
    }
    return res;
}

} // namespace creatortools
