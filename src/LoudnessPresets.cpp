#include "LoudnessPresets.h"

#include "LoudnessAnalyzer.h"
#include "PlaybackTypes.h"
#include "Timeline.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace loudnesspreset {

namespace {

constexpr double kMinMeasuredLufs = -69.5;
constexpr double kMinClipGain = 0.01;

struct AudioDecodeCtx {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    SwrContext *swr = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    int streamIdx = -1;

    ~AudioDecodeCtx()
    {
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (swr) swr_free(&swr);
        if (codec) avcodec_free_context(&codec);
        if (fmt) avformat_close_input(&fmt);
    }

    bool open(const QString &path)
    {
        if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0)
            return false;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                streamIdx = static_cast<int>(i);
                break;
            }
        }
        if (streamIdx < 0)
            return false;
        AVCodecParameters *par = fmt->streams[streamIdx]->codecpar;
        const AVCodec *dec = avcodec_find_decoder(par->codec_id);
        if (!dec)
            return false;
        codec = avcodec_alloc_context3(dec);
        if (!codec || avcodec_parameters_to_context(codec, par) < 0)
            return false;
        if (avcodec_open2(codec, dec, nullptr) < 0)
            return false;

        AVChannelLayout outLayout;
        const int outCh = qBound(1, codec->ch_layout.nb_channels, 2);
        av_channel_layout_default(&outLayout, outCh);
        if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, codec->sample_rate,
                                &codec->ch_layout, codec->sample_fmt, codec->sample_rate,
                                0, nullptr) < 0
            || !swr || swr_init(swr) < 0) {
            av_channel_layout_uninit(&outLayout);
            return false;
        }
        av_channel_layout_uninit(&outLayout);

        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        return pkt != nullptr && frame != nullptr;
    }
};

} // namespace

QVector<PlatformPreset> exportDialogPresets()
{
    // Index 0 = -14.0, Index 1 = -23.0, Index 2 = -16.0 (matches loudness-auto G6 contract).
    return {
        { QStringLiteral("youtube_tiktok_reels"),
          QStringLiteral("YouTube / TikTok / Reels / Spotify (-14 LUFS)"), -14.0, -1.0 },
        { QStringLiteral("broadcast_ebu"),
          QStringLiteral("TV 放送 EBU R128 (-23 LUFS)"), -23.0, -1.0 },
        { QStringLiteral("x_podcast_apple"),
          QStringLiteral("X / ポッドキャスト / Apple Music (-16 LUFS)"), -16.0, -1.0 },
        { QStringLiteral("broadcast_arib_cinema"),
          QStringLiteral("TV 放送 ARIB TR-B32 / 映画 (-24 LUFS)"), -24.0, -1.0 },
    };
}

int exportPresetIndexForLufs(double targetLufs)
{
    const auto presets = exportDialogPresets();
    for (int i = 0; i < presets.size(); ++i) {
        if (std::abs(presets[i].targetLufs - targetLufs) < 0.05)
            return i;
    }
    return presets.size(); // 自由入力 (custom)
}

QVector<PlatformPreset> timelinePresets()
{
    return {
        { QStringLiteral("youtube_tiktok_reels"),
          QStringLiteral("YouTube / TikTok / Instagram Reels (-14 LUFS)"), -14.0, -1.0 },
        { QStringLiteral("spotify"),
          QStringLiteral("Spotify (-14 LUFS)"), -14.0, -1.0 },
        { QStringLiteral("x_podcast_apple"),
          QStringLiteral("X / ポッドキャスト / Apple Music (-16 LUFS)"), -16.0, -1.0 },
        { QStringLiteral("broadcast_ebu"),
          QStringLiteral("TV 放送 EBU R128 (-23 LUFS)"), -23.0, -1.0 },
        { QStringLiteral("broadcast_arib_cinema"),
          QStringLiteral("TV 放送 ARIB TR-B32 / 映画 (-24 LUFS)"), -24.0, -1.0 },
    };
}

