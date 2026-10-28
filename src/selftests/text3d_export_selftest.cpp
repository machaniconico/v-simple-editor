// US-1707: a clip's 3D text (ClipInfo::text3DConfig) is drawn into exports by
// tlrender::renderFrameAt, on the clip's native layer source after the clip
// mask and before placement (rule (78), user decision M), for V1, upper layers
// and the overlap-transition neighbour alike.
#include "../ClipGeometry.h"
#include "../MaskSystem.h"
#include "../ShapeLayer.h"
#include "../Text3DLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <QVector3D>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr int kFps = 10;

QJsonObject textConfig(const QString &text)
{
    Text3DLayer layer;
    layer.setText(text, QFont(QStringLiteral("Arial"), 72));
    layer.setExtrudeEnabled(true);
    layer.setExtrude(0.35, 0.03, 0.03, 2);
    layer.setCameraDistance(3.0);
    layer.setRotationAnimAxis(QVector3D(0.0f, 1.0f, 0.0f));
    return layer.toJson();
}

ClipInfo shapeClip(const QColor &color, const QRectF &rect, double duration = 2.0)
{
    Shape rectangle;
    rectangle.type = ShapeType::Rectangle;
    rectangle.position = rect.center();
    rectangle.properties.size = rect.size();
    rectangle.fill.color = color;
    rectangle.stroke.enabled = false;
    ClipInfo clip;
    clip.displayName = QStringLiteral("text3d-export shape");
    clip.duration = clip.outPoint = duration;
    clip.shapes = {rectangle};
    return clip;
}

ClipInfo fullFrame(const QColor &color)
{
    return shapeClip(color, QRectF(0, 0, kW, kH));
}

ClipInfo withText(ClipInfo clip, const QString &text = QStringLiteral("3D"))
{
    clip.text3DConfig = textConfig(text);
    return clip;
}

std::unique_ptr<Timeline> makeTimeline(const QVector<QVector<ClipInfo>> &tracks)
{
    auto timeline = std::make_unique<Timeline>();
    timeline->setProjectOutputConfig(kW, kH, false, kFps);
    timeline->restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    return timeline;
}

qint64 us(double sec) { return qint64(sec * 1000000.0); }

QImage frameAt(const Timeline *timeline, double sec)
{
    return tlrender::renderFrameAt(timeline, us(sec), QSize(kW, kH));
}

// Pixels whose RGBA differs by more than `tolerance` in any channel; -1 when
// either side is null or the sizes differ (never a passing comparison).
qint64 diffPixels(const QImage &a, const QImage &b, const QRect &rect = QRect(),
                  int tolerance = 0)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return -1;
    const QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage ib = b.convertToFormat(QImage::Format_RGBA8888);
    const QRect area = rect.isNull() ? ia.rect() : rect.intersected(ia.rect());
    qint64 count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const uchar *pa = ia.constScanLine(y);
        const uchar *pb = ib.constScanLine(y);
        for (int x = area.left(); x <= area.right(); ++x) {
            for (int c = 0; c < 4; ++c) {
                if (std::abs(int(pa[x * 4 + c]) - int(pb[x * 4 + c])) > tolerance) {
                    ++count;
                    break;
                }
            }
        }
    }
    return count;
}

// Byte identity: same size, same format, same pixels.
bool sameBytes(const QImage &a, const QImage &b)
{
    return !a.isNull() && !b.isNull() && a.format() == b.format() && a == b;
}

// The contract, built by hand: a fresh Text3DLayer from the config draws at
// the native source size and is alpha-overed with the preview's painter calls
// (MainWindow::buildSpecialClipComposite).
QImage handText(const QImage &native, const QJsonObject &config, double localSec)
{
    Text3DLayer layer;
    layer.fromJson(config);
    const QImage text = layer.renderFrame(native.size(), localSec, layer.camera());
    QImage base = native.convertToFormat(QImage::Format_ARGB32);
    QPainter painter(&base);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRect(QPoint(0, 0), base.size()), text);
    painter.end();
    return base;
}

