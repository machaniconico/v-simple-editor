#pragma once

#include <QObject>
#include <QVector>
#include <QString>
#include <QHash>
#include <QMutex>
#include <QIODevice>
#include <QAudioSink>
#include <QAudioFormat>
#include <QElapsedTimer>
#include "SpeedRampData.h"
#include <array>
#include <atomic>

#include "PlaybackTypes.h"
#include "AudioEQ.h"
#include "AudioBusRouting.h"

// AudioMixer — sums up to MAX_AUDIO_TRACKS independent FFmpeg-decoded
// audio streams into a single 48 kHz s16 stereo output via QAudioSink in
// push mode. Replaces the old single-source QMediaPlayer side-player. The
// class header keeps FFmpeg includes private; AudioDecoderEntry,
// MixerIODevice, and AudioDecodeRunner are forward-declared and defined
// in AudioMixer.cpp.
//
// Thread model:
//   * GUI thread owns AudioMixer and calls setSequence/seekTo/play/pause.
//   * QAudioSink invokes MixerIODevice::readData on its internal worker
//     thread; that path locks m_controlMutex briefly to drain ring data
//     and read per-track gains.
//   * AudioDecodeRunner runs on a dedicated QThread, periodically taking
//     m_controlMutex to refill ring buffers ahead of the audio sink.
//
// Master clock contract: m_writeCursorUs is the audible timeline-space
// position. The audio callback publishes updates with release semantics;
// VideoPlayer's scheduling code reads with acquire. The clock never
// rewinds except on an explicit seekTo.
struct AudioDecoderEntry;
class MixerIODevice;
class AudioDecodeRunner;

struct AudioTrackKey {
    QString filePath;
    qint64 clipInMs = 0;
    int sourceTrack = 0;
    int sourceClipIndex = -1;
    bool operator==(const AudioTrackKey &o) const noexcept {
        return filePath == o.filePath
            && clipInMs == o.clipInMs
            && sourceTrack == o.sourceTrack
            && sourceClipIndex == o.sourceClipIndex;
    }
};
inline uint qHash(const AudioTrackKey &k, uint seed = 0) noexcept {
    // Boost-style hash combine. XOR-of-equal-seed collides whenever fields
    // pairwise produce identical hashes (e.g. sourceTrack == sourceClipIndex);
    // with hash_combine the bits diffuse properly.
    uint h = qHash(k.filePath, seed);
    h ^= qHash(k.clipInMs, seed) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= qHash(k.sourceTrack, seed) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= qHash(k.sourceClipIndex, seed) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}

class AudioMixer : public QObject {
    Q_OBJECT
public:
    static constexpr int kSampleRateHz = 48000;
    static constexpr int kChannels = 2;
    static constexpr int kBytesPerSample = 2;                  // s16
    static constexpr int kBytesPerFrame = kChannels * kBytesPerSample;
    static constexpr int kMaxAudioTracks = 16;
    // Trust-boundary clamp limits for per-track EQ (US-501).
    // Applied at ProjectFile JSON deserialization so a malicious .veditor
    // cannot push NaN or out-of-range values into the biquad coefficient
    // computation inside setTrackEqConfig.
    static constexpr double kEqMinQ = 0.1;
    static constexpr double kEqMaxQ = 18.0;
    static constexpr double kEqMinGainDb = -24.0;
    static constexpr double kEqMaxGainDb = 24.0;
    static constexpr double kEqMinFreqHz = 20.0;
    static constexpr double kEqMaxFreqHz = 20000.0;
    static constexpr int kRingTargetBytes = 64 * 1024;         // ~340 ms stereo s16 @ 48k
    static constexpr int kPrerollLeadUs = 2'000'000;           // pre-warm 2 s before entry start

    explicit AudioMixer(QObject *parent = nullptr);
    ~AudioMixer() override;

    // Replace the active timeline schedule. Opens decoders for new entries
    // and releases decoders for entries no longer present. Safe from GUI
    // thread; locks m_controlMutex briefly.
    void setSequence(const QVector<PlaybackEntry> &entries);
    // Parallel speed-ramp array aligned to setSequence entries.
    void setSpeedRamps(const QVector<speedramp::SpeedRamp> &ramps);