ClipMeasurement measureClipLoudness(const ClipInfo &clip, double scaleFactor)
{
    ClipMeasurement out;
    if (clip.filePath.isEmpty())
        return out;

    AudioDecodeCtx dec;
    if (!dec.open(clip.filePath))
        return out;

    const double inSec = qMax(0.0, clip.inPoint);
    const double outSec = (clip.outPoint > inSec) ? clip.outPoint : clip.duration;
    if (outSec <= inSec)
        return out;

    const double tb = av_q2d(dec.fmt->streams[dec.streamIdx]->time_base);
    if (inSec > 0.0 && tb > 0.0) {
        const int64_t seekTs = static_cast<int64_t>(inSec / tb);
        av_seek_frame(dec.fmt, dec.streamIdx, seekTs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec.codec);
    }

    const int sampleRate = dec.codec->sample_rate > 0 ? dec.codec->sample_rate : 48000;
    const int outCh = qBound(1, dec.codec->ch_layout.nb_channels, 2);

    LoudnessAnalyzer analyzer;
    analyzer.setSampleRate(sampleRate);

    QVector<float> buf;
    int64_t totalFrames = 0;
    double cursorSec = inSec;
    bool done = false;

    auto processFrame = [&](AVFrame *f) {
        double frameStart = cursorSec;
        if (f->pts != AV_NOPTS_VALUE && tb > 0.0)
            frameStart = static_cast<double>(f->pts) * tb;
        const double frameDur = static_cast<double>(f->nb_samples) / sampleRate;
        const double frameEnd = frameStart + frameDur;
        cursorSec = frameEnd;

        if (frameEnd <= inSec)
            return;
        if (frameStart >= outSec) {
            done = true;
            return;
        }

        const int maxOut = swr_get_out_samples(dec.swr, f->nb_samples);
        if (maxOut <= 0)
            return;
        buf.resize(maxOut * outCh);
        uint8_t *outPlanes[1] = { reinterpret_cast<uint8_t *>(buf.data()) };
        const int got = swr_convert(dec.swr, outPlanes, maxOut,
                                    const_cast<const uint8_t **>(f->extended_data), f->nb_samples);
        if (got <= 0)
            return;

        int startIdx = 0;
        int endIdx = got;
        if (frameStart < inSec) {
            startIdx = qBound(0, static_cast<int>(std::round((inSec - frameStart) * sampleRate)), got);
        }
        if (frameEnd > outSec) {
            endIdx = qBound(startIdx, static_cast<int>(std::round((outSec - frameStart) * sampleRate)), got);
            done = true;
        }
        const int count = endIdx - startIdx;
        if (count <= 0)
            return;

        float *ptr = buf.data() + startIdx * outCh;
        if (std::abs(scaleFactor - 1.0) > 1e-6) {
            const float sf = static_cast<float>(scaleFactor);
            const int totalSamples = count * outCh;
            for (int i = 0; i < totalSamples; ++i)
                ptr[i] *= sf;
        }
        analyzer.processBlock(ptr, count, outCh);
        totalFrames += count;
    };

    while (!done && av_read_frame(dec.fmt, dec.pkt) >= 0) {
        if (dec.pkt->stream_index != dec.streamIdx) {
            av_packet_unref(dec.pkt);
            continue;
        }
        if (avcodec_send_packet(dec.codec, dec.pkt) == 0) {
            while (!done && avcodec_receive_frame(dec.codec, dec.frame) == 0) {
                processFrame(dec.frame);
                av_frame_unref(dec.frame);
            }
        }
        av_packet_unref(dec.pkt);
    }
    if (!done && avcodec_send_packet(dec.codec, nullptr) == 0) {
        while (!done && avcodec_receive_frame(dec.codec, dec.frame) == 0) {
            processFrame(dec.frame);
            av_frame_unref(dec.frame);
        }
    }

    if (totalFrames <= 0)
        return out;

    const double lufs = analyzer.integratedLUFS();
    const double peak = analyzer.truePeakDBTP();
    if (!std::isfinite(lufs) || lufs <= kMinMeasuredLufs)
        return out;

    out.valid = true;
    out.integratedLufs = lufs;
    out.truePeakDbtp = std::isfinite(peak) ? peak : -70.0;
    out.durationSec = static_cast<double>(totalFrames) / sampleRate;
    return out;
}

