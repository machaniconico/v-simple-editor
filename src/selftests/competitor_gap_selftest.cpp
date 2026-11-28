#include "../ClipRoles.h"
#include "../CreatorTools.h"
#include "../DuplicateFrameDetector.h"
#include "../ExternalGenerator.h"
#include "../GradeVersions.h"
#include "../OtioExport.h"
#include "../OtioImport.h"
#include "../PowerBinStore.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TimelineIndexDock.h"
#include "SelftestGate.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

namespace {

bool writeWavTone(const QString &path, double durationSec, double freqHz, double amp = 0.4)
{
    const int sr = 48000;
    const int ch = 2;
    const int frames = qMax(1, qRound(durationSec * sr));
    const quint32 dataBytes = static_cast<quint32>(frames * ch * 2);
    const quint32 riffSize = 36 + dataBytes;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    auto w32 = [&](quint32 v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto w16 = [&](quint16 v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    f.write("RIFF", 4); w32(riffSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(ch); w32(sr);
    w32(sr * ch * 2); w16(ch * 2); w16(16);
    f.write("data", 4); w32(dataBytes);
    QByteArray pcm(static_cast<int>(dataBytes), '\0');
    auto *dst = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        const qint16 s = static_cast<qint16>(
            std::sin(2.0 * 3.141592653589793 * freqHz * i / sr) * amp * 32767.0);
        dst[i * 2] = s;
        dst[i * 2 + 1] = s;
    }
    return f.write(pcm) == pcm.size();
}

} // namespace

int runCompetitorGapSelftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        std::fprintf(stderr, "competitor-gap: failed to create temp dir\n");
        return 1;
    }

    const QString mediaA = tmpDir.filePath(QStringLiteral("clipA.mp4"));
    const QString mediaB = tmpDir.filePath(QStringLiteral("clipB.mp4"));
    {
        QFile f(mediaA);
        if (f.open(QIODevice::WriteOnly)) f.write("dummy-a");
    }
    {
        QFile f(mediaB);
        if (f.open(QIODevice::WriteOnly)) f.write("dummy-b");
    }

    const QString wavVoice = tmpDir.filePath(QStringLiteral("voice.wav"));
    const QString wavMusic = tmpDir.filePath(QStringLiteral("music.wav"));
    const bool wavsOk = writeWavTone(wavVoice, 1.0, 440.0, 0.35)
                     && writeWavTone(wavMusic, 1.0, 880.0, 0.25);

    const auto savedRoles = cliproles::allAssignedRoles();
    auto restoreRoles = [&]() {
        cliproles::clearAllRoles();
        for (auto it = savedRoles.cbegin(); it != savedRoles.cend(); ++it)
            cliproles::setRoleForClip(it.key(), it.value());
    };

    // G1: DuplicateFrameDetector detects overlapping source ranges, ignores disjoint & synthetic clips.
    {
        Timeline tl;
        ClipInfo c0; c0.filePath = mediaA; c0.displayName = QStringLiteral("A1"); c0.duration = 10.0; c0.inPoint = 1.0; c0.outPoint = 5.0;
        ClipInfo c1; c1.filePath = mediaA; c1.displayName = QStringLiteral("A2"); c1.duration = 10.0; c1.inPoint = 3.0; c1.outPoint = 7.0; // overlaps [3,5]
        ClipInfo c2; c2.filePath = mediaA; c2.displayName = QStringLiteral("A3"); c2.duration = 10.0; c2.inPoint = 7.5; c2.outPoint = 9.0; // disjoint
        ClipInfo c3; c3.filePath = mediaB; c3.displayName = QStringLiteral("B1"); c3.duration = 10.0; c3.inPoint = 1.0; c3.outPoint = 5.0; // different file
        tl.videoTracks()[0]->addClip(c0);
        tl.videoTracks()[0]->addClip(c1);
        tl.videoTracks()[0]->addClip(c2);
        tl.videoTracks()[0]->addClip(c3);

        const auto res = dupframes::analyzeTimeline(&tl, true, false);
        const bool ok = res.overlaps.size() == 1
            && res.distinctSources == 1
            && res.affectedClipCount == 2
            && std::abs(res.overlaps[0].overlapSourceIn - 3.0) < 1e-6
            && std::abs(res.overlaps[0].overlapSourceOut - 5.0) < 1e-6
            && res.overlaps[0].groupColor.isValid();
        rec(1, ok, QStringLiteral("overlaps=%1 sources=%2 affected=%3 range=[%4,%5]")
                       .arg(res.overlaps.size()).arg(res.distinctSources).arg(res.affectedClipCount)
                       .arg(res.overlaps.isEmpty() ? -1.0 : res.overlaps[0].overlapSourceIn, 0, 'f', 1)
                       .arg(res.overlaps.isEmpty() ? -1.0 : res.overlaps[0].overlapSourceOut, 0, 'f', 1));
    }

