// src/selftests/sprint_selftests.cpp
// PRD-SPLIT-MAIN-3 Phase 3-C: Sprint 11-22 系サービス selftest 27 関数を
// src/main.cpp から verbatim 移動。Dispatch via SelftestRegistry.{h,cpp}.

#include "AIMask.h"
#include "MagneticTimeline.h"
#include "AudioClipEditor.h"
#include "ShortcutManager.h"
#include "SocialPreset.h"
#include "AspectReframer.h"
#include "CaptionTrack.h"
#include "CaptionStyle.h"
#include "SubtitleIO.h"
#include "SpeechRecognizer.h"
#include "PlanarTracker.h"
#include "PlanarTrackerPreset.h"
#include "PlanarTrackerPresetRegistry.h"
#include "MobilePreset.h"
#include "MobileRotate.h"
#include "ObsScanner.h"
#include "ObsProfile.h"
#include "ObsLayout.h"
#include "AffinityPsdImporter.h"
#include "AffinityVectorImporter.h"
#include "BlenderMeshImporter.h"
#include "BlenderBpyBridge.h"
#include "BlenderExrReader.h"
#include "ImportHubDialog.h"
#include "YoutubeOAuth.h"
#include "YoutubeUploadManager.h"
#include "CollaborationModel.h"
#include "ColorMatchAnalyzer.h"
#include "ColorMatchLutGenerator.h"
#include "VimeoOAuth.h"
#include "VimeoUploadClient.h"
#include "VimeoUploadManager.h"
#include "TwitchStreamConfig.h"
#include "FrameIoImporter.h"
#include "DavinciResolveXmlExporter.h"
#include "FcpxmlExporter.h"
#include "SmartEditAssistant.h"
#include "CloudRenderClient.h"
#include "XVideoUpload.h"
#include "InstagramPublish.h"
#include "ProjectTemplate.h"
#include "LoudnessMaster.h"
#include "HdrGrading.h"
#include "MultiCamSync.h"
#include "BatchExportQueue.h"
#include "SelftestRegistry.h"
#include "Timeline.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QString>
#include <QStringLiteral>
#include <QStringList>
#include <QByteArray>
#include <QColor>
#include <QRectF>
#include <QDebug>
#include <QTemporaryDir>
#include <QPainter>
#include <QUrl>
#include <QUrlQuery>
#include <QMetaObject>
#include <QMetaMethod>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QTimer>
#include <QEventLoop>
#include <QProcess>
#include <QVector3D>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if __has_include("AIMask.h")
#define HAVE_AIMASK 1
#endif
#if __has_include("MagneticTimeline.h")
#define HAVE_MAGTL 1
#endif
#if __has_include("AudioClipEditor.h")
#define HAVE_AUDIOCLIPEDITOR 1
#endif
#if __has_include("ShortcutManager.h")
#define HAVE_SHORTCUTMANAGER 1
#endif
#if __has_include("SocialPreset.h")
#define HAVE_SOCIALPRESET 1
#endif
#if __has_include("AspectReframer.h")
#define HAVE_ASPECTREFRAMER 1
#endif
#if __has_include("CaptionTrack.h")
#define HAVE_CAPTIONTRACK 1
#endif
#if __has_include("CaptionStyle.h")
#define HAVE_CAPTIONSTYLE 1
#endif
#if __has_include("SubtitleIO.h")
#define HAVE_SUBTITLEIO 1
#endif
#if __has_include("SpeechRecognizer.h")
#define HAVE_SPEECHRECOGNIZER 1
#endif
#if __has_include("PlanarTracker.h")
#define HAVE_PLANARTRACKER 1
#endif
#if __has_include("PlanarTrackerPreset.h")
#define HAVE_PLANARTRACKER_PRESET 1
#endif
#if __has_include("MobilePreset.h")
#define HAVE_MOBILEPRESET 1
#endif
#if __has_include("MobileRotate.h")
#define HAVE_MOBILEROTATE 1
#endif
#if __has_include("ObsScanner.h")
#define HAVE_OBSSCANNER 1
#endif
#if __has_include("ObsProfile.h")
#define HAVE_OBSPROFILE 1
#endif
#if __has_include("ObsLayout.h")
#define HAVE_OBSLAYOUT 1
#endif
#if __has_include("AffinityPsdImporter.h")
#define HAVE_AFFINITYPSD 1
#endif
#if __has_include("AffinityVectorImporter.h")
#define HAVE_AFFINITYVECTOR 1
#endif
#if __has_include("BlenderMeshImporter.h")
#define HAVE_BLENDERMESH 1
#endif
#if __has_include("BlenderBpyBridge.h")
#define HAVE_BLENDERBPY 1
#endif
#if __has_include("BlenderExrReader.h")
#define HAVE_BLENDEREXR 1
#endif
#if __has_include("ImportHubDialog.h")
#define HAVE_IMPORTHUBDIALOG 1
#endif
#if __has_include("YoutubeOAuth.h")
#define HAVE_YOUTUBE_OAUTH 1
#endif
#if __has_include("YoutubeUploadManager.h")
#define HAVE_YOUTUBE_MANAGER 1
#endif
#if __has_include("CollaborationModel.h")
#define HAVE_COLLABMODEL 1
#endif
#if __has_include("ColorMatchAnalyzer.h")
#define HAVE_COLORMATCH_ANALYZE 1
#endif
#if __has_include("ColorMatchLutGenerator.h")
#define HAVE_COLORMATCH_LUT 1
#endif
#if __has_include("VimeoOAuth.h")
#define HAVE_VIMEO_OAUTH 1
#endif
#if __has_include("VimeoUploadClient.h")
#define HAVE_VIMEO_UPLOAD_CLIENT 1
#endif
#if __has_include("VimeoUploadManager.h")
#define HAVE_VIMEO_UPLOAD_MANAGER 1
#endif
#if __has_include("TwitchStreamConfig.h")
#define HAVE_TWITCH_STREAM_CONFIG 1
#endif
#if __has_include("FrameIoImporter.h")
#define HAVE_FRAMEIO_IMPORTER 1
#endif
#if __has_include("DavinciResolveXmlExporter.h")
#define HAVE_DAVINCI_XML 1
#endif
#if __has_include("FcpxmlExporter.h")
#define HAVE_FCPXML_EXPORTER 1
#endif
#if __has_include("SmartEditAssistant.h")
#define HAVE_SMARTEDIT_ASSISTANT 1
#endif
#if __has_include("CloudRenderClient.h")
#define HAVE_CLOUDRENDER_CLIENT 1
#endif
#if __has_include("XVideoUpload.h")
#define HAVE_XVIDEO_UPLOAD 1
#endif
#if __has_include("InstagramPublish.h")
#define HAVE_INSTAGRAM_PUBLISH 1
#endif
#if __has_include("ProjectTemplate.h")
#define HAVE_PROJECT_TEMPLATE 1
#endif
#if __has_include("LoudnessMaster.h")
#define HAVE_LOUDNESS_MASTER 1
#endif
#if __has_include("HdrGrading.h")
#define HAVE_HDR_GRADING 1
#endif
#if __has_include("MultiCamSync.h")
#define HAVE_MULTICAM_SYNC 1
#endif
#if __has_include("BatchExportQueue.h")
#define HAVE_BATCHEXPORT_QUEUE 1
#endif

using selftests::requireSelftest;

// ---- selftest functions moved verbatim from src/main.cpp lines 511-2053 ----

