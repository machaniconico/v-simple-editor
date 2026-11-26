#include "../ExternalGenerator.h"
#include "../ExternalGeneratorEdge.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "SelftestGate.h"

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

namespace {

bool imagesHaveDifferentPixels(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return true;
    const QImage ar = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage br = b.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < ar.height(); y += 4) {
        for (int x = 0; x < ar.width(); x += 4) {
            if (ar.pixel(x, y) != br.pixel(x, y))
                return true;
        }
    }
    return false;
}

} // namespace

int runExtGeneratorSelftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        std::fprintf(stderr, "ext-generator: failed to create temp dir\n");
        return 1;
    }

    // Save/restore trust store around test
    const QStringList savedTrusted = extgen::trustedFolders();
    const QByteArray savedBrowserEnv = qgetenv("VEDITOR_GENERATOR_BROWSER");
    const QByteArray savedPluginEnv = qgetenv("VEDITOR_GENERATOR_PLUGIN_DIR");

    auto restoreEnvAndTrust = [&]() {
        extgen::clearTestRenderers();
        extgen::clearMemoryCache();
        extgen::clearTrustedFolders();
        for (const QString &f : savedTrusted)
            extgen::trustFolder(f);
        if (savedBrowserEnv.isNull()) qunsetenv("VEDITOR_GENERATOR_BROWSER");
        else qputenv("VEDITOR_GENERATOR_BROWSER", savedBrowserEnv);
        if (savedPluginEnv.isNull()) qunsetenv("VEDITOR_GENERATOR_PLUGIN_DIR");
        else qputenv("VEDITOR_GENERATOR_PLUGIN_DIR", savedPluginEnv);
    };

    const QString cacheDir = tmpDir.filePath(QStringLiteral("cache"));
    const QString scriptDir = tmpDir.filePath(QStringLiteral("scripts"));
    QDir().mkpath(scriptDir);
    const QString fakeScript = QDir(scriptDir).filePath(QStringLiteral("demo.js"));
    {
        QFile f(fakeScript);
        if (f.open(QIODevice::WriteOnly))
            f.write("// dummy script\n");
    }

    // G1: URI encode/decode round-trip & .vseproj ProjectFile::save/load persistence.
    {
        extgen::GeneratorSpec s;
        s.renderer = QStringLiteral("edge");
        s.entryPath = fakeScript;
        s.fps = 24.0;
        s.params = QJsonObject{{QStringLiteral("title"), QStringLiteral("日本語タイトル")},
                               {QStringLiteral("count"), 42}};
        s.cacheDir = cacheDir;

        const QString uri = extgen::buildGeneratorUri(s);
        const extgen::GeneratorSpec parsed = extgen::parseGeneratorUri(uri);

        ClipInfo clip;
        clip.filePath = uri;
        clip.displayName = QStringLiteral("GenClip");
        clip.duration = 4.0;
        clip.inPoint = 0.0;
        clip.outPoint = 4.0;
        ProjectData pd;
        pd.videoTracks = {{clip}};
        const QString g1Proj = tmpDir.filePath(QStringLiteral("g1_roundtrip.vseproj"));
        const bool savedOk = ProjectFile::save(g1Proj, pd);
        ProjectData loadedPd;
        const bool loadedOk = savedOk && ProjectFile::load(g1Proj, loadedPd);
        const QString loadedPath = (loadedOk && !loadedPd.videoTracks.isEmpty() && !loadedPd.videoTracks[0].isEmpty())
            ? loadedPd.videoTracks[0][0].filePath : QString();

        const bool ok = extgen::isExternalGeneratorFilePath(uri)
            && !extgen::isExternalGeneratorFilePath(QStringLiteral("C:/video.mp4"))
            && parsed.renderer == s.renderer
            && parsed.entryPath == s.entryPath
            && std::abs(parsed.fps - 24.0) < 1e-6
            && parsed.params.value(QStringLiteral("title")).toString() == QStringLiteral("日本語タイトル")
            && parsed.params.value(QStringLiteral("count")).toInt() == 42
            && parsed.cacheDir == cacheDir
            && loadedOk && loadedPath == uri;
        rec(1, ok, QStringLiteral("renderer=%1 fps=%2 titleOk=%3 persist=%4")
                       .arg(parsed.renderer).arg(parsed.fps, 0, 'f', 1)
                       .arg(parsed.params.value(QStringLiteral("title")).toString() == QStringLiteral("日本語タイトル") ? 1 : 0)
                       .arg(loadedPath == uri ? 1 : 0));
    }

    // G2: R3 Trust gate blocks untrusted folder, allows after trustFolder(), persists in QSettings.
    {
        extgen::clearTrustedFolders();
        int invoked = 0;
        extgen::registerTestRenderer(QStringLiteral("mock-trust"),
            [&](const extgen::GeneratorSpec &, int, QSize sz) {
                ++invoked;
                QImage img(sz, QImage::Format_RGBA8888);
                img.fill(QColor(10, 220, 80, 255));
                return img;
            });

        extgen::GeneratorSpec spec;
        spec.renderer = QStringLiteral("mock-trust");
        spec.entryPath = fakeScript;
        spec.cacheDir = cacheDir;

        const bool beforeTrust = extgen::isFolderTrusted(fakeScript);
        const auto blocked = extgen::renderGeneratorFrame(spec, 0, QSize(160, 90), false);
        const int invAfterBlock = invoked;
        extgen::trustFolder(fakeScript);
        const bool afterTrust = extgen::isFolderTrusted(fakeScript);
        const auto allowed = extgen::renderGeneratorFrame(spec, 0, QSize(160, 90), false);
        extgen::revokeFolderTrust(fakeScript);
        const bool afterRevoke = extgen::isFolderTrusted(fakeScript);

        const bool ok = !beforeTrust && blocked.untrusted && !blocked.ok && invAfterBlock == 0
            && afterTrust && allowed.ok && !allowed.untrusted && invoked == 1
            && allowed.image.pixelColor(80, 45).green() > 200
            && !afterRevoke;
        rec(2, ok, QStringLiteral("before=%1 blocked=%2 invBlock=%3 invAllow=%4 allowed=%5 revoke=%6")
                       .arg(beforeTrust ? 1 : 0).arg(blocked.untrusted ? 1 : 0)
                       .arg(invAfterBlock).arg(invoked)
                       .arg(allowed.ok ? 1 : 0).arg(afterRevoke ? 1 : 0));
    }

    // G3: Dummy & test renderer render deterministic frames through TimelineFrameRenderer::renderFrameAt
    // including alpha compositing over a V1 background.
    {
        extgen::trustFolder(fakeScript);
        extgen::registerTestRenderer(QStringLiteral("mock-alpha"),
            [](const extgen::GeneratorSpec &, int frameIdx, QSize sz) {
                QImage img(sz, QImage::Format_RGBA8888);
                img.fill(Qt::transparent);
                // Top-left quadrant opaque green (varies with frameIdx), rest transparent
                for (int y = 0; y < sz.height() / 2; ++y)
                    for (int x = 0; x < sz.width() / 2; ++x)
                        img.setPixelColor(x, y, QColor(0, qBound(60, 100 + frameIdx * 10, 255), 0, 255));
                return img;
            });

        Timeline tl;
        // Canonical V1-wins order: V1 (index 0) is painted frontmost over V2 (index 1).
        // Put transparent alpha overlay on V1 and opaque dummy background on V2.
        extgen::GeneratorSpec ovSpec;
        ovSpec.renderer = QStringLiteral("mock-alpha");
        ovSpec.entryPath = fakeScript;
        ovSpec.fps = 30.0;
        ovSpec.cacheDir = cacheDir;
        ClipInfo v1;
        v1.filePath = extgen::buildGeneratorUri(ovSpec);
        v1.duration = 2.0;
        v1.outPoint = 2.0;
        tl.videoTracks()[0]->addClip(v1);

        tl.addVideoTrack(false);
        extgen::GeneratorSpec bgSpec;
        bgSpec.renderer = QStringLiteral("dummy");
        bgSpec.fps = 30.0;
        bgSpec.cacheDir = cacheDir;
        bgSpec.params = QJsonObject{{QStringLiteral("title"), QStringLiteral("BG")}};
        ClipInfo v2;
        v2.filePath = extgen::buildGeneratorUri(bgSpec);
        v2.duration = 2.0;
        v2.outPoint = 2.0;
        tl.videoTracks()[1]->addClip(v2);

        const QImage f0 = tlrender::renderFrameAt(&tl, 0, QSize(160, 90));
        const QImage f10 = tlrender::renderFrameAt(&tl, 333334, QSize(160, 90)); // ~frame 10
        const QColor tlPx0 = f0.pixelColor(20, 15);
        const QColor tlPx10 = f10.pixelColor(20, 15);
        const QColor brPx0 = f0.pixelColor(130, 75); // shows V1 BG (blue=150)

        const bool ok = !f0.isNull() && !f10.isNull()
            && tlPx0.green() >= 95 && tlPx0.red() < 20
            && tlPx10.green() > tlPx0.green()
            && brPx0.blue() >= 120
            && imagesHaveDifferentPixels(f0, f10);
        rec(3, ok, QStringLiteral("f0G=%1 f10G=%2 bgB=%3")
                       .arg(tlPx0.green()).arg(tlPx10.green()).arg(brPx0.blue()));
    }

    // G4: Preview scaled cache hit vs full-resolution export render.
    {
        extgen::clearMemoryCache();
        QVector<QSize> requestedSizes;
        extgen::registerTestRenderer(QStringLiteral("mock-cache"),
            [&](const extgen::GeneratorSpec &, int, QSize sz) {
                requestedSizes.append(sz);
                QImage img(sz, QImage::Format_RGBA8888);
                img.fill(QColor(40, 90, 200, 255));
                return img;
            });

        extgen::GeneratorSpec spec;
        spec.renderer = QStringLiteral("mock-cache");
        spec.entryPath = fakeScript;
        spec.cacheDir = tmpDir.filePath(QStringLiteral("cache_g4"));

        const QSize fullSize(1280, 720);
        const auto prev1 = extgen::renderGeneratorFrame(spec, 3, fullSize, /*isPreview=*/true);
        const auto prev2 = extgen::renderGeneratorFrame(spec, 3, fullSize, /*isPreview=*/true);
        const auto exp1 = extgen::renderGeneratorFrame(spec, 3, fullSize, /*isPreview=*/false);

        const bool ok = prev1.ok && !prev1.fromCache
            && prev2.ok && prev2.fromCache
            && exp1.ok && !exp1.fromCache
            && requestedSizes.size() == 2
            && requestedSizes[0].width() <= 640 && requestedSizes[0].height() <= 360
            && requestedSizes[1] == fullSize
            && prev1.image.size() == fullSize && exp1.image.size() == fullSize;
        rec(4, ok, QStringLiteral("calls=%1 prevSz=%2x%3 expSz=%4x%5 hit=%6")
                       .arg(requestedSizes.size())
                       .arg(requestedSizes.value(0).width()).arg(requestedSizes.value(0).height())
                       .arg(requestedSizes.value(1).width()).arg(requestedSizes.value(1).height())
                       .arg(prev2.fromCache ? 1 : 0));
    }

    // G5: Missing-renderer fallback shows last cached frame when available,
    // and clean "ジェネレータ未導入" placeholder when uncached.
    {
        extgen::GeneratorSpec missingSpec;
        missingSpec.renderer = QStringLiteral("nonexistent-plugin-xyz");
        missingSpec.entryPath = fakeScript;
        missingSpec.cacheDir = tmpDir.filePath(QStringLiteral("cache_g5"));

        // Uncached -> missing placeholder
        const auto uncached = extgen::renderGeneratorFrame(missingSpec, 0, QSize(320, 180), false);
        // Seed a cached last frame -> missing renderer falls back to cached frame + badge
        QImage seeded(320, 180, QImage::Format_RGBA8888);
        seeded.fill(QColor(20, 210, 60, 255));
        extgen::storeFrameInCache(missingSpec, 99, QSize(320, 180), seeded);
        const auto fallbackCached = extgen::renderGeneratorFrame(missingSpec, 5, QSize(320, 180), false);

        const QColor uncachedCenter = uncached.image.pixelColor(160, 90);
        const QColor fallbackBottomRight = fallbackCached.image.pixelColor(280, 150);
        const bool ok = uncached.missingRenderer && !uncached.fromCache && !uncached.image.isNull()
            && uncachedCenter.blue() < 100
            && fallbackCached.missingRenderer && fallbackCached.fromCache
            && fallbackBottomRight.green() > 180;
        rec(5, ok, QStringLiteral("uncachedMiss=%1 cachedFallback=%2 brGreen=%3")
                       .arg(uncached.missingRenderer ? 1 : 0)
                       .arg(fallbackCached.fromCache ? 1 : 0)
                       .arg(fallbackBottomRight.green()));
    }

    // G6: VGEN binary RGBA framing round-trip + corruption rejection.
    {
        QImage src(32, 24, QImage::Format_RGBA8888);
        src.fill(QColor(12, 34, 56, 200));
        src.setPixelColor(7, 9, QColor(250, 128, 64, 255));
        const QByteArray framed = extgen::encodeVgenFrame(src);
        QImage decoded;
        const bool goodOk = extgen::decodeVgenFrame(framed, &decoded);
        QImage badDecoded;
        const bool badMagicRejected = !extgen::decodeVgenFrame(QByteArrayLiteral("NOPE123456789012"), &badDecoded);
        const bool truncRejected = !extgen::decodeVgenFrame(framed.left(20), &badDecoded);

        const bool ok = goodOk && decoded.size() == src.size()
            && decoded.pixelColor(7, 9) == QColor(250, 128, 64, 255)
            && decoded.pixelColor(0, 0) == QColor(12, 34, 56, 200)
            && badMagicRejected && truncRejected;
        rec(6, ok, QStringLiteral("framedBytes=%1 good=%2 badRej=%3 truncRej=%4")
                       .arg(framed.size()).arg(goodOk ? 1 : 0)
                       .arg(badMagicRejected ? 1 : 0).arg(truncRejected ? 1 : 0));
    }

    // G7: Bake external generator clip to standalone MP4 + 1-step Undo restores veditor://extgen URI.
    {
        Timeline tl;
        extgen::GeneratorSpec spec;
        spec.renderer = QStringLiteral("dummy");
        spec.fps = 15.0;
        spec.cacheDir = tmpDir.filePath(QStringLiteral("cache_g7"));
        spec.params = QJsonObject{{QStringLiteral("title"), QStringLiteral("BakeTest")}};
        const QString origUri = extgen::buildGeneratorUri(spec);

        ClipInfo c;
        c.filePath = origUri;
        c.displayName = QStringLiteral("BakeMe");
        c.duration = 0.4;
        c.inPoint = 0.0;
        c.outPoint = 0.4;
        tl.videoTracks()[0]->addClip(c);
        tl.saveUndoState(QStringLiteral("Initial"));

        const QString bakedMp4 = tmpDir.filePath(QStringLiteral("baked.mp4"));
        const auto res = extgen::bakeExternalGeneratorClip(
            &tl, 0, 0, bakedMp4, QSize(160, 90), 15.0, /*forceSoftwareCodec=*/true);
        const QString pathAfter = tl.videoTracks()[0]->clips()[0].filePath;
        const qint64 bakedSize = QFileInfo(bakedMp4).size();
        const QImage renderedBaked = tlrender::renderFrameAt(&tl, 0, QSize(160, 90));

        tl.undo();
        const QString pathUndo = tl.videoTracks()[0]->clips()[0].filePath;

        const bool ok = res.ok && res.framesRendered >= 6
            && bakedSize > 500
            && pathAfter == bakedMp4
            && !renderedBaked.isNull()
            && pathUndo == origUri;
        rec(7, ok, QStringLiteral("bakedOk=%1 frames=%2 bytes=%3 undoRestored=%4")
                       .arg(res.ok ? 1 : 0).arg(res.framesRendered)
                       .arg(bakedSize).arg(pathUndo == origUri ? 1 : 0));
    }

    // G8: Plan C Edge CDP helpers: VEDITOR_GENERATOR_BROWSER override, WebSocket RFC6455 framing,
    // and setFrame expression generation (no real Edge process launched).
    {
        const QString fakeBrowser = tmpDir.filePath(QStringLiteral("fake_edge.exe"));
        {
            QFile f(fakeBrowser);
            if (f.open(QIODevice::WriteOnly))
                f.write("stub");
        }
        qputenv("VEDITOR_GENERATOR_BROWSER", fakeBrowser.toUtf8());
        const QString detected = extgen::edge::findBrowserExecutable();

        const QByteArray msg = QByteArrayLiteral("{\"id\":1,\"method\":\"Page.captureScreenshot\"}");
        const QByteArray wsFrame = extgen::edge::buildClientTextFrame(msg, 0xCAFEBABEu);
        QByteArray parsedPayload;
        int opcode = -1;
        int consumed = 0;
        const bool wsOk = extgen::edge::parseServerFrame(wsFrame, &parsedPayload, &opcode, &consumed);

        const QString expr = extgen::edge::buildSetFrameExpression(
            12, 30.0, QJsonObject{{QStringLiteral("title"), QStringLiteral("EdgeTest")}});

        const bool ok = (QFileInfo(detected).absoluteFilePath() == QFileInfo(fakeBrowser).absoluteFilePath())
            && wsOk && opcode == 1 && consumed == wsFrame.size() && parsedPayload == msg
            && expr.contains(QStringLiteral("window.setFrame"))
            && expr.contains(QStringLiteral("EdgeTest"));
        rec(8, ok, QStringLiteral("browserOverride=%1 wsOk=%2 exprOk=%3")
                       .arg(detected.endsWith(QStringLiteral("fake_edge.exe")) ? 1 : 0)
                       .arg(wsOk ? 1 : 0)
                       .arg(expr.contains(QStringLiteral("window.setFrame")) ? 1 : 0));
    }

    // G9: Plan B QuickJS plugin manifest discovery & ExternalGeneratorDialog controls.
    {
        const QString pluginRoot = tmpDir.filePath(QStringLiteral("plugins"));
        const QString qjsDir = QDir(pluginRoot).filePath(QStringLiteral("quickjs"));
        QDir().mkpath(qjsDir);
        const QString fakeExe = QDir(qjsDir).filePath(QStringLiteral("vse-quickjs-gen.exe"));
        {
            QFile ef(fakeExe);
            if (ef.open(QIODevice::WriteOnly))
                ef.write("stub");
        }
        {
            QFile mf(QDir(qjsDir).filePath(QStringLiteral("generator.json")));
            if (mf.open(QIODevice::WriteOnly))
                mf.write(R"({"id":"quickjs","name":"QuickJS Script Renderer","executable":"vse-quickjs-gen.exe","protocol":"stdio-rgba","extensions":[".js"]})");
        }
        qputenv("VEDITOR_GENERATOR_PLUGIN_DIR", pluginRoot.toUtf8());

        const extgen::PluginManifest found = extgen::findPlugin(QStringLiteral("quickjs"));
        extgen::ExternalGeneratorDialog dlg;
        auto *combo = dlg.findChild<QComboBox *>(QStringLiteral("extgenRendererCombo"));
        auto *fpsSpin = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("extgenFpsSpin"));
        auto *durSpin = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("extgenDurationSpin"));

        const bool ok = found.valid && found.id == QStringLiteral("quickjs")
            && found.protocol == QStringLiteral("stdio-rgba")
            && extgen::isRendererAvailable(QStringLiteral("quickjs"))
            && combo && fpsSpin && durSpin && combo->count() >= 3;
        rec(9, ok, QStringLiteral("qjsValid=%1 proto=%2 comboCnt=%3")
                       .arg(found.valid ? 1 : 0).arg(found.protocol)
                       .arg(combo ? combo->count() : -1));
    }

    // G10: Plan A generated .vseproj JSON loads via ProjectFile::load and renders via renderFrameAt.
    {
        extgen::GeneratorSpec s;
        s.renderer = QStringLiteral("dummy");
        s.fps = 30.0;
        s.cacheDir = tmpDir.filePath(QStringLiteral("cache_g10"));
        s.params = QJsonObject{{QStringLiteral("title"), QStringLiteral("Plan A Scene")}};

        QJsonObject clipObj;
        clipObj[QStringLiteral("filePath")] = extgen::buildGeneratorUri(s);
        clipObj[QStringLiteral("displayName")] = QStringLiteral("Plan A Clip");
        clipObj[QStringLiteral("duration")] = 2.0;
        clipObj[QStringLiteral("inPoint")] = 0.0;
        clipObj[QStringLiteral("outPoint")] = 2.0;
        clipObj[QStringLiteral("speed")] = 1.0;
        clipObj[QStringLiteral("volume")] = 1.0;
        clipObj[QStringLiteral("opacity")] = 1.0;

        QJsonObject cfg;
        cfg[QStringLiteral("width")] = 160;
        cfg[QStringLiteral("height")] = 90;
        cfg[QStringLiteral("fps")] = 30;

        QJsonArray track0;
        track0.append(clipObj);
        QJsonArray vTracks;
        vTracks.append(track0);
        QJsonArray aTracks;
        aTracks.append(QJsonArray());

        QJsonObject proj;
        proj[QStringLiteral("version")] = 2;
        proj[QStringLiteral("config")] = cfg;
        proj[QStringLiteral("videoTracks")] = vTracks;
        proj[QStringLiteral("audioTracks")] = aTracks;

        const QString projPath = tmpDir.filePath(QStringLiteral("plan_a.vseproj"));
        {
            QFile pf(projPath);
            if (pf.open(QIODevice::WriteOnly))
                pf.write(QJsonDocument(proj).toJson(QJsonDocument::Indented));
        }

        ProjectData loaded;
        const bool loadOk = ProjectFile::load(projPath, loaded);
        Timeline tl;
        if (loadOk && !loaded.videoTracks.isEmpty())
            tl.restoreFromProject(loaded.videoTracks, loaded.audioTracks, 0.0, -1.0, -1.0, 100);
        const QImage frame = tlrender::renderFrameAt(&tl, 100000, QSize(160, 90));

        const bool ok = loadOk
            && loaded.videoTracks.size() == 1
            && loaded.videoTracks[0].size() == 1
            && extgen::isExternalGeneratorFilePath(loaded.videoTracks[0][0].filePath)
            && !frame.isNull()
            && frame.pixelColor(10, 10).blue() >= 120;
        rec(10, ok, QStringLiteral("loadOk=%1 tracks=%2 rendered=%3")
                        .arg(loadOk ? 1 : 0).arg(loaded.videoTracks.size())
                        .arg(!frame.isNull() ? 1 : 0));
    }

    restoreEnvAndTrust();
    return tally.finish("ext-generator");
}
