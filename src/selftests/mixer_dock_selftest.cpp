#include "../AudioBusRouting.h"
#include "../AudioMixerDock.h"
#include "../AudioMeterWidget.h"
#include "../AudioMixer.h"
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"
#include "../RenderInPlace.h"
#include "../libavcore/AudioExtract.h"
#include "SelftestGate.h"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {
void baseline(Timeline &timeline)
{
    timeline.undoManager()->clear();
    timeline.saveUndoState(QStringLiteral("基準"));
}
void wire(Timeline &timeline, AudioMixer &mixer)
{
    timeline.setAudioMixer(&mixer);
    QObject::connect(&timeline, &Timeline::trackIndicesRemapped, &mixer,
                     [&mixer](bool audio, const QVector<int> &map) {
        if (audio) mixer.remapTrackIndices(map);
    });
    QObject::connect(&timeline, &Timeline::trackSoloChanged, &mixer, &AudioMixer::setTrackSolo);
    baseline(timeline);
}
QSlider *fader(AudioMixerDock &dock, int index = 0)
{
    return dock.findChild<QSlider *>(QStringLiteral("mixerFader%1").arg(index));
}
QPushButton *button(AudioMixerDock &dock, const char *kind, int index = 0)
{
    return dock.findChild<QPushButton *>(QStringLiteral("mixer%1%2").arg(QString::fromLatin1(kind)).arg(index));
}
bool events(Timeline &timeline, AudioMixerDock &dock, AudioMixer &mixer)
{
    // Wait for observable convergence, including the debounced header -> Dock
    // update. The timeout is a failure bound, not a minimum settling delay.
    // Never call dock.synchronize() here: that would bypass the wiring under test.
    const bool ready = QTest::qWaitFor([&]() {
        QCoreApplication::processEvents();
        // Rebuilt strips are deleted later; retire them before findChild/counts
        // so a stale strip cannot satisfy the condition or mask the current one.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const int count = timeline.audioTrackCount();
        if (dock.stripCount() != count)
            return false;
        for (int i = 0; i < count; ++i) {
            const auto *track = timeline.trackAt(true, i);
            const auto *slider = fader(dock, i);
            const auto *mute = button(dock, "Mute", i);
            const auto *solo = button(dock, "Solo", i);
            const auto *strip = dock.findChild<QWidget *>(QStringLiteral("mixerStrip%1").arg(i));
            const auto *meter = dock.findChild<AudioMeterWidget *>(QStringLiteral("mixerMeter%1").arg(i));
            const auto *label = dock.findChild<QLabel *>(QStringLiteral("mixerName%1").arg(i));
            if (!track || !slider || !mute || !solo || !strip || !meter || !label)
                return false;
            const double gain = mixer.trackGain(i);
            const int position = gain <= 0.0 ? -601
                : qBound(-600, qRound(200.0 * std::log10(gain)), 120);
            if (slider->value() != position || mute->isChecked() != track->isMuted()
                || solo->isChecked() != track->isSolo())
                return false;
            const QString name = track->customName.isEmpty()
                ? QStringLiteral("A%1").arg(i + 1) : track->customName;
            if (label->text() != name || label->parentWidget() != strip
                || meter->parentWidget() != strip) return false;
        }
        return true;
    }, 2000);
    if (!ready)
        std::fprintf(stderr, "mixer-dock: timed out waiting for strip/model synchronization\n");
    return ready;
}
QPushButton *headerButton(Timeline &timeline, const char *kind, int index)
{
    const QRegularExpression name(QStringLiteral("^timelineTrack%1Button$")
        .arg(QString::fromLatin1(kind)));
    const auto buttons = timeline.findChildren<QPushButton *>(name);
    return index >= 0 && index < buttons.size() ? buttons.at(index) : nullptr;
}
void drag(QSlider *slider, int position)
{
    slider->setSliderDown(true);
    slider->setValue(position);
    slider->setSliderDown(false);
}
void fixture(Timeline &timeline, const QString &source)
{
    ClipInfo clip;
    clip.filePath = source;
    clip.duration = clip.outPoint = 1.0;
    const QVector<QVector<ClipInfo>> video{{}};
    const QVector<QVector<ClipInfo>> audio{{clip}};
    timeline.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
}
double pcmEnergy(const QByteArray &pcm)
{
    // Ignore the production 5 ms seam ramp; retain a fixed middle interval.
    if (pcm.size() < 48000 * AudioMixer::kBytesPerFrame) return 0.0;
    double sum = 0.0;
    for (int i = 4800; i < 40000; ++i) {
        int16_t value = 0;
        std::memcpy(&value, pcm.constData() + i * AudioMixer::kBytesPerFrame, sizeof(value));
        const double sample = value / 32768.0;
        sum += sample * sample;
    }
    return sum / 35200.0;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
// 4800 interleaved stereo frames of a 1 kHz sine at 48 kHz; the right channel
// is a quarter period ahead so a channel swap cannot match.
std::vector<int16_t> sineBlock(double amplitude)
{
    std::vector<int16_t> block(4800 * 2);
    for (int i = 0; i < 4800; ++i) {
        const double phase = 2.0 * 3.141592653589793 * 1000 * i / 48000;
        block[2 * i] = int16_t(std::lround(amplitude * 32767 * std::sin(phase)));
        block[2 * i + 1] = int16_t(std::lround(amplitude * 32767 * std::cos(phase)));
    }
    return block;
}
// Largest |tap - source/32768| over the frames both hold; 1.0 when the frame
// counts differ so a short or long take cannot pass.
double tapError(const QVector<float> &tap, const int16_t *source, int frames)
{
    if (tap.size() != frames * 2) return 1.0;
    double worst = 0.0;
    for (int i = 0; i < frames * 2; ++i)
        worst = std::max(worst, std::abs(double(tap[i]) - source[i] / 32768.0));
    return worst;
}

// G6 forwards every message to the handler main() installed, counting the
// wildcard-disconnect warnings on the way so the gate proves they reached it.
QtMessageHandler g_productionHandler = nullptr;
std::atomic<int> g_wildcardWarnings{0};
void countWildcardWarning(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (message.contains(QLatin1String("wildcard call disconnects from destroyed signal")))
        ++g_wildcardWarnings;
    g_productionHandler(type, context, message);
}
}

