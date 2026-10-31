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
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QSettings>
#include <QPainter>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>
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

// US-1805: `tracks` is the whole timeline (the bound brush is tracks[0][0]'s);
// `bindTimeline` hands it to the GLPreview as MainWindow does.
QImage previewUpload(const QVector<QVector<ClipInfo>> &tracks, double progress, double sec,
                     const QSize &expectedSize, QSize *decodedSize, bool *glOn,
                     bool bindTimeline, bool *bakedOut = nullptr,
                     const QHash<QString, QString> &parentEntries = {})
{
    FullSizeDecodeGuard guard;
    Timeline timeline;
    timeline.setProjectOutputConfig(kW, kH, false, kFps);
    timeline.restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    timeline.setClipParentEntries(parentEntries);
    timeline.refreshPlaybackSequence();
    VideoPlayer player;
    GLPreview *view = player.glPreview();
    if (glOn) *glOn = player.isGLAccelerated();
    if (!view)
        return QImage();
    if (bindTimeline)
        view->setTimeline(&timeline);
    player.setProxyDivisor(1);
    BrushAnimation animation;
    animation.fromJson(tracks.value(0).value(0).brushData);
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
    if (bakedOut) *bakedOut = view->compositeBakedMode();
    view->setTimeline(nullptr);
    if (!settled) {
        view->clearBrushAnimation();
        return QImage();
    }
    if (decodedSize) *decodedSize = upload.size();
    view->clearBrushAnimation();
    return upload;
}

