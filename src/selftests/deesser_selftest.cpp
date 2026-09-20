#include "../AudioTrackFx.h"
#include "../AudioMixer.h"
#include "../MainWindow.h"
#include "../ProjectFile.h"

#include <QFile>
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
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };

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
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