    // G2: OpenTimelineIO (.otio) full round-trip: OtioExport -> writeOtio -> readOtioFile -> applyOtioToTimeline.
    {
        Timeline srcTl;
        srcTl.addVideoTrack(false); // V2
        ClipInfo v1c; v1c.filePath = mediaA; v1c.displayName = QStringLiteral("V1Main"); v1c.duration = 6.0; v1c.inPoint = 1.0; v1c.outPoint = 4.0; v1c.speed = 1.5; v1c.leadInSec = 0.5;
        ClipInfo v2c; v2c.filePath = mediaB; v2c.displayName = QStringLiteral("V2Pip");  v2c.duration = 4.0; v2c.inPoint = 0.0; v2c.outPoint = 2.0;
        ClipInfo a1c; a1c.filePath = wavVoice; a1c.displayName = QStringLiteral("Voice"); a1c.duration = 1.0; a1c.inPoint = 0.0; a1c.outPoint = 1.0; a1c.volume = 0.75;
        srcTl.videoTracks()[0]->addClip(v1c);
        srcTl.videoTracks()[1]->addClip(v2c);
        srcTl.audioTracks()[0]->addClip(a1c);
        Marker mk; mk.timelineUs = 1500000; mk.durationUs = 250000; mk.label = QStringLiteral("Intro"); mk.color = QColor(0x26, 0xc6, 0xda);
        srcTl.setMarkers({mk});

        const QString otioPath = tmpDir.filePath(QStringLiteral("roundtrip.otio"));
        QString writeErr;
        const bool wrote = otio::writeOtio(&srcTl, 30.0, QStringLiteral("Wave26Seq"), otioPath, &writeErr);
        const otio::ImportResult parsed = otio::readOtioFile(otioPath);
        Timeline dstTl;
        QString applyErr;
        const bool applied = otio::applyOtioToTimeline(&dstTl, parsed, &applyErr);

        bool ok = wrote && parsed.ok && applied
            && parsed.name == QStringLiteral("Wave26Seq")
            && dstTl.videoTracks().size() == 2
            && dstTl.audioTracks().size() == 1
            && dstTl.videoTracks()[0]->clips().size() == 1
            && dstTl.videoTracks()[1]->clips().size() == 1
            && dstTl.audioTracks()[0]->clips().size() == 1
            && dstTl.markers().size() == 1
            && dstTl.markers()[0].label == QStringLiteral("Intro");
        if (ok) {
            const ClipInfo &rv1 = dstTl.videoTracks()[0]->clips()[0];
            const ClipInfo &ra1 = dstTl.audioTracks()[0]->clips()[0];
            ok = ok && QFileInfo(rv1.filePath).fileName() == QStringLiteral("clipA.mp4")
                && std::abs(rv1.inPoint - 1.0) < 0.05
                && std::abs(rv1.speed - 1.5) < 1e-4
                && std::abs(rv1.leadInSec - 0.5) < 0.05
                && std::abs(ra1.volume - 0.75) < 1e-4;
        }
        rec(2, ok, QStringLiteral("wrote=%1 parsed=%2 applied=%3 vTrk=%4 aTrk=%5 mk=%6")
                       .arg(wrote ? 1 : 0).arg(parsed.ok ? 1 : 0).arg(applied ? 1 : 0)
                       .arg(dstTl.videoTracks().size()).arg(dstTl.audioTracks().size())
                       .arg(dstTl.markers().size()));
    }

