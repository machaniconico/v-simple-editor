#include "AudioMixer.h"

#include <QElapsedTimer>
#include <QtGlobal>
#include <QDebug>
#include <QtMath>
#include <QThread>
#include <QVarLengthArray>
#include <QSet>
#include <QStringList>
#include <QWaitCondition>
#include <QMediaDevices>
#include <QAudioDevice>
#include <cstring>
#include <algorithm>
#include <limits>

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
    // Resampled s16 stereo, FIFO. Drained via ringHead advance instead of
    // QByteArray::remove(0, N) so the audio callback doesn't pay an O(N)
    // memmove per drain — the ring is compacted only when ringHead crosses
    // the kRingCompactThreshold below, amortising compaction cost.
    QByteArray ring;
    int ringHead = 0;
    // Timeline microseconds of the FIRST live sample (ring[ringHead]).
    // Used by readData to skip past samples behind the master clock when a
    // ring underrun + decoder catch-up has produced samples that should
    // have been mixed earlier. Without this tracker, late-arriving samples
    // would play out of phase with other tracks (CRITICAL #2 from the
    // Phase 2 review).
    int64_t ringStartTlUs = 0;
    bool eof = false;
    bool seekPending = true;         // first-time seek to entry's start when approached
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;

    AudioDecoderEntry() = default;
    AudioDecoderEntry(const AudioDecoderEntry &) = delete;
    AudioDecoderEntry &operator=(const AudioDecoderEntry &) = delete;

    // RAII tear-down. Free FFmpeg resources in the reverse order of
    // creation: frame -> pkt -> swr -> codec -> fmt. This means a
    // partial-failure openEntry path can simply `delete e;` rather than
    // relying on every caller to remember closeEntry().
    ~AudioDecoderEntry() {
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (swrCtx) swr_free(&swrCtx);
        if (codecCtx) avcodec_free_context(&codecCtx);
        if (fmtCtx) avformat_close_input(&fmtCtx);
    }
};

namespace {
// Phase 1e Win #10 — VEDITOR_STALL_TRACE=1 logs wall-time around
// refillRingForEntry's av_read_frame loop. Architect H1 (stall
// investigation): the loop runs under m_controlMutex, so a slow
// av_read_frame (cache miss / network share / sparse-keyframe
// 4h MP4) freezes both the audio sink callback and any GUI
// audio control call. Logging the per-call wall time helps the
// user identify whether the stall is rooted in audio I/O.
inline bool stallTraceEnabled()
{
    static const bool enabled = qEnvironmentVariableIntValue("VEDITOR_STALL_TRACE") != 0;
    return enabled;
}
inline constexpr qint64 kStallThresholdRefillMs = 100;

// Helpers to keep AudioDecoderEntry's head-indexed FIFO consistent.
inline int liveBytes(const AudioDecoderEntry &e) {
    return e.ring.size() - e.ringHead;
}
inline const char *liveData(const AudioDecoderEntry &e) {
    return e.ring.constData() + e.ringHead;
}
constexpr int kRingCompactThreshold = 32 * 1024;
} // namespace

class MixerIODevice : public QIODevice {
public:
    explicit MixerIODevice(AudioMixer *mixer) : m_mixer(mixer) {}
    ~MixerIODevice() override = default;

    // QAudioSink in Qt 6 pull mode (start(QIODevice*)) checks
    // bytesAvailable() before calling readData — if it returns 0 the
    // sink transitions to IdleState and STOPS pulling. Our mixer
    // generates data on demand from FFmpeg decoders + a silence
    // fallback, so we always have data to give. Returning a large
    // value here keeps the sink in ActiveState and the readData
    // callback firing. Without this override, the sink went Active
    // for ~8 ms after start() then went Idle and never called
    // readData again — the actual root cause of "no audio plays" in
    // Phase 2 sum-mix.
    bool isSequential() const override { return true; }
    // Advertise enough on-demand availability to keep Qt's pull-mode
    // worker from declaring the device starved. Anything >= one sink
    // period (~10–20 ms typical) keeps it Active; we use 1 s as a safety
    // margin. Do NOT return INT64_MAX or similar — some Qt backends use
    // bytesAvailable() to size an internal pre-buffer and a huge value
    // freezes the audio worker for several seconds.
    qint64 bytesAvailable() const override {
        return AudioMixer::kSampleRateHz * AudioMixer::kBytesPerFrame;
    }

protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *, qint64) override { return -1; }
private:
    AudioMixer *m_mixer;
};

// AudioDecodeRunner — dedicated decode thread that periodically refills
// ring buffers ahead of the audio sink. When refillRings reports no work
// (every active entry's ring is already at target), the runner sleeps
// kIdleSleepMs instead of kBusySleepMs to keep idle CPU draw negligible.
// External wake() can short-circuit the wait when setSequence / seekTo /
// play schedules new work.
class AudioDecodeRunner : public QThread {
public:
    static constexpr int kBusySleepMs = 5;
    static constexpr int kIdleSleepMs = 50;

