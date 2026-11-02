// src/selftests/keyframe_anim_parity_selftest.cpp
// Render-time S1 motion/opacity keyframe parity gates.

#include "../Camera3D.h"
#include "../clipanim/ClipAnim.h"
#include "../ClipGeometry.h"
#include "../EffectClipboard.h"
#include "../EffectControlsPanel.h"
#include "../FrameDiff.h"
#include "../Keyframe.h"
#include "../LayerStyle.h"
#include "../MainWindow.h"
#include "../MotionSectionWidget.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"
#include "../mcp/McpEditorTools.h"
#include "../mcp/McpProtocol.h"
#include "../mcp/McpToolRegistry.h"
#include "SelftestRegistry.h"

#include <cmath>
#include <cstdio>

#include <QColor>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QToolButton>
#include <QStringList>
#include <QTemporaryDir>

// GLPreview.cpp (US-1509): the layer3D GLPreview's projection uses for the clip active at a time.
Layer3DTransform previewLayer3DForSelftest(const Timeline *timeline, double timelineSec);

namespace {

constexpr int kClipW = 64;
constexpr int kClipH = 64;
constexpr int kFps = 10;
constexpr int kFrameCount = 20;
constexpr double kEps = 1e-6;

QImage makePatternFrame()
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const int r = (x * 5 + y * 2) & 255;
            const int g = (x * 3 + y * 7) & 255;
            const int b = (x * 11 + y * 13 + 23) & 255;
            frame.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    return frame;
}

bool writeSyntheticClip(const QString &path, QString *error)
{
    libavcore::EncodeRequest req;
    req.width = kClipW;
    req.height = kClipH;
    req.fps = kFps;
    req.fpsNum = kFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 600000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (auto err = encoder.open(req)) {
        if (error)
            *error = QString::fromStdString(*err);
        return false;
    }

    const QImage frame = makePatternFrame();
    for (int i = 0; i < kFrameCount; ++i) {
        if (!encoder.pushFrame(frame, i)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrame failed at frame %1").arg(i);
            return false;
        }
    }
    if (auto err = encoder.finalize()) {
        if (error)
            *error = QString::fromStdString(*err);
        return false;
    }
    return true;
}

ClipInfo makeClip(const QString &path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("keyframe_anim_parity");
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
}

void setTimelineClip(Timeline &timeline, const ClipInfo &clip)
{
    timeline.videoTracks().first()->setClips(QVector<ClipInfo>{clip});
}

bool near(double a, double b, double eps = kEps)
{
    return std::abs(a - b) <= eps;
}

QString imageDigest(const QImage &image)
{
    if (image.isNull())
        return QStringLiteral("null");
    const QByteArray bytes(reinterpret_cast<const char *>(image.constBits()),
                           static_cast<int>(image.sizeInBytes()));
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(12));
}

bool sameBytes(const QImage &a, const QImage &b)
{
    return !a.isNull() && !b.isNull() && a.size() == b.size()
        && a.format() == b.format() && a == b;
}

int maxAlpha(const QImage &image)
{
    if (image.isNull())
        return -1;
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    int best = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *row = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x)
            best = qMax(best, int(row[x * 4 + 3]));
    }
    return best;
}

} // namespace

int runKeyframeAnimParitySelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok, const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::fprintf(stderr, "[keyframe-anim-parity] %s G%d %s%s%s\n",
                             ok ? "PASS" : "FAIL",
                             gate,
                             name,
                             detail.isEmpty() ? "" : " - ",
                             detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    QTemporaryDir tempDir;
    QString clipPath;
    if (!tempDir.isValid()) {
        check(0, "temp dir", false, QStringLiteral("QTemporaryDir invalid"));
        return 1;
    }
    clipPath = tempDir.filePath(QStringLiteral("keyframe_anim_src.mp4"));
    QString mediaError;
    if (!writeSyntheticClip(clipPath, &mediaError)) {
        check(0, "synthetic media", false, mediaError);
        return 1;
    }

    // G1: no motion keyframes returns static transform + static opacity.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.videoScale = 1.35;
        clip.videoDx = 0.17;
        clip.videoDy = -0.09;
        clip.rotation2DDegrees = 12.5;
        clip.opacity = 0.42;

        const clipgeom::ClipTransform t =
            clipanim::effectiveTransformAt(clip, 0.5);
        const double opacity =
            clipanim::effectiveOpacityAt(clip, 0.5, clip.opacity);
        check(1, "no keyframes == static values",
              near(t.videoScale, clip.videoScale)
                  && near(t.videoDx, clip.videoDx)
                  && near(t.videoDy, clip.videoDy)
                  && near(t.rotationDeg, clip.rotation2DDegrees)
                  && near(opacity, clip.opacity));
    }

    // G2: renderFrameAt keyframe-free output matches the legacy static oracle.
    {
        const QSize outSize(80, 60);
        const double sourceSec = 0.5;
        const qint64 usec = 500000;
        const QVector<clipgeom::ClipTransform> transforms = {
            clipgeom::ClipTransform{0.75, 0.10, -0.08, 0.0},
            clipgeom::ClipTransform{1.30, -0.12, 0.07, 15.0},
        };
        bool ok = true;
        QString detail;
        const QImage native =
            tlrender::detail::decodeClipFrameNativeForTest(clipPath, sourceSec);
        if (native.isNull()) {
            ok = false;
            detail = QStringLiteral("decodeClipFrameNativeForTest returned null");
        }
        for (int i = 0; ok && i < transforms.size(); ++i) {
            ClipInfo clip = makeClip(clipPath);
            clip.videoScale = transforms[i].videoScale;
            clip.videoDx = transforms[i].videoDx;
            clip.videoDy = transforms[i].videoDy;
            clip.rotation2DDegrees = transforms[i].rotationDeg;
            Timeline timeline;
            setTimelineClip(timeline, clip);
            const QImage rendered = tlrender::renderFrameAt(&timeline, usec, outSize);
            const QImage expected =
                clipgeom::renderLayer(native, transforms[i], outSize, /*smooth=*/true);
            const double mse = framediff::mse(rendered, expected);
            if (mse != 0.0) {
                ok = false;
                detail = QStringLiteral("variant %1 MSE=%2").arg(i).arg(mse, 0, 'g', 12);
            }
        }
        check(2, "keyframe-free renderFrameAt == static oracle", ok, detail);
    }

    // G3: motion.scale interpolation is delegated to KeyframeManager::valueAt.
    {
        ClipInfo clip = makeClip(clipPath);
        KeyframeTrack scale(QStringLiteral("motion.scale"), 1.0);
        scale.addKeyframe(0.0, 1.0);
        scale.addKeyframe(1.0, 2.0);
        clip.keyframes.addTrack(scale);
        const clipgeom::ClipTransform t =
            clipanim::effectiveTransformAt(clip, 0.5);
        check(3, "motion.scale linear interpolation at t=0.5",
              near(t.videoScale, 1.5, 1e-5),
              QStringLiteral("scale=%1").arg(t.videoScale, 0, 'g', 12));
    }

    // G4: renderFrameAt changes over time when motion.scale is keyed.
    {
        ClipInfo clip = makeClip(clipPath);
        KeyframeTrack scale(QStringLiteral("motion.scale"), 1.0);
        scale.addKeyframe(0.0, 1.0);
        scale.addKeyframe(1.0, 2.0);
        clip.keyframes.addTrack(scale);
        Timeline timeline;
        setTimelineClip(timeline, clip);
        const QSize outSize(80, 60);
        const QImage a = tlrender::renderFrameAt(&timeline, 0, outSize);
        const QImage b = tlrender::renderFrameAt(&timeline, 1'000'000, outSize);
        const double mse = framediff::mse(a, b);
        check(4, "renderFrameAt output changes with scale keyframes",
              mse > 0.0,
              QStringLiteral("MSE=%1").arg(mse, 0, 'g', 12));
    }

    // G5: motion.opacity evaluation changes over time.
    {
        ClipInfo clip = makeClip(clipPath);
        KeyframeTrack opacity(QStringLiteral("motion.opacity"), 1.0);
        opacity.addKeyframe(0.0, 1.0);
        opacity.addKeyframe(1.0, 0.2);
        clip.keyframes.addTrack(opacity);
        const double at0 = clipanim::effectiveOpacityAt(clip, 0.0, clip.opacity);
        const double atHalf = clipanim::effectiveOpacityAt(clip, 0.5, clip.opacity);
        const double at1 = clipanim::effectiveOpacityAt(clip, 1.0, clip.opacity);
        check(5, "motion.opacity keyframe evaluation changes over time",
              near(at0, 1.0, 1e-5) && near(atHalf, 0.6, 1e-5) && near(at1, 0.2, 1e-5),
              QStringLiteral("t0=%1 t0.5=%2 t1=%3")
                  .arg(at0, 0, 'g', 12)
                  .arg(atHalf, 0, 'g', 12)
                  .arg(at1, 0, 'g', 12));
    }

    // G6: discrete effect parameters clamp easing overshoot into schema range.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.effects.append(VideoEffect::createMirror(0));

        const QString trackName = QStringLiteral("effect.0.mode");
        KeyframeTrack mirrorMode(trackName, 0.0);
        mirrorMode.addKeyframe(0.0, 0.0, KeyframePoint::ElasticOut);
        mirrorMode.addKeyframe(1.0, 3.0);
        clip.keyframes.addTrack(mirrorMode);

        const double sampleTime = 0.15;
        const double rawValue = clip.keyframes.valueAt(trackName, sampleTime, 0.0);
        const QVector<VideoEffect> effects =
            clipanim::effectiveEffectsAt(clip, sampleTime);
        const double effectiveMode =
            effects.isEmpty() ? -1.0 : effects.first().param1;
        check(6, "ElasticOut enum/int effect keyframes clamp to valid range",
              rawValue > 3.0
                  && effects.size() == 1
                  && effectiveMode >= 0.0
                  && effectiveMode <= 3.0
                  && near(effectiveMode, 3.0, 1e-9),
              QStringLiteral("raw=%1 effective=%2")
                  .arg(rawValue, 0, 'g', 12)
                  .arg(effectiveMode, 0, 'g', 12));
    }

    // G7 (US-1703): the single-layer export shortcut keeps a lone V1 clip's
    // effective opacity. The same clip rendered (a) alone on V1 and (b) on V2
    // over an empty V1 must produce the same bytes for a static opacity, a
    // keyed opacity, a transform.opacity expression, and a layer style +
    // opacity. HEAD returned (a) opaque (max alpha 255) and FAILed here.
    // Sub-check (regression pin): at opacity 1.0 (a) == (b), and the (a)
    // digest is printed so builds before / after the fix can be compared.
    {
        const QSize outSize(80, 60);
        const qint64 usec = 500000;
        auto renderBoth = [&](const ClipInfo &clip, QImage *alone, QImage *onV2) {
            Timeline v1Timeline;
            setTimelineClip(v1Timeline, clip);
            *alone = tlrender::renderFrameAt(&v1Timeline, usec, outSize);
            Timeline v2Timeline;
            v2Timeline.addVideoTrack(false);
            v2Timeline.videoTracks()[1]->setClips(QVector<ClipInfo>{clip});
            *onV2 = tlrender::renderFrameAt(&v2Timeline, usec, outSize);
        };

        bool ok = true;
        QStringList parts;

        // Step 0 item 11: (a) vs (b) at opacity 1.0 decides the reference
        // (plain clip, and the scaled + stroked clip of the last variant).
        // Measured at HEAD: the plain clip is byte-identical, the stroked one
        // has identical pixels but the shortcut hands back layerstyle's
        // ARGB32_Premultiplied image (format 6) instead of RGBA8888 (17). The
        // opacity 1.0 shortcut is left untouched, so that pin compares pixels
        // after converting (a); (b) stays the reference for opacity < 1.
        {
            ClipInfo styledOpaque = makeClip(clipPath);
            styledOpaque.videoScale = 0.75;
            styledOpaque.layerStyle.strokeEnabled = true;
            styledOpaque.layerStyle.strokeColor = QColor(255, 40, 40);
            styledOpaque.layerStyle.strokeWidth = 3.0;
            const QVector<QPair<const char *, ClipInfo>> opaque = {
                {"opacity1.0", makeClip(clipPath)},
                {"opacity1.0+layerStyle", styledOpaque},
            };
            for (const auto &o : opaque) {
                QImage alone, onV2;
                renderBoth(o.second, &alone, &onV2);
                const bool same = sameBytes(alone, onV2);
                const bool samePixels = !alone.isNull()
                    && sameBytes(alone.convertToFormat(QImage::Format_RGBA8888), onV2);
                parts << QStringLiteral("%1 a==b:%2 pixels:%9 a=%3 b=%4 aMaxAlpha=%5 fmt=%6/%7 mse=%8")
                             .arg(QString::fromLatin1(o.first))
                             .arg(same ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(imageDigest(alone), imageDigest(onV2))
                             .arg(maxAlpha(alone))
                             .arg(int(alone.format()))
                             .arg(int(onV2.format()))
                             .arg(framediff::mse(alone, onV2), 0, 'g', 12)
                             .arg(samePixels ? QStringLiteral("yes") : QStringLiteral("no"));
                ok = ok && samePixels;
            }
        }

        struct Variant {
            const char *name;
            ClipInfo clip;
        };
        QVector<Variant> variants;
        {
            ClipInfo clip = makeClip(clipPath);
            clip.opacity = 0.5;
            variants.append({"static0.5", clip});
        }
        {
            ClipInfo clip = makeClip(clipPath);
            KeyframeTrack opacity(QStringLiteral("motion.opacity"), 1.0);
            opacity.addKeyframe(0.0, 1.0);
            opacity.addKeyframe(1.0, 0.2);
            clip.keyframes.addTrack(opacity);
            variants.append({"keyed0.6", clip});
        }
        {
            ClipInfo clip = makeClip(clipPath);
            clip.expressions.setExpression(QStringLiteral("transform.opacity"),
                                           QStringLiteral("time * 100"));
            variants.append({"expr0.5", clip});
        }
        // The layer-style variant is scaled down so the stroke has an alpha
        // edge to draw on (a full-canvas layer has none and the style would
        // be a no-op); the unstyled twin below proves the style is visible.
        ClipInfo unstyledSmall = makeClip(clipPath);
        unstyledSmall.opacity = 0.5;
        unstyledSmall.videoScale = 0.75;
        {
            ClipInfo clip = unstyledSmall;
            clip.layerStyle.strokeEnabled = true;
            clip.layerStyle.strokeColor = QColor(255, 40, 40);
            clip.layerStyle.strokeWidth = 3.0;
            variants.append({"layerStyle+static0.5", clip});
        }
        for (const Variant &v : variants) {
            QImage alone, onV2;
            renderBoth(v.clip, &alone, &onV2);
            const bool same = sameBytes(alone, onV2);
            const int aAlpha = maxAlpha(alone);
            const int bAlpha = maxAlpha(onV2);
            // Sanity band on the reference itself: (b) must be translucent.
            const bool translucent = bAlpha > 0 && bAlpha < 255;
            parts << QStringLiteral("%1 a==b:%2 a=%3 b=%4 aMaxAlpha=%5 bMaxAlpha=%6 mse=%7")
                         .arg(QString::fromLatin1(v.name))
                         .arg(same ? QStringLiteral("yes") : QStringLiteral("no"))
                         .arg(imageDigest(alone), imageDigest(onV2))
                         .arg(aAlpha)
                         .arg(bAlpha)
                         .arg(framediff::mse(alone, onV2), 0, 'g', 12);
            ok = ok && same && translucent;
        }
        {
            QImage styledAlone, styledOnV2, plainAlone, plainOnV2;
            renderBoth(variants.last().clip, &styledAlone, &styledOnV2);
            renderBoth(unstyledSmall, &plainAlone, &plainOnV2);
            const bool styleVisible = !sameBytes(styledOnV2, plainOnV2);
            parts << QStringLiteral("layerStyle visible on b:%1 styled=%2 plain=%3")
                         .arg(styleVisible ? QStringLiteral("yes") : QStringLiteral("no"))
                         .arg(imageDigest(styledOnV2), imageDigest(plainOnV2));
            ok = ok && styleVisible;
        }
        check(7, "lone V1 effective opacity == same clip on V2 over empty V1",
              ok, parts.join(QStringLiteral("; ")));
    }

    // G8 (US-1814): the 3D layer world x/y through the motion UI surfaces. (a) setClipMotion stores
    // worldX / worldY (+-10000 clamp) and the static value moves the trueProjection quad by
    // worldX * fov / (fov + Z) like camera3d G13; (b) motion.world.x leaves the motion.position.x
    // (2D) track alone; (c) turning the 3D layer off zeroes worldX through MCP set_clip_property,
    // the EffectControls motion widget and MainWindow::pasteEffects, while MCP is3DLayer=true on a
    // flat clip that already holds worldX keeps it (the control that fails if the MCP motion is
    // built without worldX).
    {
        bool ok = true;
        QStringList parts;
        const auto alphaCentroidX = [](const QImage &image) {
            const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
            double sum = 0.0;
            double count = 0.0;
            for (int y = 0; y < rgba.height(); ++y) {
                const uchar *row = rgba.constScanLine(y);
                for (int x = 0; x < rgba.width(); ++x) {
                    if (row[x * 4 + 3] > 128) {
                        sum += x;
                        count += 1.0;
                    }
                }
            }
            return count > 0.0 ? sum / count : -1.0;
        };
        const auto firstClip = [](Timeline &timeline) {
            return timeline.videoTracks().first()->clips().first();
        };

        // (a) static value reaches storage, the clamp and the render.
        {
            Camera3DState camera;
            camera.trueProjection = true;
            Timeline timeline;
            timeline.setProjectCamera(camera);
            ClipInfo clip = makeClip(clipPath);
            clip.is3DLayer = true;
            clip.layer3D.positionZ = 40.0;
            setTimelineClip(timeline, clip);

            const QSize outSize(160, 90);
            const double baseX = alphaCentroidX(tlrender::renderFrameAt(&timeline, 0, outSize));

            effectctrl::MotionState motion;
            motion.scale = clip.videoScale;
            motion.is3DLayer = true;
            motion.posZ = 40.0;
            motion.worldX = 20.0;
            timeline.setClipMotion(0, 0, motion);
            const ClipInfo stored = firstClip(timeline);
            const double shiftedX = alphaCentroidX(tlrender::renderFrameAt(&timeline, 0, outSize));
            const double expectedDx = 20.0 * camera.fov / (camera.fov + 40.0);

            motion.worldX = 123.0;
            motion.worldY = -45.0;
            timeline.setClipMotion(0, 0, motion);
            const ClipInfo exact = firstClip(timeline);
            motion.worldX = 99999.0;
            motion.worldY = -99999.0;
            timeline.setClipMotion(0, 0, motion);
            const ClipInfo clamped = firstClip(timeline);

            const bool storedOk = stored.layer3D.worldX == 20.0 && exact.layer3D.worldX == 123.0
                && exact.layer3D.worldY == -45.0 && clamped.layer3D.worldX == 10000.0
                && clamped.layer3D.worldY == -10000.0;
            const bool renderOk = baseX >= 0.0 && shiftedX >= 0.0
                && std::abs((shiftedX - baseX) - expectedDx) < 1.0 && expectedDx > 5.0;
            parts << QStringLiteral("(a) stored=%1 world(20,123,99999)=(%2,%3,%4) worldY(-45,-99999)=(%5,%6) "
                                    "centroidX base=%7 shifted=%8 dx=%9 expected=%10 renderOk=%11")
                         .arg(storedOk ? QStringLiteral("yes") : QStringLiteral("no"))
                         .arg(stored.layer3D.worldX).arg(exact.layer3D.worldX).arg(clamped.layer3D.worldX)
                         .arg(exact.layer3D.worldY).arg(clamped.layer3D.worldY)
                         .arg(baseX, 0, 'f', 3).arg(shiftedX, 0, 'f', 3)
                         .arg(shiftedX - baseX, 0, 'f', 3).arg(expectedDx, 0, 'f', 3)
                         .arg(renderOk ? QStringLiteral("yes") : QStringLiteral("no"));
            ok = ok && storedOk && renderOk;
        }

        // (b) a motion.world.x track does not touch the 2D position track or its evaluation.
        {
            ClipInfo plain = makeClip(clipPath);
            KeyframeTrack posX(QStringLiteral("motion.position.x"), 0.0);
            posX.addKeyframe(0.0, 0.1);
            posX.addKeyframe(1.0, 0.3);
            plain.keyframes.addTrack(posX);
            ClipInfo withWorld = plain;
            KeyframeTrack worldX(QStringLiteral("motion.world.x"), 0.0);
            worldX.addKeyframe(0.0, 10.0);
            worldX.addKeyframe(1.0, 500.0);
            withWorld.keyframes.addTrack(worldX);
            const double plainDx = clipanim::effectiveTransformAt(plain, 0.5).videoDx;
            const double worldDx = clipanim::effectiveTransformAt(withWorld, 0.5).videoDx;
            const KeyframeTrack *posAfter = withWorld.keyframes.track(QStringLiteral("motion.position.x"));
            ClipInfo shifted = plain;
            KeyframeTrack posXMoved(QStringLiteral("motion.position.x"), 0.0);
            posXMoved.addKeyframe(0.0, 0.5);
            posXMoved.addKeyframe(1.0, 0.9);
            shifted.keyframes.addTrack(posXMoved);
            const double movedDx = clipanim::effectiveTransformAt(shifted, 0.5).videoDx;
            const bool trackOk = withWorld.keyframes.hasTrack(QStringLiteral("motion.world.x"))
                && posAfter && posAfter->count() == 2
                && near(posAfter->valueAt(0.0), 0.1) && near(posAfter->valueAt(1.0), 0.3);
            const bool evalOk = near(plainDx, 0.2) && near(worldDx, plainDx) && !near(movedDx, plainDx);
            parts << QStringLiteral("(b) plainDx=%1 withWorldDx=%2 (expect 0.2 both) controlMovedDx=%3 trackOk=%4")
                         .arg(plainDx, 0, 'f', 6).arg(worldDx, 0, 'f', 6).arg(movedDx, 0, 'f', 6)
                         .arg(trackOk ? QStringLiteral("yes") : QStringLiteral("no"));
            ok = ok && trackOk && evalOk;
        }

        // (c) keep / zero across the three routes.
        {
            ClipInfo threeD = makeClip(clipPath);
            threeD.is3DLayer = true;
            threeD.layer3D.positionZ = 50.0;
            threeD.layer3D.worldX = 123.0;
            threeD.layer3D.worldY = -45.0;
            ClipInfo flatWorld = threeD;
            flatWorld.is3DLayer = false;
            flatWorld.layer3D.positionZ = 0.0;

            // EffectControls: the widget is filled from the clip, then the 3D toggle is switched off.
            double panelLoaded = -1.0;
            double panelAfter = -1.0;
            bool panelFound = false;
            {
                Timeline timeline;
                setTimelineClip(timeline, threeD);
                effectctrl::EffectControlsPanel panel;
                panel.setTimeline(&timeline);
                timeline.videoTracks().first()->setSelectedClip(0);
                QMetaObject::invokeMethod(&timeline, "clipSelectedOnTrack", Qt::DirectConnection,
                                          Q_ARG(int, 0), Q_ARG(int, 0));
                auto *motionWidget = panel.findChild<effectctrl::MotionSectionWidget *>(
                    QStringLiteral("motionSectionWidget"));
                QToolButton *toggle3D = motionWidget
                    ? motionWidget->findChild<QToolButton *>(QStringLiteral("motionToggle3D"))
                    : nullptr;
                panelFound = motionWidget && toggle3D;
                if (panelFound) {
                    panelLoaded = motionWidget->currentMotion().worldX;
                    toggle3D->click();
                    panelAfter = firstClip(timeline).layer3D.worldX;
                }
            }
            const bool panelOk = panelFound && panelLoaded == 123.0 && panelAfter == 0.0;

            // MCP set_clip_property + MainWindow::pasteEffects on one MainWindow timeline.
            mcp::McpToolRegistry registry;
            MainWindow window;
            window.hide();
            mcp::McpEditorTools tools(&window, &registry);
            tools.registerReadTools();
            tools.registerWriteTools();
            mcp::McpProtocol protocol(&registry, mcp::ServerInfo{});
            Timeline *winTimeline = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
            int requestId = 800;
            const auto setIs3D = [&](bool value) {
                const QJsonObject request{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), ++requestId},
                    {QStringLiteral("method"), QStringLiteral("tools/call")},
                    {QStringLiteral("params"), QJsonObject{
                        {QStringLiteral("name"), QStringLiteral("set_clip_property")},
                        {QStringLiteral("arguments"), QJsonObject{
                            {QStringLiteral("trackIndex"), 0},
                            {QStringLiteral("clipIndex"), 0},
                            {QStringLiteral("property"), QStringLiteral("is3DLayer")},
                            {QStringLiteral("value"), value}}}}}};
                const QJsonObject response = QJsonDocument::fromJson(protocol.handleMessage(
                    QJsonDocument(request).toJson(QJsonDocument::Compact))).object();
                return !response.value(QStringLiteral("result")).toObject()
                            .value(QStringLiteral("isError")).toBool(true);
            };
            const auto loadWindowClip = [&](const ClipInfo &clip) {
                winTimeline->restoreFromProject(
                    QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}},
                    QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}}, 0.0, -1.0, -1.0, 100);
            };
            bool mcpOff = false, mcpOn = false, pasted = false;
            double mcpOffX = -1.0, mcpOnX = -1.0, mcpOnY = -1.0, pasteX = -1.0, pasteBefore = -1.0;
            if (winTimeline) {
                loadWindowClip(threeD);
                mcpOff = setIs3D(false);
                mcpOffX = winTimeline->videoTracks().first()->clips().first().layer3D.worldX;

                loadWindowClip(flatWorld);
                mcpOn = setIs3D(true);
                const ClipInfo turnedOn = winTimeline->videoTracks().first()->clips().first();
                mcpOnX = turnedOn.layer3D.worldX;
                mcpOnY = turnedOn.layer3D.worldY;

                loadWindowClip(threeD);
                pasteBefore = winTimeline->videoTracks().first()->clips().first().layer3D.worldX;
                effectctrl::EffectClipboard &clipboard = effectctrl::EffectClipboard::instance();
                const QVector<VideoEffect> savedEffects = clipboard.effects();
                const effectctrl::ClipMotion savedMotion = clipboard.motion();
                clipboard.capture(QVector<VideoEffect>{VideoEffect{}}, effectctrl::ClipMotion{});
                winTimeline->videoTracks().first()->setSelectedClip(0);
                QMetaObject::invokeMethod(winTimeline, "clipSelectedOnTrack", Qt::DirectConnection,
                                          Q_ARG(int, 0), Q_ARG(int, 0));
                pasted = QMetaObject::invokeMethod(&window, "pasteEffects", Qt::DirectConnection);
                pasteX = winTimeline->videoTracks().first()->clips().first().layer3D.worldX;
                clipboard.capture(savedEffects, savedMotion);
            }
            const bool mcpOk = winTimeline && mcpOff && mcpOffX == 0.0
                && mcpOn && mcpOnX == 123.0 && mcpOnY == -45.0;
            const bool pasteOk = winTimeline && pasted && pasteBefore == 123.0 && pasteX == 0.0;
            parts << QStringLiteral("(c) panel found=%1 loaded=%2 after=%3 | mcp off ok=%4 worldX=%5 "
                                    "on(flat 123) ok=%6 worldX=%7 worldY=%8 | pasteEffects invoked=%9 "
                                    "before=%10 after=%11")
                         .arg(panelFound).arg(panelLoaded).arg(panelAfter)
                         .arg(mcpOff).arg(mcpOffX).arg(mcpOn).arg(mcpOnX).arg(mcpOnY)
                         .arg(pasted).arg(pasteBefore).arg(pasteX);
            ok = ok && panelOk && mcpOk && pasteOk;
        }
        check(8, "3D layer world x/y through motion UI surfaces", ok, parts.join(QStringLiteral("; ")));
    }

    // US-1908 G9-G11: the 3D keyframe tracks reach renderFrameAt through clipanim::layer3DAt.
    {
        const QSize size3D(160, 90);
        const qint64 oneSecondUsec = 1'000'000;
        ClipInfo plain3D = makeClip(clipPath);
        plain3D.is3DLayer = true;

        const auto same3D = [](const Layer3DTransform &a, const Layer3DTransform &b) {
            return std::abs(a.positionZ - b.positionZ) <= 1e-12
                && std::abs(a.rotationX - b.rotationX) <= 1e-12
                && std::abs(a.rotationY - b.rotationY) <= 1e-12
                && std::abs(a.rotationZ - b.rotationZ) <= 1e-12
                && std::abs(a.worldX - b.worldX) <= 1e-12
                && std::abs(a.worldY - b.worldY) <= 1e-12;
        };
        const auto str3D = [](const Layer3DTransform &l) {
            return QStringLiteral("{z %1 rx %2 ry %3 rz %4 wx %5 wy %6}")
                .arg(l.positionZ).arg(l.rotationX).arg(l.rotationY).arg(l.rotationZ)
                .arg(l.worldX).arg(l.worldY);
        };
        const auto withTrack = [](ClipInfo clip, const QString &name, double valueAtTwoSeconds) {
            KeyframeTrack track(name, 0.0);
            track.addKeyframe(0.0, 0.0);
            track.addKeyframe(2.0, valueAtTwoSeconds);
            clip.keyframes.addTrack(track);
            return clip;
        };
        const auto render3D = [&](const ClipInfo &clip, bool trueProjection, bool onV2,
                                  const QVector<Light3D> &lights) {
            Camera3DState camera;
            camera.trueProjection = trueProjection;
            Timeline timeline;
            timeline.setProjectCamera(camera);
            timeline.setProjectLights(lights);
            if (onV2) {
                // V1 paints on top: half opacity keeps the V2 layer in the frame.
                ClipInfo v1 = makeClip(clipPath);
                v1.opacity = 0.5;
                timeline.addVideoTrack(false);
                setTimelineClip(timeline, v1);
                timeline.videoTracks()[1]->setClips(QVector<ClipInfo>{clip});
            } else {
                setTimelineClip(timeline, clip);
            }
            return tlrender::renderFrameAt(&timeline, oneSecondUsec, size3D);
        };
        struct Track3D {
            const char *label;
            const char *track;
            double Layer3DTransform::*field;
            double valueAtTwoSeconds;
        };
        const Track3D posZ{"posZ", "motion.position.z", &Layer3DTransform::positionZ, 400.0};
        const Track3D rotY{"rotY", "motion.rotation.y", &Layer3DTransform::rotationY, 60.0};
        const Track3D worldX{"worldX", "motion.world.x", &Layer3DTransform::worldX, 300.0};
        // A key pair at t = 1.0 equals the static value of half the end value, and not the zero one.
        const auto compareKeyed = [&](const ClipInfo &base, const Track3D &row, bool trueProjection,
                                      bool onV2, const QVector<Light3D> &lights, QStringList *parts) {
            ClipInfo wantedClip = base;
            wantedClip.layer3D.*row.field = row.valueAtTwoSeconds / 2.0;
            const QImage keyed = render3D(withTrack(base, QString::fromLatin1(row.track),
                                                    row.valueAtTwoSeconds),
                                          trueProjection, onV2, lights);
            const QImage wanted = render3D(wantedClip, trueProjection, onV2, lights);
            const QImage zero = render3D(base, trueProjection, onV2, lights);
            const bool equalsWanted = sameBytes(keyed, wanted);
            const bool differsFromZero = !sameBytes(keyed, zero);
            *parts << QStringLiteral("%1 %2 keyed=%3 static=%4 zero=%5 equal=%6 differsFromZero=%7")
                          .arg(QString::fromLatin1(row.label), onV2 ? QStringLiteral("V2") : QStringLiteral("V1"),
                               imageDigest(keyed), imageDigest(wanted), imageDigest(zero))
                          .arg(equalsWanted).arg(differsFromZero);
            return equalsWanted && differsFromZero;
        };

        // G9: keyed position z / rotation y / world x under trueProjection (HEAD: track ignored).
        {
            bool ok = true;
            QStringList parts;
            for (const Track3D *row : {&posZ, &rotY, &worldX}) {
                ok = compareKeyed(plain3D, *row, true, false, {}, &parts) && ok;
                ok = compareKeyed(plain3D, *row, true, true, {}, &parts) && ok;
            }

            // All six tracks land on their own fields; a track-less field keeps its static value.
            ClipInfo allTracks = plain3D;
            allTracks = withTrack(allTracks, QStringLiteral("motion.position.z"), 400.0);
            allTracks = withTrack(allTracks, QStringLiteral("motion.rotation.x"), 10.0);
            allTracks = withTrack(allTracks, QStringLiteral("motion.rotation.y"), 60.0);
            allTracks = withTrack(allTracks, QStringLiteral("motion.rotation.z"), 14.0);
            allTracks = withTrack(allTracks, QStringLiteral("motion.world.x"), 300.0);
            allTracks = withTrack(allTracks, QStringLiteral("motion.world.y"), -40.0);
            Layer3DTransform wantAll;
            wantAll.positionZ = 200.0;
            wantAll.rotationX = 5.0;
            wantAll.rotationY = 30.0;
            wantAll.rotationZ = 7.0;
            wantAll.worldX = 150.0;
            wantAll.worldY = -20.0;
            const Layer3DTransform gotAll = clipanim::layer3DAt(allTracks, 1.0);
            ClipInfo partial = withTrack(plain3D, QStringLiteral("motion.rotation.y"), 60.0);
            partial.layer3D.positionZ = 11.0;
            partial.layer3D.worldY = -3.0;
            Layer3DTransform wantPartial = partial.layer3D;
            wantPartial.rotationY = 30.0;
            const Layer3DTransform gotPartial = clipanim::layer3DAt(partial, 1.0);
            ok = ok && same3D(gotAll, wantAll) && same3D(gotPartial, wantPartial);
            parts << QStringLiteral("all six %1 partial %2").arg(str3D(gotAll), str3D(gotPartial));

            // The incoming side of an overlap transition is projected with the keyed value:
            // flat keys, so the expectation does not depend on where the transition samples.
            const auto overlapFrame = [&](const ClipInfo &incomingBase, bool *overlaps) {
                ClipInfo outgoing = makeClip(clipPath);
                ClipInfo incoming = incomingBase;
                outgoing.inPoint = incoming.inPoint = 0.5;
                outgoing.outPoint = incoming.outPoint = 1.5;
                outgoing.trailOut.type = incoming.leadIn.type = TransitionType::CrossDissolve;
                outgoing.trailOut.duration = incoming.leadIn.duration = 0.5;
                outgoing.trailOut.alignment = incoming.leadIn.alignment = TransitionAlignment::Center;
                Camera3DState camera;
                camera.trueProjection = true;
                Timeline timeline;
                timeline.setProjectCamera(camera);
                timeline.videoTracks().first()->setClips({outgoing, incoming});
                const auto intervals = timeline.videoOverlapIntervals();
                *overlaps = !intervals.isEmpty() && intervals[0].size() == 2
                    && intervals[0][1].timelineStart < intervals[0][0].timelineEnd;
                if (!*overlaps)
                    return QImage();
                return tlrender::renderFrameAt(
                    &timeline,
                    qRound64((intervals[0][1].timelineStart + intervals[0][0].timelineEnd) * 0.5 * 1e6),
                    size3D);
            };
            ClipInfo flatKeyed = plain3D;
            KeyframeTrack flatZ(QStringLiteral("motion.position.z"), 0.0);
            flatZ.addKeyframe(0.0, 200.0);
            flatZ.addKeyframe(2.0, 200.0);
            flatKeyed.keyframes.addTrack(flatZ);
            ClipInfo staticZ = plain3D;
            staticZ.layer3D.positionZ = 200.0;
            bool overlapKeyed = false, overlapStatic = false, overlapZero = false;
            const QImage keyedOverlap = overlapFrame(flatKeyed, &overlapKeyed);
            const QImage staticOverlap = overlapFrame(staticZ, &overlapStatic);
            const QImage zeroOverlap = overlapFrame(plain3D, &overlapZero);
            ok = ok && overlapKeyed && overlapStatic && overlapZero
                && sameBytes(keyedOverlap, staticOverlap) && !sameBytes(keyedOverlap, zeroOverlap);
            parts << QStringLiteral("overlap overlaps=%1 keyed=%2 static=%3 zero=%4")
                         .arg(overlapKeyed && overlapStatic && overlapZero)
                         .arg(imageDigest(keyedOverlap), imageDigest(staticOverlap), imageDigest(zeroOverlap));
            check(9, "3D keyframes drive trueProjection in renderFrameAt", ok,
                  parts.join(QStringLiteral("; ")));
        }

        // G10: keyed values reach the lights without trueProjection.
        {
            Light3DState point;
            point.type = LightType::Point;
            point.position = QVector3D(200.0f, 100.0f, 150.0f);
            point.intensity = 2.0;
            point.falloffEnabled = true;
            const QVector<Light3D> lights{Light3D(point)};
            ClipInfo lit = plain3D;
            lit.material.acceptsLights = true;

            bool ok = true;
            QStringList parts;
            ok = compareKeyed(lit, posZ, false, false, lights, &parts) && ok;
            ok = compareKeyed(lit, posZ, false, true, lights, &parts) && ok;
            ok = compareKeyed(lit, rotY, false, false, lights, &parts) && ok;
            check(10, "3D keyframes drive the lighting input", ok, parts.join(QStringLiteral("; ")));
        }

        // G11 (pin): no 3D keyframes = static values; a clip with 3D off never evaluates its track.
        {
            bool ok = true;
            QStringList parts;

            // Compared in the raw log with the base exe's sha (S0-7), not here.
            const auto sha = [](const QImage &image) {
                if (image.isNull())
                    return QStringLiteral("null");
                const QByteArray bytes(reinterpret_cast<const char *>(image.constBits()),
                                       static_cast<int>(image.sizeInBytes()));
                return QStringLiteral("%1:%2x%3:%4")
                    .arg(static_cast<int>(image.format())).arg(image.width()).arg(image.height())
                    .arg(QString::fromLatin1(
                        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
            };
            for (const bool trueProjection : {false, true}) {
                for (const bool is3D : {true, false}) {
                    ClipInfo clip = makeClip(clipPath);
                    clip.is3DLayer = is3D;
                    if (is3D) {
                        clip.layer3D.positionZ = 200.0;
                        clip.layer3D.rotationY = 30.0;
                    }
                    const QImage frame = render3D(clip, trueProjection, false, {});
                    ok = ok && !frame.isNull();
                    parts << QStringLiteral("sha trueProjection=%1 is3D=%2 %3")
                                 .arg(trueProjection).arg(is3D).arg(sha(frame));
                }
            }

            ClipInfo noTracks = plain3D;
            noTracks.layer3D.positionZ = 12.0;
            noTracks.layer3D.rotationX = 1.0;
            noTracks.layer3D.rotationY = 2.0;
            noTracks.layer3D.rotationZ = 3.0;
            noTracks.layer3D.worldX = 4.0;
            noTracks.layer3D.worldY = 5.0;
            const Layer3DTransform noTrackValue = clipanim::layer3DAt(noTracks, 1.0);
            Layer3DTransform nudged = noTracks.layer3D;
            nudged.worldY += 1e-9;
            const bool noTrackOk = same3D(noTrackValue, noTracks.layer3D)
                && same3D(clipanim::layer3DAt(noTracks, 99.0), noTracks.layer3D);
            const bool comparerOk = !same3D(nudged, noTracks.layer3D);
            ok = ok && noTrackOk && comparerOk;
            parts << QStringLiteral("noTrack=%1 %2 comparerRejects=%3")
                         .arg(noTrackOk).arg(str3D(noTrackValue)).arg(comparerOk);

            // The three-argument form is the four-argument form with base = clip.layer3D.
            Camera3DState orientCamera;
            orientCamera.position = QVector3D(300.0f, 200.0f, 800.0f);
            orientCamera.target = orientCamera.position + QVector3D(0.0f, 0.0f, -1.0f);
            orientCamera.trueProjection = true;
            ClipInfo oriented = noTracks;
            oriented.autoOrientToCamera = true;
            const QPointF layerPx(150.0, 0.0);
            const Layer3DTransform orient3 = camera3d::effectiveLayer3DAt(oriented, layerPx, orientCamera);
            const Layer3DTransform orient4 = camera3d::effectiveLayer3DAt(
                oriented, oriented.layer3D, layerPx, orientCamera);
            const Layer3DTransform plain3 = camera3d::effectiveLayer3DAt(noTracks, layerPx, orientCamera);
            const Layer3DTransform plain4 = camera3d::effectiveLayer3DAt(
                noTracks, noTracks.layer3D, layerPx, orientCamera);
            const bool formsOk = same3D(orient3, orient4) && !same3D(orient3, oriented.layer3D)
                && same3D(plain3, plain4) && same3D(plain3, noTracks.layer3D);
            // autoOrient replaces keyed rotations X / Y and orients from the base it is given.
            Layer3DTransform keyedBase = oriented.layer3D;
            keyedBase.positionZ = 400.0;
            keyedBase.rotationX = 33.0;
            keyedBase.rotationY = 44.0;
            keyedBase.rotationZ = 55.0;
            const Layer3DTransform orientKeyed = camera3d::effectiveLayer3DAt(
                oriented, keyedBase, layerPx, orientCamera);
            ClipInfo staticEquivalent = oriented;
            staticEquivalent.layer3D = keyedBase;
            const bool baseOk = same3D(orientKeyed, camera3d::effectiveLayer3DAt(
                                                        staticEquivalent, layerPx, orientCamera))
                && orientKeyed.positionZ == 400.0 && orientKeyed.rotationZ == 55.0
                && orientKeyed.rotationX != 33.0 && orientKeyed.rotationY != 44.0;
            ok = ok && formsOk && baseOk;
            parts << QStringLiteral("forms=%1 orient3=%2 base=%3 orientKeyed=%4")
                         .arg(formsOk).arg(str3D(orient3)).arg(baseOk).arg(str3D(orientKeyed));

            // M-4: 3D off keeps the track and renders without it.
            ClipInfo off = makeClip(clipPath);
            const ClipInfo offKeyed = withTrack(off, QStringLiteral("motion.position.z"), 400.0);
            const QImage offKeyedFrame = render3D(offKeyed, true, false, {});
            const QImage offPlainFrame = render3D(off, true, false, {});
            const bool offFrameOk = sameBytes(offKeyedFrame, offPlainFrame);
            const bool offValueOk = same3D(clipanim::layer3DAt(offKeyed, 1.0), offKeyed.layer3D);
            ok = ok && offFrameOk && offValueOk;
            parts << QStringLiteral("3D off with track: frame=%1 value=%2 keyed=%3 plain=%4")
                         .arg(offFrameOk).arg(offValueOk)
                         .arg(imageDigest(offKeyedFrame), imageDigest(offPlainFrame));
            check(11, "no 3D keyframes or 3D off: byte-identical", ok, parts.join(QStringLiteral("; ")));
        }

        // G12: the paused preview reads the keyed values: the GL seam (static and auto-orient
        // paths of makePreviewMotion) and the special composite's lighting input.
        {
            bool ok = true;
            QStringList parts;
            const ClipInfo keyedZ = withTrack(plain3D, QStringLiteral("motion.position.z"), 400.0);
            Camera3DState camera;
            camera.trueProjection = true;

            Timeline glTimeline;
            glTimeline.setProjectCamera(camera);
            setTimelineClip(glTimeline, keyedZ);
            const Layer3DTransform glKeyed = previewLayer3DForSelftest(&glTimeline, 1.0);
            setTimelineClip(glTimeline, plain3D);
            const double glStaticZ = previewLayer3DForSelftest(&glTimeline, 1.0).positionZ;
            const bool glOk = near(glKeyed.positionZ, 200.0, 1e-9) && glStaticZ == 0.0;
            ok = ok && glOk;
            parts << QStringLiteral("gl keyed=%1 static z=%2").arg(str3D(glKeyed)).arg(glStaticZ);

            // Off-axis, so the orientation depends on the keyed distance to the camera.
            ClipInfo oriented = keyedZ;
            oriented.autoOrientToCamera = true;
            oriented.videoDx = 0.25;
            setTimelineClip(glTimeline, oriented);
            const Layer3DTransform glOriented = previewLayer3DForSelftest(&glTimeline, 1.0);
            const QPointF orientedPx(0.25 * glTimeline.projectOutputSize().width(), 0.0);
            const Camera3DState cameraAtFrame = glTimeline.projectCameraAt(1.0);
            const Layer3DTransform wantOriented = camera3d::effectiveLayer3DAt(
                oriented, clipanim::layer3DAt(oriented, 1.0), orientedPx, cameraAtFrame);
            const Layer3DTransform oldOriented = camera3d::effectiveLayer3DAt(
                oriented, orientedPx, cameraAtFrame);
            const bool orientOk = glOriented.rotationX == wantOriented.rotationX
                && glOriented.rotationY == wantOriented.rotationY
                && glOriented.rotationZ == wantOriented.rotationZ
                && near(glOriented.positionZ, 200.0, 1e-9)
                && std::abs(wantOriented.rotationY) > 0.01
                && std::abs(wantOriented.rotationY - oldOriented.rotationY) > 0.01;
            ok = ok && orientOk;
            parts << QStringLiteral("orient gl=%1 want=%2 static-base=%3 ok=%4")
                         .arg(str3D(glOriented), str3D(wantOriented), str3D(oldOriented))
                         .arg(orientOk);

            Light3DState point;
            point.type = LightType::Point;
            point.position = QVector3D(200.0f, 100.0f, 150.0f);
            point.intensity = 2.0;
            point.falloffEnabled = true;
            const QVector<Light3D> lights{Light3D(point)};
            MainWindow window;
            window.m_projectConfig.width = size3D.width();
            window.m_projectConfig.height = size3D.height();
            window.m_projectConfig.fps = kFps;
            window.m_projectLights = lights;
            window.m_timeline->setProjectLights(lights);
            const auto composite = [&](const ClipInfo &clip) {
                window.m_timeline->restoreFromProject(QVector<QVector<ClipInfo>>{{clip}},
                                                      QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
                return window.buildSpecialClipComposite(1.0);
            };
            ClipInfo lit = plain3D;
            lit.material.acceptsLights = true;
            ClipInfo lit200 = lit;
            lit200.layer3D.positionZ = 200.0;
            const QImage keyedComposite = composite(
                withTrack(lit, QStringLiteral("motion.position.z"), 400.0));
            const QImage keyedExport = tlrender::renderFrameAt(window.m_timeline, oneSecondUsec, size3D);
            const QImage staticComposite = composite(lit200);
            const QImage zeroComposite = composite(lit);
            const bool equalsStatic = sameBytes(keyedComposite, staticComposite);
            const bool differsFromZero = !sameBytes(keyedComposite, zeroComposite);
            const bool matchesExport = !keyedExport.isNull() && sameBytes(
                keyedComposite.convertToFormat(QImage::Format_RGBA8888),
                keyedExport.convertToFormat(QImage::Format_RGBA8888));
            ok = ok && !keyedComposite.isNull() && !zeroComposite.isNull()
                && equalsStatic && differsFromZero && matchesExport;
            parts << QStringLiteral("composite keyed=%1 static200=%2 zero=%3 export=%4 "
                                    "equal=%5 differsFromZero=%6 matchesExport=%7")
                         .arg(imageDigest(keyedComposite), imageDigest(staticComposite),
                              imageDigest(zeroComposite), imageDigest(keyedExport))
                         .arg(equalsStatic).arg(differsFromZero).arg(matchesExport);
            check(12, "3D keyframes reach the GL preview and the special composite", ok,
                  parts.join(QStringLiteral("; ")));
        }
    }

    qInfo().noquote()
        << QStringLiteral("KEYFRAME-ANIM-PARITY: %1/%2 PASS")
               .arg(passed)
               .arg(passed + failed);
    std::fprintf(stderr, "[keyframe-anim-parity] summary: %d PASS, %d FAIL, 0 SKIP\n",
                 passed, failed);
    // The registry checks the declared "(N gates)" against this count.
    selftests::reportTally(passed, failed, 0);
    return failed == 0 ? 0 : 1;
}
