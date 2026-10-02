#include "../AudioTrackFx.h"
#include "../AudioMixer.h"
#include "../DeEsserPanel.h"
#include "../MainWindow.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../VideoPlayer.h"
#include "SelftestGate.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {
constexpr int kRate = 48000;
constexpr int kFrames = 2 * kRate;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> tone(double high = 0.4, double low = 0.2, double hz = 6000.0)
{
    std::vector<float> samples(kFrames * 2);
    for (int f = 0; f < kFrames; ++f) {
        const double value = high * std::sin(2 * kPi * hz * f / kRate)
            + low * std::sin(2 * kPi * 200 * f / kRate);
        const float pcm = static_cast<int16_t>(value * 32768.0) / 32768.0f;
        samples[2 * f] = samples[2 * f + 1] = pcm;
    }
    return samples;
}

double amplitude(const std::vector<float> &samples, double hz)
{
    // Exact integer cycles in the last second; ignore detector attack transient.
    double real = 0.0, imaginary = 0.0;
    for (int f = kRate; f < kFrames; ++f) {
        const double angle = 2 * kPi * hz * f / kRate;
        real += samples[2 * f] * std::cos(angle);
        imaginary += samples[2 * f] * std::sin(angle);
    }
    return 2.0 * std::hypot(real, imaginary) / kRate;
}

double changeDb(const std::vector<float> &wet, const std::vector<float> &dry, double hz)
{
    return 20.0 * std::log10(amplitude(wet, hz) / amplitude(dry, hz));
}

bool identical(const std::vector<float> &a, const std::vector<float> &b)
{
    return a.size() == b.size()
        && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

void process(trackfx::Processor &processor, std::vector<float> &samples, int block = 257)
{
    for (int f = 0; f < kFrames; f += block)
        processor.process(samples.data() + 2 * f, std::min(block, kFrames - f));
}
} // namespace