GainDecision computeTargetClipGain(double rawIntegratedLufs,
                                   double rawTruePeakDbtp,
                                   double targetLufs,
                                   double maxTruePeakDbtp)
{
    GainDecision d;
    if (!std::isfinite(rawIntegratedLufs) || rawIntegratedLufs <= kMinMeasuredLufs)
        return d;

    double gainDb = targetLufs - rawIntegratedLufs;
    if (std::isfinite(rawTruePeakDbtp) && rawTruePeakDbtp > -69.5) {
        if (rawTruePeakDbtp + gainDb > maxTruePeakDbtp) {
            gainDb = maxTruePeakDbtp - rawTruePeakDbtp;
            d.peakClamped = true;
        }
    }

    double lin = std::pow(10.0, gainDb / 20.0);
    if (lin > audio::kMaxClipGain) {
        lin = audio::kMaxClipGain;
        d.maxGainClamped = true;
    } else if (lin < kMinClipGain) {
        lin = kMinClipGain;
    }
    d.valid = true;
    d.gainLinear = lin;
    d.gainDb = 20.0 * std::log10(lin);
    return d;
}

GainDecision computeUniformGainRatio(const QVector<ClipMeasurement> &scaledMeasurements,
                                     double targetLufs,
                                     double maxTruePeakDbtp)
{
    GainDecision d;
    double weightedEnergy = 0.0;
    double totalDur = 0.0;
    double maxPeakDbtp = -70.0;
    bool hasPeak = false;

    for (const auto &m : scaledMeasurements) {
        if (!m.valid || m.integratedLufs <= kMinMeasuredLufs || m.durationSec <= 0.0)
            continue;
        // Invert BS.1770 loudness formula L = -0.691 + 10 * log10(E)
        const double energy = std::pow(10.0, (m.integratedLufs + 0.691) / 10.0);
        weightedEnergy += energy * m.durationSec;
        totalDur += m.durationSec;
        if (std::isfinite(m.truePeakDbtp) && m.truePeakDbtp > -69.5) {
            maxPeakDbtp = hasPeak ? qMax(maxPeakDbtp, m.truePeakDbtp) : m.truePeakDbtp;
            hasPeak = true;
        }
    }
    if (totalDur <= 0.0 || weightedEnergy <= 0.0)
        return d;

    const double combinedLufs = -0.691 + 10.0 * std::log10(weightedEnergy / totalDur);
    double deltaDb = targetLufs - combinedLufs;
    if (hasPeak && maxPeakDbtp + deltaDb > maxTruePeakDbtp) {
        deltaDb = maxTruePeakDbtp - maxPeakDbtp;
        d.peakClamped = true;
    }

    double ratio = std::pow(10.0, deltaDb / 20.0);
    if (!std::isfinite(ratio) || ratio <= 0.0)
        return d;
    d.valid = true;
    d.gainLinear = ratio;
    d.gainDb = 20.0 * std::log10(ratio);
    return d;
}