    // Jump the audible playhead. Resyncs FFmpeg seek inside every active
    // entry (lazily on next refill) and flushes ring buffers so the next
    // sample read is at the new position.
    void seekTo(int64_t timelineUs);

    // Master clock — position the mixer has DELIVERED samples to the OS
    // audio buffer through. Note this runs ahead of what the user actually
    // hears by sink->bufferSize() (≈200 ms): the difference is samples sat
    // in the hardware buffer, not yet played. VideoPlayer pace/drift code
    // can use audibleClockUs() to compensate when sub-frame accuracy
    // matters.
    int64_t masterClockUs() const {
        return m_writeCursorUs.load(std::memory_order_acquire);
    }
    int64_t audibleClockUs() const;

    // Transport
    void play();
    void pause();
    void stop();
    bool isPlaying() const { return m_playing.load(std::memory_order_acquire); }

    // Per-track controls. trackIdx is sourceTrack from PlaybackEntry.
    void setTrackMute(int trackIdx, bool muted);
    void setTrackSolo(int trackIdx, bool solo);
    void setTrackGain(int trackIdx, double gain);
    double trackGain(int trackIdx) const;

    // Bus / submix / aux-send routing (AB-4). Replaces the per-track →
    // master bus-gain map consulted in the mix loop. The routing object is
    // a pure value engine (audiobus::AudioBusRouting); setBusRouting swaps
    // the whole snapshot under m_controlMutex so the audio worker thread
    // always reads a coherent copy. When no buses are defined and no track
    // is assigned, resolveTrackToMasterGain returns 1.0 for every track, so
    // the mix is bit-identical to the pre-AB-4 path (identity guarantee).
    void setBusRouting(const audiobus::AudioBusRouting &r);
    audiobus::AudioBusRouting busRouting() const;

    // Per-track realtime EQ (3-band biquad, applied before effectiveGain).
    void setTrackEqConfig(int trackIdx, const AudioEQConfig &cfg);
    AudioEQConfig trackEqConfig(int trackIdx) const;
    void setTrackEqEnabled(int trackIdx, bool enabled);

    // Per-track 4-band parametric EQ (Premiere/Audition parity). Independent
    // of the legacy 3-band path above; cascaded BEFORE volume/pan stages
    // (signal flow: 4-band EQ → existing 3-band EQ → preamp → gain).
    // trackId convention: 0 = master, 1 = A1, 2 = A2, ... (the audio mix
    // path uses sourceTrack which is 1-based for tracks; trackId=0 is
    // reserved for a future master-bus EQ and currently no-ops in the
    // mix loop, so it is safe to call with 0 from the panel).
    struct EqBand {
        double freq;
        double gainDb;
        double q;
        bool enabled = true;
    };
    struct EqSettings {
        EqBand low{80.0, 0.0, 0.7};
        EqBand lowMid{250.0, 0.0, 1.0};
        EqBand highMid{3000.0, 0.0, 1.0};
        EqBand high{10000.0, 0.0, 0.7};
    };
    void setEqForTrack(int trackId, const EqSettings &eq);
    EqSettings eqForTrack(int trackId) const;

    // Cached biquad coefficients for the 4-band path. Kept public so the
    // namespace-scoped computeEqBand helper in AudioMixer.cpp can return it
    // before the GUI thread takes m_controlMutex.
    struct EqBandCoefsParam {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        bool active = false; // false = pass-through (skip math, keep history pristine)
    };

    // Per-track feed-forward compressor / limiter (Audition / Resolve
    // Fairlight parity). Cascaded AFTER the 4-band EQ stage and BEFORE the
    // legacy 3-band / preamp / gain stages, so meters and the master bus
    // see post-compression peaks. Stereo-link detector (max(|L|, |R|))
    // drives a transposed envelope follower with separate attack / release
    // coefficients. Static curve uses Cherny-style soft knee (quadratic
    // spline between threshold ± knee/2). Disabled = bit-exact bypass
    // (skip the entire envelope + curve math; do not touch state).
    //
    // Limiter mode contract: when ratio >= 20 the path uses an effective
    // 1000:1 ratio and forces attack to 0.5 ms regardless of attackMs, so
    // any ratio knob value of 20+ becomes a brick-wall limit at threshold
    // (overshoot bounded by the 0.5 ms attack, ≤0.5 dB at typical material).
    struct CompressorSettings {
        double thresholdDb = 0.0;     // 0 = no compression at any signal level
        double ratio = 1.0;           // 1:1 = no compression (pass-through)
        double attackMs = 5.0;
        double releaseMs = 100.0;
        double kneeDb = 2.0;
        double makeupDb = 0.0;
        bool enabled = false;
    };
    struct CompressorState {
        double env = 0.0;        // envelope follower (linear amplitude)
        double currentGrDb = 0.0;// last gain reduction in dB (for meter)
    };
    void setCompressorForTrack(int trackId, const CompressorSettings &c);
    CompressorSettings compressorForTrack(int trackId) const;
    double currentGainReductionDb(int trackId) const;

