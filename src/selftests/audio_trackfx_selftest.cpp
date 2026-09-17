#include "../AudioTrackFx.h"
#include "../ProjectFile.h"
#include <QJsonDocument>
#include "../AudioMixer.h"
#include "../SpectralEngine.h"
#include "../Timeline.h"
#include "../MainWindow.h"
#include "../EqualizerPanel.h"
#include "../CompressorPanel.h"
#include "../ReverbPanel.h"
#include "../NoiseReductionPanel.h"
#include <QComboBox>
#include <QSignalBlocker>
#include <QSlider>
#include "../RenderInPlace.h"
#include "../libavcore/AudioExtract.h"
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
bool sameChain(const trackfx::Chain &a, const trackfx::Chain &b)
{
    const auto band = [](const trackfx::EqBand &x, const trackfx::EqBand &y) {
        return x.freq == y.freq && x.gainDb == y.gainDb && x.q == y.q && x.enabled == y.enabled;
    };
    return band(a.eq.low, b.eq.low) && band(a.eq.lowMid, b.eq.lowMid)
        && band(a.eq.highMid, b.eq.highMid) && band(a.eq.high, b.eq.high)
        && a.nrEnabled == b.nrEnabled
        && a.eqEnabled == b.eqEnabled
        && a.compEnabled == b.compEnabled
        && a.reverbEnabled == b.reverbEnabled
        && a.comp.thresholdDb == b.comp.thresholdDb
        && a.comp.ratio == b.comp.ratio
        && a.comp.attackMs == b.comp.attackMs
        && a.comp.releaseMs == b.comp.releaseMs
        && a.comp.kneeDb == b.comp.kneeDb
        && a.comp.makeupDb == b.comp.makeupDb
        && a.comp.enabled == b.comp.enabled
        && a.reverb.mixRatio == b.reverb.mixRatio
        && a.reverb.decaySeconds == b.reverb.decaySeconds
        && a.reverb.preDelayMs == b.reverb.preDelayMs
        && a.reverb.dampingHF == b.reverb.dampingHF
        && a.reverb.widthPercent == b.reverb.widthPercent
        && a.reverb.enabled == b.reverb.enabled
        && a.nr.thresholdDb == b.nr.thresholdDb
        && a.nr.reductionDb == b.nr.reductionDb
        && a.nr.attackMs == b.nr.attackMs
        && a.nr.releaseMs == b.nr.releaseMs
        && a.nr.manualFloorDb == b.nr.manualFloorDb
        && a.nr.autoFloor == b.nr.autoFloor
        && a.nr.enabled == b.nr.enabled;
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

    const QString pre = buildExportAudioMixEntryPreFxFilterChain(0,
        QStringLiteral("1.000000"), QStringLiteral("3.000000"),
        AudioChannelMode::Swap, true, 2.0);
    const QString post = buildExportAudioMixEntryPostFxFilterChain(0, 500,
        QStringLiteral("0.500000"), 1.0, TransitionType::FadeIn, 0.25,
        TransitionType::FadeOut, 0.25);
    const QString entryChain = buildExportAudioMixEntryFilterChain(0,
        QStringLiteral("1.000000"), QStringLiteral("3.000000"), 500,
        QStringLiteral("0.500000"), AudioChannelMode::Swap, true, 2.0,
        TransitionType::FadeIn, 0.25, TransitionType::FadeOut, 0.25);
    QString joined = pre;
    joined.chop(QStringLiteral("[prefx0]").size());
    joined += QLatin1Char(',') + post.mid(QStringLiteral("[0:a]").size());
    gate(7, joined == entryChain
        && pre.contains(QStringLiteral("atrim="))
        && pre.contains(QStringLiteral("aresample=48000"))
        && pre.contains(QStringLiteral("aformat="))
        && pre.contains(QStringLiteral("areverse,atempo=2"))
        && pre.contains(exportAudioChannelPanFilterForMode(AudioChannelMode::Swap))
        && !pre.contains(QStringLiteral("volume="))
        && !pre.contains(QStringLiteral("afade="))
        && !pre.contains(QStringLiteral("adelay="))
        && !pre.contains(QStringLiteral("amix="))
        && post.contains(QStringLiteral("volume='0.500000'"))
        && post.contains(QStringLiteral("afade=t=in"))
        && post.contains(QStringLiteral("afade=t=out:curve=qsin:st=0.75:d=0.25"))
        && post.contains(QStringLiteral("adelay=500"))
        && !post.contains(QStringLiteral("atrim=")));

    QTemporaryDir directory;
    const QString wav = directory.filePath(QStringLiteral("float.wav"));
    auto precise = input;
    precise[0] = 0.1234567f;
    precise[1] = -0.0f;
    precise[2] = 1.25f; // no clipping at WAV boundaries
    std::vector<float> roundtrip;
    int rate = 0, channels = 0;
    QString error;
    const bool wrote = directory.isValid()
        && libavcore::writeWavFloatInterleaved(wav, precise, kRate, 2, &error);
    const bool read = wrote
        && libavcore::readWavFloatInterleaved(wav, &roundtrip, &rate, &channels, &error);
    bool wavOk = read && rate == kRate && channels == 2 && identical(precise, roundtrip);
    // The export decoder must also accept FFmpeg's extensible float header.
    QFile original(wav);
    QByteArray extensible;
    if (original.open(QIODevice::ReadOnly)) extensible = original.readAll();
    original.close();
    if (extensible.size() >= 56) {
        const QByteArray extension = QByteArray::fromHex("16002000030000000300000000001000800000aa00389b71");
        extensible.insert(36, extension);
        extensible[16] = 40;
        extensible[20] = char(0xfe); extensible[21] = char(0xff);
        const quint32 riffSize = static_cast<quint32>(extensible.size() - 8);
        for (int i = 0; i < 4; ++i) extensible[4 + i] = char(riffSize >> (8 * i));
        QFile alternate(directory.filePath(QStringLiteral("extensible.wav")));
        const bool saved = alternate.open(QIODevice::WriteOnly)
            && alternate.write(extensible) == extensible.size();
        alternate.close();
        wavOk = wavOk && saved && libavcore::readWavFloatInterleaved(alternate.fileName(),
            &roundtrip, &rate, &channels, &error) && identical(precise, roundtrip);
    } else {
        wavOk = false;
    }
    gate(8, wavOk);

    std::vector<float> fromWav, processedWav;
    bool dspOk = libavcore::writeWavFloatInterleaved(wav, input, kRate, 2, &error)
        && libavcore::readWavFloatInterleaved(wav, &fromWav, &rate, &channels, &error);
    double wavDb = 0.0;
    if (dspOk) {
        trackfx::Processor exportProcessor(eq, rate, channels);
        exportProcessor.reset();
        processBlocks(exportProcessor, fromWav);
        dspOk = libavcore::writeWavFloatInterleaved(wav, fromWav, rate, channels, &error)
            && libavcore::readWavFloatInterleaved(wav, &processedWav, &rate, &channels, &error);
        if (dspOk) {
            wavDb = 20 * std::log10(spectralAmplitude(processedWav, 100) / spectralAmplitude(input, 100));
            dspOk = std::abs(wavDb + 12) <= 1.5 && identical(fromWav, processedWav);
        }
    }
    std::fprintf(stderr, "WAV EQ: 100Hz %.3f dB %s\n", wavDb, error.toUtf8().constData());
    gate(9, dspOk);
    // Level-dependent DSP must see the source level, before clip gain/fades.
    auto playbackComp = std::make_unique<AudioMixer>();
    configure(*playbackComp, comp);
    auto dspThenGain = sine;
    auto gainThenDsp = sine;
    std::vector<int16_t> playbackPcm(sine.size());
    for (size_t i = 0; i < sine.size(); ++i) {
        playbackPcm[i] = static_cast<int16_t>(sine[i] * 32768.0f);
        gainThenDsp[i] *= 0.5f;
    }
    trackfx::Processor beforeGain(comp, kRate, 2), afterGain(comp, kRate, 2);
    processBlocks(beforeGain, dspThenGain);
    processBlocks(afterGain, gainThenDsp);
    for (int f = 0; f < kFrames; f += 257)
        playbackComp->processTrackFxForTest(1, playbackPcm.data() + 2 * f,
                                           std::min(257, kFrames - f));
    bool orderOk = true;
    for (size_t i = 0; i < sine.size(); ++i) {
        dspThenGain[i] *= 0.5f;
        orderOk = orderOk && dspThenGain[i] == (playbackPcm[i] / 32768.0f) * 0.5f;
    }
    const double orderDb = 10 * std::log10(energy(gainThenDsp, kRate, kFrames)
                                         / energy(dspThenGain, kRate, kFrames));
    // Persisted EQ enablement is independent of the newer track Chain.
    AudioEQConfig persistedEq;
    persistedEq.bands = {{80.0, 0.0, 0.7}, {100.0, -12.0, 1.0}, {10000.0, 0.0, 0.7}};
    persistedEq.preamp = -3.0;
    playbackComp->setTrackEqConfig(0, persistedEq);
    playbackComp->setTrackEqEnabled(0, true);
    orderOk = orderOk && playbackComp->trackEqEnabled(0)
        && !playbackComp->trackEqConfig(0).isDefault()
        && playbackComp->trackEqConfig(0).preamp == -3.0
        && !playbackComp->trackChain(0).eqEnabled
        && !playbackComp->trackEqEnabled(-1) && !playbackComp->trackEqEnabled(99);
    playbackComp->setTrackEqEnabled(0, false);
    orderOk = orderOk && !playbackComp->trackEqEnabled(0);
    std::fprintf(stderr, "Compressor gain-order difference: %.3f dB\n", orderDb);
    gate(10, orderOk && std::abs(orderDb) > 1.0);
    // Exercise the actual export cascade with headroom above full scale between
    // the peaking band and preamp. PCM16 at that boundary would lose ~6 dB.
    AudioEQConfig boostEq;
    boostEq.bands = {{80.0, 0.0, 0.7}, {100.0, 12.0, 1.0}, {10000.0, 0.0, 0.7}};
    boostEq.preamp = -12.0;
    playbackComp->setTrackEqConfig(0, boostEq);
    playbackComp->setTrackEqEnabled(0, true);
    std::vector<float> lowTone(kFrames * 2);
    for (int f = 0; f < kFrames; ++f)
        lowTone[2 * f] = lowTone[2 * f + 1] = static_cast<float>(0.5 * std::sin(2 * kPi * 100 * f / kRate));
    std::vector<audioexport::DspEntry> boosted{{0, lowTone}};
    audioexport::processTrackEntries(boosted, {}, playbackComp->trackEqCoeffs(0),
                                     true, boostEq.preamp);
    double peak = 0;
    for (float value : boosted[0].samples) peak = std::max(peak, std::abs(static_cast<double>(value)));
    const double boostDb = 20 * std::log10(spectralAmplitude(boosted[0].samples, 100)
                                          / spectralAmplitude(lowTone, 100));
    std::fprintf(stderr, "Legacy EQ headroom: peak %.6f, 100Hz %.3f dB\n", peak, boostDb);
    gate(11, peak <= 0.55 && std::abs(boostDb) <= 0.5);

    // A: 0..2s, B: 1..3s. Different source levels expose incorrect compressor
    // history when a whole clip is processed ahead of an overlapping clip.
    auto quiet = sine;
    for (float &value : quiet)
        value = static_cast<int16_t>(value * 32768.0f * 0.125f) / 32768.0f;
    std::vector<audioexport::DspEntry> overlapping{{0, sine}, {kRate, quiet}};
    auto overlapPlayback = std::make_unique<AudioMixer>();
    configure(*overlapPlayback, comp);
    std::array<std::vector<int16_t>, 2> oracle;
    for (int entry = 0; entry < 2; ++entry) {
        oracle[entry].resize(overlapping[entry].samples.size());
        for (size_t i = 0; i < oracle[entry].size(); ++i)
            oracle[entry][i] = static_cast<int16_t>(overlapping[entry].samples[i] * 32768.0f);
    }
    for (int t = 0; t < 3 * kRate; t += 1024) {
        for (int entry = 0; entry < 2; ++entry) {
            const int start = entry * kRate;
            const int first = std::max(t, start);
            const int last = std::min(t + 1024, start + kFrames);
            if (first < last)
                overlapPlayback->processTrackFxForTest(1,
                    oracle[entry].data() + 2 * (first - start), last - first);
        }
    }
    audioexport::processTrackEntries(overlapping, comp, {}, false, 0.0);
    bool overlapOk = true;
    for (int entry = 0; entry < 2; ++entry)
        for (size_t i = 0; i < oracle[entry].size(); ++i)
            overlapOk = overlapOk && overlapping[entry].samples[i] == oracle[entry][i] / 32768.0f;
    gate(12, overlapOk);

    // End-to-end export: the public wrapper runs the production FFmpeg PreFx,
    // persisted EQ and PostFx/master mix path. Its WAV intermediate is PCM16.
    const QString sourcePath = directory.filePath(QStringLiteral("export-source.wav"));
    const QString outputPath = directory.filePath(QStringLiteral("export-eq.wav"));
    QString exportError;
    bool exportOk = directory.isValid()
        && libavcore::writeWavFloatInterleaved(sourcePath, input, kRate, 2, &exportError);
    auto exportMixer = std::make_unique<AudioMixer>();
    Timeline exportTimeline;
    ClipInfo audioClip;
    audioClip.filePath = sourcePath;
    audioClip.duration = 2.0;
    audioClip.inPoint = 0.0;
    audioClip.outPoint = 2.0;
    exportTimeline.restoreFromProject(QVector<QVector<ClipInfo>>{{}},
        QVector<QVector<ClipInfo>>{{audioClip}}, 0, -1, -1, 10);
    exportTimeline.setAudioMixer(exportMixer.get());
    AudioEQConfig cutEq;
    cutEq.bands = {{80.0, 0.0, 0.7}, {100.0, -12.0, 1.0}, {10000.0, 0.0, 0.7}};
    exportMixer->setTrackEqConfig(0, cutEq);
    exportMixer->setTrackEqEnabled(0, true);
    exportOk = exportOk && renderinplace::prepareAudioMix(&exportTimeline, outputPath,
                                                         2.0, &exportError) == outputPath;
    std::vector<double> exportedMono;
    int exportRate = 0;
    exportOk = exportOk && libavcore::readPcm16WavToMono(outputPath, exportedMono,
                                                       exportRate, &exportError);
    double exportLowDb = 0.0, exportMidDb = 0.0;
    exportOk = exportOk && exportRate == kRate && exportedMono.size() >= kFrames;
    if (exportOk) {
        std::vector<float> exportedStereo(kFrames * 2);
        for (int f = 0; f < kFrames; ++f)
            exportedStereo[2 * f] = exportedStereo[2 * f + 1] = static_cast<float>(exportedMono[f]);
        exportLowDb = 20 * std::log10(spectralAmplitude(exportedStereo, 100) / spectralAmplitude(input, 100));
        exportMidDb = 20 * std::log10(spectralAmplitude(exportedStereo, 1000) / spectralAmplitude(input, 1000));
        exportOk = std::abs(exportLowDb + 12) <= 1.5 && std::abs(exportMidDb) <= 1.0;
    }
    std::fprintf(stderr, "Export persisted EQ: 100Hz %.3f dB, 1kHz %.3f dB, error: %s\n",
                 exportLowDb, exportMidDb, exportError.toUtf8().constData());
    gate(13, exportOk && exportError.isEmpty());
    // G14: use the actual list builder/selector adapter used by all panels.
    QStringList panelNames;
    QList<int> panelIds;
    audiofxui::buildAudioTrackList(2, panelNames, panelIds);
    bool idsOk = panelNames == QStringList{QStringLiteral("マスター"),
        QStringLiteral("A1"), QStringLiteral("A2")}
        && panelIds == QList<int>{AudioMixer::kMasterTrackId, 0, 1};
    EqualizerPanel eqPanel;
    audiofxui::suppressEqualizerSelectionWrites(&eqPanel);
    int eqWrites = 0;
    int lastEqId = 99;
    QObject::connect(&eqPanel, &EqualizerPanel::eqChanged, &eqPanel,
        [&](int id, AudioMixer::EqSettings) { ++eqWrites; lastEqId = id; });
    {
        const QSignalBlocker blocker(&eqPanel);
        eqPanel.setTracks(panelNames, panelIds);
    }
    auto *eqCombo = eqPanel.findChild<QComboBox *>();
    idsOk = idsOk && eqCombo && eqCombo->itemData(0).toInt() == AudioMixer::kMasterTrackId
        && eqCombo->itemData(1).toInt() == 0 && eqCombo->itemData(2).toInt() == 1;
    if (eqCombo) {
        eqCombo->setCurrentIndex(1);
        eqCombo->setCurrentIndex(0);
        idsOk = idsOk && eqWrites == 0;
        auto *gain = eqPanel.findChild<QSlider *>();
        if (gain) gain->setValue(-12);
        idsOk = idsOk && eqWrites == 1 && lastEqId == AudioMixer::kMasterTrackId;
        eqCombo->setCurrentIndex(1);
        if (gain) gain->setValue(-6);
        idsOk = idsOk && eqWrites == 2 && lastEqId == 0;
    }
    audiofxui::buildAudioTrackList(2, panelNames, panelIds, false);
    CompressorPanel compPanel;
    ReverbPanel reverbPanel;
    NoiseReductionPanel nrPanel;
    const auto checkPanel = [&](auto &panel) {
        auto *combo = panel.template findChild<QComboBox *>();
        audiofxui::setTrackComboItems(combo, panelNames, panelIds);
        if (!combo || combo->count() != 2 || combo->findData(AudioMixer::kMasterTrackId) >= 0)
            return false;
        for (int i = 0; i < 2; ++i) {
            combo->setCurrentIndex(i);
            if (panel.currentTrackId() != i || combo->currentText() != QStringLiteral("A%1").arg(i + 1))
                return false;
        }
        audiofxui::setTrackComboItems(combo, {}, {});
        return combo->count() == 0;
    };
    idsOk = checkPanel(compPanel) && checkPanel(reverbPanel) && checkPanel(nrPanel) && idsOk;
    AudioMixer::EqSettings masterEq;
    masterEq.lowMid = {100.0, -12.0, 1.0, true};
    std::vector<int16_t> trackOne(input.size()), trackTwo(input.size());
    std::vector<float> drySum(input.size());
    for (int f = 0; f < kFrames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        for (int ch = 0; ch < 2; ++ch) {
            trackOne[2 * f + ch] = static_cast<int16_t>(6000 * std::sin(2 * kPi * 100 * t));
            trackTwo[2 * f + ch] = static_cast<int16_t>(6000 * std::sin(2 * kPi * 1000 * t));
            drySum[2 * f + ch] = (trackOne[2 * f + ch] + trackTwo[2 * f + ch]) / 32768.0f;
        }
    }
    const auto originalOne = trackOne;
    const auto originalTwo = trackTwo;
    auto indexedMixer = std::make_unique<AudioMixer>();
    indexedMixer->setEqForTrack(0, masterEq);
    indexedMixer->processTrackFxForTest(0, trackOne.data(), kFrames);
    indexedMixer->processTrackFxForTest(1, trackTwo.data(), kFrames);
    std::vector<float> trackSum(input.size());
    for (size_t i = 0; i < trackSum.size(); ++i)
        trackSum[i] = (trackOne[i] + trackTwo[i]) / 32768.0f;
    const auto hasCut = [&](const std::vector<float> &wet, const std::vector<float> &dry) {
        const double low = 20 * std::log10(spectralAmplitude(wet, 100) / spectralAmplitude(dry, 100));
        const double mid = 20 * std::log10(spectralAmplitude(wet, 1000) / spectralAmplitude(dry, 1000));
        return std::abs(low + 12) <= 1.5 && std::abs(mid) <= 1.0;
    };
    gate(14, idsOk && trackOne != originalOne && trackTwo == originalTwo
        && hasCut(trackSum, drySum) && !indexedMixer->masterChain().eqEnabled);

    // G15: bypass the new production branch, rather than duplicating old DSP.
    // These are the same int32 post-sum buffers consumed by readData.
    auto masterMixer = std::make_unique<AudioMixer>();
    std::vector<int32_t> masterDry(input.size());
    for (size_t i = 0; i < masterDry.size(); ++i)
        masterDry[i] = originalOne[i] + originalTwo[i];
    auto masterReference = masterDry;
    masterMixer->setMasterEqBypassForTest(true);
    masterMixer->processMasterEqForTest(masterReference.data(), kFrames);
    bool masterOk = masterMixer->masterEqProcessCallsForTest() == 0;
    masterMixer->setMasterEqBypassForTest(false);
    auto masterDisabled = masterDry;
    masterMixer->processMasterEqForTest(masterDisabled.data(), kFrames);
    masterOk = masterOk && masterReference == masterDisabled
        && masterMixer->masterEqProcessCallsForTest() == 0;
    masterMixer->setMasterEq(masterEq, false);
    masterMixer->processMasterEqForTest(masterDisabled.data(), kFrames);
    masterOk = masterOk && masterReference == masterDisabled
        && masterMixer->masterEqProcessCallsForTest() == 0;
    masterMixer->setMasterEq(masterEq, true);
    auto masterWet = masterDry;
    for (int f = 0; f < kFrames; f += 257)
        masterMixer->processMasterEqForTest(masterWet.data() + 2 * f, std::min(257, kFrames - f));
    std::vector<float> masterOutput(masterWet.size());
    for (size_t i = 0; i < masterWet.size(); ++i)
        masterOutput[i] = masterWet[i] / 32768.0f;
    const auto masterSettings = masterMixer->masterChain();

    // Two in-phase tracks exceed s16 before the master cut. Neither the
    // input nor the output of the master filter may clip to that range.
    std::vector<float> loudTrack(input.size()), loudDry(input.size());
    std::vector<int32_t> loudSum(input.size());
    for (int f = 0; f < kFrames; ++f) {
        const auto sample = static_cast<int16_t>(24000 * std::sin(2 * kPi * 100 * f / kRate));
        for (int ch = 0; ch < 2; ++ch) {
            const int i = 2 * f + ch;
            loudTrack[i] = sample / 32768.0f;
            loudSum[i] = 2 * static_cast<int32_t>(sample);
            loudDry[i] = loudSum[i] / 32768.0f;
        }
    }
    AudioMixer::MasterEqFilter flatMaster;
    flatMaster.setEq(AudioMixer::EqSettings{});
    auto flatInt = loudSum;
    auto flatFloat = loudDry;
    flatMaster.process(flatInt.data(), kFrames);
    flatMaster.process(flatFloat.data(), kFrames);
    bool headroomOk = flatInt == loudSum && identical(flatFloat, loudDry);
    auto loudMixer = std::make_unique<AudioMixer>();
    loudMixer->setMasterEq(masterEq, true);
    for (int f = 0; f < kFrames; f += 257)
        loudMixer->processMasterEqForTest(loudSum.data() + 2 * f, std::min(257, kFrames - f));
    std::vector<float> loudWet(input.size());
    double loudPeak = 0;
    for (size_t i = 0; i < loudWet.size(); ++i) {
        loudWet[i] = loudSum[i] / 32768.0f;
        loudPeak = std::max(loudPeak, std::abs(static_cast<double>(loudSum[i])));
    }
    const double loudAmplitude = spectralAmplitude(loudWet, 100);
    const double loudCutDb = 20 * std::log10(loudAmplitude / spectralAmplitude(loudDry, 100));
    headroomOk = headroomOk && loudPeak < 32767 && std::abs(loudCutDb + 12) <= 0.1;

    const QString loudPath = directory.filePath(QStringLiteral("master-headroom-source.wav"));
    const QString loudOutputPath = directory.filePath(QStringLiteral("master-headroom-output.wav"));
    QString loudError;
    bool loudExportOk = libavcore::writeWavFloatInterleaved(loudPath, loudTrack, kRate, 2, &loudError);
    Timeline loudTimeline;
    ClipInfo loudClip = audioClip;
    loudClip.filePath = loudPath;
    loudTimeline.restoreFromProject(QVector<QVector<ClipInfo>>{{}},
        QVector<QVector<ClipInfo>>{{loudClip}, {loudClip}}, 0, -1, -1, 10);
    loudTimeline.setAudioMixer(loudMixer.get());
    loudExportOk = loudExportOk && renderinplace::prepareAudioMix(&loudTimeline,
        loudOutputPath, 2.0, &loudError) == loudOutputPath;
    std::vector<double> loudMono;
    int loudRate = 0;
    loudExportOk = loudExportOk && libavcore::readPcm16WavToMono(loudOutputPath,
        loudMono, loudRate, &loudError) && loudRate == kRate && loudMono.size() >= kFrames;
    double loudExportDeltaDb = 100.0;
    if (loudExportOk) {
        std::vector<float> loudExport(input.size());
        for (int f = 0; f < kFrames; ++f)
            loudExport[2 * f] = loudExport[2 * f + 1] = static_cast<float>(loudMono[f]);
        loudExportDeltaDb = 20 * std::log10(spectralAmplitude(loudExport, 100) / loudAmplitude);
    }
    std::fprintf(stderr, "Master EQ headroom: peak %.0f, 100Hz %.3f dB, export delta %.3f dB, error: %s\n",
        loudPeak, loudCutDb, loudExportDeltaDb, loudError.toUtf8().constData());
    gate(15, masterOk && masterMixer->masterEqProcessCallsForTest() > 0
        && hasCut(masterOutput, drySum) && masterSettings.eqEnabled
        && !masterSettings.compEnabled && !masterSettings.reverbEnabled && !masterSettings.nrEnabled
        && headroomOk && loudExportOk && loudError.isEmpty() && std::abs(loudExportDeltaDb) <= 0.1);

    // G16: G13's real export, with the new branch disabled explicitly and
    // implicitly, must produce the very same WAV bytes and take zero EQ passes.
    const auto wavBytes = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    const QString bypassPath = directory.filePath(QStringLiteral("master-bypass.wav"));
    const QString disabledPath = directory.filePath(QStringLiteral("master-disabled.wav"));
    exportMixer->setMasterEq(masterEq, true);
    audiofxexport::setMasterEqBypassForTest(true);
    bool masterExportOk = renderinplace::prepareAudioMix(&exportTimeline, bypassPath,
        2.0, &exportError) == bypassPath && audiofxexport::masterEqPassesForTest() == 0;
    audiofxexport::setMasterEqBypassForTest(false);
    exportMixer->setMasterEq(masterEq, false);
    masterExportOk = renderinplace::prepareAudioMix(&exportTimeline, disabledPath,
        2.0, &exportError) == disabledPath && masterExportOk
        && audiofxexport::masterEqPassesForTest() == 0;
    const auto g13Bytes = wavBytes(outputPath);
    masterExportOk = masterExportOk && !g13Bytes.isEmpty()
        && wavBytes(bypassPath) == g13Bytes && wavBytes(disabledPath) == g13Bytes;

    // Two independent source tracks; master-only also exercises the non-track-FX mix.
    const QString lowPath = directory.filePath(QStringLiteral("master-low.wav"));
    const QString midPath = directory.filePath(QStringLiteral("master-mid.wav"));
    std::vector<float> lowSamples(input.size()), midSamples(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        lowSamples[i] = originalOne[i] / 32768.0f;
        midSamples[i] = originalTwo[i] / 32768.0f;
    }
    masterExportOk = libavcore::writeWavFloatInterleaved(lowPath, lowSamples, kRate, 2, &exportError)
        && libavcore::writeWavFloatInterleaved(midPath, midSamples, kRate, 2, &exportError) && masterExportOk;
    ClipInfo lowClip = audioClip, midClip = audioClip;
    lowClip.filePath = lowPath;
    midClip.filePath = midPath;
    exportTimeline.restoreFromProject(QVector<QVector<ClipInfo>>{{}},
        QVector<QVector<ClipInfo>>{{lowClip}, {midClip}}, 0, -1, -1, 10);
    exportMixer->setTrackEqEnabled(0, false);
    exportMixer->setMasterEq(masterEq, true);
    const auto checkMasterExport = [&](const QString &path) {
        if (renderinplace::prepareAudioMix(&exportTimeline, path, 2.0, &exportError) != path)
            return false;
        std::vector<double> mono;
        int rate = 0;
        if (!libavcore::readPcm16WavToMono(path, mono, rate, &exportError)
            || rate != kRate || mono.size() < kFrames) return false;
        std::vector<float> stereo(kFrames * 2);
        for (int f = 0; f < kFrames; ++f)
            stereo[2 * f] = stereo[2 * f + 1] = static_cast<float>(mono[f]);
        return hasCut(stereo, drySum);
    };
    masterExportOk = checkMasterExport(directory.filePath(QStringLiteral("master-only.wav")))
        && masterExportOk && audiofxexport::masterEqPassesForTest() == 1;
    // And the existing per-track DSP export branch followed by master EQ.
    exportMixer->setEqForTrack(0, AudioMixer::EqSettings{});
    masterExportOk = checkMasterExport(directory.filePath(QStringLiteral("track-and-master.wav")))
        && masterExportOk && audiofxexport::masterEqPassesForTest() == 2;
    audiofxexport::setMasterEqBypassForTest(false);
    gate(16, masterExportOk && exportError.isEmpty());
    // G17: every parameter and both enable layers survive serialization.
    trackfx::Chain savedChain;
    savedChain.eqEnabled = savedChain.compEnabled = savedChain.reverbEnabled = savedChain.nrEnabled = true;
    savedChain.eq.low = {90.0, 3.0, 0.9, false};
    savedChain.eq.lowMid = {400.0, -6.0, 1.2, false};
    savedChain.eq.highMid = {4000.0, 2.0, 1.4, false};
    savedChain.eq.high = {12000.0, -2.0, 0.8, false};
    savedChain.comp = {-18.0, 3.0, 8.0, 150.0, 4.0, 2.0, true};
    savedChain.reverb = {0.3, 0.7, 35.0, 45.0, 75.0, true};
    savedChain.nr = {-15.0, 18.0, 7.0, 250.0, -55.0, false, true};
    const auto chainJson = savedChain.toJson();
    auto dormantChain = savedChain;
    dormantChain.eqEnabled = dormantChain.compEnabled = dormantChain.reverbEnabled = dormantChain.nrEnabled = false;
    bool jsonOk = sameChain(savedChain, trackfx::Chain::fromJson(chainJson))
        && sameChain(dormantChain, trackfx::Chain::fromJson(dormantChain.toJson()))
        && !savedChain.isDefault() && !dormantChain.isDefault()
        && trackfx::Chain{}.toJson().isEmpty()
        && sameChain(trackfx::Chain::fromJson(QJsonObject{}), trackfx::Chain{});
    auto parameterOnly = trackfx::Chain{};
    parameterOnly.eq.high.q = 1.5;
    jsonOk = jsonOk && !parameterOnly.isDefault();
    const auto invalidChain = trackfx::Chain::fromJson(QJsonObject{
        {"comp", QJsonObject{{"attackMs", -10.0}, {"ratio", 999.0}}},
        {"eq", QJsonObject{{"lowMid", QJsonObject{{"q", 0.0}}}}}});
    jsonOk = jsonOk && invalidChain.comp.attackMs == 0.1
        && invalidChain.comp.ratio == 50.0 && invalidChain.eq.lowMid.q == 0.1;
    gate(17, jsonOk);

    // G18: string and disk entry points use identical optional audio keys.
    ProjectData fxProject;
    fxProject.trackFx.insert(0, savedChain);
    fxProject.trackFx.insert(2, dormantChain);
    fxProject.trackFx.insert(1, trackfx::Chain{});
    fxProject.masterFx.eq = masterEq;
    fxProject.masterFx.eqEnabled = true;
    const QString fxJson = ProjectFile::toJsonString(fxProject);
    const auto audioJson = QJsonDocument::fromJson(fxJson.toUtf8()).object()["audioMixer"].toObject();
    ProjectData restoredProject;
    bool projectOk = ProjectFile::fromJsonString(fxJson, restoredProject)
        && audioJson["trackFx"].toArray().size() == 2 && audioJson.contains("masterFx")
        && restoredProject.trackFx.size() == 2
        && sameChain(restoredProject.trackFx.value(0), savedChain)
        && sameChain(restoredProject.trackFx.value(2), dormantChain)
        && sameChain(restoredProject.masterFx, fxProject.masterFx);
    const QString fxPath = directory.filePath(QStringLiteral("track-fx.veditor"));
    ProjectData diskProject;
    projectOk = ProjectFile::save(fxPath, fxProject) && ProjectFile::load(fxPath, diskProject)
        && sameChain(diskProject.trackFx.value(0), savedChain)
        && sameChain(diskProject.trackFx.value(2), dormantChain)
        && sameChain(diskProject.masterFx, fxProject.masterFx) && projectOk;
    const QString defaultJson = ProjectFile::toJsonString(ProjectData{});
    const auto defaultAudio = QJsonDocument::fromJson(defaultJson.toUtf8()).object()["audioMixer"].toObject();
    projectOk = !defaultAudio.contains("trackFx") && !defaultAudio.contains("masterFx")
        && ProjectFile::fromJsonString(defaultJson, restoredProject)
        && restoredProject.trackFx.isEmpty() && restoredProject.masterFx.isDefault() && projectOk;
    auto oldRoot = QJsonDocument::fromJson(defaultJson.toUtf8()).object();
    oldRoot.remove("audioMixer");
    restoredProject = fxProject;
    projectOk = ProjectFile::fromJsonString(QString::fromUtf8(QJsonDocument(oldRoot).toJson()), restoredProject)
        && restoredProject.trackFx.isEmpty() && restoredProject.masterFx.isDefault() && projectOk;
    trackfx::Processor restoredDsp(diskProject.trackFx.value(0), kRate, 2);
    trackfx::Processor savedDsp(savedChain, kRate, 2);
    auto restoredSamples = input;
    auto savedSamples = input;
    processBlocks(restoredDsp, restoredSamples);
    processBlocks(savedDsp, savedSamples);
    projectOk = projectOk && identical(restoredSamples, savedSamples)
        && !identical(restoredSamples, input);
    gate(18, projectOk);

    // G19: project switching clears modern/legacy settings and DSP histories.
    auto resetMixer = std::make_unique<AudioMixer>();
    resetMixer->setTrackChain(0, diskProject.trackFx.value(0));
    resetMixer->setTrackChain(2, diskProject.trackFx.value(2));
    resetMixer->setMasterEq(diskProject.masterFx.eq, diskProject.masterFx.eqEnabled);
    resetMixer->setTrackEqConfig(0, persistedEq);
    resetMixer->setTrackEqEnabled(0, true);
    bool resetOk = sameChain(resetMixer->trackChain(0), savedChain)
        && sameChain(resetMixer->trackChain(2), dormantChain)
        && sameChain(resetMixer->masterChain(), fxProject.masterFx);
    auto warmPcm = originalOne;
    resetMixer->processTrackFxForTest(0, warmPcm.data(), kFrames);
    auto warmMaster = masterDry;
    resetMixer->processMasterEqForTest(warmMaster.data(), kFrames);
    resetMixer->clearTrackFx();
    resetMixer->setLegacyTrackFxPathForTest(false); // zero the call counter
    resetMixer->setMasterEqBypassForTest(false);
    auto dryPcm = originalOne;
    resetMixer->processTrackFxForTest(0, dryPcm.data(), kFrames);
    auto dryMaster = masterDry;
    resetMixer->processMasterEqForTest(dryMaster.data(), kFrames);
    auto preciseBypass = exact;
    trackfx::Processor resetTrack(resetMixer->trackChain(0), kRate, 2);
    trackfx::Processor resetMaster(resetMixer->masterChain(), kRate, 2);
    processBlocks(resetTrack, preciseBypass);
    processBlocks(resetMaster, preciseBypass);
    resetOk = resetOk && resetMixer->trackChain(0).isDefault()
        && resetMixer->trackChain(2).isDefault() && resetMixer->masterChain().isDefault()
        && resetMixer->trackEqConfig(0).isDefault() && !resetMixer->trackEqEnabled(0)
        && !resetMixer->compressorForTrack(0).enabled && !resetMixer->reverbForTrack(0).enabled
        && !resetMixer->noiseReductionForTrack(0).enabled
        && resetMixer->eqForTrack(0).lowMid.gainDb == 0.0
        && resetMixer->currentGainReductionDb(0) == 0.0
        && resetMixer->trackFxProcessCallsForTest() == 0
        && resetMixer->masterEqProcessCallsForTest() == 0
        && dryPcm == originalOne && dryMaster == masterDry && identical(exact, preciseBypass);
    resetMixer->clearTrackFx(); // idempotent on an FX-free project
    gate(19, resetOk && resetMixer->trackChain(0).isDefault());
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", pass, fail);
    return fail;
}