    explicit AudioDecodeRunner(AudioMixer *m, QObject *parent = nullptr)
        : QThread(parent), m_mixer(m) {}

    void requestStop() {
        m_stopRequested.store(true, std::memory_order_release);
        QMutexLocker lock(&m_wakeMutex);
        m_wakeCond.wakeAll();
    }
    void wake() {
        // Mark work pending before wakeOne so a wake() that races between
        // refillRings()'s return and run()'s wait() is not lost. Without
        // this flag, that window left the decoder asleep for up to
        // kIdleSleepMs after setSequence/seekTo/play/stall events had
        // already queued more decode work.
        m_workPending.store(true, std::memory_order_release);
        QMutexLocker lock(&m_wakeMutex);
        m_wakeCond.wakeOne();
    }
protected:
    void run() override {
        while (!m_stopRequested.load(std::memory_order_acquire)) {
            const bool didWork = m_mixer->refillRings();
            QMutexLocker lock(&m_wakeMutex);
            if (m_stopRequested.load(std::memory_order_acquire)) break;
            // Skip the wait when wake() set the flag during the
            // refillRings() pass. exchange clears the flag atomically.
            if (m_workPending.exchange(false, std::memory_order_acq_rel))
                continue;
            m_wakeCond.wait(&m_wakeMutex, didWork ? kBusySleepMs : kIdleSleepMs);
        }
    }
private:
    AudioMixer *m_mixer;
    QMutex m_wakeMutex;
    QWaitCondition m_wakeCond;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_workPending{false};
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

    // Refresh the audible-lag publication so audibleClockUs() can stay
    // lock-free. The audio worker thread is the only path that calls into
    // QAudioSink internals; doing it here avoids the GUI thread blocking on
    // m_controlMutex (and on Qt sink mutexes) once per video tick — which
    // was starving the audio worker before this fix and producing silence.
    if (m_mixer->m_sink) {
        const qint64 buffered = m_mixer->m_sink->bufferSize() - m_mixer->m_sink->bytesFree();
        const int64_t lagUs = (buffered > 0)
            ? buffered * 1'000'000LL
                  / (static_cast<int64_t>(AudioMixer::kBytesPerFrame) * AudioMixer::kSampleRateHz)
            : 0;
        m_mixer->m_audibleLagUs.store(lagUs, std::memory_order_release);
    }

    if (!m_mixer->m_playing.load(std::memory_order_acquire)) {
        m_mixer->m_consecutiveStallCallbacks.store(0, std::memory_order_release);
        return maxlen;
    }

    const int64_t cursorUs = m_mixer->m_writeCursorUs.load(std::memory_order_acquire);
    const int frameCount = static_cast<int>(maxlen / AudioMixer::kBytesPerFrame);
    const int sampleCount = frameCount * AudioMixer::kChannels;
    const int64_t maxlenUs = static_cast<int64_t>(frameCount) * 1'000'000
                             / AudioMixer::kSampleRateHz;

    // Accumulator for the sum-mix. int32 lets us add many s16 sources before
    // saturating; gains are clamped per entry below so the headroom math
    // holds even if PlaybackEntry::volume bypassed the Timeline 0..2 clamp.
    QVarLengthArray<int32_t, 16384> accum(sampleCount);
    std::memset(accum.data(), 0, sampleCount * sizeof(int32_t));

    auto bytesToUs = [](qint64 bytes) -> int64_t {
        return bytes * 1'000'000LL
               / (AudioMixer::kSampleRateHz * AudioMixer::kBytesPerFrame);
    };
    auto usToBytes = [](int64_t us) -> qint64 {
        return us * AudioMixer::kSampleRateHz * AudioMixer::kBytesPerFrame
               / 1'000'000LL;
    };