int runDeEsserSelftest()
{
    selftest::GateTally tally;
    const auto gate = [&](int number, bool ok) { tally.gate(number, ok); };

    // G1: default off skips the new DSP entirely, including sub-PCM16 floats.
    auto precise = tone();
    for (float &sample : precise) sample += 0.000001f;
    precise[0] = -0.0f;
    auto bypassed = precise, disabled = precise;
    trackfx::Chain bypassChain;
    bypassChain.deesser.enabled = true;
    trackfx::Processor bypass(bypassChain, kRate, 2), defaults;
    bypass.setDeEsserBypassForTest(true);
    process(bypass, bypassed);
    process(defaults, disabled);
    gate(1, identical(precise, bypassed) && identical(precise, disabled)
        && defaults.deEsserProcessCallsForTest() == 0
        && bypass.deEsserProcessCallsForTest() == 0);

    trackfx::Chain chain;
    chain.deesser.enabled = true;
    chain.deesser.thresholdDb = -30.0;
    const auto dry = tone();
    auto wet = dry;
    trackfx::Processor deesser(chain, kRate, 2);
    process(deesser, wet);
    const double highDb = changeDb(wet, dry, 6000.0);
    const double lowDb = changeDb(wet, dry, 200.0);

    // Range cap, frequency tuning, listen band, mono, and block-size invariance.
    auto limitedChain = chain;
    limitedChain.deesser.rangeDb = 3.0;
    auto limited = dry;
    trackfx::Processor limiter(limitedChain, kRate, 2);
    process(limiter, limited);
    auto listenChain = chain;
    listenChain.deesser.listen = true;
    auto listened = dry;
    trackfx::Processor listener(listenChain, kRate, 2);
    process(listener, listened);
    auto tunedChain = chain;
    tunedChain.deesser.frequencyHz = 8000.0;
    const auto tunedDry = tone(0.4, 0.2, 8000.0);
    auto tuned = tunedDry;
    trackfx::Processor tunedProcessor(tunedChain, kRate, 2);
    process(tunedProcessor, tuned);
    auto singleBlock = dry;
    trackfx::Processor whole(chain, kRate, 2);
    process(whole, singleBlock, kFrames);
    std::vector<float> mono(kFrames);
    for (int f = 0; f < kFrames; ++f) mono[f] = dry[2 * f];
    trackfx::Processor monoProcessor(chain, kRate, 1);
    monoProcessor.process(mono.data(), kFrames);
    bool monoOk = true;
    for (int f = 0; f < kFrames; ++f) monoOk = monoOk && mono[f] == wet[2 * f];
    std::fprintf(stderr, "deesser: 6kHz %.4f dB, 200Hz %.4f dB, range=3 %.4f dB\n",
                 highDb, lowDb, changeDb(limited, dry, 6000.0));
    gate(2, highDb < -8.0 && highDb >= -12.1 && std::abs(lowDb) < 0.05
        && std::abs(changeDb(limited, dry, 6000.0) + 3.0) < 0.1
        && changeDb(listened, dry, 200.0) < -30.0
        && std::abs(changeDb(listened, dry, 6000.0)) < 0.1
        && changeDb(tuned, tunedDry, 8000.0) < -8.0
        && std::abs(changeDb(tuned, tunedDry, 200.0)) < 0.05
        && identical(singleBlock, wet) && monoOk
        && deesser.deEsserProcessCallsForTest() == kFrames);

    // G3: sub-threshold PCM samples remain exactly unchanged, also after seek.
    const auto quiet = tone(0.001, 0.001);
    auto quietWet = quiet;
    trackfx::Processor quietProcessor(chain, kRate, 2);
    process(quietProcessor, quietWet);
    deesser.resetFeedback();
    auto afterSeek = quiet;
    process(deesser, afterSeek);
    gate(3, identical(quiet, quietWet) && identical(quiet, afterSeek));

    // G4: actual readData helper vs actual export helper, including other FX.
    auto cascade = chain;
    cascade.eqEnabled = true;
    cascade.eq.highMid = {4000.0, 3.0, 1.0, true};
    cascade.compEnabled = cascade.comp.enabled = true;
    cascade.comp.thresholdDb = -18.0;
    cascade.comp.ratio = 2.0;
    auto mixer = std::make_unique<AudioMixer>();
    mixer->setTrackChain(0, cascade);
    std::vector<int16_t> playback(dry.size());
    for (size_t i = 0; i < dry.size(); ++i)
        playback[i] = static_cast<int16_t>(dry[i] * 32768.0f);
    for (int f = 0; f < kFrames; f += 1024)
        mixer->processTrackFxForTest(0, playback.data() + 2 * f, std::min(1024, kFrames - f));
    std::vector<audioexport::DspEntry> entries{{0, dry}};
    audioexport::processTrackEntries(entries, cascade, {}, false, 0.0);
    bool parity = !identical(entries[0].samples, dry);
    for (size_t i = 0; i < dry.size(); ++i)
        parity = parity && entries[0].samples[i] == playback[i] / 32768.0f;

    // Master uses the shared post-sum processor with no premature PCM16 clamp.
    mixer->setMasterDeEsser(chain.deesser);
    mixer->setMasterEq(trackfx::EqSettings{}, false); // must preserve de-esser
    std::vector<int32_t> master(dry.size());
    auto masterExport = dry;
    for (size_t i = 0; i < dry.size(); ++i) {
        master[i] = static_cast<int32_t>(dry[i] * 65536.0f);
        masterExport[i] *= 2.0f;
    }
    mixer->processMasterEqForTest(master.data(), kFrames);
    AudioMixer::MasterEqFilter exportMaster;
    exportMaster.setDeEsser(chain.deesser);
    exportMaster.process(masterExport.data(), kFrames);
    double maxError = 0.0;
    for (size_t i = 0; i < master.size(); ++i)
        maxError = std::max(maxError, std::abs(masterExport[i] - master[i] / 32768.0));
    gate(4, parity && maxError <= 1.01 / 32768.0
        && mixer->masterChain().deesser.enabled);

    // G5: all settings, dormant settings/isDefault, both ProjectFile formats.
    auto stored = chain;
    stored.deesser.frequencyHz = 7500.0;
    stored.deesser.thresholdDb = -31.0;
    stored.deesser.ratio = 7.0;
    stored.deesser.rangeDb = 9.0;
    stored.deesser.listen = true;
    const auto json = stored.toJson();
    bool persisted = trackfx::Chain::fromJson(json).toJson() == json
        && !stored.isDefault() && trackfx::Chain{}.isDefault()
        && !trackfx::Chain{}.toJson().contains("deesser");
    auto dormant = stored;
    dormant.deesser.enabled = false;
    persisted = persisted && !dormant.isDefault()
        && trackfx::Chain::fromJson(dormant.toJson()).toJson() == dormant.toJson();
    QTemporaryDir temporary;
    for (bool enabled : {false, true}) {
        ProjectData project, textLoaded, fileLoaded;
        project.trackFx.insert(0, trackfx::Chain{});
        if (enabled) {
            project.trackFx.insert(0, stored);
            project.masterFx = stored;
        }
        const QString text = ProjectFile::toJsonString(project);
        const QString path = temporary.filePath(QStringLiteral("deesser.veditor"));
        const bool loaded = temporary.isValid() && ProjectFile::fromJsonString(text, textLoaded)
            && ProjectFile::save(path, project) && ProjectFile::load(path, fileLoaded);
        QFile file(path);
        const bool opened = file.open(QIODevice::ReadOnly);
        const QByteArray bytes = file.readAll();
        persisted = persisted && loaded && opened;
        if (enabled) {
            for (const ProjectData *restored : {&textLoaded, &fileLoaded})
                persisted = persisted && restored->trackFx.value(0).toJson() == json
                    && restored->masterFx.toJson() == json;
        } else {
            persisted = persisted && !text.contains(QStringLiteral("deesser"))
                && !bytes.contains("deesser") && textLoaded.trackFx.isEmpty()
                && fileLoaded.trackFx.isEmpty();
        }
    }
    gate(5, persisted);

    // G6: the dock panel applies live. Real MainWindow::openDeEsserPanel wiring:
    // settingsChanged -> AudioMixer chain, per-target edits survive switching.
    {
        MainWindow window;
        auto *mixer = window.m_player ? window.m_player->audioMixer() : nullptr;
        if (window.m_timeline && window.m_timeline->audioTrackCount() < 1)
            window.m_timeline->restoreFromProject(QVector<QVector<ClipInfo>>{},
                QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
        const int audioTracks = window.m_timeline ? window.m_timeline->audioTrackCount() : 0;
        if (mixer && audioTracks >= 1) window.openDeEsserPanel();
        auto *panel = window.m_deesserDock
            ? window.m_deesserDock->findChild<DeEsserPanel *>("DeEsserPanel") : nullptr;
        auto *target = panel ? panel->findChild<QComboBox *>("deesser_target") : nullptr;
        auto *enabledBox = panel ? panel->findChild<QCheckBox *>("deesser_enabled") : nullptr;
        auto *thresholdBox = panel ? panel->findChild<QDoubleSpinBox *>("deesser_threshold") : nullptr;
        const bool built = mixer && panel && target && enabledBox && thresholdBox
            && window.m_deesserDock->objectName() == QStringLiteral("DeEsserDock");
        std::fprintf(stderr, "deesser G6: built=%d audioTracks=%d\n", built, audioTracks);
        bool live = false;
        if (built) {
            const trackfx::DeEsserSettings defaults;
            const int a1 = target->findData(0);
            const int master = target->findData(AudioMixer::kMasterTrackId);
            target->setCurrentIndex(a1);
            QSignalSpy spy(panel, &DeEsserPanel::settingsChanged);
            enabledBox->setChecked(true);
            thresholdBox->setValue(-40.0);
            const auto applied = mixer->trackChain(0).deesser;
            const bool immediate = spy.count() == 2 && applied.enabled
                && applied.thresholdDb == -40.0 && applied.ratio == defaults.ratio;
            const int emittedBeforeSwitch = spy.count();
            target->setCurrentIndex(master);
            const bool masterShownDefault = !enabledBox->isChecked()
                && thresholdBox->value() == defaults.thresholdDb;
            target->setCurrentIndex(a1);
            const bool restored = enabledBox->isChecked() && thresholdBox->value() == -40.0;
            const auto a1After = mixer->trackChain(0).deesser;
            const auto masterAfter = mixer->masterChain().deesser;
            const bool masterDefault = !masterAfter.enabled
                && masterAfter.thresholdDb == defaults.thresholdDb
                && masterAfter.ratio == defaults.ratio;
            const bool noSwitchEmit = spy.count() == emittedBeforeSwitch;
            std::fprintf(stderr,
                "deesser G6: emitted=%d immediate=%d a1.enabled=%d a1.threshold=%.1f "
                "masterShownDefault=%d restored=%d a1After.threshold=%.1f "
                "master.enabled=%d master.threshold=%.1f noSwitchEmit=%d "
                "applied.ratio=%.1f a1After.enabled=%d master.ratio=%.1f masterDefault=%d\n",
                emittedBeforeSwitch, immediate, applied.enabled, applied.thresholdDb,
                masterShownDefault, restored, a1After.thresholdDb,
                masterAfter.enabled, masterAfter.thresholdDb, noSwitchEmit,
                applied.ratio, a1After.enabled, masterAfter.ratio, masterDefault);
            live = immediate && masterShownDefault && restored && masterDefault
                && noSwitchEmit && a1After.enabled && a1After.thresholdDb == -40.0;
        }
        gate(6, live);
    }

    // G7: the process-call counter survives resetFeedback (seek) and keeps growing.
    {
        auto seekChain = chain;
        trackfx::Processor seeker(seekChain, kRate, 2);
        auto first = dry;
        process(seeker, first);
        const uint64_t afterFirst = seeker.deEsserProcessCallsForTest();
        seeker.resetFeedback();
        auto second = dry;
        process(seeker, second);
        const uint64_t afterSecond = seeker.deEsserProcessCallsForTest();
        seeker.resetFeedback();
        const uint64_t afterSeek = seeker.deEsserProcessCallsForTest();
        std::fprintf(stderr, "deesser G7: afterFirst=%llu afterSecond=%llu afterSeek=%llu\n",
                     static_cast<unsigned long long>(afterFirst),
                     static_cast<unsigned long long>(afterSecond),
                     static_cast<unsigned long long>(afterSeek));
        gate(7, afterFirst == static_cast<uint64_t>(kFrames)
            && afterSecond == 2u * static_cast<uint64_t>(kFrames)
            && afterSeek == afterSecond);
    }

    // G8: an open dock follows project load / new and track add / remove, and a
    // stale panel never overwrites the new mixer state on the next edit.
    {
        MainWindow window;
        auto *mixer = window.m_player ? window.m_player->audioMixer() : nullptr;
        if (window.m_timeline && window.m_timeline->audioTrackCount() < 1)
            window.m_timeline->restoreFromProject(QVector<QVector<ClipInfo>>{},
                QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
        if (mixer) window.openDeEsserPanel();
        auto *panel = window.m_deesserDock
            ? window.m_deesserDock->findChild<DeEsserPanel *>("DeEsserPanel") : nullptr;
        auto *target = panel ? panel->findChild<QComboBox *>("deesser_target") : nullptr;
        auto *enabledBox = panel ? panel->findChild<QCheckBox *>("deesser_enabled") : nullptr;
        auto *thresholdBox = panel ? panel->findChild<QDoubleSpinBox *>("deesser_threshold") : nullptr;
        bool ok = false;
        if (mixer && panel && target && enabledBox && thresholdBox) {
            target->setCurrentIndex(target->findData(0));
            enabledBox->setChecked(true);
            const bool enabledBefore = mixer->trackChain(0).deesser.enabled;
            // Project load / new: de-esser disabled state arrives via applyAudioState.
            window.applyAudioState(ProjectData{});
            const bool mixerOff = !mixer->trackChain(0).deesser.enabled;
            const bool panelOff = !enabledBox->isChecked();
            QSignalSpy spy(panel, &DeEsserPanel::settingsChanged);
            thresholdBox->setValue(-33.0);
            const auto after = mixer->trackChain(0).deesser;
            const bool noOverwrite = spy.count() == 1 && !after.enabled
                && after.thresholdDb == -33.0;
            // Track add / remove: target list follows, removed target falls back.
            const int comboBefore = target->count();
            window.addAudioTrack();
            const int comboAdded = target->count();
            target->setCurrentIndex(target->findData(1));
            const int selectedBeforeRemove = panel->currentTrackId();
            QString err;
            const bool removed = window.m_timeline->removeTrack(true, 1, &err);
            const int comboRemoved = target->count();
            const int selectedAfterRemove = panel->currentTrackId();
            const bool selectedValid = target->findData(selectedAfterRemove) >= 0;
            // Timeline::addClip with an audio-only clip auto-creates A2 (the only track
            // is occupied) without any remap signal; the dock must follow.
            const int tracksBeforeImport = window.m_timeline->audioTrackCount();
            const int comboBeforeImport = target->count();
            QTemporaryDir importDir;
            const QString wavPath = importDir.filePath(QStringLiteral("g8.wav"));
            bool wavOk = importDir.isValid();
            if (wavOk) {
                QFile wav(wavPath);
                wavOk = wav.open(QIODevice::WriteOnly);
                if (wavOk) {
                    const int dataBytes = 48000 * 2;
                    QByteArray header("RIFF", 4);
                    const auto le32 = [&](quint32 v) {
                        for (int i = 0; i < 4; ++i) header.append(char((v >> (8 * i)) & 0xff));
                    };
                    const auto le16 = [&](quint16 v) {
                        header.append(char(v & 0xff)); header.append(char(v >> 8));
                    };
                    le32(36 + dataBytes); header.append("WAVEfmt ", 8);
                    le32(16); le16(1); le16(1); le32(48000); le32(96000); le16(2); le16(16);
                    header.append("data", 4); le32(dataBytes);
                    wav.write(header);
                    wav.write(QByteArray(dataBytes, '\0'));
                }
            }
            // Occupy every existing audio track so addClip must create a new one.
            ClipInfo filler;
            filler.filePath = wavPath;
            filler.duration = filler.outPoint = 1.0;
            filler.speed = 1.0;
            QVector<QVector<ClipInfo>> occupied;
            for (int t = 0; t < tracksBeforeImport; ++t) occupied.append(QVector<ClipInfo>{filler});
            window.m_timeline->restoreFromProject(QVector<QVector<ClipInfo>>{}, occupied,
                0.0, -1.0, -1.0, 100);
            window.m_timeline->importMedia(wavPath);
            const bool grew = QTest::qWaitFor([&]() {
                return target->count() == comboBeforeImport + 1; }, 3000);
            const int tracksAfterImport = window.m_timeline->audioTrackCount();
            std::fprintf(stderr,
                "deesser G8 addClip: wavOk=%d tracksBefore=%d tracksAfter=%d comboBefore=%d "
                "comboAfter=%d grew=%d\n", wavOk, tracksBeforeImport, tracksAfterImport,
                comboBeforeImport, target->count(), grew);
            std::fprintf(stderr,
                "deesser G8: enabledBefore=%d mixerOff=%d panelOff=%d spy=%d after.enabled=%d "
                "after.threshold=%.1f comboBefore=%d comboAdded=%d selectedBeforeRemove=%d "
                "removed=%d comboRemoved=%d selectedAfterRemove=%d selectedValid=%d\n",
                enabledBefore, mixerOff, panelOff, static_cast<int>(spy.count()),
                after.enabled, after.thresholdDb, comboBefore, comboAdded,
                selectedBeforeRemove, removed, comboRemoved, selectedAfterRemove, selectedValid);
            ok = enabledBefore && mixerOff && panelOff && noOverwrite
                && comboBefore == 2 && comboAdded == 3 && selectedBeforeRemove == 1
                && removed && comboRemoved == 2 && selectedValid
                && wavOk && grew && tracksAfterImport == tracksBeforeImport + 1;
        } else {
            std::fprintf(stderr, "deesser G8: dock not built\n");
        }
        gate(8, ok);
    }
    return tally.finish("deesser");
}
