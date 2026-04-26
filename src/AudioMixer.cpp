#include "AudioMixer.h"

#include <QtGlobal>
#include <QDebug>
#include <QtMath>
#include <QThread>
#include <QVarLengthArray>
#include <cstring>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

// Per-entry decoder context + ring buffer. Kept out of the header so FFmpeg
// types stay private. The ring stores resampled output samples in 48 kHz s16
// stereo, ready for direct memcpy / accumulate from MixerIODevice::readData.
struct AudioDecoderEntry {
    PlaybackEntry entry;
    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *codecCtx = nullptr;
    SwrContext *swrCtx = nullptr;
    int audioStreamIdx = -1;
    QByteArray ring;                 // resampled s16 stereo, FIFO
    bool eof = false;
    bool seekPending = true;         // first-time seek to entry's start when approached
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
};

class MixerIODevice : public QIODevice {
public:
    explicit MixerIODevice(AudioMixer *mixer) : m_mixer(mixer) {}
protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *, qint64) override { return -1; }
private:
    AudioMixer *m_mixer;
};

// AudioDecodeRunner — dedicated decode thread that periodically refills
// ring buffers ahead of the audio sink. Sleeps 5 ms between refills; the
// audio thread blocks on m_controlMutex only briefly because each refill
// pass is bounded.
class AudioDecodeRunner : public QThread {
public:
    explicit AudioDecodeRunner(AudioMixer *m, QObject *parent = nullptr)
        : QThread(parent), m_mixer(m) {}
    void requestStop() { m_stopRequested.store(true, std::memory_order_release); }
protected:
    void run() override {
        while (!m_stopRequested.load(std::memory_order_acquire)) {
            m_mixer->refillRings();
            QThread::msleep(5);
        }
    }
private:
    AudioMixer *m_mixer;
    std::atomic<bool> m_stopRequested{false};
};

// =====================================================================
// MixerIODevice — sums samples across every active entry into an int32
// accumulator, then clamps to s16 stereo. Per-clip volume + per-track
// effective gain (mute / solo / linear gain) are applied per entry
// before summation. The master clock advances by the full requested
// length so silence in gaps and decode catch-up don't stall the clock.
// =====================================================================
qint64 MixerIODevice::readData(char *data, qint64 maxlen) {
    if (!data || maxlen <= 0) return 0;
    QMutexLocker lock(&m_mixer->m_controlMutex);
    std::memset(data, 0, static_cast<size_t>(maxlen));

    if (!m_mixer->m_playing.load(std::memory_order_acquire)) {
        return maxlen;
    }

    const int64_t cursorUs = m_mixer->m_writeCursorUs.load(std::memory_order_acquire);
    const int frameCount = static_cast<int>(maxlen / AudioMixer::kBytesPerFrame);
    const int sampleCount = frameCount * AudioMixer::kChannels;

    // Accumulator for the sum-mix. int32 lets us add up to 65535 unit-gain
    // s16 sources before saturating; we cap inputs at MAX_AUDIO_TRACKS=16
    // and the per-track effective gain is bounded at 4.0, so headroom is
    // ample.
    QVarLengthArray<int32_t, 8192> accum(sampleCount);
    std::memset(accum.data(), 0, sampleCount * sizeof(int32_t));

    bool anyMixed = false;
    for (auto it = m_mixer->m_entries.begin(); it != m_mixer->m_entries.end(); ++it) {
        AudioDecoderEntry *e = it.value();
        if (!e) continue;
        const int64_t startUs = static_cast<int64_t>(e->entry.timelineStart * 1e6);
        const int64_t endUs = static_cast<int64_t>(e->entry.timelineEnd * 1e6);
        if (cursorUs < startUs || cursorUs >= endUs) continue;
        if (e->ring.isEmpty()) continue;

        double gain = e->entry.volume;
        if (e->entry.audioMuted) gain = 0.0;
        const int trackIdx = e->entry.sourceTrack;
        if (trackIdx >= 0 && trackIdx < m_mixer->m_trackStates.size())
            gain *= m_mixer->m_trackStates[trackIdx].effectiveGain;
        if (gain <= 0.0) {
            // Still drain the ring so this muted entry stays in time with
            // the cursor; otherwise unmuting mid-playback would resume from
            // a stale position.
            const int dropBytes = static_cast<int>(qMin<qint64>(maxlen, e->ring.size()));
            e->ring.remove(0, dropBytes);
            continue;
        }

        const int copyBytes = static_cast<int>(qMin<qint64>(maxlen, e->ring.size()));
        const int copySamples = copyBytes / static_cast<int>(sizeof(int16_t));
        const int16_t *src = reinterpret_cast<const int16_t *>(e->ring.constData());
        if (qFuzzyCompare(gain, 1.0)) {
            for (int i = 0; i < copySamples; ++i)
                accum[i] += src[i];
        } else {
            for (int i = 0; i < copySamples; ++i)
                accum[i] += static_cast<int32_t>(src[i] * gain);
        }
        e->ring.remove(0, copyBytes);
        anyMixed = true;
    }

    if (anyMixed) {
        int16_t *dst = reinterpret_cast<int16_t *>(data);
        for (int i = 0; i < sampleCount; ++i) {
            dst[i] = static_cast<int16_t>(qBound<int32_t>(-32768, accum[i], 32767));
        }
    }

    const int64_t deltaUs = static_cast<int64_t>(frameCount) * 1'000'000
                            / AudioMixer::kSampleRateHz;
    m_mixer->m_writeCursorUs.fetch_add(deltaUs, std::memory_order_release);

    return maxlen;
}

