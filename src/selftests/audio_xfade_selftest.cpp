#include "../Timeline.h"
#include "../UndoManager.h"
#include "../AudioMixer.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QJsonObject>
#include <tuple>

#include <QApplication>

#include <cmath>
#include <cstdio>

// AudioMixer.cpp keeps this QIODevice class private to its translation unit.
// Repeat its token-identical definition here (C++ ODR), linking readData to the
// production implementation. This permits deterministic PCM reads without an
// audio device and without changing AudioMixer's public API or implementation.
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

namespace {

ClipInfo testClip(const QString &name)
{
    ClipInfo clip;
    clip.filePath = name;
    clip.displayName = name;
    clip.duration = 4.0;
    clip.outPoint = 4.0;
    return clip;
}

bool sameTransition(const Transition &left, const Transition &right)
{
    return left.type == right.type
        && std::abs(left.duration - right.duration) < 1e-9;
}

bool sameClipIdentity(const QVector<ClipInfo> &left,
                      const QVector<ClipInfo> &right)
{
    if (left.size() != right.size())
        return false;
    for (int i = 0; i < left.size(); ++i) {
        if (left[i].filePath != right[i].filePath
            || left[i].leadIn.type != right[i].leadIn.type
            || left[i].trailOut.type != right[i].trailOut.type
            || std::abs(left[i].leadIn.duration - right[i].leadIn.duration) >= 1e-9
            || std::abs(left[i].trailOut.duration - right[i].trailOut.duration) >= 1e-9) {
            return false;
        }
    }
    return true;
}

bool transitionsAreNone(const QVector<ClipInfo> &clips)
{
    for (const ClipInfo &clip : clips) {
        if (clip.leadIn.type != TransitionType::None
            || clip.trailOut.type != TransitionType::None) {
            return false;
        }
    }
    return true;
}

bool saveAudioBaseline(Timeline &timeline,
                       const QVector<ClipInfo> &clips)
{
    TimelineTrack *track = timeline.trackAt(true, 0);
    if (!track || !timeline.undoManager())
        return false;
    track->setClips(clips);
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(
        timeline.currentState(), QStringLiteral("audio-xfade selftest baseline"));
    return true;
}

QVector<ClipInfo> handleClips()
{
    auto a = testClip(QStringLiteral("handle-a"));
    auto b = testClip(QStringLiteral("handle-b"));
    a.duration = b.duration = 8.0;
    a.outPoint = 5.0;
    b.inPoint = 1.0;
    b.outPoint = 6.0;
    return {a, b};
}

bool near(double a, double b) { return std::abs(a - b) <= 0.001; }

// Explicit value comparison: never memcmp Qt objects or struct padding.
bool sameEntry(const PlaybackEntry &a, const PlaybackEntry &b)
{
    const auto fields = [](const PlaybackEntry &e) {
        return std::tie(e.filePath, e.clipIn, e.clipOut, e.timelineStart,
            e.timelineEnd, e.speed, e.sourceTrack, e.audioMuted,
            e.videoScale, e.videoDx, e.videoDy, e.rotation2DDegrees,
            e.opacity, e.isVfxFootage, e.blendMode, e.vfxIntensity,
            e.vfxBlackLevel, e.fitContain, e.fitCover, e.colorMeta,
            e.volume, e.pan, e.sourceClipIndex, e.matteTypeOrdinal,
            e.matteSourceClipId, e.parentClipId, e.leadInType,
            e.leadInDuration, e.leadInEasing, e.trailOutType,
            e.trailOutDuration, e.trailOutEasing);
    };
    if (fields(a) != fields(b) || a.layerStyle.toJson() != b.layerStyle.toJson()
        || a.volumeEnvelope.size() != b.volumeEnvelope.size()
        || a.stabilizerKeyframes.size() != b.stabilizerKeyframes.size())
        return false;
    for (int i = 0; i < a.volumeEnvelope.size(); ++i) {
        if (a.volumeEnvelope[i].time != b.volumeEnvelope[i].time
            || a.volumeEnvelope[i].gain != b.volumeEnvelope[i].gain)
            return false;
    }
    for (int i = 0; i < a.stabilizerKeyframes.size(); ++i) {
        const auto &x = a.stabilizerKeyframes[i];
        const auto &y = b.stabilizerKeyframes[i];
        if (std::tie(x.timeUs, x.dx, x.dy, x.theta, x.scale)
            != std::tie(y.timeUs, y.dx, y.dy, y.theta, y.scale))
            return false;
    }
    return true;
}

// MainWindow's exportAudioFilterChainForEntry is translation-unit private.
// Exercise its public production delegate with the resolved entry arguments.
QString entryFilter(const PlaybackEntry &e, int index)
{
    return buildExportAudioMixEntryFilterChain(index,
        QString::number(e.clipIn, 'f', 6), QString::number(e.clipOut, 'f', 6),
        qMax(0, qRound(e.timelineStart * 1000.0)),
        QString::number(e.volume, 'f', 6), AudioChannelMode::Stereo, false,
        e.speed, e.leadInType, e.leadInDuration, e.trailOutType, e.trailOutDuration);
}

bool writeTone(const QString &path, double hz)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 frames = 8 * 48000;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + frames * 2);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(48000)
        << quint32(96000) << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(frames * 2);
    for (quint32 i = 0; i < frames; ++i)
        out << qint16(std::lround(8192.0 * std::sin(6.283185307179586 * hz * i / 48000.0)));
    return out.status() == QDataStream::Ok && file.flush();
}