    // G3: ClipRoles persistence + Multi-Stem WAV export (Dialogue.wav & Music.wav).
    {
        cliproles::clearAllRoles();
        Timeline tl;
        tl.addAudioTrack(false); // A2
        ClipInfo aVoice; aVoice.filePath = wavVoice; aVoice.displayName = QStringLiteral("Narration"); aVoice.duration = 1.0; aVoice.outPoint = 1.0;
        ClipInfo aMusic; aMusic.filePath = wavMusic; aMusic.displayName = QStringLiteral("BGM");       aMusic.duration = 1.0; aMusic.outPoint = 1.0;
        tl.audioTracks()[0]->addClip(aVoice);
        tl.audioTracks()[1]->addClip(aMusic);
        const QString uidVoice = tl.audioTracks()[0]->clips()[0].uid;
        const QString uidMusic = tl.audioTracks()[1]->clips()[0].uid;

        cliproles::setRoleForClip(uidVoice, cliproles::Role::Dialogue);
        cliproles::setRoleForClip(uidMusic, cliproles::Role::Music);

        const QString stemDir = tmpDir.filePath(QStringLiteral("stems"));
        const auto res = cliproles::exportAudioStemsByRole(&tl, stemDir, QStringLiteral("mix"));
        const QString dlgWav = QDir(stemDir).filePath(QStringLiteral("mix_dialogue.wav"));
        const QString musWav = QDir(stemDir).filePath(QStringLiteral("mix_music.wav"));

        const bool ok = wavsOk && res.ok && res.stems.size() == 2
            && cliproles::roleForClip(uidVoice) == cliproles::Role::Dialogue
            && cliproles::roleForClip(uidMusic) == cliproles::Role::Music
            && QFileInfo(dlgWav).size() > 10000
            && QFileInfo(musWav).size() > 10000;
        rec(3, ok, QStringLiteral("stems=%1 dlgBytes=%2 musBytes=%3")
                       .arg(res.stems.size())
                       .arg(QFileInfo(dlgWav).size())
                       .arg(QFileInfo(musWav).size()));
    }

    // G4: PowerBinStore cross-project JSON persistence, category/search filtering, and dialog UI.
    {
        const QString pbJson = tmpDir.filePath(QStringLiteral("powerbins.json"));
        const auto e1 = powerbin::addAsset(mediaA, QStringLiteral("イントロ/アウトロ"), QStringLiteral("MainIntro"), pbJson);
        const auto e2 = powerbin::addAsset(wavMusic, QStringLiteral("BGM/効果音"), QStringLiteral("UpbeatBGM"), pbJson);
        const auto all = powerbin::loadAssets(pbJson);
        const auto bgmOnly = powerbin::filterAssets(all, QStringLiteral("BGM/効果音"), QString());
        const auto searchIntro = powerbin::filterAssets(all, QStringLiteral("すべて"), QStringLiteral("intro"));
        powerbin::PowerBinDialog dlg(nullptr, pbJson);
        const bool removed = powerbin::removeAsset(e1.id, pbJson);
        const auto afterRem = powerbin::loadAssets(pbJson);

        const bool ok = all.size() == 2 && bgmOnly.size() == 1 && bgmOnly[0].id == e2.id
            && searchIntro.size() == 1 && searchIntro[0].id == e1.id
            && removed && afterRem.size() == 1 && afterRem[0].id == e2.id;
        rec(4, ok, QStringLiteral("all=%1 bgm=%2 search=%3 afterRem=%4")
                       .arg(all.size()).arg(bgmOnly.size()).arg(searchIntro.size()).arg(afterRem.size()));
    }

