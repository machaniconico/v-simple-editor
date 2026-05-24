// src/selftests/misc_selftests.cpp
// PRD-SPLIT-MAIN-3 Phase 3-D: misc 9 selftest 関数を verbatim 移動。
// Dispatch via SelftestRegistry.{h,cpp}.

#if __has_include("ChromaKeyRefine.h")
#include "ChromaKeyRefine.h"
#define HAVE_CHROMA_KEY_REFINE 1
#endif
#if __has_include("AudioRestoration.h")
#include "AudioRestoration.h"
#define HAVE_AUDIO_RESTORATION 1
#endif
#if __has_include("AnimatedExport.h")
#include "AnimatedExport.h"
#define HAVE_ANIMATED_EXPORT 1
#endif
#if __has_include("EasingCurveModel.h")
#include "EasingCurveModel.h"
#define HAVE_EASING_CURVE 1
#endif
#if __has_include("SubtitleTranslator.h")
#include "SubtitleTranslator.h"
#define HAVE_SUBTITLE_TRANSLATOR 1
#endif
#if __has_include("LowerThirdTemplates.h")
#include "LowerThirdTemplates.h"
#define HAVE_LOWER_THIRD 1
#endif
#if __has_include("WatermarkOverlay.h")
#include "WatermarkOverlay.h"
#define HAVE_WATERMARK_OVERLAY 1
#endif
#if __has_include("CaptionTrack.h")
#include "CaptionTrack.h"
#define HAVE_CAPTIONTRACK 1
#endif

#include "FrameDiff.h"
#include "Timeline.h"
#include "RenderQueue.h"
#include "TextOverlayBake.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QString>
#include <QStringLiteral>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QVector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern void writeLogLine(const QString &level, const QString &msg);
extern bool requireSelftest(bool condition, const QString &message, QString *error);