// Explicit-instantiation access is confined to this test: transport is set
// without starting a hardware sink, and decoder refill uses the production
// method under its own mutex. No layout casts or alternate mixing code.
template<class Tag, typename Tag::Type Member>
struct MixerTestAccess {
    friend typename Tag::Type mixerMember(Tag) { return Member; }
};
struct PlayingMember {
    using Type = std::atomic<bool> AudioMixer::*;
    friend Type mixerMember(PlayingMember);
};
struct RefillMember {
    using Type = bool (AudioMixer::*)();
    friend Type mixerMember(RefillMember);
};
template struct MixerTestAccess<PlayingMember, &AudioMixer::m_playing>;
template struct MixerTestAccess<RefillMember, &AudioMixer::refillRings>;

double mixerWindowRms(const QVector<PlaybackEntry> &entries, double center)
{
    AudioMixer mixer;
    mixer.setSequence(entries);
    const qint64 startUs = qRound64((center - 0.05) * 1000000.0);
    mixer.seekTo(startUs);
    (mixer.*mixerMember(PlayingMember{})).store(true);
    MixerIODevice io(&mixer);
    io.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    // refillRings budgets one pending seek per call; warm both decoders and
    // fill their rings before sampling, independent of worker scheduling.
    for (int pass = 0; pass < 16; ++pass)
        (mixer.*mixerMember(RefillMember{}))();
    // Exactly 100 ms, split into 10 ms reads to cross both clip boundaries
    // with the same refill/accumulate path used by preview's audio sink.
    double sumSquares = 0.0;
    int sampleCount = 0;
    for (int block = 0; block < 10; ++block) {
        (mixer.*mixerMember(RefillMember{}))();
        const QByteArray pcm = io.read(480 * AudioMixer::kBytesPerFrame);
        if (pcm.size() != 480 * AudioMixer::kBytesPerFrame
            || mixer.masterClockUs() != startUs + (block + 1) * 10000) {
            mixer.stop();
            return 0.0;
        }
        for (int i = 0; i < pcm.size(); i += 2) {
            const double value = qFromLittleEndian<qint16>(
                reinterpret_cast<const uchar *>(pcm.constData() + i)) / 32768.0;
            sumSquares += value * value;
            ++sampleCount;
        }
    }
    mixer.stop();
    return std::sqrt(sumSquares / sampleCount);
}

void gate(int number, const char *description, bool ok, int &passed, int &failed)
{
    std::fprintf(stderr, "%s G%d %s\n", ok ? "PASS" : "FAIL", number,
                 description);
    ok ? ++passed : ++failed;
}

} // namespace

