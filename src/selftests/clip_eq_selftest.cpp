#include "../AudioClipEditor.h"
#include "../AudioMixer.h"
#include "../AudioTrackFx.h"
#include "../MainWindow.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../libavcore/AudioExtract.h"
#include "SelftestGate.h"

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QSlider>
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
// US-1111: the EQ panel of AudioClipEditor, opened for audio clip (track, clip)
// through Timeline::audioClipEqRequested on a real MainWindow. Band 1 = lowMid,
// Band 2 = highMid, Band 3 = low shelf, Band 4 = high shelf. The overload hides
// the volume / pan row and disconnects those handlers (they would write into
// A1 through the V1 selection).
//   G3: A2:1 opened, clipEqEnable ON (+1 undo point) and clipEqBand1Gain -12
//       (+1 undo point): A2:1 on with lowMid -12, A1:0 volume / pan / EQ
//       unchanged, and a volumeChanged / panChanged emitted by the editor does
//       not reach A1:0 (negative control for the V1-selection write path).
//   G4: undo twice returns to off, redo twice to -12 dB.
//   G5: reopening another clip reloads that clip's EQ and rebinds the target
//       (a Band edit lands on the reopened clip only). Controls with the editor
//       open: a track move (the edit follows the clip's uid), a new sequence
//       holding a same-uid copy (the copy is not written, the panel hides) and
//       a same-uid project reload (the editor is unbound and hidden, so an edit
//       through its old widgets cannot write the stale EQ back).
//   G6: negative control: with the checkbox OFF a Band1 gain edit stores the
//       value but clipEqEnabled stays false; ON flips it.
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

    // G3-G6: the editor panel on a real MainWindow.
    {
        MainWindow window;
        window.hide();
        Timeline *timeline = window.m_timeline;
        const QVector<QVector<ClipInfo>> video{
            {clipFor(QStringLiteral("/selftests/clip-eq-v0.mp4")),
             clipFor(QStringLiteral("/selftests/clip-eq-v1.mp4"))}};
        QVector<ClipInfo> a1{clipFor(hum), clipFor(hum)};
        a1[0].volume = 0.8;
        a1[0].pan = 0.3;
        timeline->restoreFromProject(video, QVector<QVector<ClipInfo>>{a1, {clipFor(high), clipFor(high)}},
                                     0.0, -1.0, -1.0, 100);
        timeline->trackAt(false, 0)->setSelectedClip(0);
        timeline->undoManager()->clear();
        timeline->saveUndoState(QStringLiteral("基準"));
        const auto clipAt = [&](int track, int clip) {
            return timeline->trackAt(true, track)->clips().value(clip);
        };
        const auto serial = [&]() { return timeline->undoManager()->saveSerial(); };
        const auto open = [&](int track, int clip) {
            emit timeline->audioClipEqRequested(track, clip);
            return window.m_audioClipEditorDialog;
        };
        const auto find = [](QDialog *dialog, const char *name) {
            return dialog ? dialog->findChild<QDoubleSpinBox *>(QString::fromLatin1(name)) : nullptr;
        };
        const auto enableOf = [](QDialog *dialog) {
            return dialog ? dialog->findChild<QCheckBox *>(QStringLiteral("clipEqEnable")) : nullptr;
        };

        // G3
        const quint64 serial0 = serial();
        QDialog *dialog = open(1, 1);
        QCheckBox *enable = enableOf(dialog);
        QDoubleSpinBox *gain1 = find(dialog, "clipEqBand1Gain");
        const bool widgets = enable && gain1 && find(dialog, "clipEqBand1Freq")
            && find(dialog, "clipEqBand1Q") && find(dialog, "clipEqBand2Gain")
            && find(dialog, "clipEqBand3Gain") && find(dialog, "clipEqBand4Gain");
        if (enable) enable->setChecked(true);
        const quint64 serialOn = serial();
        if (gain1) gain1->setValue(-12.0);
        const quint64 serialGain = serial();
        const ClipInfo a2 = clipAt(1, 1);
        const bool a2On = a2.clipEqEnabled && a2.clipEq.lowMid.gainDb == -12.0;
        // The overload hides volume / pan and cuts the handlers.
        AudioClipEditor *editor = dialog ? dialog->findChild<AudioClipEditor *>(QStringLiteral("audioClipEditor")) : nullptr;
        const QSlider *volumeSlider = dialog ? dialog->findChild<QSlider *>(QStringLiteral("clipVolumeSlider")) : nullptr;
        const QSlider *panSlider = dialog ? dialog->findChild<QSlider *>(QStringLiteral("clipPanSlider")) : nullptr;
        const bool volPanHidden = volumeSlider && panSlider && !volumeSlider->isVisibleTo(dialog)
            && !panSlider->isVisibleTo(dialog);
        const bool noKeyTracking = gain1 && !gain1->keyboardTracking();
        if (editor) {
            emit editor->volumeChanged(0.1);
            emit editor->panChanged(-0.9);
        }
        const ClipInfo a1First = clipAt(0, 0);
        const bool a1Same = a1First.volume == 0.8 && a1First.pan == 0.3 && !a1First.clipEqEnabled
            && sameEq(a1First.clipEq, trackfx::EqSettings{});
        // The panel must fit its themed contents: the last spin row stays inside it.
        QApplication::processEvents();
        QWidget *eqPanel = dialog ? dialog->findChild<QWidget *>(QStringLiteral("clipEqPanel")) : nullptr;
        const QDoubleSpinBox *q4 = find(dialog, "clipEqBand4Q");
        const int panelH = eqPanel ? eqPanel->height() : -1;
        const int panelMinH = eqPanel ? eqPanel->minimumSizeHint().height() : 1 << 20;
        const int q4Bottom = (eqPanel && q4) ? q4->mapTo(eqPanel, q4->rect().bottomLeft()).y() : 1 << 20;
        const int contentBottom = eqPanel ? eqPanel->contentsRect().bottom() : -1;
        const bool panelFits = eqPanel && eqPanel->isVisible() && panelH >= panelMinH && q4Bottom <= contentBottom;
        tally.gate(3, widgets && serialOn == serial0 + 1 && serialGain == serial0 + 2 && a2On
                   && volPanHidden && noKeyTracking && a1Same && panelFits,
                   QString::asprintf("widgets=%d serial=%llu/%llu/%llu a2On=%d lowMidGain=%g volPanHidden=%d "
                                     "noKeyTracking=%d panelFits=%d panelH=%d panelMinH=%d q4Bottom=%d contentBottom=%d "
                                     "a1Vol=%g a1Pan=%g a1EqOn=%d",
                                     int(widgets), static_cast<unsigned long long>(serial0),
                                     static_cast<unsigned long long>(serialOn),
                                     static_cast<unsigned long long>(serialGain), int(a2On),
                                     a2.clipEq.lowMid.gainDb, int(volPanHidden), int(noKeyTracking), int(panelFits),
                                     panelH, panelMinH, q4Bottom, contentBottom, a1First.volume,
                                     a1First.pan, int(a1First.clipEqEnabled)).toStdString());

        // G4
        timeline->undo();
        timeline->undo();
        const bool undone = !clipAt(1, 1).clipEqEnabled && clipAt(1, 1).clipEq.lowMid.gainDb == 0.0;
        // The still-open dialog follows the restored state.
        const bool undoneWidgets = enable && gain1 && !enable->isChecked() && gain1->value() == 0.0;
        const int undoneOn = enable ? int(enable->isChecked()) : -1;
        const double undoneGain = gain1 ? gain1->value() : -999.0;
        timeline->redo();
        timeline->redo();
        const bool redone = clipAt(1, 1).clipEqEnabled && clipAt(1, 1).clipEq.lowMid.gainDb == -12.0;
        const bool redoneWidgets = enable && gain1 && enable->isChecked() && gain1->value() == -12.0;
        tally.gate(4, undone && redone && undoneWidgets && redoneWidgets,
                   QString::asprintf("undoneOff=%d redoneOn=%d undoneWidgets=%d (on=%d gain=%g) "
                                     "redoneWidgets=%d (on=%d gain=%g)",
                                     int(undone), int(redone), int(undoneWidgets), undoneOn, undoneGain,
                                     int(redoneWidgets), enable ? int(enable->isChecked()) : -1,
                                     gain1 ? gain1->value() : -999.0).toStdString());

        // G5
        trackfx::EqSettings other;
        other.highMid.gainDb = 5.0;
        timeline->setClipEq(1, 0, true, other);
        dialog = open(1, 0);
        const bool loadedOther = enableOf(dialog) && enableOf(dialog)->isChecked()
            && find(dialog, "clipEqBand2Gain") && find(dialog, "clipEqBand2Gain")->value() == 5.0
            && find(dialog, "clipEqBand1Gain") && find(dialog, "clipEqBand1Gain")->value() == 0.0;
        if (find(dialog, "clipEqBand1Gain")) find(dialog, "clipEqBand1Gain")->setValue(3.0);
        const bool rebound = clipAt(1, 0).clipEq.lowMid.gainDb == 3.0
            && clipAt(1, 1).clipEq.lowMid.gainDb == -12.0;
        dialog = open(1, 1);
        const bool reloaded = enableOf(dialog) && enableOf(dialog)->isChecked()
            && find(dialog, "clipEqBand1Gain") && find(dialog, "clipEqBand1Gain")->value() == -12.0
            && find(dialog, "clipEqBand2Gain") && find(dialog, "clipEqBand2Gain")->value() == 0.0;
        // Control: with A2:1 open, the track order changes; the edit must follow the clip (now at
        // track 0) and leave the clip now at the old index alone.
        QString movedUid = clipAt(1, 1).uid;
        QDoubleSpinBox *movedGain = find(dialog, "clipEqBand1Gain");
        const bool moved = timeline->moveTrack(true, 1, 0);
        const bool uidAtNewPos = !movedUid.isEmpty() && clipAt(0, 1).uid == movedUid;
        if (movedGain) movedGain->setValue(7.0);
        const bool followed = clipAt(0, 1).uid == movedUid && clipAt(0, 1).clipEq.lowMid.gainDb == 7.0;
        const bool oldIdxUntouched = clipAt(1, 1).uid != movedUid
            && !clipAt(1, 1).clipEqEnabled && clipAt(1, 1).clipEq.lowMid.gainDb == 0.0;
        const double followedGain = clipAt(0, 1).clipEq.lowMid.gainDb;
        const double oldIdxGain = clipAt(1, 1).clipEq.lowMid.gainDb;
        timeline->moveTrack(true, 0, 1);
        // Control: with A2:1 open, "create sequence" copies the clip (same uid) into a new active
        // sequence whose A1:0 is that copy. The edit must not reach the copy, and the panel hides.
        QString origSeq = timeline->activeSequenceId();
        if (origSeq.isEmpty()) origSeq = QStringLiteral("main");  // kDefaultSequenceId
        const double baseGain = clipAt(1, 1).clipEq.lowMid.gainDb;
        const bool seqMade = timeline->createSequence(QStringLiteral("eqcopy"), {{}}, {{clipAt(1, 1)}});
        const QString activeAfterCreate = timeline->activeSequenceId();
        QDoubleSpinBox *seqGain = find(dialog, "clipEqBand1Gain");
        if (seqGain) seqGain->setValue(9.0);
        QApplication::processEvents();
        QWidget *seqPanel = dialog ? dialog->findChild<QWidget *>(QStringLiteral("clipEqPanel")) : nullptr;
        const bool panelHiddenOnForeign = seqPanel && !seqPanel->isVisibleTo(dialog);
        const double copyGain = clipAt(0, 0).clipEq.lowMid.gainDb;
        const bool copyUntouched = seqMade && activeAfterCreate != origSeq && copyGain == baseGain;
        timeline->setActiveSequence(origSeq);
        QApplication::processEvents();
        const double origGain = clipAt(1, 1).clipEq.lowMid.gainDb;
        const bool seqOk = copyUntouched && panelHiddenOnForeign && origGain == baseGain;
        // Control: with A2:1 open at ON / -12, a project load restores a same-uid A2:1 at OFF / 0.
        // The load must unbind and hide the editor, so an edit through its old widgets cannot write
        // the cached ON / -12 back over the restored clip.
        if (QDoubleSpinBox *prep = find(dialog, "clipEqBand1Gain")) prep->setValue(-12.0);
        const bool reloadPrep = clipAt(1, 1).clipEqEnabled && clipAt(1, 1).clipEq.lowMid.gainDb == -12.0;
        const QString reloadUid = clipAt(1, 1).uid;
        ProjectData reload;
        window.populateProjectData(reload);
        bool reloadSameUid = false;
        if (reload.audioTracks.size() > 1 && reload.audioTracks[1].size() > 1
            && !reloadUid.isEmpty() && reload.audioTracks[1][1].uid == reloadUid) {
            reload.audioTracks[1][1].clipEqEnabled = false;
            reload.audioTracks[1][1].clipEq = trackfx::EqSettings{};
            reloadSameUid = true;
        }
        window.m_promptForMissingMedia = false;
        window.applyLoadedProjectData(reload, QString());
        QDialog *reloadDialog = window.m_audioClipEditorDialog;
        if (QDoubleSpinBox *stale = find(reloadDialog, "clipEqBand2Gain")) stale->setValue(2.0);
        const ClipInfo afterReload = clipAt(1, 1);
        const bool reloadEqOn = afterReload.clipEqEnabled;
        const double reloadGain = afterReload.clipEq.lowMid.gainDb;
        QWidget *reloadPanel = reloadDialog ? reloadDialog->findChild<QWidget *>(QStringLiteral("clipEqPanel")) : nullptr;
        const bool reloadHidden = reloadDialog && (reloadDialog->isHidden()
                                                   || (reloadPanel && !reloadPanel->isVisibleTo(reloadDialog)));
        const bool reloadOk = reloadPrep && reloadSameUid && afterReload.uid == reloadUid && !reloadEqOn
            && reloadGain == 0.0 && reloadHidden;
        tally.gate(5, loadedOther && rebound && reloaded && moved && uidAtNewPos && followed && oldIdxUntouched
                   && seqOk && reloadOk,
                   QString::asprintf("loadedOther=%d reboundToReopened=%d reloadedA2_1=%d moved=%d "
                                     "uidAtNewPos=%d followedGain=%g oldIdxUntouched=%d (oldIdxGain=%g) "
                                     "seqMade=%d activeAfterCreate=%s origSeq=%s baseGain=%g copyGain=%g "
                                     "copyUntouched=%d origGain=%g panelHiddenOnForeign=%d seqOk=%d "
                                     "reloadPrep=%d reloadUid=%s reloadSameUid=%d reloadEqOn=%d reloadGain=%g "
                                     "reloadHidden=%d",
                                     int(loadedOther), int(rebound), int(reloaded), int(moved),
                                     int(uidAtNewPos), followedGain,
                                     int(oldIdxUntouched), oldIdxGain, int(seqMade), qPrintable(activeAfterCreate),
                                     qPrintable(origSeq), baseGain, copyGain, int(copyUntouched), origGain,
                                     int(panelHiddenOnForeign), int(seqOk), int(reloadPrep), qPrintable(reloadUid),
                                     int(reloadSameUid && afterReload.uid == reloadUid), int(reloadEqOn), reloadGain,
                                     int(reloadHidden)).toStdString());

        // G6
        dialog = open(0, 0);
        QDoubleSpinBox *offGain = find(dialog, "clipEqBand1Gain");
        const bool startsOff = enableOf(dialog) && !enableOf(dialog)->isChecked();
        if (offGain) offGain->setValue(6.0);
        const bool storedButOff = !clipAt(0, 0).clipEqEnabled && clipAt(0, 0).clipEq.lowMid.gainDb == 6.0;
        if (enableOf(dialog)) enableOf(dialog)->setChecked(true);
        const bool controlOn = clipAt(0, 0).clipEqEnabled;
        tally.gate(6, startsOff && storedButOff && controlOn,
                   QString::asprintf("startsOff=%d storedGain6ButOff=%d onAfterToggle=%d",
                                     int(startsOff), int(storedButOff), int(controlOn)).toStdString());
    }

    return tally.finish("clip-eq");
}
