#include "../AudioMixer.h"
#include "../AudioTrackFx.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../libavcore/AudioExtract.h"
#include "SelftestGate.h"

#include <QByteArray>
#include <QTemporaryDir>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// clip-eq (US-1110): the per-clip 4-band EQ in the clip model, playback and
// the project file.
//   G1: setClipEq addresses the audio clip itself (A2:1 while the V1 selection
//       is index 0; A1:0 stays off, the negative control for the setClipPan
//       pattern of reusing the V1 selection), save -> load keeps it, undo /
//       redo flip it, computeAudioPlaybackSequence carries it, and nested
//       sequences take the child's EQ when it is on, otherwise the parent's.
//   G2: AudioMixer playback applies the clip EQ to A1:0 only: the 1 kHz ratio
//       matches a trackfx::Processor run of the same EQ within 0.5 dB, the
//       3 kHz tone on A2 is unchanged, and turning the EQ off again restores
//       the off PCM byte for byte.
namespace {
constexpr int kRate = 48000;
constexpr double kPi = 3.141592653589793;

trackfx::EqSettings lowMidCut()
{
    trackfx::EqSettings eq;
    eq.lowMid.freq = 1000.0;
    eq.lowMid.gainDb = -12.0;
    eq.lowMid.q = 1.0;
    return eq;
}

bool sameBand(const trackfx::EqBand &a, const trackfx::EqBand &b)
{
    return a.freq == b.freq && a.gainDb == b.gainDb && a.q == b.q && a.enabled == b.enabled;
}

bool sameEq(const trackfx::EqSettings &a, const trackfx::EqSettings &b)
{
    return sameBand(a.low, b.low) && sameBand(a.lowMid, b.lowMid)
        && sameBand(a.highMid, b.highMid) && sameBand(a.high, b.high);
}

ClipInfo clipFor(const QString &path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = path;
    clip.duration = clip.outPoint = 1.0;
    return clip;
}

const PlaybackEntry *findEntry(const QVector<PlaybackEntry> &entries, int track, int clip)
{
    for (const PlaybackEntry &entry : entries)
        if (entry.sourceTrack == track && entry.sourceClipIndex == clip) return &entry;
    return nullptr;
}

std::vector<float> stereoSine(double frequency, double amplitude)
{
    std::vector<float> samples(kRate * 2);
    for (int i = 0; i < kRate; ++i)
        samples[2 * i] = samples[2 * i + 1] =
            float(amplitude * std::sin(2.0 * kPi * frequency * i / kRate));
    return samples;
}

// Amplitude of `frequency` on the left channel over frames [9600, 48000): a
// whole number of 1 kHz and 3 kHz periods, past the 5 ms seam ramp and the
// filter's settling.
double toneAmplitude(const float *interleaved, int frames, double frequency)
{
    constexpr int kStart = 9600;
    if (frames <= kStart) return 0.0;
    std::complex<double> sum;
    for (int i = kStart; i < frames; ++i)
        sum += double(interleaved[2 * i]) * std::polar(1.0, -2.0 * kPi * frequency * i / kRate);
    return 2.0 * std::abs(sum) / (frames - kStart);
}

double pcmToneAmplitude(const QByteArray &pcm, double frequency)
{
    const int frames = int(pcm.size() / AudioMixer::kBytesPerFrame);
    std::vector<float> samples(size_t(frames) * 2);
    for (int i = 0; i < frames * 2; ++i) {
        int16_t value = 0;
        std::memcpy(&value, pcm.constData() + i * int(sizeof(int16_t)), sizeof(value));
        samples[size_t(i)] = value / 32768.0f;
    }
    return toneAmplitude(samples.data(), frames, frequency);
}

double db(double ratio) { return ratio > 0.0 ? 20.0 * std::log10(ratio) : -999.0; }
} // namespace

int runClipEqSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir directory;
    QString error;
    const QString hum = directory.filePath(QStringLiteral("hum1k.wav"));
    const QString high = directory.filePath(QStringLiteral("tone3k.wav"));
    const std::vector<float> humSamples = stereoSine(1000.0, 0.2);
    const bool sourcesOk = directory.isValid()
        && libavcore::writeWavFloatInterleaved(hum, humSamples, kRate, 2, &error)
        && libavcore::writeWavFloatInterleaved(high, stereoSine(3000.0, 0.2), kRate, 2, &error);

    {
        Timeline timeline;
        const QVector<QVector<ClipInfo>> video{
            {clipFor(QStringLiteral("/selftests/clip-eq-v0.mp4")),
             clipFor(QStringLiteral("/selftests/clip-eq-v1.mp4"))}};
        const QVector<QVector<ClipInfo>> audio{{clipFor(hum), clipFor(hum)},
                                               {clipFor(high), clipFor(high)}};
        timeline.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
        timeline.trackAt(false, 0)->setSelectedClip(0);
        timeline.undoManager()->clear();
        timeline.saveUndoState(QStringLiteral("基準"));
        const int v1Selected = timeline.trackAt(false, 0)->selectedClip();
        const QString defaultJson = ProjectFile::toJsonString([&] {
            ProjectData data;
            data.videoTracks = timeline.allVideoTracks();
            data.audioTracks = timeline.allAudioTracks();
            return data;
        }());
        const bool defaultOmitsKey = !defaultJson.isEmpty()
            && !defaultJson.contains(QStringLiteral("\"clipEq\""));

        const trackfx::EqSettings eq = lowMidCut();
        timeline.setClipEq(1, 1, true, eq);
        const auto clipAt = [&](int track, int clip) {
            return timeline.trackAt(true, track)->clips().value(clip);
        };
        const ClipInfo target = clipAt(1, 1);
        const bool targetOn = target.clipEqEnabled && sameEq(target.clipEq, eq);
        // Negative control: the V1 selection (index 0) must not steer the write.
        const bool a1Untouched = !clipAt(0, 0).clipEqEnabled
            && sameEq(clipAt(0, 0).clipEq, trackfx::EqSettings{})
            && !clipAt(0, 1).clipEqEnabled && !clipAt(1, 0).clipEqEnabled;

        ProjectData saved;
        saved.videoTracks = timeline.allVideoTracks();
        saved.audioTracks = timeline.allAudioTracks();
        const QString path = directory.filePath(QStringLiteral("clip-eq.veditor"));
        ProjectData loaded;
        const bool savedOk = ProjectFile::save(path, saved) && ProjectFile::load(path, loaded);
        const QString savedJson = ProjectFile::toJsonString(saved);
        const int keyCount = savedJson.count(QStringLiteral("\"clipEq\""));
        const bool loadedOk = savedOk && loaded.audioTracks.size() == 2
            && loaded.audioTracks[1].size() == 2 && loaded.audioTracks[0].size() == 2
            && loaded.audioTracks[1][1].clipEqEnabled
            && sameEq(loaded.audioTracks[1][1].clipEq, eq)
            && !loaded.audioTracks[0][0].clipEqEnabled
            && !loaded.audioTracks[1][0].clipEqEnabled;

        const QVector<PlaybackEntry> entries = timeline.computeAudioPlaybackSequence();
        const PlaybackEntry *onEntry = findEntry(entries, 1, 1);
        const PlaybackEntry *offEntry = findEntry(entries, 0, 0);
        const bool entryOk = onEntry && offEntry && onEntry->clipEqEnabled
            && sameEq(onEntry->clipEq, eq) && !offEntry->clipEqEnabled;

        timeline.undo();
        const bool undoOff = !clipAt(1, 1).clipEqEnabled;
        timeline.redo();
        const bool redoOn = clipAt(1, 1).clipEqEnabled && sameEq(clipAt(1, 1).clipEq, eq);

        // Nested sequences: the child's EQ when it is on, otherwise the
        // parent's; never both. The child-on case also turns the parent on
        // (with a different EQ) so a parent-wins or chained rule cannot pass.
        trackfx::EqSettings parentEq;
        parentEq.high.gainDb = 6.0;
        struct NestedCase { const char *name; bool childOn; bool parentOn; };
        const NestedCase cases[] = {{"childOn", true, true},
                                    {"parentOnly", false, true},
                                    {"bothOff", false, false}};
        bool nestedOk = true;
        std::string nestedDetail;
        for (const NestedCase &nested : cases) {
            Timeline nestedTimeline;
            ClipInfo child = clipFor(hum);
            child.clipEqEnabled = nested.childOn;
            if (nested.childOn) child.clipEq = eq;
            TimelineSequence childSequence;
            childSequence.id = QStringLiteral("clip-eq-child");
            childSequence.name = childSequence.id;
            childSequence.audioTracks = {{child}};
            const bool added = nestedTimeline.addSequence(childSequence);
            ClipInfo parent = nestedTimeline.makeSequenceClip(childSequence.id);
            parent.clipEqEnabled = nested.parentOn;
            if (nested.parentOn) parent.clipEq = parentEq;
            nestedTimeline.trackAt(true, 0)->setClips({parent});
            const QVector<PlaybackEntry> nestedEntries = nestedTimeline.computeAudioPlaybackSequence();
            const bool one = added && nestedEntries.size() == 1;
            const bool wantOn = nested.childOn || nested.parentOn;
            const trackfx::EqSettings want = nested.childOn ? eq
                : (nested.parentOn ? parentEq : trackfx::EqSettings{});
            const bool ok = one && nestedEntries[0].clipEqEnabled == wantOn
                && sameEq(nestedEntries[0].clipEq, want);
            nestedOk = nestedOk && ok;
            nestedDetail += QString::asprintf(" %s:entries=%d on=%d lowMid=%g high=%g",
                nested.name, int(nestedEntries.size()),
                one ? int(nestedEntries[0].clipEqEnabled) : -1,
                one ? nestedEntries[0].clipEq.lowMid.gainDb : 0.0,
                one ? nestedEntries[0].clipEq.high.gainDb : 0.0).toStdString();
        }

        const std::string detail = QString::asprintf(
            "v1Selected=%d target(A2:1)=%d a1Untouched=%d defaultOmitsKey=%d clipEqKeys=%d "
            "saveLoad=%d entry=%d undoOff=%d redoOn=%d nested=%d |",
            v1Selected, int(targetOn), int(a1Untouched), int(defaultOmitsKey), keyCount,
            int(loadedOk), int(entryOk), int(undoOff), int(redoOn), int(nestedOk)).toStdString()
            + nestedDetail;
        tally.gate(1, v1Selected == 0 && targetOn && a1Untouched && defaultOmitsKey
                   && keyCount == 1 && loadedOk && entryOk && undoOff && redoOn && nestedOk,
                   detail);
    }

    {
        auto mixer = std::make_unique<AudioMixer>();
        Timeline timeline;
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{}},
                                    QVector<QVector<ClipInfo>>{{clipFor(hum)}, {clipFor(high)}},
                                    0.0, -1.0, -1.0, 100);
        mixer->setSequence(timeline.computeAudioPlaybackSequence());
        const QByteArray off = mixer->renderPcmForTest(kRate);

        const trackfx::EqSettings eq = lowMidCut();
        timeline.setClipEq(0, 0, true, eq);
        mixer->setSequence(timeline.computeAudioPlaybackSequence());
        const QByteArray on = mixer->renderPcmForTest(kRate);

        timeline.setClipEq(0, 0, false, eq);
        mixer->setSequence(timeline.computeAudioPlaybackSequence());
        const QByteArray offAgain = mixer->renderPcmForTest(kRate);

        // Expected 1 kHz ratio from the same EQ run through trackfx::Processor
        // on the source tone; sanity band: a 1 kHz peaking cut of -12 dB.
        trackfx::Chain chain;
        chain.eq = eq;
        chain.eqEnabled = true;
        trackfx::Processor processor(chain, kRate, 2);
        std::vector<float> processed = humSamples;
        processor.process(processed.data(), kRate);
        const double expectedDb = db(toneAmplitude(processed.data(), kRate, 1000.0)
                                     / toneAmplitude(humSamples.data(), kRate, 1000.0));
        const bool expectedSane = expectedDb > -12.5 && expectedDb < -11.5;

        const bool sizes = off.size() == kRate * AudioMixer::kBytesPerFrame
            && on.size() == off.size() && offAgain.size() == off.size();
        const double off1k = sizes ? pcmToneAmplitude(off, 1000.0) : 0.0;
        const double on1k = sizes ? pcmToneAmplitude(on, 1000.0) : 0.0;
        const double off3k = sizes ? pcmToneAmplitude(off, 3000.0) : 0.0;
        const double on3k = sizes ? pcmToneAmplitude(on, 3000.0) : 0.0;
        const double ratio1kDb = db(on1k / off1k);
        const double ratio3kDb = db(on3k / off3k);
        const bool restored = sizes && offAgain == off;
        const std::string detail = QString::asprintf(
            "sources=%d bytes=%lld/%lld/%lld off1k=%g on1k=%g ratio1k=%.3fdB expected=%.3fdB "
            "off3k=%g on3k=%g ratio3k=%.4fdB offRestoredByteEqual=%d %s",
            int(sourcesOk), static_cast<long long>(off.size()), static_cast<long long>(on.size()),
            static_cast<long long>(offAgain.size()), off1k, on1k, ratio1kDb, expectedDb,
            off3k, on3k, ratio3kDb, int(restored), qPrintable(error)).toStdString();
        tally.gate(2, sourcesOk && sizes && expectedSane && off1k > 0.1 && off3k > 0.1
                   && std::abs(ratio1kDb - expectedDb) <= 0.5
                   && std::abs(ratio3kDb) <= 0.05 && restored, detail);
    }

    return tally.finish("clip-eq");
}