    // G5: Color Grade Local Versions (save v1/v2, switch with 1 Undo step, delete).
    {
        Timeline tl;
        ClipInfo c;
        c.filePath = mediaA;
        c.duration = 2.0;
        c.outPoint = 2.0;
        c.colorCorrection.exposure = 0.0;
        c.colorCorrection.saturation = 0.0;
        tl.videoTracks()[0]->addClip(c);
        tl.saveUndoState(QStringLiteral("Base"));
        const QString uid = tl.videoTracks()[0]->clips()[0].uid;
        gradeversions::clearVersionsForClip(uid);

        ColorCorrection natural; natural.exposure = 0.2; natural.saturation = 10.0;
        ColorCorrection cinema;  cinema.exposure = -0.4; cinema.saturation = -25.0; cinema.temperature = 18.0;
        const auto v1 = gradeversions::saveVersion(uid, QStringLiteral("Natural"), natural);
        const auto v2 = gradeversions::saveVersion(uid, QStringLiteral("Cinematic"), cinema);

        const bool app2 = gradeversions::applyVersionToClip(&tl, 0, 0, v2.id);
        const double expAfter2 = tl.videoTracks()[0]->clips()[0].colorCorrection.exposure;
        const double tempAfter2 = tl.videoTracks()[0]->clips()[0].colorCorrection.temperature;

        const bool app1 = gradeversions::applyVersionToClip(&tl, 0, 0, v1.id);
        const double expAfter1 = tl.videoTracks()[0]->clips()[0].colorCorrection.exposure;

        tl.undo(); // back to v2
        const double expUndo = tl.videoTracks()[0]->clips()[0].colorCorrection.exposure;
        const bool del1 = gradeversions::deleteVersion(uid, v1.id);
        const int remCount = gradeversions::versionsForClip(uid).size();
        gradeversions::clearVersionsForClip(uid);

        const bool ok = app2 && app1 && del1
            && std::abs(expAfter2 - (-0.4)) < 1e-6 && std::abs(tempAfter2 - 18.0) < 1e-6
            && std::abs(expAfter1 - 0.2) < 1e-6
            && std::abs(expUndo - (-0.4)) < 1e-6
            && remCount == 1;
        rec(5, ok, QStringLiteral("v2Exp=%1 v1Exp=%2 undoExp=%3 rem=%4")
                       .arg(expAfter2, 0, 'f', 1).arg(expAfter1, 0, 'f', 1)
                       .arg(expUndo, 0, 'f', 1).arg(remCount));
    }

    // G6: TimelineIndex buildIndexRows & TimelineIndexDialog filtering (search, kind, offline, duplicates).
    {
        Timeline tl;
        ClipInfo c0; c0.filePath = mediaA; c0.displayName = QStringLiteral("HeroShot"); c0.duration = 4.0; c0.inPoint = 0.0; c0.outPoint = 3.0;
        ClipInfo c1; c1.filePath = mediaA; c1.displayName = QStringLiteral("HeroDup");  c1.duration = 4.0; c1.inPoint = 1.0; c1.outPoint = 3.0;
        ClipInfo cOff; cOff.filePath = QStringLiteral("Z:/definitely_missing_xyz_999.mp4"); cOff.displayName = QStringLiteral("MissingClip"); cOff.duration = 2.0; cOff.outPoint = 2.0;
        tl.videoTracks()[0]->addClip(c0);
        tl.videoTracks()[0]->addClip(c1);
        tl.videoTracks()[0]->addClip(cOff);
        Marker m; m.timelineUs = 500000; m.label = QStringLiteral("BeatDrop");
        tl.setMarkers({m});

        const auto allRows = tlindex::buildIndexRows(&tl);
        tlindex::FilterOptions fDup; fDup.onlyDuplicates = true;
        const auto dupRows = tlindex::buildIndexRows(&tl, fDup);
        tlindex::FilterOptions fOff; fOff.onlyOffline = true;
        const auto offRows = tlindex::buildIndexRows(&tl, fOff);
        tlindex::FilterOptions fMk; fMk.kindFilter = 3;
        const auto mkRows = tlindex::buildIndexRows(&tl, fMk);
        tlindex::TimelineIndexDialog dlg(&tl);

        const bool ok = allRows.size() == 4
            && dupRows.size() == 2
            && offRows.size() == 1 && offRows[0].name == QStringLiteral("MissingClip")
            && mkRows.size() == 1 && mkRows[0].name == QStringLiteral("BeatDrop")
            && dlg.rowCount() == 4;
        rec(6, ok, QStringLiteral("all=%1 dup=%2 off=%3 mk=%4 dlg=%5")
                       .arg(allRows.size()).arg(dupRows.size()).arg(offRows.size())
                       .arg(mkRows.size()).arg(dlg.rowCount()));
    }

