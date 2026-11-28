// Wave 28 P2 competitor-gap features selftest (8 gates):
//   G1: Lottie JSON parse + animated keyframe SSOT TimelineFrameRenderer diff & alpha
//   G2: Lottie insertLottieClip + 1-step Undo + LottieImportDialog
//   G3: PitchShift WSOLA DSP duration invariance + DFT frequency shift (+12st / -12st) + Voice Presets
//   G4: PitchShift WAV bounce + applyPitchShiftToAudioClip + 1-step Undo + PitchShiftDialog + fast-path
//   G5: MOGRT built-in templates + custom controls + SSOT animated TimelineFrameRenderer render
//   G6: MOGRT PKZIP .mogrt archive round-trip (PK\x03\x04 + CRC-32) + 1-step Undo + MogrtBrowserDialog
//   G7: AAF binary OLE2/CFB v3 export (D0 CF 11 E0 magic) + readAafFile + applyAafToTimeline + 1-step Undo
//   G8: ExternalGenerator built-in availability (lottie/mogrt) + AafExchangeDialog + clean restoration

#include "../AafExchange.h"
#include "../ClipRoles.h"
#include "../ExternalGenerator.h"
#include "../LottiePlayer.h"
#include "../MogrtTemplate.h"
#include "../PitchShifter.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "SelftestGate.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <cmath>
#include <cstring>

namespace {

constexpr double kPi = 3.14159265358979323846;

double meanAbsPixelDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return 1e9;
    const QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage ib = b.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0.0;
    const int w = ia.width();
    const int h = ia.height();
    for (int y = 0; y < h; ++y) {
        const uchar *la = ia.constScanLine(y);
        const uchar *lb = ib.constScanLine(y);
        for (int x = 0; x < w * 4; ++x)
            sum += std::abs(int(la[x]) - int(lb[x]));
    }
    return sum / double(w * h * 4);
}

bool writeSineWav16(const QString &path, int sampleRate, double durationSec, double freqHz)
{
    const int frames = qMax(1, static_cast<int>(std::lround(sampleRate * durationSec)));
    const quint32 dataBytes = static_cast<quint32>(frames * 2);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    auto w16 = [&](quint16 v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    auto w32 = [&](quint32 v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    f.write("RIFF", 4); w32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(1); w32(static_cast<quint32>(sampleRate));
    w32(static_cast<quint32>(sampleRate * 2)); w16(2); w16(16);
    f.write("data", 4); w32(dataBytes);
    QByteArray pcm(static_cast<int>(dataBytes), '\0');
    auto *dst = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < frames; ++i)
        dst[i] = static_cast<qint16>(std::lround(std::sin(2.0 * kPi * freqHz * i / sampleRate) * 24000.0));
    return f.write(pcm) == pcm.size();
}

QByteArray makeSampleLottieJson()
{
    return QByteArray(R"json({
      "v": "5.7.4",
      "nm": "BounceBadge",
      "fr": 30,
      "ip": 0,
      "op": 60,
      "w": 200,
      "h": 200,
      "layers": [
        {
          "ind": 1,
          "ty": 4,
          "nm": "MovingCircle",
          "ip": 0,
          "op": 60,
          "ks": {
            "o": { "a": 0, "k": 100 },
            "r": { "a": 0, "k": 0 },
            "p": {
              "a": 1,
              "k": [
                { "t": 0,  "s": [40, 100], "e": [160, 100], "o": {"x": [0.33], "y": [0.0]}, "i": {"x": [0.67], "y": [1.0]} },
                { "t": 60, "s": [160, 100] }
              ]
            },
            "a": { "a": 0, "k": [0, 0] },
            "s": { "a": 0, "k": [100, 100] }
          },
          "shapes": [
            {
              "ty": "gr",
              "nm": "OrbGroup",
              "it": [
                { "ty": "el", "p": { "a": 0, "k": [0, 0] }, "s": { "a": 0, "k": [60, 60] } },
                { "ty": "fl", "c": { "a": 0, "k": [0.95, 0.25, 0.20] }, "o": { "a": 0, "k": 100 } },
                { "ty": "tr", "p": { "a": 0, "k": [0, 0] }, "a": { "a": 0, "k": [0, 0] }, "s": { "a": 0, "k": [100, 100] }, "r": { "a": 0, "k": 0 }, "o": { "a": 0, "k": 100 } }
              ]
            }
          ]
        }
      ]
    })json");
}

} // namespace