    bool anyMixed = false;
    // Set when an entry is active at the current cursor but its ring has no
    // samples yet (decoder warming up after open/seek). Used below to freeze
    // the cursor for up to kMaxStallCallbacks so we don't race past samples
    // the decoder is about to produce — which would land them in the late-drop
    // path and discard them, leaving permanent silence.
    bool entryActiveButStalled = false;
    for (auto it = m_mixer->m_entries.begin(); it != m_mixer->m_entries.end(); ++it) {
        AudioDecoderEntry *e = it.value();
        if (!e) continue;
        const int64_t startUs = static_cast<int64_t>(e->entry.timelineStart * 1e6);
        const int64_t endUs = static_cast<int64_t>(e->entry.timelineEnd * 1e6);
        if (cursorUs < startUs || cursorUs >= endUs) continue;
        if (liveBytes(*e) <= 0) {
            entryActiveButStalled = true;
            continue;
        }

        // Drop samples that landed in the ring late — i.e. their timeline
        // position is already behind the master cursor. Without this, a
        // ring underrun + decoder catch-up sequence permanently shifts the
        // entry's audio relative to the rest of the mix.
        if (e->ringStartTlUs < cursorUs) {
            const int64_t lateUs = cursorUs - e->ringStartTlUs;
            const qint64 lateB = usToBytes(lateUs);
            const qint64 dropBytes = qMin<qint64>(lateB, liveBytes(*e));
            if (dropBytes > 0) {
                e->ringHead += static_cast<int>(dropBytes);
                e->ringStartTlUs += bytesToUs(dropBytes);
            }
            if (liveBytes(*e) <= 0) continue;
        }
        // If samples sit in the future the entry shouldn't be mixed yet.
        if (e->ringStartTlUs > cursorUs + maxlenUs / 2) continue;

        double gain = qBound(0.0, e->entry.volume, 4.0);
        if (e->entry.audioMuted) gain = 0.0;
        const int trackIdx = e->entry.sourceTrack;
        if (trackIdx >= 0 && trackIdx < m_mixer->m_trackStates.size())
            gain *= m_mixer->m_trackStates[trackIdx].effectiveGain;
        if (gain <= 0.0) {
            // Drain the ring so this muted entry stays in time with the
            // cursor; otherwise unmuting mid-playback would resume from a
            // stale position.
            const int dropBytes = static_cast<int>(qMin<qint64>(maxlen, liveBytes(*e)));
            e->ringHead += dropBytes;
            e->ringStartTlUs += bytesToUs(dropBytes);
            continue;
        }

        const int copyBytes = static_cast<int>(qMin<qint64>(maxlen, liveBytes(*e)));
        const int copySamples = copyBytes / static_cast<int>(sizeof(int16_t));
        const int16_t *src = reinterpret_cast<const int16_t *>(liveData(*e));
        if (qFuzzyCompare(gain, 1.0)) {
            for (int i = 0; i < copySamples; ++i)
                accum[i] += src[i];
        } else {
            for (int i = 0; i < copySamples; ++i)
                accum[i] += static_cast<int32_t>(src[i] * gain);
        }
        e->ringHead += copyBytes;
        e->ringStartTlUs += bytesToUs(copyBytes);
        anyMixed = true;
    }

    // Compact rings whose consumed prefix has grown past the threshold.
    // O(remaining) memmove, but only one per ~32 KB consumed (~170 ms of
    // audio at 48 kHz stereo s16) — amortised cost is constant per byte.
    for (auto it = m_mixer->m_entries.begin(); it != m_mixer->m_entries.end(); ++it) {
        AudioDecoderEntry *e = it.value();
        if (!e) continue;
        if (e->ringHead >= kRingCompactThreshold) {
            e->ring.remove(0, e->ringHead);
            e->ringHead = 0;
        }
    }

    if (anyMixed) {
        int16_t *dst = reinterpret_cast<int16_t *>(data);
        for (int i = 0; i < sampleCount; ++i) {
            dst[i] = static_cast<int16_t>(qBound<int32_t>(-32768, accum[i], 32767));
        }
    }

    const int64_t deltaUs = static_cast<int64_t>(frameCount) * 1'000'000
                            / AudioMixer::kSampleRateHz;

    // Freeze the cursor while we're waiting on the decoder. If we always
    // advance, the very next callback's late-drop discards the samples the
    // decoder is about to produce — they arrive timestamped at the freeze
    // point but the cursor is now ahead. Result: permanent silence at every
    // cold start / seek. Bound the freeze with kMaxStallCallbacks so a truly
    // broken decoder (avformat error, file unavailable) doesn't deadlock the
    // video pacer indefinitely.
    constexpr int kMaxStallCallbacks = 50; // ~50 callbacks * ~21 ms = ~1 s
    if (entryActiveButStalled && !anyMixed) {
        const int stalls = m_mixer->m_consecutiveStallCallbacks.fetch_add(1,
                               std::memory_order_acq_rel) + 1;
        // Wake the decode thread out of its idle 50 ms wait so it gets a
        // chance to refill before this stall budget runs out.
        if (m_mixer->m_decodeRunner) m_mixer->m_decodeRunner->wake();
        if (stalls < kMaxStallCallbacks) {
            return maxlen;
        }
        // Fall through and advance the cursor so video can keep moving.
    } else {
        m_mixer->m_consecutiveStallCallbacks.store(0, std::memory_order_release);
    }

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

    qInfo() << "AudioMixer::ctor format SR=" << m_format.sampleRate()
            << "Ch=" << m_format.channelCount()
            << "Fmt=" << m_format.sampleFormat();

    m_decodeRunner = new AudioDecodeRunner(this);
    m_decodeRunner->start();
    qInfo() << "AudioMixer::ctor decode thread started running="
            << m_decodeRunner->isRunning();
}