QImage previewUpload(const ClipInfo &clip, double progress, double sec, const QSize &expectedSize,
                     QSize *decodedSize, bool *glOn)
{
    return previewUpload(QVector<QVector<ClipInfo>>{{clip}}, progress, sec, expectedSize,
                         decodedSize, glOn, false);
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

    // G11-G14 (US-1805): the special-composite preview (MainWindow) of a brushed
    // timeline. V1 = the movie with brush "P" at dx -0.25, V2 = the movie with
    // brush "Q" at dx +0.25 (a 160x180 overlap; V1 is frontmost, US-1804, so V2
    // shows on x 240-320 where its "Q" lies), both at progress 1, paused at t.
    {
        MainWindow window;
        window.hide();
        Timeline *mainTimeline = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
        GLPreview *view = window.m_player ? window.m_player->glPreview() : nullptr;
        const bool glOn = window.m_player && window.m_player->isGLAccelerated();
        window.m_projectConfig.width = kW;
        window.m_projectConfig.height = kH;
        window.m_projectConfig.fps = kFps;
        const QJsonObject v1Brush = brushJson(QStringLiteral("P"), QPointF(50.0, 75.0));
        const QJsonObject v2Brush = brushJson(QStringLiteral("Q"), QPointF(90.0, 85.0));
        ClipInfo v1Plain = movieClipPlain;
        v1Plain.videoDx = -0.25;
        const ClipInfo v1Clip = fullBrush(v1Plain, v1Brush);
        ClipInfo v2Plain = movieClipPlain;
        v2Plain.videoDx = 0.25;
        const ClipInfo v2Clip = fullBrush(v2Plain, v2Brush);
        // Pixel-identical in the export's format: the composite comes back
        // ARGB32_Premultiplied, renderFrameAt RGBA8888.
        const auto sameAsExport = [](const QImage &composite, const QImage &exported) {
            return !composite.isNull() && !exported.isNull()
                && composite.convertToFormat(exported.format()) == exported;
        };
        const auto setTracks = [&](const QVector<QVector<ClipInfo>> &tracks) {
            if (!mainTimeline)
                return;
            mainTimeline->setProjectOutputConfig(kW, kH, false, kFps);
            mainTimeline->restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0.0, -1.0,
                                             -1.0, 100);
            mainTimeline->refreshPlaybackSequence();
        };
        const auto formatOf = [](const QImage &image) { return int(image.format()); };
        // V1 = G1 [0, 0.5), a gap, G2 [0.8, 1.3); V2 = the brushed movie [0, 2).
        ClipInfo gapG1 = v1Clip;
        gapG1.outPoint = 0.5;
        ClipInfo gapG2 = v1Clip;
        gapG2.leadInSec = 0.3;
        gapG2.outPoint = 0.5;
        const auto settledUpload = [&] {
            QImage upload, previous;
            (void)QTest::qWaitFor([&] {
                upload = view->uploadFrameForTest();
                const bool stable = !upload.isNull() && !previous.isNull() && upload == previous;
                previous = upload;
                return stable;
            }, 5000);
            return upload;
        };
        // Paused at `sec` after `display` with V2's brush bound: the ordinary frame
        // with the brush drawn on it once (the G6 shape), on a timeline the helper
        // keeps true. `plain` is the same frame without the bound brush.
        int boundClip = 0;
        const auto ordinaryFrameShape = [&](const QImage &upload, QImage *plain) {
            window.syncBrushAnimationPreviewForClip(-1, -1);
            *plain = settledUpload();
            window.syncBrushAnimationPreviewForClip(1, boundClip);
            return samePixels(upload, handBrush(*plain, v2Brush, 1.0))
                && diffPixels(upload, *plain) > 100;
        };

        // G11: two brushed tracks: buildSpecialClipComposite(t) == renderFrameAt(t),
        // byte for byte once in the export's format (HEAD: brushData alone is no special data, so the composite
        // is null). Controls: both brushes show in the export (each differs from the
        // export without it), so a composite that drops either brush cannot match.
        QImage g11Composite;
        {
            QImage exported, v1Only, plainBoth;
            if (mainTimeline && movieOk) {
                setTracks({{v1Clip}, {v2Clip}});
                g11Composite = window.buildSpecialClipComposite(t);
                exported = frameAt(mainTimeline, t);
                setTracks({{v1Clip}, {v2Plain}});
                v1Only = frameAt(mainTimeline, t);
                setTracks({{v1Plain}, {v2Plain}});
                plainBoth = frameAt(mainTimeline, t);
                setTracks({{v1Clip}, {v2Clip}});
            }
            const bool same = sameAsExport(g11Composite, exported);
            const qint64 v2BrushVisible = diffPixels(exported, v1Only);
            const qint64 v1BrushVisible = diffPixels(v1Only, plainBoth);
            // A cut on one track picks one clip over export's [start, end): V2 =
            // A (brush "P", dx -0.25) [0, 0.87), B (brush "Q", dx +0.25, in 0.07)
            // [0.87, 1.47), a gap, C [1.67, 2.0); V1 = the brushed movie at dx -0.5
            // (the left half, so V2's right half shows).
            // Sampled just before, at and just after the cut (inside the old 1/120 s
            // slack, which drew A and B together) and in V2's gap.
            // Control: the cut changes the export.
            ClipInfo cutA = v1Clip;
            cutA.outPoint = 0.87;
            ClipInfo cutB = v2Clip;
            cutB.inPoint = 0.07;
            cutB.outPoint = 0.67;
            ClipInfo cutC = v1Clip;
            cutC.leadInSec = 0.2;
            cutC.outPoint = 0.33;
            ClipInfo cutV1 = movieClip;
            cutV1.videoDx = -0.5;
            bool cutSame = mainTimeline && movieOk;
            QString cutLog;
            qint64 cutVisible = -1;
            if (mainTimeline && movieOk) {
                setTracks({{cutV1}, {cutA, cutB, cutC}});
                for (const double cutT : {0.866, 0.87, 0.874, 1.57}) {
                    const QImage cutComposite = window.buildSpecialClipComposite(cutT);
                    const QImage cutExport = frameAt(mainTimeline, cutT);
                    const bool sameAtT = sameAsExport(cutComposite, cutExport);
                    cutSame = cutSame && sameAtT;
                    cutLog += QStringLiteral(" %1:%2/%3").arg(cutT).arg(int(sameAtT))
                                  .arg(diffPixels(cutComposite, cutExport));
                }
                cutVisible = diffPixels(frameAt(mainTimeline, 0.866), frameAt(mainTimeline, 0.874));
                setTracks({{v1Clip}, {v2Clip}});
            }
            // Off-grid times just after a source frame: export takes the first
            // frame at or after the source time, while grabFrameAt also accepts
            // one up to 1/120 s earlier. Then a reversed V1, whose export frame is
            // the last one before its source position (0.796 reads source 1.204,
            // frame 12; the composite's legacy offset lands on frame 11).
            // Control: the frame changes between 0.796 and 0.804, so a composite
            // holding the earlier frame cannot match.
            bool offGridSame = mainTimeline && movieOk;
            QString offGridLog;
            qint64 offGridVisible = -1;
            qint64 echoVisible = -1;
            qint64 reversedEchoVisible = -1;
            qint64 rollingShutterVisible = -1;
            const auto offGridSample = [&](double gridT) {
                const QImage gridComposite = window.buildSpecialClipComposite(gridT);
                const QImage gridExport = frameAt(mainTimeline, gridT);
                const bool sameAtT = sameAsExport(gridComposite, gridExport);
                offGridSame = offGridSame && sameAtT;
                offGridLog += QStringLiteral(" %1:%2/%3").arg(gridT).arg(int(sameAtT))
                                  .arg(diffPixels(gridComposite, gridExport));
            };
            if (mainTimeline && movieOk) {
                for (const double gridT : {0.804, 1.204})
                    offGridSample(gridT);
                offGridVisible = diffPixels(frameAt(mainTimeline, 0.796),
                                            frameAt(mainTimeline, 0.804));
                ClipInfo reversedV1 = v1Clip;
                reversedV1.reversed = true;
                setTracks({{reversedV1}, {v2Clip}});
                offGridLog += QStringLiteral(" reversed");
                offGridSample(0.796);
                // Echo on V1: the past frame comes from the same decode as export.
                // Forward at 0.904, the echo sample lands on source 0.804 (export
                // frame 0.9, grabFrameAt 0.8). Reversed at 0.896, it lands on
                // source 1.204, which the legacy shifted anchor would miss.
                // Blend 3 (Normal): Add (0) saturates this bright fixture at 255, so
                // a wrong echo frame would not show. Control: the echo changes both
                // renders.
                ClipInfo echoV1 = v1Clip;
                echoV1.effects.append(VideoEffect::createEcho(0.1, 1, 0.5, 3));
                setTracks({{echoV1}, {v2Clip}});
                offGridLog += QStringLiteral(" echo");
                offGridSample(0.904);
                const QImage echoForward = frameAt(mainTimeline, 0.904);
                ClipInfo reversedEchoV1 = echoV1;
                reversedEchoV1.reversed = true;
                setTracks({{reversedEchoV1}, {v2Clip}});
                offGridLog += QStringLiteral(" reversedEcho");
                offGridSample(0.896);
                const QImage echoReversed = frameAt(mainTimeline, 0.896);
                // Rolling shutter on V1: its second frame is the same decode too.
                ClipInfo shutterV1 = v1Clip;
                shutterV1.effects.append(VideoEffect::createRollingShutterRepair(1.0));
                setTracks({{shutterV1}, {v2Clip}});
                offGridLog += QStringLiteral(" rollingShutter");
                offGridSample(0.904);
                const QImage shutterFrame = frameAt(mainTimeline, 0.904);
                setTracks({{v1Clip}, {v2Clip}});
                rollingShutterVisible = diffPixels(shutterFrame, frameAt(mainTimeline, 0.904));
                echoVisible = diffPixels(echoForward, frameAt(mainTimeline, 0.904));
                setTracks({{reversedV1}, {v2Clip}});
                reversedEchoVisible = diffPixels(echoReversed, frameAt(mainTimeline, 0.896));
                setTracks({{v1Clip}, {v2Clip}});
            }
            // V1 gaps: renderFrameAt draws nothing when a visible, non-empty V1
            // has no export-active clip, so the composite must be null there too
            // (HEAD composited V2 alone). Samples inside G1 / G2 (both drawn,
            // byte-equal), at G1's end, inside the gap and at G2's end (the last
            // V1 clip). Then V1 = one clip with a 0.2 s lead-in at t = 0 (the
            // export's clamp onto clip 0). Control: the in-clip samples are
            // non-null, so the null agreement is not vacuous.
            ClipInfo leadV1 = v1Clip;
            leadV1.leadInSec = 0.2;
            bool gapSame = mainTimeline && movieOk;
            int gapDrawn = 0;
            QString gapLog;
            const auto gapSample = [&](double gapT) {
                const QImage gapComposite = window.buildSpecialClipComposite(gapT);
                const QImage gapExport = frameAt(mainTimeline, gapT);
                const bool nullSame = gapComposite.isNull() == gapExport.isNull();
                const bool sameAtT = nullSame
                    && (gapExport.isNull() || sameAsExport(gapComposite, gapExport));
                gapSame = gapSame && sameAtT;
                gapDrawn += gapExport.isNull() ? 0 : 1;
                gapLog += QStringLiteral(" %1:%2/%3/%4").arg(gapT).arg(int(gapComposite.isNull()))
                              .arg(int(gapExport.isNull())).arg(int(sameAtT));
            };
            if (mainTimeline && movieOk) {
                setTracks({{gapG1, gapG2}, {v2Clip}});
                for (const double gapT : {0.38, 0.5, 0.65, 0.88, 1.3})
                    gapSample(gapT);
                setTracks({{leadV1}, {v2Clip}});
                gapSample(0.0);
                setTracks({{v1Clip}, {v2Clip}});
            }
            // V1 with no decodable source (a zero-byte file) over the brushed V2:
            // renderFrameAt is null, so the composite is too. The samples above are
            // the non-null control; G12 covers what the preview then shows.
            QTemporaryFile zeroByteV1(QDir::temp().filePath(QStringLiteral("brush-XXXXXX.mp4")));
            bool zeroByteSame = false;
            QString zeroByteLog;
            if (mainTimeline && movieOk && zeroByteV1.open()) {
                zeroByteV1.close();
                ClipInfo undecodableV1 = v1Plain;
                undecodableV1.filePath = zeroByteV1.fileName();
                setTracks({{undecodableV1}, {v2Clip}});
                const QImage composite = window.buildSpecialClipComposite(t);
                const QImage exported = frameAt(mainTimeline, t);
                zeroByteSame = composite.isNull() && exported.isNull();
                zeroByteLog = QStringLiteral("%1/%2").arg(int(composite.isNull()))
                                  .arg(int(exported.isNull()));
                setTracks({{v1Clip}, {v2Clip}});
            }
            std::cerr << "brush-export G11 movie=" << movieOk << " compositeNull="
                      << g11Composite.isNull() << " (HEAD 1) compositeEqualsRenderFrameAt=" << same
                      << " diffPixels=" << diffPixels(g11Composite, exported)
                      << " formats=" << formatOf(g11Composite) << '/' << formatOf(exported)
                      << " v1BrushVisible=" << v1BrushVisible
                      << " v2BrushVisible=" << v2BrushVisible
                      << " cut t:equal/diffPixels=" << cutLog.toStdString()
                      << " cutVisible=" << cutVisible
                      << " offGrid t:equal/diffPixels=" << offGridLog.toStdString()
                      << " offGridVisible=" << offGridVisible
                      << " echoVisible=" << echoVisible
                      << " reversedEchoVisible=" << reversedEchoVisible
                      << " rollingShutterVisible=" << rollingShutterVisible
                      << " gap t:compositeNull/exportNull/equal=" << gapLog.toStdString()
                      << " gapDrawn=" << gapDrawn
                      << " zeroByteV1 compositeNull/exportNull=" << zeroByteLog.toStdString() << '\n';
            tally.gate(11, movieOk && same && v1BrushVisible > 100 && v2BrushVisible > 100
                               && cutSame && cutVisible > 100 && offGridSame && offGridVisible > 100
                               && echoVisible > 100 && reversedEchoVisible > 100
                               && rollingShutterVisible > 100
                               && gapSame && gapDrawn >= 2
                               && zeroByteSame,
                       "the special composite of two brushed tracks differs from renderFrameAt");
        }

        // G12: the same display through the real path: play, then pause, with V1's
        // brush bound to the GL preview at progress 1. The pause re-enters the
        // special composite at the playhead (no direct refresh here), so the upload
        // is buildSpecialClipComposite(playhead) (composite-baked, the bound brush
        // not drawn again). Controls: playback really ran and its frame differs
        // from that composite, the brush is really bound, and drawing it once more
        // on the composite changes it (a double draw cannot hide). The non-baked
        // control is G6 (the bound brush drawn on the decoded frame).
        if (!view || !glOn) {
            tally.skip(12, "OpenGL preview unavailable in this environment");
        } else {
            QImage upload, playUpload, pausedComposite;
            bool bound = false, baked = false, played = false, paused = false;
            bool markedBeforePlay = false, markedWhilePlaying = true;
            double pausedSec = -1.0;
            VideoPlayer *player = window.m_player;
            if (mainTimeline && movieOk && !g11Composite.isNull()) {
                mainTimeline->setPlayheadPosition(t);
                window.syncBrushAnimationPreviewForClip(0, 0);
                bound = window.brushPreviewClipIdForTest() == QStringLiteral("0:0");
                player->seek(qRound(t * 1000.0));
                (void)QTest::qWaitFor([&] { return player->timelinePositionUs() == us(t); }, 10000);
                markedBeforePlay = view->property(tlrender::kSpecialCompositeHoldsBrushProperty).toBool();
                player->play();
                played = QTest::qWaitFor([&] {
                    return player->isPlaying() && player->timelinePositionUs() > us(t);
                }, 10000);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                markedWhilePlaying = view->property(tlrender::kSpecialCompositeHoldsBrushProperty).toBool();
                playUpload = view->uploadFrameForTest();
                player->pause();
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                paused = !player->isPlaying();
                // Settle: two equal reads in a row.
                QImage previous;
                (void)QTest::qWaitFor([&] {
                    upload = view->uploadFrameForTest();
                    const bool stable = !upload.isNull() && !previous.isNull() && upload == previous;
                    previous = upload;
                    return stable;
                }, 5000);
                baked = view->compositeBakedMode();
                pausedSec = mainTimeline->playheadPosition();
                pausedComposite = window.buildSpecialClipComposite(pausedSec);
            }
            // A helper-true timeline falling back to the ordinary frame keeps the
            // selected brush: V2 bound, from a composite (mark set) to a null
            // composite by a plain seek and by the real refresh (the mark must not
            // go stale), and by a seek back onto a composite (the mark returns).
            // V1-gap: V1 = G1, gap, G2. Overlap: two plain clips at 0.5 and the
            // brushed V2 clip from 1.0. A paused seek un-bakes the frame, but a
            // compositor frame stays composite-baked when a preview seek refuses
            // a file switch; set here at the seam, only the mark keeps the brush.
            // Zero-byte V1: with V2 over it the ordinary V2 frame comes; alone, no
            // frame comes, so the composite stays, its brush not drawn again.
            bool fallbackOk = false, lateOk = false, zeroByteOk = false;
            QString fallbackLog;
            if (mainTimeline && movieOk && !g11Composite.isNull()) {
                player->setProjectOutputSize(QSize(kW, kH));
                player->setProxyDivisor(1);
                setTracks({{gapG1, gapG2}, {v2Clip}});
                window.syncBrushAnimationPreviewForClip(1, 0);
                // Pending seeks settle first, then the brush is bound (the player's
                // position update rebinds it to the selection).
                const auto seekTo = [&](double sec) {
                    player->seek(qRound(sec * 1000.0));
                    (void)QTest::qWaitFor([&] { return player->timelinePositionUs() == us(sec); }, 10000);
                    QTest::qWait(300);
                    window.syncBrushAnimationPreviewForClip(1, boundClip);
                    return settledUpload();
                };
                const auto refreshTo = [&](double sec) {
                    (void)seekTo(sec);
                    mainTimeline->setPlayheadPosition(sec);
                    window.syncBrushAnimationPreviewForClip(1, boundClip);
                    window.refreshSpecialClipPreview();
                    QTest::qWait(300);
                    return settledUpload();
                };
                const QImage drawnA = window.buildSpecialClipComposite(0.38);
                const bool refreshDrawn = !drawnA.isNull() && samePixels(refreshTo(0.38), drawnA);
                const QImage gapComposite = window.buildSpecialClipComposite(0.65);
                const QImage refreshed = refreshTo(0.65);
                QImage plain;
                const bool refreshShape = ordinaryFrameShape(refreshed, &plain);
                const bool seekDrawn = samePixels(refreshTo(0.38), drawnA);
                const QImage sought = seekTo(0.65);
                QImage soughtPlain;
                const bool seekShape = ordinaryFrameShape(sought, &soughtPlain);
                const QImage drawnB = window.buildSpecialClipComposite(0.88);
                const bool seekBack = !drawnB.isNull() && samePixels(seekTo(0.88), drawnB);
                fallbackOk = refreshDrawn && gapComposite.isNull() && refreshShape
                    && seekDrawn && seekShape && seekBack;
                fallbackLog = QStringLiteral("drawn=%1 compositeNull=%2 shape=%3 brushVisible=%4 "
                                             "seekDrawn=%5 seekShape=%6 seekBack=%7")
                                  .arg(int(refreshDrawn)).arg(int(gapComposite.isNull()))
                                  .arg(int(refreshShape)).arg(diffPixels(refreshed, plain))
                                  .arg(int(seekDrawn)).arg(int(seekShape)).arg(int(seekBack));

                ClipInfo plainV2Head = v2Plain;
                plainV2Head.outPoint = 1.0;
                setTracks({{v1Plain}, {plainV2Head, v2Clip}});
                boundClip = 1;
                const QImage drawnLate = window.buildSpecialClipComposite(1.2);
                const bool lateDrawn = !drawnLate.isNull() && samePixels(refreshTo(1.2), drawnLate);
                const QImage lateComposite = window.buildSpecialClipComposite(0.5);
                const QImage lateUpload = refreshTo(0.5);
                QImage latePlain;
                const bool lateShape = ordinaryFrameShape(lateUpload, &latePlain);
                view->setCompositeBakedMode(true);
                const bool bakedKeepsBrush = samePixels(settledUpload(), lateUpload);
                view->setCompositeBakedMode(false);
                lateOk = lateDrawn && lateComposite.isNull() && lateShape && bakedKeepsBrush;
                fallbackLog += QStringLiteral(" overlap drawn=%1 compositeNull=%2 bakedKeepsBrush=%3 shape=%4 brushVisible=%5")
                                   .arg(int(lateDrawn)).arg(int(lateComposite.isNull()))
                                   .arg(int(bakedKeepsBrush)).arg(int(lateShape))
                                   .arg(diffPixels(lateUpload, latePlain));
                boundClip = 0;

                QTemporaryFile zeroByteV1(QDir::temp().filePath(QStringLiteral("brush-XXXXXX.mp4")));
                if (zeroByteV1.open()) {
                    zeroByteV1.close();
                    ClipInfo undecodableV1 = v1Plain;
                    undecodableV1.filePath = zeroByteV1.fileName();
                    undecodableV1.outPoint = 0.5;
                    setTracks({{gapG1, undecodableV1}, {v2Clip}});
                    const QImage drawnZ = window.buildSpecialClipComposite(0.38);
                    const bool zeroDrawn = !drawnZ.isNull() && samePixels(refreshTo(0.38), drawnZ);
                    const QImage zeroComposite = window.buildSpecialClipComposite(0.65);
                    const QImage zeroExport = frameAt(mainTimeline, 0.65);
                    const QImage zeroUpload = refreshTo(0.65);
                    QImage zeroPlain;
                    const bool zeroShape = ordinaryFrameShape(zeroUpload, &zeroPlain);

                    ClipInfo lateV2 = v2Clip;
                    lateV2.leadInSec = 1.0;
                    setTracks({{gapG1, undecodableV1}, {lateV2}});
                    const QImage drawnS = window.buildSpecialClipComposite(0.38);
                    const bool staleDrawn = !drawnS.isNull() && samePixels(refreshTo(0.38), drawnS);
                    const QImage staleComposite = window.buildSpecialClipComposite(0.65);
                    const QImage staleUpload = refreshTo(0.65);
                    const qint64 staleDiff = diffPixels(staleUpload, drawnS);
                    const bool doubleDrawn = samePixels(staleUpload, handBrush(drawnS, v2Brush, 1.0));
                    zeroByteOk = zeroDrawn && zeroComposite.isNull() && zeroExport.isNull() && zeroShape
                        && staleDrawn && staleComposite.isNull() && staleDiff == 0 && !doubleDrawn;
                    fallbackLog += QStringLiteral(" zeroByte drawn=%1 null=%2/%3 shape=%4 alone drawn=%5 "
                                                  "null=%6 keptDiff=%7 doubleDrawn=%8")
                                       .arg(int(zeroDrawn)).arg(int(zeroComposite.isNull()))
                                       .arg(int(zeroExport.isNull())).arg(int(zeroShape))
                                       .arg(int(staleDrawn)).arg(int(staleComposite.isNull()))
                                       .arg(staleDiff).arg(int(doubleDrawn));
                }
                setTracks({{v1Clip}, {v2Clip}});
            }
            const bool same = samePixels(upload, pausedComposite);
            // The playback frame may come at a playback proxy size; any size
            // other than the composite's differs too.
            const qint64 playDiff = diffPixels(playUpload, pausedComposite);
            const bool playDiffers = !playUpload.isNull() && !pausedComposite.isNull()
                && (playUpload.size() != pausedComposite.size() || playDiff > 100);
            const qint64 doubleDrawDiff = pausedComposite.isNull() ? -1
                : diffPixels(handBrush(pausedComposite, v1Brush, 1.0), pausedComposite);
            std::cerr << "brush-export G12 bound=" << bound << " played=" << played
                      << " paused=" << paused << " pausedSec=" << pausedSec
                      << " mark before/while play=" << markedBeforePlay << '/' << markedWhilePlaying
                      << " compositeBaked=" << baked
                      << " uploadEqualsComposite=" << same
                      << " diffPixels=" << diffPixels(upload, pausedComposite)
                      << " uploadSize=" << upload.width() << "x" << upload.height()
                      << " playFrameSize=" << playUpload.width() << "x" << playUpload.height()
                      << " playFrameDiff=" << playDiff << " playFrameDiffers=" << playDiffers
                      << " doubleDrawDiff=" << doubleDrawDiff
                      << " fallback " << fallbackLog.toStdString() << '\n';
            tally.gate(12, bound && played && paused && baked && same && playDiffers
                               && markedBeforePlay && !markedWhilePlaying
                               && doubleDrawDiff > 100 && fallbackOk && lateOk && zeroByteOk,
                       "the composite-baked preview upload differs from the special composite "
                       "(bound brush drawn again or composite missing)");
            window.syncBrushAnimationPreviewForClip(-1, -1);
        }

        // G13: a non-default grade on V1 with its brush: the special composite grades
        // on the CPU after the brush, like renderFrameAt: byte-identical (HEAD: null).
        // Controls: the grade shows (graded export != ungraded export) and the brush
        // shows under it (graded export != graded export without the brush).
        {
            ClipInfo gradedV1 = v1Clip;
            gradedV1.colorCorrection.brightness = -50.0;
            gradedV1.colorCorrection.saturation = -40.0;
            ClipInfo gradedPlainV1 = v1Plain;
            gradedPlainV1.colorCorrection = gradedV1.colorCorrection;
            QImage composite, exported, ungraded, unbrushed;
            if (mainTimeline && movieOk) {
                setTracks({{gradedV1}, {v2Clip}});
                composite = window.buildSpecialClipComposite(t);
                exported = frameAt(mainTimeline, t);
                setTracks({{v1Clip}, {v2Clip}});
                ungraded = frameAt(mainTimeline, t);
                setTracks({{gradedPlainV1}, {v2Clip}});
                unbrushed = frameAt(mainTimeline, t);
            }
            const bool same = sameAsExport(composite, exported);
            const qint64 gradeVisible = diffPixels(exported, ungraded);
            const qint64 brushVisible = diffPixels(exported, unbrushed);
            std::cerr << "brush-export G13 compositeNull=" << composite.isNull() << " (HEAD 1)"
                      << " compositeEqualsRenderFrameAt=" << same
                      << " diffPixels=" << diffPixels(composite, exported)
                      << " formats=" << formatOf(composite) << '/' << formatOf(exported)
                      << " gradeVisible=" << gradeVisible << " brushVisible=" << brushVisible << '\n';
            tally.gate(13, movieOk && same && gradeVisible > 1000 && brushVisible > 100,
                       "graded brushed special composite differs from renderFrameAt");
        }

        // G14 (negative control): V2 with a non-Normal blend (Multiply) keeps the
        // legacy path. (1) buildSpecialClipComposite returns {} while the same
        // timeline with V2 Normal does not (the blend is what routes it away). (2)
        // The preview upload keeps the HEAD shape of G6: V2 starts after V1 (lead-in
        // 2 s) so at t the player decodes V1 alone, the GL holds the timeline, and
        // the bound brush is drawn on the decoded frame (== renderClipSourceAt).
        {
            ClipInfo blendV2 = v2Clip;
            blendV2.blendMode = BlendMode::Multiply;
            ClipInfo lateBlendV2 = blendV2;
            lateBlendV2.leadInSec = 2.0;
            ClipInfo lateNormalV2 = lateBlendV2;
            lateNormalV2.blendMode = BlendMode::Normal;
            QImage blendComposite, lateBlendComposite, normalComposite;
            if (mainTimeline && movieOk) {
                setTracks({{movieClip}, {blendV2}});
                blendComposite = window.buildSpecialClipComposite(t);
                setTracks({{movieClip}, {lateBlendV2}});
                lateBlendComposite = window.buildSpecialClipComposite(t);
                setTracks({{movieClip}, {lateNormalV2}});
                normalComposite = window.buildSpecialClipComposite(t);
            }
            QSize decoded;
            bool previewGl = false, baked = true;
            const QImage upload = movieOk
                ? previewUpload({{movieClip}, {lateBlendV2}}, 1.0, t, QSize(160, 90), &decoded,
                                &previewGl, true, &baked)
                : QImage();
            const auto tl = makeTimeline({{movieClip}, {lateBlendV2}});
            const QImage exported = tlrender::renderClipSourceAt(tl.get(), 0, 0, us(t));
            const auto plainTl = makeTimeline({{movieClipPlain}, {lateBlendV2}});
            const qint64 brushVisible = diffPixels(exported,
                tlrender::renderClipSourceAt(plainTl.get(), 0, 0, us(t)));
            const bool same = samePixels(upload, exported);
            // (4) A layer style (drop shadow) on V2 and V2 parented to V1: the
            // special composite applies neither, so both keep the legacy path (the
            // same timeline without them builds it: lateNormalCompositeNull=0), and
            // the upload keeps the G6 shape as in (2).
            ClipInfo styledV2 = lateNormalV2;
            styledV2.layerStyle.dropShadowEnabled = true;
            const QHash<QString, QString> parentV2{{QStringLiteral("1:0"), QStringLiteral("0:0")}};
            bool styleParentLegacy = movieOk && mainTimeline;
            QString styleParentLog;
            for (int c = 0; c < 2; ++c) {
                const ClipInfo &caseV2 = c == 0 ? styledV2 : lateNormalV2;
                const QHash<QString, QString> caseParents = c == 0 ? QHash<QString, QString>{} : parentV2;
                QImage caseComposite;
                if (mainTimeline && movieOk) {
                    setTracks({{movieClip}, {caseV2}});
                    mainTimeline->setClipParentEntries(caseParents);
                    caseComposite = window.buildSpecialClipComposite(t);
                    mainTimeline->setClipParentEntries({});
                }
                QSize caseDecoded;
                bool caseGl = false, caseBaked = true;
                const QImage caseUpload = movieOk
                    ? previewUpload({{movieClip}, {caseV2}}, 1.0, t, QSize(160, 90), &caseDecoded,
                                    &caseGl, true, &caseBaked, caseParents)
                    : QImage();
                const bool caseSame = samePixels(caseUpload, exported);
                styleParentLegacy = styleParentLegacy && caseComposite.isNull()
                    && caseDecoded == QSize(160, 90) && !caseBaked && caseSame;
                styleParentLog += QStringLiteral(" %1/%2/%3").arg(int(caseComposite.isNull()))
                                      .arg(int(caseSame)).arg(diffPixels(caseUpload, exported));
            }
            // (3) Otherwise-G11-eligible timelines the special composite would
            // misplace keep the legacy path: a keyframed position (it places layers
            // statically), fitContain / fitCover on a square source (it stretches to
            // the canvas), a fade-in lead-in / fade-out trail-out (it applies no
            // transitions), a shape clip and a sequence reference (it only decodes
            // filePath), a disabled clip (it would draw it). V1 keeps its brush, so
            // each helper term alone decides. The control: G11's pair, whose only
            // keyframe track is brush_progress, stays special.
            ClipInfo animatedV2 = v2Clip;
            KeyframeTrack posX(QStringLiteral("positionX"), 0.0);
            posX.addKeyframe(0.0, 0.0);
            posX.addKeyframe(2.0, 100.0);
            animatedV2.keyframes.addTrack(posX);
            const QString square = dir.filePath(QStringLiteral("brush-export-square.mp4"));
            const bool squareOk = movieOk && writeClip(square, 90, 90);
            ClipInfo fitV2 = v2Clip;
            fitV2.filePath = square;
            fitV2.fitContain = true;
            ClipInfo fadeV2 = v2Clip;
            fadeV2.leadIn.type = TransitionType::FadeIn;
            fadeV2.leadIn.duration = 0.5;
            ClipInfo shapeV2 = v2Clip;
            shapeV2.shapes = shapeClip(QColor(200, 40, 40), QRectF(0, 0, kW, kH)).shapes;
            ClipInfo sequenceV2 = v2Clip;
            sequenceV2.sequenceRefId = QStringLiteral("brush-export-sequence");
            ClipInfo disabledV2 = v2Clip;
            disabledV2.visible = false;
            ClipInfo coverV2 = v2Clip;
            coverV2.filePath = square;
            coverV2.fitCover = true;
            ClipInfo trailV2 = v2Clip;
            trailV2.trailOut.type = TransitionType::FadeOut;
            trailV2.trailOut.duration = 0.5;
            const bool brushProgressOnly = v2Clip.keyframes.tracks().size() == 1
                && v2Clip.keyframes.tracks().first().propertyName() == QStringLiteral("brush_progress");
            const bool controlSpecial = tlrender::timelineBrushWantsSpecialPath(
                makeTimeline({{v1Clip}, {v2Clip}}).get());
            bool excludedLegacy = squareOk;
            QString excludedLog;
            for (const ClipInfo &excludedV2 : {animatedV2, fitV2, fadeV2, shapeV2, sequenceV2,
                                                disabledV2, coverV2, trailV2}) {
                const bool special = tlrender::timelineBrushWantsSpecialPath(
                    makeTimeline({{v1Clip}, {excludedV2}}).get());
                QImage excludedComposite;
                if (mainTimeline && movieOk) {
                    setTracks({{v1Clip}, {excludedV2}});
                    excludedComposite = window.buildSpecialClipComposite(t);
                }
                excludedLegacy = excludedLegacy && !special && excludedComposite.isNull();
                excludedLog += QStringLiteral(" %1/%2").arg(int(special)).arg(int(excludedComposite.isNull()));
            }
            std::cerr << "brush-export G14 blendCompositeNull=" << blendComposite.isNull()
                      << " lateBlendCompositeNull=" << lateBlendComposite.isNull()
                      << " lateNormalCompositeNull=" << normalComposite.isNull()
                      << " (expect 1 / 1 / 0)"
                      << " gl=" << previewGl << " compositeBaked=" << baked << " decoded="
                      << decoded.width() << "x" << decoded.height()
                      << " uploadEqualsRenderClipSourceAt=" << same
                      << " diffPixels=" << diffPixels(upload, exported)
                      << " brushVisible=" << brushVisible
                      << " keyframed/fitContain/fadeIn/shape/sequenceRef/disabled/fitCover/trailOut"
                         " special/compositeNull="
                      << excludedLog.toStdString() << " (expect 0/1 x8) squareOk=" << squareOk
                      << " brushProgressOnlySpecial=" << controlSpecial << " (expect 1)"
                      << " brushProgressOnly=" << brushProgressOnly
                      << " layerStyle/parent compositeNull/uploadEquals/diffPixels="
                      << styleParentLog.toStdString() << " (expect 1/1/0 x2)" << '\n';
            tally.gate(14, movieOk && mainTimeline && blendComposite.isNull()
                               && lateBlendComposite.isNull() && !normalComposite.isNull() && decoded == QSize(160, 90)
                               && !baked && same && brushVisible > 100
                               && excludedLegacy && controlSpecial && brushProgressOnly
                               && styleParentLegacy,
                       "a blended brushed timeline left the legacy preview path (composite built "
                       "or the selected-clip brush upload changed)");
        }
    }

    return tally.finish("brush-export");
}