// A lone V1 clip: placement through clipgeom, then SourceOver at the clip's
// opacity onto a transparent canvas (renderFrameFromTracks' no-overlay path).
QImage handPlaceV1(const QImage &native, const ClipInfo &clip)
{
    const clipgeom::ClipTransform transform{clip.videoScale, clip.videoDx,
                                            clip.videoDy, clip.rotation2DDegrees};
    const QImage placed = clipgeom::renderLayer(native, transform, QSize(kW, kH), true);
    QImage composed(placed.size(), QImage::Format_ARGB32_Premultiplied);
    composed.fill(Qt::transparent);
    QPainter p(&composed);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setOpacity(qBound(0.0, clip.opacity, 1.0));
    p.drawImage(0, 0, placed);
    p.end();
    return composed.convertToFormat(QImage::Format_RGBA8888);
}

// mpeg4 clip of `w` x `h`, frame i a flat grey of luma 20 * (i + 1).
bool writeClip(const QString &path, int w, int h)
{
    QImage frame(w, h, QImage::Format_RGB888);
    libavcore::EncodeRequest request;
    request.width = w;
    request.height = h;
    request.fps = request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 2000000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (const auto error = encoder.open(request)) {
        std::cerr << "text3d-export fixture open: " << *error << '\n';
        return false;
    }
    for (int i = 0; i < 2 * kFps; ++i) {
        const int luma = std::min(255, 20 * (i % 12 + 1));
        frame.fill(QColor(luma, luma, luma));
        if (!encoder.pushFrameRgb24(frame.constBits(), frame.bytesPerLine(), i))
            return false;
    }
    return !encoder.finalize();
}

} // namespace

