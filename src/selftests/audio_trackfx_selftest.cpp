#include "../AudioTrackFx.h"
#include "../AudioMixer.h"
#include "../SpectralEngine.h"

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

std::vector<float> tone(bool dual = false)
{
    std::vector<float> samples(kFrames * 2);
    for (int f = 0; f < kFrames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        const double wave = dual ? 0.5 * (std::sin(2 * kPi * 100 * t)
            + std::sin(2 * kPi * 1000 * t)) : std::sin(2 * kPi * 1000 * t);
        const auto pcm = static_cast<int16_t>(32768 * std::pow(10.0, -6.0 / 20.0) * wave);
        samples[2 * f] = samples[2 * f + 1] = pcm / 32768.0f;
    }
    return samples;
}

bool identical(const std::vector<float> &a, const std::vector<float> &b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

void processBlocks(trackfx::Processor &p, std::vector<float> &samples)
{
    // Auto-floor intentionally samples once per fragment, as in playback.
    for (int f = 0; f < kFrames; f += 257)
        p.process(samples.data() + 2 * f, std::min(257, kFrames - f));
}

void configure(AudioMixer &mixer, const trackfx::Chain &chain)
{
    mixer.setEqForTrack(1, chain.eq);
    mixer.setCompressorForTrack(1, chain.comp);
    mixer.setReverbForTrack(1, chain.reverb);
    mixer.setNoiseReductionForTrack(1, chain.nr);
}

double spectralAmplitude(const std::vector<float> &samples, double hz)
{
    constexpr int n = 32768;
    const auto window = spectral::hannWindow(n);
    std::vector<std::complex<double>> bins(n);
    for (int i = 0; i < n; ++i)
        bins[i] = samples[2 * (kFrames - n + i)] * window[i];
    spectral::fft(bins, false);
    return std::abs(bins[spectral::hzToBin(hz, n, kRate)]);
}

double energy(const std::vector<float> &samples, int firstFrame, int lastFrame)
{
    double sum = 0;
    for (int i = 2 * firstFrame; i < 2 * lastFrame; ++i)
        sum += static_cast<double>(samples[i]) * samples[i];
    return sum;
}
} // namespace

