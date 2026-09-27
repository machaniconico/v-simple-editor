#include "../AudioMixerDock.h"
#include "../AudioMeterWidget.h"
#include "../AudioMixer.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../RenderInPlace.h"
#include "../libavcore/AudioExtract.h"

#include <QApplication>
#include <QAction>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
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
}

int runMixerDockSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };

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
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