NormalizeResult applyLoudnessNormalization(Timeline *timeline,
                                           NormalizeScope scope,
                                           int trackIdx,
                                           int clipIdx,
                                           double targetLufs,
                                           double maxTruePeakDbtp,
                                           const QString &presetLabel)
{
    NormalizeResult res;
    if (!timeline)
        return res;
    const QVector<TimelineTrack *> tracks = timeline->audioTracks();
    if (tracks.isEmpty())
        return res;

    if (scope == NormalizeScope::SingleClip) {
        if (trackIdx < 0 || trackIdx >= tracks.size() || !tracks[trackIdx])
            return res;
        TimelineTrack *track = tracks[trackIdx];
        if (track->isLocked()) {
            res.summary = QStringLiteral("ノーマライズ失敗: トラックがロックされています。");
            return res;
        }
        QVector<ClipInfo> clips = track->clips();
        if (clipIdx < 0 || clipIdx >= clips.size())
            return res;

        const ClipMeasurement m = measureClipLoudness(clips[clipIdx], 1.0);
        if (!m.valid) {
            res.summary = QStringLiteral("ノーマライズ失敗: 音声信号を計測できませんでした。");
            return res;
        }
        const GainDecision g = computeTargetClipGain(m.integratedLufs, m.truePeakDbtp,
                                                     targetLufs, maxTruePeakDbtp);
        if (!g.valid)
            return res;

        clips[clipIdx].volume = g.gainLinear;
        track->setClips(clips);
        timeline->saveUndoState(QStringLiteral("ノーマライズ (%1)").arg(presetLabel));

        res.ok = true;
        res.clipsUpdated = 1;
        res.measuredLufs = m.integratedLufs;
        res.appliedGainDb = g.gainDb;
        res.peakClamped = g.peakClamped;
        res.summary = QStringLiteral("ノーマライズ完了 (%1): 計測 %2 LUFS → ゲイン %3 dB (音量 %4%)%5")
            .arg(presetLabel)
            .arg(m.integratedLufs, 0, 'f', 1)
            .arg(g.gainDb >= 0.0 ? QStringLiteral("+%1").arg(g.gainDb, 0, 'f', 1)
                                 : QString::number(g.gainDb, 'f', 1))
            .arg(qRound(g.gainLinear * 100.0))
            .arg(g.peakClamped ? QStringLiteral(" [ピーク保護]") : QString());
        return res;
    }

    // All-clips modes: collect all audible clips on unlocked, non-muted audio tracks.
    struct TargetRef {
        int trackIdx = -1;
        int clipIdx = -1;
        ClipInfo clip;
    };
    QVector<TargetRef> targets;
    for (int t = 0; t < tracks.size(); ++t) {
        TimelineTrack *tr = tracks[t];
        if (!tr || tr->isLocked() || tr->isMuted()) {
            if (tr) res.clipsSkipped += tr->clips().size();
            continue;
        }
        const auto clips = tr->clips();
        for (int c = 0; c < clips.size(); ++c) {
            if (!clips[c].visible || clips[c].filePath.isEmpty()) {
                ++res.clipsSkipped;
                continue;
            }
            targets.append({ t, c, clips[c] });
        }
    }
    if (targets.isEmpty()) {
        res.summary = QStringLiteral("ノーマライズ対象となる有効な音声クリップがありません。");
        return res;
    }

    if (scope == NormalizeScope::AllClipsIndividual) {
        QHash<int, QVector<ClipInfo>> modifiedTracks;
        double lufsSum = 0.0;
        double gainSum = 0.0;
        bool anyPeakClamped = false;

        for (const auto &ref : targets) {
            const ClipMeasurement m = measureClipLoudness(ref.clip, 1.0);
            if (!m.valid) {
                ++res.clipsSkipped;
                continue;
            }
            const GainDecision g = computeTargetClipGain(m.integratedLufs, m.truePeakDbtp,
                                                         targetLufs, maxTruePeakDbtp);
            if (!g.valid) {
                ++res.clipsSkipped;
                continue;
            }
            if (!modifiedTracks.contains(ref.trackIdx))
                modifiedTracks.insert(ref.trackIdx, tracks[ref.trackIdx]->clips());
            modifiedTracks[ref.trackIdx][ref.clipIdx].volume = g.gainLinear;
            ++res.clipsUpdated;
            lufsSum += m.integratedLufs;
            gainSum += g.gainDb;
            anyPeakClamped = anyPeakClamped || g.peakClamped;
        }
        if (res.clipsUpdated == 0) {
            res.summary = QStringLiteral("ノーマライズ失敗: 有効な音声信号が検出されませんでした。");
            return res;
        }
        for (auto it = modifiedTracks.cbegin(); it != modifiedTracks.cend(); ++it)
            tracks[it.key()]->setClips(it.value());
        timeline->saveUndoState(QStringLiteral("全体ノーマライズ・個別 (%1)").arg(presetLabel));

        res.ok = true;
        res.measuredLufs = lufsSum / res.clipsUpdated;
        res.appliedGainDb = gainSum / res.clipsUpdated;
        res.peakClamped = anyPeakClamped;
        res.summary = QStringLiteral("全体ノーマライズ・個別完了 (%1): %2 クリップ更新%3")
            .arg(presetLabel)
            .arg(res.clipsUpdated)
            .arg(anyPeakClamped ? QStringLiteral(" [ピーク保護あり]") : QString());
        return res;
    }

    // Scope::AllClipsUniform — preserve relative clip balance with one shared gain ratio.
    QVector<ClipMeasurement> scaledMeasurements;
    QVector<TargetRef> validTargets;
    scaledMeasurements.reserve(targets.size());
    validTargets.reserve(targets.size());
    for (const auto &ref : targets) {
        const double curVol = audio::clampClipGain(ref.clip.volume);
        if (curVol <= 1e-6) {
            ++res.clipsSkipped;
            continue;
        }
        const ClipMeasurement m = measureClipLoudness(ref.clip, curVol);
        if (!m.valid) {
            ++res.clipsSkipped;
            continue;
        }
        scaledMeasurements.append(m);
        validTargets.append(ref);
    }
    if (validTargets.isEmpty()) {
        res.summary = QStringLiteral("ノーマライズ失敗: 有効な音声信号が検出されませんでした。");
        return res;
    }

    const GainDecision ratio = computeUniformGainRatio(scaledMeasurements, targetLufs, maxTruePeakDbtp);
    if (!ratio.valid) {
        res.summary = QStringLiteral("ノーマライズ失敗: ラウドネス計算に失敗しました。");
        return res;
    }

    // Compute combined measured LUFS for reporting.
    double wE = 0.0, totD = 0.0;
    for (const auto &m : scaledMeasurements) {
        wE += std::pow(10.0, (m.integratedLufs + 0.691) / 10.0) * m.durationSec;
        totD += m.durationSec;
    }
    res.measuredLufs = (totD > 0.0 && wE > 0.0) ? (-0.691 + 10.0 * std::log10(wE / totD)) : -70.0;

    QHash<int, QVector<ClipInfo>> modifiedTracks;
    for (const auto &ref : validTargets) {
        if (!modifiedTracks.contains(ref.trackIdx))
            modifiedTracks.insert(ref.trackIdx, tracks[ref.trackIdx]->clips());
        const double nextVol = qBound(kMinClipGain,
                                      audio::clampClipGain(ref.clip.volume) * ratio.gainLinear,
                                      audio::kMaxClipGain);
        modifiedTracks[ref.trackIdx][ref.clipIdx].volume = nextVol;
        ++res.clipsUpdated;
    }
    for (auto it = modifiedTracks.cbegin(); it != modifiedTracks.cend(); ++it)
        tracks[it.key()]->setClips(it.value());
    timeline->saveUndoState(QStringLiteral("全体ノーマライズ・バランス維持 (%1)").arg(presetLabel));

    res.ok = true;
    res.appliedGainDb = ratio.gainDb;
    res.peakClamped = ratio.peakClamped;
    res.summary = QStringLiteral("全体ノーマライズ完了 (%1): %2 クリップ一律 %3 dB (全体計測 %4 LUFS)%5")
        .arg(presetLabel)
        .arg(res.clipsUpdated)
        .arg(ratio.gainDb >= 0.0 ? QStringLiteral("+%1").arg(ratio.gainDb, 0, 'f', 1)
                                 : QString::number(ratio.gainDb, 'f', 1))
        .arg(res.measuredLufs, 0, 'f', 1)
        .arg(ratio.peakClamped ? QStringLiteral(" [ピーク保護]") : QString());
    return res;
}