    // Per-track reverb (Audition / Fairlight Multitap parity, simplified).
    // Schroeder topology: pre-delay → 4 parallel comb filters → 2 series
    // allpass filters → wet/dry mix. Cascaded AFTER the compressor stage
    // and BEFORE the legacy 3-band / preamp / gain stages so meters and
    // the master bus see the post-reverb signal. Disabled or mixRatio=0
    // = bit-exact bypass (skip the entire DSP path; do not touch state).
    //
    // Comb delays (Freeverb defaults @ 44.1 kHz, scaled to actual sample
    // rate): 1116 / 1188 / 1277 / 1356 samples. Allpass delays: 556 /
    // 441 samples (g = 0.5). Comb feedback gain g = 0.7 * decaySeconds /
    // 1.0 sec reference. High-freq damping = LP coefficient inside the
    // comb feedback loop (0=none, 1=heavy damping). Width controls
    // stereo cross-feed (0=mono sum, 100=fully decorrelated channels).
    struct ReverbSettings {
        double mixRatio = 0.0;       // 0.0..1.0  (UI 0..100 / 100)
        double decaySeconds = 1.0;   // 0.1..5.0
        double preDelayMs = 20.0;    // 0..200
        double dampingHF = 30.0;     // 0..100
        double widthPercent = 50.0;  // 0..100
        bool enabled = false;
    };
    void setReverbForTrack(int trackId, const ReverbSettings &r);
    ReverbSettings reverbForTrack(int trackId) const;

    // Per-track noise reduction (Audition Voice Isolation / Resolve
    // Fairlight noise gate parity, simplified expander). Cascaded FIRST in
    // the per-track effect chain — BEFORE the 4-band EQ, compressor, and
    // reverb stages — so the cleanup runs on the rawest signal and downstream
    // effects amplify the de-noised material. Stereo-link detector
    // (max(|L|, |R|)) drives a transposed envelope follower with separate
    // attack / release coefficients; the static curve compares the envelope
    // to a noise-floor estimate and applies a smooth ramp between full
    // pass-through and reductionDb of attenuation. Auto-floor mode tracks a
    // running 5th-percentile of the envelope (in dBFS) over the last
    // ~5 seconds; manual mode uses manualFloorDb directly.
    //
    // Disabled = bit-exact bypass (skip the entire envelope + curve math
    // and do not touch state, mirroring the compressor / reverb stages).
    struct NoiseReductionSettings {
        double thresholdDb = -20.0;   // gate engages this many dB above floor
        double reductionDb = 12.0;    // max attenuation when fully gated
        double attackMs = 5.0;        // envelope attack
        double releaseMs = 200.0;     // envelope release
        double manualFloorDb = -50.0; // used when autoFloor == false
        bool autoFloor = true;
        bool enabled = false;
    };
    // Per-track NR DSP state. recentEnvs is a rolling-window envelope
    // history (in dBFS) used to compute the 5th-percentile auto-floor;
    // capacity is sized to ~5 seconds at the readData fragment cadence.
    struct NRState {
        double env = 0.0;                  // envelope follower (linear amplitude)
        double estimatedFloorDb = -60.0;   // last computed auto-floor
        QVector<double> recentEnvs;        // rolling-window dBFS samples
        int recentEnvsHead = 0;            // circular write index
    };
    void setNoiseReductionForTrack(int trackId, const NoiseReductionSettings &nr);
    NoiseReductionSettings noiseReductionForTrack(int trackId) const;
    double estimatedNoiseFloorDb(int trackId) const;

