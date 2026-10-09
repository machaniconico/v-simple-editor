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
#include "../DeEsserPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include "../RenderInPlace.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"
#include "SelftestGate.h"
#include <QAbstractSlider>
#include <QDockWidget>
#include <functional>
#include "../libavcore/AudioExtract.h"
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <climits>
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
    selftest::GateTally tally;
    auto gate = [&](int n, bool ok) { tally.gate(n, ok); };
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
    auto *eqCombo = eqPanel.findChild<QComboBox *>(QStringLiteral("eq_track"));
    idsOk = idsOk && eqCombo && eqCombo->itemData(0).toInt() == AudioMixer::kMasterTrackId
        && eqCombo->itemData(1).toInt() == 0 && eqCombo->itemData(2).toInt() == 1;
    if (eqCombo) {
        eqCombo->setCurrentIndex(1);
        eqCombo->setCurrentIndex(0);
        idsOk = idsOk && eqWrites == 0;
        auto *gain = eqPanel.findChild<QSlider *>(QStringLiteral("eq_low_gain"));
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
    const auto checkPanel = [&](auto &panel, const char *comboName) {
        auto *combo = panel.template findChild<QComboBox *>(QString::fromLatin1(comboName));
        const auto combosByType = panel.template findChildren<QComboBox *>();
        std::fprintf(stderr, "[audio-trackfx] G14 track-combo found=%s typeOnlyFirst=%s combos=%lld\n",
                     combo ? qPrintable(combo->objectName()) : "(null)",
                     combosByType.isEmpty() ? "(none)" : qPrintable(combosByType.first()->objectName()),
                     static_cast<long long>(combosByType.size()));
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
    idsOk = checkPanel(compPanel, "compressor_track") && checkPanel(reverbPanel, "reverb_track")
        && checkPanel(nrPanel, "nr_track") && idsOk;
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

    // G20-G23: an open EQ / compressor / reverb / NR dock follows a project load
    // (the mixer is the source of truth) and its reseed writes nothing back.
    struct DockCase {
        int gateNumber;
        const char *name;
        const char *comboName;
        const char *valueName;   // control whose value must follow the project
        const char *touchName;   // another control, moved after the reseed
        int touchValue;
        std::function<void(MainWindow &)> open;
        std::function<QDockWidget *(MainWindow &)> dock;
        std::function<void(trackfx::Chain &)> arrange;
        std::function<double(AudioMixer &)> mixerValue;
        std::function<int(MainWindow &)> emitCount;
        double projectValue;
        double dockPerValue;     // dock control value = projectValue * dockPerValue
    };
    const std::vector<DockCase> dockCases = {
        {20, "eq", "eq_track", "eq_lowmid_gain", "eq_high_gain", 3,
         [](MainWindow &w) { w.openEqualizerPanel(); },
         [](MainWindow &w) { return w.m_equalizerDock; },
         [](trackfx::Chain &c) { c.eqEnabled = true; c.eq.lowMid = {250.0, -9.0, 1.0, true}; },
         [](AudioMixer &m) { return m.eqForTrack(0).lowMid.gainDb; },
         [](MainWindow &w) { return qobject_cast<EqualizerPanel *>(w.m_equalizerDock->widget())
                                 ->changedEmitCountForTest(); },
         -9.0, 1.0},
        {21, "compressor", "compressor_track", "compressor_threshold", "compressor_makeup", 30,
         [](MainWindow &w) { w.openCompressorPanel(); },
         [](MainWindow &w) { return w.m_compressorDock; },
         [](trackfx::Chain &c) { c.compEnabled = true; c.comp.enabled = true;
                                 c.comp.thresholdDb = -30.0; c.comp.ratio = 6.0; },
         [](AudioMixer &m) { return m.compressorForTrack(0).thresholdDb; },
         [](MainWindow &w) { return qobject_cast<CompressorPanel *>(w.m_compressorDock->widget())
                                 ->changedEmitCountForTest(); },
         -30.0, 1.0},
        {22, "reverb", "reverb_track", "reverb_mix", "reverb_width", 70,
         [](MainWindow &w) { w.openReverbPanel(); },
         [](MainWindow &w) { return w.m_reverbDock; },
         [](trackfx::Chain &c) { c.reverbEnabled = true; c.reverb.enabled = true;
                                 c.reverb.mixRatio = 0.4; c.reverb.decaySeconds = 2.0; },
         [](AudioMixer &m) { return m.reverbForTrack(0).mixRatio; },
         [](MainWindow &w) { return qobject_cast<ReverbPanel *>(w.m_reverbDock->widget())
                                 ->changedEmitCountForTest(); },
         0.4, 100.0},
        {23, "nr", "nr_track", "nr_reduction", "nr_attack", 100,
         [](MainWindow &w) { w.openNoiseReductionPanel(); },
         [](MainWindow &w) { return w.m_noiseReductionDock; },
         [](trackfx::Chain &c) { c.nrEnabled = true; c.nr.enabled = true;
                                 c.nr.reductionDb = 24.0; c.nr.thresholdDb = -30.0; },
         [](AudioMixer &m) { return m.noiseReductionForTrack(0).reductionDb; },
         [](MainWindow &w) { return qobject_cast<NoiseReductionPanel *>(w.m_noiseReductionDock->widget())
                                 ->changedEmitCountForTest(); },
         24.0, 1.0},
    };
    for (const auto &dc : dockCases) {
        MainWindow window;
        auto *mixer = window.m_player ? window.m_player->audioMixer() : nullptr;
        if (window.m_timeline && window.m_timeline->audioTrackCount() < 1)
            window.m_timeline->restoreFromProject(QVector<QVector<ClipInfo>>{},
                QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
        bool ok = false;
        if (mixer && window.m_timeline) {
            dc.open(window);
            QDockWidget *dock = dc.dock(window);
            QWidget *panel = dock ? dock->widget() : nullptr;
            auto *combo = panel ? panel->findChild<QComboBox *>(dc.comboName) : nullptr;
            auto *value = panel ? panel->findChild<QAbstractSlider *>(dc.valueName) : nullptr;
            auto *touch = panel ? panel->findChild<QAbstractSlider *>(dc.touchName) : nullptr;
            if (combo && value && touch && combo->findData(0) >= 0) {
                combo->setCurrentIndex(combo->findData(0)); // edit A1 (engine id 0)
                const int undoBefore = window.m_timeline->undoManager()->currentIndex();
                const int emitBefore = dc.emitCount(window);
                trackfx::Chain chain;
                dc.arrange(chain);
                ProjectData project;
                project.trackFx.insert(0, chain);
                window.applyAudioState(project); // project load: mixer -> dock
                const double dockValue = value->value() / dc.dockPerValue;
                const double mixerValue = dc.mixerValue(*mixer);
                const int emitReseed = dc.emitCount(window) - emitBefore;
                const int undoAfterReseed = window.m_timeline->undoManager()->currentIndex();
                // Outside a reseed one control change emits exactly once and must
                // not write the stale (default) value of the loaded control back.
                touch->setValue(dc.touchValue);
                const int emitTouch = dc.emitCount(window) - emitBefore - emitReseed;
                const double mixerAfterTouch = dc.mixerValue(*mixer);
                std::fprintf(stderr,
                    "audio-trackfx G%d %s: dock=%g mixer=%g project=%g emitsDuringReseed=%d "
                    "emitsOnTouch=%d mixerAfterTouch=%g undoIdx=%d->%d\n",
                    dc.gateNumber, dc.name, dockValue, mixerValue, dc.projectValue, emitReseed,
                    emitTouch, mixerAfterTouch, undoBefore, undoAfterReseed);
                const auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
                ok = same(dockValue, dc.projectValue) && same(mixerValue, dc.projectValue)
                    && emitReseed == 0 && emitTouch == 1
                    && same(mixerAfterTouch, dc.projectValue)
                    && undoAfterReseed == undoBefore;
            } else {
                std::fprintf(stderr, "audio-trackfx G%d %s: dock controls not found "
                    "(combo=%d value=%d touch=%d)\n", dc.gateNumber, dc.name,
                    combo != nullptr, value != nullptr, touch != nullptr);
            }
        }
        gate(dc.gateNumber, ok);
    }

    // G24: suppression entered by a caller survives a reseed (the panels save the
    // state on entry and restore it on exit instead of forcing it off).
    {
        const QStringList names{QStringLiteral("M"), QStringLiteral("A1")};
        const QList<int> ids{AudioMixer::kMasterTrackId, 0};
        bool outerKept = true, outerClear = true;
        int eqEmits = 0;
        {
            EqualizerPanel eq;
            auto *gain = eq.findChild<QAbstractSlider *>("eq_low_gain");
            const int eqBase = eq.changedEmitCountForTest(); // the constructor refreshes labels once
            eq.m_suppressSignals = true;
            if (gain) gain->blockSignals(true);
            eq.setTracks(names, ids);
            eq.setEqSettings(AudioMixer::kMasterTrackId, AudioMixer::EqSettings{});
            const bool kept = eq.m_suppressSignals && gain && gain->signalsBlocked();
            std::fprintf(stderr, "audio-trackfx G24 eq: outer suppress kept=%d (suppress=%d blocked=%d)\n",
                kept, eq.m_suppressSignals, gain && gain->signalsBlocked());
            outerKept = outerKept && kept;
            eq.m_suppressSignals = false;
            if (gain) gain->blockSignals(false);
            eq.setTracks(names, ids);
            eq.setEqSettings(AudioMixer::kMasterTrackId, AudioMixer::EqSettings{});
            const bool clear = !eq.m_suppressSignals && gain && !gain->signalsBlocked();
            outerClear = outerClear && clear;
            eqEmits = eq.changedEmitCountForTest() - eqBase;
        }
        const auto checkPanel = [&](const char *name, auto &panel, auto settings) {
            auto *combo = panel.template findChild<QComboBox *>(QStringLiteral("%1_track").arg(QLatin1String(name)));
            audiofxui::setTrackComboItems(combo, {QStringLiteral("A1")}, {0});
            const int base = panel.changedEmitCountForTest(); // constructor refresh
            panel.m_suppressEmit = true;
            panel.loadSettings(0, settings);
            panel.setTrackList({1});
            const bool kept = panel.m_suppressEmit;
            panel.m_suppressEmit = false;
            panel.loadSettings(0, settings);
            const bool clear = !panel.m_suppressEmit;
            std::fprintf(stderr, "audio-trackfx G24 %s: outer suppress kept=%d, cleared stays clear=%d, emits=%d\n",
                name, kept, clear, panel.changedEmitCountForTest() - base);
            outerKept = outerKept && kept;
            outerClear = outerClear && clear && panel.changedEmitCountForTest() == base;
        };
        CompressorPanel comp;
        ReverbPanel reverb;
        NoiseReductionPanel nr;
        checkPanel("compressor", comp, AudioMixer::CompressorSettings{});
        checkPanel("reverb", reverb, AudioMixer::ReverbSettings{});
        checkPanel("nr", nr, AudioMixer::NoiseReductionSettings{});
        std::fprintf(stderr, "audio-trackfx G24: kept=%d clear=%d eqEmits=%d\n",
            outerKept, outerClear, eqEmits);
        gate(24, outerKept && outerClear && eqEmits == 0);
    }
    // G25-G36 (US-911): audio FX dock gestures are undo steps. One window: A1
    // carries the edited track FX, A2 a clip for G35's unrelated edit.
    // Master test signal on the int32 bus: 100 Hz + 1 kHz + 6 kHz (de-esser band).
    std::vector<int32_t> busSignal(2 * kFrames);
    for (int f = 0; f < kFrames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        const auto sample = static_cast<int32_t>(8000 * (std::sin(2 * kPi * 100 * t)
            + std::sin(2 * kPi * 1000 * t) + std::sin(2 * kPi * 6000 * t)));
        busSignal[2 * f] = busSignal[2 * f + 1] = sample;
    }
    // The master stage's output for busSignal from a cleared filter history.
    const auto masterPcm = [&](AudioMixer *m) {
        m->setMasterEqBypassForTest(false); // resets the filter history
        auto pcm = busSignal;
        for (int f = 0; f < kFrames; f += 257)
            m->processMasterEqForTest(pcm.data() + 2 * f, std::min(257, kFrames - f));
        return pcm;
    };
    const auto rmsDiff = [](const std::vector<int32_t> &a, const std::vector<int32_t> &b) {
        double sum = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            sum += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
        return a.empty() ? 0.0 : std::sqrt(sum / a.size());
    };
    {
        MainWindow window;
        auto *mixer = window.m_player ? window.m_player->audioMixer() : nullptr;
        Timeline *timeline = window.m_timeline;
        if (timeline)
            timeline->restoreFromProject(QVector<QVector<ClipInfo>>{},
                QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}, QVector<ClipInfo>{audioClip}},
                0.0, -1.0, -1.0, 100);
        if (mixer && timeline) {
            window.openEqualizerPanel();
            window.openCompressorPanel();
            window.openReverbPanel();
            window.openNoiseReductionPanel();
            window.openDeEsserPanel();
        }
        const auto panelOf = [](QDockWidget *dock) { return dock ? dock->widget() : nullptr; };
        QWidget *eqPanel = panelOf(window.m_equalizerDock);
        QWidget *compPanel = panelOf(window.m_compressorDock);
        QWidget *reverbPanel = panelOf(window.m_reverbDock);
        QWidget *nrPanel = panelOf(window.m_noiseReductionDock);
        QWidget *dePanel = panelOf(window.m_deesserDock);
        const auto serial = [&]() -> quint64 {
            return timeline ? timeline->undoManager()->saveSerial() : 0;
        };
        const auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        const auto select = [](QWidget *panel, const char *comboName, int id) {
            auto *combo = panel ? panel->findChild<QComboBox *>(comboName) : nullptr;
            const int index = combo ? combo->findData(id) : -1;
            if (index >= 0) combo->setCurrentIndex(index);
            return index >= 0;
        };
        // A mouse drag: press, valueChanged per value, release.
        const auto drag = [](QAbstractSlider *slider, const std::vector<int> &values) {
            slider->setSliderDown(true);
            for (int v : values) slider->setValue(v);
            slider->setSliderDown(false);
        };

        // G25-G28: one 5-step slider / dial drag = exactly one save; undo / redo
        // move the mixer back to the previous snapshot and forward again, and
        // the reseeded dock shows the mixer value.
        struct SliderCase {
            int gateNumber;
            const char *name;
            QWidget *panel;
            const char *comboName;
            const char *controlName;
            std::function<double()> mixerValue;
            double dockPerValue; // control value = mixer value * dockPerValue
        };
        const std::vector<SliderCase> sliderCases = {
            {25, "eq", eqPanel, "eq_track", "eq_lowmid_gain",
             [&] { return mixer->eqForTrack(0).lowMid.gainDb; }, 1.0},
            {26, "compressor", compPanel, "compressor_track", "compressor_threshold",
             [&] { return mixer->compressorForTrack(0).thresholdDb; }, 1.0},
            {27, "reverb", reverbPanel, "reverb_track", "reverb_mix",
             [&] { return mixer->reverbForTrack(0).mixRatio; }, 100.0},
            {28, "nr", nrPanel, "nr_track", "nr_reduction",
             [&] { return mixer->noiseReductionForTrack(0).reductionDb; }, 1.0},
        };
        for (const auto &sc : sliderCases) {
            bool ok = false;
            auto *control = sc.panel ? sc.panel->findChild<QAbstractSlider *>(sc.controlName) : nullptr;
            if (mixer && control && select(sc.panel, sc.comboName, 0)) {
                const int start = control->value();
                const int step = start + 5 <= control->maximum() ? 1 : -1;
                std::vector<int> values;
                for (int k = 1; k <= 5; ++k) values.push_back(start + k * step);
                int changes = 0;
                const auto counter = QObject::connect(control, &QAbstractSlider::valueChanged,
                                                      [&changes] { ++changes; });
                const double before = sc.mixerValue();
                const quint64 serialBefore = serial();
                drag(control, values);
                QObject::disconnect(counter);
                const quint64 saves = serial() - serialBefore;
                const double after = sc.mixerValue();
                timeline->undo();
                const double undone = sc.mixerValue();
                const double dockUndone = control->value() / sc.dockPerValue;
                timeline->redo();
                const double redone = sc.mixerValue();
                const double dockRedone = control->value() / sc.dockPerValue;
                std::fprintf(stderr,
                    "audio-trackfx G%d %s: valueChanged=%d saves=%llu before=%g after=%g "
                    "undo mixer=%g dock=%g redo mixer=%g dock=%g\n",
                    sc.gateNumber, sc.name, changes, static_cast<unsigned long long>(saves),
                    before, after, undone, dockUndone, redone, dockRedone);
                ok = changes == 5 && saves == 1 && !same(before, after)
                    && same(undone, before) && same(dockUndone, undone)
                    && same(redone, after) && same(dockRedone, redone);
            } else {
                std::fprintf(stderr, "audio-trackfx G%d %s: dock control not found\n",
                             sc.gateNumber, sc.name);
            }
            gate(sc.gateNumber, ok);
        }

        // G29-G31: the de-esser has spin boxes and checkboxes only.
        auto *deThreshold = dePanel ? dePanel->findChild<QDoubleSpinBox *>("deesser_threshold") : nullptr;
        auto *deEnabled = dePanel ? dePanel->findChild<QCheckBox *>("deesser_enabled") : nullptr;
        auto *deListen = dePanel ? dePanel->findChild<QCheckBox *>("deesser_listen") : nullptr;
        const bool deReady = mixer && deThreshold && deEnabled && deListen
            && select(dePanel, "deesser_target", 0);
        const auto deChain = [&] { return trackfx::Chain(mixer->trackChain(0)).deesser; };
        {
            // G29: typed value + editingFinished = one save; undo / redo + reseed.
            bool ok = false;
            if (deReady) {
                const double before = deChain().thresholdDb;
                const quint64 serialBefore = serial();
                deThreshold->setValue(before - 6.0);
                emit deThreshold->editingFinished();
                const quint64 saves = serial() - serialBefore;
                const double after = deChain().thresholdDb;
                timeline->undo();
                const double undone = deChain().thresholdDb;
                const double dockUndone = deThreshold->value();
                timeline->redo();
                const double redone = deChain().thresholdDb;
                const double dockRedone = deThreshold->value();
                std::fprintf(stderr,
                    "audio-trackfx G29 deesser spin: saves=%llu before=%g after=%g undo mixer=%g "
                    "dock=%g redo mixer=%g dock=%g\n", static_cast<unsigned long long>(saves),
                    before, after, undone, dockUndone, redone, dockRedone);
                ok = saves == 1 && same(after, before - 6.0) && same(undone, before)
                    && same(dockUndone, undone) && same(redone, after) && same(dockRedone, redone);
            }
            gate(29, ok);
        }
        {
            // G30: valueChanged bursts while typing save nothing until editingFinished.
            bool ok = false;
            if (deReady) {
                const double base = deThreshold->value();
                const quint64 serialBefore = serial();
                for (int k = 1; k <= 5; ++k)
                    deThreshold->setValue(base - k);
                const quint64 typingSaves = serial() - serialBefore;
                const double live = deChain().thresholdDb;
                emit deThreshold->editingFinished();
                const quint64 finishSaves = serial() - serialBefore - typingSaves;
                std::fprintf(stderr,
                    "audio-trackfx G30 deesser typing: saves while typing=%llu mixer live=%g "
                    "(expected %g) saves on editingFinished=%llu\n",
                    static_cast<unsigned long long>(typingSaves), live, base - 5,
                    static_cast<unsigned long long>(finishSaves));
                ok = typingSaves == 0 && same(live, base - 5) && finishSaves == 1;
            }
            gate(30, ok);
        }
        {
            // G31: the enable checkbox is one save; the listen monitor is none.
            bool ok = false;
            if (deReady) {
                const bool enabledBefore = deChain().enabled;
                quint64 serialBefore = serial();
                deEnabled->setChecked(!enabledBefore);
                const quint64 enableSaves = serial() - serialBefore;
                const bool enabledAfter = deChain().enabled;
                serialBefore = serial();
                deListen->setChecked(!deListen->isChecked());
                const quint64 listenSaves = serial() - serialBefore;
                const bool listenLive = deChain().listen;
                deListen->setChecked(!deListen->isChecked());
                const quint64 listenBackSaves = serial() - serialBefore;
                std::fprintf(stderr,
                    "audio-trackfx G31 checkbox: enable %d->%d saves=%llu; listen live=%d "
                    "saves=%llu (toggle back total %llu)\n", enabledBefore, enabledAfter,
                    static_cast<unsigned long long>(enableSaves), listenLive,
                    static_cast<unsigned long long>(listenSaves),
                    static_cast<unsigned long long>(listenBackSaves));
                ok = enableSaves == 1 && enabledAfter != enabledBefore
                    && listenSaves == 0 && listenLive && listenBackSaves == 0;
            }
            gate(31, ok);
        }
        {
            // G32: a reverb preset moves several dials but is one gesture.
            bool ok = false;
            auto *preset = reverbPanel ? reverbPanel->findChild<QPushButton *>("reverb_preset_cathedral") : nullptr;
            if (mixer && preset && select(reverbPanel, "reverb_track", 0)) {
                const char *dialNames[] = {"reverb_mix", "reverb_decay", "reverb_preDelay",
                                           "reverb_damping", "reverb_width"};
                std::vector<int> before;
                for (const char *name : dialNames) {
                    auto *dial = reverbPanel->findChild<QAbstractSlider *>(name);
                    before.push_back(dial ? dial->value() : INT_MIN);
                }
                const quint64 serialBefore = serial();
                preset->click();
                const quint64 saves = serial() - serialBefore;
                int moved = 0;
                for (size_t i = 0; i < before.size(); ++i) {
                    auto *dial = reverbPanel->findChild<QAbstractSlider *>(dialNames[i]);
                    moved += dial && dial->value() != before[i];
                }
                const double mix = mixer->reverbForTrack(0).mixRatio;
                std::fprintf(stderr, "audio-trackfx G32 reverb preset: dials moved=%d saves=%llu mix=%g\n",
                             moved, static_cast<unsigned long long>(saves), mix);
                ok = moved >= 3 && saves == 1 && same(mix, 0.40);
            }
            gate(32, ok);
        }
        {
            // G33: switching the edited track in every dock saves nothing.
            bool ok = false;
            if (mixer) {
                const quint64 serialBefore = serial();
                bool switched = true;
                for (int id : {1, 0}) {
                    switched = select(eqPanel, "eq_track", id) && switched;
                    switched = select(compPanel, "compressor_track", id) && switched;
                    switched = select(reverbPanel, "reverb_track", id) && switched;
                    switched = select(nrPanel, "nr_track", id) && switched;
                    switched = select(dePanel, "deesser_target", id) && switched;
                }
                switched = select(eqPanel, "eq_track", AudioMixer::kMasterTrackId) && switched;
                switched = select(dePanel, "deesser_target", AudioMixer::kMasterTrackId) && switched;
                switched = select(eqPanel, "eq_track", 0) && select(dePanel, "deesser_target", 0) && switched;
                const quint64 saves = serial() - serialBefore;
                std::fprintf(stderr, "audio-trackfx G33 track combo: switched=%d saves=%llu\n",
                             switched, static_cast<unsigned long long>(saves));
                ok = switched && saves == 0;
            }
            gate(33, ok);
        }
        {
            // G34 (negative control): a press-release without a value change, a
            // drag that returns to its start (the panel reports a gesture, the
            // snapshot comparison drops it) and a project-load reseed save nothing.
            bool ok = false;
            auto *gain = eqPanel ? eqPanel->findChild<QAbstractSlider *>("eq_lowmid_gain") : nullptr;
            auto *eq = qobject_cast<EqualizerPanel *>(eqPanel);
            if (mixer && gain && eq && select(eqPanel, "eq_track", 0)) {
                int gestures = 0;
                const auto counter = QObject::connect(eq, &EqualizerPanel::gestureFinished,
                                                      [&gestures] { ++gestures; });
                quint64 serialBefore = serial();
                gain->setSliderDown(true);
                gain->setSliderDown(false);
                const quint64 pressSaves = serial() - serialBefore;
                const int pressGestures = gestures;
                const int start = gain->value();
                const int away = start + 1 <= gain->maximum() ? start + 1 : start - 1;
                serialBefore = serial();
                drag(gain, {away, start});
                const quint64 returnSaves = serial() - serialBefore;
                const int returnGestures = gestures - pressGestures;
                QObject::disconnect(counter);
                // Drag and return after an undo on an untouched track: the undo
                // leaves a default chain under the track key where the history
                // entry has none; both mean the same state, so nothing is saved
                // and the redo stays available.
                auto *compDial = compPanel ? compPanel->findChild<QAbstractSlider *>("compressor_threshold") : nullptr;
                quint64 compSaves = 0, compReturnSaves = 0;
                bool compRedo = false;
                QByteArray compChainJson;
                const bool compReady = compDial && select(compPanel, "compressor_track", 1)
                    && !mixer->collectTrackState().value("chains").toObject().contains("1");
                if (compReady) {
                    const int compStart = compDial->value();
                    const int compAway = compStart + 1 <= compDial->maximum() ? compStart + 1 : compStart - 1;
                    serialBefore = serial();
                    drag(compDial, {compAway});
                    compSaves = serial() - serialBefore;
                    timeline->undo();
                    serialBefore = serial();
                    drag(compDial, {compAway, compDial->value()});
                    compReturnSaves = serial() - serialBefore;
                    compRedo = timeline->canRedo();
                    compChainJson = QJsonDocument(mixer->collectTrackState().value("chains").toObject()
                        .value("1").toObject()).toJson(QJsonDocument::Compact);
                    select(compPanel, "compressor_track", 0);
                }
                std::fprintf(stderr,
                    "audio-trackfx G34 untouched track 1: ready=%d compressor gesture saves=%llu; "
                    "after undo drag-and-return saves=%llu canRedo=%d live chain[1]=%s\n",
                    compReady, static_cast<unsigned long long>(compSaves),
                    static_cast<unsigned long long>(compReturnSaves), compRedo,
                    compChainJson.constData());
                trackfx::Chain loaded;
                loaded.compEnabled = loaded.comp.enabled = true;
                loaded.comp.thresholdDb = -33.0;
                ProjectData project;
                project.trackFx.insert(0, loaded);
                serialBefore = serial();
                window.applyAudioState(project);
                const quint64 reseedSaves = serial() - serialBefore;
                const double reseeded = mixer->compressorForTrack(0).thresholdDb;
                std::fprintf(stderr,
                    "audio-trackfx G34: press-release gestures=%d saves=%llu; drag-and-return "
                    "gestures=%d saves=%llu; applyAudioState reseed saves=%llu (mixer threshold %g)\n",
                    pressGestures, static_cast<unsigned long long>(pressSaves), returnGestures,
                    static_cast<unsigned long long>(returnSaves),
                    static_cast<unsigned long long>(reseedSaves), reseeded);
                ok = pressGestures == 0 && pressSaves == 0 && returnGestures == 1
                    && returnSaves == 0 && reseedSaves == 0 && same(reseeded, -33.0)
                    && compReady && compSaves == 1 && compReturnSaves == 0 && compRedo;
            }
            gate(34, ok);
        }
        {
            // G35: master EQ / de-esser gestures undo and redo both the chain
            // values and the master stage's output; an unrelated clip edit's
            // undo also returns the master to its snapshot.
            bool ok = false;
            auto *gain = eqPanel ? eqPanel->findChild<QAbstractSlider *>("eq_lowmid_gain") : nullptr;
            if (mixer && gain && deReady && select(eqPanel, "eq_track", AudioMixer::kMasterTrackId)
                && select(dePanel, "deesser_target", AudioMixer::kMasterTrackId)) {
                const auto master = [&] { return mixer->masterChain(); };
                const double gain0 = master().eq.lowMid.gainDb;
                const bool de0 = master().deesser.enabled;
                const auto pcm0 = masterPcm(mixer);
                quint64 serialBefore = serial();
                drag(gain, {-2, -4, -6, -8, -10});
                const quint64 eqSaves = serial() - serialBefore;
                const double gain1 = master().eq.lowMid.gainDb;
                const auto pcm1 = masterPcm(mixer);
                serialBefore = serial();
                deEnabled->setChecked(!de0);
                const quint64 deSaves = serial() - serialBefore;
                const bool de2 = master().deesser.enabled;
                const auto pcm2 = masterPcm(mixer);
                timeline->undo();
                const bool deUndo = master().deesser.enabled;
                const auto pcmUndoDe = masterPcm(mixer);
                timeline->undo();
                const double gainUndo = master().eq.lowMid.gainDb;
                const auto pcmUndoEq = masterPcm(mixer);
                timeline->redo();
                const double gainRedo = master().eq.lowMid.gainDb;
                const auto pcmRedoEq = masterPcm(mixer);
                timeline->redo();
                const bool deRedo = master().deesser.enabled;
                const auto pcmRedoDe = masterPcm(mixer);
                // Unrelated clip edit, then a master write that records no undo.
                QString editError;
                serialBefore = serial();
                const bool deleted = timeline->deleteClipByIndex(true, 1, 0, false, &editError);
                const quint64 editSaves = serial() - serialBefore;
                mixer->setMasterEq(AudioMixer::EqSettings{}, false);
                mixer->setMasterDeEsser(trackfx::DeEsserSettings{});
                const auto pcmDrift = masterPcm(mixer);
                timeline->undo();
                const double gainBack = master().eq.lowMid.gainDb;
                const bool deBack = master().deesser.enabled;
                const auto pcmBack = masterPcm(mixer);
                std::fprintf(stderr,
                    "audio-trackfx G35 master: eq gain %g -> %g (saves=%llu, pcm rms diff %.1f), "
                    "deesser %d -> %d (saves=%llu, pcm rms diff %.1f)\n", gain0, gain1,
                    static_cast<unsigned long long>(eqSaves), rmsDiff(pcm0, pcm1), de0, de2,
                    static_cast<unsigned long long>(deSaves), rmsDiff(pcm1, pcm2));
                std::fprintf(stderr,
                    "audio-trackfx G35 master: undo deesser=%d pcm==before=%d (rms %.1f); undo eq "
                    "gain=%g pcm==before=%d (rms %.1f); redo eq gain=%g pcm==after=%d (rms %.1f); "
                    "redo deesser=%d pcm==after=%d (rms %.1f)\n", deUndo, pcmUndoDe == pcm1,
                    rmsDiff(pcmUndoDe, pcm1), gainUndo, pcmUndoEq == pcm0, rmsDiff(pcmUndoEq, pcm0),
                    gainRedo, pcmRedoEq == pcm1, rmsDiff(pcmRedoEq, pcm1), deRedo,
                    pcmRedoDe == pcm2, rmsDiff(pcmRedoDe, pcm2));
                std::fprintf(stderr,
                    "audio-trackfx G35 unrelated clip delete: ok=%d saves=%llu error=%s; drift pcm "
                    "rms %.1f; after its undo gain=%g deesser=%d pcm==snapshot=%d (rms %.1f)\n",
                    deleted, static_cast<unsigned long long>(editSaves),
                    editError.toUtf8().constData(), rmsDiff(pcmDrift, pcm2), gainBack, deBack,
                    pcmBack == pcm2, rmsDiff(pcmBack, pcm2));
                ok = eqSaves == 1 && same(gain1, -10.0) && pcm1 != pcm0
                    && deSaves == 1 && de2 != de0 && pcm2 != pcm1
                    && deUndo == de0 && pcmUndoDe == pcm1
                    && same(gainUndo, gain0) && pcmUndoEq == pcm0
                    && same(gainRedo, gain1) && pcmRedoEq == pcm1
                    && deRedo == de2 && pcmRedoDe == pcm2
                    && deleted && editSaves == 1 && pcmDrift != pcm2
                    && same(gainBack, gain1) && deBack == de2 && pcmBack == pcm2;
            }
            gate(35, ok);
        }
    }
    {
        // G36 (negative control): applyTrackState leaves the master alone without
        // "masterChain", and with a different one moves chain and processor.
        auto m = std::make_unique<AudioMixer>();
        AudioMixer::EqSettings eqA;
        eqA.lowMid = {250.0, -10.0, 1.0, true};
        trackfx::DeEsserSettings deA;
        deA.enabled = true;
        m->setMasterEq(eqA, true);
        m->setMasterDeEsser(deA);
        const QJsonObject snapshotA = m->collectTrackState();
        const auto pcmA = masterPcm(m.get());
        AudioMixer::EqSettings eqB;
        eqB.lowMid = {250.0, 6.0, 1.0, true};
        m->setMasterEq(eqB, true);
        m->setMasterDeEsser(trackfx::DeEsserSettings{});
        const auto pcmB = masterPcm(m.get());
        QJsonObject withoutMaster = snapshotA;
        withoutMaster.remove(QStringLiteral("masterChain"));
        m->applyTrackState(withoutMaster);
        const auto kept = m->masterChain();
        const auto pcmKept = masterPcm(m.get());
        m->applyTrackState(snapshotA);
        const auto restored = m->masterChain();
        const auto pcmRestored = masterPcm(m.get());
        std::fprintf(stderr,
            "audio-trackfx G36: snapshot has masterChain=%d; A vs B pcm rms %.1f; without key: "
            "gain=%g deesser=%d pcm==B=%d (rms %.1f); with A: gain=%g deesser=%d pcm==A=%d (rms %.1f)\n",
            snapshotA.contains(QStringLiteral("masterChain")), rmsDiff(pcmA, pcmB),
            kept.eq.lowMid.gainDb, kept.deesser.enabled, pcmKept == pcmB, rmsDiff(pcmKept, pcmB),
            restored.eq.lowMid.gainDb, restored.deesser.enabled, pcmRestored == pcmA,
            rmsDiff(pcmRestored, pcmA));
        // The de-esser listen monitor is not undo state: a snapshot moves the
        // master EQ (chain and processor) but keeps the live listen toggle, in
        // both directions. With the de-esser on, listen changes the output.
        AudioMixer::EqSettings eqLive, eqSnap;
        eqLive.highMid = {6000.0, 6.0, 1.0, true};
        eqSnap.highMid = {6000.0, -9.0, 1.0, true};
        const auto configure = [](AudioMixer *mx, const AudioMixer::EqSettings &eq, bool listen) {
            trackfx::DeEsserSettings de;
            de.enabled = true;
            de.listen = listen;
            mx->setMasterEq(eq, true);
            mx->setMasterDeEsser(de);
        };
        // Snapshots and reference outputs (eqSnap) for listen off / on.
        auto ref = std::make_unique<AudioMixer>();
        QJsonObject listenSnapshot[2];
        std::vector<int32_t> refPcm[2];
        for (int listen : {0, 1}) {
            configure(ref.get(), eqSnap, listen);
            listenSnapshot[listen] = ref->collectTrackState();
            refPcm[listen] = masterPcm(ref.get());
        }
        bool listenOk = refPcm[0] != refPcm[1];
        for (int live : {1, 0}) {
            configure(m.get(), eqLive, live);
            const auto pcmLive = masterPcm(m.get());
            const QJsonObject &snapshot = listenSnapshot[!live];
            m->applyTrackState(snapshot);
            const auto after = m->masterChain();
            const auto pcmAfter = masterPcm(m.get());
            std::fprintf(stderr,
                "audio-trackfx G36 listen: live listen=%d eq highMid %g; snapshot listen=%d eq "
                "highMid %g -> chain highMid=%g listen=%d; pcm vs live rms %.1f, pcm==ref(listen=%d)=%d "
                "(rms %.1f), pcm vs ref(listen=%d) rms %.1f; ref listen on vs off rms %.1f\n",
                live, eqLive.highMid.gainDb,
                snapshot.value("masterChain").toObject().value("deesser").toObject()
                    .value("listen").toBool(false),
                eqSnap.highMid.gainDb, after.eq.highMid.gainDb, after.deesser.listen,
                rmsDiff(pcmAfter, pcmLive), live, pcmAfter == refPcm[live],
                rmsDiff(pcmAfter, refPcm[live]), !live, rmsDiff(pcmAfter, refPcm[!live]),
                rmsDiff(refPcm[0], refPcm[1]));
            listenOk = listenOk && after.eq.highMid.gainDb == eqSnap.highMid.gainDb
                && after.deesser.listen == (live == 1) && pcmAfter != pcmLive
                && pcmAfter == refPcm[live] && pcmAfter != refPcm[!live];
        }
        gate(36, snapshotA.contains(QStringLiteral("masterChain")) && pcmA != pcmB
            && kept.eq.lowMid.gainDb == 6.0 && !kept.deesser.enabled && pcmKept == pcmB
            && restored.eq.lowMid.gainDb == -10.0 && restored.deesser.enabled
            && pcmRestored == pcmA && listenOk);
    }
    // G37 / G39 (US-1010): the DeEsser listen monitor is live state. A
    // structural undo / redo moves it with the track it belongs to, never by
    // track index. A1 is empty, A2 holds a clip; both have the de-esser on.
    // Detects the regression to restoring the snapshot's listen (stale after a
    // later toggle off, dropped after a later toggle on) and, in G39 (b), to
    // copying listen by index (A2's monitor lands on the restored A1).
    bool listenControlOk = false;
    bool deleteCycleOk = false;
    {
        MainWindow window;
        auto *mixer = window.m_player ? window.m_player->audioMixer() : nullptr;
        Timeline *timeline = window.m_timeline;
        if (timeline)
            timeline->restoreFromProject(QVector<QVector<ClipInfo>>{},
                QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}, QVector<ClipInfo>{audioClip}},
                0.0, -1.0, -1.0, 100);
        if (mixer && timeline) window.openDeEsserPanel();
        QWidget *dePanel = window.m_deesserDock ? window.m_deesserDock->widget() : nullptr;
        auto *deEnabled = dePanel ? dePanel->findChild<QCheckBox *>("deesser_enabled") : nullptr;
        auto *deListen = dePanel ? dePanel->findChild<QCheckBox *>("deesser_listen") : nullptr;
        auto *deTarget = dePanel ? dePanel->findChild<QComboBox *>("deesser_target") : nullptr;
        const bool ready = mixer && timeline && deEnabled && deListen && deTarget;
        const auto selectTrack = [&](int track) {
            const int index = deTarget->findData(track);
            if (index >= 0) deTarget->setCurrentIndex(index);
            return index >= 0;
        };
        // The dock's listen checkbox for one audio track (no undo entry).
        const auto setListen = [&](int track, bool on) {
            if (!selectTrack(track)) return false;
            if (deListen->isChecked() != on) deListen->setChecked(on);
            return mixer->trackChain(track).deesser.listen == on;
        };
        const auto listenOf = [&](int track) { return mixer->trackChain(track).deesser.listen; };
        // Prints the per-track listen state and compares it with `expected`.
        const auto expect = [&](const char *step, const std::vector<int> &expected) {
            bool ok = timeline->audioTrackCount() == static_cast<int>(expected.size());
            std::string actual, wanted;
            for (int i = 0; i < timeline->audioTrackCount(); ++i)
                actual += " idx" + std::to_string(i) + "=" + std::to_string(listenOf(i));
            for (size_t i = 0; i < expected.size(); ++i) {
                wanted += " idx" + std::to_string(i) + "=" + std::to_string(expected[i]);
                ok = ok && i < static_cast<size_t>(timeline->audioTrackCount())
                    && listenOf(static_cast<int>(i)) == (expected[i] != 0);
            }
            std::fprintf(stderr, "audio-trackfx G37 %s: tracks=%d listen%s (expected%s) %s\n",
                step, timeline->audioTrackCount(), actual.c_str(), wanted.c_str(),
                ok ? "ok" : "MISMATCH");
            return ok;
        };
        bool setupOk = false, clipOk = false, cycleOk = false;
        bool a1AfterUndo[2] = {true, true};
        bool a2AfterUndo[2] = {false, false};
        if (ready) {
            // Both tracks carry a de-esser chain, so a listen copied by index
            // has a restored A1 chain to land on.
            for (int track : {0, 1}) {
                setupOk = selectTrack(track) && !deEnabled->isChecked();
                if (setupOk) deEnabled->setChecked(true);
                if (!setupOk) break;
            }
            setupOk = setupOk && mixer->trackChain(0).deesser.enabled
                && mixer->trackChain(1).deesser.enabled && setListen(1, true);

            // G39 (a): a clip delete is no structural edit; its undo rewrites
            // the drifted de-esser through restoreAudioFxUndoState and keeps listen.
            const double thresholdBefore = mixer->trackChain(1).deesser.thresholdDb;
            QString editError;
            const bool deleted = timeline->deleteClipByIndex(true, 1, 0, false, &editError);
            auto drift = mixer->trackChain(1);
            drift.deesser.thresholdDb = thresholdBefore - 12.0;
            mixer->setTrackChain(1, drift);
            timeline->undo();
            const double thresholdUndo = mixer->trackChain(1).deesser.thresholdDb;
            const int clipsUndo = timeline->audioTrackCount() > 1
                ? int(timeline->audioTracks()[1]->clips().size()) : -1;
            std::fprintf(stderr,
                "audio-trackfx G39 (a) clip delete: ok=%d error=%s; drifted threshold %g; after "
                "undo tracks=%d A2 clips=%d threshold=%g (snapshot %g) listen idx0=%d idx1=%d\n",
                deleted, editError.toUtf8().constData(), drift.deesser.thresholdDb,
                timeline->audioTrackCount(), clipsUndo, thresholdUndo, thresholdBefore,
                listenOf(0), listenOf(1));
            clipOk = deleted && timeline->audioTrackCount() == 2 && clipsUndo == 1
                && thresholdUndo == thresholdBefore && !listenOf(0) && listenOf(1);

            // Cycle 1: listen on before the track delete (it is in the snapshot).
            QString removeError;
            bool removed = timeline->removeTrack(true, 0, &removeError);
            cycleOk = removed && expect("cycle 1 delete A1", {1});
            timeline->undo();
            a1AfterUndo[0] = listenOf(0);
            a2AfterUndo[0] = listenOf(1);
            cycleOk = expect("cycle 1 undo", {0, 1}) && cycleOk;
            timeline->redo();
            cycleOk = expect("cycle 1 redo", {1}) && cycleOk;
            // Listen off after the snapshot: the undo must not bring back the
            // snapshot's listen.
            cycleOk = setListen(0, false) && cycleOk;
            timeline->undo();
            cycleOk = expect("listen off, then undo", {0, 0}) && cycleOk;

            // Cycle 2: listen on only after the track delete (no snapshot has it).
            cycleOk = setListen(1, false) && cycleOk;
            removed =timeline->removeTrack(true, 0, &removeError) && removed;
            cycleOk = removed && expect("cycle 2 delete A1", {0}) && cycleOk;
            cycleOk = setListen(0, true) && cycleOk;
            timeline->undo();
            a1AfterUndo[1] = listenOf(0);
            a2AfterUndo[1] = listenOf(1);
            cycleOk = expect("cycle 2 listen on, then undo", {0, 1}) && cycleOk;
            timeline->redo();
            cycleOk = expect("cycle 2 redo", {1}) && cycleOk;
            std::fprintf(stderr, "audio-trackfx G37: setup=%d removeTrack ok=%d error=%s\n",
                         setupOk, removed, removeError.toUtf8().constData());
        } else {
            std::fprintf(stderr, "audio-trackfx G37: de-esser dock controls not found\n");
        }
        deleteCycleOk = setupOk && cycleOk;
        // G39 (b): the live listen of A2 (index 0 while A1 is deleted) must not
        // land on the restored A1 at index 0.
        std::fprintf(stderr,
            "audio-trackfx G39 (b) live idx0 (A2) -> restored idx1: cycle 1 A1 idx0=%d A2 idx1=%d; "
            "cycle 2 A1 idx0=%d A2 idx1=%d\n", a1AfterUndo[0], a2AfterUndo[0],
            a1AfterUndo[1], a2AfterUndo[1]);
        listenControlOk = setupOk && clipOk && !a1AfterUndo[0] && !a1AfterUndo[1];
    }
    // G37 (US-1010, add / move): the listen of an added A2 follows it through
    // add -> undo -> redo, never stays on the vacated index or reaches a track
    // added later, and a move undo / redo keeps it on the moved track although
    // no snapshot holds a chain for it (listen set with setTrackChain, no undo
    // entry). Detects an unused index sharing the identity of an undone track
    // (listen off after redo) and listen carried only into snapshot chains.
    bool addCycleOk = false;
    {
        MainWindow window;
        auto *mixer = window.m_player ? window.m_player->audioMixer() : nullptr;
        Timeline *timeline = window.m_timeline;
        if (mixer && timeline) {
            timeline->restoreFromProject(QVector<QVector<ClipInfo>>{},
                QVector<QVector<ClipInfo>>{QVector<ClipInfo>{audioClip}}, 0.0, -1.0, -1.0, 100);
            const auto listenOf = [&](int track) { return mixer->trackChain(track).deesser.listen; };
            const auto listenOn = [&](int track) {
                auto chain = mixer->trackChain(track);
                chain.deesser.listen = true;
                mixer->setTrackChain(track, chain);
            };
            // Checks every mixer index: indices past the tracks must stay off.
            const auto expect = [&](const char *step, const std::vector<int> &expected) {
                bool ok = timeline->audioTrackCount() == static_cast<int>(expected.size());
                std::string actual, wanted;
                for (int i = 0; i < AudioMixer::kMaxAudioTracks; ++i) {
                    const bool want = i < static_cast<int>(expected.size()) && expected[i] != 0;
                    if (i < timeline->audioTrackCount() || listenOf(i))
                        actual += " idx" + std::to_string(i) + "=" + std::to_string(listenOf(i));
                    if (i < static_cast<int>(expected.size()))
                        wanted += " idx" + std::to_string(i) + "=" + std::to_string(want);
                    ok = ok && listenOf(i) == want;
                }
                std::fprintf(stderr, "audio-trackfx G37 %s: tracks=%d listen%s (expected%s) %s\n",
                    step, timeline->audioTrackCount(), actual.c_str(), wanted.c_str(),
                    ok ? "ok" : "MISMATCH");
                return ok;
            };
            addCycleOk = expect("add: one track", {0});
            timeline->addAudioTrack(true);
            listenOn(1);
            addCycleOk = expect("add A2, listen on", {0, 1}) && addCycleOk;
            timeline->undo();
            addCycleOk = expect("add undo", {0}) && addCycleOk;
            timeline->redo();
            addCycleOk = expect("add redo", {0, 1}) && addCycleOk;
            timeline->undo();
            addCycleOk = expect("add undo again", {0}) && addCycleOk;
            timeline->addAudioTrack(true);
            addCycleOk = expect("fresh add, no redo", {0, 0}) && addCycleOk;
            QString moveError;
            const bool moved = timeline->moveTrack(true, 1, 0, &moveError);
            listenOn(0);
            addCycleOk = expect("move A2 up, listen on", {1, 0}) && moved && addCycleOk;
            timeline->undo();
            addCycleOk = expect("move undo", {0, 1}) && addCycleOk;
            timeline->redo();
            addCycleOk = expect("move redo", {1, 0}) && addCycleOk;
            std::fprintf(stderr, "audio-trackfx G37 add / move: moveTrack ok=%d error=%s\n",
                         moved, moveError.toUtf8().constData());
        } else {
            std::fprintf(stderr, "audio-trackfx G37 add / move: mixer or timeline missing\n");
        }
    }
    gate(37, deleteCycleOk && addCycleOk);
    {
        // G38 (US-1010): setMasterChain writes the master EQ and DeEsser in one
        // locked section (one applyMasterChainLocked call); the two-setter path
        // (negative control) takes two and reaches the same output.
        AudioMixer::EqSettings eq;
        eq.lowMid = {250.0, -10.0, 1.0, true};
        trackfx::DeEsserSettings de;
        de.enabled = true;
        de.thresholdDb = -30.0;
        const auto calls = [](AudioMixer *m) { return m->applyMasterChainLockedCallCountForTest(); };
        auto one = std::make_unique<AudioMixer>();
        auto two = std::make_unique<AudioMixer>();
        auto eqOnly = std::make_unique<AudioMixer>();
        auto deOnly = std::make_unique<AudioMixer>();
        const auto pcmDefault = masterPcm(one.get());
        quint64 before = calls(one.get());
        one->setMasterChain(eq, true, de);
        const quint64 oneCalls = calls(one.get()) - before;
        const auto chain = one->masterChain();
        const auto pcmOne = masterPcm(one.get());
        before = calls(two.get());
        two->setMasterEq(eq, true);
        two->setMasterDeEsser(de);
        const quint64 twoCalls = calls(two.get()) - before;
        const auto pcmTwo = masterPcm(two.get());
        eqOnly->setMasterEq(eq, true);
        const auto pcmEqOnly = masterPcm(eqOnly.get());
        deOnly->setMasterDeEsser(de);
        const auto pcmDeOnly = masterPcm(deOnly.get());
        // EQ disabled in the same call: only the DeEsser acts.
        before = calls(one.get());
        one->setMasterChain(eq, false, de);
        const quint64 offCalls = calls(one.get()) - before;
        const auto chainOff = one->masterChain();
        const auto pcmOff = masterPcm(one.get());
        std::fprintf(stderr,
            "audio-trackfx G38: setMasterChain calls=%llu (two setters %llu); chain eqEnabled=%d "
            "lowMid=%g deesser enabled=%d threshold=%g; pcm==two setters=%d (rms %.1f), vs default "
            "rms %.1f, vs eq only rms %.1f, vs deesser only rms %.1f\n",
            static_cast<unsigned long long>(oneCalls), static_cast<unsigned long long>(twoCalls),
            chain.eqEnabled, chain.eq.lowMid.gainDb, chain.deesser.enabled,
            chain.deesser.thresholdDb, pcmOne == pcmTwo, rmsDiff(pcmOne, pcmTwo),
            rmsDiff(pcmOne, pcmDefault), rmsDiff(pcmOne, pcmEqOnly), rmsDiff(pcmOne, pcmDeOnly));
        std::fprintf(stderr,
            "audio-trackfx G38 eq off: calls=%llu eqEnabled=%d lowMid=%g deesser=%d pcm==deesser "
            "only=%d (rms %.1f)\n", static_cast<unsigned long long>(offCalls), chainOff.eqEnabled,
            chainOff.eq.lowMid.gainDb, chainOff.deesser.enabled, pcmOff == pcmDeOnly,
            rmsDiff(pcmOff, pcmDeOnly));
        gate(38, oneCalls == 1 && twoCalls == 2 && chain.eqEnabled
            && chain.eq.lowMid.gainDb == -10.0 && chain.deesser.enabled
            && chain.deesser.thresholdDb == -30.0 && pcmOne == pcmTwo && pcmOne != pcmDefault
            && pcmOne != pcmEqOnly && pcmOne != pcmDeOnly && pcmEqOnly != pcmDefault
            && pcmDeOnly != pcmDefault && offCalls == 1 && !chainOff.eqEnabled
            && chainOff.deesser.enabled && pcmOff == pcmDeOnly);
    }
    gate(39, listenControlOk);
    return tally.finish("audio-trackfx");
}