    // G7: Split-Screen Collage Templates (4-up grid across V1..V4) + 1-step Undo.
    {
        Timeline tl;
        tl.addVideoTrack(false); // V2
        tl.addVideoTrack(false); // V3
        tl.addVideoTrack(false); // V4
        for (int t = 0; t < 4; ++t) {
            ClipInfo c; c.filePath = mediaA; c.duration = 3.0; c.outPoint = 3.0;
            tl.videoTracks()[t]->addClip(c);
        }
        tl.saveUndoState(QStringLiteral("Base"));

        const auto res = creatortools::applySplitScreenCollage(&tl, creatortools::CollagePreset::FourUpGrid, 0.5);
        const ClipInfo c0 = tl.videoTracks()[0]->clips()[0];
        const ClipInfo c3 = tl.videoTracks()[3]->clips()[0];
        tl.undo();
        const ClipInfo c0Undo = tl.videoTracks()[0]->clips()[0];

        const bool ok = res.ok && res.clipsUpdated == 4
            && std::abs(c0.videoScale - 0.48) < 1e-6 && c0.videoDx < -0.1 && c0.videoDy < -0.1
            && std::abs(c3.videoScale - 0.48) < 1e-6 && c3.videoDx > 0.1 && c3.videoDy > 0.1
            && std::abs(c0Undo.videoScale - 1.0) < 1e-6 && std::abs(c0Undo.videoDx) < 1e-6;
        rec(7, ok, QStringLiteral("updated=%1 c0=(%2,%3) c3=(%4,%5) undoScale=%6")
                       .arg(res.clipsUpdated)
                       .arg(c0.videoDx, 0, 'f', 2).arg(c0.videoDy, 0, 'f', 2)
                       .arg(c3.videoDx, 0, 'f', 2).arg(c3.videoDy, 0, 'f', 2)
                       .arg(c0Undo.videoScale, 0, 'f', 2));
    }

    // G8: Audio Visualizer Spectrum FFT bands (440 Hz tone produces clear peak) & all 4 styles render non-empty RGBA.
    {
        QVector<float> sine440(512);
        for (int i = 0; i < 512; ++i)
            sine440[i] = static_cast<float>(0.8 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0));
        const QVector<double> bands = creatortools::computeSpectrumBands(sine440, 24);
        double maxBand = 0.0;
        for (double b : bands) maxBand = qMax(maxBand, b);

        bool stylesOk = true;
        for (auto st : {creatortools::VisualizerStyle::SpectrumBars,
                        creatortools::VisualizerStyle::MirrorBars,
                        creatortools::VisualizerStyle::CircularRing,
                        creatortools::VisualizerStyle::WaveformLine}) {
            QJsonObject p;
            p[QStringLiteral("style")] = creatortools::visualizerStyleToString(st);
            p[QStringLiteral("transparent")] = true;
            const QImage img = creatortools::renderAudioVisualizerFrame(p, 0.5, QSize(160, 90), nullptr);
            int opaquePx = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (img.pixelColor(x, y).alpha() > 20) ++opaquePx;
            stylesOk = stylesOk && !img.isNull() && opaquePx > 30;
        }