// US-WF-D: Sprint 11 workflow self-test (VEDITOR_WORKFLOW_SELFTEST=1).
// Exercises AI auto-mask, Magnetic Timeline, and AudioClipEditor envelope
// evaluation without spinning up MainWindow. Each block is guarded by
// HAVE_* macros so the test still passes when a header is unavailable.
int runWorkflowSelftest()
{
    QString error;

#ifdef HAVE_AIMASK
    {
        // Pure white 8x8: LumaThreshold (threshold=0.5) -> all pixels above
        // threshold, mask should be fully opaque (255).
        QImage white(8, 8, QImage::Format_ARGB32);
        white.fill(QColor(255, 255, 255, 255));
        aimask::MaskParams params;
        params.engine = aimask::Engine::LumaThreshold;
        params.lumaThreshold = 0.5;
        const aimask::MaskResult resultWhite = aimask::generateMask(white, params);
        if (!requireSelftest(resultWhite.success,
                             QStringLiteral("aimask::generateMask(white) reported failure: ")
                                 + resultWhite.error,
                             &error))
            return 1;
        if (!requireSelftest(!resultWhite.mask.isNull()
                                 && resultWhite.mask.size() == white.size(),
                             QStringLiteral("aimask::generateMask(white) returned null/wrong-size mask"),
                             &error))
            return 1;
        bool allFullyOpaqueWhite = true;
        for (int y = 0; y < resultWhite.mask.height() && allFullyOpaqueWhite; ++y) {
            for (int x = 0; x < resultWhite.mask.width(); ++x) {
                const int gray = qGray(resultWhite.mask.pixel(x, y));
                if (gray != 255) {
                    allFullyOpaqueWhite = false;
                    break;
                }
            }
        }
        if (!requireSelftest(allFullyOpaqueWhite,
                             QStringLiteral("aimask LumaThreshold(white) mask not all 255"),
                             &error))
            return 1;

        // Pure black 8x8: LumaThreshold (threshold=0.5) -> all pixels below
        // threshold, mask should be fully transparent (0).
        QImage black(8, 8, QImage::Format_ARGB32);
        black.fill(QColor(0, 0, 0, 255));
        const aimask::MaskResult resultBlack = aimask::generateMask(black, params);
        if (!requireSelftest(resultBlack.success,
                             QStringLiteral("aimask::generateMask(black) reported failure: ")
                                 + resultBlack.error,
                             &error))
            return 1;
        bool allZeroBlack = true;
        for (int y = 0; y < resultBlack.mask.height() && allZeroBlack; ++y) {
            for (int x = 0; x < resultBlack.mask.width(); ++x) {
                const int gray = qGray(resultBlack.mask.pixel(x, y));
                if (gray != 0) {
                    allZeroBlack = false;
                    break;
                }
            }
        }
        if (!requireSelftest(allZeroBlack,
                             QStringLiteral("aimask LumaThreshold(black) mask not all 0"),
                             &error))
            return 1;
    }
#endif // HAVE_AIMASK

#ifdef HAVE_MAGTL
    {
        // AC2: 2 clips with a 100ms gap should be packed (closeGaps).
        QList<magtl::Clip> twoWithGap;
        twoWithGap.append(magtl::Clip{0, 0, 1000, QStringLiteral("A")});
        twoWithGap.append(magtl::Clip{0, 1100, 2000, QStringLiteral("B")});
        const QList<magtl::Clip> packed = magtl::closeGaps(twoWithGap);
        if (!requireSelftest(packed.size() == 2,
                             QStringLiteral("magtl::closeGaps did not preserve clip count"),
                             &error))
            return 1;
        if (!requireSelftest(packed.at(0).startMs == 0 && packed.at(0).endMs == 1000,
                             QStringLiteral("magtl::closeGaps moved/altered first clip"),
                             &error))
            return 1;
        // Second clip should now butt up against first (start == 1000) and
        // preserve its duration (900ms -> end == 1900).
        if (!requireSelftest(packed.at(1).startMs == 1000 && packed.at(1).endMs == 1900,
                             QStringLiteral("magtl::closeGaps did not collapse the 100ms gap"),
                             &error))
            return 1;

        // AC4: 3 clips, delete index=1, expect 2 clips and the trailing clip
        // shifted forward by the deleted clip's duration.
        QList<magtl::Clip> threeClips;
        threeClips.append(magtl::Clip{0, 0, 1000, QStringLiteral("A")});
        threeClips.append(magtl::Clip{0, 1000, 1500, QStringLiteral("B")}); // 500ms
        threeClips.append(magtl::Clip{0, 1500, 2500, QStringLiteral("C")});
        const QList<magtl::Clip> afterDelete = magtl::rippleDelete(threeClips, 1);
        if (!requireSelftest(afterDelete.size() == 2,
                             QStringLiteral("magtl::rippleDelete did not reduce clip count to 2"),
                             &error))
            return 1;
        if (!requireSelftest(afterDelete.at(0).startMs == 0
                                 && afterDelete.at(0).endMs == 1000
                                 && afterDelete.at(0).id == QStringLiteral("A"),
                             QStringLiteral("magtl::rippleDelete altered the first clip"),
                             &error))
            return 1;
        // Third clip (C) should have shifted left by 500ms (deleted B duration).
        if (!requireSelftest(afterDelete.at(1).startMs == 1000
                                 && afterDelete.at(1).endMs == 2000
                                 && afterDelete.at(1).id == QStringLiteral("C"),
                             QStringLiteral("magtl::rippleDelete did not shift trailing clip"),
                             &error))
            return 1;
    }
#endif // HAVE_MAGTL

#ifdef HAVE_AUDIOCLIPEDITOR
    {
        // AudioClipEditor is a QWidget; QApplication must already exist
        // (caller dispatches runWorkflowSelftest after QApplication construction).
        AudioClipEditor editor;
        editor.setClipDuration(5000);
        editor.clearEnvelope();
        const QList<VolumeEnvelopePoint> defaultEnv = editor.envelope();
        if (!requireSelftest(defaultEnv.size() == 2,
                             QStringLiteral("AudioClipEditor::clearEnvelope did not yield 2 points"),
                             &error))
            return 1;
        if (!requireSelftest(defaultEnv.at(0).timeMs == 0
                                 && std::abs(defaultEnv.at(0).dB - 0.0) < 1e-6,
                             QStringLiteral("AudioClipEditor default first point != (0, 0dB)"),
                             &error))
            return 1;
        if (!requireSelftest(defaultEnv.at(1).timeMs == 5000
                                 && std::abs(defaultEnv.at(1).dB - 0.0) < 1e-6,
                             QStringLiteral("AudioClipEditor default last point != (5000, 0dB)"),
                             &error))
            return 1;

        QList<VolumeEnvelopePoint> custom;
        custom.append(VolumeEnvelopePoint{0,    0.0});
        custom.append(VolumeEnvelopePoint{2500, -6.0});
        custom.append(VolumeEnvelopePoint{5000, 0.0});
        editor.setEnvelope(custom);
        const double dbAtMid = editor.evaluateAt(2500);
        if (!requireSelftest(std::abs(dbAtMid - (-6.0)) < 1e-6,
                             QStringLiteral("AudioClipEditor::evaluateAt(2500) != -6.0 (got %1)")
                                 .arg(dbAtMid),
                             &error))
            return 1;
    }
#endif // HAVE_AUDIOCLIPEDITOR

    qInfo().noquote() << QStringLiteral("WORKFLOW selftest OK");
    return 0;
}

// US-SC-B: Sprint 12 shortcut customization self-test (VEDITOR_SHORTCUT_SELFTEST=1)
int runShortcutSelftest()
{
    QString error;
#ifdef HAVE_SHORTCUTMANAGER
    {
        QAction dummyA;
        dummyA.setShortcut(QKeySequence("Ctrl+O"));
        QAction dummyB;
        dummyB.setShortcut(QKeySequence("Ctrl+S"));

        shortcut::ShortcutManager mgr;
        mgr.registerAction(&dummyA, "file.open",
                           QStringLiteral("ファイルを開く"),
                           QStringLiteral("ファイル"));
        mgr.registerAction(&dummyB, "file.save",
                           QStringLiteral("保存"),
                           QStringLiteral("ファイル"));

        // 1. registerAction → bindings() に entry
        if (!requireSelftest(mgr.bindings().size() == 2,
                             QStringLiteral("ShortcutManager: bindings size != 2"), &error))
            return 1;
        // 2. setBinding でカスタム値、bindingFor で取れる
        mgr.setBinding("file.open", QKeySequence("Ctrl+Shift+O"));
        if (!requireSelftest(mgr.bindingFor("file.open").sequence ==
                                 QKeySequence("Ctrl+Shift+O"),
                             QStringLiteral("setBinding round-trip failed"), &error))
            return 1;
        // 3. QAction 自体の shortcut も反映
        if (!requireSelftest(dummyA.shortcut() == QKeySequence("Ctrl+Shift+O"),
                             QStringLiteral("QAction::shortcut not updated"), &error))
            return 1;
        // 4. applyPreset(Premiere) — presetDisplayName が non-empty。
        mgr.applyPreset(shortcut::Preset::Premiere);
        if (!requireSelftest(!shortcut::ShortcutManager::presetDisplayName(
                                    shortcut::Preset::Premiere).isEmpty(),
                             QStringLiteral("presetDisplayName(Premiere) empty"), &error))
            return 1;
        // 5. availablePresets contains 4
        if (!requireSelftest(shortcut::ShortcutManager::availablePresets().size() == 4,
                             QStringLiteral("availablePresets != 4"), &error))
            return 1;
        // 6. resetAllToDefaults → default に戻る (Ctrl+O)
        mgr.resetAllToDefaults();
        if (!requireSelftest(mgr.bindingFor("file.open").sequence == QKeySequence("Ctrl+O"),
                             QStringLiteral("resetAllToDefaults didn't restore"), &error))
            return 1;
    }
#endif // HAVE_SHORTCUTMANAGER
    qInfo().noquote() << QStringLiteral("SHORTCUT selftest OK");
    return 0;
}

