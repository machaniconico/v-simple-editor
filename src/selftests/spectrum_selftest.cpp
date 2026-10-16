#include "SelftestGate.h"
#include "../AudioMixer.h"
#include "../MainWindow.h"
#include "../SpectrumAnalyzerDock.h"
#include "../Timeline.h"
#include "../libavcore/AudioExtract.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QMenu>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBinHz = double(SpectrumAnalyzerDock::kSampleRateHz)
    / SpectrumAnalyzerDock::kFftSize;

QVector<float> sine(double hz)
{
    QVector<float> samples(SpectrumAnalyzerDock::kFftSize);
    for (int i = 0; i < samples.size(); ++i)
        samples[i] = float(0.5 * std::sin(2.0 * kPi * hz * i
            / SpectrumAnalyzerDock::kSampleRateHz));
    return samples;
}

int peakBin(const QVector<float> &db)
{
    if (db.isEmpty()) return -1;
    return int(std::max_element(db.cbegin(), db.cend()) - db.cbegin());
}

std::vector<int16_t> stereoPcm(const QVector<float> &mono)
{
    std::vector<int16_t> stereo(mono.size() * 2);
    for (int i = 0; i < mono.size(); ++i) {
        const int16_t sample = int16_t(std::lround(mono[i] * 32767.0f));
        stereo[2 * i] = stereo[2 * i + 1] = sample;
    }
    return stereo;
}

// Isolates the default-visibility check from the user's saved dock layout and
// restores it however the scope is left.
class WindowStateGuard
{
public:
    WindowStateGuard()
        : m_settings("VSimpleEditor", "WindowState"), m_previous(m_settings.value("windowState"))
    {
        m_settings.remove("windowState");
        m_settings.sync();
    }
    ~WindowStateGuard()
    {
        if (m_previous.isValid()) m_settings.setValue("windowState", m_previous);
        else m_settings.remove("windowState");
        m_settings.sync();
    }
    WindowStateGuard(const WindowStateGuard &) = delete;
    WindowStateGuard &operator=(const WindowStateGuard &) = delete;

private:
    QSettings m_settings;
    QVariant m_previous;
};

// Highest level within +/- 2 bins of `hz` (the Hann main lobe of an
// off-centre tone); -200 dB when the spectrum is missing.
float levelAt(const QVector<float> &db, double hz)
{
    const int centre = int(std::lround(hz / kBinHz));
    float level = -200.0f;
    for (int bin = centre - 2; bin <= centre + 2; ++bin)
        if (bin >= 0 && bin < db.size()) level = std::max(level, db[bin]);
    return level;
}

float maxLevel(const QVector<float> &db)
{
    return db.isEmpty() ? -200.0f : *std::max_element(db.cbegin(), db.cend());
}

// Mono spectrum of the newest kFftSize frames of a readData PCM block.
QVector<float> pcmSpectrum(const QByteArray &pcm)
{
    const int frames = int(pcm.size() / AudioMixer::kBytesPerFrame);
    if (frames < SpectrumAnalyzerDock::kFftSize) return {};
    const auto *samples = reinterpret_cast<const int16_t *>(pcm.constData());
    QVector<float> mono(SpectrumAnalyzerDock::kFftSize);
    const int first = frames - SpectrumAnalyzerDock::kFftSize;
    for (int i = 0; i < mono.size(); ++i)
        mono[i] = 0.5f * (samples[2 * (first + i)] + samples[2 * (first + i) + 1]) / 32768.0f;
    return SpectrumAnalyzerDock::computeSpectrumForTest(mono);
}

} // namespace