    // Master-bus compressor + brick-wall limiter, applied to the sum-mixed
    // master output after per-track EQ/gain and the loudness normalizer,
    // before s16 clamping. Disabled by default (bit-exact bypass).
    struct CompressorParams {
        double thresholdDb = -12;
        double ratio = 4;
        double attackMs = 10;
        double releaseMs = 120;
        double makeupDb = 0;
        bool enabled = false;
    };
    void setCompressorParams(const CompressorParams &params);
    void setCompressorEnabled(bool enabled);
    CompressorParams compressorParams() const;
    bool compressorEnabled() const;

    // Master loudness normalizer (FCP-style Loudness effect, applied to the
    // sum-mixed master output before s16 clamping).
    //   amount     0..1 — 0 bypasses entirely, 1 = full target-gain follow.
    //   uniformity 0..1 — 0 = slow smoothing (preserves dynamics),
    //                     1 = fast smoothing (uniform output).
    void setNormalizerAmount(double amount);
    void setNormalizerUniformity(double uniformity);

    // Auto-ducking parameters. Drive the gain reduction applied to BGM
    // tracks when the voice track is active. Wired into Timeline's
    // envelope-based applyDuckingFromTrack (duckGain derived from threshold,
    // attack/release passed as seconds).
    struct AutoDuckParams {
        double thresholdDb = -20.0;
        double ratio = 4.0;
        double attackMs = 5.0;
        double releaseMs = 250.0;
    };
    void setAutoDuckParams(const AutoDuckParams &params);
    AutoDuckParams autoDuckParams() const;

signals:
    void decoderError(const QString &message);
    void levelChanged(int trackIdx, float peakL, float peakR, float rmsL, float rmsR);
    void masterLevelChanged(float pkL, float pkR, float rmsL, float rmsR);

private:
    friend class MixerIODevice;
    friend class AudioDecodeRunner;

    struct EqBandCoefs {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };

public:
    struct EqBandCache {
        double frequency = 0;
        double q = 0;
    };

    struct TrackState {
        bool muted = false;
        bool solo = false;
        double gain = 1.0;
        double effectiveGain = 1.0;  // gain * mute factor * solo factor
        bool eqEnabled = false;
        AudioEQConfig eq;
        std::array<EqBandCoefs, 3> eqCoeffs;
        std::array<std::array<double, 4>, 3> z{}; // per-band: z1_L, z1_R, z2_L, z2_R (transposed DF2)
        std::array<EqBandCache, 3> eqCache{};
    };
private:

    bool ensureSinkLocked();              // m_controlMutex must be held
    void recomputeEffectiveGainsLocked(); // m_controlMutex must be held
    void releaseAllEntriesLocked();       // m_controlMutex must be held
    bool openEntry(AudioDecoderEntry *e);
    void closeEntry(AudioDecoderEntry *e);
    void seekEntryToTimeline(AudioDecoderEntry *e, int64_t timelineUs);
    void refillRingForEntry(AudioDecoderEntry *e, int targetBytes);
    void resampleAndAppend(AudioDecoderEntry *e);
    bool refillRings();                   // called by AudioDecodeRunner; returns whether work was done

    QAudioFormat m_format;
    QAudioSink *m_sink = nullptr;
    MixerIODevice *m_io = nullptr;

    QHash<AudioTrackKey, AudioDecoderEntry *> m_entries;
    QVector<TrackState> m_trackStates;
    QVector<speedramp::SpeedRamp> m_speedRamps;  // parallel to setSequence entries
    // US-INT-2 Phase B: hash side-channel for O(1) lookup inside the audio
    // worker's per-fragment loop. m_speedRampKeyOrder is captured during
    // setSequence (in input-vector order); setSpeedRamps walks both vectors
    // in lockstep and rebuilds m_speedRampByKey under m_controlMutex. The
    // QVector m_speedRamps is retained as the storage backing the hash so
    // the existing public API stays QVector-shaped (Phase A compat).
    QVector<AudioTrackKey> m_speedRampKeyOrder;
    QHash<AudioTrackKey, speedramp::SpeedRamp> m_speedRampByKey;
    // PRD-B / US-FIX-7 R10: monotonically increasing generation counter, bumped
    // under m_controlMutex inside setSpeedRamps each time m_speedRampByKey is
    // rebuilt. Decision sites that consult m_speedRampByKey (the readData
    // atempo path and the seekEntryToTimeline R8 gate) capture the generation
    // on entry; if a later compare under-lock observes a mismatch the
    // ramp-dependent branch is silently dropped (no audio side-effect, no
    // log spam) so an in-flight decision based on a stale snapshot cannot
    // commit. Atomic so non-locked observers (e.g. defense-in-depth peek)
    // can still read it lock-free; production decision sites compare under
    // m_controlMutex so the relaxed memory order is sufficient.
    //
    // CRITICAL: this counter is only CONSULTED on the opt-in atempo /
    // non-1x path. The default editor preview (atempo OFF, speed==1.0, no
    // ramp) never enters either consulting branch (audioAtempoEnabledCached()
    // short-circuits and m_speedRampByKey is empty), so byte-identity vs
    // R7/R8/R9 is preserved by construction. The counter itself increments
    // unconditionally inside setSpeedRamps — that is a write to a private
    // atomic with no audible side-effect.
    std::atomic<uint64_t> m_speedRampGeneration{0};