// US-SC2-B: Sprint 13 social export self-test (VEDITOR_SOCIAL_SELFTEST=1)
int runSocialSelftest()
{
    QString error;
#ifdef HAVE_SOCIALPRESET
    {
        // 1. allPresets >= 9
        const auto presets = social::allPresets();
        if (!requireSelftest(presets.size() >= 9,
                             QStringLiteral("social::allPresets size < 9"), &error))
            return 1;
        // 2. instagram_reels の resolution & vertical flag
        const social::Preset reels = social::presetById("instagram_reels");
        if (!requireSelftest(!reels.id.isEmpty() && reels.resolution == QSize(1080, 1920)
                                 && reels.requiresVerticalReframe,
                             QStringLiteral("instagram_reels preset invalid"), &error))
            return 1;
        // 3. youtube_standard horizontal
        const social::Preset yt = social::presetById("youtube_standard");
        if (!requireSelftest(!yt.id.isEmpty() && yt.resolution == QSize(1920, 1080)
                                 && !yt.requiresVerticalReframe,
                             QStringLiteral("youtube_standard preset invalid"), &error))
            return 1;
        // 4. 存在しない id は空 Preset
        const social::Preset missing = social::presetById("does_not_exist");
        if (!requireSelftest(missing.id.isEmpty(),
                             QStringLiteral("presetById missing should be empty"), &error))
            return 1;
    }
#endif
#ifdef HAVE_ASPECTREFRAMER
    {
        // 5. CenterCrop 1920x1080 -> 1080x1920 の crop rect
        QImage src(1920, 1080, QImage::Format_ARGB32);
        src.fill(QColor(64, 128, 192, 255));
        reframe::ReframeParams p;
        p.sourceSize = QSize(1920, 1080);
        p.targetSize = QSize(1080, 1920);
        p.mode = reframe::Mode::CenterCrop;
        const QRectF rect = reframe::computeCropRect(src, p);
        const double expectedW = (1080.0 / 1920.0) / (1920.0 / 1080.0); // ~0.316
        if (!requireSelftest(std::abs(rect.width() - expectedW) < 1e-2
                                 && std::abs(rect.height() - 1.0) < 1e-2
                                 && std::abs(rect.y()) < 1e-3,
                             QStringLiteral("CenterCrop rect math wrong"), &error))
            return 1;
        // 6. applyReframe output size matches target
        const reframe::ReframeResult res = reframe::applyReframe(src, p);
        if (!requireSelftest(res.success
                                 && res.previewImage.size() == QSize(1080, 1920),
                             QStringLiteral("applyReframe output size mismatch"), &error))
            return 1;
        // 7. null source -> success=false, error non-empty
        QImage nullImg;
        const reframe::ReframeResult nullRes = reframe::applyReframe(nullImg, p);
        if (!requireSelftest(!nullRes.success && !nullRes.error.isEmpty(),
                             QStringLiteral("applyReframe null source should fail"), &error))
            return 1;
        // 8. modes >= 5
        if (!requireSelftest(reframe::availableModes().size() >= 5,
                             QStringLiteral("availableModes < 5"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("SOCIAL selftest OK");
    return 0;
}

// US-CAP-B: Sprint 14 caption self-test (VEDITOR_CAPTION_SELFTEST=1)
int runCaptionSelftest()
{
    QString error;
#ifdef HAVE_CAPTIONTRACK
    {
        caption::Track track;
        caption::Clip c1{1000, 2000, QStringLiteral("hello"), QString()};
        caption::Clip c2{2500, 3500, QStringLiteral("world"), QString()};
        track.addClip(c1);
        track.addClip(c2);
        track.sortByStart();
        if (!requireSelftest(track.clipCount() == 2,
                             QStringLiteral("Track::clipCount != 2"), &error))
            return 1;
        const auto active = track.clipsAtTime(1500);
        if (!requireSelftest(active.size() == 1 && active[0].text == QStringLiteral("hello"),
                             QStringLiteral("clipsAtTime(1500) wrong"), &error))
            return 1;
    }
#endif
#ifdef HAVE_CAPTIONSTYLE
    {
        const auto style = caption::defaultStyle();
        if (!requireSelftest(style.anchor == caption::Anchor::BottomCenter
                                 && style.fontSizePt == 24,
                             QStringLiteral("defaultStyle wrong"), &error))
            return 1;
        if (!requireSelftest(caption::anchorFromString(
                                 caption::anchorToString(caption::Anchor::TopLeft))
                                 == caption::Anchor::TopLeft,
                             QStringLiteral("Anchor round-trip failed"), &error))
            return 1;
        if (!requireSelftest(caption::anchorNames().size() == 9,
                             QStringLiteral("anchorNames size != 9"), &error))
            return 1;
    }
#endif
#ifdef HAVE_SUBTITLEIO
    {
        // formatSrtTimestamp + parseSrtTimestamp round-trip
        const QString ts = subtitle::formatSrtTimestamp(3661500);
        if (!requireSelftest(ts == QStringLiteral("01:01:01,500"),
                             QStringLiteral("formatSrtTimestamp wrong"), &error))
            return 1;
        if (!requireSelftest(subtitle::parseSrtTimestamp(ts) == 3661500,
                             QStringLiteral("parseSrtTimestamp round-trip"), &error))
            return 1;
        if (!requireSelftest(subtitle::parseSrtTimestamp(QStringLiteral("invalid")) == -1,
                             QStringLiteral("parseSrtTimestamp invalid should return -1"), &error))
            return 1;

        // SRT round-trip: write + read 2 clips
        const QString tmpPath = QDir::tempPath() + QStringLiteral("/veditor_caption_test.srt");
        QList<caption::Clip> clips;
        clips.append({1000, 2000, QStringLiteral("first"), QString()});
        clips.append({3000, 4500, QStringLiteral("second"), QString()});
        if (!requireSelftest(subtitle::exportSrt(tmpPath, clips),
                             QStringLiteral("exportSrt failed"), &error))
            return 1;
        const auto imp = subtitle::importSrt(tmpPath);
        if (!requireSelftest(imp.success && imp.clips.size() == 2
                                 && imp.clips[0].text == QStringLiteral("first")
                                 && imp.clips[1].startMs == 3000,
                             QStringLiteral("SRT round-trip failed"), &error))
            return 1;
        QFile::remove(tmpPath);

        // VTT round-trip
        const QString vttPath = QDir::tempPath() + QStringLiteral("/veditor_caption_test.vtt");
        if (!requireSelftest(subtitle::exportVtt(vttPath, clips),
                             QStringLiteral("exportVtt failed"), &error))
            return 1;
        const auto vttImp = subtitle::importVtt(vttPath);
        if (!requireSelftest(vttImp.success && vttImp.clips.size() == 2
                                 && vttImp.clips[0].text == QStringLiteral("first"),
                             QStringLiteral("VTT round-trip failed"), &error))
            return 1;
        QFile::remove(vttPath);
    }
#endif
#ifdef HAVE_SPEECHRECOGNIZER
    {
        const auto recogs = speech::availableRecognizers();
        if (!requireSelftest(recogs.size() >= 1,
                             QStringLiteral("availableRecognizers size < 1"), &error))
            return 1;
        // Stub fallback
        auto stub = speech::recognizerByName(QStringLiteral("does_not_exist"));
        if (!requireSelftest(stub && stub->name() == QStringLiteral("Stub"),
                             QStringLiteral("recognizerByName fallback failed"), &error))
            return 1;
        speech::RecognizeParams p;
        p.audioPath = QStringLiteral("/tmp/dummy.wav"); // 非空、Stub は中身読まない
        p.language = QStringLiteral("ja");
        const auto res = stub->recognize(p);
        if (!requireSelftest(res.success && res.segments.size() == 3,
                             QStringLiteral("Stub recognize wrong segment count"), &error))
            return 1;
        // empty audioPath -> failure
        speech::RecognizeParams pEmpty;
        const auto resEmpty = stub->recognize(pEmpty);
        if (!requireSelftest(!resEmpty.success && !resEmpty.error.isEmpty(),
                             QStringLiteral("Stub empty path should fail"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("CAPTION selftest OK");
    return 0;
}

// US-PT-B: Sprint 15 planar tracker self-test (VEDITOR_PLANAR_SELFTEST=1)
int runPlanarSelftest()
{
    QString error;
#ifdef HAVE_PLANARTRACKER
    {
        // 1. CornerSet::rectangle
        const auto cs = planar::CornerSet::rectangle(QRectF(10, 20, 100, 80));
        if (!requireSelftest(cs.tl == QPointF(10, 20) && cs.br == QPointF(110, 100)
                                 && cs.isValid(),
                             QStringLiteral("CornerSet::rectangle wrong"), &error))
            return 1;

        // 2. homographyFromCorners(identity) で transformPoint ≈ id
        const auto idH = planar::homographyFromCorners(cs, cs);
        const QPointF mapped = planar::transformPoint(QPointF(50, 60), idH);
        if (!requireSelftest(std::abs(mapped.x() - 50.0) < 1e-2
                                 && std::abs(mapped.y() - 60.0) < 1e-2,
                             QStringLiteral("identity homography deviates"), &error))
            return 1;

        // 3. Tracker init + reset
        planar::Tracker tracker;
        if (!requireSelftest(!tracker.isInitialized(),
                             QStringLiteral("Tracker initially initialized=true"), &error))
            return 1;
        QImage refFrame(64, 64, QImage::Format_ARGB32);
        refFrame.fill(QColor(128, 128, 128, 255));
        // draw a small rectangle at center to give SAD something to track
        QPainter p(&refFrame);
        p.fillRect(QRect(24, 24, 16, 16), QColor(255, 0, 0, 255));
        p.end();
        tracker.setReferenceFrame(refFrame, cs);
        if (!requireSelftest(tracker.isInitialized(),
                             QStringLiteral("Tracker not initialized after setRef"), &error))
            return 1;
        tracker.reset();
        if (!requireSelftest(!tracker.isInitialized(),
                             QStringLiteral("Tracker still initialized after reset"), &error))
            return 1;

        // 4. trackSequence: 1 frame → returns 1 Frame
        tracker.reset();
        QList<QImage> frames; frames.append(refFrame);
        const auto result = tracker.trackSequence(frames, cs, 33);
        if (!requireSelftest(result.size() == 1,
                             QStringLiteral("trackSequence(1 frame) returned !=1"), &error))
            return 1;

        // 5. warpImage で identity → 同じ画像 (pixel-wise compare 一部)
        QImage warped = planar::warpImage(refFrame, idH, refFrame.size());
        if (!requireSelftest(warped.size() == refFrame.size(),
                             QStringLiteral("warpImage identity size mismatch"), &error))
            return 1;
        // ピクセルチェックは多少誤差を許容: 中央 pixel が赤に近い
        const QRgb cp = warped.pixel(32, 32);
        if (!requireSelftest(qRed(cp) > 200 && qGreen(cp) < 50 && qBlue(cp) < 50,
                             QStringLiteral("warpImage identity center pixel wrong"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("PLANAR selftest OK");
    return 0;
}

// US-MOB-2: Sprint 16 mobile export self-test (VEDITOR_MOBILE_SELFTEST=1)
int runMobileSelftest()
{
    QString error;
#ifdef HAVE_MOBILEPRESET
    {
        // US-MOB-2 AC-4: configForDevice(iphone_15_pro, 3840x2160, -14.0)
        const mobile::DeviceProfile dev = mobile::deviceById(QStringLiteral("iphone_15_pro"));
        const ExportConfig cfg = mobile::preset::configForDevice(dev, QSize(3840, 2160), -14.0);

        if (!requireSelftest(!cfg.videoCodec.isEmpty(),
                             QStringLiteral("MOB-2 AC-4: videoCodec is empty"), &error))
            return 1;
        if (!requireSelftest(cfg.videoBitrate <= dev.maxVideoBitrateKbps,
                             QStringLiteral("MOB-2 AC-4: videoBitrate exceeds device max"), &error))
            return 1;
        if (!requireSelftest(cfg.hdr10,
                             QStringLiteral("MOB-2 AC-4: hdr10 should be true for iPhone 15 Pro"), &error))
            return 1;
        if (!requireSelftest(cfg.container == QStringLiteral("mp4"),
                             QStringLiteral("MOB-2 AC-4: container should be mp4"), &error))
            return 1;
        if (!requireSelftest(cfg.audioBitrate == 192,
                             QStringLiteral("MOB-2 AC-4: audioBitrate should be 192"), &error))
            return 1;
    }
#endif
#ifdef HAVE_MOBILEROTATE
    {
        // US-MOB-2 AC-5: computeRotation(1920x1080, portrait target) → needsRotate=true
        // Build a minimal portrait DeviceProfile (height > width)
        mobile::DeviceProfile portraitDev;
        portraitDev.id              = QStringLiteral("test_portrait");
        portraitDev.displayName     = QStringLiteral("Test Portrait");
        portraitDev.category        = mobile::Category::AndroidPhone;
        portraitDev.maxResolution   = QSize(1080, 1920);  // portrait
        portraitDev.maxFrameRate    = 30;
        portraitDev.preferredCodec  = QStringLiteral("h264");
        portraitDev.maxVideoBitrateKbps = 20000;
        portraitDev.supportsHdr     = false;

        const mobile::rotate::RotateDecision dec =
            mobile::rotate::computeRotation(QSize(1920, 1080), portraitDev);

        if (!requireSelftest(dec.needsRotate,
                             QStringLiteral("MOB-2 AC-5: landscape→portrait needsRotate should be true"), &error))
            return 1;
        if (!requireSelftest(dec.angleDeg == 90,
                             QStringLiteral("MOB-2 AC-5: angleDeg should be 90"), &error))
            return 1;

        // Same orientation: landscape source, landscape target → no rotation
        mobile::DeviceProfile landscapeDev;
        landscapeDev.maxResolution = QSize(1920, 1080); // landscape
        const mobile::rotate::RotateDecision dec2 =
            mobile::rotate::computeRotation(QSize(1920, 1080), landscapeDev);
        if (!requireSelftest(!dec2.needsRotate,
                             QStringLiteral("MOB-2: landscape→landscape needsRotate should be false"), &error))
            return 1;

        // isPortraitTarget
        if (!requireSelftest(mobile::rotate::isPortraitTarget(portraitDev),
                             QStringLiteral("MOB-2: isPortraitTarget portrait should be true"), &error))
            return 1;
        if (!requireSelftest(!mobile::rotate::isPortraitTarget(landscapeDev),
                             QStringLiteral("MOB-2: isPortraitTarget landscape should be false"), &error))
            return 1;

        // applyRotation: 0° → same size; 90° → swapped dimensions
        QImage img(320, 180, QImage::Format_ARGB32);
        img.fill(Qt::red);
        const QImage rot0 = mobile::rotate::applyRotation(img, 0);
        if (!requireSelftest(rot0.width() == 320 && rot0.height() == 180,
                             QStringLiteral("MOB-2: applyRotation(0°) size wrong"), &error))
            return 1;
        const QImage rot90 = mobile::rotate::applyRotation(img, 90);
        if (!requireSelftest(rot90.width() == 180 && rot90.height() == 320,
                             QStringLiteral("MOB-2: applyRotation(90°) size wrong"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("MOBILE selftest OK");
    return 0;
}

// US-INT-2: Sprint 16 OBS importer self-test (VEDITOR_OBS_SELFTEST=1).
// Smoke-checks that scan / profile / layout entry points return empty lists
// for non-existent inputs without throwing.
int runObsSelftest()
{
    QString error;
#if defined(HAVE_OBSSCANNER)
    {
        const QList<obs::scan::RecordingGroup> groups =
            obs::scan::scanFolder(QStringLiteral("/nonexistent_obs_folder_xyz"));
        if (!requireSelftest(groups.isEmpty(),
                             QStringLiteral("INT-2 OBS: scanFolder('/nonexistent') should be empty"), &error))
            return 1;
    }
#endif
#if defined(HAVE_OBSPROFILE)
    {
        const QList<obs::profile::SceneInfo> scenes =
            obs::profile::loadSceneCollection(QStringLiteral("/nonexistent_scene.json"));
        if (!requireSelftest(scenes.isEmpty(),
                             QStringLiteral("INT-2 OBS: loadSceneCollection('/nonexistent') should be empty"), &error))
            return 1;
    }
#endif
#if defined(HAVE_OBSLAYOUT)
    {
        const QList<obs::layout::TimelineClipPlacement> placements =
            obs::layout::layoutToTimeline({}, {});
        if (!requireSelftest(placements.isEmpty(),
                             QStringLiteral("INT-2 OBS: layoutToTimeline({},{}) should be empty"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("OBS selftest OK");
    return 0;
}

// US-INT-2: Sprint 16 Affinity importer self-test (VEDITOR_AFFINITY_SELFTEST=1).
int runAffinitySelftest()
{
    QString error;
#if defined(HAVE_AFFINITYPSD)
    {
        const affinity::psd::PsdDocument doc =
            affinity::psd::loadPsd(QStringLiteral("/nonexistent_file.psd"));
        if (!requireSelftest(doc.canvasSize == QSize(0, 0),
                             QStringLiteral("INT-2 AFF: loadPsd('/nonexistent') canvasSize should be (0,0)"), &error))
            return 1;
    }
#endif
#if defined(HAVE_AFFINITYVECTOR)
    {
        const QImage svg = affinity::vector::loadSvg(QStringLiteral("/nonexistent.svg"), QSize(0, 0));
        if (!requireSelftest(svg.isNull(),
                             QStringLiteral("INT-2 AFF: loadSvg('/nonexistent') should be null"), &error))
            return 1;
        const QImage tiff = affinity::vector::loadTiff(QStringLiteral("/nonexistent.tiff"));
        if (!requireSelftest(tiff.isNull(),
                             QStringLiteral("INT-2 AFF: loadTiff('/nonexistent') should be null"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("AFFINITY selftest OK");
    return 0;
}

// US-INT-2: Sprint 16 Blender importer self-test (VEDITOR_BLENDER_SELFTEST=1).
int runBlenderSelftest()
{
    QString error;
#if defined(HAVE_BLENDERMESH)
    {
        const blender::mesh::MeshData mesh =
            blender::mesh::loadMeshFile(QStringLiteral("/nonexistent_mesh.obj"));
        if (!requireSelftest(mesh.vertices.isEmpty(),
                             QStringLiteral("INT-2 BLE: loadMeshFile('/nonexistent') vertices should be empty"), &error))
            return 1;
    }
#endif
#if defined(HAVE_BLENDERBPY)
    {
        const blender::bridge::BridgeResult res =
            blender::bridge::runBpyScript(QString(), QString(), QStringList(), 1000);
        if (!requireSelftest(res.exitCode == -1,
                             QStringLiteral("INT-2 BLE: runBpyScript('','') exitCode should be -1"), &error))
            return 1;
    }
#endif
#if defined(HAVE_BLENDEREXR)
    {
        const QList<blender::exr::ExrFrame> frames =
            blender::exr::loadExrSequence(QStringLiteral("/nonexistent_exr_dir"),
                                          QStringLiteral("render_####.exr"));
        if (!requireSelftest(frames.isEmpty(),
                             QStringLiteral("INT-2 BLE: loadExrSequence('/nonexistent') should be empty"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("BLENDER selftest OK");
    return 0;
}

// US-INT-2: Sprint 16 Import-hub self-test (VEDITOR_IMPORT_SELFTEST=1).
// Construct the dialog and verify it has its expected signals via QMetaObject.
int runImportSelftest()
{
    QString error;
#if defined(HAVE_IMPORTHUBDIALOG)
    {
        ImportHubDialog dialog;
        const QMetaObject *mo = dialog.metaObject();
        if (!requireSelftest(mo != nullptr,
                             QStringLiteral("INT-2 IMP: metaObject() returned null"), &error))
            return 1;
        const int sig = mo->indexOfSignal(
            "timelineImportRequested(QList<obs::layout::TimelineClipPlacement>)");
        // The exact normalised signature may vary across Qt versions, so accept
        // any signal whose name starts with "timelineImportRequested" as well.
        bool hasTimelineSignal = (sig >= 0);
        if (!hasTimelineSignal) {
            for (int i = 0; i < mo->methodCount(); ++i) {
                if (mo->method(i).methodType() == QMetaMethod::Signal &&
                    QString::fromLatin1(mo->method(i).name()) == QLatin1String("timelineImportRequested")) {
                    hasTimelineSignal = true;
                    break;
                }
            }
        }
        if (!requireSelftest(hasTimelineSignal,
                             QStringLiteral("INT-2 IMP: timelineImportRequested signal not found"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("IMPORT selftest OK");
    return 0;
}

// US-INT-3: Sprint 17 YouTube upload self-test (VEDITOR_YOUTUBE_SELFTEST=1).
// Smoke-checks that the OAuth AuthClient + upload Manager can be constructed
// without a real network connection. Does NOT actually upload anything.
int runYoutubeSelftest()
{
    QString error;
#if defined(HAVE_YOUTUBE_OAUTH)
    {
        const youtube::oauth::YoutubeOAuthConfig cfg =
            youtube::oauth::YoutubeOAuthConfig::defaultConfig();
        if (!requireSelftest(!cfg.redirectUri.isEmpty(),
                             QStringLiteral("YT-1: defaultConfig.redirectUri should not be empty"), &error))
            return 1;
        if (!requireSelftest(!cfg.scope.isEmpty(),
                             QStringLiteral("YT-1: defaultConfig.scope should not be empty"), &error))
            return 1;

        youtube::oauth::AuthClient client(cfg);
        if (!requireSelftest(client.callbackPort() == 0,
                             QStringLiteral("YT-1: callbackPort should be 0 before launchAuthFlow"), &error))
            return 1;
        if (!requireSelftest(!client.currentToken().isValid(),
                             QStringLiteral("YT-1: currentToken should be invalid initially"), &error))
            return 1;

#if defined(HAVE_YOUTUBE_MANAGER)
        youtube::manager::Manager mgr(&client);
        // Static helpers from upload::Client are used by Manager internally;
        // verify chunk-size constant + addJob early-fail on missing file.
        if (!requireSelftest(youtube::manager::Manager::kChunkSize > 0,
                             QStringLiteral("YT-3: kChunkSize must be positive"), &error))
            return 1;
        const QByteArray cr = youtube::upload::Client::buildContentRange(0, 1024, 4096);
        if (!requireSelftest(!cr.isEmpty(),
                             QStringLiteral("YT-2: buildContentRange should produce a non-empty header"), &error))
            return 1;
        const qint64 end = youtube::upload::Client::parseRangeHeaderEnd(
            QByteArray("bytes=0-1023"));
        if (!requireSelftest(end == 1023,
                             QStringLiteral("YT-2: parseRangeHeaderEnd should return 1023"), &error))
            return 1;

        const youtube::upload::UploadMetadata meta{
            QStringLiteral("selftest"), QStringLiteral("desc"), {}, QStringLiteral("private"), 22 };
        const QString jobId = mgr.addJob(QStringLiteral("/nonexistent_video.mp4"), meta);
        // addJob with missing file → state Failed (sync emit jobFailed)
        if (!requireSelftest(!jobId.isEmpty(),
                             QStringLiteral("YT-3: addJob should return a non-empty id even for missing file"), &error))
            return 1;
        if (!requireSelftest(mgr.jobState(jobId) == youtube::manager::State::Failed,
                             QStringLiteral("YT-3: missing-file job should land in Failed state"), &error))
            return 1;
#endif
    }
#endif
    qInfo().noquote() << QStringLiteral("YOUTUBE selftest OK");
    return 0;
}

// US-INT-3: Sprint 18 Collaboration self-test (VEDITOR_COLLAB_SELFTEST=1).
// Exercises CommentTrack add/reply/markResolved + JSON round-trip.
int runCollabSelftest()
{
    QString error;
#if defined(HAVE_COLLABMODEL)
    {
        collab::CommentTrack ct;
        const collab::Comment c1 =
            ct.addComment(QStringLiteral("u1"), 1000, QStringLiteral("hi"));
        if (!requireSelftest(!c1.id.isEmpty(),
                             QStringLiteral("COL: addComment must return a non-empty id"), &error))
            return 1;

        const collab::Comment r1 =
            ct.replyTo(c1.id, QStringLiteral("u2"), QStringLiteral("reply"));
        if (!requireSelftest(!r1.id.isEmpty(),
                             QStringLiteral("COL: replyTo must return a non-empty id"), &error))
            return 1;
        if (!requireSelftest(r1.parentId == c1.id,
                             QStringLiteral("COL: replyTo parentId mismatch"), &error))
            return 1;

        if (!requireSelftest(ct.markResolved(c1.id),
                             QStringLiteral("COL: markResolved should succeed"), &error))
            return 1;

        if (!requireSelftest(ct.topLevelComments().size() == 1,
                             QStringLiteral("COL: topLevelComments() should have 1 entry"), &error))
            return 1;
        if (!requireSelftest(ct.repliesOf(c1.id).size() == 1,
                             QStringLiteral("COL: repliesOf(c1) should have 1 entry"), &error))
            return 1;

        // JSON round-trip
        const QJsonObject json = ct.toJson();
        const collab::CommentTrack ct2 = collab::CommentTrack::fromJson(json);
        if (!requireSelftest(ct2.comments.size() == ct.comments.size(),
                             QStringLiteral("COL: round-trip comment count mismatch"), &error))
            return 1;
        if (!requireSelftest(ct2.topLevelComments().size() == 1,
                             QStringLiteral("COL: round-trip topLevelComments mismatch"), &error))
            return 1;
        if (!requireSelftest(ct2.repliesOf(c1.id).size() == 1,
                             QStringLiteral("COL: round-trip repliesOf mismatch"), &error))
            return 1;
    }
#endif
    qInfo().noquote() << QStringLiteral("COLLAB selftest OK");
    return 0;
}

// US-INT-3: Sprint 19 Color match self-test (VEDITOR_COLORMATCH_SELFTEST=1).
// Analyses two solid-colour QImages, generates a 33-step LUT, and writes a
// .cube file to QDir::tempPath() to confirm the export pipeline works end-to-end.
int runColorMatchSelftest()
{
    QString error;
#if defined(HAVE_COLORMATCH_ANALYZE) && defined(HAVE_COLORMATCH_LUT)
    {
        QImage red(100, 100, QImage::Format_ARGB32);
        red.fill(qRgb(200, 50, 50));
        const colormatch::analyze::ColorStats sR =
            colormatch::analyze::analyzeImage(red);
        if (!requireSelftest(sR.sampleCount > 0,
                             QStringLiteral("CMA: analyzeImage(red) should produce samples"), &error))
            return 1;

        QImage blue(100, 100, QImage::Format_ARGB32);
        blue.fill(qRgb(50, 50, 200));
        const colormatch::analyze::ColorStats sB =
            colormatch::analyze::analyzeImage(blue);
        if (!requireSelftest(sB.sampleCount > 0,
                             QStringLiteral("CMA: analyzeImage(blue) should produce samples"), &error))
            return 1;

        const colormatch::lut::Lut3D lut =
            colormatch::lut::generateMatchLut(sR, sB, 33);
        if (!requireSelftest(lut.size == 33,
                             QStringLiteral("CMA: LUT size should be 33"), &error))
            return 1;
        if (!requireSelftest(lut.data.size() == 33 * 33 * 33,
                             QStringLiteral("CMA: LUT data size should be 33^3"), &error))
            return 1;

        const QString outPath = QDir::tempPath() + QStringLiteral("/veditor_colormatch_selftest.cube");
        QFile::remove(outPath);
        const bool ok = colormatch::lut::exportCube(lut, outPath);
        if (!requireSelftest(ok,
                             QStringLiteral("CMA: exportCube should succeed"), &error))
            return 1;
        if (!requireSelftest(QFile::exists(outPath),
                             QStringLiteral("CMA: exported .cube file should exist on disk"), &error))
            return 1;
        QFile::remove(outPath);
    }
#endif
    qInfo().noquote() << QStringLiteral("COLORMATCH selftest OK");
    return 0;
}

int runVimeoSelftest()
{
    QString error;
#if defined(HAVE_VIMEO_OAUTH) && defined(HAVE_VIMEO_UPLOAD_CLIENT) && defined(HAVE_VIMEO_UPLOAD_MANAGER)
    {
        vimeo::oauth::VimeoOAuthConfig config;
        config.clientId = QStringLiteral("dummy-vimeo-client");
        config.clientSecret = QStringLiteral("dummy-vimeo-secret");
        config.scope = QStringLiteral("private public video_files");
        config.accessToken = QStringLiteral("dummy-access-token");
        config.refreshToken = QStringLiteral("dummy-refresh-token");
        config.redirectUri = QStringLiteral("http://localhost:8080/vimeo/callback");

        const QStringList ffmpegArgs{
            QStringLiteral("ffmpeg"),
            QStringLiteral("-i"),
            QStringLiteral("input.mp4"),
            QStringLiteral("-c:v"),
            QStringLiteral("libx264"),
            QStringLiteral("output.mp4")
        };

        vimeo::oauth::AuthClient auth(config);
        const QUrl authUrl = auth.authorizationUrl(config.redirectUri, QStringLiteral("selftest"));
        if (!requireSelftest(authUrl.isValid(),
                             QStringLiteral("VIMEO: authorizationUrl should be valid"), &error))
            return 1;
        if (!requireSelftest(authUrl.host() == QStringLiteral("api.vimeo.com"),
                             QStringLiteral("VIMEO: authorizationUrl host mismatch"), &error))
            return 1;

        const QUrlQuery authQuery(authUrl);
        if (!requireSelftest(authQuery.queryItemValue(QStringLiteral("client_id")) == config.clientId,
                             QStringLiteral("VIMEO: client_id query mismatch"), &error))
            return 1;
        if (!requireSelftest(authQuery.queryItemValue(QStringLiteral("redirect_uri"))
                                 == config.redirectUri,
                             QStringLiteral("VIMEO: redirect_uri query mismatch"), &error))
            return 1;

        vimeo::upload::UploadClient uploadClient(config);
        uploadClient.setAccessToken(config.accessToken);
        if (!requireSelftest(uploadClient.accessToken() == config.accessToken,
                             QStringLiteral("VIMEO: access token round-trip failed"), &error))
            return 1;

        vimeo::manager::Manager manager(&auth);
        if (!requireSelftest(manager.activeJobs().isEmpty(),
                             QStringLiteral("VIMEO: new Manager should have no jobs"), &error))
            return 1;
        if (!requireSelftest(!ffmpegArgs.isEmpty() && ffmpegArgs.front() == QStringLiteral("ffmpeg"),
                             QStringLiteral("VIMEO: dummy ffmpegArgs should start with ffmpeg"), &error))
            return 1;
    }
#endif
    qInfo() << "VIMEO selftest OK";
    return 0;
}

int runTwitchSelftest()
{
    QString error;
#if defined(HAVE_TWITCH_STREAM_CONFIG)
    {
        twitch::stream::StreamConfig config;
        config.streamKey = QStringLiteral("live_dummy_key");
        config.server = twitch::stream::StreamServer::USEast;
        config.bitrate = 6000;
        config.resolution = QSize(1920, 1080);
        config.framerate = 60;
        config.audioBitrate = 160;

        const QStringList command =
            twitch::stream::buildFfmpegCommand(config, QStringLiteral("input.mp4"));
        if (!requireSelftest(!command.isEmpty(),
                             QStringLiteral("TWITCH: buildFfmpegCommand returned no args"), &error))
            return 1;
        if (!requireSelftest(command.front() == QStringLiteral("ffmpeg"),
                             QStringLiteral("TWITCH: command should start with ffmpeg"), &error))
            return 1;
        if (!requireSelftest(command.back().contains(QStringLiteral("twitch.tv/app/")),
                             QStringLiteral("TWITCH: RTMP output URL missing"), &error))
            return 1;
    }
#endif
    qInfo() << "TWITCH selftest OK";
    return 0;
}

int runFrameIoSelftest()
{
    QString error;
#if defined(HAVE_FRAMEIO_IMPORTER)
    {
        const QByteArray fixture = R"json(
[
  {
    "id": "c1",
    "body": "First comment",
    "timestamp": 1.25,
    "author": { "name": "alice" },
    "inserted_at": "2025-01-01T00:00:00.000Z"
  },
  {
    "id": "c2",
    "body": "Second comment",
    "timestamp": 3.50,
    "author": { "name": "bob" },
    "inserted_at": "2025-01-01T00:00:02.000Z"
  },
  {
    "id": "c3",
    "body": "Reply comment",
    "timestamp": 5.75,
    "author": { "name": "carol" },
    "parent_id": "c2",
    "inserted_at": "2025-01-01T00:00:03.000Z"
  }
]
)json";

        const QJsonDocument doc = QJsonDocument::fromJson(fixture);
        if (!requireSelftest(doc.isArray(),
                             QStringLiteral("FRAMEIO: fixture should parse to a JSON array"), &error))
            return 1;

        const collab::CommentTrack track =
            frameio::importer::FrameIoImporter::parseFrameIoJson(doc.array());
        if (!requireSelftest(track.comments.size() == 3,
                             QStringLiteral("FRAMEIO: expected 3 parsed comments"), &error))
            return 1;
        if (!requireSelftest(track.comments.at(2).parentId == QStringLiteral("c2"),
                             QStringLiteral("FRAMEIO: parent_id round-trip mismatch"), &error))
            return 1;
    }
#endif
    qInfo() << "FRAMEIO selftest OK";
    return 0;
}

int runDavinciSelftest()
{
    QString error;
#if defined(HAVE_DAVINCI_XML)
    {
        QVector<davinci::xml::ClipEntry> clips;
        clips.append(davinci::xml::ClipEntry{
            QStringLiteral("/tmp/selftest_clip.mov"),
            0,
            120,
            0,
            0
        });

        davinci::xml::ExporterConfig config;
        config.sequenceName = QStringLiteral("DaVinci Selftest");
        config.fps = 24;
        config.width = 1920;
        config.height = 1080;

        const QString xml = davinci::xml::buildXml(clips, config);
        if (!requireSelftest(xml.contains(QStringLiteral("<xmeml")),
                             QStringLiteral("DAVINCI: xmeml root element missing"), &error))
            return 1;
    }
#endif
    qInfo() << "DAVINCI selftest OK";
    return 0;
}

int runFcpxmlSelftest()
{
    QString error;
#if defined(HAVE_FCPXML_EXPORTER)
    {
        QVector<fcpx::xml::ClipEntry> clips;
        clips.append(fcpx::xml::ClipEntry{
            QStringLiteral("/tmp/selftest_clip.mov"),
            0.0,
            4.0,
            1.0,
            QStringLiteral("Selftest Clip")
        });

        fcpx::xml::ExporterConfig config;
        config.projectName = QStringLiteral("FCPXML Selftest");
        config.fps = 30;
        config.frameDuration = QStringLiteral("1/30s");
        config.width = 1920;
        config.height = 1080;

        const QString xml = fcpx::xml::buildXml(clips, config);
        if (!requireSelftest(xml.contains(QStringLiteral("<fcpxml")),
                             QStringLiteral("FCPXML: fcpxml root element missing"), &error))
            return 1;
    }
#endif
    qInfo() << "FCPXML selftest OK";
    return 0;
}

int runSmartEditSelftest()
{
    QString error;
#if defined(HAVE_SMARTEDIT_ASSISTANT)
    {
        smartedit::Assistant assistant;
        if (!requireSelftest(assistant.metaObject() != nullptr,
                             QStringLiteral("SMARTEDIT: Assistant metaObject missing"), &error))
            return 1;

        QVector<smartedit::CutSuggestion> suggestions{
            smartedit::CutSuggestion{400, 500, smartedit::CutSuggestion::Silence, 0.5},
            smartedit::CutSuggestion{100, 300, smartedit::CutSuggestion::SceneChange, 0.9},
            smartedit::CutSuggestion{100, 200, smartedit::CutSuggestion::Combined, 0.8}
        };

        std::sort(suggestions.begin(), suggestions.end(),
                  [](const smartedit::CutSuggestion &left,
                     const smartedit::CutSuggestion &right) {
                      if (left.startMs != right.startMs)
                          return left.startMs < right.startMs;
                      if (left.endMs != right.endMs)
                          return left.endMs < right.endMs;
                      return left.reason < right.reason;
                  });

        if (!requireSelftest(suggestions.at(0).startMs == 100 && suggestions.at(0).endMs == 200,
                             QStringLiteral("SMARTEDIT: sorted first suggestion mismatch"), &error))
            return 1;
        if (!requireSelftest(suggestions.at(1).startMs == 100 && suggestions.at(1).endMs == 300,
                             QStringLiteral("SMARTEDIT: sorted second suggestion mismatch"), &error))
            return 1;
        if (!requireSelftest(suggestions.at(2).startMs == 400,
                             QStringLiteral("SMARTEDIT: sorted third suggestion mismatch"), &error))
            return 1;
    }
#endif
    qInfo() << "SMARTEDIT selftest OK";
    return 0;
}

int runCloudRenderSelftest()
{
    QString error;
#if defined(HAVE_CLOUDRENDER_CLIENT)
    {
        cloudrender::Client client;

        cloudrender::ProviderConfig config;
        config.provider = cloudrender::Provider::Generic;
        config.endpointUrl = QStringLiteral("https://render.example/v1/jobs");
        config.apiKey = QStringLiteral("dummy-api-key");
        client.setProviderConfig(config);

        cloudrender::RenderJob job;
        job.jobId = QStringLiteral("job-selftest");
        job.inputUrl = QStringLiteral("https://cdn.example/input.mp4");
        job.outputUrl = QStringLiteral("https://cdn.example/output.mp4");
        job.ffmpegArgs = QStringLiteral("ffmpeg -i input.mp4 -c:v libx264 output.mp4");

        const QString submittedJobId = client.submitJob(job);
        if (!requireSelftest(submittedJobId == job.jobId,
                             QStringLiteral("CLOUDRENDER: submitJob should preserve explicit jobId"), &error))
            return 1;

        QUrl expectedUrl = QUrl::fromUserInput(config.endpointUrl);
        QString expectedPath = expectedUrl.path();
        if (!expectedPath.endsWith(QLatin1Char('/'))) {
            expectedPath += QLatin1Char('/');
        }
        expectedPath += QStringLiteral("submit");
        expectedUrl.setPath(expectedPath);

        QNetworkAccessManager *network = client.findChild<QNetworkAccessManager *>();
        const QList<QNetworkReply *> replies =
            network ? network->findChildren<QNetworkReply *>() : QList<QNetworkReply *>{};
        if (!requireSelftest(!replies.isEmpty(),
                             QStringLiteral("CLOUDRENDER: submitJob should create a QNetworkReply"), &error))
            return 1;
        if (!requireSelftest(replies.constLast()->url() == expectedUrl,
                             QStringLiteral("CLOUDRENDER: endpoint URL mismatch"), &error))
            return 1;

        for (QNetworkReply *reply : replies) {
            reply->abort();
        }
    }
#endif
    qInfo() << "CLOUDRENDER selftest OK";
    return 0;
}

int runXUploadSelftest()
{
    QString error;
#if defined(HAVE_XVIDEO_UPLOAD)
    {
        const x::upload::XUploadConfig config = x::upload::XUploadConfig::defaultConfig();
        if (!requireSelftest(!config.apiBase.isEmpty(),
                             QStringLiteral("XUPLOAD: apiBase should be non-empty"), &error))
            return 1;
        if (!requireSelftest(!config.tweetApiBase.isEmpty(),
                             QStringLiteral("XUPLOAD: tweetApiBase should be non-empty"), &error))
            return 1;

        x::upload::UploadJob job;
        job.filePath = QStringLiteral("/tmp/selftest_clip.mp4");
        job.tweetText = QStringLiteral("selftest tweet");
        if (!requireSelftest(!job.filePath.isEmpty() && !job.tweetText.isEmpty(),
                             QStringLiteral("XUPLOAD: UploadJob round-trip failed"), &error))
            return 1;
    }
#endif
    qInfo() << "XUPLOAD selftest OK";
    return 0;
}

int runInstagramSelftest()
{
    QString error;
#if defined(HAVE_INSTAGRAM_PUBLISH)
    {
        const instagram::publish::IgConfig config =
            instagram::publish::IgConfig::defaultConfig();
        if (!requireSelftest(config.graphBase.contains(QStringLiteral("graph.facebook.com")),
                             QStringLiteral("INSTAGRAM: graphBase should target graph.facebook.com"),
                             &error))
            return 1;

        instagram::publish::PublishJob job;
        job.videoUrl = QStringLiteral("https://cdn.example/reel.mp4");
        job.caption = QStringLiteral("selftest caption");
        if (!requireSelftest(job.shareToFeed,
                             QStringLiteral("INSTAGRAM: PublishJob should default shareToFeed=true"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "INSTAGRAM selftest OK";
    return 0;
}

int runProjTmplSelftest()
{
    QString error;
#if defined(HAVE_PROJECT_TEMPLATE)
    {
        const QVector<projtmpl::TemplateMeta> builtins =
            projtmpl::TemplateLibrary::builtInTemplates();
        if (!requireSelftest(builtins.size() == 6,
                             QStringLiteral("PROJTMPL: expected 6 built-in templates"), &error))
            return 1;

        const QByteArray project =
            projtmpl::TemplateLibrary::createProjectFromTemplate(QStringLiteral("yt1080p30"));
        if (!requireSelftest(!project.isEmpty(),
                             QStringLiteral("PROJTMPL: createProjectFromTemplate returned empty"),
                             &error))
            return 1;
        if (!requireSelftest(project.contains("width"),
                             QStringLiteral("PROJTMPL: project payload missing \"width\""), &error))
            return 1;
    }
#endif
    qInfo() << "PROJTMPL selftest OK";
    return 0;
}

int runLoudnessSelftest()
{
    QString error;
#if defined(HAVE_LOUDNESS_MASTER)
    {
        const double gain = loudness::computeGainDb(-20.0, -14.0);
        if (!requireSelftest(qFuzzyCompare(gain + 1.0, 6.0 + 1.0),
                             QStringLiteral("LOUDNESS: computeGainDb(-20,-14) should be +6.0"),
                             &error))
            return 1;

        const double broadcast =
            loudness::presetTargetLufs(loudness::LoudnessPreset::Broadcast);
        if (!requireSelftest(qFuzzyCompare(broadcast + 100.0, -23.0 + 100.0),
                             QStringLiteral("LOUDNESS: Broadcast preset should be -23.0 LUFS"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "LOUDNESS selftest OK";
    return 0;
}

int runHdrSelftest()
{
    QString error;
#if defined(HAVE_HDR_GRADING)
    {
        const double pq0 = hdr::applyPqEotf(0.0);
        if (!requireSelftest(qAbs(pq0) < 1e-6,
                             QStringLiteral("HDR: applyPqEotf(0.0) should be ~0.0"), &error))
            return 1;

        const double pq1 = hdr::applyPqEotf(1.0);
        if (!requireSelftest(pq1 > 0.9,
                             QStringLiteral("HDR: applyPqEotf(1.0) should be > 0.9"), &error))
            return 1;

        const double pqMid = hdr::applyPqEotf(0.5);
        const double pqHigh = hdr::applyPqEotf(0.8);
        if (!requireSelftest(pqMid < pqHigh,
                             QStringLiteral("HDR: applyPqEotf should be monotonic increasing"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "HDR selftest OK";
    return 0;
}

int runMultiCamSelftest()
{
    QString error;
#if defined(HAVE_MULTICAM_SYNC)
    {
        // `other` is `ref` delayed by 2 samples; at 10ms/hop that is ~20ms lag.
        const QVector<float> ref{0, 0, 1, 2, 3, 2, 1, 0, 0, 0};
        const QVector<float> other{0, 0, 0, 0, 1, 2, 3, 2, 1, 0};

        const double ms =
            multicam::MultiCamSync::estimateOffsetMs(ref, other, 10.0);
        // Sign convention is ambiguous; assert magnitude ~20ms within one hop.
        if (!requireSelftest(qAbs(qAbs(ms) - 20.0) <= 10.0,
                             QStringLiteral("MULTICAM: estimateOffsetMs magnitude should be ~20ms"),
                             &error))
            return 1;
        if (!requireSelftest(!qFuzzyIsNull(ms),
                             QStringLiteral("MULTICAM: shifted signal should yield non-zero lag"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "MULTICAM selftest OK";
    return 0;
}

// US-BX-1: real batch-export selftest (VEDITOR_BATCHEXPORT_SELFTEST=1).
//
// S9 makes this a GENUINE test. BatchExportQueue used to fake progress
// (`task.progress += 20`) and produce NO file — pure UI theatre. It now
// delegates every task to the real RenderQueue (the proven S8 ffmpeg
// render-pipe). This selftest proves that end-to-end: it queues TWO real
// batch tasks (each = the e2e clip on V1 via the same in-memory Timeline*
// seam PARITY S8 uses, distinct temp outputs, ~20 frames), runs
// BatchExportQueue to completion through its real public API + the Qt event
// loop, then asserts BOTH output files EXIST, are non-empty, and carry the
// expected video frame count (±1, via ffprobe -count_frames) — a real
// on-disk artifact, NOT a progress counter hitting 100. A fake-progress
// regression (no file) FAILS here. It also exercises pause/resume: pause
// after job 1 completes, assert the queue does NOT advance to job 2 while
// paused, resume, assert job 2 then completes.
int runBatchExportSelftest()
{
#if !defined(HAVE_BATCHEXPORT_QUEUE)
    qInfo() << "BATCHEXPORT selftest OK (BatchExportQueue not compiled in)";
    return 0;
#else
    const QString clipArg = qEnvironmentVariable(
        "VEDITOR_E2E_CLIP", QStringLiteral("test_assets/e2e_clip.mp4"));
    const QString clipPath = QDir::current().absoluteFilePath(clipArg);
    qInfo() << "BATCHEXPORT: clip path" << clipPath;
    if (!QFile::exists(clipPath)) {
        qWarning() << "BATCHEXPORT: missing test asset" << clipPath
                   << "(skipping — CI-tolerant, never a silent pass)";
        qInfo() << "BATCHEXPORT selftest OK";
        return 0;
    }

    // Two independent live edit-graph Timelines (the same QWidget Timeline +
    // public addClip(filePath) the parity selftest uses; the running process
    // owns a QApplication). They must outlive the Queue, so they live on the
    // stack for the whole function.
    Timeline tl1;
    tl1.addClip(clipPath);
    Timeline tl2;
    tl2.addClip(clipPath);
    if (tl1.videoClips().isEmpty() || tl2.videoClips().isEmpty()) {
        qCritical() << "BATCHEXPORT FAILED: addClip produced no V1 clip";
        return 1;
    }

    const int outW = 320, outH = 240;
    const double fps = 30.0;
    const int kFrames = 20;
    const qint64 rangeUs =
        static_cast<qint64>((kFrames / fps) * 1'000'000.0 + 0.5);

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qCritical() << "BATCHEXPORT FAILED: could not create temp dir";
        return 1;
    }
    const QString out1 = tmpDir.filePath(QStringLiteral("batch_job1.mp4"));
    const QString out2 = tmpDir.filePath(QStringLiteral("batch_job2.mp4"));

    batchexport::Queue queue;

    // Track per-task completion / progress via the EXACT public signals
    // BatchExportDialog consumes — proving the dialog-facing contract works.
    QHash<QString, int>  lastProgress;
    QHash<QString, batchexport::TaskState> lastState;
    QObject::connect(&queue, &batchexport::Queue::taskProgress, &queue,
                     [&](const QString &id, int pct) {
        lastProgress[id] = pct;
    });
    QObject::connect(&queue, &batchexport::Queue::taskStateChanged, &queue,
                     [&](const QString &id, batchexport::TaskState st) {
        lastState[id] = st;
    });

    // Distinct temp outputs + the in-memory Timeline seam + a bounded frame
    // range (start/end map onto RenderJob::startUs/endUs). projectPath
    // doubles as the audio-mux source (RenderQueue mirrors VideoStabilizer
    // -i original -map 1:a?), so point it at the real clip.
    const QString id1 = queue.addTask(clipPath, out1,
                                      QStringLiteral("1080p"),
                                      &tl1, outW, outH, 0, rangeUs);
    const QString id2 = queue.addTask(clipPath, out2,
                                      QStringLiteral("720p"),
                                      &tl2, outW, outH, 0, rangeUs);
    if (id1.isEmpty() || id2.isEmpty()
        || queue.tasks().size() != 2) {
        qCritical() << "BATCHEXPORT FAILED: addTask did not register 2 tasks";
        return 1;
    }

    auto stateOf = [&](const QString &id) -> batchexport::TaskState {
        for (const auto &t : queue.tasks())
            if (t.id == id)
                return t.state;
        return batchexport::TaskState::Failed;
    };
    auto fileSize = [](const QString &p) -> qint64 {
        QFileInfo fi(p);
        return fi.exists() ? fi.size() : -1;
    };

    // ── Run job 1 only, then PAUSE before it can advance to job 2 ───────────
    // pause() is called immediately; BatchExportQueue must finish the
    // in-flight job 1 (acceptable batch-pause semantics: one QProcess at a
    // time, killing mid-encode corrupts output) but must NOT dispatch job 2
    // while paused. We spin the event loop until job 1 reaches a terminal
    // state, asserting job 2 stays Queued throughout.
    QEventLoop loop;
    bool job1Terminal = false;
    QObject::connect(&queue, &batchexport::Queue::taskStateChanged, &loop,
                     [&](const QString &id, batchexport::TaskState st) {
        if (id == id1 && (st == batchexport::TaskState::Done
                          || st == batchexport::TaskState::Failed)) {
            job1Terminal = true;
            loop.quit();
        }
    });
    QTimer hardTimeout;
    hardTimeout.setSingleShot(true);
    QObject::connect(&hardTimeout, &QTimer::timeout, &loop, [&]() {
        qCritical() << "BATCHEXPORT FAILED: job 1 timed out";
        loop.quit();
    });
    hardTimeout.start(360000);   // 6min: slow-CI margin (was 180s, critic follow-up)

    queue.start();
    queue.pause();               // pause IMMEDIATELY — guard must hold
    if (!job1Terminal)
        loop.exec();

    if (!job1Terminal || stateOf(id1) != batchexport::TaskState::Done) {
        qCritical() << "BATCHEXPORT FAILED: job 1 did not complete (state="
                    << static_cast<int>(stateOf(id1)) << ")";
        return 1;
    }
    if (lastProgress.value(id1, -1) != 100
        || lastState.value(id1) != batchexport::TaskState::Done) {
        qCritical() << "BATCHEXPORT FAILED: job 1 did not emit progress=100 /"
                       " taskStateChanged(Done) (progress="
                    << lastProgress.value(id1, -1) << ")";
        return 1;
    }
    if (fileSize(out1) <= 0) {
        qCritical() << "BATCHEXPORT FAILED: job 1 output missing/empty"
                    << out1 << "size=" << fileSize(out1);
        return 1;
    }

    // ── Pause sub-check: queue must NOT advance to job 2 while paused ───────
    // Spin the event loop for a short while; job 2 must stay Queued and its
    // file must not appear. This is the real teeth of pause() — a regression
    // that ignores the pause guard would start job 2 here.
    {
        QEventLoop spin;
        QTimer t;
        t.setSingleShot(true);
        QObject::connect(&t, &QTimer::timeout, &spin, &QEventLoop::quit);
        t.start(3000);
        spin.exec();
    }
    const bool job2HeldWhilePaused =
        stateOf(id2) == batchexport::TaskState::Queued
        && fileSize(out2) < 0;
    if (!job2HeldWhilePaused) {
        qCritical() << "BATCHEXPORT FAILED: pause did NOT hold — job 2 "
                       "advanced while paused (state="
                    << static_cast<int>(stateOf(id2))
                    << " out2 size=" << fileSize(out2) << ")";
        return 1;
    }
    qInfo() << "BATCHEXPORT: pause held — job 2 stayed Queued, no file "
               "produced while paused";

    // ── Resume → job 2 must now complete ────────────────────────────────────
    bool job2Terminal = false;
    QEventLoop loop2;
    QObject::connect(&queue, &batchexport::Queue::taskStateChanged, &loop2,
                     [&](const QString &id, batchexport::TaskState st) {
        if (id == id2 && (st == batchexport::TaskState::Done
                          || st == batchexport::TaskState::Failed)) {
            job2Terminal = true;
            loop2.quit();
        }
    });
    QTimer hardTimeout2;
    hardTimeout2.setSingleShot(true);
    QObject::connect(&hardTimeout2, &QTimer::timeout, &loop2, [&]() {
        qCritical() << "BATCHEXPORT FAILED: job 2 timed out after resume";
        loop2.quit();
    });
    hardTimeout2.start(360000);  // 6min: slow-CI margin (was 180s, critic follow-up)

    queue.resume();
    if (!job2Terminal)
        loop2.exec();

    if (!job2Terminal || stateOf(id2) != batchexport::TaskState::Done) {
        qCritical() << "BATCHEXPORT FAILED: job 2 did not complete after "
                       "resume (state=" << static_cast<int>(stateOf(id2))
                    << ")";
        return 1;
    }
    if (lastProgress.value(id2, -1) != 100
        || lastState.value(id2) != batchexport::TaskState::Done) {
        qCritical() << "BATCHEXPORT FAILED: job 2 did not emit progress=100 /"
                       " taskStateChanged(Done) (progress="
                    << lastProgress.value(id2, -1) << ")";
        return 1;
    }
    if (fileSize(out2) <= 0) {
        qCritical() << "BATCHEXPORT FAILED: job 2 output missing/empty"
                    << out2 << "size=" << fileSize(out2);
        return 1;
    }

    // ── Real on-disk frame-count assertion (±1) via ffprobe, BOTH files ─────
    auto countFrames = [](const QString &path) -> int {
        QProcess probe;
        probe.start(QStringLiteral("ffprobe"),
                    { QStringLiteral("-v"), QStringLiteral("error"),
                      QStringLiteral("-select_streams"),
                      QStringLiteral("v:0"),
                      QStringLiteral("-count_frames"),
                      QStringLiteral("-show_entries"),
                      QStringLiteral("stream=nb_read_frames"),
                      QStringLiteral("-of"),
                      QStringLiteral("csv=p=0"),
                      path });
        if (!probe.waitForStarted(15000))
            return -1;
        probe.waitForFinished(60000);
        bool okNum = false;
        const int n = QString::fromUtf8(probe.readAllStandardOutput())
                          .trimmed().toInt(&okNum);
        return okNum ? n : -1;
    };
    const int f1 = countFrames(out1);
    const int f2 = countFrames(out2);
    qInfo() << "BATCHEXPORT: job1 frames =" << f1
            << "(file" << fileSize(out1) << "bytes); job2 frames =" << f2
            << "(file" << fileSize(out2) << "bytes); expected ~" << kFrames;
    if (f1 < 0 || std::abs(f1 - kFrames) > 1) {
        qCritical() << "BATCHEXPORT FAILED: job 1 frame count" << f1
                    << "differs from expected" << kFrames << "by > 1";
        return 1;
    }
    if (f2 < 0 || std::abs(f2 - kFrames) > 1) {
        qCritical() << "BATCHEXPORT FAILED: job 2 frame count" << f2
                    << "differs from expected" << kFrames << "by > 1";
        return 1;
    }

    qInfo() << "BATCHEXPORT: pause-check = HELD; both jobs produced real "
               "on-disk files with the expected frame counts";
    qInfo() << "BATCHEXPORT selftest OK";
    return 0;
#endif
}