// =====================================================================
// AudioMixer
// =====================================================================
AudioMixer::AudioMixer(QObject *parent) : QObject(parent) {
    m_format.setSampleRate(kSampleRateHz);
    m_format.setChannelCount(kChannels);
    m_format.setSampleFormat(QAudioFormat::Int16);

    m_decodeRunner = new AudioDecodeRunner(this);
    m_decodeRunner->start();
}

AudioMixer::~AudioMixer() {
    if (m_decodeRunner) {
        m_decodeRunner->requestStop();
        m_decodeRunner->wait(2000);
        delete m_decodeRunner;
        m_decodeRunner = nullptr;
    }
    {
        QMutexLocker lock(&m_controlMutex);
        m_playing.store(false, std::memory_order_release);
        if (m_sink) m_sink->stop();
        releaseAllEntriesLocked();
        if (m_io) {
            m_io->close();
            delete m_io;
            m_io = nullptr;
        }
        if (m_sink) {
            delete m_sink;
            m_sink = nullptr;
        }
    }
}

void AudioMixer::setSequence(const QVector<PlaybackEntry> &entries) {
    QMutexLocker lock(&m_controlMutex);

    QHash<AudioTrackKey, AudioDecoderEntry *> retained;
    int maxTrack = -1;
    int openedCount = 0;

    for (const auto &e : entries) {
        ++openedCount;
        if (openedCount > kMaxAudioTracks) {
            emit decoderError(QStringLiteral("AudioMixer: exceeded MAX_AUDIO_TRACKS=%1, dropping extra entry %2")
                              .arg(kMaxAudioTracks).arg(e.filePath));
            continue;
        }
        AudioTrackKey key{
            e.filePath,
            qRound64(e.clipIn * 1000.0),
            e.sourceTrack,
            e.sourceClipIndex
        };
        if (e.sourceTrack > maxTrack) maxTrack = e.sourceTrack;

        AudioDecoderEntry *de = m_entries.take(key);
        if (de) {
            // Same key reappears — update mutable timeline metadata only.
            const auto oldStart = de->entry.timelineStart;
            const auto oldEnd = de->entry.timelineEnd;
            de->entry = e;
            if (!qFuzzyCompare(oldStart, e.timelineStart)
                || !qFuzzyCompare(oldEnd, e.timelineEnd)) {
                de->seekPending = true;
                de->ring.clear();
            }
            retained.insert(key, de);
        } else {
            de = new AudioDecoderEntry;
            de->entry = e;
            if (!openEntry(de)) {
                emit decoderError(QStringLiteral("Failed to open audio: %1").arg(e.filePath));
                closeEntry(de);
                continue;
            }
            retained.insert(key, de);
        }
    }

    // Anything still in m_entries is no longer in the new sequence.
    for (auto *de : qAsConst(m_entries)) {
        closeEntry(de);
    }
    m_entries = retained;

    if (m_trackStates.size() < maxTrack + 1)
        m_trackStates.resize(maxTrack + 1);

    recomputeEffectiveGainsLocked();
    ensureSinkLocked();
}

void AudioMixer::seekTo(int64_t timelineUs) {
    QMutexLocker lock(&m_controlMutex);
    m_writeCursorUs.store(timelineUs, std::memory_order_release);
    for (auto *e : qAsConst(m_entries)) {
        if (!e) continue;
        e->seekPending = true;
        e->ring.clear();
        e->eof = false;
    }
    if (m_sink) {
        // Drop any samples already in the OS audio buffer so post-seek audio
        // starts cleanly. Without this the listener hears ~hardware-buffer
        // worth of stale pre-seek audio after a long seek.
        m_sink->reset();
        if (m_io && m_playing.load(std::memory_order_acquire)) {
            m_sink->start(m_io);
        }
    }
}