int runAudioXfadeSelftest()
{
    int passed = 0;
    int failed = 0;

    Timeline timeline;
    TimelineTrack *audioTrack = timeline.trackAt(true, 0);
    const QVector<ClipInfo> before{testClip(QStringLiteral("A")),
                                   testClip(QStringLiteral("B"))};
    const Transition expectedCrossfade{
        TransitionType::CrossDissolve, 1.25,
        TransitionAlignment::Center, TransitionEasing::Linear};
    QString error;
    const bool baselineReady = saveAudioBaseline(timeline, before);
    const bool crossfadeApplied = baselineReady
        && timeline.applyAudioCrossfade(0, 0, 1.25, &error);
    const QVector<ClipInfo> crossfadeState = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool crossfadeMutation = crossfadeApplied
        && crossfadeState.size() == 2
        && sameTransition(crossfadeState[0].trailOut, expectedCrossfade)
        && sameTransition(crossfadeState[1].leadIn, expectedCrossfade)
        && crossfadeState[0].leadIn.type == TransitionType::None
        && crossfadeState[1].trailOut.type == TransitionType::None
        && timeline.canUndo();
    if (crossfadeApplied)
        timeline.undo();
    const QVector<ClipInfo> crossfadeRestored = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool crossfadeUndo = crossfadeApplied
        && sameClipIdentity(crossfadeRestored, before)
        && transitionsAreNone(crossfadeRestored)
        && !timeline.canUndo();
    gate(1, "applyAudioCrossfade pairs A.trailOut/B.leadIn and one undo restores it",
         crossfadeMutation && crossfadeUndo, passed, failed);

    const QString legacy = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("1"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, false, 1.0);
    const QString fadeOff = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("1"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, false, 1.0,
        TransitionType::None, 0.0, TransitionType::None, 0.0);
    const QString fadeOn = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("1"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, false, 1.0,
        TransitionType::FadeIn, 0.75, TransitionType::FadeOut, 0.5);
    const QString reversedFade = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("1"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, true, 2.0,
        TransitionType::FadeIn, 0.75);
    const int reversePos = reversedFade.indexOf(QStringLiteral("areverse"));
    const int atempoPos = reversedFade.indexOf(QStringLiteral("atempo="));
    const int afadePos = reversedFade.indexOf(QStringLiteral("afade="));
    const bool g2 = legacy == fadeOff
        && fadeOn.contains(QStringLiteral("afade=t=in"))
        && fadeOn.contains(QStringLiteral("afade=t=out"))
        && fadeOn.contains(QStringLiteral("curve=qsin"))
        && reversePos >= 0 && atempoPos > reversePos && afadePos > atempoPos;
    gate(2, "export keeps the no-fade chain byte-identical and emits qsin afade",
         g2, passed, failed);

    const QVector<ClipInfo> fadeBefore{testClip(QStringLiteral("fade"))};
    const bool fadeInBaseline = saveAudioBaseline(timeline, fadeBefore);
    const bool fadeIn = fadeInBaseline
        && timeline.applyAudioFade(0, 0, AudioFadeEdge::In, 0.4, &error);
    const QVector<ClipInfo> fadeInState = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool fadeInMutation = fadeIn
        && fadeInState.size() == 1
        && fadeInState[0].leadIn.type == TransitionType::FadeIn
        && std::abs(fadeInState[0].leadIn.duration - 0.4) < 1e-9
        && fadeInState[0].trailOut.type == TransitionType::None
        && timeline.canUndo();
    if (fadeIn)
        timeline.undo();
    const QVector<ClipInfo> fadeInRestored = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool fadeInUndo = fadeIn
        && sameClipIdentity(fadeInRestored, fadeBefore)
        && transitionsAreNone(fadeInRestored)
        && !timeline.canUndo();

    const bool fadeOutBaseline = saveAudioBaseline(timeline, fadeBefore);
    const bool fadeOut = fadeOutBaseline
        && timeline.applyAudioFade(0, 0, AudioFadeEdge::Out, 0.6, &error);
    const QVector<ClipInfo> fadeOutState = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool fadeOutMutation = fadeOut
        && fadeOutState.size() == 1
        && fadeOutState[0].trailOut.type == TransitionType::FadeOut
        && std::abs(fadeOutState[0].trailOut.duration - 0.6) < 1e-9
        && fadeOutState[0].leadIn.type == TransitionType::None
        && timeline.canUndo();
    if (fadeOut)
        timeline.undo();
    const QVector<ClipInfo> fadeOutRestored = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool fadeOutUndo = fadeOut
        && sameClipIdentity(fadeOutRestored, fadeBefore)
        && transitionsAreNone(fadeOutRestored)
        && !timeline.canUndo();
    gate(3, "applyAudioFade sets each requested edge and one undo restores it",
         fadeInMutation && fadeInUndo && fadeOutMutation && fadeOutUndo,
         passed, failed);

    QVector<ClipInfo> nonAdjacent{testClip(QStringLiteral("A")),
                                  testClip(QStringLiteral("B"))};
    error.clear();
    const bool nonAdjacentRejected = !audioxfade::applyCrossfade(
        nonAdjacent, 1, 1.0, &error) && !error.isEmpty();
    gate(4, "non-adjacent audio crossfade is rejected without mutation",
         nonAdjacentRejected
             && nonAdjacent[0].trailOut.type == TransitionType::None
             && nonAdjacent[1].leadIn.type == TransitionType::None,
         passed, failed);

    Timeline mirrorTimeline;
    TimelineTrack *videoTrack = mirrorTimeline.trackAt(false, 0);
    TimelineTrack *mirrorAudioTrack = mirrorTimeline.trackAt(true, 0);
    const QVector<ClipInfo> videoBefore{testClip(QStringLiteral("video-a")),
                                        testClip(QStringLiteral("video-b"))};
    const QVector<ClipInfo> audioBefore{testClip(QStringLiteral("audio-a")),
                                        testClip(QStringLiteral("audio-b"))};
    if (videoTrack)
        videoTrack->setClips(videoBefore);
    if (mirrorAudioTrack)
        mirrorAudioTrack->setClips(audioBefore);
    if (videoTrack)
        videoTrack->setSelectedClip(0);
    const Transition mirrorTransition{
        TransitionType::CrossDissolve, 1.0,
        TransitionAlignment::Center, TransitionEasing::Linear};
    if (videoTrack && mirrorAudioTrack)
        mirrorTimeline.applyTransitionToSelected(mirrorTransition);
    const QVector<ClipInfo> mirroredAudio = mirrorAudioTrack
        ? mirrorAudioTrack->clips() : QVector<ClipInfo>{};
    gate(5, "applyTransitionToSelected mirrors CrossDissolve to the A1 cut",
         mirroredAudio.size() == 2
             && sameTransition(mirroredAudio[0].trailOut, mirrorTransition)
             && sameTransition(mirroredAudio[1].leadIn, mirrorTransition)
             && mirroredAudio[0].leadIn.type == TransitionType::None
             && mirroredAudio[1].trailOut.type == TransitionType::None,
         passed, failed);

    // G1..G5 intentionally retain their original handle-less fixtures.
    Timeline overlapTimeline;
    saveAudioBaseline(overlapTimeline, handleClips());
    const bool applied = overlapTimeline.applyAudioCrossfade(0, 0, 1.0, &error);
    const auto overlap = overlapTimeline.computeAudioPlaybackSequence();
    gate(6, "Center consumes audio source handles without changing total duration",
         applied && overlap.size() == 2 && near(overlap[0].clipOut, 5.5)
         && near(overlap[1].clipIn, 0.5) && near(overlap[0].timelineEnd, 5.5)
         && near(overlap[1].timelineStart, 4.5) && near(overlap[1].timelineEnd, 10.0),
         passed, failed);

    Timeline unchanged;
    auto plain = handleClips();
    plain[0].leadIn = {TransitionType::FadeIn, 0.4};
    plain[1].trailOut = {TransitionType::FadeOut, 0.3};
    auto zero = testClip(QStringLiteral("zero"));
    zero.inPoint = zero.outPoint = 4.0;
    zero.leadInSec = 0.75;
    auto missing = testClip(QStringLiteral("veditor://sequence/missing"));
    missing.sequenceRefId = QStringLiteral("missing");
    plain.insert(1, zero);
    plain.append(missing);
    auto wipePair = handleClips();
    wipePair[0].trailOut = {TransitionType::WipeLeft, 1.0};
    wipePair[1].leadIn = {TransitionType::WipeLeft, 1.0};
    plain += wipePair;
    // Empty URI reference makes appendSequenceAudioEntries return false;
    // its ordinary-entry fallthrough must survive the two-pass conversion.
    const auto unresolved = testClip(QStringLiteral("veditor://sequence/"));
    plain.append(unresolved);
    saveAudioBaseline(unchanged, plain);
    Timeline::setAudioOverlapEnabledForTest(false);
    const auto disabled = unchanged.computeAudioPlaybackSequence();
    bool identical = Timeline::audioOverlapCallCountForTest() == 0;
    Timeline::setAudioOverlapEnabledForTest(true);
    const auto enabled = unchanged.computeAudioPlaybackSequence();
    identical &= Timeline::audioOverlapCallCountForTest() == 0
        && enabled.size() == disabled.size();
    for (int i = 0; identical && i < enabled.size(); ++i)
        identical = sameEntry(enabled[i], disabled[i])
            && entryFilter(enabled[i], i) == entryFilter(disabled[i], i);
    gate(7, "no-overlap entries/all fields and production export chains are identical",
         identical && enabled.size() == 5 && near(enabled[1].timelineStart, 5.75)
         && enabled.last().filePath == unresolved.filePath
         && near(enabled.last().timelineStart, 24.75),
         passed, failed);

    auto noTrail = handleClips();
    noTrail[0].duration = noTrail[0].outPoint;
    saveAudioBaseline(overlapTimeline, noTrail);
    overlapTimeline.applyAudioCrossfade(0, 0, 1.0, &error);
    const auto borrowed = overlapTimeline.computeAudioPlaybackSequence();
    Timeline videoHandles;
    noTrail[0].trailOut = mirrorTransition;
    noTrail[1].leadIn = mirrorTransition;
    videoHandles.trackAt(false, 0)->setClips(noTrail);
    const auto videoIntervals = videoHandles.videoOverlapIntervals();
    gate(8, "missing trail handle borrows End-at-Cut identically to video solver",
         borrowed.size() == 2 && near(borrowed[0].timelineEnd, 5.0)
         && near(borrowed[1].timelineStart, 4.0)
         && !videoIntervals.isEmpty() && videoIntervals[0].size() == 2
         && near(borrowed[0].timelineEnd - borrowed[1].timelineStart,
                 videoIntervals[0][0].timelineEnd - videoIntervals[0][1].timelineStart),
         passed, failed);

    {
        QTemporaryDir dir(QDir::temp().filePath(QStringLiteral("audio-xfade-XXXXXX")));
        auto tones = handleClips();
        tones[0].filePath = dir.filePath(QStringLiteral("440.wav"));
        tones[1].filePath = dir.filePath(QStringLiteral("660.wav"));
        const bool wavs = dir.isValid() && writeTone(tones[0].filePath, 440.0)
            && writeTone(tones[1].filePath, 660.0);
        saveAudioBaseline(overlapTimeline, tones);
        overlapTimeline.applyAudioCrossfade(0, 0, 1.0, &error);
        const auto mixed = overlapTimeline.computeAudioPlaybackSequence();
        Timeline::setAudioOverlapEnabledForTest(false);
        const auto oldSchedule = overlapTimeline.computeAudioPlaybackSequence();
        Timeline::setAudioOverlapEnabledForTest(true);
        const double steady = wavs ? mixerWindowRms(mixed, 2.0) : 0.0;
        const double seam = wavs ? mixerWindowRms(mixed, 5.0) : 0.0;
        const double oldSeam = wavs ? mixerWindowRms(oldSchedule, 5.0) : 0.0;
        const double db = steady > 0 && seam > 0 ? 20 * std::log10(seam / steady) : -100;
        const double oldDb = steady > 0 && oldSeam > 0 ? 20 * std::log10(oldSeam / steady) : -100;
        std::fprintf(stderr, "G9 seam=%g dB disabled=%g dB\n", db, oldDb);
        gate(9, "decoded 440/660Hz mixer seam RMS has no dip; disabled branch dips",
             wavs && steady > 0 && seam > 0 && oldSeam > 0
             && db >= -1.0 && db <= 3.5 && oldDb <= -3.0, passed, failed);
    } // delete both WAVs after all mixer/decoder instances have closed them

    bool nestedOk = true;
    for (int variant = 0; variant < 4; ++variant) {
        const bool reverseParent = (variant & 1) != 0;
        const double parentSpeed = variant < 2 ? 1.0 : 2.0;
        Timeline nestedTimeline;
        auto children = handleClips();
        children[0].reversed = children[1].reversed = true;
        // Reverse A needs a trailing handle below inPoint.
        children[0].inPoint = 1.0;
        children[0].outPoint = 6.0;
        children[0].trailOut = mirrorTransition;
        children[1].leadIn = mirrorTransition;
        TimelineSequence childSequence;
        childSequence.id = QStringLiteral("audio-child");
        childSequence.name = QStringLiteral("音声子シーケンス");
        childSequence.audioTracks = {children};
        nestedOk &= nestedTimeline.addSequence(childSequence);
        auto parent = nestedTimeline.makeSequenceClip(childSequence.id);
        parent.reversed = reverseParent;
        parent.speed = parentSpeed;
        nestedTimeline.trackAt(true, 0)->setClips({parent});
        const auto entries = nestedTimeline.computeAudioPlaybackSequence();
        nestedOk &= entries.size() == 2;
        if (entries.size() == 2) {
            nestedOk &= near(entries[0].timelineEnd - entries[1].timelineStart, 1.0 / parentSpeed)
                && near(entries[0].trailOutDuration, 1.0 / parentSpeed)
                && near(entries[1].leadInDuration, 1.0 / parentSpeed)
                && entries[0].trailOutType == TransitionType::CrossDissolve
                && entries[1].leadInType == TransitionType::CrossDissolve
                && entries[0].leadInType == TransitionType::None
                && entries[1].trailOutType == TransitionType::None
                && near(entries[1].timelineEnd, 10.0 / parentSpeed)
                && entries[0].filePath == children[reverseParent ? 1 : 0].filePath;
        }
    }
    gate(10, "nested reversed sources and reversed parent preserve overlap and trailing edge",
         nestedOk, passed, failed);

    Timeline linked;
    auto linkedVideo = handleClips();
    auto linkedAudio = handleClips();
    for (int i = 0; i < 2; ++i) {
        linkedVideo[i].linkGroup = linkedAudio[i].linkGroup = i + 1;
        // Deliberately different source duration: audio owns its handles.
        linkedVideo[i].duration = 12.0;
    }
    linked.trackAt(false, 0)->setClips(linkedVideo);
    linked.trackAt(true, 0)->setClips(linkedAudio);
    linked.trackAt(false, 0)->setSelectedClip(0);
    linked.applyTransitionToSelected(mirrorTransition);
    const auto va = linked.videoOverlapIntervals();
    const auto aa = linked.computeAudioPlaybackSequence();
    gate(11, "linked V/A mirrored dissolve starts B at the same timeline position",
         aa.size() == 2 && !va.isEmpty() && va[0].size() == 2
         && near(aa[1].timelineStart, 4.5)
         && near(aa[1].timelineStart, va[0][1].timelineStart), passed, failed);

    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
