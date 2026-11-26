#include "../ExportDialog.h"
#include "../LoudnessPresets.h"
#include "../PlaybackTypes.h"
#include "../Timeline.h"
#include "SelftestGate.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstdio>

namespace {

bool writeToneWav(const QString &path, double durationSec, double amplitude, int sampleRate = 48000)
{
    const int numSamples = qMax(1, static_cast<int>(std::round(durationSec * sampleRate)));
    const int channels = 2;
    const int bitsPerSample = 16;
    const int dataBytes = numSamples * channels * (bitsPerSample / 8);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;

    auto writeU32 = [&](quint32 v) {
        char b[4];
        qToLittleEndian(v, reinterpret_cast<uchar *>(b));
        f.write(b, 4);
    };
    auto writeU16 = [&](quint16 v) {
        char b[2];
        qToLittleEndian(v, reinterpret_cast<uchar *>(b));
        f.write(b, 2);
    };

    f.write("RIFF", 4);
    writeU32(static_cast<quint32>(36 + dataBytes));
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    writeU32(16);
    writeU16(1);
    writeU16(static_cast<quint16>(channels));
    writeU32(static_cast<quint32>(sampleRate));
    writeU32(static_cast<quint32>(sampleRate * channels * (bitsPerSample / 8)));
    writeU16(static_cast<quint16>(channels * (bitsPerSample / 8)));
    writeU16(static_cast<quint16>(bitsPerSample));
    f.write("data", 4);
    writeU32(static_cast<quint32>(dataBytes));

    QByteArray pcm(dataBytes, '\0');
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < numSamples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double v = qBound(-1.0, amplitude * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t), 1.0);
        const qint16 s = static_cast<qint16>(std::lround(v * 32767.0));
        samples[i * 2] = s;
        samples[i * 2 + 1] = s;
    }
    f.write(pcm);
    f.close();
    return true;
}

} // namespace