void AudioMixer::play() {
    QMutexLocker lock(&m_controlMutex);
    m_playing.store(true, std::memory_order_release);
    ensureSinkLocked();
    if (m_sink) {
        if (m_sink->state() == QAudio::SuspendedState
            || m_sink->state() == QAudio::IdleState) {
            m_sink->resume();
        } else if (m_sink->state() == QAudio::StoppedState && m_io) {
            m_sink->start(m_io);
        }
    }
}

void AudioMixer::pause() {
    QMutexLocker lock(&m_controlMutex);
    m_playing.store(false, std::memory_order_release);
    if (m_sink && m_sink->state() == QAudio::ActiveState) {
        m_sink->suspend();
    }
}

void AudioMixer::stop() {
    QMutexLocker lock(&m_controlMutex);
    m_playing.store(false, std::memory_order_release);
    if (m_sink) m_sink->stop();
}

void AudioMixer::setTrackMute(int trackIdx, bool muted) {
    QMutexLocker lock(&m_controlMutex);
    if (trackIdx < 0 || trackIdx >= kMaxAudioTracks) return;
    if (m_trackStates.size() < trackIdx + 1)
        m_trackStates.resize(trackIdx + 1);
    m_trackStates[trackIdx].muted = muted;
    recomputeEffectiveGainsLocked();
}

void AudioMixer::setTrackSolo(int trackIdx, bool solo) {
    QMutexLocker lock(&m_controlMutex);
    if (trackIdx < 0 || trackIdx >= kMaxAudioTracks) return;
    if (m_trackStates.size() < trackIdx + 1)
        m_trackStates.resize(trackIdx + 1);
    m_trackStates[trackIdx].solo = solo;
    recomputeEffectiveGainsLocked();
}

void AudioMixer::setTrackGain(int trackIdx, double gain) {
    QMutexLocker lock(&m_controlMutex);
    if (trackIdx < 0 || trackIdx >= kMaxAudioTracks) return;
    if (m_trackStates.size() < trackIdx + 1)
        m_trackStates.resize(trackIdx + 1);
    m_trackStates[trackIdx].gain = qBound(0.0, gain, 4.0);
    recomputeEffectiveGainsLocked();
}

bool AudioMixer::ensureSinkLocked() {
    if (m_sink && m_io) return true;
    if (!m_io) {
        m_io = new MixerIODevice(this);
        m_io->open(QIODevice::ReadOnly);
    }
    if (!m_sink) {
        m_sink = new QAudioSink(m_format, this);
        // Keep the OS buffer modest so seek latency stays low while leaving
        // enough headroom that decoder hiccups don't underrun.
        m_sink->setBufferSize(static_cast<qsizetype>(kSampleRateHz) * kBytesPerFrame / 5); // ~200 ms
    }
    if (m_sink->state() == QAudio::StoppedState) {
        m_sink->start(m_io);
        if (!m_playing.load(std::memory_order_acquire))
            m_sink->suspend();
    }
    return true;
}

void AudioMixer::recomputeEffectiveGainsLocked() {
    bool anySolo = false;
    for (const auto &t : m_trackStates) if (t.solo) { anySolo = true; break; }
    for (auto &t : m_trackStates) {
        const bool audible = anySolo ? t.solo : !t.muted;
        t.effectiveGain = audible ? t.gain : 0.0;
    }
}

void AudioMixer::releaseAllEntriesLocked() {
    for (auto *e : qAsConst(m_entries)) {
        if (e) closeEntry(e);
    }
    m_entries.clear();
}

