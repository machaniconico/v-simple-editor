#include "../AudioExportMix.h"
#include "../AudioMixer.h"
#include "../AudioTrackFx.h"
#include "../MultibandCompressorPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonObject>
#include <QPushButton>
#include <QSignalSpy>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kSampleRate = 48000;

double rmsDb(const std::vector<double> &buf, size_t skipFrames)
{
    double sumSq = 0.0;
    size_t count = 0;
    for (size_t i = skipFrames; i < buf.size(); ++i) {
        sumSq += buf[i] * buf[i];
        ++count;
    }
    if (count == 0) return -120.0;
    const double rms = std::sqrt(sumSq / static_cast<double>(count));
    return 20.0 * std::log10(std::max(rms, 1e-12));
}

// Single-frequency DFT magnitude (dB) over steady-state window to measure
// per-tone attenuation cleanly in a dual-tone signal.
double toneMagnitudeDb(const std::vector<double> &buf, size_t skipFrames, double freqHz)
{
    double re = 0.0, im = 0.0;
    size_t count = 0;
    for (size_t i = skipFrames; i < buf.size(); ++i) {
        const double ph = 2.0 * kPi * freqHz * static_cast<double>(i) / static_cast<double>(kSampleRate);
        re += buf[i] * std::cos(ph);
        im -= buf[i] * std::sin(ph);
        ++count;
    }
    if (count == 0) return -120.0;
    const double mag = 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
    return 20.0 * std::log10(std::max(mag, 1e-12));
}

} // namespace

int runMultibandCompSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](const char *gate, bool ok, const QString &detail) {
        if (ok) {
            ++passed;
            std::cout << "PASS " << gate << " " << detail.toStdString() << "\n";
        } else {
            ++failed;
            std::cerr << "FAIL " << gate << " " << detail.toStdString() << "\n";
        }
    };

    // G1: Disabled = bit-exact bypass (zero diff, zero processCalls).
    {
        trackfx::MultibandCompProcessor proc;
        trackfx::MultibandCompSettings s = trackfx::multibandCompPreset(QStringLiteral("podcast"));
        s.enabled = false;
        proc.setSettings(s, kSampleRate);
        double maxDiff = 0.0;
        for (int i = 0; i < 1024; ++i) {
            const double origL = 16000.0 * std::sin(2.0 * kPi * 440.0 * i / kSampleRate);
            const double origR = 12000.0 * std::cos(2.0 * kPi * 1000.0 * i / kSampleRate);
            double l = origL, r = origR;
            proc.processFrame(l, r);
            maxDiff = std::max(maxDiff, std::max(std::abs(l - origL), std::abs(r - origR)));
        }
        const bool ok = (maxDiff == 0.0 && proc.processCallsForTest() == 0);
        check("G1", ok, QStringLiteral("bypassDiff=%1 calls=%2").arg(maxDiff).arg(proc.processCallsForTest()));
    }

    // G2: LR4 4-band crossover flat magnitude reconstruction (ratio=1, makeup=0)
    // across 80 Hz, 500 Hz, 2500 Hz, 9000 Hz -> within +/-0.15 dB of unity.
    {
        trackfx::MultibandCompProcessor proc;
        trackfx::MultibandCompSettings s = trackfx::multibandCompPreset(QStringLiteral("flat"));
        proc.setSettings(s, kSampleRate);

        const double freqs[4] = {80.0, 500.0, 2500.0, 9000.0};
        double maxErrDb = 0.0;
        for (double f : freqs) {
            proc.reset();
            constexpr int N = 12000;
            std::vector<double> dry(N), wet(N);
            for (int i = 0; i < N; ++i) {
                const double x = 10000.0 * std::sin(2.0 * kPi * f * i / kSampleRate);
                dry[i] = x;
                double l = x, r = x;
                proc.processFrame(l, r);
                wet[i] = l;
            }
            const double errDb = std::abs(rmsDb(wet, 4000) - rmsDb(dry, 4000));
            maxErrDb = std::max(maxErrDb, errDb);
        }
        check("G2", maxErrDb <= 0.15, QStringLiteral("maxFlatErrDb=%1").arg(maxErrDb, 0, 'f', 4));
    }

    // G3: Band isolation / independent compression: compress ONLY Band 0 (<200 Hz)
    // on an 80 Hz + 6000 Hz dual-tone; 80 Hz drops > 8 dB while 6000 Hz stays within +/-0.3 dB.
    {
        trackfx::MultibandCompProcessor proc;
        trackfx::MultibandCompSettings s = trackfx::multibandCompPreset(QStringLiteral("flat"));
        s.crossoverHz = {200.0, 1200.0, 5000.0};
        s.bands[0].thresholdDb = -28.0;
        s.bands[0].ratio = 8.0;
        s.bands[0].attackMs = 2.0;
        s.bands[0].releaseMs = 80.0;
        s.bands[0].kneeDb = 0.0;
        proc.setSettings(s, kSampleRate);

        constexpr int N = 24000;
        std::vector<double> dry(N), wet(N);
        for (int i = 0; i < N; ++i) {
            const double low = 12000.0 * std::sin(2.0 * kPi * 80.0 * i / kSampleRate);
            const double hi  = 10000.0 * std::sin(2.0 * kPi * 6000.0 * i / kSampleRate);
            dry[i] = low + hi;
            double l = dry[i], r = dry[i];
            proc.processFrame(l, r);
            wet[i] = l;
        }
        const double lowDropDb = toneMagnitudeDb(dry, 8000, 80.0) - toneMagnitudeDb(wet, 8000, 80.0);
        const double hiDiffDb  = std::abs(toneMagnitudeDb(dry, 8000, 6000.0) - toneMagnitudeDb(wet, 8000, 6000.0));
        const auto gr = proc.currentGrDb();
        const bool ok = (lowDropDb > 8.0 && hiDiffDb < 0.3 && gr[0] > 8.0 && gr[3] < 0.05);
        check("G3", ok, QStringLiteral("lowDropDb=%1 hiDiffDb=%2 gr0=%3 gr3=%4")
                            .arg(lowDropDb, 0, 'f', 2).arg(hiDiffDb, 0, 'f', 3)
                            .arg(gr[0], 0, 'f', 2).arg(gr[3], 0, 'f', 3));
    }

    // G4: Band Solo auditioning: solo Band 2 (1200-5000 Hz) on 80 Hz + 2500 Hz mix.
    {
        trackfx::MultibandCompProcessor proc;
        trackfx::MultibandCompSettings s = trackfx::multibandCompPreset(QStringLiteral("flat"));
        s.crossoverHz = {200.0, 1200.0, 5000.0};
        s.bands[2].solo = true;
        proc.setSettings(s, kSampleRate);

        constexpr int N = 24000;
        std::vector<double> dry(N), wet(N);
        for (int i = 0; i < N; ++i) {
            const double low = 10000.0 * std::sin(2.0 * kPi * 80.0 * i / kSampleRate);
            const double mid = 10000.0 * std::sin(2.0 * kPi * 2500.0 * i / kSampleRate);
            dry[i] = low + mid;
            double l = dry[i], r = dry[i];
            proc.processFrame(l, r);
            wet[i] = l;
        }
        const double lowAttenDb = toneMagnitudeDb(dry, 8000, 80.0) - toneMagnitudeDb(wet, 8000, 80.0);
        const double midDiffDb  = std::abs(toneMagnitudeDb(dry, 8000, 2500.0) - toneMagnitudeDb(wet, 8000, 2500.0));
        check("G4", lowAttenDb > 24.0 && midDiffDb < 0.5,
              QStringLiteral("soloLowAttenDb=%1 midDiffDb=%2").arg(lowAttenDb, 0, 'f', 2).arg(midDiffDb, 0, 'f', 3));
    }

    // G5: Band Bypass + Makeup Gain + Wet/Dry parallel mix.
    {
        trackfx::MultibandCompProcessor proc;
        trackfx::MultibandCompSettings s = trackfx::multibandCompPreset(QStringLiteral("flat"));
        s.wetMix = 0.0;
        s.outputGainDb = 12.0;
        proc.setSettings(s, kSampleRate);
        double dryErr = 0.0;
        for (int i = 0; i < 2048; ++i) {
            const double x = 8000.0 * std::sin(2.0 * kPi * 500.0 * i / kSampleRate);
            double l = x, r = x;
            proc.processFrame(l, r);
            dryErr = std::max(dryErr, std::abs(l - x));
        }

        // Makeup +6 dB on Band 1 (200-1200 Hz) with wetMix=1.0, bypassed -> 0 dB change, unbypassed -> +6 dB.
        s = trackfx::multibandCompPreset(QStringLiteral("flat"));
        s.bands[1].makeupDb = 6.0;
        s.bands[1].bypass = true;
        proc.setSettings(s, kSampleRate);
        constexpr int N = 12000;
        std::vector<double> dry(N), byp(N), boosted(N);
        for (int i = 0; i < N; ++i) {
            const double x = 5000.0 * std::sin(2.0 * kPi * 500.0 * i / kSampleRate);
            dry[i] = x;
            double l = x, r = x;
            proc.processFrame(l, r);
            byp[i] = l;
        }
        s.bands[1].bypass = false;
        proc.setSettings(s, kSampleRate);
        proc.reset();
        for (int i = 0; i < N; ++i) {
            double l = dry[i], r = dry[i];
            proc.processFrame(l, r);
            boosted[i] = l;
        }
        const double bypDiffDb = std::abs(rmsDb(byp, 4000) - rmsDb(dry, 4000));
        const double boostDb   = rmsDb(boosted, 4000) - rmsDb(dry, 4000);
        check("G5", dryErr < 1e-9 && bypDiffDb < 0.2 && std::abs(boostDb - 6.0) < 0.3,
              QStringLiteral("dryErr=%1 bypDiffDb=%2 boostDb=%3").arg(dryErr).arg(bypDiffDb, 0, 'f', 3).arg(boostDb, 0, 'f', 2));
    }

    // G6: trackfx::Chain JSON round-trip, default sparsity, and out-of-range clamp.
    {
        const trackfx::Chain def;
        const bool defaultSparse = def.isDefault() && def.toJson().isEmpty();

        trackfx::Chain c;
        c.multibandComp = trackfx::multibandCompPreset(QStringLiteral("narration"));
        c.multibandComp.outputGainDb = 2.5;
        c.multibandComp.wetMix = 0.75;
        c.multibandComp.bands[1].bypass = true;
        const QJsonObject json = c.toJson();
        const trackfx::Chain rt = trackfx::Chain::fromJson(json);
        const bool rtOk = json.contains(QStringLiteral("multibandComp"))
            && rt.multibandComp.enabled == true
            && std::abs(rt.multibandComp.outputGainDb - 2.5) < 1e-9
            && std::abs(rt.multibandComp.wetMix - 0.75) < 1e-9
            && rt.multibandComp.crossoverHz == c.multibandComp.crossoverHz
            && rt.multibandComp.bands[1].bypass == true
            && std::abs(rt.multibandComp.bands[0].thresholdDb - c.multibandComp.bands[0].thresholdDb) < 1e-9;

        // Inverted crossovers should be sanitized into strictly ascending order.
        trackfx::Chain bad;
        bad.multibandComp.enabled = true;
        bad.multibandComp.crossoverHz = {5000.0, 200.0, 100.0};
        const trackfx::Chain clean = trackfx::Chain::fromJson(bad.toJson());
        const bool orderOk = clean.multibandComp.crossoverHz[0] < clean.multibandComp.crossoverHz[1]
            && clean.multibandComp.crossoverHz[1] < clean.multibandComp.crossoverHz[2];
        check("G6", defaultSparse && rtOk && orderOk,
              QStringLiteral("sparse=%1 rt=%2 ordered=%3").arg(defaultSparse).arg(rtOk).arg(orderOk));
    }

    // G7: AudioMixer Master + Per-track integration, GR meter readout, and Undo snapshot restore.
    {
        AudioMixer mixer;
        const QJsonObject snapBefore = mixer.collectTrackState();

        auto mbTrack = trackfx::multibandCompPreset(QStringLiteral("vocal"));
        auto mbMaster = trackfx::multibandCompPreset(QStringLiteral("master_punch"));
        mixer.setTrackMultibandComp(0, mbTrack);
        mixer.setMasterMultibandComp(mbMaster);

        // Drive track 0 with loud 100 Hz sine -> Band 0 GR > 3 dB.
        constexpr int frames = 4096;
        std::vector<int16_t> trackBuf(frames * 2);
        for (int i = 0; i < frames; ++i) {
            const int16_t v = static_cast<int16_t>(20000.0 * std::sin(2.0 * kPi * 100.0 * i / kSampleRate));
            trackBuf[i * 2] = v;
            trackBuf[i * 2 + 1] = v;
        }
        mixer.processTrackFxForTest(0, trackBuf.data(), frames);
        const auto grTrack = mixer.multibandGainReductionDb(0);

        // Drive master bus with loud 100 Hz sine -> Master Band 0 GR > 2 dB.
        std::vector<int32_t> masterBuf(frames * 2);
        for (int i = 0; i < frames; ++i) {
            const int32_t v = static_cast<int32_t>(22000.0 * std::sin(2.0 * kPi * 100.0 * i / kSampleRate));
            masterBuf[i * 2] = v;
            masterBuf[i * 2 + 1] = v;
        }
        mixer.processMasterEqForTest(masterBuf.data(), frames);
        const auto grMaster = mixer.multibandGainReductionDb(AudioMixer::kMasterTrackId);

        // Restore snapshot -> both master and track 0 return to disabled default.
        mixer.applyTrackState(snapBefore);
        const bool restored = !mixer.trackMultibandComp(0).enabled
            && !mixer.trackMultibandComp(AudioMixer::kMasterTrackId).enabled;
        check("G7", grTrack[0] > 3.0 && grMaster[0] > 2.0 && restored,
              QStringLiteral("grTrack0=%1 grMaster0=%2 restored=%3")
                  .arg(grTrack[0], 0, 'f', 2).arg(grMaster[0], 0, 'f', 2).arg(restored));
    }

    // G8: Preview vs Export SSOT parity + export ignores preview Solo flag.
    {
        trackfx::Chain chain;
        chain.multibandComp = trackfx::multibandCompPreset(QStringLiteral("podcast"));
        chain.multibandComp.bands[0].solo = true; // preview solo ON

        // Export path via audioexport::processTrackEntries (must ignore solo!)
        constexpr int frames = 8192;
        std::vector<float> expSamples(frames * 2);
        for (int i = 0; i < frames; ++i) {
            const float x = static_cast<float>(0.35 * std::sin(2.0 * kPi * 80.0 * i / kSampleRate)
                                             + 0.35 * std::sin(2.0 * kPi * 3000.0 * i / kSampleRate));
            expSamples[i * 2] = x;
            expSamples[i * 2 + 1] = x;
        }
        std::vector<audioexport::DspEntry> entries;
        entries.push_back(audioexport::DspEntry{0, expSamples});
        std::array<AudioMixer::EqBandCoefs, 3> noLegacy{};
        audioexport::processTrackEntries(entries, chain, noLegacy, false, 0.0);

        // Reference processor with solo cleared -> must match export bit-for-bit.
        trackfx::Chain refChain = chain;
        refChain.multibandComp.bands[0].solo = false;
        trackfx::Processor refProc(refChain, kSampleRate, 2);
        refProc.reset();
        std::vector<float> refSamples = expSamples;
        for (int off = 0; off < frames; off += 1024)
            refProc.process(refSamples.data() + off * 2, std::min(1024, frames - off));

        double maxDiff = 0.0;
        for (size_t i = 0; i < refSamples.size(); ++i)
            maxDiff = std::max(maxDiff, static_cast<double>(std::abs(entries[0].samples[i] - refSamples[i])));
        check("G8", maxDiff < 1e-6, QStringLiteral("exportParityMaxDiff=%1").arg(maxDiff));
    }

    // G9: Presets valid + MultibandCompressorPanel silent reseed & 1-gesture Undo emit.
    {
        const QStringList ids = trackfx::multibandCompPresetIds();
        bool presetsOk = (ids.size() == 5);
        for (const QString &id : ids) {
            const auto p = trackfx::multibandCompPreset(id);
            presetsOk = presetsOk && p.enabled && !trackfx::multibandCompPresetLabel(id).isEmpty();
        }

        MultibandCompressorPanel panel;
        QSignalSpy spyChanged(&panel, &MultibandCompressorPanel::settingsChanged);
        QSignalSpy spyGesture(&panel, &MultibandCompressorPanel::gestureFinished);

        // Reseed must emit 0 signals.
        panel.setTracks({QStringLiteral("Master"), QStringLiteral("A1")},
                        {AudioMixer::kMasterTrackId, 0});
        panel.setSettings(AudioMixer::kMasterTrackId, trackfx::multibandCompPreset(QStringLiteral("vocal")));
        panel.setSettings(0, trackfx::MultibandCompSettings{});
        const bool silentReseed = (spyChanged.count() == 0 && spyGesture.count() == 0);

        // Toggling enable emits settingsChanged + 1 gestureFinished.
        auto *en = panel.findChild<QCheckBox *>(QStringLiteral("multiband_enabled"));
        if (en) en->setChecked(false);
        const bool editEmitted = (spyChanged.count() == 1 && spyGesture.count() == 1);

        // Toggling Solo emits settingsChanged but NOT gestureFinished (monitor-only).
        auto *solo0 = panel.findChild<QPushButton *>(QStringLiteral("multiband_b0_solo"));
        if (solo0) solo0->setChecked(true);
        const bool soloNoUndo = (spyChanged.count() == 2 && spyGesture.count() == 1);

        check("G9", presetsOk && silentReseed && editEmitted && soloNoUndo,
              QStringLiteral("presets=%1 silent=%2 edit=%3 soloNoUndo=%4")
                  .arg(presetsOk).arg(silentReseed).arg(editEmitted).arg(soloNoUndo));
    }

    const int declared = 9;
    std::cout << "multiband-comp summary: " << passed << " PASS, " << failed << " FAIL, 0 SKIP\n";
    std::cout << "gate-count: declared " << declared << ", ran " << (passed + failed) << "\n";
    return failed == 0 ? 0 : 1;
}