AudioMixer::~AudioMixer() {
    // Stop the decode thread first so it can't queue more refills.
    if (m_decodeRunner) {
        m_decodeRunner->requestStop();
        m_decodeRunner->wait();   // bounded by stop flag — refillRings exits at the top of its loop
        delete m_decodeRunner;
        m_decodeRunner = nullptr;
    }
    // Mark not playing and stop the audio sink BEFORE locking. QAudioSink
    // delivers readData callbacks on its own worker thread; if we hold the
    // mutex during stop(), an in-flight callback that's already parked on
    // the mutex will resume against a half-destroyed object after we
    // release the lock. Stopping the sink first lets that callback finish
    // (it sees m_playing=false and returns silence) so the lock contention
    // is gone by the time we tear m_io / m_sink / m_entries down.
    m_playing.store(false, std::memory_order_release);
    if (m_sink) m_sink->stop();

    QMutexLocker lock(&m_controlMutex);
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

void AudioMixer::setSequence(const QVector<PlaybackEntry> &entries) {
    qInfo() << "AudioMixer::setSequence in=" << entries.size()
            << "existing=" << m_entries.size();
    QStringList pendingErrors;
    {
        QMutexLocker lock(&m_controlMutex);

        QHash<AudioTrackKey, AudioDecoderEntry *> retained;
        int maxTrack = -1;
        QSet<int> uniqueTracks;

        for (const auto &e : entries) {
            // Cap on UNIQUE source tracks, not total entry count, so a
            // single track with many clips doesn't get its trailing entries
            // silently dropped.
            uniqueTracks.insert(e.sourceTrack);
            if (uniqueTracks.size() > kMaxAudioTracks
                && !uniqueTracks.contains(e.sourceTrack)) {
                pendingErrors << QStringLiteral("AudioMixer: exceeded MAX_AUDIO_TRACKS=%1, dropping track %2 entry %3")
                                  .arg(kMaxAudioTracks).arg(e.sourceTrack).arg(e.filePath);
                continue;
            }
            AudioTrackKey key{
                e.filePath,
                qRound64(e.clipIn * 1000.0),
                e.sourceTrack,
                e.sourceClipIndex
            };
            if (e.sourceTrack > maxTrack) maxTrack = e.sourceTrack;

            // Defensive: if the same key appears twice in one batch the
            // earlier-opened entry would otherwise be overwritten and leak.
            if (retained.contains(key)) {
                pendingErrors << QStringLiteral("AudioMixer: duplicate AudioTrackKey for %1 (clipIn=%2 track=%3 clipIdx=%4); keeping the first")
                                  .arg(e.filePath).arg(key.clipInMs).arg(key.sourceTrack).arg(key.sourceClipIndex);
                continue;
            }

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
                    de->ringHead = 0;
                }
                retained.insert(key, de);
            } else {
                de = new AudioDecoderEntry;
                de->entry = e;
                if (!openEntry(de)) {
                    pendingErrors << QStringLiteral("AudioMixer: failed to open audio: %1").arg(e.filePath);
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
        qInfo() << "AudioMixer::setSequence reconciled entries=" << m_entries.size()
                << "trackStates=" << m_trackStates.size()
                << "sink=" << (m_sink ? m_sink->state() : QAudio::StoppedState);
    }
    // Emit AFTER releasing the mutex so any future re-entrant slot (e.g. a
    // listener that calls back into setSequence) does not deadlock.
    for (const auto &msg : qAsConst(pendingErrors))
        emit decoderError(msg);
    if (m_decodeRunner) m_decodeRunner->wake();
}

void AudioMixer::seekTo(int64_t timelineUs) {
    qInfo() << "AudioMixer::seekTo us=" << timelineUs
            << "playing=" << m_playing.load(std::memory_order_acquire);
    // De-dup: if we're already at exactly this timeline position with
    // the sink in ActiveState, skip the stop/start cycle. Each
    // QAudioSink restart dispatches synchronously on the main thread
    // (~10–20 ms) and audibly clicks. Win #6 overlay rotation +
    // sequenceChanged storm previously produced 256 seek calls in a
    // 30 s session, the primary source of the "V2 のところでノイズ"
    // user report. Skip only when state matches — paused/idle or a
    // pending sink rebuild still needs the full path.
    {
        const int64_t cur = m_writeCursorUs.load(std::memory_order_acquire);
        const auto sinkState = m_sink ? m_sink->state() : QAudio::IdleState;
        if (cur == timelineUs
            && sinkState == QAudio::ActiveState
            && m_playing.load(std::memory_order_acquire)) {
            // m_audibleLagUs and per-entry seekPending stay valid here:
            // the cursor never moved and the sink kept producing samples,
            // so the prior seek's bookkeeping remains accurate.
            return;
        }
        // Iteration 13 — tolerance-based dedup for clip boundary advance.
        // User report 「たまに音声にノイズが入る」: at every clip boundary
        // VideoPlayer::advanceToEntry calls m_mixer->seekTo(next.timelineStart *
        // AV_TIME_BASE), but AudioMixer's master clock has typically already
        // crossed the boundary by a few ms via natural playback. The strict
        // equality dedup above misses (cur ≠ timelineUs by µs–ms), the 50 ms
        // Fix K dedup also misses (the last seekTo was on the previous
        // boundary, far outside the 50 ms window), so the full path runs:
        // sinkSnap->reset() drops OS-buffered samples + sinkSnap->start()
        // restarts the audio backend → 5–15 ms gap + transient pop = the
        // boundary click the user is hearing.
        // When the cursor and target are within kTolerantDedupUs (100 ms)
        // and the sink is already producing the right audio (Active +
        // playing), skip the sink restart — the audio is correct, only the
        // bookkeeping cursor needs the small bump. Trade-off: up to ~100 ms
        // of AV-sync drift between video PTS and audio output, bounded to
        // one OS-buffer period (~170 ms at 48 kHz/8 KB) and self-corrected
        // via correctVideoDriftAgainstAudioClock; imperceptible vs. the
        // audible click. Per-entry rings stay intact because the active
        // entry is already streaming.
        // Opt-out: VEDITOR_SEEK_TOLERANT_DEDUP_DISABLE=1.
        static const bool kTolerantDedupDisabled =
            qEnvironmentVariableIntValue("VEDITOR_SEEK_TOLERANT_DEDUP_DISABLE") != 0;
        static constexpr int64_t kTolerantDedupUs = 100'000;
        const int64_t deltaUs = cur >= timelineUs ? (cur - timelineUs)
                                                  : (timelineUs - cur);
        if (!kTolerantDedupDisabled
            && deltaUs < kTolerantDedupUs
            && sinkState == QAudio::ActiveState
            && m_playing.load(std::memory_order_acquire)) {
            QMutexLocker lock(&m_controlMutex);
            m_writeCursorUs.store(timelineUs, std::memory_order_release);
            // NIT-2 (architect): restart the Fix K scrub-dedup timer here
            // too. Otherwise rapid back-to-back tolerance hits (e.g.,
            // multi-clip timeline boundaries arriving < 50 ms apart) would
            // let the next call slip into Fix K's full ring.clear() path,
            // wiping the rings the tolerance path was meant to preserve.
            m_lastSeekToCallTimer.restart();
            // 1 Hz throttle keeps boundary-rate logging out of the file.
            static QElapsedTimer tolerantLogThrottle;
            if (!tolerantLogThrottle.isValid()
                || tolerantLogThrottle.elapsed() >= 1000) {
                qInfo() << "AudioMixer::seekTo tolerant dedup hit cur=" << cur
                        << "target=" << timelineUs << "delta=" << deltaUs;
                tolerantLogThrottle.start();
            }
            return;
        }
        // Phase 1e Win #13 — Fix K: time-based scrub dedup. When the user
        // drags the timeline slider or a post-loadFile settling burst fires
        // sequenceChanged at ~35 ms cadence, each seekTo arrives with a
        // *different* timelineUs and slips past the equality dedup above —
        // but the actual cost is the synchronous sinkSnap->reset() +
        // sinkSnap->start() pair below, which monopolises the GUI thread
        // for ~15 ms per call. Empirical log
        // veditor_20260501_103732.log @ 10:39:39.673-983 logged 8 such
        // calls in 310 ms = ~120 ms cumulative main-thread block, which
        // slipped scheduleNextFrame into the !advanced auto-pause path
        // (VideoPlayer.cpp:2911-2923) and re-armed the play() cascade
        // outside Fix J's 200 ms window. Inside a 50 ms window we still
        // update the cursor and invalidate the per-entry rings (so the
        // next readData() reads from the new position) but skip the sink
        // restart entirely. The next seekTo outside the window — or the
        // user releasing the slider — will run the full path with the
        // final position. Cursor lag stays bounded to one window.
        static const bool kScrubDedupDisabled =
            qEnvironmentVariableIntValue("VEDITOR_SEEK_SCRUB_DEDUP_DISABLE") != 0;
        if (!kScrubDedupDisabled
            && sinkState == QAudio::ActiveState
            && m_playing.load(std::memory_order_acquire)
            && m_lastSeekToCallTimer.isValid()
            && m_lastSeekToCallTimer.elapsed() < 50) {
            QMutexLocker lock(&m_controlMutex);
            m_writeCursorUs.store(timelineUs, std::memory_order_release);
            m_audibleLagUs.store(0, std::memory_order_release);
            m_consecutiveStallCallbacks.store(0, std::memory_order_release);
            for (auto *e : qAsConst(m_entries)) {
                if (!e) continue;
                e->seekPending = true;
                e->ring.clear();
                e->ringHead = 0;
                e->ringStartTlUs = timelineUs;
                e->eof = false;
            }
            // Wake the decode runner so the next readData() pulls from the
            // updated cursor without an audible click. The sink stays in
            // ActiveState; the worker thread will refill from the new
            // position transparently.
            if (m_decodeRunner) m_decodeRunner->wake();
            return;
        }
    }
    m_lastSeekToCallTimer.restart();
    // Hoist QAudioSink state changes outside the lock — start/reset can
    // synchronously dispatch into MixerIODevice::readData on the audio
    // worker thread, which would deadlock if it tries to take this same
    // mutex.
    QAudioSink *sinkSnap = nullptr;
    MixerIODevice *ioSnap = nullptr;
    bool restartAfterReset = false;
    {
        QMutexLocker lock(&m_controlMutex);
        m_writeCursorUs.store(timelineUs, std::memory_order_release);
        // Drop the stale OS-buffered estimate too: sinkSnap->reset() below
        // empties the OS buffer, so the next readData() will publish a
        // fresh lag against an empty buffer. Without this reset, the first
        // audibleClockUs() call after a seek subtracted the pre-seek
        // bufferedUs from the post-seek cursor.
        m_audibleLagUs.store(0, std::memory_order_release);
        m_consecutiveStallCallbacks.store(0, std::memory_order_release);
        for (auto *e : qAsConst(m_entries)) {
            if (!e) continue;
            e->seekPending = true;
            e->ring.clear();
            e->ringHead = 0;
            e->ringStartTlUs = timelineUs;
            e->eof = false;
        }
        sinkSnap = m_sink;
        ioSnap = m_io;
        restartAfterReset = (m_io && m_playing.load(std::memory_order_acquire));
    }
    if (sinkSnap) {
        // Drop OS-buffered samples so post-seek audio starts cleanly.
        sinkSnap->reset();
        if (restartAfterReset && ioSnap) {
            sinkSnap->start(ioSnap);
        }
    }
    if (m_decodeRunner) m_decodeRunner->wake();
}

void AudioMixer::play() {
    qInfo() << "AudioMixer::play entries=" << m_entries.size();
    QAudioSink *sinkSnap = nullptr;
    QIODevice *ioSnap = nullptr;
    {
        QMutexLocker lock(&m_controlMutex);
        m_playing.store(true, std::memory_order_release);
        ensureSinkLocked();
        sinkSnap = m_sink;
        ioSnap = m_io;
    }
    // sink->start() / resume() must run OUTSIDE the lock — Qt's audio
    // sink can synchronously dispatch into MixerIODevice::readData on
    // its worker thread, which would deadlock if we still held
    // m_controlMutex.
    if (sinkSnap) {
        const auto preState = sinkSnap->state();
        if (preState == QAudio::SuspendedState
            || preState == QAudio::IdleState) {
            sinkSnap->resume();
        } else if (preState == QAudio::StoppedState && ioSnap) {
            sinkSnap->start(ioSnap);
        }
        qInfo() << "AudioMixer::play sink pre=" << preState
                << "post=" << sinkSnap->state()
                << "error=" << sinkSnap->error();
    } else {
        qWarning() << "AudioMixer::play m_sink is null after ensureSinkLocked!";
    }
    if (m_decodeRunner) m_decodeRunner->wake();
}

void AudioMixer::pause() {
    qInfo() << "AudioMixer::pause";
    // suspend()/stop() must be called outside m_controlMutex for the same
    // reason play()/seekTo() do: Qt's QAudioSink synchronously waits for
    // the in-flight readData callback to finish before transitioning state,
    // and that callback is parked on m_controlMutex. ABBA deadlock.
    QAudioSink *sinkSnap = nullptr;
    {
        QMutexLocker lock(&m_controlMutex);
        m_playing.store(false, std::memory_order_release);
        if (m_sink && m_sink->state() == QAudio::ActiveState)
            sinkSnap = m_sink;
    }
    if (sinkSnap) sinkSnap->suspend();
}

void AudioMixer::stop() {
    qInfo() << "AudioMixer::stop";
    QAudioSink *sinkSnap = nullptr;
    {
        QMutexLocker lock(&m_controlMutex);
        m_playing.store(false, std::memory_order_release);
        sinkSnap = m_sink;
    }
    if (sinkSnap) sinkSnap->stop();
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
    // NOTE: callers may hold m_controlMutex. We must not call
    // m_sink->start() here because Qt's audio sink (especially the
    // Windows backend) can synchronously dispatch into MixerIODevice::readData
    // on the audio worker thread, and readData takes m_controlMutex —
    // so a start() call from inside the lock deadlocks the audio worker
    // and freezes the GUI thread for the duration of Qt's internal
    // timeout. Sink construction is fine (no callbacks fire); the actual
    // start() is hoisted to startSinkUnlocked() called from outside the
    // mutex.
    if (m_sink && m_io) return true;
    if (!m_io) {
        m_io = new MixerIODevice(this);
        m_io->open(QIODevice::ReadOnly);
        qInfo() << "AudioMixer: MixerIODevice opened";
    }
    if (!m_sink) {
        // Probe the default audio device + verify format support before
        // constructing the sink. Without this, an unsupported sample
        // rate / channel layout silently produces a sink that's "started"
        // but emits no sound — the regression Phase 2 introduced.
        QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull()) {
            qWarning() << "AudioMixer: no default audio output device — sink will be silent";
        } else {
            qInfo() << "AudioMixer: default audio output =" << device.description();
            const bool supported = device.isFormatSupported(m_format);
            qInfo() << "AudioMixer: requested format supported by device =" << supported;
            if (!supported) {
                const QAudioFormat preferred = device.preferredFormat();
                qWarning() << "AudioMixer: format not supported, falling back to preferred"
                           << "SR=" << preferred.sampleRate()
                           << "Ch=" << preferred.channelCount()
                           << "Fmt=" << preferred.sampleFormat();
                // Keep our internal mix at 48k/s16/stereo (swr already
                // resamples decoded streams to that). If the device
                // really can't accept 48k/s16/stereo, hand it the
                // preferred format — but this means swr's output won't
                // match the sink's expectation. The mismatch is a known
                // hazard; the warning is the minimum so we know if it
                // ever fires on the user's rig.
                if (preferred.sampleRate() == kSampleRateHz
                    && preferred.channelCount() == kChannels
                    && preferred.sampleFormat() == QAudioFormat::Int16) {
                    qInfo() << "AudioMixer: preferred matches mix format — using preferred";
                    m_format = preferred;
                } else {
                    qWarning() << "AudioMixer: preferred format diverges from internal mix — keeping requested format and hoping for the best";
                }
            }
            m_sink = new QAudioSink(device, m_format, this);
        }
        if (!m_sink) {
            // Fallback: construct without explicit device.
            m_sink = new QAudioSink(m_format, this);
        }
        // Keep the OS buffer modest so seek latency stays low while leaving
        // enough headroom that decoder hiccups don't underrun.
        m_sink->setBufferSize(static_cast<qsizetype>(kSampleRateHz) * kBytesPerFrame / 5); // ~200 ms
        qInfo() << "AudioMixer: QAudioSink created bufferSize=" << m_sink->bufferSize()
                << "state=" << m_sink->state();

        // Surface state transitions and errors so silent sink failures
        // become visible in the log. QueuedConnection prevents the lambda
        // from running on the audio worker thread between ~AudioMixer's
        // m_sink->stop() (called before the lock) and m_sink = nullptr
        // (called inside the lock); without it, a state-change racing
        // shutdown would dereference a half-destroyed AudioMixer.
        QObject::connect(m_sink, &QAudioSink::stateChanged, this,
            [this](QAudio::State s) {
                qInfo() << "AudioMixer: sink stateChanged ->" << s
                        << "error=" << (m_sink ? m_sink->error() : QAudio::NoError);
            }, Qt::QueuedConnection);
    }
    // start() is intentionally NOT called here — it would deadlock when
    // ensureSinkLocked is invoked under m_controlMutex (the Windows
    // QAudioSink synchronously dispatches into readData during start,
    // and readData takes the same mutex). Callers (play/seekTo) start
    // the sink outside the lock.
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
    qInfo() << "AudioMixer::openEntry path=" << e->entry.filePath
            << "track=" << e->entry.sourceTrack;
    if (avformat_open_input(&e->fmtCtx, e->entry.filePath.toUtf8().constData(), nullptr, nullptr) < 0) {
        qWarning() << "AudioMixer::openEntry avformat_open_input FAILED" << e->entry.filePath;
        return false;
    }
    if (avformat_find_stream_info(e->fmtCtx, nullptr) < 0) {
        qWarning() << "AudioMixer::openEntry avformat_find_stream_info FAILED";
        return false;
    }
    int idx = av_find_best_stream(e->fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (idx < 0) {
        qWarning() << "AudioMixer::openEntry no audio stream found";
        return false;
    }
    e->audioStreamIdx = idx;
    AVCodecParameters *par = e->fmtCtx->streams[idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { qWarning() << "AudioMixer::openEntry no decoder for codec_id" << par->codec_id; return false; }
    e->codecCtx = avcodec_alloc_context3(codec);
    if (!e->codecCtx) { qWarning() << "AudioMixer::openEntry alloc_context3 failed"; return false; }
    if (avcodec_parameters_to_context(e->codecCtx, par) < 0) { qWarning() << "AudioMixer::openEntry params_to_context failed"; return false; }
    if (avcodec_open2(e->codecCtx, codec, nullptr) < 0) { qWarning() << "AudioMixer::openEntry avcodec_open2 failed"; return false; }
    qInfo() << "AudioMixer::openEntry codec=" << codec->name
            << "src SR=" << e->codecCtx->sample_rate
            << "ch=" << e->codecCtx->ch_layout.nb_channels
            << "fmt=" << e->codecCtx->sample_fmt;

    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&e->swrCtx,
            &outLayout, AV_SAMPLE_FMT_S16, AudioMixer::kSampleRateHz,
            &e->codecCtx->ch_layout, e->codecCtx->sample_fmt, e->codecCtx->sample_rate,
            0, nullptr) < 0) {
        qWarning() << "AudioMixer::openEntry swr_alloc_set_opts2 failed";
        return false;
    }
    if (!e->swrCtx || swr_init(e->swrCtx) < 0) {
        qWarning() << "AudioMixer::openEntry swr_init failed swrCtx="
                   << static_cast<void *>(e->swrCtx);
        return false;
    }

    e->pkt = av_packet_alloc();
    e->frame = av_frame_alloc();
    if (!e->pkt || !e->frame) { qWarning() << "AudioMixer::openEntry packet/frame alloc failed"; return false; }
    e->seekPending = true;
    e->eof = false;
    qInfo() << "AudioMixer::openEntry SUCCESS path=" << e->entry.filePath;
    return true;
}

void AudioMixer::closeEntry(AudioDecoderEntry *e) {
    // FFmpeg cleanup lives in ~AudioDecoderEntry now. Keeping this thin
    // wrapper around `delete` lets callers say "close this entry" without
    // reading the destructor's full responsibility list.
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
    e->ringHead = 0;
    // The next sample appended to the ring corresponds to the timeline
    // position we just seeked the file to.
    e->ringStartTlUs = timelineUs;
    e->eof = false;
}

void AudioMixer::resampleAndAppend(AudioDecoderEntry *e) {
    if (!e || !e->frame || !e->swrCtx) return;
    const int outSamples = swr_get_out_samples(e->swrCtx, e->frame->nb_samples);
    if (outSamples <= 0) return;
    const int prevSize = e->ring.size();
    e->ring.resize(prevSize + outSamples * AudioMixer::kBytesPerFrame);
    uint8_t *out[1] = {
        reinterpret_cast<uint8_t *>(e->ring.data()) + prevSize
    };
    const int got = swr_convert(e->swrCtx, out, outSamples,
                                const_cast<const uint8_t **>(e->frame->data),
                                e->frame->nb_samples);
    // Truncate to actual output count. Note: ringStartTlUs is unchanged
    // because we only appended to the back — readData advances ringStartTlUs
    // and ringHead on drain.
    if (got > 0) {
        e->ring.resize(prevSize + got * AudioMixer::kBytesPerFrame);
    } else {
        e->ring.resize(prevSize);
    }
}

void AudioMixer::refillRingForEntry(AudioDecoderEntry *e, int targetBytes) {
    if (!e || !e->fmtCtx || !e->codecCtx) return;
    // Phase 1e Win #10 stall trace — log when av_read_frame takes long
    // enough (>=100 ms) to plausibly contribute to the user-reported
    // multi-second stall. The mutex held across this loop blocks
    // MixerIODevice::readData on the audio worker thread, which would
    // explain a synchronous video freeze through the audio clock.
    QElapsedTimer stallTimer;
    if (stallTraceEnabled())
        stallTimer.start();
    while (liveBytes(*e) < targetBytes && !e->eof) {
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
    if (stallTraceEnabled() && stallTimer.isValid()) {
        const qint64 elapsedMs = stallTimer.elapsed();
        if (elapsedMs >= kStallThresholdRefillMs) {
            qWarning().noquote()
                << QStringLiteral("[stall>=%1ms] refillRingForEntry %2ms targetBytes=%3 file=%4")
                       .arg(kStallThresholdRefillMs)
                       .arg(elapsedMs)
                       .arg(targetBytes)
                       .arg(e ? e->entry.filePath : QString());
        }
    }
}

bool AudioMixer::refillRings() {
    QMutexLocker lock(&m_controlMutex);
    if (m_entries.isEmpty()) return false;
    const int64_t cursorUs = m_writeCursorUs.load(std::memory_order_acquire);
    // Bound decode work per refill pass so the 200 ms QAudioSink OS buffer
    // doesn't underrun while readData waits on m_controlMutex. ~8 KB per
    // entry decodes in well under a millisecond on modern hardware; the
    // 5 ms decode loop tops the ring back up to kRingTargetBytes within
    // ~16 ticks (~80 ms total).
    constexpr int kPerCallChunkBytes = 8 * 1024;
    int remainingSeekBudget = 1;  // at most one expensive avformat_seek_file per pass
    bool didWork = false;
    for (auto *e : qAsConst(m_entries)) {
        if (!e) continue;
        const int64_t startUs = static_cast<int64_t>(e->entry.timelineStart * 1e6);
        const int64_t endUs = static_cast<int64_t>(e->entry.timelineEnd * 1e6);
        // Skip entries far in the future or already past — they don't need
        // their rings refilled.
        if (cursorUs < startUs - kPrerollLeadUs) continue;
        if (cursorUs >= endUs) continue;

        if (e->seekPending) {
            if (remainingSeekBudget <= 0) continue;
            --remainingSeekBudget;
            const int64_t target = qMax<int64_t>(cursorUs, startUs);
            seekEntryToTimeline(e, target);
            e->seekPending = false;
            didWork = true;
        }
        if (liveBytes(*e) < kRingTargetBytes) {
            const int chunkTarget = qMin<int>(liveBytes(*e) + kPerCallChunkBytes,
                                              kRingTargetBytes);
            refillRingForEntry(e, chunkTarget);
            didWork = true;
        }
    }
    return didWork;
}

int64_t AudioMixer::audibleClockUs() const {
    // Lock-free: scheduleNextFrame and correctVideoDriftAgainstAudioClock
    // call this on every video tick. Locking m_controlMutex here previously
    // produced heavy contention with MixerIODevice::readData on the audio
    // worker thread, manifesting as audio underruns and silence. The
    // audible-lag estimate is published by readData itself so we can read
    // it via two atomic loads.
    const int64_t cursor = m_writeCursorUs.load(std::memory_order_acquire);
    const int64_t lag = m_audibleLagUs.load(std::memory_order_acquire);
    return qMax<int64_t>(0, cursor - lag);
}