    // 4-band parametric EQ — separate path from TrackState's legacy 3-band.
    // Per-track config + per-channel biquad history (4 bands x 2 channels x
    // 2 history samples = 16 doubles). Coefs cached in m_trackEqCoefs to
    // avoid recomputing inside readData.
    QHash<int, EqSettings> m_trackEq;
    QHash<int, std::array<double, 16>> m_trackEqHist;
    QHash<int, std::array<EqBandCoefsParam, 4>> m_trackEqCoefs;

    // Per-track compressor settings (touched on GUI thread under
    // m_controlMutex) and envelope state (touched on audio worker thread
    // inside readData; the envelope value persists across atomic settings
    // swaps so changing a knob never produces a transient).
    QHash<int, CompressorSettings> m_trackComp;
    QHash<int, CompressorState> m_trackCompState;

    // Per-track reverb settings (touched on GUI thread under
    // m_controlMutex) and DSP state (touched on audio worker thread
    // inside readData; buffers persist across atomic settings swaps so
    // changing a knob never produces a transient).
    //
    // ReverbState layout per track (stereo, kChannels = 2):
    //   preDelay[ch]  — circular buffer, kPreDelayMaxSamples entries
    //   comb[ch][b]   — circular comb buffer (b = 0..3), sized at
    //                   construction from kCombDelays44k1 scaled to
    //                   kSampleRateHz.
    //   combLP[ch][b] — single-sample LP history for HF damping
    //   ap[ch][a]     — circular allpass buffer (a = 0..1)
    // All write indices live in *Idx fields and wrap modulo buffer size.
    static constexpr int kReverbCombCount = 4;
    static constexpr int kReverbAllpassCount = 2;
    static constexpr int kReverbPreDelayMaxSamples = 9600; // 200 ms @ 48k
    struct ReverbState {
        // Per-channel pre-delay buffer (linear, fixed max size).
        std::array<std::array<float, kReverbPreDelayMaxSamples>, kChannels> preDelay{};
        std::array<int, kChannels> preDelayIdx{};
        // Per-channel comb buffers + indices + LP histories.
        std::array<std::array<QVector<float>, kReverbCombCount>, kChannels> comb{};
        std::array<std::array<int, kReverbCombCount>, kChannels> combIdx{};
        std::array<std::array<float, kReverbCombCount>, kChannels> combLP{};
        // Per-channel allpass buffers + indices.
        std::array<std::array<QVector<float>, kReverbAllpassCount>, kChannels> ap{};
        std::array<std::array<int, kReverbAllpassCount>, kChannels> apIdx{};
        bool initialized = false;
    };
    QHash<int, ReverbSettings> m_trackReverb;
    QHash<int, ReverbState> m_trackReverbState;

    // Per-track noise reduction settings (touched on GUI thread under
    // m_controlMutex) and DSP state (touched on audio worker thread inside
    // readData; envelope + recent-envs history persist across atomic
    // settings swaps so live tweaking is glitch-free, mirroring the
    // compressor / reverb stages).
    //
    // Auto-floor uses a rolling 5-second window of envelope-in-dBFS
    // samples; the 5th-percentile of that window becomes the noise-floor
    // estimate. The window is sampled once per readData fragment (≈340 ms
    // worth of audio per kRingTargetBytes), so kAutoFloorWindowSize = 256
    // covers ~85 seconds at the worst case and ~5 seconds at the typical
    // 32-frame fragment cadence — capped on the upper end so memory
    // doesn't grow unbounded.
    static constexpr int kAutoFloorWindowSize = 256;
    QHash<int, NoiseReductionSettings> m_trackNoiseReduction;
    QHash<int, NRState> m_trackNoiseReductionState;