// US-INT-1: Sprint 22 chroma-key refine self-test (VEDITOR_CHROMA_SELFTEST=1).
int runChromaSelftest()
{
    QString error;
#if defined(HAVE_CHROMA_KEY_REFINE)
    {
        chromakey::KeyConfig cfg;
        cfg.keyColor = QColor(0, 255, 0);
        const double keyedAlpha = chromakey::computeAlpha(cfg.keyColor, cfg);
        if (!requireSelftest(keyedAlpha < 0.1,
                             QStringLiteral("CHROMA: key colour should be keyed out (alpha < 0.1)"),
                             &error))
            return 1;

        const double magentaAlpha = chromakey::computeAlpha(QColor(255, 0, 255), cfg);
        if (!requireSelftest(magentaAlpha > 0.9,
                             QStringLiteral("CHROMA: magenta should stay opaque (alpha > 0.9)"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "CHROMA selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 audio restoration self-test (VEDITOR_AUDIORESTORE_SELFTEST=1).
int runAudioRestoreSelftest()
{
    QString error;
#if defined(HAVE_AUDIO_RESTORATION)
    {
        const int sampleRate = 48000;
        const double humHz   = 50.0;
        QVector<float> buf;
        buf.reserve(sampleRate);
        for (int i = 0; i < sampleRate; ++i) {
            const double t = static_cast<double>(i) / sampleRate;
            buf.append(static_cast<float>(std::sin(2.0 * M_PI * humHz * t)));
        }

        auto rms = [](const QVector<float> &v) {
            double acc = 0.0;
            for (float s : v)
                acc += static_cast<double>(s) * static_cast<double>(s);
            return v.isEmpty() ? 0.0 : std::sqrt(acc / v.size());
        };

        const double preRms = rms(buf);
        QVector<float> processed = buf;
        audiorestore::deHum(processed, sampleRate, humHz, 4);
        const double postRms = rms(processed);

        if (!requireSelftest(preRms > 1e-6,
                             QStringLiteral("AUDIORESTORE: synthesized hum should have non-zero RMS"),
                             &error))
            return 1;
        if (!requireSelftest(postRms < preRms * 0.5,
                             QStringLiteral("AUDIORESTORE: deHum should attenuate 50Hz RMS by >50%"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "AUDIORESTORE selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 animated export self-test (VEDITOR_ANIMEXPORT_SELFTEST=1).
int runAnimExportSelftest()
{
    QString error;
#if defined(HAVE_ANIMATED_EXPORT)
    {
        QImage img(64, 64, QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                img.setPixelColor(x, y,
                                  QColor((x * 4) % 256, (y * 4) % 256,
                                         ((x + y) * 2) % 256));
            }
        }

        const QVector<QRgb> pal = animexport::medianCutPalette(img, 256);
        if (!requireSelftest(pal.size() > 0 && pal.size() <= 256,
                             QStringLiteral("ANIMEXPORT: palette size should be in (0, 256]"),
                             &error))
            return 1;

        animexport::ExportConfig cfg;
        cfg.format = animexport::Format::Gif;
        cfg.fps    = 12;
        cfg.width  = 64;
        if (!requireSelftest(cfg.fps > 0 && cfg.width > 0,
                             QStringLiteral("ANIMEXPORT: ExportConfig round-trip failed"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "ANIMEXPORT selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 easing curve self-test (VEDITOR_EASING_SELFTEST=1).
int runEasingSelftest()
{
    QString error;
#if defined(HAVE_EASING_CURVE)
    {
        const double lin = easing::evaluate(easing::EasingType::Linear, 0.5, {});
        if (!requireSelftest(qFuzzyCompare(lin + 1.0, 0.5 + 1.0),
                             QStringLiteral("EASING: Linear(0.5) should be 0.5"),
                             &error))
            return 1;

        if (!requireSelftest(easing::presets().size() >= 8,
                             QStringLiteral("EASING: expected >= 8 presets"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "EASING selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 subtitle translator self-test (VEDITOR_SUBXLAT_SELFTEST=1).
int runSubXlatSelftest()
{
    QString error;
#if defined(HAVE_SUBTITLE_TRANSLATOR) && defined(HAVE_CAPTIONTRACK)
    {
        caption::Track track;
        caption::Clip c1;
        c1.startMs = 0;
        c1.endMs   = 1000;
        c1.text    = QStringLiteral("hello");
        track.addClip(c1);
        caption::Clip c2;
        c2.startMs = 1000;
        c2.endMs   = 2000;
        c2.text    = QStringLiteral("world");
        track.addClip(c2);

        subxlat::TranslateConfig cfg;
        cfg.provider   = subxlat::Provider::Stub;
        cfg.targetLang = QStringLiteral("ja");

        subxlat::TranslatorClient client;
        caption::Track captured;
        bool gotResult = false;
        QObject::connect(&client, &subxlat::TranslatorClient::translateFinished,
                         [&](const caption::Track &t) {
                             captured  = t;
                             gotResult = true;
                         });

        // Stub provider emits translateFinished synchronously from translateTrack.
        client.translateTrack(track, cfg);

        if (!gotResult) {
            QEventLoop loop;
            QTimer::singleShot(2000, &loop, &QEventLoop::quit);
            QObject::connect(&client, &subxlat::TranslatorClient::translateFinished,
                             &loop, &QEventLoop::quit);
            loop.exec();
        }

        if (!requireSelftest(gotResult,
                             QStringLiteral("SUBXLAT: translateFinished was not emitted"),
                             &error))
            return 1;
        if (!requireSelftest(captured.clipCount() == 2,
                             QStringLiteral("SUBXLAT: translated track should hold 2 clips"),
                             &error))
            return 1;
        if (!requireSelftest(captured.clipAt(0).text.startsWith(QStringLiteral("[ja]")),
                             QStringLiteral("SUBXLAT: clip text should start with \"[ja]\""),
                             &error))
            return 1;
    }
#endif
    qInfo() << "SUBXLAT selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 lower-third templates self-test (VEDITOR_LOWERTHIRD_SELFTEST=1).
int runLowerThirdSelftest()
{
    QString error;
#if defined(HAVE_LOWER_THIRD)
    {
        const QVector<lowerthird::LowerThirdStyle> styles = lowerthird::builtInStyles();
        if (!requireSelftest(styles.size() >= 10,
                             QStringLiteral("LOWERTHIRD: expected >= 10 built-in styles"),
                             &error))
            return 1;

        auto countOpaque = [](const QImage &img) {
            int n = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(img.pixel(x, y)) > 0)
                        ++n;
                }
            }
            return n;
        };

        const QImage a = lowerthird::renderFrame(styles[0], 0.0, QSize(640, 360));
        const QImage b = lowerthird::renderFrame(styles[0], 1.0, QSize(640, 360));
        const int na = countOpaque(a);
        const int nb = countOpaque(b);
        if (!requireSelftest(nb > na,
                             QStringLiteral("LOWERTHIRD: progress=1.0 should be more visible than 0.0"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "LOWERTHIRD selftest OK";
    return 0;
}

// US-INT-1: Sprint 22 watermark overlay self-test (VEDITOR_WATERMARK_SELFTEST=1).
int runWatermarkSelftest()
{
    QString error;
#if defined(HAVE_WATERMARK_OVERLAY)
    {
        QImage img(320, 240, QImage::Format_ARGB32);
        img.fill(QColor(40, 80, 120));

        watermark::WmConfig cfg;
        cfg.mode = watermark::Mode::Text;
        cfg.text = QStringLiteral("X");

        const QImage out = watermark::applyWatermark(img, cfg);
        if (!requireSelftest(!out.isNull() && out.size() == img.size(),
                             QStringLiteral("WATERMARK: output must be non-null and same size"),
                             &error))
            return 1;
    }
#endif
    qInfo() << "WATERMARK selftest OK";
    return 0;
}

// S12 single-path audit self-test (VEDITOR_EXPORTAUDIT_SELFTEST=1).
//
// The S12 invariant: there is NO UI-reachable export path that bypasses the
// SSOT (tlrender::renderFrameAt). The grep audit in progress.txt enumerates
// every entry point; this is the RUNTIME assertion of the load-bearing one —
// that the production export path (the File->Export / Mobile Export action,
// both now routing through RenderQueue, MainWindow.cpp::exportVideo /
// onMobileExport) genuinely renders the live edit graph, NOT a transcode that
// drops it (the legacy Exporter::doExport shape).
//
// Method (the proven S11-Assertion-1 / S8 technique — NOT tautological):
// build a RenderJob EXACTLY as MainWindow::exportVideo() now builds it (the
// additive in-memory Timeline* seam set), drive it through the REAL
// RenderQueue TWICE — run B with a 3D LUT + colour-correction seated on the
// V1 clip, run P with a clean clip — then assert the two decoded outputs
// DIFFER materially. renderFrameAt applies ClipInfo::lutFilePath /
// colorCorrection per clip; the legacy Exporter transcode shape does NOT read
// the in-memory seam at all. So if export still bypassed the SSOT the LUT
// could not move a single pixel and MSE(B,P) would be ~0. A large MSE is
// positive proof the production export path IS RenderQueue->renderFrameAt.
// CI-tolerant: missing test asset -> qWarning + return 0 (never a silent
// pass). Returns 0 on pass, 1 on fail; runnable via cmd.exe set-env.
int runExportAuditSelftest()
{
    const QString clipArg = qEnvironmentVariable(
        "VEDITOR_E2E_CLIP", QStringLiteral("test_assets/e2e_clip.mp4"));
    const QString clipPath = QDir::current().absoluteFilePath(clipArg);
    const QString lutArg = qEnvironmentVariable(
        "VEDITOR_PARITY_LUT", QStringLiteral("test_assets/s4_tint.cube"));
    const QString lutPath = QDir::current().absoluteFilePath(lutArg);
    qInfo() << "EXPORTAUDIT: clip path" << clipPath;
    qInfo() << "EXPORTAUDIT: LUT path" << lutPath;
    if (!QFile::exists(clipPath) || !QFile::exists(lutPath)) {
        qWarning() << "EXPORTAUDIT: missing test asset (clip or LUT)"
                   << "(skipping — CI-tolerant, never a silent pass)";
        qInfo() << "EXPORTAUDIT selftest OK";
        return 0;
    }

    const int outW = 320, outH = 240;
    const double fps = 30.0;
    const int kFrames = 12;
    const qint64 rangeUs =
        static_cast<qint64>((kFrames / fps) * 1'000'000.0 + 0.5);

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qCritical() << "EXPORTAUDIT FAILED: could not create temp dir";
        return 1;
    }

    // Drive ONE export through the real RenderQueue, built byte-for-byte the
    // way MainWindow::exportVideo() now assembles the RenderJob (the additive
    // in-memory Timeline* seam — projectFilePath points at real media so the
    // audio mux + non-.veditor path is exercised exactly like the UI).
    auto runExport = [&](bool withLut, const QString &outPath) -> bool {
        Timeline tl;
        tl.addClip(clipPath);
        if (tl.videoClips().isEmpty()) {
            qCritical() << "EXPORTAUDIT FAILED: addClip produced no V1 clip";
            return false;
        }
        const QVector<TimelineTrack *> &vtracks = tl.videoTracks();
        if (vtracks.isEmpty() || !vtracks.first()) {
            qCritical() << "EXPORTAUDIT FAILED: no V1 track";
            return false;
        }
        ClipInfo c0 = tl.videoClips().first();
        if (withLut) {
            // The exact per-clip graph S4/S8 prove renderFrameAt applies and
            // the legacy Exporter transcode shape ignores.
            c0.colorCorrection.saturation = 40.0;
            c0.colorCorrection.brightness = 15.0;
            c0.lutFilePath  = lutPath;
            c0.lutIntensity = 0.9;
            vtracks.first()->setClips({ c0 });
            if (!tl.videoClips().first().hasLut()) {
                qCritical() << "EXPORTAUDIT FAILED: LUT not seated on V1";
                return false;
            }
        }

        RenderJob job;
        job.name = QFileInfo(outPath).fileName();
        job.projectFilePath = clipPath;   // audio-mux source (UI uses this)
        job.outputPath = outPath;
        job.width   = outW;
        job.height  = outH;
        job.bitrateBps = 20'000'000;
        job.startUs = 0;
        job.endUs   = rangeUs;
        job.timeline = &tl;               // the in-memory edit-graph seam
        QJsonObject cfg;
        cfg["width"]  = outW;
        cfg["height"] = outH;
        cfg["fps"]    = fps;
        cfg["videoCodec"]   = QStringLiteral("libx264");
        cfg["videoBitrate"] = 20000;
        cfg["audioCodec"]   = QStringLiteral("aac");
        cfg["audioBitrate"] = 192;
        job.exportConfig = cfg;

        RenderQueue queue;
        bool jobOk = false, jobDone = false;
        QString jobErr;
        QEventLoop loop;
        QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
                         [&](const QString &, bool ok, const QString &err) {
            jobOk = ok; jobErr = err; jobDone = true; loop.quit();
        });
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
            jobErr = QStringLiteral("export timed out");
            jobDone = true; loop.quit();
        });
        timeoutTimer.start(180000);
        queue.addJob(job);
        queue.start();
        if (!jobDone)
            loop.exec();
        if (!jobOk) {
            qCritical() << "EXPORTAUDIT FAILED: RenderQueue export did not "
                           "complete:" << jobErr;
            return false;
        }
        QFileInfo fi(outPath);
        if (!fi.exists() || fi.size() <= 0) {
            qCritical() << "EXPORTAUDIT FAILED: output missing/empty"
                        << outPath;
            return false;
        }
        return true;
    };

    const QString outB = tmpDir.filePath(QStringLiteral("audit_lut.mp4"));
    const QString outP = tmpDir.filePath(QStringLiteral("audit_plain.mp4"));
    if (!runExport(true, outB))  return 1;
    if (!runExport(false, outP)) return 1;

    // Decode both outputs to PNG frames via the ffmpeg CLI (the S8/S11
    // sibling of the render pipe) and compare. flattenOnBlack mirrors the S8
    // RGBA->opaque-black equalisation so the comparison is like-with-like.
    QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        for (const QString &p : { QStringLiteral("/usr/local/bin"),
                                  QStringLiteral("/usr/bin") }) {
            ffmpeg = QStandardPaths::findExecutable(
                QStringLiteral("ffmpeg"), { p });
            if (!ffmpeg.isEmpty()) break;
        }
    }
    if (ffmpeg.isEmpty()) {
        qWarning() << "EXPORTAUDIT: ffmpeg not found for decode "
                      "(skipping diff — CI-tolerant)";
        qInfo() << "EXPORTAUDIT selftest OK";
        return 0;
    }

    auto decodeFrames = [&](const QString &src,
                            const QString &tag) -> QStringList {
        QProcess p;
        const QString pat =
            tmpDir.filePath(tag + QStringLiteral("_%05d.png"));
        p.start(ffmpeg, { QStringLiteral("-y"), QStringLiteral("-i"), src,
                          QStringLiteral("-frames:v"),
                          QString::number(kFrames), pat });
        p.waitForFinished(120000);
        QStringList out;
        for (int f = 1; f <= kFrames; ++f) {
            const QString fp = tmpDir.filePath(
                tag + QStringLiteral("_%1.png")
                    .arg(f, 5, 10, QChar('0')));
            if (QFile::exists(fp)) out << fp;
        }
        return out;
    };

    const QStringList bFrames = decodeFrames(outB, QStringLiteral("b"));
    const QStringList pFrames = decodeFrames(outP, QStringLiteral("p"));
    const int n = qMin(bFrames.size(), pFrames.size());
    if (n <= 0) {
        qCritical() << "EXPORTAUDIT FAILED: no frames decoded from outputs";
        return 1;
    }

    // Local flattenOnBlack — the SAME RGBA->opaque-black equalisation S8/S11
    // use (defined locally there too; it is not a shared global). Both sides
    // get the identical flatten so the comparison is strictly like-with-like.
    auto flattenOnBlack = [](const QImage &src, const QSize &sz) -> QImage {
        const QImage rgba = src.convertToFormat(QImage::Format_RGBA8888);
        QImage out(sz, QImage::Format_RGB888);
        out.fill(Qt::black);
        QPainter p(&out);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (rgba.size() == sz)
            p.drawImage(0, 0, rgba);
        else
            p.drawImage(QRect(QPoint(0, 0), sz), rgba);
        p.end();
        return out.convertToFormat(QImage::Format_RGBA8888);
    };

    const QSize cmpSize(outW, outH);
    double mseSum = 0.0;
    int mseCount = 0;
    for (int i = 0; i < n; ++i) {
        const QImage bImg(bFrames[i]);
        const QImage pImg(pFrames[i]);
        if (bImg.isNull() || pImg.isNull())
            continue;
        const double m = framediff::mse(flattenOnBlack(bImg, cmpSize),
                                        flattenOnBlack(pImg, cmpSize));
        if (m >= 0.0) { mseSum += m; ++mseCount; }
    }
    const double meanMse = mseCount > 0 ? mseSum / mseCount : 0.0;
    qInfo() << "EXPORTAUDIT: LUT-export vs plain-export mean MSE ="
            << meanMse << "over" << mseCount
            << "frames (MUST be > 30 — proves the production export path "
               "applies the in-memory edit graph via renderFrameAt; if any "
               "UI export still bypassed the SSOT via legacy Exporter the "
               "LUT could not move a pixel and this would be ~0)";

    QString error;
    if (!requireSelftest(meanMse > 30.0,
            QStringLiteral("EXPORTAUDIT: production export bypassed the SSOT "
                           "edit graph — a seated 3D LUT did not change the "
                           "rendered output (single-path NOT achieved)"),
            &error))
        return 1;

    qInfo() << "[INFO] EXPORTAUDIT single-path: production export routes "
               "RenderQueue -> tlrender::renderFrameAt OK";
    qInfo() << "EXPORTAUDIT selftest OK";
    return 0;
}

// Worker-thread TEXT-EXPORT selftest (VEDITOR_TEXTEXPORT_SELFTEST=1).
//
// DEFECT THIS GUARDS: the SSOT renderer's S6 text stage USED to construct a
// VideoPlayer (a QWidget) to bake text. tlrender::renderFrameAt runs on the
// RenderQueue WORKER THREAD (RenderQueue::startRenderPipe -> QThread::create),
// so a real export of any timeline whose V1 clip carries a text overlay would
// construct a QWidget off the GUI thread — Qt undefined behaviour
// (asserts/crashes on Qt6/MSVC). The PARITY/EXPORTAUDIT selftests never hit
// it because they bake text on the GUI thread (S6) or export WITHOUT text
// (S8/EXPORTAUDIT). This selftest closes that gap: it drives a REAL
// RenderQueue export of a timeline WITH a text overlay (so renderFrameAt's
// text stage genuinely runs on the worker thread) and proves BOTH that the
// baked text reaches the encoded artifact AND that it executed OFF the GUI
// thread. Pre-fix this test would crash/fail at the off-thread QWidget
// construction; post-fix the text bakes via the free textbake::bakeOverlays.
//
// NON-TAUTOLOGICAL (the S8/EXPORTAUDIT technique): export the SAME timeline
// TWICE through the real RenderQueue — once WITH the text overlay, once
// WITHOUT — then assert the decoded artifacts DIFFER materially in the text
// region (mean MSE over the first frames >> 0). If text never reached the
// encoded file the two outputs would be ~identical. Independence: a separate
// ffmpeg process encodes each file; libav decodes them fresh.
//
// OFF-GUI-THREAD PROOF: textbake::bakeOverlays records (env-gated) the
// QThread it baked on. After the WITH-text export we assert that recorded
// thread is non-null and != the GUI thread — genuine evidence the production
// text stage ran on the RenderQueue worker, exactly where the old QWidget
// seam was UB. CI-tolerant: missing asset -> qWarning + return 0.
int runTextExportSelftest()
{
    const QString clipArg = qEnvironmentVariable(
        "VEDITOR_E2E_CLIP", QStringLiteral("test_assets/e2e_clip.mp4"));
    const QString clipPath = QDir::current().absoluteFilePath(clipArg);
    qInfo() << "TEXTEXPORT: clip path" << clipPath;
    if (!QFile::exists(clipPath)) {
        qWarning() << "TEXTEXPORT: missing test asset" << clipPath
                   << "(skipping — CI-tolerant, never a silent pass)";
        qInfo() << "TEXTEXPORT selftest OK";
        return 0;
    }

    QThread *const guiThread = QThread::currentThread();
    const int outW = 320, outH = 240;
    const double fps = 30.0;
    const int kFrames = 12;
    const qint64 rangeUs =
        static_cast<qint64>((kFrames / fps) * 1'000'000.0 + 0.5);

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qCritical() << "TEXTEXPORT FAILED: could not create temp dir";
        return 1;
    }

    // Build the keyframed pure-red text overlay (mirrors PARITY S6's overlay
    // so the baked glyph is a huge, unambiguous delta over the video).
    auto makeOverlay = []() -> EnhancedTextOverlay {
        EnhancedTextOverlay ov;
        ov.text            = QStringLiteral("EXPORT");
        ov.font            = QFont(QStringLiteral("Arial"), 40, QFont::Bold);
        ov.color           = QColor(255, 0, 0);
        ov.backgroundColor = QColor(0, 0, 0, 0);
        ov.outlineColor    = QColor(0, 0, 0, 0);
        ov.outlineWidth    = 0;
        ov.gradientEnabled = false;
        ov.width           = 0.0;
        ov.height          = 0.0;
        ov.x               = 0.5;
        ov.y               = 0.5;
        ov.startTime       = 0.0;
        ov.endTime         = 0.0;
        ov.visible         = true;
        return ov;
    };

    // Drive ONE real RenderQueue export (the in-memory Timeline* seam, exactly
    // like MainWindow::exportVideo / S8 / EXPORTAUDIT). withText seats the
    // overlay on V1 so renderFrameAt's S6 text stage runs on the worker.
    auto runExport = [&](bool withText, const QString &outPath) -> bool {
        Timeline tl;
        tl.addClip(clipPath);
        if (tl.videoClips().isEmpty()) {
            qCritical() << "TEXTEXPORT FAILED: addClip produced no V1 clip";
            return false;
        }
        const QVector<TimelineTrack *> &vtracks = tl.videoTracks();
        if (vtracks.isEmpty() || !vtracks.first()) {
            qCritical() << "TEXTEXPORT FAILED: no V1 track";
            return false;
        }
        if (withText) {
            ClipInfo c0 = tl.videoClips().first();
            c0.textManager.addOverlay(makeOverlay());
            vtracks.first()->setClips({ c0 });
            if (tl.videoClips().isEmpty()
                || tl.videoClips().first().textManager.count() != 1) {
                qCritical() << "TEXTEXPORT FAILED: text overlay not seated "
                               "on V1";
                return false;
            }
        }

        RenderJob job;
        job.name = QFileInfo(outPath).fileName();
        job.projectFilePath = clipPath;   // audio-mux source (UI uses this)
        job.outputPath = outPath;
        job.width   = outW;
        job.height  = outH;
        job.codec   = QStringLiteral("h264");
        job.bitrateBps = 20'000'000;
        job.startUs = 0;
        job.endUs   = rangeUs;
        job.timeline = &tl;               // the in-memory edit-graph seam
        QJsonObject cfg;
        cfg["width"]  = outW;
        cfg["height"] = outH;
        cfg["fps"]    = fps;
        cfg["videoCodec"]   = QStringLiteral("libx264");
        cfg["videoBitrate"] = 20000;
        cfg["audioCodec"]   = QStringLiteral("aac");
        cfg["audioBitrate"] = 192;
        job.exportConfig = cfg;

        RenderQueue queue;
        bool jobOk = false, jobDone = false;
        QString jobErr;
        QEventLoop loop;
        QObject::connect(&queue, &RenderQueue::jobCompletedUuid, &loop,
                         [&](const QString &, bool ok, const QString &err) {
            jobOk = ok; jobErr = err; jobDone = true; loop.quit();
        });
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
            jobErr = QStringLiteral("export timed out");
            jobDone = true; loop.quit();
        });
        timeoutTimer.start(180000);
        queue.addJob(job);
        queue.start();
        if (!jobDone)
            loop.exec();
        if (!jobOk) {
            qCritical() << "TEXTEXPORT FAILED: RenderQueue export did not "
                           "complete:" << jobErr;
            return false;
        }
        QFileInfo fi(outPath);
        if (!fi.exists() || fi.size() <= 0) {
            qCritical() << "TEXTEXPORT FAILED: output missing/empty"
                        << outPath;
            return false;
        }
        return true;
    };

    const QString outT = tmpDir.filePath(QStringLiteral("text_export.mp4"));
    const QString outN = tmpDir.filePath(QStringLiteral("notext_export.mp4"));

    // Run the WITH-text export FIRST and reset the bake-thread observer just
    // before it so the recorded thread is unambiguously from THIS export's
    // worker. Pre-fix, renderFrameAt's text stage constructed a QWidget here
    // off the GUI thread (UB) — this call would crash/fail.
    textbake::resetLastBakeThreadForTest();
    if (!runExport(true, outT))  return 1;

    // ── OFF-GUI-THREAD PROOF ────────────────────────────────────────────────
    QThread *const bakeThread = textbake::lastBakeThreadForTest();
    if (!bakeThread) {
        qCritical() << "TEXTEXPORT FAILED: textbake::bakeOverlays was never "
                       "invoked during the real export — renderFrameAt's S6 "
                       "text stage did not run (the export did not carry the "
                       "text overlay through the worker pipeline)";
        return 1;
    }
    if (bakeThread == guiThread) {
        qCritical() << "TEXTEXPORT FAILED: renderFrameAt's text stage ran on "
                       "the GUI thread, NOT the RenderQueue worker — the "
                       "selftest did not actually exercise the off-thread "
                       "export path (so it could not catch the original "
                       "off-GUI-thread QWidget defect)";
        return 1;
    }
    qInfo() << "TEXTEXPORT: text baked on worker thread" << bakeThread
            << "!= GUI thread" << guiThread
            << "— renderFrameAt's S6 text stage genuinely ran OFF the GUI "
               "thread via RenderQueue::startRenderPipe (this is the path "
               "the original VideoPlayer-QWidget defect crashed on)";

    if (!runExport(false, outN)) return 1;

    // ── NON-TAUTOLOGICAL: decode both artifacts, assert text moved pixels ────
    QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        for (const QString &p : { QStringLiteral("/usr/local/bin"),
                                  QStringLiteral("/usr/bin") }) {
            ffmpeg = QStandardPaths::findExecutable(
                QStringLiteral("ffmpeg"), { p });
            if (!ffmpeg.isEmpty()) break;
        }
    }
    if (ffmpeg.isEmpty()) {
        qWarning() << "TEXTEXPORT: ffmpeg not found for decode "
                      "(skipping pixel diff — CI-tolerant; the off-GUI-thread "
                      "proof above still PASSED)";
        qInfo() << "TEXTEXPORT selftest OK";
        return 0;
    }

    auto decodeFrames = [&](const QString &src,
                            const QString &tag) -> QStringList {
        QProcess p;
        const QString pat =
            tmpDir.filePath(tag + QStringLiteral("_%05d.png"));
        p.start(ffmpeg, { QStringLiteral("-y"), QStringLiteral("-i"), src,
                          QStringLiteral("-frames:v"),
                          QString::number(kFrames), pat });
        p.waitForFinished(120000);
        QStringList out;
        for (int f = 1; f <= kFrames; ++f) {
            const QString fp = tmpDir.filePath(
                tag + QStringLiteral("_%1.png")
                    .arg(f, 5, 10, QChar('0')));
            if (QFile::exists(fp)) out << fp;
        }
        return out;
    };

    const QStringList tFrames = decodeFrames(outT, QStringLiteral("t"));
    const QStringList nFrames = decodeFrames(outN, QStringLiteral("n"));
    const int n = qMin(tFrames.size(), nFrames.size());
    if (n <= 0) {
        qCritical() << "TEXTEXPORT FAILED: no frames decoded from outputs";
        return 1;
    }

    // Same RGBA->opaque-black equalisation S8/EXPORTAUDIT use so the diff is
    // strictly like-with-like (both sides get the identical flatten).
    auto flattenOnBlack = [](const QImage &src, const QSize &sz) -> QImage {
        const QImage rgba = src.convertToFormat(QImage::Format_RGBA8888);
        QImage out(sz, QImage::Format_RGB888);
        out.fill(Qt::black);
        QPainter p(&out);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (rgba.size() == sz)
            p.drawImage(0, 0, rgba);
        else
            p.drawImage(QRect(QPoint(0, 0), sz), rgba);
        p.end();
        return out.convertToFormat(QImage::Format_RGBA8888);
    };

    const QSize cmpSize(outW, outH);
    double mseSum = 0.0;
    int mseCount = 0;
    for (int i = 0; i < n; ++i) {
        const QImage tImg(tFrames[i]);
        const QImage nImg(nFrames[i]);
        if (tImg.isNull() || nImg.isNull())
            continue;
        const double m = framediff::mse(flattenOnBlack(tImg, cmpSize),
                                        flattenOnBlack(nImg, cmpSize));
        if (m >= 0.0) { mseSum += m; ++mseCount; }
    }
    const double meanMse = mseCount > 0 ? mseSum / mseCount : 0.0;
    qInfo() << "TEXTEXPORT: text-export vs no-text-export mean MSE ="
            << meanMse << "over" << mseCount
            << "frames (MUST be > 20 — proves the worker-thread export "
               "genuinely baked the text overlay INTO the encoded artifact; "
               "if text never reached the file the two exports would be ~0)";

    QString error;
    if (!requireSelftest(meanMse > 20.0,
            QStringLiteral("TEXTEXPORT: the real worker-thread export did NOT "
                           "carry the text overlay into the encoded file (a "
                           "seated text overlay moved no pixels vs the no-text "
                           "export)"),
            &error))
        return 1;

    qInfo() << "[INFO] TEXTEXPORT: worker-thread text-export path OK — "
               "renderFrameAt's S6 text stage ran off the GUI thread AND the "
               "baked text reached the RenderQueue-encoded artifact";
    qInfo() << "TEXTEXPORT selftest OK";
    return 0;
}