bool AudioMixer::openEntry(AudioDecoderEntry *e) {
    if (!e) return false;
    if (avformat_open_input(&e->fmtCtx, e->entry.filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(e->fmtCtx, nullptr) < 0)
        return false;
    int idx = av_find_best_stream(e->fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (idx < 0) return false;
    e->audioStreamIdx = idx;
    AVCodecParameters *par = e->fmtCtx->streams[idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;
    e->codecCtx = avcodec_alloc_context3(codec);
    if (!e->codecCtx) return false;
    if (avcodec_parameters_to_context(e->codecCtx, par) < 0) return false;
    if (avcodec_open2(e->codecCtx, codec, nullptr) < 0) return false;

    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&e->swrCtx,
            &outLayout, AV_SAMPLE_FMT_S16, AudioMixer::kSampleRateHz,
            &e->codecCtx->ch_layout, e->codecCtx->sample_fmt, e->codecCtx->sample_rate,
            0, nullptr) < 0) {
        return false;
    }
    if (!e->swrCtx || swr_init(e->swrCtx) < 0) return false;

    e->pkt = av_packet_alloc();
    e->frame = av_frame_alloc();
    if (!e->pkt || !e->frame) return false;
    e->seekPending = true;
    e->eof = false;
    return true;
}

void AudioMixer::closeEntry(AudioDecoderEntry *e) {
    if (!e) return;
    if (e->frame) av_frame_free(&e->frame);
    if (e->pkt) av_packet_free(&e->pkt);
    if (e->swrCtx) swr_free(&e->swrCtx);
    if (e->codecCtx) avcodec_free_context(&e->codecCtx);
    if (e->fmtCtx) avformat_close_input(&e->fmtCtx);
    delete e;
}

void AudioMixer::seekEntryToTimeline(AudioDecoderEntry *e, int64_t timelineUs) {
    if (!e || !e->fmtCtx || !e->codecCtx || e->audioStreamIdx < 0) return;
    const double tlSec = static_cast<double>(timelineUs) / 1e6;
    double fileLocalSec = e->entry.clipIn + (tlSec - e->entry.timelineStart);
    fileLocalSec = qBound(e->entry.clipIn, fileLocalSec, e->entry.clipOut);
    AVRational tb = e->fmtCtx->streams[e->audioStreamIdx]->time_base;
    const int64_t ts = static_cast<int64_t>(fileLocalSec / av_q2d(tb));
    av_seek_frame(e->fmtCtx, e->audioStreamIdx, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(e->codecCtx);
    e->ring.clear();
    e->eof = false;
}

void AudioMixer::resampleAndAppend(AudioDecoderEntry *e) {
    if (!e || !e->frame || !e->swrCtx) return;
    const int outSamples = swr_get_out_samples(e->swrCtx, e->frame->nb_samples);
    if (outSamples <= 0) return;
    QByteArray buf;
    buf.resize(outSamples * AudioMixer::kBytesPerFrame);
    uint8_t *out[1] = { reinterpret_cast<uint8_t *>(buf.data()) };
    const int got = swr_convert(e->swrCtx, out, outSamples,
                                const_cast<const uint8_t **>(e->frame->data),
                                e->frame->nb_samples);
    if (got > 0) {
        e->ring.append(buf.left(got * AudioMixer::kBytesPerFrame));
    }
}

void AudioMixer::refillRingForEntry(AudioDecoderEntry *e, int targetBytes) {
    if (!e || !e->fmtCtx || !e->codecCtx) return;
    while (e->ring.size() < targetBytes && !e->eof) {
        const int rc = av_read_frame(e->fmtCtx, e->pkt);
        if (rc == AVERROR_EOF) {
            // Drain decoder.
            avcodec_send_packet(e->codecCtx, nullptr);
            while (avcodec_receive_frame(e->codecCtx, e->frame) >= 0) {
                resampleAndAppend(e);
            }
            e->eof = true;
            break;
        }
        if (rc < 0) {
            // Read error — bail this round, retry on next refill tick.
            break;
        }
        if (e->pkt->stream_index != e->audioStreamIdx) {
            av_packet_unref(e->pkt);
            continue;
        }
        const int sendRc = avcodec_send_packet(e->codecCtx, e->pkt);
        av_packet_unref(e->pkt);
        if (sendRc < 0) continue;
        while (true) {
            const int recvRc = avcodec_receive_frame(e->codecCtx, e->frame);
            if (recvRc == AVERROR(EAGAIN) || recvRc == AVERROR_EOF) break;
            if (recvRc < 0) break;
            resampleAndAppend(e);
        }
    }
}

void AudioMixer::refillRings() {
    QMutexLocker lock(&m_controlMutex);
    if (m_entries.isEmpty()) return;
    const int64_t cursorUs = m_writeCursorUs.load(std::memory_order_acquire);
    for (auto *e : qAsConst(m_entries)) {
        if (!e) continue;
        const int64_t startUs = static_cast<int64_t>(e->entry.timelineStart * 1e6);
        const int64_t endUs = static_cast<int64_t>(e->entry.timelineEnd * 1e6);
        // Skip entries far in the future or already past — they don't need
        // their rings refilled.
        if (cursorUs < startUs - kPrerollLeadUs) continue;
        if (cursorUs >= endUs) continue;

        if (e->seekPending) {
            seekEntryToTimeline(e, qMax<int64_t>(cursorUs, startUs));
            e->seekPending = false;
        }
        if (e->ring.size() < kRingTargetBytes) {
            refillRingForEntry(e, kRingTargetBytes);
        }
    }
}