    mutable QMutex m_controlMutex;
    std::atomic<int64_t> m_writeCursorUs{0};
    // Phase 1e Win #13 — Fix K: time-based scrub dedup for seekTo. The
    // existing us-equality + Active + playing early-return (Fix C/F) only
    // skips identical re-seeks. During slider scrub or post-loadFile
    // settling the timeline position changes monotonically every 35-40 ms,
    // so each call wins the dedup but still pays a synchronous QAudioSink
    // stop/start cycle (~15 ms on the main thread). Empirical log
    // veditor_20260501_103732.log @ 10:39:39.673-983 captured 8 such calls
    // in 310 ms, monopolising the GUI thread for ~120 ms and slipping
    // scheduleNextFrame into !advanced auto-pause. The cascade then
    // multiplied via Fix J's 200 ms window (which only catches play()
    // bursts, not seek bursts). Within a 50 ms window we update the cursor,
    // reset the per-entry ring, and wake the decode runner — but skip the
    // sink stop/start. The next seekTo outside the window will run the
    // full path; cursor never lags more than one window.
    QElapsedTimer m_lastSeekToCallTimer;
    // OS-buffered samples in microseconds, published by MixerIODevice::readData
    // so audibleClockUs() is lock-free. Reading m_sink->bytesFree() from the
    // GUI thread under m_controlMutex caused starvation of the audio worker
    // thread (called audibleClockUs every video tick).
    std::atomic<int64_t> m_audibleLagUs{0};
    // Consecutive readData callbacks that wanted to mix an active entry but
    // found its ring empty. Drives the cursor-stall logic so cursor doesn't
    // race past unfilled rings while still self-healing if the decoder is
    // permanently broken.
    std::atomic<int> m_consecutiveStallCallbacks{0};
    std::atomic<bool> m_playing{false};

    // Master compressor state. Params are set from GUI thread under
    // m_controlMutex; readData reads them under the same mutex. The
    // envelope follower state (m_compressorEnv) is touched only from
    // readData on the audio worker thread.
    CompressorParams m_compressorParams;
    double m_compressorEnv = 0.0;

    // Auto-ducking parameters. Set from GUI thread under m_controlMutex;
    // read by ducking menu handler (GUI thread) to drive Timeline's
    // envelope-based applyDuckingFromTrack.
    AutoDuckParams m_autoDuckParams;

    // Bus / submix / aux-send routing (AB-4). Set from the GUI thread via
    // setBusRouting under m_controlMutex; the audio worker thread reads it
    // inside MixerIODevice::readData under the same mutex (resolveTrack-
    // ToMasterGain is a cheap O(bus-count) double accumulation). Default-
    // constructed = no buses, no assignments → resolveTrackToMasterGain
    // returns 1.0 for every track, so the mix path is unchanged (identity).
    audiobus::AudioBusRouting m_busRouting;

    // Master loudness normalizer state. Atomics are touched from the GUI
    // thread (setters) and the audio worker thread (readData). The mutable
    // RMS / smoothed-gain fields are touched only from readData.
    std::atomic<double> m_normalizerAmount{0.0};
    std::atomic<double> m_normalizerUniformity{0.5};
    double m_normalizerRmsMeanSq = 0.0;
    double m_normalizerSmoothedGain = 1.0;

    // Per-track + master level-meter accumulators. readData gathers
    // peak/RMS per fragment and emits levelChanged / masterLevelChanged
    // throttled to <=30 Hz.
    struct LevelAccum {
        float peakL = 0.f;
        float peakR = 0.f;
        double sumSqL = 0.0;
        double sumSqR = 0.0;
        qint64 chanCount = 0;     // per-channel sample count
        void reset() { *this = LevelAccum(); }
    };
    QVector<LevelAccum> m_trackLevelAccum;
    LevelAccum m_masterLevelAccum;
    qint64 m_lastLevelEmitNs = 0;

    AudioDecodeRunner *m_decodeRunner = nullptr;
};