int runMixerDockSelftest()
{
    selftest::GateTally tally;
    const auto gate = [&](int n, bool ok) { tally.gate(n, ok); };

    {
        auto mixer = std::make_unique<AudioMixer>();
        Timeline timeline;
        wire(timeline, *mixer);
        AudioMixerDock dock(&timeline, mixer.get());
        bool ok = dock.stripCount() == timeline.audioTrackCount();
        timeline.addAudioTrack();
        timeline.setTrackCustomName(true, 0, QStringLiteral("音声"));
        timeline.setTrackCustomName(true, 1, QStringLiteral("音楽"));
        ok = events(timeline, dock, *mixer) && ok;
        ok = ok && dock.stripCount() == 2
            && dock.findChild<AudioMeterWidget *>("mixerMeter0")
            && dock.findChild<AudioMeterWidget *>("mixerMeter1");
        mixer->setTrackGain(1, 0.5);
        ok = timeline.moveTrack(true, 1, 0) && ok;
        ok = events(timeline, dock, *mixer) && ok;
        auto *strip = dock.findChild<QWidget *>(QStringLiteral("mixerStrip0"));
        auto *label = strip ? strip->findChild<QLabel *>("mixerName0") : nullptr;
        const bool named = label && label->text() == QStringLiteral("音楽");
        ok = ok && named && fader(dock) && fader(dock)->value() == -60;
        ok = timeline.removeTrack(true, 1) && ok;
        ok = events(timeline, dock, *mixer) && ok;
        ok = ok && dock.stripCount() == 1;
        timeline.undo();
        ok = events(timeline, dock, *mixer) && ok;
        gate(1, ok && dock.stripCount() == 2);
    }

    QTemporaryDir directory;
    const QString source = directory.filePath(QStringLiteral("tone.wav"));
    std::vector<float> samples(48000 * 2);
    for (int i = 0; i < 48000; ++i)
        samples[2 * i] = samples[2 * i + 1] = float(0.2 * std::sin(2.0 * 3.141592653589793 * 1000 * i / 48000));
    QString error;
    const bool sourceOk = directory.isValid()
        && libavcore::writeWavFloatInterleaved(source, samples, 48000, 2, &error);
    {
        auto mixer = std::make_unique<AudioMixer>();
        Timeline timeline;
        wire(timeline, *mixer);
        fixture(timeline, source);
        baseline(timeline);
        mixer->setSequence(timeline.computeAudioPlaybackSequence());
        AudioMixerDock dock(&timeline, mixer.get());
        const auto dry = mixer->renderPcmForTest(48000);
        const double dryEnergy = pcmEnergy(dry);
        auto *slider = fader(dock);
        if (slider) drag(slider, -60);
        const double expected = std::pow(10.0, -6.0 / 20.0);
        const auto wet = mixer->renderPcmForTest(48000);
        const double wetEnergy = pcmEnergy(wet);
        const QString output = directory.filePath(QStringLiteral("gain.wav"));
        const bool exported = sourceOk && renderinplace::prepareAudioMix(&timeline, output, 1.0, &error) == output;
        std::vector<double> mono;
        int rate = 0;
        bool ok = exported && libavcore::readPcm16WavToMono(output, mono, rate, &error)
            && rate == 48000 && mono.size() >= 40000;
        double exportEnergy = 0.0;
        if (ok) {
            for (int i = 4800; i < 40000; ++i) exportEnergy += mono[i] * mono[i];
            exportEnergy /= 35200.0;
        }
        ok = ok && slider && near(mixer->trackGain(0), expected)
            && near(timeline.currentState().audioTrackGains[0], expected)
            && dryEnergy > 0.001
            && std::abs(std::sqrt(wetEnergy / dryEnergy) - expected) < 0.01
            && std::abs(std::sqrt(exportEnergy / dryEnergy) - expected) < 0.01;
        std::fprintf(stderr, "mixer-dock gain: %s dry=%g playback=%g export=%g dryBytes=%lld playbackBytes=%lld\n",
                     qPrintable(error), dryEnergy, wetEnergy, exportEnergy,
                     static_cast<long long>(dry.size()), static_cast<long long>(wet.size()));
        gate(2, ok);
    }
    {
        auto mixer = std::make_unique<AudioMixer>();
        Timeline timeline;
        wire(timeline, *mixer);
        timeline.addAudioTrack();
        AudioMixerDock dock(&timeline, mixer.get());
        const auto serial = timeline.undoManager()->saveSerial();
        auto *mute = headerButton(timeline, "Mute", 0);
        auto *solo = headerButton(timeline, "Solo", 0);
        bool ok = mute && solo && button(dock, "Mute") && button(dock, "Solo");
        if (ok) {
            button(dock, "Mute")->click();
            ok = mute->isChecked() && timeline.trackAt(true, 0)->isMuted();
            mute->click();
            ok = events(timeline, dock, *mixer) && ok;
            ok = ok && !button(dock, "Mute")->isChecked();
            button(dock, "Solo")->click();
            ok = ok && solo->isChecked() && timeline.trackAt(true, 0)->isSolo();
            solo->click();
            ok = events(timeline, dock, *mixer) && ok;
            ok = ok && !button(dock, "Solo")->isChecked();
            button(dock, "Solo", 1)->click();
            ok = ok && !button(dock, "Solo")->isChecked()
                && timeline.trackAt(true, 1)->isSolo();
        }
        gate(3, ok && timeline.undoManager()->saveSerial() == serial);
    }
    {
        auto mixer = std::make_unique<AudioMixer>();
        Timeline timeline;
        wire(timeline, *mixer);
        // Completed preceding gain edit (the normalize convention).
        mixer->setTrackGain(0, 0.8);
        timeline.saveUndoState(QStringLiteral("ノーマライズ"));
        AudioMixerDock dock(&timeline, mixer.get());
        auto *slider = fader(dock);
        const auto serial = timeline.undoManager()->saveSerial();
        bool ok = slider;
        if (slider) {
            slider->setSliderDown(true);
            for (int value : {-20, -40, -60}) slider->setValue(value);
            ok = timeline.undoManager()->saveSerial() == serial;
            slider->setSliderDown(false);
            const double finalGain = mixer->trackGain(0);
            ok = ok && timeline.undoManager()->saveSerial() == serial + 1;
            timeline.undo();
            ok = ok && near(mixer->trackGain(0), 0.8);
            timeline.redo();
            ok = ok && near(mixer->trackGain(0), finalGain);
            timeline.undo();
            timeline.undo();
            ok = ok && near(mixer->trackGain(0), 1.0);
            ok = events(timeline, dock, *mixer) && ok;
            const auto noOpSerial = timeline.undoManager()->saveSerial();
            slider->setSliderDown(true);
            slider->setSliderDown(false);
            ok = ok && timeline.undoManager()->saveSerial() == noOpSerial;
            slider->setValue(-120); // keyboard/wheel value change contract
            ok = ok && timeline.undoManager()->saveSerial() == noOpSerial + 1;
            timeline.undo();
            ok = ok && near(mixer->trackGain(0), 1.0);
        }
        gate(4, ok);
    }
    {
        auto control = std::make_unique<AudioMixer>();
        auto mixer = std::make_unique<AudioMixer>();
        Timeline timeline;
        wire(timeline, *mixer);
        fixture(timeline, source);
        baseline(timeline);
        const auto entries = timeline.computeAudioPlaybackSequence();
        mixer->setSequence(entries);
        control->setSequence(entries);
        const auto state = mixer->collectTrackState();
        const auto serial = timeline.undoManager()->saveSerial();
        // Establish audible production output before constructing the Dock,
        // so a constructor-side playback regression cannot hide as silence.
        const auto beforeDock = mixer->renderPcmForTest(48000);
        const auto controlBefore = control->renderPcmForTest(48000);
        AudioMixerDock dock(&timeline, mixer.get());
        dock.synchronize();
        const bool synchronized = events(timeline, dock, *mixer);
        const auto actual = mixer->renderPcmForTest(48000);
        const auto untouched = control->renderPcmForTest(48000);
        const bool hidden = dock.isHidden() && !dock.toggleViewAction()->isChecked();
        const bool noWrites = dock.gainWriteCountForTest() == 0;
        const bool stateSame = mixer->collectTrackState() == state;
        const bool undoSame = timeline.undoManager()->saveSerial() == serial;
        const bool pcmSame = beforeDock == controlBefore && actual == beforeDock
            && actual == untouched;
        std::fprintf(stderr, "mixer-dock hidden: before=%g after=%g control=%g hidden=%d noWrites=%d stateSame=%d undoSame=%d pcmSame=%d\n",
                     pcmEnergy(beforeDock), pcmEnergy(actual), pcmEnergy(untouched),
                     int(hidden), int(noWrites), int(stateSame), int(undoSame), int(pcmSame));
        gate(5, sourceOk && synchronized && hidden && noWrites && stateSame && undoSame
             && pcmEnergy(beforeDock) > 0.001 && pcmEnergy(actual) > 0.001 && pcmSame);
    }
    {
        // Qt reports a wildcard disconnect of a connected destroyed() signal
        // (Timeline::remapTrackIndices: track->disconnect()) while it holds the
        // sender's signal/slot pool mutex. The installed message handler must not
        // take a pool mutex itself (QTextStream connects its device): with this
        // many distinct sender addresses one of them shares the handler's pool
        // slot, so such a handler hangs here (watchdog TIMEOUT) instead of passing.
        constexpr int kSenders = 400;
        QObject context;
        std::vector<std::unique_ptr<QObject>> senders;
        std::vector<QMetaObject::Connection> connections;
        for (int i = 0; i < kSenders; ++i) {
            senders.push_back(std::make_unique<QObject>());
            connections.push_back(QObject::connect(senders.back().get(), &QObject::destroyed,
                                                   &context, [] {}));
        }
        int connected = 0;
        for (const auto &connection : connections) connected += bool(connection) ? 1 : 0;
        g_productionHandler = qInstallMessageHandler(nullptr);
        const bool handlerInstalled = g_productionHandler != nullptr;
        if (handlerInstalled) qInstallMessageHandler(countWildcardWarning);
        g_wildcardWarnings = 0;
        for (const auto &sender : senders) sender->disconnect();
        qInstallMessageHandler(g_productionHandler);
        int stillConnected = 0;
        for (const auto &connection : connections) stillConnected += QObject::disconnect(connection) ? 1 : 0;
        std::fprintf(stderr, "mixer-dock handler-lock: senders=%d connected=%d handlerInstalled=%d warnings=%d stillConnected=%d\n",
                     kSenders, connected, int(handlerInstalled), g_wildcardWarnings.load(), stillConnected);
        gate(6, handlerInstalled && connected == kSenders && g_wildcardWarnings == kSenders
             && stillConnected == 0);
    }
    constexpr double kTapTolerance = 1.0 / 32768.0;
    {
        // Published blocks come back in order, one coalesced notification
        // covers both, and readData itself feeds the tap with its output.
        auto mixer = std::make_unique<AudioMixer>();
        QSignalSpy spy(mixer.get(), &AudioMixer::masterTapAvailable);
        mixer->subscribeMasterTap();
        const auto first = sineBlock(0.5);
        const auto second = sineBlock(0.25);
        QVector<float> tapFirst, tapSecond;
        mixer->publishMasterBlockForTest(first.data(), 4800);
        const int takenFirst = mixer->takeMasterTap(&tapFirst, AudioMixer::kMasterTapFrames);
        mixer->publishMasterBlockForTest(second.data(), 4800);
        const int takenSecond = mixer->takeMasterTap(&tapSecond, AudioMixer::kMasterTapFrames);
        const double error = std::max(tapError(tapFirst, first.data(), 4800),
                                      tapError(tapSecond, second.data(), 4800));
        // The two blocks differ, so a reordered take could not match.
        const double swapped = tapError(tapFirst, second.data(), 4800);
        const bool notified = QTest::qWaitFor([&] { return spy.count() >= 1; }, 2000);
        QCoreApplication::processEvents();
        const int emits = spy.count();

        auto played = std::make_unique<AudioMixer>();
        Timeline timeline;
        wire(timeline, *played);
        fixture(timeline, source);
        played->setSequence(timeline.computeAudioPlaybackSequence());
        played->subscribeMasterTap();
        const auto pcm = played->renderPcmForTest(9600);
        QVector<float> tapPlayed;
        const int takenPlayed = played->takeMasterTap(&tapPlayed, 9600);
        const int kept = AudioMixer::kMasterTapFrames;
        const auto *pcmTail = pcm.size() == 9600 * AudioMixer::kBytesPerFrame
            ? reinterpret_cast<const int16_t *>(pcm.constData()) + (9600 - kept) * 2 : nullptr;
        const double playError = pcmTail ? tapError(tapPlayed, pcmTail, kept) : 1.0;
        double playPeak = 0.0;
        for (float value : tapPlayed) playPeak = std::max(playPeak, std::abs(double(value)));
        const std::string detail = QString::asprintf(
            "taken=%d+%d maxError=%g swappedError=%g notified=%d emits=%d | readData: pcmBytes=%lld taken=%d maxError=%g peak=%g copies=%llu",
            takenFirst, takenSecond, error, swapped, int(notified), emits,
            static_cast<long long>(pcm.size()), takenPlayed, playError, playPeak,
            static_cast<unsigned long long>(played->tapCopyCountForTest())).toStdString();
        tally.gate(7, takenFirst == 4800 && takenSecond == 4800 && error <= kTapTolerance
                   && swapped > 0.01 && notified && emits == 1
                   && sourceOk && takenPlayed == kept && playError <= kTapTolerance
                   && playPeak > 0.05, detail);
    }
    {
        // Negative control: without subscribers neither the test entry nor
        // readData copies. The control mixer proves the same calls do copy
        // once subscribed, and that the last unsubscribe discards the ring.
        auto mixer = std::make_unique<AudioMixer>();
        const auto block = sineBlock(0.5);
        QVector<float> tap;
        mixer->publishMasterBlockForTest(block.data(), 4800);
        const int idleFrames = mixer->takeMasterTap(&tap, AudioMixer::kMasterTapFrames);
        const quint64 idleCopies = mixer->tapCopyCountForTest();
        mixer->subscribeMasterTap();
        mixer->unsubscribeMasterTap();
        mixer->publishMasterBlockForTest(block.data(), 4800);
        const int unsubscribedFrames = mixer->takeMasterTap(&tap, AudioMixer::kMasterTapFrames);
        const quint64 unsubscribedCopies = mixer->tapCopyCountForTest();

        auto control = std::make_unique<AudioMixer>();
        control->subscribeMasterTap();
        control->publishMasterBlockForTest(block.data(), 4800);
        const quint64 controlCopies = control->tapCopyCountForTest();
        control->unsubscribeMasterTap();
        const int discarded = control->takeMasterTap(&tap, AudioMixer::kMasterTapFrames);
        control->subscribeMasterTap();
        control->publishMasterBlockForTest(block.data(), 4800);
        const int controlFrames = control->takeMasterTap(&tap, AudioMixer::kMasterTapFrames);

        auto played = std::make_unique<AudioMixer>();
        Timeline timeline;
        wire(timeline, *played);
        fixture(timeline, source);
        played->setSequence(timeline.computeAudioPlaybackSequence());
        const double playEnergy = pcmEnergy(played->renderPcmForTest(48000));
        const int playFrames = played->takeMasterTap(&tap, AudioMixer::kMasterTapFrames);
        const quint64 playCopies = played->tapCopyCountForTest();
        const std::string detail = QString::asprintf(
            "idle frames=%d copies=%llu | unsubscribed frames=%d copies=%llu | control copies=%llu discarded=%d frames=%d | readData energy=%g frames=%d copies=%llu",
            idleFrames, static_cast<unsigned long long>(idleCopies),
            unsubscribedFrames, static_cast<unsigned long long>(unsubscribedCopies),
            static_cast<unsigned long long>(controlCopies), discarded, controlFrames,
            playEnergy, playFrames, static_cast<unsigned long long>(playCopies)).toStdString();
        tally.gate(8, idleFrames == 0 && idleCopies == 0 && unsubscribedFrames == 0
                   && unsubscribedCopies == 0 && controlCopies == 1 && discarded == 0
                   && controlFrames == 4800 && sourceOk && playEnergy > 0.001
                   && playFrames == 0 && playCopies == 0, detail);
    }
    {
        // Three 4800-frame blocks overflow the 8192-frame ring: the oldest
        // frames are dropped. Each frame carries its own index, so the take
        // names the first frame it kept.
        auto mixer = std::make_unique<AudioMixer>();
        mixer->subscribeMasterTap();
        constexpr int kTotal = 3 * 4800;
        std::vector<int16_t> stream(kTotal * 2);
        for (int i = 0; i < kTotal; ++i) {
            stream[2 * i] = int16_t(i - 7200);
            stream[2 * i + 1] = int16_t(7200 - i);
        }
        for (int block = 0; block < 3; ++block)
            mixer->publishMasterBlockForTest(stream.data() + block * 4800 * 2, 4800);
        QVector<float> tap;
        const int taken = mixer->takeMasterTap(&tap, kTotal);
        const int kept = AudioMixer::kMasterTapFrames;
        const double error = tapError(tap, stream.data() + (kTotal - kept) * 2, kept);
        const int firstFrame = tap.isEmpty() ? -1 : int(std::lround(tap[0] * 32768.0)) + 7200;
        QVector<float> rest;
        const int remaining = mixer->takeMasterTap(&rest, kTotal);
        const std::string detail = QString::asprintf(
            "capacity=%d taken=%d firstFrame=%d expectedFirst=%d maxError=%g remaining=%d copies=%llu",
            kept, taken, firstFrame, kTotal - kept, error, remaining,
            static_cast<unsigned long long>(mixer->tapCopyCountForTest())).toStdString();
        tally.gate(9, kept == 8192 && taken == kept && firstFrame == kTotal - kept
                   && error <= kTapTolerance && remaining == 0
                   && mixer->tapCopyCountForTest() == 3, detail);
    }
    {
        // A decoder stall returns a zeroed block to the sink without moving the
        // clock; the tap must receive that silence too, or the spectrum keeps
        // showing the previous block. Detects the stall return skipping the tap
        // (0 frames, copy delta 0). An advancing clock means the stall branch
        // was not taken, which fails rather than passing vacuously.
        auto mixer = std::make_unique<AudioMixer>();
        Timeline timeline;
        wire(timeline, *mixer);
        fixture(timeline, source);
        mixer->setSequence(timeline.computeAudioPlaybackSequence());
        mixer->subscribeMasterTap();
        const auto tone = sineBlock(0.5);
        mixer->publishMasterBlockForTest(tone.data(), 4800);
        QVector<float> tap;
        const int drained = mixer->takeMasterTap(&tap, AudioMixer::kMasterTapFrames);
        const quint64 copiesBefore = mixer->tapCopyCountForTest();
        const qint64 clockBefore = mixer->masterClockUs();
        const auto pcm = mixer->pullStalledBlockForTest(480);
        const qint64 clockAfter = mixer->masterClockUs();
        const quint64 copyDelta = mixer->tapCopyCountForTest() - copiesBefore;
        const int taken = mixer->takeMasterTap(&tap, AudioMixer::kMasterTapFrames);
        double maxAbs = 0.0;
        for (float value : tap) maxAbs = std::max(maxAbs, std::abs(double(value)));
        const std::string detail = QString::asprintf(
            "drained=%d pcmBytes=%lld taken=%d maxAbs=%g clockBefore=%lld clockAfter=%lld copyDelta=%llu",
            drained, static_cast<long long>(pcm.size()), taken, maxAbs,
            static_cast<long long>(clockBefore), static_cast<long long>(clockAfter),
            static_cast<unsigned long long>(copyDelta)).toStdString();
        tally.gate(10, sourceOk && drained == 4800 && pcm.size() == 480 * AudioMixer::kBytesPerFrame
                   && clockAfter == clockBefore && copyDelta == 1 && taken == 480
                   && maxAbs == 0.0, detail);
    }
    {
        // G11-G12: pan slider and bus combo of a real MainWindow's Dock. The Dock
        // only requests; MainWindow owns the routing (SSOT) and the undo points.
        MainWindow window;
        window.hide();
        Timeline *timeline = window.m_timeline;
        AudioMixer *mixer = window.m_player->audioMixer();
        auto *dock = window.findChild<AudioMixerDock *>();
        if (dock && timeline && timeline->audioTrackCount() == 0) timeline->addAudioTrack();
        if (dock) dock->synchronize();
        const bool wired = dock && mixer && timeline->audioMixer() == mixer && dock->stripCount() >= 1;
        const auto serial = [&]() { return timeline->undoManager()->saveSerial(); };
        QSlider *pan = wired ? dock->findChild<QSlider *>(QStringLiteral("mixerPan0")) : nullptr;
        QLabel *panText = wired ? dock->findChild<QLabel *>(QStringLiteral("mixerPanLabel0")) : nullptr;
        if (wired) baseline(*timeline);
        {
            const quint64 serial0 = serial();
            bool ok = wired && pan && panText && pan->minimum() == -100 && pan->maximum() == 100;
            if (ok) {
                // Near the centre a mouse drag snaps to 0: no write, no undo point.
                drag(pan, -3);
                const bool snapped = pan->value() == 0 && mixer->trackPan(0) == 0.0
                    && serial() == serial0;
                // One gesture with several moves: no undo point while held, exactly one on release.
                pan->setSliderDown(true);
                pan->setValue(-40);
                pan->setValue(-70);
                pan->setValue(-100);
                const bool heldSilent = serial() == serial0;
                pan->setSliderDown(false);
                const bool applied = heldSilent && near(mixer->trackPan(0), -1.0) && serial() == serial0 + 1
                    && panText->text() == QStringLiteral("L100");
                timeline->undo();
                const bool undone = near(mixer->trackPan(0), 0.0) && pan->value() == 0;
                timeline->redo();
                const bool redone = near(mixer->trackPan(0), -1.0) && pan->value() == -100;
                std::fprintf(stderr, "mixer-dock pan: snapped=%d heldSilent=%d applied=%d undone=%d redone=%d serial=%llu->%llu\n",
                             int(snapped), int(heldSilent), int(applied), int(undone), int(redone),
                             static_cast<unsigned long long>(serial0),
                             static_cast<unsigned long long>(serial()));
                ok = snapped && applied && undone && redone;
            }
            gate(11, ok);
        }
        {
            bool ok = wired;
            QComboBox *combo = nullptr;
            int busB = -1;
            if (ok) {
                window.m_audioBusRouting.addBus(QStringLiteral("バスA"));
                busB = window.m_audioBusRouting.addBus(QStringLiteral("バスB"));
                window.onAudioBusRoutingChanged();
                baseline(*timeline);
                combo = dock->findChild<QComboBox *>(QStringLiteral("mixerBus0"));
                ok = combo && busB == 1 && combo->count() == 3
                    && combo->itemText(0) == QStringLiteral("マスター")
                    && combo->itemText(1) == QStringLiteral("バスA")
                    && combo->itemText(2) == QStringLiteral("バスB")
                    && window.m_audioBusRouting.trackBus(0) == -1;
            }
            if (ok) {
                const quint64 serial0 = serial();
                combo->setCurrentIndex(2);
                const bool assigned = window.m_audioBusRouting.trackBus(0) == busB
                    && serial() == serial0 + 1 && combo->currentIndex() == 2;
                timeline->undo();
                const bool undone = window.m_audioBusRouting.trackBus(0) == -1
                    && combo->currentIndex() == 0;
                timeline->redo();
                const bool redone = window.m_audioBusRouting.trackBus(0) == busB
                    && combo->currentIndex() == 2;
                std::fprintf(stderr, "mixer-dock bus: assigned=%d undone=%d redone=%d trackBus=%d serial=%llu->%llu\n",
                             int(assigned), int(undone), int(redone), window.m_audioBusRouting.trackBus(0),
                             static_cast<unsigned long long>(serial0),
                             static_cast<unsigned long long>(serial()));
                ok = assigned && undone && redone;
            }
            gate(12, ok);
        }
    }
    {
        // G13 (negative control): a hidden Dock follows external pan / bus changes
        // by synchronize() without writing anything. The control writes through
        // the visible controls and must move the counter, so a counter that never
        // counts (or a synchronize that writes) fails here.
        auto mixer = std::make_unique<AudioMixer>();
        Timeline timeline;
        wire(timeline, *mixer);
        audiobus::AudioBusRouting routing;
        const int bus = routing.addBus(QStringLiteral("バスA"));
        AudioMixerDock dock(&timeline, mixer.get());
        dock.setBusRouting(&routing);
        const quint64 writes0 = dock.gainWriteCountForTest();
        const auto serial = timeline.undoManager()->saveSerial();
        QSignalSpy requests(&dock, &AudioMixerDock::trackBusRequested);
        mixer->setTrackPan(0, 0.5);
        routing.assignTrackToBus(0, bus);
        dock.synchronize();
        auto *pan = dock.findChild<QSlider *>(QStringLiteral("mixerPan0"));
        auto *combo = dock.findChild<QComboBox *>(QStringLiteral("mixerBus0"));
        const bool hidden = dock.isHidden();
        const bool followed = pan && combo && pan->value() == 50 && combo->currentIndex() == 1;
        const bool silent = dock.gainWriteCountForTest() == writes0 && requests.count() == 0
            && timeline.undoManager()->saveSerial() == serial;
        // Control: the same kind of change through the controls is counted.
        if (pan) pan->setValue(-20);
        if (combo) combo->setCurrentIndex(0);
        const quint64 controlWrites = dock.gainWriteCountForTest() - writes0;
        std::fprintf(stderr, "mixer-dock hidden-sync: hidden=%d followed=%d silent=%d controlWrites=%llu requests=%d\n",
                     int(hidden), int(followed), int(silent),
                     static_cast<unsigned long long>(controlWrites), int(requests.count()));
        gate(13, hidden && followed && silent && controlWrites == 2 && requests.count() == 1);
    }
    return tally.finish("mixer-dock");
}
