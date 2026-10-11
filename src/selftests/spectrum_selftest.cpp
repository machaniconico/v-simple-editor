#include "SelftestGate.h"
#include "../AudioMixer.h"
#include "../MainWindow.h"
#include "../SpectrumAnalyzerDock.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QMenu>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    // Isolate the default-visibility check from a user's saved dock layout.
    QSettings settings("VSimpleEditor", "WindowState");
    const QVariant previousState = settings.value("windowState");
    settings.remove("windowState");
    settings.sync();
    {
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
        if (dock) dock->hide();
    }
    if (previousState.isValid()) settings.setValue("windowState", previousState);
    else settings.remove("windowState");
    settings.sync();
    return tally.finish("spectrum");
}
