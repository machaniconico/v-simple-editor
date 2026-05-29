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
#include <chrono>
#include <cmath>
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
    double prevGain = 1.0; // last gain applied in inner copy loop, used to ramp at fragment seam
    // US-INT-2 Phase B M2 fix: fractional source-frame remainder carried
    // across atempo fragments. Without this, int(outputFrames*speedMul)
    // truncates per fragment, leaving ringHead lagging ringStartTlUs by up
    // to ~83µs each call on non-integer speedMul. Reset on seek/restart
    // alongside the ring reset; preserved across ring compaction (which
    // only reindexes — source pointer is unchanged).
    double atempoSrcFrameCarry = 0.0;
    bool eof = false;
    bool seekPending = true;         // first-time seek to entry's start when approached
    // US-FIX-7 (seek 砂嵐, R3 — two-flag design):
    //
    // needsPtsAnchor — ONE-SHOT, owned by resampleAndAppend.
    //   Set TRUE by seekEntryToTimeline (and cleared to false by the Fix-K /
    //   full-path ring-reset loops). On the very first resampleAndAppend call
    //   after a seek, reads best_effort_timestamp from the decoded frame,
    //   maps it to timeline µs via the inverse of seekEntryToTimeline's
    //   fileLocalSec formula, and writes ringStartTlUs = trueTlUs. Consumed
    //   (set false) by resampleAndAppend immediately after, regardless of
    //   whether the PTS was valid (NOPTS fallback keeps ringStartTlUs =
    //   timelineUs and also clears postSeekFullDrop — see below).
    //   resampleAndAppend is called from refillRingForEntry while
    //   m_controlMutex is held; readData cannot interleave.
    bool needsPtsAnchor = false;
    //
    // postSeekFullDrop — FAST-DRAIN GATE, owned by readData.
    //   Set TRUE by seekEntryToTimeline (and cleared to false by ring-reset
    //   loops). resampleAndAppend MUST NOT clear it (except the NOPTS path).
    //   readData: when true, bypasses kMaxLateDropUs = 2 ms throttle and
    //   drops the full lateUs = (cursorUs − trueTlUs) per callback, draining
    //   the entire real keyframe pre-roll in 1–2 callbacks rather than
    //   seconds. Cleared by readData once ringStartTlUs >= cursorUs (pre-roll
    //   gone) — both the `if postSeekFullDrop && ringStartTlUs>=cursorUs` path
    //   and the `else if postSeekFullDrop` path (ring empty, no pre-roll).
    //   Flag becomes reachable in readData because refillRings releases
    //   m_controlMutex before readData's next callback; by then needsPtsAnchor
    //   is already consumed and ringStartTlUs = trueTlUs.
    bool postSeekFullDrop = false;
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

// [P2-M3] Single SSOT for atempo pre-seek invalidation. Called from
// setSequence's retained-entry timeline-change branch and from setSpeedRamps's
// R10-a removed-key loop. The helper deliberately only touches the two fields
// that need pre-seek invalidation (atempoSrcFrameCarry, seekPending) because
// seekEntryToTimeline runs later under m_controlMutex and fully resets
// ring/ringHead/ringStartTlUs/prevGain/needsPtsAnchor/postSeekFullDrop. Forcing
// both call sites through this helper structurally prevents drift between the
// invalidate field sets (the previous code maintained two parallel resets in
// the retained-entry path and the R10-a loop, which had already drifted by one
// field across an earlier refactor).
inline void resetAtempoState(AudioDecoderEntry *de) {
    if (!de) return;
    de->atempoSrcFrameCarry = 0.0;
    de->seekPending = true;
}

// US-INT-2 Phase B: per-fragment atempo gate. Default OFF preserves the
// Phase A bit-identical path; VEDITOR_AUDIO_ATEMPO=1 opts into nearest-
// neighbor source-pointer drift on non-identity ramps (PRD permits
// uncorrected pitch for v1). Static-cached since qgetenv is per-fragment.
inline bool audioAtempoEnabledCached() {
    static const bool enabled = []() {
        const QByteArray v = qgetenv("VEDITOR_AUDIO_ATEMPO");
        return !v.isEmpty() && v != "0" && v != "false" && v != "FALSE";
    }();
    return enabled;
}
constexpr int kRingCompactThreshold = 32 * 1024;

// Linear-interpolate the per-clip volume envelope at a given clip-local
// time (seconds, 0.0 == clip start on the timeline). Empty envelope falls
// back to the static `volume` field so existing behaviour is preserved
// when no automation has been authored. Endpoints clamp (no extrapolation).
inline double evaluateVolumeEnvelope(const QVector<AudioGainPoint> &env,
                                     double clipLocalSec,
                                     double fallbackGain) {
    if (env.isEmpty()) return fallbackGain;
    if (clipLocalSec <= env.first().time) return env.first().gain;
    if (clipLocalSec >= env.last().time) return env.last().gain;
    for (int i = 0; i + 1 < env.size(); ++i) {
        const double aT = env[i].time;
        const double bT = env[i + 1].time;
        if (clipLocalSec >= aT && clipLocalSec <= bT) {
            const double span = bT - aT;
            if (span <= 0.0) return env[i + 1].gain;
            const double u = (clipLocalSec - aT) / span;
            return env[i].gain + (env[i + 1].gain - env[i].gain) * u;
        }
    }
    return fallbackGain;
}
} // namespace

