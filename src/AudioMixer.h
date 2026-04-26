#pragma once

#include <QObject>
#include <QVector>
#include <QString>
#include <QHash>
#include <QMutex>
#include <QIODevice>
#include <QAudioSink>
#include <QAudioFormat>
#include <atomic>

#include "PlaybackTypes.h"

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
    static constexpr int kRingTargetBytes = 64 * 1024;         // ~340 ms stereo s16 @ 48k
    static constexpr int kPrerollLeadUs = 2'000'000;           // pre-warm 2 s before entry start

    explicit AudioMixer(QObject *parent = nullptr);
    ~AudioMixer() override;

    // Replace the active timeline schedule. Opens decoders for new entries
    // and releases decoders for entries no longer present. Safe from GUI
    // thread; locks m_controlMutex briefly.
    void setSequence(const QVector<PlaybackEntry> &entries);

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

signals:
    void decoderError(const QString &message);

private:
    friend class MixerIODevice;
    friend class AudioDecodeRunner;

    struct TrackState {
        bool muted = false;
        bool solo = false;
        double gain = 1.0;
        double effectiveGain = 1.0;  // gain * mute factor * solo factor
    };

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

    mutable QMutex m_controlMutex;
    std::atomic<int64_t> m_writeCursorUs{0};
    std::atomic<bool> m_playing{false};

    AudioDecodeRunner *m_decodeRunner = nullptr;
};