int runText3dExportSelftest()
{
    selftest::GateTally tally;
    const double t = 0.5;
    const QColor grey(90, 110, 130);
    const ClipInfo plain = fullFrame(grey);
    const ClipInfo texted = withText(plain);

    // G1: the 3D text is drawn (HEAD: renderFrameAt ignored text3DConfig, so
    // the two frames were byte-identical). Also times one 1080p renderFrame
    // (Step 0 item 3).
    {
        const auto withTl = makeTimeline({{texted}});
        const auto withoutTl = makeTimeline({{plain}});
        const qint64 d = diffPixels(frameAt(withTl.get(), t), frameAt(withoutTl.get(), t));
        Text3DLayer layer;
        layer.fromJson(texted.text3DConfig);
        QElapsedTimer timer;
        timer.start();
        const QImage big = layer.renderFrame(QSize(1920, 1080), t, layer.camera());
        const qint64 ms = timer.elapsed();
        std::cerr << "text3d-export G1 textVsPlainDiffPixels=" << d << " (HEAD 0)"
                  << " renderFrame1080pMs=" << ms << " null1080p=" << big.isNull() << '\n';
        tally.gate(1, d > 200 && !big.isNull(), "3D text not drawn into renderFrameAt");
    }

    // G2: renderClipSourceAt == the native source after fx (a grade here) and
    // mask, with renderFrame(native.size(), localSec, camera) alpha-overed.
    // Sanity: the text is visible on the source and the grade changes it.
    {
        ClipInfo graded = plain;
        graded.colorCorrection.brightness = 25.0;
        const auto withTl = makeTimeline({{withText(graded)}});
        const auto withoutTl = makeTimeline({{graded}});
        const auto ungradedTl = makeTimeline({{plain}});
        const QImage src = tlrender::renderClipSourceAt(withTl.get(), 0, 0, us(t));
        const QImage native = tlrender::renderClipSourceAt(withoutTl.get(), 0, 0, us(t));
        const QImage ungraded = tlrender::renderClipSourceAt(ungradedTl.get(), 0, 0, us(t));
        const QImage hand = native.isNull() ? QImage()
            : handText(native, withText(graded).text3DConfig, t);
        const bool same = sameBytes(src, hand);
        const qint64 textVisible = diffPixels(src, native);
        const qint64 gradeVisible = diffPixels(native, ungraded);
        std::cerr << "text3d-export G2 sourceEqualsHand=" << same
                  << " sourceSize=" << src.width() << "x" << src.height()
                  << " textVisible=" << textVisible << " gradeVisible=" << gradeVisible << '\n';
        tally.gate(2, same && textVisible > 200 && gradeVisible > 1000,
                   "renderClipSourceAt differs from the hand-built fx -> mask -> 3D text source");
    }

    // G3: an empty config, and a config whose text is the empty string, leave
    // the frame and the layer source byte-unchanged (the control: G1's diff).
    {
        ClipInfo emptyConfig = plain;
        emptyConfig.text3DConfig = QJsonObject{};
        const ClipInfo emptyString = withText(plain, QString());
        const auto plainTl = makeTimeline({{plain}});
        const auto emptyConfigTl = makeTimeline({{emptyConfig}});
        const auto emptyStringTl = makeTimeline({{emptyString}});
        const QImage reference = frameAt(plainTl.get(), t);
        const bool configSame = sameBytes(frameAt(emptyConfigTl.get(), t), reference);
        const bool stringSame = sameBytes(frameAt(emptyStringTl.get(), t), reference);
        const bool sourceSame = sameBytes(
            tlrender::renderClipSourceAt(emptyStringTl.get(), 0, 0, us(t)),
            tlrender::renderClipSourceAt(plainTl.get(), 0, 0, us(t)));
        std::cerr << "text3d-export G3 emptyConfigSame=" << configSame
                  << " emptyStringConfigKeys=" << emptyString.text3DConfig.size()
                  << " emptyStringSame=" << stringSame << " emptyStringSourceSame=" << sourceSame << '\n';
        tally.gate(3, configSame && stringSame && sourceSame && !emptyString.text3DConfig.isEmpty(),
                   "empty 3D text config changed the output");
    }

    // G4: the 3D text is part of its clip's layer, so it takes the layer's z
    // order. Export stacks V1-wins (PlaybackTypes.h: the lower track index is
    // in front), so with V1 empty the opaque clip on V2 hides the text of the
    // clip on V3 where it covers it (left half), and the text shows where it
    // does not (right half; HEAD: no text there either).
    {
        const ClipInfo cover = shapeClip(QColor(200, 40, 40), QRectF(0, 0, kW / 2, kH));
        const ClipInfo layer = fullFrame(QColor(40, 40, 200));
        const auto withTl = makeTimeline({{}, {cover}, {withText(layer)}});
        const auto withoutTl = makeTimeline({{}, {cover}, {layer}});
        const QImage fa = frameAt(withTl.get(), t);
        const QImage fb = frameAt(withoutTl.get(), t);
        const qint64 covered = diffPixels(fa, fb, QRect(0, 0, kW / 2, kH));
        const qint64 open = diffPixels(fa, fb, QRect(kW / 2, 0, kW - kW / 2, kH));
        // Control: without the cover the left half shows text too.
        const auto uncoveredTl = makeTimeline({{}, {}, {withText(layer)}});
        const auto uncoveredPlainTl = makeTimeline({{}, {}, {layer}});
        const qint64 leftWithoutCover = diffPixels(frameAt(uncoveredTl.get(), t),
            frameAt(uncoveredPlainTl.get(), t), QRect(0, 0, kW / 2, kH));
        std::cerr << "text3d-export G4 coveredHalfDiff=" << covered << " (expect 0)"
                  << " openHalfDiff=" << open << " (HEAD 0) leftHalfWithoutCoverDiff="
                  << leftWithoutCover << '\n';
        tally.gate(4, covered == 0 && open > 50 && leftWithoutCover > 50,
                   "3D text not hidden by the opaque clip in front of it");
    }

    // G5: dx / scale / opacity: renderFrameAt == the hand-built source (text on
    // the native source) placed with the clip transform and opacity.
    {
        ClipInfo moved = plain;
        moved.videoDx = 0.1;
        moved.videoScale = 0.8;
        moved.opacity = 0.7;
        const ClipInfo movedText = withText(moved);
        const auto withTl = makeTimeline({{movedText}});
        const auto withoutTl = makeTimeline({{moved}});
        const QImage native = tlrender::renderClipSourceAt(withoutTl.get(), 0, 0, us(t));
        const QImage expected = native.isNull() ? QImage()
            : handPlaceV1(handText(native, movedText.text3DConfig, t), moved);
        const QImage actual = frameAt(withTl.get(), t);
        const qint64 d = diffPixels(actual, expected);
        const qint64 textVisible = diffPixels(actual, frameAt(withoutTl.get(), t));
        // Control: the text placed WITHOUT the transform differs.
        const qint64 untransformed = diffPixels(actual,
            native.isNull() ? QImage() : handText(native, movedText.text3DConfig, t));
        std::cerr << "text3d-export G5 actualVsHandDiffPixels=" << d
                  << " sameBytes=" << sameBytes(actual, expected)
                  << " textVisible=" << textVisible << " untransformedDiff=" << untransformed << '\n';
        tally.gate(5, sameBytes(actual, expected) && textVisible > 100 && untransformed > 1000,
                   "transformed 3D text clip differs from the hand-built placement");
    }

    // G6: the text animates (rotation axis): two times give different frames.
    {
        const auto tl = makeTimeline({{texted}});
        const auto plainTl = makeTimeline({{plain}});
        const qint64 d = diffPixels(frameAt(tl.get(), 0.2), frameAt(tl.get(), 0.9));
        const qint64 plainD = diffPixels(frameAt(plainTl.get(), 0.2), frameAt(plainTl.get(), 0.9));
        std::cerr << "text3d-export G6 t0.2VsT0.9DiffPixels=" << d
                  << " plainDiffPixels=" << plainD << " (HEAD/plain 0)" << '\n';
        tally.gate(6, d > 50 && plainD == 0, "3D text does not follow the clip time");
    }

    // G7: renderFrameAt on a worker QThread (which builds the Text3DLayer
    // there: a config no other gate uses) == the UI-thread frame, no crash.
    {
        const ClipInfo workerClip = withText(plain, QStringLiteral("W7"));
        const auto tl = makeTimeline({{workerClip}});
        const auto plainTl = makeTimeline({{plain}});
        QImage workerFrame;
        QThread *thread = QThread::create([&]() { workerFrame = frameAt(tl.get(), t); });
        thread->start();
        const bool finished = thread->wait(120000);
        delete thread;
        const QImage uiFrame = frameAt(tl.get(), t);
        const qint64 textVisible = diffPixels(uiFrame, frameAt(plainTl.get(), t));
        std::cerr << "text3d-export G7 finished=" << finished
                  << " workerEqualsUi=" << sameBytes(workerFrame, uiFrame)
                  << " textVisible=" << textVisible << '\n';
        tally.gate(7, finished && sameBytes(workerFrame, uiFrame) && textVisible > 200,
                   "worker-thread renderFrameAt differs from the UI thread");
    }

    // G8: the production path. (a) Two clips (V1 + V2, no transition, no
    // motion blur): renderFrameAt builds 2 layer sources. (b) A lone default
    // clip of a 160x90 movie: renderFrameAt == renderClipSourceAt scaled onto
    // the canvas (the V1 default-transform fit). (c) The same with a time
    // remap; the control is the same clip without the remap (different frame).
    {
        QString whyNot;
        if (!selftest::requireEncoder(QStringLiteral("mpeg4"), &whyNot)) {
            tally.skip(8, "mpeg4 encoder unavailable");
        } else {
            QTemporaryDir dir;
            const QString movie = dir.filePath(QStringLiteral("text3d-export.mp4"));
            const bool movieOk = dir.isValid() && writeClip(movie, 160, 90);
            const auto twoTl = makeTimeline({{texted}, {withText(fullFrame(QColor(10, 200, 10)))}});
            const quint64 before = tlrender::clipLayerSourceCallsForTest();
            const QImage twoFrame = frameAt(twoTl.get(), t);
            const quint64 calls = tlrender::clipLayerSourceCallsForTest() - before;

            ClipInfo movieClip;
            movieClip.filePath = movie;
            movieClip.duration = movieClip.outPoint = 2.0;
            movieClip.speed = 1.0;
            movieClip.text3DConfig = textConfig(QStringLiteral("M8"));
            ClipInfo remapped = movieClip;
            remapped.timeRemapCurve.sourceFps = kFps;
            remapped.timeRemapCurve.addKey(0.0, 1.2);
            remapped.timeRemapCurve.addKey(2.0, 0.2);
            const auto placedEquals = [&](const ClipInfo &clip, QImage *sourceOut) {
                const auto tl = makeTimeline({{clip}});
                const QImage source = tlrender::renderClipSourceAt(tl.get(), 0, 0, us(t));
                if (sourceOut) *sourceOut = source;
                const QImage frame = frameAt(tl.get(), t);
                return !source.isNull() && source.size() == QSize(160, 90)
                    && sameBytes(frame, source.scaled(QSize(kW, kH), Qt::IgnoreAspectRatio,
                                                      Qt::SmoothTransformation));
            };
            QImage plainSource;
            QImage remapSource;
            const bool plainOk = movieOk && placedEquals(movieClip, &plainSource);
            const bool remapOk = movieOk && placedEquals(remapped, &remapSource);
            const qint64 remapMatters = diffPixels(plainSource, remapSource);
            std::cerr << "text3d-export G8 (a) layerSourceCalls=" << calls << " (expect 2) frameNull="
                      << twoFrame.isNull() << " (b) movie=" << movieOk << " plainPlacedEquals=" << plainOk
                      << " (c) remapPlacedEquals=" << remapOk << " remapVsPlainSourceDiff=" << remapMatters
                      << '\n';
            tally.gate(8, calls == 2 && !twoFrame.isNull() && plainOk && remapOk && remapMatters > 1000,
                       "renderFrameAt does not build its layers through renderClipSourceAt's function");
        }
    }

    // G9: cross dissolve on V1, A -> B (B carries the text). At the middle of
    // the overlap A is the selected clip and B is drawn as the transition
    // neighbour; its text must show (HEAD: the neighbour had no 3D text).
    {
        ClipInfo a = fullFrame(QColor(30, 30, 30));
        a.duration = 3.0;
        a.inPoint = 0.0;
        a.outPoint = 2.0;
        ClipInfo b = fullFrame(QColor(150, 150, 150));
        b.duration = 3.0;
        b.inPoint = 1.0;
        b.outPoint = 3.0;
        a.trailOut.type = b.leadIn.type = TransitionType::CrossDissolve;
        a.trailOut.duration = b.leadIn.duration = 1.0;
        a.trailOut.alignment = b.leadIn.alignment = TransitionAlignment::Center;
        const auto withTl = makeTimeline({{a, withText(b, QStringLiteral("X9"))}});
        const auto withoutTl = makeTimeline({{a, b}});
        const double mid = 2.0;
        const quint64 before = tlrender::clipLayerSourceCallsForTest();
        const QImage fa = frameAt(withTl.get(), mid);
        const quint64 calls = tlrender::clipLayerSourceCallsForTest() - before;
        const QImage fb = frameAt(withoutTl.get(), mid);
        const qint64 d = diffPixels(fa, fb);
        // The neighbour really blends: the dissolve frame differs from A alone
        // (at 1.0 s, before the overlap [1.5, 2.5)).
        const qint64 blend = diffPixels(fb, frameAt(withoutTl.get(), 1.0));
        std::cerr << "text3d-export G9 neighbourTextDiffPixels=" << d << " (HEAD 0)"
                  << " layerSourceCalls=" << calls << " (expect 2) dissolveVsAOnly=" << blend << '\n';
        tally.gate(9, d > 100 && calls == 2 && blend > 1000,
                   "3D text missing on the overlap-transition neighbour");
    }

    // G10: after the mask (user decision M). The clip's mask keeps only its
    // top-left 100x60 corner: outside it the clip body is cut (transparent
    // without text) while the text pixels there remain.
    {
        Mask mask;
        mask.shape = MaskShape::Rectangle;
        mask.rect = QRectF(0, 0, 100, 60);
        ClipInfo masked = plain;
        masked.maskSystem.addMask(mask);
        const auto withTl = makeTimeline({{withText(masked)}});
        const auto withoutTl = makeTimeline({{masked}});
        const QImage fa = frameAt(withTl.get(), t).convertToFormat(QImage::Format_RGBA8888);
        const QImage fb = frameAt(withoutTl.get(), t).convertToFormat(QImage::Format_RGBA8888);
        qint64 bodyOutside = 0;   // opaque body pixels outside the mask (expect 0)
        qint64 bodyInside = 0;    // opaque body pixels inside the mask
        qint64 textOutside = 0;   // text pixels outside the mask (HEAD 0)
        const QRect keep(0, 0, 100, 60);
        for (int y = 0; y < kH && !fa.isNull() && !fb.isNull(); ++y) {
            for (int x = 0; x < kW; ++x) {
                const int alphaB = qAlpha(fb.pixel(x, y));
                const bool inside = keep.contains(x, y);
                if (inside && alphaB == 255) ++bodyInside;
                if (!inside && alphaB != 0) ++bodyOutside;
                if (!inside && qAlpha(fa.pixel(x, y)) > 0) ++textOutside;
            }
        }
        std::cerr << "text3d-export G10 bodyInside=" << bodyInside << " bodyOutside=" << bodyOutside
                  << " (expect 0) textOutsideMask=" << textOutside << " (HEAD 0, mask-before 0)" << '\n';
        tally.gate(10, bodyInside > 4000 && bodyOutside == 0 && textOutside > 100,
                   "3D text cut by the clip mask, or the mask no longer cuts the clip");
    }

    return tally.finish("text3d-export");
}