int runCompetitorGapP2Selftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        rec(1, false, QStringLiteral("QTemporaryDir unavailable"));
        return tally.finish("competitor-gap-p2");
    }

    lottie::clearDocumentCache();
    pitchshift::clearAllConfigsForTest();

    const QString lottiePath = tmpDir.filePath(QStringLiteral("bounce_badge.json"));
    {
        QFile f(lottiePath);
        if (f.open(QIODevice::WriteOnly))
            f.write(makeSampleLottieJson());
    }

    // ------------------------------------------------------------------
    // G1: Lottie JSON parse + animated keyframe SSOT render & alpha
    // ------------------------------------------------------------------
    {
        const lottie::LottieDocument doc = lottie::loadLottieFile(lottiePath);
        const QImage fEarly = lottie::renderLottieFrame(doc, 0.05, QSize(160, 160));
        const QImage fLate = lottie::renderLottieFrame(doc, 1.80, QSize(160, 160));
        const double directDiff = meanAbsPixelDiff(fEarly, fLate);
        const bool cornerTransparent = (fEarly.pixelColor(2, 2).alpha() == 0);

        // SSOT via TimelineFrameRenderer::renderFrameAt
        Timeline tl;
        QString err;
        const bool ins = lottie::insertLottieClip(&tl, lottiePath, 0, &err);
        const QImage ssotA = tlrender::renderFrameAt(&tl, 100'000, QSize(160, 160));
        const QImage ssotB = tlrender::renderFrameAt(&tl, 1'600'000, QSize(160, 160));
        const double ssotDiff = meanAbsPixelDiff(ssotA, ssotB);

        const bool ok = doc.valid && std::abs(doc.durationSec() - 2.0) < 1e-3
            && cornerTransparent && directDiff > 2.0 && ins && ssotDiff > 2.0;
        rec(1, ok, QStringLiteral("valid=%1 dur=%2 alpha0=%3 dDiff=%4 ssotDiff=%5")
                       .arg(doc.valid ? 1 : 0).arg(doc.durationSec(), 0, 'f', 2)
                       .arg(cornerTransparent ? 1 : 0).arg(directDiff, 0, 'f', 2)
                       .arg(ssotDiff, 0, 'f', 2));
    }

    // ------------------------------------------------------------------
    // G2: Lottie insertLottieClip + 1-step Undo + LottieImportDialog
    // ------------------------------------------------------------------
    {
        Timeline tl;
        QString err;
        const bool inserted = lottie::insertLottieClip(&tl, lottiePath, 0, &err);
        const int countAfter = tl.videoTracks()[0]->clips().size();
        const bool uriOk = (countAfter == 1)
            && extgen::isExternalGeneratorFilePath(tl.videoTracks()[0]->clips()[0].filePath);

        lottie::LottieImportDialog dlg(&tl, nullptr, lottiePath);
        const bool dlgLoaded = dlg.isLoaded() && (dlg.selectedFilePath() == lottiePath);

        tl.undo();
        const int countUndo = tl.videoTracks()[0]->clips().size();

        const bool ok = inserted && uriOk && dlgLoaded && (countUndo == 0);
        rec(2, ok, QStringLiteral("ins=%1 uri=%2 dlg=%3 undoCnt=%4")
                       .arg(inserted ? 1 : 0).arg(uriOk ? 1 : 0)
                       .arg(dlgLoaded ? 1 : 0).arg(countUndo));
    }

    // ------------------------------------------------------------------
    // G3: PitchShift WSOLA DSP duration invariance + DFT freq shift + Presets
    // ------------------------------------------------------------------
    {
        const int sr = 48000;
        const int N = sr / 2; // 0.5s
        QVector<float> tone440(N);
        for (int i = 0; i < N; ++i)
            tone440[i] = static_cast<float>(0.6 * std::sin(2.0 * kPi * 440.0 * i / sr));

        const double fIn = pitchshift::estimateDominantFrequencyHz(tone440, sr);

        pitchshift::PitchShiftParams upOct;
        upOct.enabled = true;
        upOct.semitones = 12.0; // +1 octave -> ~880 Hz
        const QVector<float> outUp = pitchshift::processMonoBuffer(tone440, sr, upOct);
        const double fUp = pitchshift::estimateDominantFrequencyHz(outUp, sr);

        pitchshift::PitchShiftParams downOct;
        downOct.enabled = true;
        downOct.semitones = -12.0; // -1 octave -> ~220 Hz
        const QVector<float> outDown = pitchshift::processMonoBuffer(tone440, sr, downOct);
        const double fDown = pitchshift::estimateDominantFrequencyHz(outDown, sr);

        const QVector<float> outRobot = pitchshift::processMonoBuffer(
            tone440, sr, pitchshift::paramsForPreset(pitchshift::VoicePreset::Robot));
        const QVector<float> outRadio = pitchshift::processMonoBuffer(
            tone440, sr, pitchshift::paramsForPreset(pitchshift::VoicePreset::TelephoneRadio));

        const bool durInvariant = (outUp.size() == N) && (outDown.size() == N)
            && (outRobot.size() == N) && (outRadio.size() == N);
        const bool freqUpOk = (fUp > 820.0 && fUp < 940.0);
        const bool freqDownOk = (fDown > 180.0 && fDown < 260.0);

        rec(3, durInvariant && freqUpOk && freqDownOk,
            QStringLiteral("fIn=%1 fUp=%2 fDown=%3 lenEq=%4")
                .arg(fIn, 0, 'f', 1).arg(fUp, 0, 'f', 1).arg(fDown, 0, 'f', 1)
                .arg(durInvariant ? 1 : 0));
    }

    // ------------------------------------------------------------------
    // G4: PitchShift WAV bounce + applyPitchShiftToAudioClip + Undo + Dialog
    // ------------------------------------------------------------------
    {
        const QString inWav = tmpDir.filePath(QStringLiteral("voice_in.wav"));
        const QString outWav = tmpDir.filePath(QStringLiteral("voice_chipmunk.wav"));
        const bool wroteWav = writeSineWav16(inWav, 48000, 0.4, 440.0);

        Timeline tl;
        ClipInfo aClip;
        aClip.uid = QStringLiteral("w28-audio-uid");
        aClip.filePath = inWav;
        aClip.displayName = QStringLiteral("VoiceClip");
        aClip.duration = 0.4;
        aClip.inPoint = 0.0;
        aClip.outPoint = 0.4;
        tl.audioTracks()[0]->addClip(aClip);
        tl.saveUndoState(QStringLiteral("Initial Audio"));

        const auto chip = pitchshift::paramsForPreset(pitchshift::VoicePreset::Chipmunk);
        QString err;
        const bool applied = pitchshift::applyPitchShiftToAudioClip(&tl, 0, 0, chip, outWav, &err);
        const bool pathSwapped = applied
            && (tl.audioTracks()[0]->clips()[0].filePath == outWav)
            && QFileInfo::exists(outWav)
            && (QFileInfo(outWav).size() == QFileInfo(inWav).size());

        pitchshift::PitchShiftDialog dlg(aClip.uid, aClip.displayName);
        const bool dlgMatches = dlg.currentParams().enabled
            && std::abs(dlg.currentParams().semitones - 6.0) < 1e-3
            && pitchshift::hasAnyActiveConfig();

        tl.undo();
        const bool undoRestored = (tl.audioTracks()[0]->clips()[0].filePath == inWav);

        pitchshift::clearAllConfigsForTest();
        const bool fastPathClean = !pitchshift::hasAnyActiveConfig();

        rec(4, wroteWav && pathSwapped && dlgMatches && undoRestored && fastPathClean,
            QStringLiteral("wav=%1 swap=%2 dlg=%3 undo=%4 clean=%5")
                .arg(wroteWav ? 1 : 0).arg(pathSwapped ? 1 : 0).arg(dlgMatches ? 1 : 0)
                .arg(undoRestored ? 1 : 0).arg(fastPathClean ? 1 : 0));
    }

    // ------------------------------------------------------------------
    // G5: MOGRT built-in templates + custom controls + SSOT animated render
    // ------------------------------------------------------------------
    {
        const auto gallery = mogrt::builtInTemplates();
        mogrt::MogrtTemplate tpl = mogrt::templateById(QStringLiteral("builtin-lower-third"));
        tpl.setTextParam(QStringLiteral("title"), QStringLiteral("Wave 28 Live Title"));
        tpl.setColorParam(QStringLiteral("accent"), QColor(16, 185, 129));

        Timeline tl;
        QString err;
        const bool inserted = mogrt::insertMogrtClip(&tl, tpl, 0, &err);
        const QImage early = tlrender::renderFrameAt(&tl, 80'000, QSize(320, 180));
        const QImage mid = tlrender::renderFrameAt(&tl, 1'200'000, QSize(320, 180));
        const double animDiff = meanAbsPixelDiff(early, mid);
        const bool cornerAlphaZero = (mid.pixelColor(2, 2).alpha() == 0);

        rec(5, (gallery.size() >= 5) && inserted && cornerAlphaZero && animDiff > 1.0,
            QStringLiteral("gallery=%1 ins=%2 alpha0=%3 animDiff=%4")
                .arg(gallery.size()).arg(inserted ? 1 : 0)
                .arg(cornerAlphaZero ? 1 : 0).arg(animDiff, 0, 'f', 2));
    }

    // ------------------------------------------------------------------
    // G6: MOGRT PKZIP .mogrt archive export/import round-trip + Undo + Dialog
    // ------------------------------------------------------------------
    {
        mogrt::MogrtTemplate src = mogrt::templateById(QStringLiteral("builtin-countdown"));
        src.setNumberParam(QStringLiteral("startSec"), 45.0);
        src.setTextParam(QStringLiteral("label"), QStringLiteral("GO LIVE"));

        const QString mogrtPath = tmpDir.filePath(QStringLiteral("custom_countdown.mogrt"));
        QString err;
        const bool exported = mogrt::exportMogrtArchive(src, mogrtPath, &err);

        QFile rawF(mogrtPath);
        bool pkMagic = false;
        if (rawF.open(QIODevice::ReadOnly)) {
            const QByteArray head = rawF.read(4);
            pkMagic = (head.size() == 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4);
        }

        bool importOk = false;
        const mogrt::MogrtTemplate loaded = mogrt::importMogrtArchive(mogrtPath, &importOk, &err);
        const bool roundTripMatches = importOk
            && loaded.id == src.id
            && std::abs(loaded.numberParam(QStringLiteral("startSec")) - 45.0) < 1e-6
            && loaded.textParam(QStringLiteral("label")) == QStringLiteral("GO LIVE");

        Timeline tl;
        const bool ins = mogrt::insertMogrtClip(&tl, loaded, 0, &err);
        tl.undo();
        const bool undoOk = tl.videoTracks()[0]->clips().isEmpty();

        mogrt::MogrtBrowserDialog dlg(&tl);
        const bool dlgOk = (dlg.templateCount() >= 5);

        rec(6, exported && pkMagic && roundTripMatches && ins && undoOk && dlgOk,
            QStringLiteral("exp=%1 pk=%2 rt=%3 undo=%4 dlgCnt=%5")
                .arg(exported ? 1 : 0).arg(pkMagic ? 1 : 0).arg(roundTripMatches ? 1 : 0)
                .arg(undoOk ? 1 : 0).arg(dlg.templateCount()));
    }

    // ------------------------------------------------------------------
    // G7: AAF OLE2/CFB v3 binary export + import + applyAafToTimeline + Undo
    // ------------------------------------------------------------------
    {
        Timeline srcTl;
        srcTl.addVideoTrack(); // V1, V2
        ClipInfo v1;
        v1.uid = QStringLiteral("aaf-v1");
        v1.filePath = QStringLiteral("C:/media/camA.mov");
        v1.displayName = QStringLiteral("Cam A");
        v1.duration = 12.0;
        v1.inPoint = 1.5;
        v1.outPoint = 5.5;
        v1.leadInSec = 0.5;
        srcTl.videoTracks()[0]->addClip(v1);

        ClipInfo a1;
        a1.uid = QStringLiteral("aaf-a1");
        a1.filePath = QStringLiteral("C:/media/dialog.wav");
        a1.displayName = QStringLiteral("Dialogue");
        a1.duration = 10.0;
        a1.inPoint = 0.25;
        a1.outPoint = 4.25;
        a1.volume = 0.75;
        srcTl.audioTracks()[0]->addClip(a1);
        cliproles::setRoleForClip(a1.uid, cliproles::Role::Music);
        srcTl.addMarker(2'500'000, QStringLiteral("Sync Point"), QColor(QStringLiteral("#22c55e")));

        const QString aafPath = tmpDir.filePath(QStringLiteral("reel_01.aaf"));
        aaf::AafExportOptions opts;
        opts.fps = 24.0;
        opts.projectName = QStringLiteral("Reel01");

        QString err;
        const bool wrote = aaf::writeAafFile(&srcTl, opts, aafPath, &err);

        // Verify exact OLE2/CFB v3 binary magic bytes (D0 CF 11 E0 A1 B1 1A E1) and 512-byte sector alignment
        bool ole2Valid = false;
        QFile f(aafPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = f.readAll();
            const uchar expected[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
            ole2Valid = (bytes.size() >= 1024)
                && ((bytes.size() % 512) == 0)
                && (std::memcmp(bytes.constData(), expected, 8) == 0);
        }

        const aaf::AafImportResult parsed = aaf::readAafFile(aafPath);

        Timeline dstTl;
        dstTl.saveUndoState(QStringLiteral("Empty Base"));
        const bool applied = aaf::applyAafToTimeline(&dstTl, parsed, &err);

        const bool videoMatch = !dstTl.videoTracks().isEmpty()
            && dstTl.videoTracks()[0]->clips().size() == 1
            && dstTl.videoTracks()[0]->clips()[0].filePath == v1.filePath
            && std::abs(dstTl.videoTracks()[0]->clips()[0].inPoint - 1.5) < 1e-6
            && std::abs(dstTl.videoTracks()[0]->clips()[0].outPoint - 5.5) < 1e-6
            && std::abs(dstTl.videoTracks()[0]->clips()[0].leadInSec - 0.5) < 1e-6;
        const bool audioMatch = !dstTl.audioTracks().isEmpty()
            && dstTl.audioTracks()[0]->clips().size() == 1
            && std::abs(dstTl.audioTracks()[0]->clips()[0].volume - 0.75) < 1e-6
            && cliproles::roleForClip(dstTl.audioTracks()[0]->clips()[0].uid) == cliproles::Role::Music;
        const bool markerMatch = (dstTl.markers().size() == 1)
            && (dstTl.markers()[0].timelineUs == 2'500'000)
            && (dstTl.markers()[0].label == QStringLiteral("Sync Point"));

        const QString importedAudioUid = audioMatch ? dstTl.audioTracks()[0]->clips()[0].uid : QString();
        dstTl.undo();
        const bool undoOk = dstTl.videoTracks()[0]->clips().isEmpty()
            && dstTl.audioTracks()[0]->clips().isEmpty();

        // Clean up test role assignments
        cliproles::setRoleForClip(a1.uid, cliproles::Role::None);
        if (!importedAudioUid.isEmpty())
            cliproles::setRoleForClip(importedAudioUid, cliproles::Role::None);

        rec(7, wrote && ole2Valid && parsed.ok && applied && videoMatch && audioMatch && markerMatch && undoOk,
            QStringLiteral("wrote=%1 ole2=%2 parsed=%3 v=%4 a=%5 mk=%6 undo=%7")
                .arg(wrote ? 1 : 0).arg(ole2Valid ? 1 : 0).arg(parsed.ok ? 1 : 0)
                .arg(videoMatch ? 1 : 0).arg(audioMatch ? 1 : 0)
                .arg(markerMatch ? 1 : 0).arg(undoOk ? 1 : 0));
    }

    // ------------------------------------------------------------------
    // G8: ExternalGenerator built-in availability + AafExchangeDialog
    // ------------------------------------------------------------------
    {
        const bool lottieAvail = extgen::isRendererAvailable(QStringLiteral("lottie"));
        const bool mogrtAvail = extgen::isRendererAvailable(QStringLiteral("mogrt"));

        aaf::AafExportOptions def;
        def.fps = 29.97;
        def.projectName = QStringLiteral("ShowReel");
        aaf::AafExchangeDialog dlg(def);
        const auto readBack = dlg.options();
        const bool dlgOk = (readBack.projectName == QStringLiteral("ShowReel"))
            && (std::abs(readBack.fps - 29.97) < 1e-2)
            && readBack.includeVideo && readBack.includeAudio && readBack.includeMarkers;

        lottie::clearDocumentCache();
        pitchshift::clearAllConfigsForTest();

        rec(8, lottieAvail && mogrtAvail && dlgOk,
            QStringLiteral("lottie=%1 mogrt=%2 aafDlg=%3")
                .arg(lottieAvail ? 1 : 0).arg(mogrtAvail ? 1 : 0).arg(dlgOk ? 1 : 0));
    }

    return tally.finish("competitor-gap-p2");
}
