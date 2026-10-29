// US-1708 (brush-export): a clip's brush strokes (ClipInfo::brushData) are drawn into
// exports by tlrender::renderFrameAt on the clip's roto'd raw source, before VFX /
// grade (rule (78); the preview's non-composite-baked stage), for V1, upper layers and
// the overlap-transition neighbour alike; the preview's brush progress follows the
// playhead (user decision K).
#include "../BrushAnimation.h"
#include "../GLPreview.h"
#include "../Keyframe.h"
#include "../MainWindow.h"
#include "../Overlay.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoEffect.h"
#include "../VideoPlayer.h"
#include "../ClipGeometry.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QSettings>
#include <QPainter>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr int kFps = 10;

QJsonObject brushJson(const QString &text, const QPointF &position)
{
    BrushAnimation brush;
    brush.setText(text, QFont(QStringLiteral("Arial"), 110), position);
    brush.setBrushWidth(12.0);
    return brush.toJson();
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
    clip.displayName = QStringLiteral("brush-export shape");
    clip.duration = clip.outPoint = duration;
    clip.shapes = {rectangle};
    return clip;
}

ClipInfo fullFrame(const QColor &color)
{
    return shapeClip(color, QRectF(0, 0, kW, kH));
}

// brush_progress keys: (time, value) pairs.
ClipInfo withBrush(ClipInfo clip, const QJsonObject &brush,
                   const QVector<QPair<double, double>> &progressKeys)
{
    clip.brushData = brush;
    KeyframeTrack track(QStringLiteral("brush_progress"), 0.0);
    for (const auto &key : progressKeys)
        track.addKeyframe(key.first, key.second);
    clip.keyframes.addTrack(track);
    return clip;
}