int runAudioTrackFxSelftest()
{
    int pass = 0, fail = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++pass : ++fail;
    };
    trackfx::Chain chain;
    chain.eqEnabled = chain.compEnabled = chain.reverbEnabled = chain.nrEnabled = true;
    chain.eq.lowMid = {100.0, -12.0, 1.0, true};
    chain.comp.enabled = true;
    chain.comp.thresholdDb = -20;
    chain.comp.ratio = 4;
    chain.reverb.enabled = true;
    chain.reverb.mixRatio = 0.15;
    chain.reverb.decaySeconds = 0.5;
    chain.nr.enabled = true;
    const auto input = tone(true);
    trackfx::Processor processor(chain, kRate, 2);
    auto actual = input;
    processBlocks(processor, actual);

    // These instances never create a sink or open media. Both enter the same
    // locked dispatcher as readData; the legacy oracle is retained production
    // code in AudioMixer.cpp, not a reimplementation in this test.
    auto legacy = std::make_unique<AudioMixer>();
    auto current = std::make_unique<AudioMixer>();
    legacy->setLegacyTrackFxPathForTest(true);
    configure(*legacy, chain);
    configure(*current, chain);
    std::vector<int16_t> oldPcm(input.size()), newPcm(input.size());
    for (size_t i = 0; i < input.size(); ++i)
        oldPcm[i] = newPcm[i] = static_cast<int16_t>(input[i] * 32768.0f);
    for (int f = 0; f < kFrames; f += 257) {
        const int count = std::min(257, kFrames - f);
        legacy->processTrackFxForTest(1, oldPcm.data() + 2 * f, count);
        current->processTrackFxForTest(1, newPcm.data() + 2 * f, count);
    }
    bool parity = oldPcm == newPcm && legacy->trackFxProcessCallsForTest() == 0
        && current->trackFxProcessCallsForTest() > 0;
    for (size_t i = 0; i < actual.size(); ++i)
        parity = parity && actual[i] == oldPcm[i] / 32768.0f;
    parity = parity && processor.currentGainReductionDb() == current->currentGainReductionDb(1)
        && legacy->currentGainReductionDb(1) == current->currentGainReductionDb(1)
        && legacy->estimatedNoiseFloorDb(1) == current->estimatedNoiseFloorDb(1);
    // Live setting swaps must preserve the histories in both runtime paths.
    chain.eq.lowMid.gainDb = -4;
    chain.comp.thresholdDb = -18;
    chain.reverb.mixRatio = 0.25;
    chain.nr.autoFloor = false;
    configure(*legacy, chain);
    configure(*current, chain);
    processor.setChain(chain);
    auto changed = input;
    processBlocks(processor, changed);
    for (size_t i = 0; i < input.size(); ++i)
        oldPcm[i] = newPcm[i] = static_cast<int16_t>(input[i] * 32768.0f);
    for (int f = 0; f < kFrames; f += 257) {
        const int count = std::min(257, kFrames - f);
        legacy->processTrackFxForTest(1, oldPcm.data() + 2 * f, count);
        current->processTrackFxForTest(1, newPcm.data() + 2 * f, count);
    }
    parity = parity && oldPcm == newPcm;
    for (size_t i = 0; i < changed.size(); ++i)
        parity = parity && changed[i] == oldPcm[i] / 32768.0f;
    const trackfx::Chain exported = current->trackChain(1);
    parity = parity && exported.eq.lowMid.gainDb == -4 && exported.compEnabled
        && exported.reverbEnabled && exported.nrEnabled
        && !current->trackChain(99).eqEnabled;
    gate(1, parity);

    auto bypass = input;
    bypass[0] = 0.1234567f; // must also preserve precision beyond PCM16
    bypass[1] = -0.0f;
    const auto exact = bypass;
    auto disabled = chain;
    disabled.nrEnabled = disabled.eqEnabled = disabled.compEnabled = disabled.reverbEnabled = false;
    trackfx::Processor off(disabled, kRate, 2);
    processBlocks(off, bypass);
    trackfx::Processor defaults;
    processBlocks(defaults, bypass);
    gate(2, identical(exact, bypass));

    trackfx::Chain eq;
    eq.eqEnabled = true;
    eq.eq.lowMid = {100.0, -12.0, 1.0, true};
    trackfx::Processor equalizer(eq, kRate, 2);
    auto filtered = input;
    processBlocks(equalizer, filtered);
    const double lowDb = 20 * std::log10(spectralAmplitude(filtered, 100) / spectralAmplitude(input, 100));
    const double midDb = 20 * std::log10(spectralAmplitude(filtered, 1000) / spectralAmplitude(input, 1000));
    std::fprintf(stderr, "EQ: 100Hz %.3f dB, 1kHz %.3f dB\n", lowDb, midDb);
    gate(3, std::abs(lowDb + 12) <= 1.5 && std::abs(midDb) <= 0.5);

    trackfx::Chain comp;
    comp.compEnabled = comp.comp.enabled = true;
    comp.comp.thresholdDb = -20;
    comp.comp.ratio = 4;
    trackfx::Processor compressor(comp, kRate, 2);
    const auto sine = tone();
    auto compressed = sine;
    processBlocks(compressor, compressed);
    const double gainDb = 10 * std::log10(energy(compressed, kRate, kFrames) / energy(sine, kRate, kFrames));
    std::fprintf(stderr, "Compressor: %.3f dB\n", gainDb);
    gate(4, std::abs(gainDb + 10.5) <= 1.5);

    trackfx::Chain rev;
    rev.reverbEnabled = rev.reverb.enabled = true;
    rev.reverb.mixRatio = 0.5;
    rev.reverb.decaySeconds = 0.5;
    trackfx::Processor reverb(rev, kRate, 2);
    std::vector<float> impulse(kFrames * 2, 0.0f);
    impulse[0] = impulse[1] = 0.5f;
    processBlocks(reverb, impulse);
    const double early = energy(impulse, 1000, 12000);
    const double late = energy(impulse, 24000, 48000);
    gate(5, early > 0 && late < early && energy(impulse, 1, kFrames) > 0);

    processor.reset();
    auto first = input;
    processBlocks(processor, first);
    processor.reset();
    auto second = input;
    processBlocks(processor, second);
    gate(6, identical(first, second));
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", pass, fail);
    return fail;
}