// ---------------------------------------------------------------------------
// RBJ biquad cookbook — coefficient calculation for per-track realtime EQ.
// Three filter types: low-shelf (band index 0), peaking (1), high-shelf (2).
// All use the standard Audio EQ Cookbook formulas with shelf slope S=1.
// Coefficients are normalized so a0 = 1 and stored in EqBandCoefs.
// ---------------------------------------------------------------------------
namespace {
struct BiquadCoefs { double b0, b1, b2, a1, a2; };

BiquadCoefs calcLowShelf(double freq, double gainDB, double fs) {
    const double w = 2.0 * M_PI * freq / fs;
    const double cosW = std::cos(w);
    const double sinW = std::sin(w);
    const double A = std::pow(10.0, gainDB / 40.0);
    const double twoSqrtAalpha = 2.0 * std::sqrt(A) * (sinW * std::sqrt(2.0) / 2.0);
    const double Ap1 = A + 1.0;
    const double Am1 = A - 1.0;
    double b0 = A * (Ap1 - Am1 * cosW + twoSqrtAalpha);
    double b1 = 2.0 * A * (Am1 - Ap1 * cosW);
    double b2 = A * (Ap1 - Am1 * cosW - twoSqrtAalpha);
    double a0 = Ap1 + Am1 * cosW + twoSqrtAalpha;
    double a1 = -2.0 * (Am1 + Ap1 * cosW);
    double a2 = Ap1 + Am1 * cosW - twoSqrtAalpha;
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

BiquadCoefs calcPeaking(double freq, double gainDB, double Q, double fs) {
    const double w = 2.0 * M_PI * freq / fs;
    const double cosW = std::cos(w);
    const double sinW = std::sin(w);
    const double A = std::pow(10.0, gainDB / 40.0);
    const double alpha = sinW / (2.0 * Q);
    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cosW;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cosW;
    double a2 = 1.0 - alpha / A;
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

BiquadCoefs calcHighShelf(double freq, double gainDB, double fs) {
    const double w = 2.0 * M_PI * freq / fs;
    const double cosW = std::cos(w);
    const double sinW = std::sin(w);
    const double A = std::pow(10.0, gainDB / 40.0);
    const double twoSqrtAalpha = 2.0 * std::sqrt(A) * (sinW * std::sqrt(2.0) / 2.0);
    const double Ap1 = A + 1.0;
    const double Am1 = A - 1.0;
    double b0 = A * (Ap1 + Am1 * cosW + twoSqrtAalpha);
    double b1 = -2.0 * A * (Am1 + Ap1 * cosW);
    double b2 = A * (Ap1 + Am1 * cosW - twoSqrtAalpha);
    double a0 = Ap1 - Am1 * cosW + twoSqrtAalpha;
    double a1 = 2.0 * (Am1 - Ap1 * cosW);
    double a2 = Ap1 - Am1 * cosW - twoSqrtAalpha;
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

void recomputeEqCoefficients(AudioMixer::TrackState &ts) {
    constexpr double fs = AudioMixer::kSampleRateHz;
    for (int i = 0; i < ts.eq.bands.size() && i < 3; ++i) {
        const auto &band = ts.eq.bands[i];
        if (band.isFlat()) {
            ts.eqCoeffs[i] = {1.0, 0.0, 0.0, 0.0, 0.0};
            ts.z[i] = {};
            ts.eqCache[i] = {};
            continue;
        }
        BiquadCoefs c;
        if (i == 0)      c = calcLowShelf(band.frequency, band.gain, fs);
        else if (i == 1) c = calcPeaking(band.frequency, band.gain, band.q, fs);
        else             c = calcHighShelf(band.frequency, band.gain, fs);
        ts.eqCoeffs[i] = {c.b0, c.b1, c.b2, c.a1, c.a2};
        if (ts.eqCache[i].frequency != band.frequency || ts.eqCache[i].q != band.q) {
            ts.z[i] = {};
            ts.eqCache[i].frequency = band.frequency;
            ts.eqCache[i].q = band.q;
        }
    }
}
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

    // US-VFF-009: lag is now refreshed AFTER cursor advance at the end of
    // this function. Publishing it here (before fill) made audibleClockUs()
    // briefly return cursor + maxlenUs - 0 (cold-start prefill window where
    // bytesFree==bufferSize so lag=0 was published, then cursor jumped by
    // ~83 ms when we wrote samples below). Video saw "audio jumped ahead"
    // and skip-decoded up to 6 frames in one tick → cold-start fast-forward
    // even on single-track playback. Compute lag once cursor is already
    // advanced so audibleClockUs() reflects the actual audible position.

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
            // US-FIX-7 (R3 — fast-drain gate, owned here):
            // seekEntryToTimeline sets postSeekFullDrop=true and
            // needsPtsAnchor=true. resampleAndAppend (called from
            // refillRingForEntry under m_controlMutex) anchors
            // ringStartTlUs=trueTlUs and clears needsPtsAnchor, but leaves
            // postSeekFullDrop=true. By the time readData runs (after the
            // refillRings lock is released), postSeekFullDrop is still true
            // and ringStartTlUs=trueTlUs < cursorUs by exactly the real
            // keyframe pre-roll gap. This branch fires; postSeekFullDrop
            // bypasses kMaxLateDropUs so the gap drains in 1–2 callbacks
            // with zero valid-audio loss. Cleared here once
            // ringStartTlUs >= cursorUs; subsequent late-drops use the
            // normal 2 ms clamp (boundary-pop protection documented below).
            //
            // Normal path: Bound the late-drop click duration to 2 ms
            // (audible threshold). The tolerant-dedup path in seekTo bumps
            // m_writeCursorUs to the target without updating per-entry
            // ringStartTlUs, so this branch can fire with lateUs up to the
            // dedup window (~100 ms) and silently discard that much
            // legitimate in-time audio = boundary pop. Clamping to 2 ms
            // keeps the drop inaudible; the residual desync self-corrects
            // over the next few readData fragments.
            constexpr int64_t kMaxLateDropUs = 2'000;
            const int64_t clampedLateUs = e->postSeekFullDrop
                ? lateUs
                : qMin(lateUs, kMaxLateDropUs);
            const qint64 lateB = usToBytes(clampedLateUs);
            // US-FIX-7 (R6): align drop down to a frame boundary.
            // usToBytes uses integer division and produces an arbitrary byte
            // count; under postSeekFullDrop the full PTS-derived gap is passed,
            // giving a value that is generically NOT a multiple of kBytesPerFrame
            // (= 4). A non-frame-aligned ringHead misaligns L/R sample pairs on
            // every subsequent read → continuous static on the resumed audio.
            // Round down to the nearest kBytesPerFrame multiple before capping
            // against liveBytes (which is itself frame-aligned as long as this
            // invariant is maintained).
            // NOTE (R7): the frame-alignment floor means ringStartTlUs after
            // the drop lands STRICTLY below cursorUs by up to one frame (≤20 µs)
            // due to integer round-trip loss in usToBytes/bytesToUs. The clear
            // condition below uses a ≤1-frame tolerance instead of exact equality
            // to handle this sub-frame residual. DO NOT change to >= comparison.
            const qint64 lateBAligned =
                (lateB / AudioMixer::kBytesPerFrame) * AudioMixer::kBytesPerFrame;
            const qint64 dropBytes = qMin<qint64>(lateBAligned, liveBytes(*e));
            if (dropBytes > 0) {
                e->ringHead += static_cast<int>(dropBytes);
                e->ringStartTlUs += bytesToUs(dropBytes);
                e->atempoSrcFrameCarry = 0.0;
            }
            if (liveBytes(*e) <= 0) {
                // US-FIX-7 (R4/R5): ring emptied by the drop above.
                // Two sub-cases depending on whether the drop just reached T:
                //
                // (A) ringStartTlUs still < cursorUs (more pre-roll to drain,
                //     or exactly equal but no post-target bytes decoded yet):
                //     postSeekFullDrop is still true → mark entryActiveButStalled
                //     so the master-clock freeze gate holds m_writeCursorUs at T.
                //
                // (B) ringStartTlUs == cursorUs (this drop landed the label
                //     exactly at T) but the ring is empty (post-target audio not
                //     yet decoded): postSeekFullDrop is still true (the flag-clear
                //     below has NOT run yet — R5 reorder) → same stall path,
                //     cursor stays at T. The flag is cleared below only when
                //     liveBytes > 0 so the clearing callback is also the mixing
                //     callback, guaranteeing audio resumes from exactly T.
                //
                // The freeze is bounded by kMaxStallCallbacks (~1 s). If the
                // decoder is genuinely broken the stall count saturates and the
                // cursor resumes — bounded silence, never permanent freeze.
                // `&& !anyMixed` composes correctly for multi-track: another
                // streaming entry's anyMixed=true suppresses the freeze and the
                // clock runs with it; only the catching-up entry stays silent.
                if (e->postSeekFullDrop)
                    entryActiveButStalled = true;
                continue;
            }
            // Pre-roll fully drained AND liveBytes > 0 (post-target audio is
            // present and will be mixed this callback). Clear the flag here —
            // AFTER the empty-ring check — so the callback that clears
            // postSeekFullDrop is the same callback that mixes the first
            // post-target sample from exactly T. If we cleared it before the
            // liveBytes check (R4 ordering), the final pre-roll callback could
            // clear the flag and fall through to the master-clock advance with
            // nothing mixed, skipping ~one deltaUs (~21 ms) of post-target
            // content before the stall path could re-engage. (R5 reorder fix.)
            //
            // US-FIX-7 (R7): tolerance-based clear. After frame-aligning the
            // drop (R6), bytesToUs(lateBAligned) rounds down, so ringStartTlUs
            // lands STRICTLY below cursorUs by a sub-frame residual (≤20 µs).
            // An exact >= comparison never fires, stranding postSeekFullDrop=true
            // permanently and keeping the 2 ms clamp bypassed for the clip's
            // entire remaining life. Instead: if the deficit is <= one frame
            // (= bytesToUs(kBytesPerFrame) = 20 µs), pre-roll is effectively
            // exhausted — clear the flag. The <= boundary is required because
            // usToBytes(20) = 3 (< kBytesPerFrame), so a 20 µs residual
            // produces zero aligned-drop bytes and must itself trigger the clear.
            //
            // US-FIX-7 (R9, Q3) adjudicated-accepted edge — NO logic change
            // (the prior-round invariant "DO NOT change to >= comparison" is
            // respected; the <= boundary stays). When the post-drop residual
            // lands in the (one-frame, ~two-frame] band — e.g. lateUs=41 →
            // frame-aligned drop of 4 B = 20 µs → residual 21 µs > the 20 µs
            // (= bytesToUs(kBytesPerFrame)) tolerance — the flag does NOT clear
            // on that callback. If post-target audio is present (liveBytes>0,
            // so the empty-ring stall `continue` above was not taken) the entry
            // IS mixed once with ringStartTlUs ≤~40 µs behind cursor. This is
            // ACCEPTED, not a sandstorm: the drop is frame-aligned (R6) so
            // there is no L/R channel-interleave corruption — the mixed samples
            // are valid, in-order audio at most ~1-2 samples (≤~40 µs) phase-
            // early. It self-clears within ≤2 callbacks (the next callback's
            // smaller lateUs drops one frame and the residual falls into the
            // <= band), and the zero-progress band (residual ≤ one frame =
            // 20 µs) is fully covered by this <= clear so there is no permanent
            // strand. Mixing the ≤~40 µs-early frame-aligned audio for ≤1
            // callback is the deliberate R5/R7 tradeoff: the alternative —
            // suppressing the mix while the flag is set — would introduce a
            // ≤1-callback silence gap, which the R5 reorder comment above
            // explicitly rejects as the worse outcome.
            if (e->postSeekFullDrop &&
                (cursorUs - e->ringStartTlUs) <= bytesToUs(AudioMixer::kBytesPerFrame))
                e->postSeekFullDrop = false;
        } else if (e->postSeekFullDrop) {
            // ringStartTlUs already >= cursorUs (e.g. ring was empty when
            // the flag was set and cursor hasn't advanced past it yet).
            // Pre-roll is gone; clear the flag.
            e->postSeekFullDrop = false;
        }
        // If samples sit in the future the entry shouldn't be mixed yet.
        if (e->ringStartTlUs > cursorUs + maxlenUs / 2) continue;

        // Per-clip volume automation ("rubber band" envelope). Sampled at
        // the start of the readData fragment — the existing prevGain ramp
        // (~5 ms slew, see Iter 14 audio fix) absorbs any step between
        // fragments so per-sample evaluation is unnecessary for the typical
        // envelope curve.
        double clipGain = e->entry.volume;
        if (!e->entry.volumeEnvelope.isEmpty()) {
            const double envT = static_cast<double>(cursorUs) / 1e6
                                - e->entry.timelineStart;
            clipGain = evaluateVolumeEnvelope(e->entry.volumeEnvelope,
                                              envT, e->entry.volume);
        }
        double gain = qBound(0.0, clipGain, 4.0);
        if (e->entry.audioMuted) gain = 0.0;
        const int trackIdx = e->entry.sourceTrack;
        if (trackIdx >= 0 && trackIdx < m_mixer->m_trackStates.size())
            gain *= m_mixer->m_trackStates[trackIdx].effectiveGain;

        // Edge-attached audio fade. Equal-power (sqrt) curve so A.trailOut^2
        // + B.leadIn^2 == 1 at the midpoint of an overlap crossfade — linear
        // ramps would sum to 0.5 there and produce a 3 dB perceived dip. The
        // ramp uses the cursor at the start of this readData fragment — a
        // 64 KB ring is ~340 ms, so the within-fragment error is well below
        // audible threshold for the typical 0.5-2 s fade window. If sub-
        // buffer accuracy ever matters we can compute the multiplier per-
        // sample inside the copy loop below.
        const double posSec = static_cast<double>(cursorUs) / 1e6;
        const double elapsed = posSec - e->entry.timelineStart;
        const double remaining = e->entry.timelineEnd - posSec;
        const bool leadInFade = (e->entry.leadInType == TransitionType::FadeIn
                                 || isOverlapTransition(e->entry.leadInType))
            && e->entry.leadInDuration > 0.0
            && elapsed >= 0.0 && elapsed < e->entry.leadInDuration;
        const bool trailOutFade = (e->entry.trailOutType == TransitionType::FadeOut
                                   || isOverlapTransition(e->entry.trailOutType))
            && e->entry.trailOutDuration > 0.0
            && remaining >= 0.0 && remaining < e->entry.trailOutDuration;
        if (leadInFade) {
            // Overlap transitions (CrossDissolve / Wipe / Slide) fire both
            // arms simultaneously during the timeline overlap window — A's
            // trailOut ramps down while B's leadIn ramps up. With sqrt curves
            // the squared sum stays at unity through the crossfade, so the
            // perceived loudness is constant.
            const double t = qBound(0.0, elapsed / e->entry.leadInDuration, 1.0);
            gain *= std::sqrt(t);
        } else if (trailOutFade) {
            const double t = qBound(0.0, remaining / e->entry.trailOutDuration, 1.0);
            gain *= std::sqrt(t);
        }
        if (gain <= 0.0) {
            // Drain the ring so this muted entry stays in time with the
            // cursor; otherwise unmuting mid-playback would resume from a
            // stale position.
            // Track prevGain through the muted window so the ramp on unmute
            // starts from 0 instead of the pre-mute gain. Otherwise lowering
            // volume while muted and then unmuting produces a brief blip as
            // the ramp descends from 1.0 to the new gain.
            e->prevGain = 0.0;
            // US-FIX-7 (R6): align muted drain to a frame boundary.
            // maxlen comes from the OS audio callback and is always a
            // kBytesPerFrame multiple (Qt sets bufferSize as a frame multiple
            // and the sink requests in whole frames). liveBytes is frame-aligned
            // as long as ringHead stays aligned. The qMin of two frame-aligned
            // values is frame-aligned, so this site is safe by invariant — the
            // explicit align-down is a defensive belt-and-suspenders guard that
            // costs nothing and keeps the invariant self-maintaining if any
            // upstream ever produces an odd liveBytes.
            const qint64 rawDrop = qMin<qint64>(maxlen, liveBytes(*e));
            const int dropBytes = static_cast<int>(
                (rawDrop / AudioMixer::kBytesPerFrame) * AudioMixer::kBytesPerFrame);
            e->ringHead += dropBytes;
            e->ringStartTlUs += bytesToUs(dropBytes);
            e->atempoSrcFrameCarry = 0.0;
            continue;
        }

        // US-INT-2 Phase B: per-fragment ramp-aware atempo (envvar-gated).
        // Default path (no atempo / identity ramp): copyBytes is bounded by
        // ring availability and maxlen; src points at the ring directly;
        // sourceBytesConsumed == copyBytes. Atempo path: speed-multiplier
        // is sampled from the ramp at fragment start, output samples are
        // staged via nearest-neighbor pick from the ring (no pitch
        // correction — sprint description explicitly permits resample-only
        // for v1), and ringHead advances by speedMul * copyBytes while
        // ringStartTlUs still advances by bytesToUs(copyBytes) so the entry
        // stays in time with the master cursor. Identity-only sequences
        // never populate m_speedRampByKey so this branch is unreachable
        // unless the user has authored a ramp.
        QVarLengthArray<int16_t, 16384> stagedSamples;
        int copyBytes = static_cast<int>(qMin<qint64>(maxlen, liveBytes(*e)));
        int sourceBytesConsumed = copyBytes;
        const int16_t *src = reinterpret_cast<const int16_t *>(liveData(*e));
        const speedramp::SpeedRamp *atempoRamp = nullptr;
        if (audioAtempoEnabledCached()
            && !m_mixer->m_speedRampByKey.isEmpty()) {
            // [P2-M1] R10-b: locking-regression tripwire (Q_ASSERT under
            // mutex invariance). readData holds m_controlMutex via its
            // top-level QMutexLocker for this entire branch, and setSpeedRamps
            // bumps m_speedRampGeneration under the SAME mutex. Therefore the
            // pre-lookup and post-lookup loads of the generation MUST observe
            // identical values; if the assert fires, the single-lock
            // invariant has been broken by a future restructure and the
            // ramp-dependent branch is unsafe to commit. Q_ASSERT compiles
            // out in Release builds so the production cost is zero; Debug
            // builds detect the regression at the first stale-snapshot path.
            const uint64_t genAtEntry =
                m_mixer->m_speedRampGeneration.load(std::memory_order_relaxed);
            const AudioTrackKey atempoKey{
                e->entry.filePath,
                qRound64(e->entry.clipIn * 1000.0),
                e->entry.sourceTrack,
                e->entry.sourceClipIndex
            };
            const auto rampIt = m_mixer->m_speedRampByKey.constFind(atempoKey);
            if (rampIt != m_mixer->m_speedRampByKey.constEnd()) {
                Q_ASSERT(genAtEntry == m_mixer->m_speedRampGeneration.load(
                                         std::memory_order_relaxed)
                         && "R10-b: mutex protects gen invariance — if this "
                            "fires, lock structure regressed");
                atempoRamp = &rampIt.value();
            }
        }
        if (atempoRamp) {
            // Sample instantaneous d(src)/d(timeline) at the fragment start
            // by finite difference of timelineToSourceUs over a 1 ms tick.
            // Compose with the legacy uniform e.entry.speed (mirrors the
            // VideoPlayer entryLocalPositionUs path: ramp input is in
            // scaled-clip-relative timeline space, output is source-us).
            const int64_t localTlUs = cursorUs
                - static_cast<int64_t>(e->entry.timelineStart * 1e6);
            const double uniSpeed = (e->entry.speed > 0.0) ? e->entry.speed : 1.0;
            const qint64 scaledLocalTlUs = static_cast<qint64>(
                qMax<int64_t>(0, localTlUs) * uniSpeed);
            // 1ms tick — short enough to read per-keyframe slope, long
            // enough to dodge integer-us truncation noise.
            constexpr qint64 kSlopeTickUs = 1000;
            const qint64 srcA = atempoRamp->timelineToSourceUs(scaledLocalTlUs);
            const qint64 srcB =
                atempoRamp->timelineToSourceUs(scaledLocalTlUs + kSlopeTickUs);
            const double instSrcPerTl =
                static_cast<double>(srcB - srcA) / kSlopeTickUs;
            const double speedMul = qBound(speedramp::SpeedRamp::kMinSpeed,
                instSrcPerTl * uniSpeed,
                speedramp::SpeedRamp::kMaxSpeed);
            const int liveB = liveBytes(*e);
            // Cap output by what the ring can supply at speedMul.
            const int maxOutputByRing = static_cast<int>(
                static_cast<double>(liveB) / speedMul);
            const int outputBound = qMin<int>(static_cast<int>(maxlen),
                qMax<int>(0, maxOutputByRing));
            const int outputFrames = outputBound / AudioMixer::kBytesPerFrame;
            if (outputFrames <= 0) {
                // Atempo wants more source than the ring holds — flag stalled
                // so the cursor freezes for a few callbacks while the decoder
                // catches up (mirrors the empty-ring stall above).
                entryActiveButStalled = true;
                continue;
            }
            copyBytes = outputFrames * AudioMixer::kBytesPerFrame;
            // M2 fix: phase-coherent fractional source-frame accounting.
            // Without this, int(outputFrames*speedMul) truncates per
            // fragment (e.g. 1.5 × 999 = 1498.5 → 1498), so ringHead lags
            // ringStartTlUs and the loop's first srcFrameIdx in fragment
            // N+1 misaligns with fragment N's last read by the truncated
            // fraction. Tracking the residual phase fixes both:
            //   • wantSrcFrames adds last fragment's leftover phase, so
            //     ringHead advances by floor(phase_in + outFrames*speed)
            //     and (over time) averages exactly outFrames*speed.
            //   • The inner loop offsets srcFrameIdx by phase_in so the
            //     fragment-local walk continues the global continuous
            //     walk without a sample-boundary skip.
            const double phaseIn = e->atempoSrcFrameCarry;
            const double wantSrcFrames =
                static_cast<double>(outputFrames) * speedMul + phaseIn;
            const int takeSrcFrames =
                static_cast<int>(qMax<double>(0.0, wantSrcFrames));
            const int unclampedSrcBytes =
                takeSrcFrames * AudioMixer::kBytesPerFrame;
            sourceBytesConsumed = qMin<int>(liveB,
                qMax<int>(0, unclampedSrcBytes));
            if (sourceBytesConsumed == unclampedSrcBytes) {
                e->atempoSrcFrameCarry = wantSrcFrames
                    - static_cast<double>(takeSrcFrames);
            } else {
                // Ring-underflow clamp tripped — drop carry so the
                // late-drop / re-sync path (cursorUs vs ringStartTlUs)
                // does not see an amplified deficit on the next fragment.
                e->atempoSrcFrameCarry = 0.0;
            }
            const int outputSamples = copyBytes
                / static_cast<int>(sizeof(int16_t));
            stagedSamples.resize(outputSamples);
            const int16_t *ringSrc = reinterpret_cast<const int16_t *>(liveData(*e));
            const int liveFrames = liveB / AudioMixer::kBytesPerFrame;
            for (int frame = 0; frame < outputFrames; ++frame) {
                int srcFrameIdx = static_cast<int>(
                    phaseIn + static_cast<double>(frame) * speedMul);
                if (srcFrameIdx >= liveFrames) srcFrameIdx = liveFrames - 1;
                if (srcFrameIdx < 0) srcFrameIdx = 0;
                for (int ch = 0; ch < AudioMixer::kChannels; ++ch) {
                    stagedSamples[frame * AudioMixer::kChannels + ch] =
                        ringSrc[srcFrameIdx * AudioMixer::kChannels + ch];
                }
            }
            src = stagedSamples.data();
        }
        const int copySamples = copyBytes / static_cast<int>(sizeof(int16_t));

        // Per-track noise reduction (Audition Voice Isolation / Resolve
        // Fairlight noise gate parity, simplified expander). Runs FIRST in
        // the per-track chain so downstream EQ / compressor / reverb
        // amplify the de-noised signal rather than the noise. Stereo-link
        // detector with attack / release envelope; static curve compares
        // the envelope (in dBFS) to a noise floor (auto-estimated as the
        // 5th-percentile of a rolling window OR user-set manual floor) and
        // applies a smooth ramp between full pass-through and reductionDb
        // of attenuation across the user-configured threshold band.
        // Disabled = bit-exact bypass (skip the entire DSP path; do not
        // touch state).
        QVarLengthArray<int16_t, 8192> scratchNr;
        auto nrIt = (trackIdx >= 0)
            ? m_mixer->m_trackNoiseReduction.constFind(trackIdx)
            : m_mixer->m_trackNoiseReduction.constEnd();
        if (nrIt != m_mixer->m_trackNoiseReduction.constEnd()
            && nrIt.value().enabled
            && nrIt.value().reductionDb > 1e-6) {
            const auto &nr = nrIt.value();
            const double fs       = static_cast<double>(AudioMixer::kSampleRateHz);
            const double atkCoef  = std::exp(-1.0 / (nr.attackMs  * fs * 0.001));
            const double relCoef  = std::exp(-1.0 / (nr.releaseMs * fs * 0.001));

            auto stateIt = m_mixer->m_trackNoiseReductionState.find(trackIdx);
            if (stateIt == m_mixer->m_trackNoiseReductionState.end()) {
                stateIt = m_mixer->m_trackNoiseReductionState.insert(
                    trackIdx, AudioMixer::NRState{});
            }
            AudioMixer::NRState &nrState = stateIt.value();
            double env = nrState.env;

            // Determine the active noise floor for this fragment. Auto
            // mode pulls the 5th-percentile of the rolling envelope-dBFS
            // history; manual mode uses the user-set value directly.
            double floorDb = nr.manualFloorDb;
            if (nr.autoFloor && !nrState.recentEnvs.isEmpty()) {
                QVector<double> sorted = nrState.recentEnvs;
                std::sort(sorted.begin(), sorted.end());
                const int idx = qBound(0,
                    static_cast<int>(sorted.size() * 0.05),
                    sorted.size() - 1);
                floorDb = sorted[idx];
            }
            nrState.estimatedFloorDb = floorDb;

            // Threshold sets the band where we transition from full
            // attenuation to full pass-through. We center the smooth
            // ramp around (floorDb + thresholdDb) with a fixed 12 dB
            // wide ramp width (perceived smoothness target — narrower
            // pumps audibly, wider weakens the gating action).
            const double thresholdAbsDb = floorDb + nr.thresholdDb;
            constexpr double kRampWidthDb = 12.0;
            const double rampLo = thresholdAbsDb - 0.5 * kRampWidthDb;
            const double rampHi = thresholdAbsDb + 0.5 * kRampWidthDb;
            const double minGainLin = std::pow(10.0, -nr.reductionDb / 20.0);

            scratchNr.resize(copySamples);
            // Per stereo frame: linked level → envelope → static curve.
            for (int i = 0; i + 1 < copySamples; i += 2) {
                const double xL = static_cast<double>(src[i]);
                const double xR = static_cast<double>(src[i + 1]);
                const double level = std::max(std::abs(xL), std::abs(xR));
                if (level > env) {
                    env = level + (env - level) * atkCoef;
                } else {
                    env = level + (env - level) * relCoef;
                }

                // dBFS reference 32768 = 0 dBFS.
                const double envDb = 20.0
                    * std::log10(std::max(env, 1e-9) / 32768.0);

                double nrGain;
                if (envDb >= rampHi) {
                    nrGain = 1.0;            // signal loud enough — no NR
                } else if (envDb <= rampLo) {
                    nrGain = minGainLin;     // signal at/below floor — full NR
                } else {
                    // Smooth linear ramp in dB domain → exponential in linear.
                    const double t = (envDb - rampLo) / kRampWidthDb;
                    const double curveDb = -nr.reductionDb * (1.0 - t);
                    nrGain = std::pow(10.0, curveDb / 20.0);
                }

                double yL = xL * nrGain;
                double yR = xR * nrGain;
                if (yL > 32767.0) yL = 32767.0;
                else if (yL < -32768.0) yL = -32768.0;
                if (yR > 32767.0) yR = 32767.0;
                else if (yR < -32768.0) yR = -32768.0;
                scratchNr[i]     = static_cast<int16_t>(yL);
                scratchNr[i + 1] = static_cast<int16_t>(yR);
            }
            // Odd-sample tail (mono runt) — pass through unchanged.
            for (int i = copySamples & ~1; i < copySamples; ++i) {
                scratchNr[i] = src[i];
            }
            nrState.env = env;

            // Append the post-fragment envelope-in-dBFS into the rolling
            // window (used next fragment for auto-floor estimation). One
            // entry per fragment keeps the percentile sort cheap; with
            // kAutoFloorWindowSize=256 entries that's ~85 s of history
            // worst case, ~5 s typical.
            const double tailEnvDb = 20.0
                * std::log10(std::max(env, 1e-9) / 32768.0);
            if (nrState.recentEnvs.size() < AudioMixer::kAutoFloorWindowSize) {
                nrState.recentEnvs.append(tailEnvDb);
            } else {
                if (nrState.recentEnvsHead < 0
                    || nrState.recentEnvsHead >= AudioMixer::kAutoFloorWindowSize) {
                    nrState.recentEnvsHead = 0;
                }
                nrState.recentEnvs[nrState.recentEnvsHead] = tailEnvDb;
                nrState.recentEnvsHead =
                    (nrState.recentEnvsHead + 1) % AudioMixer::kAutoFloorWindowSize;
            }
            src = scratchNr.data();
        }

        // Per-track 4-band parametric EQ cascade (Premiere/Audition parity).
        // Runs BEFORE the legacy 3-band path and gain/preamp stages; signal
        // flow: 4-band → 3-band → preamp → gain. When all 4 bands are flat
        // or disabled, src points at the ring buffer unchanged (bit-exact
        // bypass). When any band is active, scratchEq holds the filtered
        // samples and src is rebound to it. Disabled bands skip math
        // entirely AND skip history updates so re-enabling has no transient.
        QVarLengthArray<int16_t, 8192> scratchEq;
        auto coefsIt = (trackIdx >= 0)
            ? m_mixer->m_trackEqCoefs.constFind(trackIdx)
            : m_mixer->m_trackEqCoefs.constEnd();
        if (coefsIt != m_mixer->m_trackEqCoefs.constEnd()) {
            const auto &coefs = coefsIt.value();
            const bool anyActive = coefs[0].active || coefs[1].active
                                || coefs[2].active || coefs[3].active;
            if (anyActive) {
                auto histIt = m_mixer->m_trackEqHist.find(trackIdx);
                if (histIt == m_mixer->m_trackEqHist.end()) {
                    histIt = m_mixer->m_trackEqHist.insert(
                        trackIdx, std::array<double, 16>{});
                }
                auto &hist = histIt.value();
                scratchEq.resize(copySamples);
                // hist layout per band b, channel ch: x1=hist[b*4+ch],
                // x2=hist[b*4+2+ch], y1=hist[b*4+ch] ... actually we use
                // 4 doubles per band (2 channels x x1/x2 OR y1/y2)? We need
                // 4 doubles per band per channel for a direct-form-I biquad
                // (x1, x2, y1, y2). But we only allocated 16 = 4 bands x
                // 2 channels x 2 history samples — encode x1/y1 in pairs by
                // using a transposed direct-form-II (z1, z2 per channel).
                // hist[b*4 + ch*2 + 0] = z1[ch], hist[b*4 + ch*2 + 1] = z2[ch].
                for (int i = 0; i < copySamples; ++i) {
                    const int ch = i & 1;
                    double s = static_cast<double>(src[i]);
                    for (int b = 0; b < 4; ++b) {
                        const auto &c = coefs[b];
                        if (!c.active) continue; // skip math + history
                        const int base = b * 4 + ch * 2;
                        double &z1 = hist[base];
                        double &z2 = hist[base + 1];
                        // Transposed direct-form-II:
                        //   y = b0*x + z1
                        //   z1 = b1*x - a1*y + z2
                        //   z2 = b2*x - a2*y
                        const double y = c.b0 * s + z1;
                        z1 = c.b1 * s - c.a1 * y + z2;
                        z2 = c.b2 * s - c.a2 * y;
                        s = y;
                    }
                    // Soft saturation to s16 range — high gain peak boost on
                    // a band can exceed s16 momentarily. Clamp before re-cast
                    // so the existing 3-band path receives a valid s16.
                    if (s > 32767.0) s = 32767.0;
                    else if (s < -32768.0) s = -32768.0;
                    scratchEq[i] = static_cast<int16_t>(s);
                }
                src = scratchEq.data();
            }
        }

        // Per-track feed-forward compressor / limiter (Audition / Resolve
        // Fairlight parity). Runs AFTER the 4-band EQ stage and BEFORE the
        // 3-band / preamp / gain stages so per-track meters and the master
        // bus see post-compression peaks. Stereo-link detector,
        // transposed envelope follower, Cherny-style soft knee.
        // Disabled or 1:1 ratio = bit-exact bypass (skip envelope + curve).
        QVarLengthArray<int16_t, 8192> scratchComp;
        auto compIt = (trackIdx >= 0)
            ? m_mixer->m_trackComp.constFind(trackIdx)
            : m_mixer->m_trackComp.constEnd();
        if (compIt != m_mixer->m_trackComp.constEnd()
            && compIt.value().enabled
            && compIt.value().ratio > 1.0001) {
            const auto &comp = compIt.value();
            // Limiter mode collapses to 1000:1 with a fixed 0.5 ms attack.
            const bool limiter   = comp.ratio >= 20.0;
            const double effRatio = limiter ? 1000.0 : comp.ratio;
            const double effAttackMs = limiter ? 0.5 : comp.attackMs;

            const double fs       = static_cast<double>(AudioMixer::kSampleRateHz);
            const double atkCoef  = std::exp(-1.0 / (effAttackMs * fs * 0.001));
            const double relCoef  = std::exp(-1.0 / (comp.releaseMs * fs * 0.001));
            const double slope    = 1.0 - 1.0 / effRatio; // dB-per-dB above thresh
            const double makeupLin = std::pow(10.0, comp.makeupDb / 20.0);
            const double thresh   = comp.thresholdDb;
            const double kneeHalf = 0.5 * comp.kneeDb;

            auto stateIt = m_mixer->m_trackCompState.find(trackIdx);
            if (stateIt == m_mixer->m_trackCompState.end()) {
                stateIt = m_mixer->m_trackCompState.insert(
                    trackIdx, AudioMixer::CompressorState{});
            }
            double env = stateIt.value().env;
            double lastGrDb = stateIt.value().currentGrDb;

            scratchComp.resize(copySamples);
            // copySamples is interleaved L/R (stereo), so iterate by frame.
            for (int i = 0; i + 1 < copySamples; i += 2) {
                const double xL = static_cast<double>(src[i]);
                const double xR = static_cast<double>(src[i + 1]);
                const double level = std::max(std::abs(xL), std::abs(xR));
                // Envelope follower: attack on rising, release on falling.
                if (level > env) {
                    env = level + (env - level) * atkCoef;
                } else {
                    env = level + (env - level) * relCoef;
                }

                // Static gain curve in dB. Reference 0 dBFS at 32768.
                const double envDb = 20.0 * std::log10(std::max(env, 1e-9) / 32768.0);
                double grDb = 0.0;
                if (kneeHalf > 1e-6 && envDb > thresh - kneeHalf
                    && envDb < thresh + kneeHalf) {
                    // Soft knee: quadratic spline between (thresh - knee/2, 0)
                    // and (thresh + knee/2, knee/2 * slope).
                    const double x = envDb - (thresh - kneeHalf);
                    grDb = slope * (x * x) / (2.0 * comp.kneeDb);
                } else if (envDb > thresh + kneeHalf) {
                    grDb = slope * (envDb - thresh);
                }
                lastGrDb = grDb;

                const double gainLin = std::pow(10.0, (comp.makeupDb - grDb) / 20.0);
                // Skip the makeup multiply collapse — combine into one mul.
                (void)makeupLin;
                double yL = xL * gainLin;
                double yR = xR * gainLin;
                if (yL > 32767.0) yL = 32767.0;
                else if (yL < -32768.0) yL = -32768.0;
                if (yR > 32767.0) yR = 32767.0;
                else if (yR < -32768.0) yR = -32768.0;
                scratchComp[i]     = static_cast<int16_t>(yL);
                scratchComp[i + 1] = static_cast<int16_t>(yR);
            }
            // Odd-sample tail (mono runt) — extremely unlikely with stereo
            // s16 fragments but guarded for safety.
            for (int i = copySamples & ~1; i < copySamples; ++i) {
                scratchComp[i] = src[i];
            }
            stateIt.value().env = env;
            stateIt.value().currentGrDb = lastGrDb;
            src = scratchComp.data();
        }

        // Per-track reverb (Audition / Fairlight Multitap parity, simplified
        // Schroeder topology). Cascaded AFTER the compressor stage and BEFORE
        // the legacy 3-band / preamp / gain stages so meters and the master
        // bus see the post-reverb signal. Disabled or mixRatio=0 = bit-exact
        // bypass (skip the entire DSP path; do not touch state).
        //
        // Signal flow per channel:
        //   in → preDelay buffer → sum(comb1..4 with damped feedback)
        //      → allpass1 → allpass2 → wet
        //   out = dry * (1 - mix) + wet * mix
        // Width is applied as a stereo cross-feed between the two channel
        // wet outputs (0=mono, 100=fully decorrelated).
        QVarLengthArray<int16_t, 8192> scratchRev;
        auto revIt = (trackIdx >= 0)
            ? m_mixer->m_trackReverb.constFind(trackIdx)
            : m_mixer->m_trackReverb.constEnd();
        if (revIt != m_mixer->m_trackReverb.constEnd()
            && revIt.value().enabled
            && revIt.value().mixRatio > 1e-6) {
            const auto &rev = revIt.value();
            // Comb / allpass delays: Freeverb defaults @ 44.1 kHz, scaled to
            // current sample rate so reverb character is consistent across
            // hardware. Stereo offset (~23 samples) decorrelates L/R combs.
            constexpr int kCombDelays44k1[AudioMixer::kReverbCombCount]    = {1116, 1188, 1277, 1356};
            constexpr int kCombStereoOffset44k1                = 23;
            constexpr int kAllpassDelays44k1[AudioMixer::kReverbAllpassCount] = {556, 441};
            const double srScale = static_cast<double>(AudioMixer::kSampleRateHz) / 44100.0;
            auto scaleDelay = [srScale](int n) {
                return std::max(1, static_cast<int>(std::lround(n * srScale)));
            };

            auto stateIt = m_mixer->m_trackReverbState.find(trackIdx);
            if (stateIt == m_mixer->m_trackReverbState.end()) {
                stateIt = m_mixer->m_trackReverbState.insert(
                    trackIdx, AudioMixer::ReverbState{});
            }
            AudioMixer::ReverbState &st = stateIt.value();
            // Lazy buffer initialization on first activation. Subsequent
            // calls preserve buffer contents so live tweaks don't pop.
            if (!st.initialized) {
                for (int ch = 0; ch < AudioMixer::kChannels; ++ch) {
                    for (int b = 0; b < AudioMixer::kReverbCombCount; ++b) {
                        const int sz = scaleDelay(kCombDelays44k1[b]
                            + (ch == 1 ? kCombStereoOffset44k1 : 0));
                        st.comb[ch][b].assign(sz, 0.0f);
                        st.combIdx[ch][b] = 0;
                        st.combLP[ch][b]  = 0.0f;
                    }
                    for (int a = 0; a < AudioMixer::kReverbAllpassCount; ++a) {
                        const int sz = scaleDelay(kAllpassDelays44k1[a]
                            + (ch == 1 ? kCombStereoOffset44k1 / 2 : 0));
                        st.ap[ch][a].assign(sz, 0.0f);
                        st.apIdx[ch][a] = 0;
                    }
                    std::fill(st.preDelay[ch].begin(),
                              st.preDelay[ch].end(), 0.0f);
                    st.preDelayIdx[ch] = 0;
                }
                st.initialized = true;
            }

            // Pre-delay length in samples (clamped to buffer size).
            const int preDelaySamples = std::clamp(
                static_cast<int>(std::lround(rev.preDelayMs * AudioMixer::kSampleRateHz / 1000.0)),
                0, AudioMixer::kReverbPreDelayMaxSamples - 1);
            // Comb feedback gain: g = 0.7 * decay / standardDecay (1.0 sec).
            // Clamped to <0.98 for unconditional stability across the comb
            // bank (each comb's pole magnitude must stay inside the unit
            // circle even with HF damping subtracting energy).
            const float fbGain = static_cast<float>(
                std::clamp(0.7 * rev.decaySeconds / 1.0, 0.0, 0.97));
            // HF damping coefficient: 0 = no LP (bright tail), ~1 = heavy
            // LP (dark tail). Implemented as one-pole LP inside the comb
            // feedback path: y = (1-d)*x + d*y_prev.
            const float damp = static_cast<float>(
                std::clamp(rev.dampingHF / 100.0, 0.0, 0.95));
            const float dampInv = 1.0f - damp;
            constexpr float kAllpassGain = 0.5f;
            const float mix = static_cast<float>(std::clamp(rev.mixRatio, 0.0, 1.0));
            const float dry = 1.0f - mix;
            // Width: 0 = mono sum (collapse), 100 = pass-through.
            // We blend each channel's wet with the cross-channel wet.
            const float width = static_cast<float>(
                std::clamp(rev.widthPercent / 100.0, 0.0, 1.0));
            const float crossFeed = 0.5f * (1.0f - width); // 0 at width=100, 0.5 at width=0
            const float selfFeed  = 1.0f - crossFeed;

            scratchRev.resize(copySamples);
            // Process per stereo frame so we can apply width cross-feed.
            for (int i = 0; i + 1 < copySamples; i += 2) {
                float wet[AudioMixer::kChannels];
                for (int ch = 0; ch < AudioMixer::kChannels; ++ch) {
                    const float xin = static_cast<float>(src[i + ch]);
                    // Pre-delay: write input, read N samples back.
                    auto &pd = st.preDelay[ch];
                    int &pdIdx = st.preDelayIdx[ch];
                    pd[pdIdx] = xin;
                    int readIdx = pdIdx - preDelaySamples;
                    if (readIdx < 0) readIdx += AudioMixer::kReverbPreDelayMaxSamples;
                    const float xPre = pd[readIdx];
                    pdIdx = (pdIdx + 1) % AudioMixer::kReverbPreDelayMaxSamples;

                    // Sum 4 parallel comb filters with damped feedback.
                    float combSum = 0.0f;
                    for (int b = 0; b < AudioMixer::kReverbCombCount; ++b) {
                        auto &buf = st.comb[ch][b];
                        const int sz = buf.size();
                        if (sz <= 0) continue;
                        int &idx = st.combIdx[ch][b];
                        const float yDelayed = buf[idx];
                        // One-pole LP on the feedback tap.
                        float &lp = st.combLP[ch][b];
                        lp = dampInv * yDelayed + damp * lp;
                        buf[idx] = xPre + lp * fbGain;
                        idx = (idx + 1) % sz;
                        combSum += yDelayed;
                    }
                    // Normalise comb output (4 parallel paths).
                    combSum *= 0.25f;

                    // Series allpass filters (decorrelation).
                    float ap = combSum;
                    for (int a = 0; a < AudioMixer::kReverbAllpassCount; ++a) {
                        auto &buf = st.ap[ch][a];
                        const int sz = buf.size();
                        if (sz <= 0) continue;
                        int &idx = st.apIdx[ch][a];
                        const float bufIn = ap + buf[idx] * kAllpassGain;
                        const float yOut  = buf[idx] - bufIn * kAllpassGain;
                        buf[idx] = bufIn;
                        idx = (idx + 1) % sz;
                        ap = yOut;
                    }
                    wet[ch] = ap;
                }

                // Stereo width cross-feed: collapse toward mono as width→0.
                const float wetL = selfFeed * wet[0] + crossFeed * wet[1];
                const float wetR = selfFeed * wet[1] + crossFeed * wet[0];

                float yL = dry * static_cast<float>(src[i])     + mix * wetL;
                float yR = dry * static_cast<float>(src[i + 1]) + mix * wetR;
                if (yL > 32767.0f) yL = 32767.0f;
                else if (yL < -32768.0f) yL = -32768.0f;
                if (yR > 32767.0f) yR = 32767.0f;
                else if (yR < -32768.0f) yR = -32768.0f;
                scratchRev[i]     = static_cast<int16_t>(yL);
                scratchRev[i + 1] = static_cast<int16_t>(yR);
            }
            // Odd-sample tail (mono runt) — pass through unchanged.
            for (int i = copySamples & ~1; i < copySamples; ++i) {
                scratchRev[i] = src[i];
            }
            src = scratchRev.data();
        }

        // Per-sample 5ms gain ramp at fragment seam to suppress click at entry
        // transitions (volume / fade / mute step). Ramps from prevGain → gain over
        // the first qMin(copySamples, 240) samples (240 = 5ms @ 48kHz stereo, the
        // shortest perceptually de-clicking length). Inside an entry the ramp is
        // inert (prevGain == gain), so cost is 1 div + 240 fmadds per fragment.
        // prevGain is intentionally NOT reset on seek/setSequence — the click is
        // between two fragments of the OS sink callback, not within the entry's
        // data lifecycle.
        const int rampLen = qMin(copySamples, 240);
        const double gainStep = (rampLen > 0) ? (gain - e->prevGain) / static_cast<double>(rampLen) : 0.0;

        // Per-track realtime 3-band biquad EQ (applied before effectiveGain so
        // level meters see post-EQ values). The EQ cascade uses transposed
        // direct-form-II biquads with zero-allocation inline processing.
        auto *ts = (trackIdx >= 0 && trackIdx < m_mixer->m_trackStates.size())
            ? &m_mixer->m_trackStates[trackIdx] : nullptr;
        const bool eqActive = ts && ts->eqEnabled;

        if (eqActive) {
            const double preampLin = std::pow(10.0, ts->eq.preamp / 20.0);
            // Ensure per-track level accum vector is sized
            if (trackIdx >= 0 && m_mixer->m_trackLevelAccum.size() <= trackIdx)
                m_mixer->m_trackLevelAccum.resize(trackIdx + 1);
            auto *tla = (trackIdx >= 0) ? &m_mixer->m_trackLevelAccum[trackIdx] : nullptr;
            for (int i = 0; i < rampLen; ++i) {
                const double g = e->prevGain + gainStep * static_cast<double>(i);
                double s = static_cast<double>(src[i]);
                const int ch = i & 1;
                for (int b = 0; b < 3; ++b) {
                    const auto &c = ts->eqCoeffs[b];
                    auto &z = ts->z[b];
                    double w = s - c.a1 * z[ch] - c.a2 * z[ch + 2];
                    s = c.b0 * w + c.b1 * z[ch] + c.b2 * z[ch + 2];
                    z[ch + 2] = z[ch];
                    z[ch] = w;
                }
                s *= preampLin;
                const double sampleVal = s * g;
                if (tla) {
                    const double norm = sampleVal / 32768.0;
                    const double absNorm = std::abs(norm);
                    if (ch == 0) {
                        if (absNorm > tla->peakL) tla->peakL = static_cast<float>(absNorm);
                        tla->sumSqL += norm * norm;
                        tla->chanCount++;
                    } else {
                        if (absNorm > tla->peakR) tla->peakR = static_cast<float>(absNorm);
                        tla->sumSqR += norm * norm;
                    }
                }
                accum[i] += static_cast<int32_t>(sampleVal);
            }
            for (int i = rampLen; i < copySamples; ++i) {
                double s = static_cast<double>(src[i]);
                const int ch = i & 1;
                for (int b = 0; b < 3; ++b) {
                    const auto &c = ts->eqCoeffs[b];
                    auto &z = ts->z[b];
                    double w = s - c.a1 * z[ch] - c.a2 * z[ch + 2];
                    s = c.b0 * w + c.b1 * z[ch] + c.b2 * z[ch + 2];
                    z[ch + 2] = z[ch];
                    z[ch] = w;
                }
                s *= preampLin;
                const double sampleVal = s * gain;
                if (tla) {
                    const double norm = sampleVal / 32768.0;
                    const double absNorm = std::abs(norm);
                    if (ch == 0) {
                        if (absNorm > tla->peakL) tla->peakL = static_cast<float>(absNorm);
                        tla->sumSqL += norm * norm;
                        tla->chanCount++;
                    } else {
                        if (absNorm > tla->peakR) tla->peakR = static_cast<float>(absNorm);
                        tla->sumSqR += norm * norm;
                    }
                }
                accum[i] += static_cast<int32_t>(sampleVal);
            }
        } else {
            // Ensure per-track level accum vector is sized
            if (trackIdx >= 0 && m_mixer->m_trackLevelAccum.size() <= trackIdx)
                m_mixer->m_trackLevelAccum.resize(trackIdx + 1);
            auto *tla = (trackIdx >= 0) ? &m_mixer->m_trackLevelAccum[trackIdx] : nullptr;
            for (int i = 0; i < rampLen; ++i) {
                const double g = e->prevGain + gainStep * static_cast<double>(i);
                const double sampleVal = static_cast<double>(src[i]) * g;
                if (tla) {
                    const double norm = sampleVal / 32768.0;
                    const double absNorm = std::abs(norm);
                    const int ch = i & 1;
                    if (ch == 0) {
                        if (absNorm > tla->peakL) tla->peakL = static_cast<float>(absNorm);
                        tla->sumSqL += norm * norm;
                        tla->chanCount++;
                    } else {
                        if (absNorm > tla->peakR) tla->peakR = static_cast<float>(absNorm);
                        tla->sumSqR += norm * norm;
                    }
                }
                accum[i] += static_cast<int32_t>(sampleVal);
            }
            for (int i = rampLen; i < copySamples; ++i) {
                const double sampleVal = static_cast<double>(src[i]) * gain;
                if (tla) {
                    const double norm = sampleVal / 32768.0;
                    const double absNorm = std::abs(norm);
                    const int ch = i & 1;
                    if (ch == 0) {
                        if (absNorm > tla->peakL) tla->peakL = static_cast<float>(absNorm);
                        tla->sumSqL += norm * norm;
                        tla->chanCount++;
                    } else {
                        if (absNorm > tla->peakR) tla->peakR = static_cast<float>(absNorm);
                        tla->sumSqR += norm * norm;
                    }
                }
                accum[i] += static_cast<int32_t>(sampleVal);
            }
        }
        e->prevGain = gain;
        // Phase B: under atempo, ringHead advances by SOURCE bytes consumed
        // (= speedMul * copyBytes) while ringStartTlUs advances by TIMELINE
        // us (= bytesToUs(copyBytes)) — the entry stays in time with the
        // master cursor while consuming the ring at variable rate. Default
        // (atempo off / identity ramp) keeps sourceBytesConsumed == copyBytes
        // so the existing path is unchanged.
        e->ringHead += sourceBytesConsumed;
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

    // Master loudness normalizer (FCP-style). Applies after sum-mix, before
    // s16 clamp. amount=0 bypasses entirely; amount=1 fully follows the
    // running RMS toward kTargetLevel. uniformity controls smoothing time.
    {
        const double amount = m_mixer->m_normalizerAmount.load(std::memory_order_acquire);
        if (anyMixed && amount > 0.001) {
            const double uniformity =
                m_mixer->m_normalizerUniformity.load(std::memory_order_acquire);

            double blockSumSq = 0.0;
            for (int i = 0; i < sampleCount; ++i) {
                const double s = accum[i] / 32768.0;
                blockSumSq += s * s;
            }
            const double blockMeanSq = blockSumSq / sampleCount;

            constexpr double kRmsWindowMs = 100.0;
            const double blockMs = static_cast<double>(frameCount) * 1000.0
                                   / AudioMixer::kSampleRateHz;
            const double rmsAlpha = qBound(0.001, blockMs / kRmsWindowMs, 1.0);
            m_mixer->m_normalizerRmsMeanSq =
                (1.0 - rmsAlpha) * m_mixer->m_normalizerRmsMeanSq
                + rmsAlpha * blockMeanSq;

            const double currentRms = std::sqrt(m_mixer->m_normalizerRmsMeanSq);

            // Target = -16 dBFS, accept range -32 dBFS..0 dBFS clamp.
            constexpr double kTargetLevel = 0.158;
            constexpr double kRmsFloor    = 0.001;
            constexpr double kMaxBoost    = 4.0;   // +12 dB
            constexpr double kMinCut      = 0.25;  // -12 dB
            double rawGain = 1.0;
            if (currentRms > kRmsFloor) {
                rawGain = qBound(kMinCut, kTargetLevel / currentRms, kMaxBoost);
            }

            const double targetGain = (1.0 - amount) + amount * rawGain;

            // uniformity=0 → tau=2000 ms (gradual, preserves dynamics)
            // uniformity=1 → tau=  50 ms (fast, more uniform)
            const double tauMs = 2000.0 - 1950.0 * uniformity;
            const double tauSamples = tauMs * 0.001 * AudioMixer::kSampleRateHz;
            const double smoothAlpha = (tauSamples > 1.0)
                ? qBound(0.001,
                         1.0 - std::exp(-static_cast<double>(frameCount) / tauSamples),
                         1.0)
                : 1.0;
            m_mixer->m_normalizerSmoothedGain +=
                (targetGain - m_mixer->m_normalizerSmoothedGain) * smoothAlpha;

            const double appliedGain = m_mixer->m_normalizerSmoothedGain;
            if (std::abs(appliedGain - 1.0) > 0.001) {
                for (int i = 0; i < sampleCount; ++i) {
                    accum[i] = static_cast<int32_t>(accum[i] * appliedGain);
                }
            }
        } else if (amount <= 0.001) {
            // Reset state so re-enabling starts at unity and ramps cleanly.
            m_mixer->m_normalizerSmoothedGain = 1.0;
            m_mixer->m_normalizerRmsMeanSq = 0.0;
        }
    }

    // Master-bus compressor + brick-wall limiter. Stereo-linked envelope
    // (max of |L|, |R|) drives a standard analog-style feed-forward
    // compressor. When disabled the block is a bit-exact bypass — the
    // accumulator is untouched.
    {
        const AudioMixer::CompressorParams compressorParams = m_mixer->m_compressorParams;
        const bool compEnabled = compressorParams.enabled;
        if (anyMixed && compEnabled) {
            const double threshLin = std::pow(10.0, compressorParams.thresholdDb / 20.0);
            const double ratio = compressorParams.ratio;
            const double attackMs = compressorParams.attackMs;
            const double releaseMs = compressorParams.releaseMs;
            const double makeup = std::pow(10.0, compressorParams.makeupDb / 20.0);

            const double fs = static_cast<double>(AudioMixer::kSampleRateHz);
            const double alphaA = std::exp(-1.0 / (attackMs * fs * 0.001));
            const double alphaR = std::exp(-1.0 / (releaseMs * fs * 0.001));
            const double oneOverRMinusOne = 1.0 / ratio - 1.0;
            constexpr double kLimiterCeil = 0.99;

            double env = m_mixer->m_compressorEnv;

            for (int i = 0; i < sampleCount; i += 2) {
                const double absL = std::abs(static_cast<double>(accum[i]) / 32768.0);
                const double absR = std::abs(static_cast<double>(accum[i + 1]) / 32768.0);
                const double xAbs = (absL >= absR) ? absL : absR;

                if (env >= xAbs)
                    env = alphaR * env + (1.0 - alphaR) * xAbs;
                else
                    env = alphaA * env + (1.0 - alphaA) * xAbs;

                double cGain;
                if (env > threshLin)
                    cGain = std::pow(env / threshLin, oneOverRMinusOne) * makeup;
                else
                    cGain = makeup;

                accum[i]     = static_cast<int32_t>(static_cast<double>(accum[i]) * cGain);
                accum[i + 1] = static_cast<int32_t>(static_cast<double>(accum[i + 1]) * cGain);

                // Brick-wall limiter: clamp absolute output to 0.99 * 32768
                constexpr double kCeilVal = kLimiterCeil * 32768.0;
                if (accum[i] > static_cast<int32_t>(kCeilVal))
                    accum[i] = static_cast<int32_t>(kCeilVal);
                else if (accum[i] < -static_cast<int32_t>(kCeilVal))
                    accum[i] = -static_cast<int32_t>(kCeilVal);
                if (accum[i + 1] > static_cast<int32_t>(kCeilVal))
                    accum[i + 1] = static_cast<int32_t>(kCeilVal);
                else if (accum[i + 1] < -static_cast<int32_t>(kCeilVal))
                    accum[i + 1] = -static_cast<int32_t>(kCeilVal);
            }
            m_mixer->m_compressorEnv = env;
        } else if (!compEnabled) {
            // Reset envelope so re-enabling starts from silence, not a stale
            // value that could cause a gain step.
            m_mixer->m_compressorEnv = 0.0;
        }
    }

    if (anyMixed) {
        int16_t *dst = reinterpret_cast<int16_t *>(data);
        auto &mla = m_mixer->m_masterLevelAccum;
        for (int i = 0; i < sampleCount; ++i) {
            const double norm = static_cast<double>(accum[i]) / 32768.0;
            const double absNorm = std::abs(norm);
            if ((i & 1) == 0) {
                if (absNorm > mla.peakL) mla.peakL = static_cast<float>(absNorm);
                mla.sumSqL += norm * norm;
                mla.chanCount++;
            } else {
                if (absNorm > mla.peakR) mla.peakR = static_cast<float>(absNorm);
                mla.sumSqR += norm * norm;
            }
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

    // US-VFF-009: publish audible lag AFTER cursor advance.
    // bufferedUs reflects the post-fill state (samples we just queued plus
    // anything left over). audibleClockUs() = cursor - lag now equals
    // (cursor_pre + deltaUs) - bufferedUs_post, which on cold start equals
    // cursor_pre exactly (the position currently audible — i.e. nothing yet,
    // since we just queued our first chunk). Steady state is unchanged
    // because bufferedUs hovers at ~bufferSize and cursor advances at the
    // sample-write rate. This eliminates the cold-start +50–170 ms jump
    // that previously triggered correctVideoDriftAgainstAudioClock's
    // skip-decode loop on every play() (gap-independent fast-forward
    // observed even on single-clip playback after US-VFF-008 revert).
    if (m_mixer->m_sink) {
        const qint64 buffered = m_mixer->m_sink->bufferSize() - m_mixer->m_sink->bytesFree();
        const int64_t lagUs = (buffered > 0)
            ? buffered * 1'000'000LL
                  / (static_cast<int64_t>(AudioMixer::kBytesPerFrame) * AudioMixer::kSampleRateHz)
            : 0;
        m_mixer->m_audibleLagUs.store(lagUs, std::memory_order_release);
    }

    // Throttled level-meter emission (<=30 Hz). Snapshot+reset accumulators
    // under m_controlMutex, then emit queued (unlocked) to the GUI thread.
    {
        const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowNs - m_mixer->m_lastLevelEmitNs >= 33'000'000LL) {
            QVector<AudioMixer::LevelAccum> trackSnapshots;
            trackSnapshots.reserve(m_mixer->m_trackLevelAccum.size());
            for (auto &la : m_mixer->m_trackLevelAccum) {
                trackSnapshots.append(la);
                la.reset();
            }
            AudioMixer::LevelAccum masterSnapshot = m_mixer->m_masterLevelAccum;
            m_mixer->m_masterLevelAccum.reset();
            m_mixer->m_lastLevelEmitNs = nowNs;

            lock.unlock();

            for (int ti = 0; ti < trackSnapshots.size(); ++ti) {
                const auto &la = trackSnapshots[ti];
                if (la.chanCount <= 0) continue;
                const double invCount = 1.0 / static_cast<double>(la.chanCount);
                const float rmsL = static_cast<float>(std::sqrt(la.sumSqL * invCount));
                const float rmsR = static_cast<float>(std::sqrt(la.sumSqR * invCount));
                QMetaObject::invokeMethod(m_mixer, "levelChanged", Qt::QueuedConnection,
                    Q_ARG(int, ti), Q_ARG(float, la.peakL), Q_ARG(float, la.peakR),
                    Q_ARG(float, rmsL), Q_ARG(float, rmsR));
            }
            if (masterSnapshot.chanCount > 0) {
                const double invCount = 1.0 / static_cast<double>(masterSnapshot.chanCount);
                const float rmsL = static_cast<float>(std::sqrt(masterSnapshot.sumSqL * invCount));
                const float rmsR = static_cast<float>(std::sqrt(masterSnapshot.sumSqR * invCount));
                QMetaObject::invokeMethod(m_mixer, "masterLevelChanged", Qt::QueuedConnection,
                    Q_ARG(float, masterSnapshot.peakL), Q_ARG(float, masterSnapshot.peakR),
                    Q_ARG(float, rmsL), Q_ARG(float, rmsR));
            }
            return maxlen;
        }
    }

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

        // US-INT-2 Phase B: capture the input-vector order of AudioTrackKeys
        // so a subsequent setSpeedRamps call (parallel-array contract from
        // MainWindow) can rebuild m_speedRampByKey in lockstep. Cleared each
        // call — stale keys from a previous sequence would silently mismap.
        m_speedRampKeyOrder.clear();
        m_speedRampKeyOrder.reserve(entries.size());

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
            // [P2-M2] Append AFTER the dup-skip continue so m_speedRampKeyOrder
            // never contains duplicates. This guarantees that R10-a's
            // `m_entries.constFind(k)` lookup on a removed-key entry can never
            // fail simply because a duplicate-skipped key consumed the surviving
            // entry's slot in m_speedRampByKey. The order array now mirrors
            // m_entries's key set 1:1 (in input order minus duplicates), which
            // is the invariant setSpeedRamps's parallel-array rebuild relies on.
            m_speedRampKeyOrder.append(key);

            AudioDecoderEntry *de = m_entries.take(key);
            if (de) {
                // Same key reappears — update mutable timeline metadata only.
                const auto oldStart = de->entry.timelineStart;
                const auto oldEnd = de->entry.timelineEnd;
                de->entry = e;
                if (!qFuzzyCompare(oldStart, e.timelineStart)
                    || !qFuzzyCompare(oldEnd, e.timelineEnd)) {
                    // [P2-M3] Use the shared resetAtempoState helper for the
                    // pre-seek atempo field invalidation (atempoSrcFrameCarry,
                    // seekPending) so this site and R10-a's removed-key loop
                    // are structurally guaranteed to invalidate the same field
                    // set. ring/ringHead are reset here as a setSequence-only
                    // optimisation (avoid serving stale samples from the prior
                    // timeline window); seekEntryToTimeline would reset them on
                    // the next refill regardless.
                    resetAtempoState(de);
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
        if (m_trackLevelAccum.size() < maxTrack + 1)
            m_trackLevelAccum.resize(maxTrack + 1);

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

// US-INT-2 Phase B: store per-entry ramps AND rebuild the (AudioTrackKey →
// SpeedRamp) hash for O(1) lookup inside the audio worker's per-fragment
// loop. The QVector continues to be the storage of record so the public
// API stays Phase-A compatible; the hash is a pure side-channel rebuilt
// here under m_controlMutex. Identity-only sequences leave m_speedRampByKey
// empty (entries with no hash hit fall through to the legacy resample-only
// path → bit-exact unchanged when atempo is off OR all ramps are identity).
void AudioMixer::setSpeedRamps(const QVector<speedramp::SpeedRamp> &ramps)
{
    int hashedCount = 0;
    int rearmed = 0;
    int rearmedRemoved = 0;
    uint64_t newGeneration = 0;
    {
        QMutexLocker lock(&m_controlMutex);
        m_speedRamps = ramps;
        // PRD-B / R10 (a): symmetric ramp-REMOVED inverse closure.
        // Snapshot the OLD key set BEFORE we clear so we can compute the
        // symmetric difference after rebuild. Without this snapshot, an entry
        // whose ramp was REMOVED in the new sequence (was in the OLD map,
        // absent in the NEW map) would NOT be matched by the existing
        // ramp-ADDED re-arm loop below (`m_speedRampByKey.contains(it.key())`
        // checks the NEW map only), so any cached atempo decision the entry
        // accumulated under the old map (atempoSrcFrameCarry, R8 gate
        // flags-OFF anchor) would persist into the new identity-only
        // sequence and de-sync the entry. Symmetric to the ramp-ADDED arm
        // already implemented below.
        // [P2-MINOR-2] QSet range constructor (Qt 5.14+). Qt6 baseline >> 5.14
        // so no fallback path is needed. Eliminates the manual loop and
        // matches the construction used for `removedKeys` below.
        QSet<AudioTrackKey> oldRampKeys(m_speedRampByKey.keyBegin(),
                                       m_speedRampByKey.keyEnd());
        m_speedRampByKey.clear();
        const int n = qMin(m_speedRamps.size(), m_speedRampKeyOrder.size());
        m_speedRampByKey.reserve(n);
        for (int i = 0; i < n; ++i) {
            // Skip identity ramps so the hot-path lookup short-circuits on
            // "key not found" instead of having to ask isIdentity() per
            // fragment.
            if (m_speedRamps[i].isIdentity()) continue;
            m_speedRampByKey.insert(m_speedRampKeyOrder[i], m_speedRamps[i]);
            ++hashedCount;
        }
        // PRD-B / R10 (b): bump the generation counter NOW — while still
        // holding m_controlMutex and AFTER m_speedRampByKey is fully
        // rebuilt. Any subsequent reader (readData atempo path or the
        // seekEntryToTimeline R8 gate) that captures the generation under
        // the same mutex will see the fresh map plus the fresh generation
        // atomically. A reader that captured the OLD generation BEFORE
        // this lock-region runs will, on its next under-lock compare,
        // observe a mismatch and silently drop the ramp-dependent branch
        // (defense-in-depth against an un-emittable past stale callback).
        // Relaxed memory order is sufficient because every consumer-side
        // compare we add below happens under m_controlMutex, which already
        // provides the necessary happens-before edge.
        //
        // [P2-MINOR-1] The std::atomic is operated under m_controlMutex
        // throughout — it is atomic so the readData lock-free first read (the
        // pre-lookup load before the QMutexLocker takes effect, in some
        // future lock restructure) is well-defined, NOT because the field is
        // contended. We retain fetch_add(...) + 1 (instead of load+store)
        // because atomic-operation semantic consistency across the
        // (bump-under-lock, load-under-lock, defense-in-depth-load-lock-free)
        // call sites matters more than the trivial micro-saving from a plain
        // store.
        newGeneration =
            m_speedRampGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
        // US-FIX-7 (R9, Q2): close the setSequence-unlock → setSpeedRamps-lock
        // ordering window. setSequence releases m_controlMutex then wakes the
        // decoder; the woken decode thread can run seekEntryToTimeline for a
        // seekPending entry BEFORE this call has rebuilt m_speedRampByKey, so
        // the R8 gate there can observe the PREVIOUS sequence's (stale) ramp
        // map and wrongly re-enable the fast-drain for a uniform-speed==1.0
        // clip that actually carries an authored non-identity SpeedRamp. With
        // the fresh map now installed (still under this lock), force exactly
        // one corrective re-seek for every ramped entry that has NOT already
        // queued a seek: seekEntryToTimeline fully resets ring/head/carry/flags
        // so any stale-map anchor is overwritten. Predicate "ramped AND not
        // already seekPending": a non-ramped entry's gate decision depends only
        // on the race-immune r8NonUnitSpeed, so re-seeking it mid-playback on
        // every edit would be an audible disruption with no correctness
        // benefit; an already-seekPending entry has not consumed the stale map
        // yet and will gate correctly on its first seek, so re-arming it would
        // only risk a redundant double-seek. Bounded: ≤1 extra seek per ramped
        // entry per call — the bound is guaranteed by the `!de->seekPending`
        // guard in the loop below, NOT by seekEntryToTimeline clearing
        // seekPending before the next setSpeedRamps: an entry this call just
        // re-armed has seekPending=true, so a subsequent setSpeedRamps skips it
        // via that guard even when its seekEntryToTimeline has not yet run (the
        // seek is deferred until the playback cursor approaches an out-of-window
        // entry, so seekPending is not necessarily cleared before the next
        // setSpeedRamps — the bound does not rely on that clear).
        //
        // No-op / byte-identical guarantee — the precondition is "no authored
        // non-identity ramp", NOT "atempo OFF" (the map is built regardless of
        // atempo; atempo only gates ramp CONSUMPTION in readData and the R8
        // ramp-branch). The user's reported default editor-preview path (no
        // ramp at all, speed==1.0) has an empty m_speedRampByKey → contains()
        // is always false → this loop is a pure no-op, rearmed stays 0, no
        // extra wake() → byte-identical to R8/R7. With an authored non-identity
        // ramp present (atempo OFF *or* ON) the map is populated and this loop
        // MAY re-arm + wake; on an atempo-OFF/1x clip that extra re-seek is
        // benign (seekEntryToTimeline fully resets and the R8 gate, atempo OFF,
        // still lands flags-ON for a 1x clip — a harmless redundant re-seek,
        // never a sandstorm), and on the opt-in atempo path it is the intended
        // corrective re-seek.
        //
        // SCOPE (PRD-B / R10 — full closure of the previously-documented residuals).
        // R9 closed only the ramp-ADDED stale-map direction. R10 below closes
        // both remaining holes:
        //
        //   (R10-a) The inverse ramp-REMOVED transition — an entry ramped in
        //   the OLD map but identity in the NEW one. The existing ramp-ADDED
        //   re-arm loop's contains() check is over the NEW map only, so a
        //   removed-key entry's stale R8 gate anchor (e.g. flags-OFF set when
        //   the OLD map matched) would persist into the NEW identity-only
        //   sequence. Now closed by walking the symmetric difference
        //   `oldRampKeys - new map keys` and forcing the corresponding entries'
        //   atempo cache to invalidate (clear atempoSrcFrameCarry, set
        //   seekPending so the next refill re-runs seekEntryToTimeline whose
        //   R8 gate now consults the FRESH map → flags-ON for a 1x clip).
        //
        //   (R10-b) A stale-map decision already in-flight before this call
        //   acquired the lock — cannot be literally un-emitted, but the
        //   generation counter bumped above lets the consumer SILENTLY DROP
        //   the ramp-dependent branch on its next under-lock compare. The
        //   defense-in-depth check lives in readData's atempo branch and the
        //   R8 gate inside seekEntryToTimeline (both consult m_speedRampByKey
        //   and so are now also generation-gated).
        //
        // Both closures stay strictly confined to the opt-in atempo / non-1x
        // path: the default editor preview (atempo OFF, speed==1.0, no ramp)
        // has `oldRampKeys` empty AND m_speedRampByKey empty → removedKeys is
        // empty → R10-a loop is a pure no-op. The R10-b generation check
        // lives on the same lock-protected critical sections, only inside
        // branches already gated by audioAtempoEnabledCached() and a non-empty
        // map, so byte-identity to R7/R8/R9 on the default path is preserved
        // by construction.
        for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
            AudioDecoderEntry *de = it.value();
            if (de && !de->seekPending && m_speedRampByKey.contains(it.key())) {
                de->seekPending = true;
                ++rearmed;
            }
        }
        // R10-a: symmetric ramp-REMOVED inverse closure. For each key in the
        // OLD ramp map that is NOT present in the NEW map, force an atempo
        // cache invalidation on the corresponding decoder entry: clear
        // atempoSrcFrameCarry (the phase residual is meaningless once the
        // ramp is gone) and set seekPending so the next refill re-runs
        // seekEntryToTimeline. The R8 gate inside seekEntryToTimeline reads
        // the FRESH map, so a previously-ramped-now-identity entry lands on
        // the R8 flags-ON branch (1x default path) instead of carrying stale
        // flags-OFF.
        const auto removedKeys = oldRampKeys - QSet<AudioTrackKey>(
            m_speedRampByKey.keyBegin(), m_speedRampByKey.keyEnd());
        for (const auto &k : removedKeys) {
            // [P2-M2] entry not found via removedKeys can occur if input
            // contained a duplicate AudioTrackKey that was skipped at the
            // dup-guard above — the surviving entry (already invalidated by
            // the ramp-ADDED loop if applicable) is unaffected. With the
            // P2-M2 fix, m_speedRampKeyOrder no longer contains duplicates so
            // a removed key whose entry truly exists will always be found
            // here; this constFind is the residual safety net for the
            // race-window where a previous setSequence call deleted the
            // entry between sequences.
            auto entryIt = m_entries.constFind(k);
            if (entryIt == m_entries.constEnd()) continue;
            AudioDecoderEntry *de = entryIt.value();
            if (!de) continue;
            // [P2-M3] resetAtempoState is the single helper for the pre-seek
            // atempo invalidation; the matching reset path inside the
            // setSequence retained-entry branch uses the same helper so the
            // two sites cannot drift. The helper flips seekPending=true, so
            // capture the prior state for the rearmedRemoved metric BEFORE
            // calling.
            const bool wasSeekPending = de->seekPending;
            resetAtempoState(de);
            if (!wasSeekPending) ++rearmedRemoved;
        }
    }
    // Lock released. Mirror setSequence's post-unlock wake (wake() takes only
    // m_wakeMutex, never m_controlMutex) so the re-armed seeks are serviced
    // promptly. On the default path rearmed==0 AND rearmedRemoved==0 so this
    // is skipped entirely — byte-identical preview path preserved.
    if ((rearmed > 0 || rearmedRemoved > 0) && m_decodeRunner)
        m_decodeRunner->wake();
    qInfo() << "AudioMixer::setSpeedRamps count=" << ramps.size()
            << "non-identity-hashed=" << hashedCount
            << "rearmedSeek=" << rearmed
            << "rearmedRemoved=" << rearmedRemoved
            << "generation=" << newGeneration
            << "atempoEnvvar=" << audioAtempoEnabledCached();
}

void AudioMixer::seekTo(int64_t timelineUs) {
    qInfo() << "AudioMixer::seekTo us=" << timelineUs
            << "playing=" << m_playing.load(std::memory_order_acquire);
    // US-VFF-002: Defensive clamp against out-of-bounds timelineUs.
    // v2diag log: AudioMixer::seekTo us=10628155000 (total=7540s) leaked from
    // corrupted m_timelinePositionUs. Clamp prevents bogus values from
    // corrupting m_writeCursorUs and propagating to the audible clock.
    {
        int64_t totalDurationUs = 0;
        {
            QMutexLocker lock(&m_controlMutex);
            for (auto *e : qAsConst(m_entries)) {
                if (!e) continue;
                const int64_t endUs = static_cast<int64_t>(e->entry.timelineEnd * 1e6);
                if (endUs > totalDurationUs) totalDurationUs = endUs;
            }
        }
        if (timelineUs < 0 || timelineUs > totalDurationUs + 100'000) {
            qWarning().nospace()
                << "[AudioMixer::seekTo OOB] requested=" << timelineUs
                << " totalDurationUs=" << totalDurationUs
                << " clamping to valid range";
            timelineUs = qBound<int64_t>(0, timelineUs, totalDurationUs);
        }
    }
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
            // US-VFF-008 (revert of US-VFF-007 here): strict-equality
            // dedup does NOT touch sinkSnap — the OS audio buffer is still
            // playing pre-seek samples (~50–170 ms behind cursor), so the
            // existing m_audibleLagUs accurately reflects that lag.
            // Forcing lag=0 made audibleClockUs() return cursor (sample-
            // write position), which is 50–170 ms ahead of the actual
            // audible position; correctVideoDriftAgainstAudioClock then
            // skip-decoded up to 6 frames/tick → gap-independent fast-
            // forward (3-agent RCA round 2). The cursor never moved and
            // the sink kept producing samples, so prior bookkeeping
            // (including m_audibleLagUs) remains accurate.
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
            // US-VFF-008 (revert of US-VFF-007 here): tolerant-dedup
            // bumps m_writeCursorUs to timelineUs but does NOT reset the
            // sink. The OS audio buffer (~50–170 ms ahead of audible
            // playback) keeps the previous m_audibleLagUs valid for
            // audibleClockUs() = cursor − lag. Setting lag=0 here was
            // the dominant fast-forward trigger: skip-decode loop saw
            // audioTlUs ≈ cursor (write position) instead of audible
            // position, computed +50–170 ms drift, and discard-decoded
            // up to 6 frames/tick. Empirical gap-independent reproduction
            // post-fix confirmed this. Leave m_audibleLagUs untouched.
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
                e->atempoSrcFrameCarry = 0.0;
                e->ringStartTlUs = timelineUs;
                e->eof = false;
                // US-FIX-7 (R3): Fix-K sets ringStartTlUs = timelineUs (true
                // cursor on an active sink). Clear both US-FIX-7 flags so a
                // prior paused-seek's anchor/fast-drain state does not corrupt
                // this active-sink ring reset.
                e->needsPtsAnchor = false;
                e->postSeekFullDrop = false;
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
            e->atempoSrcFrameCarry = 0.0;
            e->ringStartTlUs = timelineUs;
            e->eof = false;
            // US-FIX-7 (R3): clear both flags. seekEntryToTimeline (called
            // from refillRings once the decode worker wakes) will re-set both
            // to true. Clearing here prevents stale state from a prior seek
            // corrupting the window between this ring-reset and that call.
            e->needsPtsAnchor = false;
            e->postSeekFullDrop = false;
        }
        sinkSnap = m_sink;
        ioSnap = m_io;
        restartAfterReset = (m_io && m_playing.load(std::memory_order_acquire));
    }
    if (sinkSnap) {
        // Drop OS-buffered samples so post-seek audio starts cleanly.
        sinkSnap->reset();
        // US-FIX-3: zero per-track DSP feedback/history on full-restart
        // seek. The dedup early-returns above keep the sink streaming, so
        // they MUST NOT touch DSP state — clearing comb buffers mid-
        // playback would itself click. Here, sinkSnap->reset() has just
        // synchronously cancelled the readData callback, so the audio
        // worker is dormant until sinkSnap->start() resumes it below;
        // mutating m_track*State under m_controlMutex is safe.
        // Without this zero-fill, ReverbState comb buffers + allpass
        // delays + pre-delay buffers retain pre-seek samples that
        // recirculate through comb feedback at the new position,
        // producing a colored "砂嵐" (sandstorm) noise that decays over
        // the comb tail (~decaySeconds). EQ biquad histories produce a
        // brief filter transient on the first post-seek sample. The
        // envelope followers (CompState.env / NRState.env) reconverge
        // within ~10 ms but still contribute a quieter transient.
        // Statistical accumulators (NRState.recentEnvs auto-floor
        // window, CompState.currentGrDb meter readout) are intentionally
        // left untouched: they are not per-sample feedback paths and
        // resetting them would degrade post-seek noise-gate accuracy.
        //
        // NOTE: This zero-fill removes the DSP-feedback component of the
        // seek 砂嵐. A SECOND, independent component — decoder pre-roll from
        // av_seek_frame(AVSEEK_FLAG_BACKWARD) — is handled by US-FIX-7 in
        // seekEntryToTimeline + readData: ringStartTlUs is backstep-set so
        // readData's late-drop discards pre-roll with the postSeekFullDrop
        // clamp bypass. Both fixes are required for a clean post-seek start.
        {
            QMutexLocker lock(&m_controlMutex);
            for (auto it = m_trackReverbState.begin();
                 it != m_trackReverbState.end(); ++it) {
                ReverbState &st = it.value();
                for (int ch = 0; ch < kChannels; ++ch) {
                    st.preDelay[ch].fill(0.0f);
                    st.preDelayIdx[ch] = 0;
                    for (int c = 0; c < kReverbCombCount; ++c) {
                        if (!st.comb[ch][c].isEmpty())
                            st.comb[ch][c].fill(0.0f);
                        st.combIdx[ch][c] = 0;
                        st.combLP[ch][c] = 0.0f;
                    }
                    for (int a = 0; a < kReverbAllpassCount; ++a) {
                        if (!st.ap[ch][a].isEmpty())
                            st.ap[ch][a].fill(0.0f);
                        st.apIdx[ch][a] = 0;
                    }
                }
            }
            for (auto it = m_trackEqHist.begin();
                 it != m_trackEqHist.end(); ++it) {
                it.value().fill(0.0);
            }
            for (auto it = m_trackCompState.begin();
                 it != m_trackCompState.end(); ++it) {
                it.value().env = 0.0;
            }
            for (auto it = m_trackNoiseReductionState.begin();
                 it != m_trackNoiseReductionState.end(); ++it) {
                it.value().env = 0.0;
            }
        }
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

double AudioMixer::trackGain(int trackIdx) const
{
    QMutexLocker lock(&m_controlMutex);
    if (trackIdx < 0 || trackIdx >= kMaxAudioTracks) return 1.0;
    if (trackIdx >= m_trackStates.size()) return 1.0;
    return m_trackStates[trackIdx].gain;
}

void AudioMixer::setTrackEqConfig(int trackIdx, const AudioEQConfig &cfg) {
    if (trackIdx < 0 || trackIdx >= kMaxAudioTracks) return;

    // Compute coefficients BEFORE taking the lock so we don't block the audio thread
    std::array<EqBandCoefs, 3> coefs;
    constexpr double fs = kSampleRateHz;
    for (int i = 0; i < cfg.bands.size() && i < 3; ++i) {
        const auto &band = cfg.bands[i];
        if (band.isFlat()) {
            coefs[i] = {1.0, 0.0, 0.0, 0.0, 0.0};
            continue;
        }
        BiquadCoefs c;
        if (i == 0)      c = calcLowShelf(band.frequency, band.gain, fs);
        else if (i == 1) c = calcPeaking(band.frequency, band.gain, band.q, fs);
        else             c = calcHighShelf(band.frequency, band.gain, fs);
        coefs[i] = {c.b0, c.b1, c.b2, c.a1, c.a2};
    }

    QMutexLocker lock(&m_controlMutex);
    if (m_trackStates.size() < trackIdx + 1)
        m_trackStates.resize(trackIdx + 1);
    auto &ts = m_trackStates[trackIdx];
    ts.eq = cfg;
    ts.eqCoeffs = coefs;
    ts.z = {};
    ts.eqCache = {};
}

AudioEQConfig AudioMixer::trackEqConfig(int trackIdx) const {
    QMutexLocker lock(&m_controlMutex);
    if (trackIdx < 0 || trackIdx >= m_trackStates.size())
        return AudioEQConfig{};
    return m_trackStates[trackIdx].eq;
}

void AudioMixer::setTrackEqEnabled(int trackIdx, bool enabled) {
    QMutexLocker lock(&m_controlMutex);
    if (trackIdx < 0 || trackIdx >= kMaxAudioTracks) return;
    if (m_trackStates.size() < trackIdx + 1)
        m_trackStates.resize(trackIdx + 1);
    m_trackStates[trackIdx].eqEnabled = enabled;
}

// ---------------------------------------------------------------------------
// 4-band parametric EQ (Premiere/Audition parity). Cascaded with the legacy
// 3-band path above. The 4-band cascade runs first in readData, then the
// existing 3-band path, then preamp / gain. Coefficients are precomputed
// here on the GUI thread so the audio worker thread never recalculates
// transcendentals; readData reads m_trackEqCoefs / m_trackEqHist under
// m_controlMutex (same lock that guards the existing mix path).
// ---------------------------------------------------------------------------
namespace {
AudioMixer::EqBandCoefsParam computeEqBand(const AudioMixer::EqBand &band,
                                           int kind, // 0=low shelf, 1/2=peak, 3=high shelf
                                           double fs) {
    AudioMixer::EqBandCoefsParam out;
    if (!band.enabled || band.gainDb == 0.0) {
        out.active = false; // pass-through
        return out;
    }
    // Clamp f0 in [20, fs/2 - 100], Q in [0.1, 10], gainDb in [-24, +24].
    const double nyqMargin = fs / 2.0 - 100.0;
    const double f0 = qBound(20.0, band.freq, nyqMargin);
    const double q = qBound(0.1, band.q, 10.0);
    const double gainDb = qBound(-24.0, band.gainDb, 24.0);

    BiquadCoefs c;
    if (kind == 0)      c = calcLowShelf(f0, gainDb, fs);
    else if (kind == 3) c = calcHighShelf(f0, gainDb, fs);
    else                c = calcPeaking(f0, gainDb, q, fs);
    out.b0 = c.b0; out.b1 = c.b1; out.b2 = c.b2; out.a1 = c.a1; out.a2 = c.a2;
    out.active = true;
    return out;
}
} // namespace

void AudioMixer::setEqForTrack(int trackId, const EqSettings &eq) {
    if (trackId < 0 || trackId > kMaxAudioTracks) return;
    constexpr double fs = kSampleRateHz;

    // Compute coefficients before taking the lock so we don't block readData.
    std::array<EqBandCoefsParam, 4> coefs{};
    coefs[0] = computeEqBand(eq.low,     0, fs);
    coefs[1] = computeEqBand(eq.lowMid,  1, fs);
    coefs[2] = computeEqBand(eq.highMid, 2, fs);
    coefs[3] = computeEqBand(eq.high,    3, fs);

    QMutexLocker lock(&m_controlMutex);
    m_trackEq[trackId] = eq;
    // Atomic replace: insert overwrites the previous EqSettings/coefs in a
    // single hash slot under the mutex held by readData, so in-flight audio
    // sees either the old or new coefs but never a torn struct. History is
    // preserved across changes so the filter doesn't pop on coefficient
    // swap (resetting to zero would produce a transient).
    m_trackEqCoefs[trackId] = coefs;
    if (!m_trackEqHist.contains(trackId))
        m_trackEqHist.insert(trackId, std::array<double, 16>{});
}

AudioMixer::EqSettings AudioMixer::eqForTrack(int trackId) const {
    QMutexLocker lock(&m_controlMutex);
    return m_trackEq.value(trackId, EqSettings{});
}

// Per-track compressor (Audition / Resolve Fairlight parity). Atomic
// replace under m_controlMutex; the envelope state in m_trackCompState
// is preserved across the swap so changing a knob never causes an
// audible transient. Disabled (or ratio=1) settings still write through
// so the panel UI stays in sync; the readData path skips work for
// disabled entries (bit-exact bypass).
void AudioMixer::setCompressorForTrack(int trackId, const CompressorSettings &c) {
    CompressorSettings clamped;
    clamped.thresholdDb = qBound(-60.0, c.thresholdDb, 0.0);
    clamped.ratio       = qBound(1.0, c.ratio, 50.0);
    clamped.attackMs    = qBound(0.1, c.attackMs, 100.0);
    clamped.releaseMs   = qBound(10.0, c.releaseMs, 1000.0);
    clamped.kneeDb      = qBound(0.0, c.kneeDb, 10.0);
    clamped.makeupDb    = qBound(0.0, c.makeupDb, 24.0);
    clamped.enabled     = c.enabled;

    QMutexLocker lock(&m_controlMutex);
    m_trackComp[trackId] = clamped;
    // Initialise state on first touch; subsequent setCompressorForTrack
    // calls leave envelope untouched so live tweaking is glitch-free.
    if (!m_trackCompState.contains(trackId))
        m_trackCompState.insert(trackId, CompressorState{});
}

AudioMixer::CompressorSettings AudioMixer::compressorForTrack(int trackId) const {
    QMutexLocker lock(&m_controlMutex);
    return m_trackComp.value(trackId, CompressorSettings{});
}

double AudioMixer::currentGainReductionDb(int trackId) const {
    QMutexLocker lock(&m_controlMutex);
    auto it = m_trackCompState.constFind(trackId);
    if (it == m_trackCompState.constEnd()) return 0.0;
    return it.value().currentGrDb;
}

// Per-track reverb (Audition / Fairlight Multitap parity, simplified
// Schroeder topology). Atomic replace under m_controlMutex; the DSP
// state in m_trackReverbState is preserved across the swap so changing
// a knob never causes an audible transient. Disabled (or mixRatio=0)
// settings still write through so the panel UI stays in sync; the
// readData path skips work for disabled / zero-mix entries (bit-exact
// bypass).
void AudioMixer::setReverbForTrack(int trackId, const ReverbSettings &r) {
    if (trackId < 0 || trackId > kMaxAudioTracks) return;

    ReverbSettings clamped;
    clamped.mixRatio      = qBound(0.0,   r.mixRatio,      1.0);
    clamped.decaySeconds  = qBound(0.1,   r.decaySeconds,  5.0);
    clamped.preDelayMs    = qBound(0.0,   r.preDelayMs,    200.0);
    clamped.dampingHF     = qBound(0.0,   r.dampingHF,     100.0);
    clamped.widthPercent  = qBound(0.0,   r.widthPercent,  100.0);
    clamped.enabled       = r.enabled;

    QMutexLocker lock(&m_controlMutex);
    m_trackReverb[trackId] = clamped;
    // Initialise state on first touch; subsequent setReverbForTrack calls
    // leave buffers untouched so live tweaking is glitch-free.
    if (!m_trackReverbState.contains(trackId))
        m_trackReverbState.insert(trackId, ReverbState{});
}

AudioMixer::ReverbSettings AudioMixer::reverbForTrack(int trackId) const {
    QMutexLocker lock(&m_controlMutex);
    return m_trackReverb.value(trackId, ReverbSettings{});
}

// Per-track noise reduction (Audition Voice Isolation / Resolve Fairlight
// noise gate parity, simplified expander). Atomic replace under
// m_controlMutex; the DSP state in m_trackNoiseReductionState is preserved
// across the swap so changing a knob never causes an audible transient.
// Disabled (or default-constructed) settings still write through so the
// panel UI stays in sync; the readData path skips work for disabled
// entries (bit-exact bypass).
void AudioMixer::setNoiseReductionForTrack(int trackId,
                                           const NoiseReductionSettings &nr) {
    if (trackId < 0 || trackId > kMaxAudioTracks) return;

    NoiseReductionSettings clamped;
    clamped.thresholdDb   = qBound(-60.0, nr.thresholdDb,    0.0);
    clamped.reductionDb   = qBound(  0.0, nr.reductionDb,   40.0);
    clamped.attackMs      = qBound(  0.1, nr.attackMs,      50.0);
    clamped.releaseMs     = qBound( 10.0, nr.releaseMs,   1000.0);
    clamped.manualFloorDb = qBound(-80.0, nr.manualFloorDb, -30.0);
    clamped.autoFloor     = nr.autoFloor;
    clamped.enabled       = nr.enabled;

    QMutexLocker lock(&m_controlMutex);
    m_trackNoiseReduction[trackId] = clamped;
    // Initialise state on first touch; subsequent setNoiseReductionForTrack
    // calls leave envelope + history untouched so live tweaking is
    // glitch-free.
    if (!m_trackNoiseReductionState.contains(trackId))
        m_trackNoiseReductionState.insert(trackId, NRState{});
}

AudioMixer::NoiseReductionSettings
AudioMixer::noiseReductionForTrack(int trackId) const {
    QMutexLocker lock(&m_controlMutex);
    return m_trackNoiseReduction.value(trackId, NoiseReductionSettings{});
}

double AudioMixer::estimatedNoiseFloorDb(int trackId) const {
    QMutexLocker lock(&m_controlMutex);
    auto it = m_trackNoiseReductionState.constFind(trackId);
    if (it == m_trackNoiseReductionState.constEnd()) return -60.0;
    return it.value().estimatedFloorDb;
}

void AudioMixer::setNormalizerAmount(double amount) {
    m_normalizerAmount.store(qBound(0.0, amount, 1.0), std::memory_order_release);
}

void AudioMixer::setNormalizerUniformity(double uniformity) {
    m_normalizerUniformity.store(qBound(0.0, uniformity, 1.0), std::memory_order_release);
}

void AudioMixer::setCompressorParams(const CompressorParams &params) {
    QMutexLocker lock(&m_controlMutex);
    m_compressorParams.thresholdDb = qBound(-30.0, params.thresholdDb, 0.0);
    m_compressorParams.ratio       = qBound(1.0, params.ratio, 20.0);
    m_compressorParams.attackMs    = qBound(1.0, params.attackMs, 100.0);
    m_compressorParams.releaseMs   = qBound(10.0, params.releaseMs, 1000.0);
    m_compressorParams.makeupDb    = qBound(0.0, params.makeupDb, 18.0);
    m_compressorParams.enabled     = params.enabled;
}

void AudioMixer::setCompressorEnabled(bool enabled) {
    QMutexLocker lock(&m_controlMutex);
    m_compressorParams.enabled = enabled;
}

AudioMixer::CompressorParams AudioMixer::compressorParams() const {
    QMutexLocker lock(&m_controlMutex);
    return m_compressorParams;
}

bool AudioMixer::compressorEnabled() const {
    QMutexLocker lock(&m_controlMutex);
    return m_compressorParams.enabled;
}

void AudioMixer::setAutoDuckParams(const AutoDuckParams &params) {
    QMutexLocker lock(&m_controlMutex);
    m_autoDuckParams.thresholdDb = qBound(-60.0, params.thresholdDb, 0.0);
    m_autoDuckParams.ratio       = qBound(1.0, params.ratio, 20.0);
    m_autoDuckParams.attackMs    = qBound(0.1, params.attackMs, 5000.0);
    m_autoDuckParams.releaseMs   = qBound(1.0, params.releaseMs, 10000.0);
}

AudioMixer::AutoDuckParams AudioMixer::autoDuckParams() const {
    QMutexLocker lock(&m_controlMutex);
    return m_autoDuckParams;
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
    // B3 fix: avcodec_flush_buffers resets the codec but the SwrContext
    // (created once in openEntry) retains an internal delay-compensation
    // FIFO. Without this re-init the first post-seek resampleAndAppend call
    // drains stale pre-seek samples from that FIFO into the ring before any
    // new decoded frames arrive, producing a brief filter transient / wrong-
    // position burst on top of the keyframe pre-roll. swr_init on an already-
    // initialised context is the libswresample-documented way to clear all
    // internal state (delay buffer, compensation accumulators) while keeping
    // the channel/format/rate configuration intact.
    if (e->swrCtx) swr_init(e->swrCtx);
    e->ring.clear();
    e->ringHead = 0;
    e->atempoSrcFrameCarry = 0.0;
    e->eof = false;

    // US-FIX-7 (seek 砂嵐, R3 — two-flag PTS-anchored, no synchronous decode):
    // av_seek_frame(AVSEEK_FLAG_BACKWARD) lands on the nearest keyframe ≤ ts.
    // First samples refillRingForEntry appends are pre-roll from that keyframe.
    //
    // Two flags are set here (see struct doc for ownership):
    //   needsPtsAnchor = true  → resampleAndAppend writes trueTlUs on 1st frame
    //   postSeekFullDrop = true → readData bypasses 2 ms clamp to drain pre-roll
    //
    // refillRings holds m_controlMutex for its entire pass, so within that same
    // lock: seekEntryToTimeline (here) → refillRingForEntry → resampleAndAppend
    // anchors ringStartTlUs=trueTlUs and clears needsPtsAnchor. postSeekFullDrop
    // is NOT cleared by resampleAndAppend (PTS path) — it stays true so that
    // readData, running AFTER the lock is released, sees postSeekFullDrop=true
    // and ringStartTlUs=trueTlUs<cursorUs and fast-drains the pre-roll.
    //
    // NOPTS fallback: if first frame PTS is AV_NOPTS_VALUE, resampleAndAppend
    // clears BOTH flags and leaves ringStartTlUs=timelineUs → pre-US-FIX-7
    // behaviour (2 ms/callback residual, no valid-audio loss risk).
    e->ringStartTlUs = timelineUs;

    // US-FIX-7 (R8 — defense-in-depth speed/ramp safety gate):
    // seekEntryToTimeline maps timeline→source as a strict 1:1 affine
    //   fileLocalSec = clipIn + (tlSec − timelineStart)
    // and resampleAndAppend's anchor is the exact inverse. That pair is only
    // an exact inverse when audio is consumed 1:1. Audio time-stretch for
    // ramped clips happens ONLY on readData's atempoRamp path, which is gated
    // by audioAtempoEnabledCached() ⟸ env VEDITOR_AUDIO_ATEMPO (default OFF).
    // With atempo OFF (the default editor preview) audio is 1:1 and the
    // forward map + inverse anchor are exact inverses → the R3 two-flag
    // fast-drain is correct. For a non-1x uniform speed, OR an authored ramp
    // under the opt-in envvar, timeline-time ≠ source-time so the 1:1 anchor
    // is imprecise and a mis-anchored keyframe pre-roll could be fast-drained.
    // To keep US-FIX-7 robust even under the gated/non-default case, revert
    // exactly that case to documented pre-US-FIX-7 behaviour (normal 2 ms
    // residual late-drop, no PTS anchor, no fast-drain). The 1x default path
    // is byte-for-byte unchanged. (seekEntryToTimeline is an AudioMixer
    // method, so m_speedRampByKey / audioAtempoEnabledCached() are in scope
    // directly; the key construction mirrors readData's atempo lookup.)
    const double r8UniSpeed =
        (e->entry.speed > 0.0) ? e->entry.speed : 1.0;
    const bool r8NonUnitSpeed = std::abs(r8UniSpeed - 1.0) > 1e-9;
    bool r8AtempoRampPresent = false;
    if (audioAtempoEnabledCached() && !m_speedRampByKey.isEmpty()) {
        // [P2-M1] R10-b: locking-regression tripwire (Q_ASSERT under mutex
        // invariance). seekEntryToTimeline is called from refillRingForEntry
        // under m_controlMutex (the caller holds the mutex for its entire
        // pass) and setSpeedRamps bumps m_speedRampGeneration under the SAME
        // mutex; the pre-lookup and post-lookup loads of the generation MUST
        // observe identical values. If the assert fires, the single-lock
        // invariant has been broken by a future restructure and the R8 gate
        // decision is unsafe. Q_ASSERT compiles out in Release builds so the
        // production cost is zero; Debug builds detect the regression at the
        // first stale-snapshot path.
        const uint64_t genAtEntry =
            m_speedRampGeneration.load(std::memory_order_relaxed);
        const AudioTrackKey r8Key{
            e->entry.filePath,
            qRound64(e->entry.clipIn * 1000.0),
            e->entry.sourceTrack,
            e->entry.sourceClipIndex
        };
        const bool present =
            (m_speedRampByKey.constFind(r8Key) != m_speedRampByKey.constEnd());
        Q_ASSERT(genAtEntry == m_speedRampGeneration.load(
                                  std::memory_order_relaxed)
                 && "R10-b: mutex protects gen invariance — if this fires, "
                    "lock structure regressed");
        r8AtempoRampPresent = present;
    }
    if (r8NonUnitSpeed || r8AtempoRampPresent) {
        // Gated/non-default time-stretched case: 1:1 anchor is imprecise.
        // Fall back to pre-US-FIX-7 behaviour so a mis-anchored pre-roll is
        // never fast-drained (normal 2 ms-clamp residual late-drop).
        e->needsPtsAnchor = false;
        e->postSeekFullDrop = false;
    } else {
        // Default 1x path (no envvar, speed==1.0, no ramp): unchanged R7
        // two-flag PTS-anchored fast-drain.
        e->needsPtsAnchor = true;
        e->postSeekFullDrop = true;
    }
}

void AudioMixer::resampleAndAppend(AudioDecoderEntry *e) {
    if (!e || !e->frame || !e->swrCtx) return;

    // US-FIX-7 (R3 — two-flag PTS anchor, owned by this function):
    // needsPtsAnchor is set by seekEntryToTimeline and consumed here on the
    // first post-seek decoded frame. We write ringStartTlUs = trueTlUs (the
    // true timeline µs of ring[0]) so readData's late-drop discards exactly
    // the real keyframe pre-roll and no more. postSeekFullDrop is NOT cleared
    // here on the PTS-valid path — it must stay true until readData sees
    // ringStartTlUs < cursorUs and fast-drains the pre-roll (postSeekFullDrop
    // bypasses the 2 ms clamp; readData clears it once ringStartTlUs>=cursorUs).
    // Both flags are cleared together only on the NOPTS fallback path.
    //
    // Mapping (inverse of seekEntryToTimeline's fileLocalSec formula):
    //   seekEntryToTimeline: fileLocalSec = clipIn + (tlSec − timelineStart)
    //   inverse:             trueTlSec    = timelineStart + (framePtsSec − clipIn)
    //                        trueTlUs     = int64_t(trueTlSec × 1e6)
    if (e->needsPtsAnchor) {
        e->needsPtsAnchor = false;   // consumed — one-shot regardless of path
        const int64_t framePts =
            (e->frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? e->frame->best_effort_timestamp
                : e->frame->pts;
        if (framePts != AV_NOPTS_VALUE && e->fmtCtx
                && e->audioStreamIdx >= 0) {
            // PTS-valid path: anchor ringStartTlUs, leave postSeekFullDrop=true
            // for readData to fast-drain.
            const AVRational tb =
                e->fmtCtx->streams[e->audioStreamIdx]->time_base;
            const double framePtsSec = framePts * av_q2d(tb);
            const double trueTlSec =
                e->entry.timelineStart + (framePtsSec - e->entry.clipIn);
            const int64_t trueTlUs =
                static_cast<int64_t>(trueTlSec * 1e6);
            const int64_t prevRingStartTlUs = e->ringStartTlUs;
            e->ringStartTlUs = trueTlUs;
            qInfo() << "US-FIX-7 anchor: framePts=" << framePts
                    << "framePtsSec=" << framePtsSec
                    << "trueTlUs=" << trueTlUs
                    << "was(timelineUs)=" << prevRingStartTlUs
                    << "gapUs=" << (prevRingStartTlUs - trueTlUs);
        } else {
            // NOPTS fallback: PTS unknowable — cannot determine pre-roll gap.
            // Clear postSeekFullDrop so readData uses the normal 2 ms clamp;
            // ringStartTlUs stays at timelineUs (pre-US-FIX-7 residual path,
            // never fast-drops unknown-position content).
            //
            // US-FIX-7 (R8): the residual on this branch is an ACCEPTED
            // pre-existing limitation (adjudicated across multiple
            // remediation rounds), not a new blocker. AAC/MP3 — the codecs
            // this editor ingests — effectively always carry PTS, so
            // best_effort_timestamp is valid in practice; a NOPTS audio
            // stream is pathologically rare. On NOPTS we cannot determine
            // the keyframe pre-roll gap, so we deliberately fall back to the
            // pre-US-FIX-7 2 ms-clamp residual rather than risk fast-draining
            // unknown-position content. No logic change here — comment only.
            e->postSeekFullDrop = false;
        }
    }

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