ClipInfo fullBrush(ClipInfo clip, const QJsonObject &brush)
{
    return withBrush(clip, brush, {{0.0, 1.0}});
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

// Same size and the same RGBA8888 bytes (the preview uploads RGB888, the exporter
// keeps its own decode format: only the pixel values are compared).
bool samePixels(const QImage &a, const QImage &b)
{
    return diffPixels(a, b) == 0;
}

// The contract, built by hand: the strokes at `progress` drawn on the raw source
// with the same call the preview uses.
QImage handBrush(const QImage &raw, const QJsonObject &brush, double progress)
{
    BrushAnimation animation;
    animation.fromJson(brush);
    QImage out = raw;
    OverlayRenderer::renderBrushOverlay(out, &animation, progress);
    return out;
}

// The layer source up to the brush stage of clip 0 on V1 (the raw when the clip has
// no brush).
QImage rawAt(const Timeline *timeline, double sec)
{
    return tlrender::renderClipLayerSourceStageForTest(
        timeline, 0, 0, us(sec), tlrender::Stage::AfterBrush);
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

// mpeg4 clip of `w` x `h`, frame i a flat grey of luma 20 * (i % 12 + 1).
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
        std::cerr << "brush-export fixture open: " << *error << '\n';
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

// The image the preview would upload for `clip` (a movie clip carrying a brush at
// `progress`) at `sec`: a fresh VideoPlayer, stopped, proxy divisor 1, decodes the
// frame and the bound brush is drawn on it (GLPreview::uploadFrameForTest).
// `decodedSize` receives the decoded frame size; a null image is a failed setup.
// The playback proxy preference (default 1/2, a global that persists in QSettings) and
// the preview proxy divisor are both forced to full size for the call and restored.
struct FullSizeDecodeGuard {
    static constexpr const char *kPlaybackKey = "playbackProxyDivisor";
    static constexpr const char *kPreviewKey = "proxyDivisor";
    QSettings prefs{QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences")};
    const bool hadPlayback = prefs.contains(kPlaybackKey);
    const int oldPlayback = prefs.value(kPlaybackKey, 2).toInt();
    const bool hadPreview = prefs.contains(kPreviewKey);
    const QVariant oldPreview = prefs.value(kPreviewKey);
    FullSizeDecodeGuard() { setPlaybackProxyDivisorPreference(1); }
    ~FullSizeDecodeGuard()
    {
        // Same fallback as the player's own: env, else 2.
        bool envOk = false;
        const int envDivisor = qEnvironmentVariable("VEDITOR_PLAYBACK_PROXY").toInt(&envOk);
        setPlaybackProxyDivisorPreference(hadPlayback ? oldPlayback : (envOk ? envDivisor : 2));
        if (!hadPlayback)
            prefs.remove(kPlaybackKey);
        if (hadPreview)
            prefs.setValue(kPreviewKey, oldPreview);
        else
            prefs.remove(kPreviewKey);
    }
};

QImage previewUpload(const ClipInfo &clip, double progress, double sec, const QSize &expectedSize,
                     QSize *decodedSize, bool *glOn)
{
    FullSizeDecodeGuard guard;
    Timeline timeline;
    timeline.setProjectOutputConfig(kW, kH, false, kFps);
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, QVector<QVector<ClipInfo>>{},
                                0.0, -1.0, -1.0, 100);
    timeline.refreshPlaybackSequence();
    VideoPlayer player;
    GLPreview *view = player.glPreview();
    if (glOn) *glOn = player.isGLAccelerated();
    if (!view)
        return QImage();
    player.setProxyDivisor(1);
    BrushAnimation animation;
    animation.fromJson(clip.brushData);
    view->setBrushAnimation(&animation);
    view->setBrushAnimationProgress(progress);
    player.setSequence(timeline.computePlaybackSequence());
    const qint64 targetUs = us(sec);
    player.seek(qRound(sec * 1000.0));
    if (!QTest::qWaitFor([&] { return player.timelinePositionUs() == targetUs; }, 10000))
        return QImage();
    // Wait until the uploaded image is non-null, source-sized and stable across two reads.
    QImage upload;
    QImage previous;
    const bool settled = QTest::qWaitFor([&] {
        upload = view->uploadFrameForTest();
        const bool ok = !upload.isNull() && upload.size() == expectedSize
                        && !previous.isNull() && upload == previous;
        previous = upload;
        return ok;
    }, 10000);
    if (!settled) {
        view->clearBrushAnimation();
        return QImage();
    }
    if (decodedSize) *decodedSize = upload.size();
    view->clearBrushAnimation();
    return upload;
}

} // namespace

int runBrushExportSelftest()
{
    selftest::GateTally tally;
    const double t = 0.5;
    const QColor grey(90, 110, 130);
    const ClipInfo plain = fullFrame(grey);
    const QJsonObject brush = brushJson(QStringLiteral("B"), QPointF(30.0, 150.0));
    const ClipInfo brushed = fullBrush(plain, brush);

    // G1: the strokes are drawn (HEAD: renderFrameAt ignored brushData, so the two
    // frames were byte-identical).
    {
        const auto withTl = makeTimeline({{brushed}});
        const auto withoutTl = makeTimeline({{plain}});
        const qint64 d = diffPixels(frameAt(withTl.get(), t), frameAt(withoutTl.get(), t));
        std::cerr << "brush-export G1 brushVsPlainDiffPixels=" << d << " (HEAD 0)" << '\n';
        tally.gate(1, d > 200, "brush strokes not drawn into renderFrameAt");
    }

    // G2: progress 0 and zero strokes are byte-unchanged; progress 1 is the hand
    // composite (brush on the raw, then VFX / grade / fx), also under an Echo whose
    // past frames carry no brush; the stage control: grading first then drawing
    // differs.
    {
        const auto plainTl = makeTimeline({{plain}});
        const QImage reference = frameAt(plainTl.get(), t);
        const auto zeroProgressTl = makeTimeline({{withBrush(plain, brush, {{0.0, 0.0}})}});
        const ClipInfo noStrokes = fullBrush(plain, brushJson(QString(), QPointF(30.0, 150.0)));
        const auto noStrokesTl = makeTimeline({{noStrokes}});
        const bool progressZeroSame = sameBytes(frameAt(zeroProgressTl.get(), t), reference);
        const bool noStrokesSame = sameBytes(frameAt(noStrokesTl.get(), t), reference)
            && !noStrokes.brushData.isEmpty();

        ClipInfo gradedPlain = plain;
        gradedPlain.colorCorrection.brightness = -50.0;
        const ClipInfo graded = fullBrush(gradedPlain, brush);
        const auto gradedTl = makeTimeline({{graded}});
        const auto gradedPlainTl = makeTimeline({{gradedPlain}});
        const QImage raw = rawAt(gradedPlainTl.get(), t);
        const QImage hand = raw.isNull() ? QImage()
            : tlrender::applyClipFxStackFromSource(handBrush(raw, brush, 1.0), graded, t);
        const QImage handAfterGrade = raw.isNull() ? QImage()
            : handBrush(tlrender::applyClipFxStackFromSource(raw, graded, t), brush, 1.0);
        const QImage source = tlrender::renderClipSourceAt(gradedTl.get(), 0, 0, us(t));
        const bool gradedSame = sameBytes(source, hand);
        const qint64 orderMatters = diffPixels(hand, handAfterGrade);
        const qint64 brushVisible = diffPixels(source,
            tlrender::renderClipSourceAt(gradedPlainTl.get(), 0, 0, us(t)));

        ClipInfo echoPlain = plain;
        echoPlain.effects.append(VideoEffect::createEcho(0.1, 2, 0.5, 0));
        const ClipInfo echoBrushed = fullBrush(echoPlain, brush);
        const auto echoTl = makeTimeline({{echoBrushed}});
        const auto echoPlainTl = makeTimeline({{echoPlain}});
        const QImage echoRaw = rawAt(echoPlainTl.get(), t);
        const tlrender::EchoFrameProvider pastFrames =
            [&](double, double sampleLocalSec) -> QImage {
                return tlrender::prepareClipSourceForEcho(echoRaw, echoBrushed, sampleLocalSec);
            };
        const QImage echoHand = echoRaw.isNull() ? QImage()
            : tlrender::applyClipFxStackWithEchoFromSource(
                  handBrush(echoRaw, brush, 1.0), echoBrushed, t, t, pastFrames);
        const QImage echoSource = tlrender::renderClipSourceAt(echoTl.get(), 0, 0, us(t));
        const bool echoSame = sameBytes(echoSource, echoHand);
        const qint64 echoBrushVisible = diffPixels(echoSource,
            tlrender::renderClipSourceAt(echoPlainTl.get(), 0, 0, us(t)));

        std::cerr << "brush-export G2 progress0Same=" << progressZeroSame
                  << " noStrokesSame=" << noStrokesSame << " gradedSourceEqualsHand=" << gradedSame
                  << " orderMatters=" << orderMatters << " brushVisible=" << brushVisible
                  << " echoSourceEqualsHand=" << echoSame
                  << " echoBrushVisible=" << echoBrushVisible << '\n';
        tally.gate(2, progressZeroSame && noStrokesSame && gradedSame && orderMatters > 50
                          && brushVisible > 200 && echoSame && echoBrushVisible > 200,
                   "brush stage differs from raw -> brush -> VFX / grade / fx, or progress 0 / "
                   "zero strokes changed the frame");
    }

    // G3: brush_progress 0 -> 1 over the clip: the drawn area grows with the time.
    {
        const ClipInfo growing = withBrush(plain, brush, {{0.0, 0.0}, {2.0, 1.0}});
        const auto growingTl = makeTimeline({{growing}});
        const auto plainTl = makeTimeline({{plain}});
        const double times[] = {0.0, 0.5, 1.0, 1.5, 1.95};
        qint64 area[5];
        for (int i = 0; i < 5; ++i)
            area[i] = diffPixels(frameAt(growingTl.get(), times[i]), frameAt(plainTl.get(), times[i]));
        std::cerr << "brush-export G3 area=" << area[0] << ',' << area[1] << ',' << area[2] << ','
                  << area[3] << ',' << area[4] << '\n';
        tally.gate(3, area[0] == 0 && area[1] > 0 && area[1] < area[2] && area[2] < area[3]
                          && area[3] < area[4],
                   "brush area does not grow with brush_progress");
    }

    // G4: two clips on two tracks, each with its own brush: both show. V1 covers
    // only the left half, V2 fills the frame; the V2 brush sits in the right half.
    {
        const ClipInfo left = shapeClip(QColor(200, 40, 40), QRectF(0, 0, kW / 2, kH));
        const ClipInfo leftBrushed = fullBrush(left, brushJson(QStringLiteral("L"), QPointF(30.0, 150.0)));
        const ClipInfo back = fullFrame(QColor(40, 40, 200));
        const ClipInfo backBrushed = fullBrush(back, brushJson(QStringLiteral("R"), QPointF(200.0, 150.0)));
        const auto withTl = makeTimeline({{leftBrushed}, {backBrushed}});
        const auto withoutTl = makeTimeline({{left}, {back}});
        const auto onlyV1Tl = makeTimeline({{leftBrushed}, {back}});
        const QImage fa = frameAt(withTl.get(), t);
        const qint64 leftHalf = diffPixels(fa, frameAt(withoutTl.get(), t), QRect(0, 0, kW / 2, kH));
        const qint64 rightHalf = diffPixels(fa, frameAt(withoutTl.get(), t),
                                            QRect(kW / 2, 0, kW - kW / 2, kH));
        // Control: without the V2 brush the right half is unchanged.
        const qint64 rightWithoutV2Brush = diffPixels(frameAt(onlyV1Tl.get(), t),
            frameAt(withoutTl.get(), t), QRect(kW / 2, 0, kW - kW / 2, kH));
        std::cerr << "brush-export G4 leftHalfDiff=" << leftHalf << " rightHalfDiff=" << rightHalf
                  << " rightHalfWithoutV2Brush=" << rightWithoutV2Brush << '\n';
        tally.gate(4, leftHalf > 100 && rightHalf > 100 && rightWithoutV2Brush == 0,
                   "the brush of one of two clips on different tracks is missing");
    }

    // G5: dx / scale move the brush with its clip (frame == hand placement of the
    // brushed raw); an opaque clip in front hides it where it covers.
    {
        ClipInfo moved = plain;
        moved.videoDx = 0.1;
        moved.videoScale = 0.8;
        const ClipInfo movedBrushed = fullBrush(moved, brush);
        const auto withTl = makeTimeline({{movedBrushed}});
        const auto withoutTl = makeTimeline({{moved}});
        const QImage raw = rawAt(withoutTl.get(), t);
        const QImage expected = raw.isNull() ? QImage()
            : handPlaceV1(handBrush(raw, brush, 1.0), moved);
        const QImage actual = frameAt(withTl.get(), t);
        const qint64 d = diffPixels(actual, expected);
        const qint64 visible = diffPixels(actual, frameAt(withoutTl.get(), t));
        const qint64 untransformed = diffPixels(actual,
            raw.isNull() ? QImage() : handBrush(raw, brush, 1.0));

        const ClipInfo cover = shapeClip(QColor(200, 40, 40), QRectF(0, 0, kW / 2, kH));
        const ClipInfo layer = fullFrame(QColor(40, 40, 200));
        const auto coveredTl = makeTimeline({{}, {cover}, {fullBrush(layer, brush)}});
        const auto coveredPlainTl = makeTimeline({{}, {cover}, {layer}});
        const qint64 covered = diffPixels(frameAt(coveredTl.get(), t), frameAt(coveredPlainTl.get(), t),
                                          QRect(0, 0, kW / 2, kH));
        const auto uncoveredTl = makeTimeline({{}, {}, {fullBrush(layer, brush)}});
        const auto uncoveredPlainTl = makeTimeline({{}, {}, {layer}});
        const qint64 uncovered = diffPixels(frameAt(uncoveredTl.get(), t),
            frameAt(uncoveredPlainTl.get(), t), QRect(0, 0, kW / 2, kH));
        std::cerr << "brush-export G5 placedVsHandDiff=" << d << " brushVisible=" << visible
                  << " untransformedDiff=" << untransformed << " coveredHalfDiff=" << covered
                  << " (expect 0) uncoveredHalfDiff=" << uncovered << '\n';
        tally.gate(5, d == 0 && visible > 100 && untransformed > 1000 && covered == 0 && uncovered > 50,
                   "the brush does not follow its clip's transform or is not hidden by the clip in front");
    }

    // The preview parity fixture: a 160x90 movie (frame i luma 20 * (i % 12 + 1)).
    QTemporaryDir dir;
    const QString movie = dir.filePath(QStringLiteral("brush-export.mp4"));
    QString whyNot;
    const bool encoderOk = selftest::requireEncoder(QStringLiteral("mpeg4"), &whyNot);
    const bool movieOk = encoderOk && dir.isValid() && writeClip(movie, 160, 90);
    ClipInfo movieClip;
    movieClip.filePath = movie;
    movieClip.duration = movieClip.outPoint = 2.0;
    movieClip.speed = 1.0;
    const QJsonObject movieBrush = brushJson(QStringLiteral("P"), QPointF(20.0, 75.0));
    movieClip = fullBrush(movieClip, movieBrush);
    ClipInfo movieClipPlain = movieClip;
    movieClipPlain.brushData = QJsonObject{};
    movieClipPlain.keyframes = KeyframeManager{};

    // G6: parity without any grade / fx / roto / mask: the image the non-composite
    // preview uploads (decoded frame at material resolution, stopped, proxy divisor 1,
    // brush drawn) == renderClipSourceAt. Not skipped: a missing encoder or movie fails.
    {
        QSize decoded;
        bool glOn = false;
        const QImage upload = movieOk ? previewUpload(movieClip, 1.0, t, QSize(160, 90), &decoded, &glOn) : QImage();
        const auto tl = makeTimeline({{movieClip}});
        const auto plainTl = makeTimeline({{movieClipPlain}});
        const QImage exported = tlrender::renderClipSourceAt(tl.get(), 0, 0, us(t));
        const bool same = samePixels(upload, exported);
        const qint64 brushVisible = diffPixels(exported,
            tlrender::renderClipSourceAt(plainTl.get(), 0, 0, us(t)));
        std::cerr << "brush-export G6 movie=" << movieOk << " gl=" << glOn << " decoded="
                  << decoded.width() << "x" << decoded.height() << " exported=" << exported.width()
                  << "x" << exported.height() << " uploadEqualsExport=" << same
                  << " diffPixels=" << diffPixels(upload, exported)
                  << " formats=" << int(upload.format()) << '/' << int(exported.format())
                  << " brushVisible=" << brushVisible << '\n';
        tally.gate(6, movieOk && decoded == QSize(160, 90) && same && brushVisible > 100,
                   "the preview's uploaded frame differs from renderClipSourceAt");
    }

    // G7: the same with a grade on the clip. The preview grades in the shader after
    // the upload, so the comparison is at the stage seam: the upload == the export's
    // brushed raw (AfterBrush), and the graded final source differs from both.
    {
        ClipInfo gradedMovie = movieClip;
        gradedMovie.colorCorrection.brightness = -50.0;
        QSize decoded;
        bool glOn = false;
        const QImage upload = movieOk ? previewUpload(gradedMovie, 1.0, t, QSize(160, 90), &decoded, &glOn) : QImage();
        const auto tl = makeTimeline({{gradedMovie}});
        const QImage stage = tlrender::renderClipLayerSourceStageForTest(
            tl.get(), 0, 0, us(t), tlrender::Stage::AfterBrush);
        const QImage finalSource = tlrender::renderClipSourceAt(tl.get(), 0, 0, us(t));
        const bool same = samePixels(upload, stage);
        const qint64 gradeVisible = diffPixels(finalSource, stage);
        std::cerr << "brush-export G7 movie=" << movieOk << " decoded=" << decoded.width() << "x"
                  << decoded.height() << " uploadEqualsAfterBrush=" << same
                  << " diffPixels=" << diffPixels(upload, stage)
                  << " gradeVisible=" << gradeVisible << '\n';
        tally.gate(7, movieOk && decoded == QSize(160, 90) && same && gradeVisible > 1000,
                   "the preview's uploaded frame differs from the export's after-brush stage");
    }

    // G8: the preview's progress follows the playhead through the real signal path:
    // a 1 s filler, then the brush clip at timeline 1 s with brush_progress 0 -> 1 over
    // its local 0-2 s. Timeline::positionChanged at 2.0 s is local 1 s (the keyframe
    // value there, not the one at local 2 s); 1.0 s is local 0 and 3.0 s is local 2
    // (HEAD: the value at time 0 whatever the playhead).
    {
        MainWindow window;
        window.hide();
        Timeline *mainTimeline = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
        GLPreview *view = window.m_player ? window.m_player->glPreview() : nullptr;
        double p0 = -1.0, p1 = -1.0, p2 = -1.0, expected1 = -1.0, expected2 = -1.0;
        bool bound = false;
        if (mainTimeline && view && movieOk) {
            ClipInfo filler = fullFrame(QColor(40, 40, 40));
            filler.duration = filler.outPoint = 1.0;
            ClipInfo growing = withBrush(movieClip, movieBrush, {{0.0, 0.0}, {2.0, 1.0}});
            mainTimeline->restoreFromProject(QVector<QVector<ClipInfo>>{{filler, growing}},
                                             QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
            mainTimeline->refreshPlaybackSequence();
            window.syncBrushAnimationPreviewForClip(0, 1);
            bound = window.brushPreviewClipIdForTest() == QStringLiteral("0:1");
            const KeyframeManager &keys = mainTimeline->videoTracks().value(0)->clips().at(1).keyframes;
            expected1 = keys.valueAt(QStringLiteral("brush_progress"), 1.0, 0.0);
            expected2 = keys.valueAt(QStringLiteral("brush_progress"), 2.0, 0.0);
            emit mainTimeline->positionChanged(1.0);
            p0 = view->brushAnimationProgressForTest();
            emit mainTimeline->positionChanged(2.0);
            p1 = view->brushAnimationProgressForTest();
            emit mainTimeline->positionChanged(3.0);
            p2 = view->brushAnimationProgressForTest();
        }
        // Nested-sequence parent: a 2 s parent clip of two 1 s children emits one playback
        // interval per child; the local time is measured from the parent's own placement,
        // so a playhead inside the second child (1.5 s) is parent-local 1.5.
        double pn = -1.0, nestedExpected = -1.0, nestedFirstChild = -1.0;
        bool nestedBound = false;
        {
            MainWindow nestedWindow;
            nestedWindow.hide();
            Timeline *nt = nestedWindow.findChild<Timeline *>(QStringLiteral("mainTimeline"));
            GLPreview *nview = nestedWindow.m_player ? nestedWindow.m_player->glPreview() : nullptr;
            if (nt && nview && movieOk) {
                ClipInfo child = movieClip;
                child.brushData = QJsonObject{};
                child.keyframes = KeyframeManager{};
                child.outPoint = 1.0;
                TimelineSequence kids;
                kids.id = QStringLiteral("kids");
                kids.name = kids.id;
                kids.videoTracks = QVector<QVector<ClipInfo>>{{child, child}};
                ClipInfo parent;
                parent.sequenceRefId = kids.id;
                parent.filePath = timeline_nesting::sequenceClipFilePath(kids.id);
                parent.duration = parent.outPoint = 2.0;
                parent.opacity = 1.0;
                parent = withBrush(parent, movieBrush, {{0.0, 0.0}, {2.0, 1.0}});
                TimelineSequence main;
                main.id = QStringLiteral("main");
                main.name = main.id;
                main.videoTracks = QVector<QVector<ClipInfo>>{{parent}};
                nt->setSequences(QVector<TimelineSequence>{main, kids}, main.id);
                nt->refreshPlaybackSequence();
                nestedWindow.syncBrushAnimationPreviewForClip(0, 0);
                nestedBound = nestedWindow.brushPreviewClipIdForTest() == QStringLiteral("0:0");
                const KeyframeManager &nkeys = nt->videoTracks().value(0)->clips().at(0).keyframes;
                nestedExpected = nkeys.valueAt(QStringLiteral("brush_progress"), 1.5, 0.0);
                nestedFirstChild = nkeys.valueAt(QStringLiteral("brush_progress"), 1.0, 0.0);
                emit nt->positionChanged(1.5);
                pn = nview->brushAnimationProgressForTest();
            }
        }
        // Export-rule cases (the preview must equal export keyframes.valueAt at the export
        // clip-local time, not at an offset from the earliest playback entry).
        // (a) B placed at 2 s with a 1 s centred dissolve: its playback entry starts at 1.5 s
        // (retracted), but at timeline 3 s (after the overlap) export local time is 1.0 s.
        double pa = -1.0, expectedA = -1.0, wrongA = -1.0;
        bool boundA = false;
        // (c) inside the dissolve the incoming B measures local time from its own interval
        // start (1.5 s), as export does, not from its placement (2.0 s).
        const double cTimes[3] = {1.5, 2.0, 2.49};
        double cProgress[3] = {-1.0, -1.0, -1.0}, cExpected[3] = {-2.0, -2.0, -2.0};
        double cPlacementMid = -1.0;
        {
            MainWindow winA;
            winA.hide();
            Timeline *ta = winA.findChild<Timeline *>(QStringLiteral("mainTimeline"));
            GLPreview *va = winA.m_player ? winA.m_player->glPreview() : nullptr;
            if (ta && va) {
                ClipInfo a = fullFrame(QColor(30, 30, 30));
                a.duration = 3.0;
                a.inPoint = 0.0;
                a.outPoint = 2.0;
                ClipInfo b = withBrush(fullFrame(QColor(150, 150, 150)), movieBrush,
                                       {{0.0, 0.0}, {2.0, 1.0}});
                b.duration = 3.0;
                b.inPoint = 1.0;
                b.outPoint = 3.0;
                a.trailOut.type = b.leadIn.type = TransitionType::CrossDissolve;
                a.trailOut.duration = b.leadIn.duration = 1.0;
                a.trailOut.alignment = b.leadIn.alignment = TransitionAlignment::Center;
                ta->restoreFromProject(QVector<QVector<ClipInfo>>{{a, b}},
                                       QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
                ta->refreshPlaybackSequence();
                winA.syncBrushAnimationPreviewForClip(0, 1);
                boundA = winA.brushPreviewClipIdForTest() == QStringLiteral("0:1");
                const KeyframeManager &ka = ta->videoTracks().value(0)->clips().at(1).keyframes;
                expectedA = ka.valueAt(QStringLiteral("brush_progress"), 1.0, 0.0);
                wrongA = ka.valueAt(QStringLiteral("brush_progress"), 1.5, 0.0);
                emit ta->positionChanged(3.0);
                pa = va->brushAnimationProgressForTest();
                for (int i = 0; i < 3; ++i) {
                    emit ta->positionChanged(cTimes[i]);
                    cProgress[i] = va->brushAnimationProgressForTest();
                    cExpected[i] = ka.valueAt(QStringLiteral("brush_progress"),
                                              cTimes[i] - 1.5, 0.0);
                }
                cPlacementMid = ka.valueAt(QStringLiteral("brush_progress"),
                                           qBound(0.0, cTimes[1] - 2.0, 2.0), 0.0);
            }
        }
        // (b) nested parent whose only child has a 1 s leading gap: the first child entry
        // starts at 1 s, but the parent is placed at 0 s, so timeline 1.5 s is local 1.5 s.
        double pb = -1.0, expectedB = -1.0, wrongB = -1.0;
        bool boundB = false;
        {
            MainWindow winB;
            winB.hide();
            Timeline *tb = winB.findChild<Timeline *>(QStringLiteral("mainTimeline"));
            GLPreview *vb = winB.m_player ? winB.m_player->glPreview() : nullptr;
            if (tb && vb && movieOk) {
                ClipInfo child = movieClip;
                child.brushData = QJsonObject{};
                child.keyframes = KeyframeManager{};
                child.outPoint = 1.0;
                child.leadInSec = 1.0;
                TimelineSequence kids;
                kids.id = QStringLiteral("gapkids");
                kids.name = kids.id;
                kids.videoTracks = QVector<QVector<ClipInfo>>{{child}};
                ClipInfo parent;
                parent.sequenceRefId = kids.id;
                parent.filePath = timeline_nesting::sequenceClipFilePath(kids.id);
                parent.duration = parent.outPoint = 2.0;
                parent.opacity = 1.0;
                parent = withBrush(parent, movieBrush, {{0.0, 0.0}, {2.0, 1.0}});
                TimelineSequence main;
                main.id = QStringLiteral("gapmain");
                main.name = main.id;
                main.videoTracks = QVector<QVector<ClipInfo>>{{parent}};
                tb->setSequences(QVector<TimelineSequence>{main, kids}, main.id);
                tb->refreshPlaybackSequence();
                winB.syncBrushAnimationPreviewForClip(0, 0);
                boundB = winB.brushPreviewClipIdForTest() == QStringLiteral("0:0");
                const KeyframeManager &kb = tb->videoTracks().value(0)->clips().at(0).keyframes;
                expectedB = kb.valueAt(QStringLiteral("brush_progress"), 1.5, 0.0);
                wrongB = kb.valueAt(QStringLiteral("brush_progress"), 0.5, 0.0);
                emit tb->positionChanged(1.5);
                pb = vb->brushAnimationProgressForTest();
            }
        }
        std::cerr << "brush-export G8 caseA bound=" << boundA << " progress@timeline3s=" << pa
                  << " keyframe@local1.0s=" << expectedA << " minEntryKeyframe@local1.5s=" << wrongA
                  << '\n';
        bool caseCOk = true;
        for (int i = 0; i < 3; ++i) {
            std::cerr << "brush-export G8 caseC t=" << cTimes[i] << " progress=" << cProgress[i]
                      << " keyframe@local" << (cTimes[i] - 1.5) << "=" << cExpected[i] << '\n';
            caseCOk = caseCOk && std::abs(cProgress[i] - cExpected[i]) < 1e-6;
        }
        std::cerr << "brush-export G8 caseC placementMid=" << cPlacementMid << '\n';
        caseCOk = caseCOk && std::abs(cProgress[1] - cPlacementMid) > 0.2;
        std::cerr << "brush-export G8 caseB bound=" << boundB << " progress@timeline1.5s=" << pb
                  << " keyframe@local1.5s=" << expectedB << " minEntryKeyframe@local0.5s=" << wrongB
                  << '\n';
        std::cerr << "brush-export G8 nested bound=" << nestedBound << " progress@1.5s=" << pn
                  << " keyframe@local1.5s=" << nestedExpected
                  << " firstChildClamp@1.0s=" << nestedFirstChild << '\n';
        std::cerr << "brush-export G8 bound=" << bound << " progress@timeline1s=" << p0
                  << " progress@timeline2s=" << p1 << " keyframe@local1s=" << expected1
                  << " progress@timeline3s=" << p2 << " keyframe@local2s=" << expected2 << '\n';
        tally.gate(8, bound && expected1 > 0.3 && expected1 < 0.7 && std::abs(p1 - expected1) < 1e-6
                          && std::abs(p1 - expected2) > 0.3
                          && std::abs(p0) < 1e-6 && std::abs(p2 - expected2) < 1e-6
                          && nestedBound && std::abs(nestedExpected - 0.75) < 1e-6
                          && std::abs(pn - nestedExpected) < 1e-6
                          && std::abs(pn - nestedFirstChild) > 0.2
                          && boundA && std::abs(expectedA - 0.5) < 1e-6
                          && std::abs(pa - expectedA) < 1e-6 && std::abs(pa - wrongA) > 0.2 && caseCOk
                          && boundB && std::abs(expectedB - 0.75) < 1e-6
                          && std::abs(pb - expectedB) < 1e-6 && std::abs(pb - wrongB) > 0.2,
                   "the preview brush progress does not follow the playhead");
    }

    // G9: cross dissolve on V1, A -> B (B carries the brush). At the middle of the
    // overlap A is the selected clip and B is drawn as the transition neighbour; its
    // strokes must show (HEAD: the neighbour had no brush).
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
        const auto withTl = makeTimeline({{a, fullBrush(b, brush)}});
        const auto withoutTl = makeTimeline({{a, b}});
        const double mid = 2.0;
        const quint64 before = tlrender::clipLayerSourceCallsForTest();
        const QImage fa = frameAt(withTl.get(), mid);
        const quint64 calls = tlrender::clipLayerSourceCallsForTest() - before;
        const QImage fb = frameAt(withoutTl.get(), mid);
        const qint64 d = diffPixels(fa, fb);
        const qint64 blend = diffPixels(fb, frameAt(withoutTl.get(), 1.0));
        std::cerr << "brush-export G9 neighbourBrushDiffPixels=" << d << " (HEAD 0)"
                  << " layerSourceCalls=" << calls << " (expect 2) dissolveVsAOnly=" << blend << '\n';
        tally.gate(9, d > 100 && calls == 2 && blend > 1000,
                   "brush missing on the overlap-transition neighbour");
    }

    // G10: the project-camera baked preview (trueProjection) shows renderFrameAt of the
    // whole timeline, which already draws every clip's brush (G1 / G4), and never draws
    // the bound brush again on top (brushedUploadFrame skips it when cameraBaked). Pin
    // (passes at HEAD): the uploaded frame == renderFrameAt of the brushed timeline even
    // with the preview brush bound at progress 1; the plain timeline's frame differs
    // (control), and so does a hand double-draw of the brush (the double-draw control).
    {
        Camera3DState state;
        state.trueProjection = true;
        const Camera3D camera{state};
        const auto brushedTl = makeTimeline({{brushed}});
        const auto plainTl = makeTimeline({{plain}});
        brushedTl->setProjectCamera(camera);
        plainTl->setProjectCamera(camera);
        const QImage expected = frameAt(brushedTl.get(), 0.0);
        const QImage plainFrame = frameAt(plainTl.get(), 0.0);

        VideoPlayer player;
        GLPreview *view = player.glPreview();
        const bool glOn = player.isGLAccelerated();
        if (!view || !glOn) {
            tally.skip(10, "OpenGL preview unavailable in this environment");
        } else {
            player.setProjectOutputSize(QSize(kW, kH));
            view->setTimeline(brushedTl.get());
            view->setProjectCamera(camera);
            BrushAnimation animation;
            animation.fromJson(brush);
            view->setBrushAnimation(&animation);
            view->setBrushAnimationProgress(1.0);
            view->grabFramebuffer(); // runs paintGL, which bakes m_projectCameraFrame
            const QImage upload = view->uploadFrameForTest();
            view->clearBrushAnimation();
            const bool same = sameBytes(upload, expected);
            const qint64 brushVisible = diffPixels(expected, plainFrame);
            const qint64 doubleDrawDiff = diffPixels(handBrush(expected, brush, 1.0), expected);
            std::cerr << "brush-export G10 uploadEqualsRenderFrameAt=" << same
                      << " uploadSize=" << upload.width() << "x" << upload.height()
                      << " brushVisible=" << brushVisible
                      << " doubleDrawDiff=" << doubleDrawDiff << '\n';
            tally.gate(10, same && brushVisible > 200 && doubleDrawDiff > 0,
                       "the camera-baked preview upload differs from renderFrameAt (brush "
                       "drawn twice or missing)");
        }
    }

    return tally.finish("brush-export");
}