        const bool ok = bands.size() == 24 && maxBand > 0.3 && stylesOk;
        rec(8, ok, QStringLiteral("bands=%1 maxBand=%2 stylesOk=%3")
                       .arg(bands.size()).arg(maxBand, 0, 'f', 3).arg(stylesOk ? 1 : 0));
    }

    // G9: Audio Visualizer SSOT integration through ExternalGenerator + TimelineFrameRenderer::renderFrameAt
    // reactive to real timeline audio clip.
    {
        Timeline tl;
        ClipInfo v;
        v.filePath = creatortools::buildVisualizerGeneratorUri(
            creatortools::VisualizerStyle::SpectrumBars, QColor(76, 201, 240), QColor(247, 37, 133), 24, 30.0, false);
        v.duration = 1.0;
        v.outPoint = 1.0;
        tl.videoTracks()[0]->addClip(v);

        ClipInfo a;
        a.filePath = wavVoice;
        a.duration = 1.0;
        a.outPoint = 1.0;
        tl.audioTracks()[0]->addClip(a);

        const QImage rendered = tlrender::renderFrameAt(&tl, 200000, QSize(160, 90));
        int brightPx = 0;
        for (int y = 0; y < rendered.height(); ++y) {
            for (int x = 0; x < rendered.width(); ++x) {
                const QColor c = rendered.pixelColor(x, y);
                if (c.red() > 70 || c.green() > 140 || c.blue() > 180)
                    ++brightPx;
            }
        }
        const bool ok = extgen::isRendererAvailable(QStringLiteral("visualizer"))
            && !rendered.isNull() && brightPx > 100;
        rec(9, ok, QStringLiteral("avail=%1 rendered=%2 brightPx=%3")
                       .arg(extgen::isRendererAvailable(QStringLiteral("visualizer")) ? 1 : 0)
                       .arg(!rendered.isNull() ? 1 : 0).arg(brightPx));
    }

    // G10: Beat-Synced Auto Cut splits a video clip cleanly onto beat timestamps in 1 Undo step.
    {
        Timeline tl;
        ClipInfo c; c.filePath = mediaA; c.displayName = QStringLiteral("LongClip"); c.duration = 6.0; c.inPoint = 0.0; c.outPoint = 6.0;
        tl.videoTracks()[0]->addClip(c);
        tl.saveUndoState(QStringLiteral("Base"));

        const QVector<double> beats = {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 5.5};
        // Cut every 2 beats -> cuts at 0.5, 1.5, 2.5, 3.5, 4.5, 5.5 (6 cuts -> 7 pieces)
        const auto res = creatortools::autoCutClipToBeats(&tl, 0, 0, beats, /*everyNBeats=*/2);
        const int piecesAfter = tl.videoTracks()[0]->clips().size();
        double totalDur = 0.0;
        for (const ClipInfo &pc : tl.videoTracks()[0]->clips())
            totalDur += pc.effectiveDuration();

        tl.undo();
        const int piecesUndo = tl.videoTracks()[0]->clips().size();

        const bool ok = res.ok && res.cutsApplied == 6 && piecesAfter == 7
            && std::abs(totalDur - 6.0) < 1e-6
            && piecesUndo == 1;
        rec(10, ok, QStringLiteral("cuts=%1 pieces=%2 totalDur=%3 undoPieces=%4")
                        .arg(res.cutsApplied).arg(piecesAfter)
                        .arg(totalDur, 0, 'f', 2).arg(piecesUndo));
    }

    restoreRoles();
    return tally.finish("competitor-gap");
}