int runPlatformLoudnessSelftest()
{
    selftest::GateTally tally;
    auto rec = [&](int n, bool ok, const QString &detail) {
        tally.gate(n, ok, detail.toStdString());
    };

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        std::fprintf(stderr, "platform-loudness: failed to create temp dir\n");
        return 1;
    }

    // G1: Preset catalog & ExportDialog index mapping (-14 -> 0, -23 -> 1, -16 -> 2, -24 -> 3, custom -> 4).
    {
        const auto expPresets = loudnesspreset::exportDialogPresets();
        const auto tlPresets = loudnesspreset::timelinePresets();
        const bool ok = (expPresets.size() == 4)
            && (tlPresets.size() == 5)
            && (loudnesspreset::exportPresetIndexForLufs(-14.0) == 0)
            && (loudnesspreset::exportPresetIndexForLufs(-23.0) == 1)
            && (loudnesspreset::exportPresetIndexForLufs(-16.0) == 2)
            && (loudnesspreset::exportPresetIndexForLufs(-24.0) == 3)
            && (loudnesspreset::exportPresetIndexForLufs(-18.5) == 4);
        rec(1, ok, QStringLiteral("exp=%1 tl=%2 idx14=%3 idx23=%4 idx16=%5 idx24=%6 idxCustom=%7")
                       .arg(expPresets.size()).arg(tlPresets.size())
                       .arg(loudnesspreset::exportPresetIndexForLufs(-14.0))
                       .arg(loudnesspreset::exportPresetIndexForLufs(-23.0))
                       .arg(loudnesspreset::exportPresetIndexForLufs(-16.0))
                       .arg(loudnesspreset::exportPresetIndexForLufs(-24.0))
                       .arg(loudnesspreset::exportPresetIndexForLufs(-18.5)));
    }

    // G2: computeTargetClipGain accuracy, true-peak ceiling clamping, and kMaxClipGain cap.
    {
        // Normal boost: -20 LUFS -> -14 LUFS (+6 dB => ~1.9953 linear), peak -12 dBTP -> -6 dBTP (not clamped)
        const auto gNorm = loudnesspreset::computeTargetClipGain(-20.0, -12.0, -14.0, -1.0);
        // Peak-clamped boost: -22 LUFS -> -14 LUFS wants +8 dB, but peak is -4 dBTP so capped at +3 dB (-1 dBTP)
        const auto gPeak = loudnesspreset::computeTargetClipGain(-22.0, -4.0, -14.0, -1.0);
        // Max clip gain cap: -35 LUFS -> -14 LUFS wants +21 dB, capped at audio::kMaxClipGain (4.0 = +12.04 dB)
        const auto gCap = loudnesspreset::computeTargetClipGain(-35.0, -30.0, -14.0, -1.0);

        const bool ok = gNorm.valid && !gNorm.peakClamped && !gNorm.maxGainClamped
            && std::abs(gNorm.gainDb - 6.0) < 0.05
            && gPeak.valid && gPeak.peakClamped && std::abs(gPeak.gainDb - 3.0) < 0.05
            && gCap.valid && gCap.maxGainClamped && std::abs(gCap.gainLinear - audio::kMaxClipGain) < 1e-6;
        rec(2, ok, QStringLiteral("normDb=%1 peakDb=%2(clamp=%3) capLin=%4(clamp=%5)")
                       .arg(gNorm.gainDb, 0, 'f', 2)
                       .arg(gPeak.gainDb, 0, 'f', 2)
                       .arg(gPeak.peakClamped ? 1 : 0)
                       .arg(gCap.gainLinear, 0, 'f', 2)
                       .arg(gCap.maxGainClamped ? 1 : 0));
    }

    const QString quietWav = tmpDir.filePath(QStringLiteral("quiet.wav"));
    const QString loudWav = tmpDir.filePath(QStringLiteral("loud.wav"));
    const bool wavsOk = writeToneWav(quietWav, 1.5, 0.08) // ~ -25 LUFS
                     && writeToneWav(loudWav, 1.5, 0.25); // ~ -15 LUFS

    // G3: Scope::SingleClip normalizes only the targeted clip and undo restores original volume.
    {
        Timeline tl;
        ClipInfo c0;
        c0.filePath = quietWav;
        c0.displayName = QStringLiteral("Quiet");
        c0.duration = 1.5;
        c0.outPoint = 1.5;
        c0.volume = 1.0;

        ClipInfo c1;
        c1.filePath = loudWav;
        c1.displayName = QStringLiteral("Loud");
        c1.duration = 1.5;
        c1.outPoint = 1.5;
        c1.volume = 1.0;

        tl.audioTracks()[0]->addClip(c0);
        tl.audioTracks()[0]->addClip(c1);
        tl.saveUndoState(QStringLiteral("Base"));

        const auto res = loudnesspreset::applyLoudnessNormalization(
            &tl, loudnesspreset::NormalizeScope::SingleClip, 0, 0, -14.0, -1.0,
            QStringLiteral("YouTube (-14 LUFS)"));
        const double v0After = tl.audioTracks()[0]->clips()[0].volume;
        const double v1After = tl.audioTracks()[0]->clips()[1].volume;

        // Re-measure c0 at new volume -> should be close to -14 LUFS
        const auto checkM = loudnesspreset::measureClipLoudness(tl.audioTracks()[0]->clips()[0], v0After);

        tl.undo();
        const double v0Undo = tl.audioTracks()[0]->clips()[0].volume;

        const bool ok = wavsOk && res.ok && res.clipsUpdated == 1
            && v0After > 2.0 && std::abs(v1After - 1.0) < 1e-6
            && checkM.valid && std::abs(checkM.integratedLufs - (-14.0)) < 0.8
            && std::abs(v0Undo - 1.0) < 1e-6;
        rec(3, ok, QStringLiteral("v0=%1 v1=%2 postLufs=%3 undo=%4")
                       .arg(v0After, 0, 'f', 3)
                       .arg(v1After, 0, 'f', 3)
                       .arg(checkM.integratedLufs, 0, 'f', 2)
                       .arg(v0Undo, 0, 'f', 3));
    }

    // G4: Scope::AllClipsIndividual brings both clips individually to -14 LUFS in 1 undo step.
    {
        Timeline tl;
        ClipInfo c0; c0.filePath = quietWav; c0.duration = 1.5; c0.outPoint = 1.5; c0.volume = 1.0;
        ClipInfo c1; c1.filePath = loudWav;  c1.duration = 1.5; c1.outPoint = 1.5; c1.volume = 1.0;
        tl.audioTracks()[0]->addClip(c0);
        tl.audioTracks()[0]->addClip(c1);
        tl.saveUndoState(QStringLiteral("Base"));

        const auto res = loudnesspreset::applyLoudnessNormalization(
            &tl, loudnesspreset::NormalizeScope::AllClipsIndividual, 0, 0, -14.0, -1.0,
            QStringLiteral("YouTube (-14 LUFS)"));
        const double v0 = tl.audioTracks()[0]->clips()[0].volume;
        const double v1 = tl.audioTracks()[0]->clips()[1].volume;
        const auto m0 = loudnesspreset::measureClipLoudness(tl.audioTracks()[0]->clips()[0], v0);
        const auto m1 = loudnesspreset::measureClipLoudness(tl.audioTracks()[0]->clips()[1], v1);

        tl.undo();
        const double v0Undo = tl.audioTracks()[0]->clips()[0].volume;
        const double v1Undo = tl.audioTracks()[0]->clips()[1].volume;

        const bool ok = wavsOk && res.ok && res.clipsUpdated == 2
            && v0 > v1
            && m0.valid && std::abs(m0.integratedLufs - (-14.0)) < 0.8
            && m1.valid && std::abs(m1.integratedLufs - (-14.0)) < 0.8
            && std::abs(v0Undo - 1.0) < 1e-6 && std::abs(v1Undo - 1.0) < 1e-6;
        rec(4, ok, QStringLiteral("v0=%1(%2) v1=%3(%4) undo=%5/%6")
                       .arg(v0, 0, 'f', 3).arg(m0.integratedLufs, 0, 'f', 1)
                       .arg(v1, 0, 'f', 3).arg(m1.integratedLufs, 0, 'f', 1)
                       .arg(v0Undo, 0, 'f', 2).arg(v1Undo, 0, 'f', 2));
    }

    // G5: Scope::AllClipsUniform preserves relative balance (vol1 / vol0) and reverts in 1 undo step.
    {
        Timeline tl;
        ClipInfo c0; c0.filePath = quietWav; c0.duration = 1.5; c0.outPoint = 1.5; c0.volume = 1.0;
        ClipInfo c1; c1.filePath = quietWav; c1.duration = 1.5; c1.outPoint = 1.5; c1.volume = 0.5;
        tl.audioTracks()[0]->addClip(c0);
        tl.audioTracks()[0]->addClip(c1);
        tl.saveUndoState(QStringLiteral("Base"));

        const auto res = loudnesspreset::applyLoudnessNormalization(
            &tl, loudnesspreset::NormalizeScope::AllClipsUniform, 0, 0, -20.0, -1.0,
            QStringLiteral("Custom -20 LUFS"));
        const double v0 = tl.audioTracks()[0]->clips()[0].volume;
        const double v1 = tl.audioTracks()[0]->clips()[1].volume;
        const double ratioAfter = v0 > 0.0 ? (v1 / v0) : 0.0;

        tl.undo();
        const double v0Undo = tl.audioTracks()[0]->clips()[0].volume;
        const double v1Undo = tl.audioTracks()[0]->clips()[1].volume;

        const bool ok = wavsOk && res.ok && res.clipsUpdated == 2
            && v0 > 1.2 && std::abs(ratioAfter - 0.5) < 1e-4
            && std::abs(v0Undo - 1.0) < 1e-6 && std::abs(v1Undo - 0.5) < 1e-6;
        rec(5, ok, QStringLiteral("v0=%1 v1=%2 ratio=%3 undo=%4/%5")
                       .arg(v0, 0, 'f', 3).arg(v1, 0, 'f', 3).arg(ratioAfter, 0, 'f', 4)
                       .arg(v0Undo, 0, 'f', 2).arg(v1Undo, 0, 'f', 2));
    }

    // G6: Locked and muted audio tracks are skipped safely in all-clips mode.
    {
        Timeline tl;
        tl.addAudioTrack(false); // A2 (locked)
        tl.addAudioTrack(false); // A3 (muted)
        ClipInfo c0; c0.filePath = quietWav; c0.duration = 1.5; c0.outPoint = 1.5; c0.volume = 1.0;
        tl.audioTracks()[0]->addClip(c0);
        tl.audioTracks()[1]->addClip(c0);
        tl.audioTracks()[2]->addClip(c0);
        tl.audioTracks()[1]->setLocked(true);
        tl.audioTracks()[2]->setMuted(true);

        const auto res = loudnesspreset::applyLoudnessNormalization(
            &tl, loudnesspreset::NormalizeScope::AllClipsIndividual, 0, 0, -16.0, -1.0,
            QStringLiteral("X (-16 LUFS)"));
        const bool ok = wavsOk && res.ok && res.clipsUpdated == 1 && res.clipsSkipped == 2
            && tl.audioTracks()[0]->clips()[0].volume > 1.5
            && std::abs(tl.audioTracks()[1]->clips()[0].volume - 1.0) < 1e-6
            && std::abs(tl.audioTracks()[2]->clips()[0].volume - 1.0) < 1e-6;
        rec(6, ok, QStringLiteral("updated=%1 skipped=%2").arg(res.clipsUpdated).arg(res.clipsSkipped));
    }

    // G7: NormalizeOptionsDialog scope radios, preset combo -> LUFS spin, custom edit sync.
    {
        loudnesspreset::NormalizeOptionsDialog dlg(
            loudnesspreset::NormalizeScope::AllClipsUniform, -14.0, -1.0);
        auto *rSingle = dlg.findChild<QRadioButton *>(QStringLiteral("normScopeSingle"));
        auto *rIndiv = dlg.findChild<QRadioButton *>(QStringLiteral("normScopeAllIndividual"));
        auto *combo = dlg.findChild<QComboBox *>(QStringLiteral("normPresetCombo"));
        auto *lufsSpin = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("normLufsSpin"));
        bool ok = rSingle && rIndiv && combo && lufsSpin
            && dlg.selectedScope() == loudnesspreset::NormalizeScope::AllClipsUniform;
        if (ok) {
            rIndiv->setChecked(true);
            ok = ok && (dlg.selectedScope() == loudnesspreset::NormalizeScope::AllClipsIndividual);
            // Select X/Podcast/Apple (-16 LUFS, index 2)
            combo->setCurrentIndex(2);
            ok = ok && std::abs(dlg.targetLufs() - (-16.0)) < 1e-6;
            // Manual LUFS change switches combo to custom (last item)
            lufsSpin->setValue(-19.5);
            ok = ok && (combo->currentIndex() == combo->count() - 1)
                && std::abs(dlg.targetLufs() - (-19.5)) < 1e-6;
        }
        rec(7, ok, QStringLiteral("dialogOk=%1").arg(ok ? 1 : 0));
    }

    // G8: ExportDialog loudnessPreset reflects shared platform presets and matches loudness-auto G6 indices.
    {
        ExportDialog dialog(ProjectConfig{});
        auto *combo = dialog.findChild<QComboBox *>(QStringLiteral("loudnessPreset"));
        auto *spin = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("targetLufs"));
        bool ok = combo && spin && combo->count() == 5;
        if (ok) {
            combo->setCurrentIndex(2); // X / Podcast / Apple (-16)
            ok = ok && std::abs(spin->value() - (-16.0)) < 1e-6;
            combo->setCurrentIndex(3); // ARIB / Cinema (-24)
            ok = ok && std::abs(spin->value() - (-24.0)) < 1e-6;
            spin->setValue(-18.5);
            ok = ok && (combo->currentIndex() == 4); // 自由入力
        }
        rec(8, ok, QStringLiteral("comboCount=%1 spin=%2 idx=%3")
                       .arg(combo ? combo->count() : -1)
                       .arg(spin ? spin->value() : 0.0, 0, 'f', 1)
                       .arg(combo ? combo->currentIndex() : -1));
    }

    return tally.finish("platform-loudness");
}