int runSpectrumSelftest()
{
    selftest::GateTally tally;

    // G1 detects a missing/wrong FFT, sample rate, Hann window, or amplitude
    // normalization. A Hann main lobe occupies neighbouring bins; leakage is
    // measured outside peak +/- 3 bins so the physical main lobe is excluded.
    const QVector<float> oneKhz = SpectrumAnalyzerDock::computeSpectrumForTest(sine(1000.0));
    const int peak = peakBin(oneKhz);
    float nextDb = -200.0f;
    int nextBin = -1;
    for (int i = 0; i < oneKhz.size(); ++i) {
        if (peak >= 0 && std::abs(i - peak) <= 3) continue;
        if (oneKhz[i] > nextDb) { nextDb = oneKhz[i]; nextBin = i; }
    }
    const float peakDb = peak >= 0 ? oneKhz[peak] : -200.0f;
    std::fprintf(stderr, "spectrum G1 peak bin=%d Hz=%.2f dB=%.2f; second bin=%d Hz=%.2f dB=%.2f\n",
        peak, peak * kBinHz, peakDb, nextBin, nextBin * kBinHz, nextDb);
    tally.gate(1, oneKhz.size() == SpectrumAnalyzerDock::kFftSize / 2 + 1
        && std::abs(peak * kBinHz - 1000.0) <= kBinHz
        && peakDb > -9.0f && peakDb < -4.0f
        && peakDb - nextDb >= 40.0f,
        "1 kHz FFT peak, amplitude sanity, and Hann sidelobe rejection");

    // G2 is the negative control: silence must stay below the floor and a
    // different tone must move the peak rather than reusing a cached result.
    const QVector<float> silent = SpectrumAnalyzerDock::computeSpectrumForTest(
        QVector<float>(SpectrumAnalyzerDock::kFftSize, 0.0f));
    const QVector<float> twoKhz = SpectrumAnalyzerDock::computeSpectrumForTest(sine(2000.0));
    const int twoPeak = peakBin(twoKhz);
    const int oldBin = int(std::lround(1000.0 / kBinHz));
    const float silentMax = silent.isEmpty() ? 0.0f
        : *std::max_element(silent.cbegin(), silent.cend());
    std::fprintf(stderr, "spectrum G2 silence max=%.2f dB; 2 kHz bin=%d Hz=%.2f, 1 kHz bin=%.2f dB\n",
        silentMax, twoPeak, twoPeak * kBinHz,
        twoKhz.size() > oldBin ? twoKhz[oldBin] : 0.0f);
    tally.gate(2, silent.size() == oneKhz.size() && silentMax <= -90.0f
        && std::abs(twoPeak * kBinHz - 2000.0) <= kBinHz
        && twoKhz.size() > oldBin && twoKhz[twoPeak] - twoKhz[oldBin] >= 40.0f,
        "silence and changed input reject a stale or constant spectrum");

    int windowComboCount = -1;
    int windowAudioTracks = -1;
    {
        const WindowStateGuard windowState;
        MainWindow window;
        auto *dock = window.findChild<QDockWidget *>("spectrumAnalyzerDock");
        auto *analyzer = qobject_cast<SpectrumAnalyzerDock *>(dock);
        auto *mixer = window.findChild<AudioMixer *>();
        QMenu *viewMenu = nullptr;
        for (auto *menu : window.findChildren<QMenu *>()) {
            if (menu->title().startsWith(QStringLiteral("表示"))) {
                viewMenu = menu;
                break;
            }
        }
        QAction *action = dock ? dock->toggleViewAction() : nullptr;
        const bool inMenu = viewMenu && action && viewMenu->actions().contains(action);
        const int beforeShowSubscribers = mixer ? mixer->tapSubscriberCountForTest() : -1;
        const bool defaultHidden = dock && !dock->isVisible();
        window.show();
        QApplication::processEvents();
        const bool stillHidden = dock && !dock->isVisible();
        if (action) action->trigger();
        const bool shown = mixer && QTest::qWaitFor([&] {
            return dock->isVisible() && mixer->tapSubscriberCountForTest() == 1;
        }, 2000);
        const int shownSubscribers = mixer ? mixer->tapSubscriberCountForTest() : -1;
        QApplication::processEvents();
        if (action) action->trigger();
        const bool hiddenAgain = mixer && QTest::qWaitFor([&] {
            return !dock->isVisible() && mixer->tapSubscriberCountForTest() == 0;
        }, 2000);
        const int hiddenSubscribers = mixer ? mixer->tapSubscriberCountForTest() : -1;
        std::fprintf(stderr, "spectrum G3 dock=%d menu=%d defaultHidden=%d stillHidden=%d subscribers=%d/%d/%d\n",
            dock != nullptr, inMenu, defaultHidden, stillHidden,
            beforeShowSubscribers, shownSubscribers, hiddenSubscribers);
        tally.gate(3, defaultHidden && stillHidden && action
            && action->text() == QStringLiteral("スペクトラムアナライザ")
            && action->shortcut().isEmpty()
            && inMenu && beforeShowSubscribers == 0 && shown
            && shownSubscribers == 1 && hiddenAgain && hiddenSubscribers == 0,
            "dock/menu presence, OFF by default, and tap subscription 0->1->0");

        // G4 detects a broken masterTapAvailable -> takeMasterTap -> mono FFT
        // path. The test tone is published through the same tap writer used
        // by MixerIODevice::readData.
        if (action) action->trigger();
        const bool ready = analyzer && mixer && QTest::qWaitFor([&] {
            return dock->isVisible() && analyzer->tapSubscribedForTest();
        }, 2000);
        QSignalSpy tapSpy(mixer, &AudioMixer::masterTapAvailable);
        const quint64 copyBefore = mixer ? mixer->tapCopyCountForTest() : 0;
        const quint64 updatesBefore = analyzer ? analyzer->spectrumUpdateCountForTest() : 0;
        const auto block = stereoPcm(sine(1000.0));
        if (ready) mixer->publishMasterBlockForTest(block.data(), int(block.size() / 2));
        const bool signalled = ready && QTest::qWaitFor([&] { return !tapSpy.isEmpty(); }, 2000);
        const bool spectrumReady = signalled && QTest::qWaitFor([&] {
            return analyzer->spectrumUpdateCountForTest() > updatesBefore;
        }, 2000);
        const QVector<float> latest = analyzer ? analyzer->lastSpectrumForTest() : QVector<float>{};
        const int livePeak = peakBin(latest);
        const quint64 copyAfter = mixer ? mixer->tapCopyCountForTest() : 0;
        std::fprintf(stderr, "spectrum G4 signal=%d tapCopies=%llu peak bin=%d Hz=%.2f dB=%.2f\n",
            signalled, static_cast<unsigned long long>(copyAfter - copyBefore),
            livePeak, livePeak * kBinHz, livePeak >= 0 ? latest[livePeak] : -200.0f);
        tally.gate(4, spectrumReady && copyAfter > copyBefore
            && std::abs(livePeak * kBinHz - 1000.0) <= kBinHz,
            "published master PCM must reach the visible dock spectrum");
        // G5 also checks the MainWindow wiring: its Dock lists the window's
        // audio tracks (setTimeline) next to the master.
        const auto *windowCombo = dock ? dock->findChild<QComboBox *>("spectrumSourceCombo") : nullptr;
        const auto *windowTimeline = window.findChild<Timeline *>();
        windowComboCount = windowCombo ? windowCombo->count() : -1;
        windowAudioTracks = windowTimeline ? windowTimeline->audioTrackCount() : -1;
        if (dock) dock->hide();
    }

    // G5-G8 run a dedicated mixer: A1 = 1 kHz, A2 = 3 kHz, 0.2 amplitude each.
    // The Dock is shown (subscribed) before every stopped-transport render.
    constexpr float kFloor = SpectrumAnalyzerDock::kFloorDb;
    QTemporaryDir directory;
    QString error;
    const auto writeTone = [&](const QString &name, double hz) {
        std::vector<float> samples(48000 * 2);
        for (int i = 0; i < 48000; ++i)
            samples[2 * i] = samples[2 * i + 1] = float(0.2 * std::sin(2.0 * kPi * hz * i / 48000));
        const QString path = directory.filePath(name);
        return libavcore::writeWavFloatInterleaved(path, samples, 48000, 2, &error) ? path : QString();
    };
    const QString oneKhzPath = directory.isValid() ? writeTone(QStringLiteral("a1-1k.wav"), 1000.0) : QString();
    const QString threeKhzPath = directory.isValid() ? writeTone(QStringLiteral("a2-3k.wav"), 3000.0) : QString();
    const bool sourcesOk = !oneKhzPath.isEmpty() && !threeKhzPath.isEmpty();
    if (!sourcesOk) std::fprintf(stderr, "spectrum: tone fixture failed: %s\n", qPrintable(error));

    auto mixer = std::make_unique<AudioMixer>();
    Timeline timeline;
    timeline.setAudioMixer(mixer.get());
    QObject::connect(&timeline, &Timeline::trackIndicesRemapped, mixer.get(),
                     [m = mixer.get()](bool audio, const QVector<int> &map) {
        if (audio) m->remapTrackIndices(map);
    });
    {
        const auto clipFor = [](const QString &path) {
            ClipInfo clip;
            clip.filePath = path;
            clip.duration = clip.outPoint = 1.0;
            return clip;
        };
        const QVector<QVector<ClipInfo>> video{{}};
        const QVector<QVector<ClipInfo>> audio{{clipFor(oneKhzPath)}, {clipFor(threeKhzPath)}};
        timeline.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    }
    mixer->setSequence(timeline.computeAudioPlaybackSequence());
    SpectrumAnalyzerDock analyzerDock(mixer.get());
    analyzerDock.setTimeline(&timeline);
    auto *sourceCombo = analyzerDock.findChild<QComboBox *>("spectrumSourceCombo");
    analyzerDock.show();
    const bool subscribed = QTest::qWaitFor([&] {
        return analyzerDock.tapSubscribedForTest() && mixer->tapSubscriberCountForTest() == 1;
    }, 2000);
    // Renders through readData and waits for the Dock's spectrum of that tap.
    QByteArray pcm;
    const auto renderSpectrum = [&]() -> QVector<float> {
        const quint64 before = analyzerDock.spectrumUpdateCountForTest();
        pcm = mixer->renderPcmForTest(9600);
        const bool updated = !pcm.isEmpty() && QTest::qWaitFor([&] {
            return analyzerDock.spectrumUpdateCountForTest() > before;
        }, 2000);
        return updated ? analyzerDock.lastSpectrumForTest() : QVector<float>{};
    };
    const auto select = [&](int index) {
        if (sourceCombo && index < sourceCombo->count()) sourceCombo->setCurrentIndex(index);
    };
    const bool comboOk = sourceCombo && sourceCombo->count() == 3
        && sourceCombo->itemText(0) == QStringLiteral("マスター")
        && sourceCombo->itemText(1) == QStringLiteral("A1")
        && sourceCombo->itemText(2) == QStringLiteral("A2");

    // G5: the A1 tap shows its 1 kHz only, the master both tones. The master
    // run is the control: a tap that ignored its source would show 3 kHz on
    // A1 as loud as on the master. The tap also follows its track through a
    // structural remap (A1 -> index 1) and back through the undo restore of
    // the track identities (applyTrackState), which must not leave it on 1.
    select(0);
    const int masterSource = mixer->tapSource().track;
    const QVector<float> masterDb = renderSpectrum();
    select(1);
    const int a1Source = mixer->tapSource().track;
    const QVector<float> a1Db = renderSpectrum();
    const QJsonObject trackSnapshot = mixer->collectTrackState();
    mixer->remapTrackIndices({1, 0});
    const int remappedSource = mixer->tapSource().track;
    mixer->applyTrackState(trackSnapshot);
    const int restoredSource = mixer->tapSource().track;
    // The same through the real Timeline undo / redo with the mixer as the
    // external track state (A1, A2; A2 tapped; remove A1). Redo first removes
    // the last row (A2's live row, remap {0, -1}) and only then applies the
    // snapshot, so a tap kept only as an index ends on the master: before
    // the tap kept its track identity, redo left the tap at -1 (FAIL).
    int undoRemovedSource = -2, undoSource = -2, redoSource = -2;
    {
        auto undoMixer = std::make_unique<AudioMixer>();
        Timeline undoTimeline;
        undoTimeline.setExternalTrackStateHooks([&] { return undoMixer->collectTrackState(); },
            [&](const QJsonObject &state) { undoMixer->applyTrackState(state); });
        QObject::connect(&undoTimeline, &Timeline::trackIndicesRemapped, undoMixer.get(),
                         [m = undoMixer.get()](bool audio, const QVector<int> &map) {
            if (audio) m->remapTrackIndices(map);
        });
        ClipInfo clip;
        clip.filePath = oneKhzPath;
        clip.duration = clip.outPoint = 1.0;
        const QVector<QVector<ClipInfo>> video{{}};
        const QVector<QVector<ClipInfo>> audio{{clip}, {clip}};
        undoTimeline.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
        undoMixer->setTapSource(AudioMixer::TapSource{1});
        if (undoTimeline.removeTrack(true, 0)) {
            undoRemovedSource = undoMixer->tapSource().track;
            undoTimeline.undo();
            undoSource = undoMixer->tapSource().track;
            undoTimeline.redo();
            redoSource = undoMixer->tapSource().track;
        }
    }
    const float master1k = levelAt(masterDb, 1000.0), master3k = levelAt(masterDb, 3000.0);
    const float a1_1k = levelAt(a1Db, 1000.0), a1_3k = levelAt(a1Db, 3000.0);
    std::fprintf(stderr, "spectrum G5 sources=%d combo=%d subscribed=%d tapSource master=%d A1=%d remapped=%d restored=%d undo(remove A1 / undo / redo)=%d/%d/%d | master 1k=%.2f 3k=%.2f | A1 1k=%.2f 3k=%.2f dB (floor %.0f) | window combo=%d audioTracks=%d\n",
        sourcesOk, comboOk, subscribed, masterSource, a1Source, remappedSource, restoredSource,
        undoRemovedSource, undoSource, redoSource, master1k, master3k,
        a1_1k, a1_3k, kFloor, windowComboCount, windowAudioTracks);
    tally.gate(5, sourcesOk && comboOk && subscribed && masterSource == -1 && a1Source == 0
        && remappedSource == 1 && restoredSource == 0
        && undoRemovedSource == 0 && undoSource == 1 && redoSource == 0
        && master1k >= kFloor + 30.0f && master3k >= kFloor + 30.0f
        && a1_1k >= kFloor + 30.0f && a1_3k < kFloor + 10.0f
        && windowAudioTracks >= 1 && windowComboCount == windowAudioTracks + 1,
        "track tap shows only its own tone while the master shows both, and keeps its track through undo / redo");

    // G6: the A2 tap shows its 3 kHz only, taken after FX and before the
    // fader: A2 is muted through the timeline (the UI mute path, which
    // reaches the mixer as the entries' audioMuted) with its fader at -12 dB,
    // yet the tap reads the master's unmuted 3 kHz level while the mixed
    // output loses the tone.
    timeline.toggleMuteTrack(1);
    const QVector<PlaybackEntry> mutedSequence = timeline.computeAudioPlaybackSequence();
    const bool timelineMuted = std::any_of(mutedSequence.cbegin(), mutedSequence.cend(),
        [](const PlaybackEntry &e) { return e.sourceTrack == 1 && e.audioMuted; });
    mixer->setSequence(mutedSequence);
    mixer->setTrackGain(1, 0.25);
    select(2);
    const int a2Source = mixer->tapSource().track;
    const QVector<float> a2Db = renderSpectrum();
    const QVector<float> mixedDb = pcmSpectrum(pcm);
    const float a2_1k = levelAt(a2Db, 1000.0), a2_3k = levelAt(a2Db, 3000.0);
    const float mixed1k = levelAt(mixedDb, 1000.0), mixed3k = levelAt(mixedDb, 3000.0);
    std::fprintf(stderr, "spectrum G6 tapSource=%d timelineMuted=%d | A2 tap 1k=%.2f 3k=%.2f (master 3k %.2f, diff %.2f) | muted mix 1k=%.2f 3k=%.2f dB\n",
        a2Source, timelineMuted, a2_1k, a2_3k, master3k, a2_3k - master3k, mixed1k, mixed3k);
    tally.gate(6, sourcesOk && a2Source == 1 && timelineMuted
        && a2_3k >= kFloor + 30.0f && a2_1k < kFloor + 10.0f
        && std::abs(a2_3k - master3k) <= 1.0f
        && mixed1k >= kFloor + 30.0f && mixed3k < kFloor + 30.0f,
        "timeline-muted, faded-down track tap shows its tone pre-fader and nothing else");

    // G7: with no tap (stopped transport) the spectrum decays at 12 dB/s to
    // the floor. Control: half the steps must leave it well above the floor
    // at exactly that rate, so an instant clear or a wrong rate fails. The
    // expected rate is the test's own, not the production constant.
    constexpr double kExpectedDecayDbPerSec = 12.0;
    const double stepDb = kExpectedDecayDbPerSec
        * SpectrumAnalyzerDock::kRedrawIntervalMs / 1000.0;
    const float decayStart = maxLevel(analyzerDock.lastSpectrumForTest());
    const int stepsToFloor = int(std::ceil((decayStart - kFloor) / stepDb));
    const int halfSteps = stepsToFloor / 2;
    analyzerDock.idleDecayStepsForTest(halfSteps);
    const float decayHalf = maxLevel(analyzerDock.lastSpectrumForTest());
    analyzerDock.idleDecayStepsForTest(stepsToFloor - halfSteps);
    const QVector<float> decayed = analyzerDock.lastSpectrumForTest();
    const float decayEnd = maxLevel(decayed);
    std::fprintf(stderr, "spectrum G7 start=%.2f steps=%d (%.3f s) half=%.2f (expected %.2f) end=%.2f bins=%d\n",
        decayStart, stepsToFloor, stepsToFloor * SpectrumAnalyzerDock::kRedrawIntervalMs / 1000.0,
        decayHalf, decayStart - halfSteps * stepDb, decayEnd, int(decayed.size()));
    tally.gate(7, decayStart >= kFloor + 30.0f
        && std::abs(decayHalf - (decayStart - halfSteps * stepDb)) <= 0.05
        && decayHalf > kFloor + 1.0f
        && decayed.size() == SpectrumAnalyzerDock::kFftSize / 2 + 1 && decayEnd <= kFloor + 1.0f,
        "idle spectrum decays at 12 dB/s down to the floor");

    // G8: hiding unsubscribes and drops the last spectrum (HEAD kept it).
    const int beforeHide = analyzerDock.lastSpectrumForTest().size();
    analyzerDock.hide();
    const bool unsubscribed = QTest::qWaitFor([&] {
        return !analyzerDock.tapSubscribedForTest() && mixer->tapSubscriberCountForTest() == 0;
    }, 2000);
    const int afterHide = analyzerDock.lastSpectrumForTest().size();
    std::fprintf(stderr, "spectrum G8 bins before hide=%d after=%d unsubscribed=%d\n",
        beforeHide, afterHide, unsubscribed);
    tally.gate(8, beforeHide == SpectrumAnalyzerDock::kFftSize / 2 + 1 && unsubscribed
        && afterHide == 0, "unsubscribe clears the last spectrum");
    return tally.finish("spectrum");
}