NormalizeOptionsDialog::NormalizeOptionsDialog(NormalizeScope initialScope,
                                               double initialLufs,
                                               double initialPeakDbtp,
                                               QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("プラットフォーム別音量ノーマライズ"));
    setMinimumWidth(420);

    auto *root = new QVBoxLayout(this);

    auto *scopeBox = new QGroupBox(QStringLiteral("適用範囲"), this);
    auto *scopeLayout = new QVBoxLayout(scopeBox);
    m_scopeSingle = new QRadioButton(QStringLiteral("このクリップ単体のみ"), scopeBox);
    m_scopeSingle->setObjectName(QStringLiteral("normScopeSingle"));
    m_scopeAllIndividual = new QRadioButton(
        QStringLiteral("タイムライン全クリップ (各クリップを個別に目標 LUFS へ揃える)"), scopeBox);
    m_scopeAllIndividual->setObjectName(QStringLiteral("normScopeAllIndividual"));
    m_scopeAllUniform = new QRadioButton(
        QStringLiteral("タイムライン全クリップ (全体のバランスを維持して一律調整)"), scopeBox);
    m_scopeAllUniform->setObjectName(QStringLiteral("normScopeAllUniform"));
    scopeLayout->addWidget(m_scopeSingle);
    scopeLayout->addWidget(m_scopeAllIndividual);
    scopeLayout->addWidget(m_scopeAllUniform);
    switch (initialScope) {
    case NormalizeScope::SingleClip:         m_scopeSingle->setChecked(true); break;
    case NormalizeScope::AllClipsIndividual: m_scopeAllIndividual->setChecked(true); break;
    case NormalizeScope::AllClipsUniform:    m_scopeAllUniform->setChecked(true); break;
    }
    root->addWidget(scopeBox);

    auto *form = new QFormLayout();
    m_presetCombo = new QComboBox(this);
    m_presetCombo->setObjectName(QStringLiteral("normPresetCombo"));
    const auto presets = timelinePresets();
    for (const auto &p : presets)
        m_presetCombo->addItem(p.displayName, p.id);
    m_presetCombo->addItem(QStringLiteral("カスタム LUFS"), QStringLiteral("custom"));
    form->addRow(QStringLiteral("プラットフォーム:"), m_presetCombo);

    m_lufsSpin = new QDoubleSpinBox(this);
    m_lufsSpin->setObjectName(QStringLiteral("normLufsSpin"));
    m_lufsSpin->setRange(-70.0, -1.0);
    m_lufsSpin->setDecimals(1);
    m_lufsSpin->setSingleStep(1.0);
    m_lufsSpin->setSuffix(QStringLiteral(" LUFS"));
    m_lufsSpin->setValue(initialLufs);
    form->addRow(QStringLiteral("目標ラウドネス:"), m_lufsSpin);

    m_peakSpin = new QDoubleSpinBox(this);
    m_peakSpin->setObjectName(QStringLiteral("normPeakSpin"));
    m_peakSpin->setRange(-6.0, 0.0);
    m_peakSpin->setDecimals(1);
    m_peakSpin->setSingleStep(0.5);
    m_peakSpin->setSuffix(QStringLiteral(" dBTP"));
    m_peakSpin->setValue(initialPeakDbtp);
    form->addRow(QStringLiteral("True Peak 上限:"), m_peakSpin);
    root->addLayout(form);

    // Sync initial preset combo index
    int matchedIdx = presets.size();
    for (int i = 0; i < presets.size(); ++i) {
        if (std::abs(presets[i].targetLufs - initialLufs) < 0.05) {
            matchedIdx = i;
            break;
        }
    }
    m_presetCombo->setCurrentIndex(matchedIdx);

    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, presets](int index) {
        if (index >= 0 && index < presets.size()) {
            const QSignalBlocker b1(m_lufsSpin);
            const QSignalBlocker b2(m_peakSpin);
            m_lufsSpin->setValue(presets[index].targetLufs);
            m_peakSpin->setValue(presets[index].maxTruePeakDbtp);
        }
    });
    connect(m_lufsSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, presets](double) {
        const QSignalBlocker b(m_presetCombo);
        m_presetCombo->setCurrentIndex(presets.size());
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

NormalizeScope NormalizeOptionsDialog::selectedScope() const
{
    if (m_scopeAllIndividual && m_scopeAllIndividual->isChecked())
        return NormalizeScope::AllClipsIndividual;
    if (m_scopeAllUniform && m_scopeAllUniform->isChecked())
        return NormalizeScope::AllClipsUniform;
    return NormalizeScope::SingleClip;
}

double NormalizeOptionsDialog::targetLufs() const
{
    return m_lufsSpin ? m_lufsSpin->value() : -14.0;
}

double NormalizeOptionsDialog::maxTruePeakDbtp() const
{
    return m_peakSpin ? m_peakSpin->value() : -1.0;
}

QString NormalizeOptionsDialog::selectedPresetLabel() const
{
    if (!m_presetCombo)
        return QStringLiteral("%1 LUFS").arg(targetLufs(), 0, 'f', 1);
    const int idx = m_presetCombo->currentIndex();
    const auto presets = timelinePresets();
    if (idx >= 0 && idx < presets.size())
        return presets[idx].displayName;
    return QStringLiteral("カスタム %1 LUFS").arg(targetLufs(), 0, 'f', 1);
}

} // namespace loudnesspreset
