// US-2111 (stabilizer-export): V1's stabilizer keyframes reach the export
// (tlrender::renderFrameAt) through stabwarp::applyForExport, after the brush and
// before FX, with the matrix the GL preview copies (stabwarp::inverseMatrixUv) and
// clamp-to-edge bilinear sampling; the special-composite preview warps V1's layer
// the same way. Fixture: a 64x64 project, 60-frame `%03d` PNG sequences (black,
// opaque, 2x2 white dots) written here, so the fit is the identity and a source
// pixel lands on the same output pixel.
#include "../BrushAnimation.h"
#include "../GLPreview.h"
#include "../Keyframe.h"
#include "../MainWindow.h"
#include "../StabilizerWarp.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoPlayer.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

constexpr int kSize = 64;
constexpr int kFps = 30;
constexpr int kFrames = 60;
constexpr double kDuration = 2.0;

qint64 us(double sec) { return qint64(sec * 1000000.0); }

// A black opaque 64x64 frame with 2x2 white dots at the given top-left corners.
QImage dotFrame(const QVector<QPoint> &dots, int w = kSize, int h = kSize)
{
    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(QColor(0, 0, 0, 255));
    for (const QPoint &p : dots)
        for (int y = p.y(); y < p.y() + 2; ++y)
            for (int x = p.x(); x < p.x() + 2; ++x)
                if (x >= 0 && y >= 0 && x < w && y < h)
                    image.setPixelColor(x, y, QColor(255, 255, 255, 255));
    return image;
}

// Writes `frames` copies of `frame` as <dir>/<name>/f_%03d.png; returns the pattern.
QString writeSequence(const QString &dir, const QString &name, const QImage &frame,
                      int frames = kFrames)
{
    QDir(dir).mkpath(name);
    const QString base = dir + QLatin1Char('/') + name;
    for (int i = 0; i < frames; ++i)
        if (!frame.save(QStringLiteral("%1/f_%2.png").arg(base).arg(i, 3, 10, QLatin1Char('0'))))
            return QString();
    return base + QStringLiteral("/f_%03d.png");
}

ClipInfo seqClip(const QString &pattern, double duration = kDuration)
{
    ClipInfo clip;
    clip.filePath = pattern;
    clip.displayName = QStringLiteral("stabilizer-export");
    clip.duration = duration;
    clip.outPoint = duration;
    clip.opacity = 1.0;
    return clip;
}

StabilizerKeyframe key(double sec, double dx, double dy = 0.0, double theta = 0.0,
                       double scale = 1.0)
{
    StabilizerKeyframe kf;
    kf.timeUs = us(sec);
    kf.dx = dx;
    kf.dy = dy;
    kf.theta = theta;
    kf.scale = scale;
    return kf;
}

ClipInfo withKeys(ClipInfo clip, const QVector<StabilizerKeyframe> &keys)
{
    clip.stabilizerKeyframes = keys;
    return clip;
}

// "." in Arial 10 pt, brush width 2, at `position` (Step 0 S0-10's stroke), fully
// drawn (progress 1).
ClipInfo withBrush(ClipInfo clip, const QPointF &position)
{
    BrushAnimation brush;
    brush.setText(QStringLiteral("."), QFont(QStringLiteral("Arial"), 10), position);
    brush.setBrushWidth(2.0);
    clip.brushData = brush.toJson();
    KeyframeTrack track(QStringLiteral("brush_progress"), 0.0);
    track.addKeyframe(0.0, 1.0);
    clip.keyframes.addTrack(track);
    return clip;
}

std::unique_ptr<Timeline> makeTimeline(const QVector<QVector<ClipInfo>> &tracks)
{
    auto timeline = std::make_unique<Timeline>();
    timeline->setProjectOutputConfig(kSize, kSize, false, kFps);
    timeline->restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    return timeline;
}

QImage exportAt(const QVector<QVector<ClipInfo>> &tracks, double sec)
{
    const auto timeline = makeTimeline(tracks);
    return tlrender::renderFrameAt(timeline.get(), us(sec), QSize(kSize, kSize));
}

std::string sha(const QImage &image)
{
    if (image.isNull())
        return "null";
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < rgba.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(rgba.constScanLine(y)),
                                    rgba.width() * 4));
    return hash.result().toHex().left(16).toStdString();
}

// Bounding box (and count) of pixels in `region` passing `pick` on RGBA8888.
struct Box {
    int n = 0;
    QRect rect;
};

template <typename Pick>
Box boxOf(const QImage &image, const QRect &region, Pick pick)
{
    Box box;
    if (image.isNull())
        return box;
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    const QRect area = region.intersected(rgba.rect());
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const uchar *row = rgba.constScanLine(y);
        for (int x = area.left(); x <= area.right(); ++x) {
            if (!pick(row + x * 4))
                continue;
            ++box.n;
            box.rect = box.rect.isNull() ? QRect(x, y, 1, 1) : box.rect.united(QRect(x, y, 1, 1));
        }
    }
    return box;
}

Box whiteBox(const QImage &image, const QRect &region = QRect(0, 0, kSize, kSize))
{
    return boxOf(image, region,
                 [](const uchar *p) { return p[0] >= 254 && p[1] >= 254 && p[2] >= 254; });
}

Box nonBlackBox(const QImage &image, const QRect &region)
{
    return boxOf(image, region, [](const uchar *p) { return p[0] > 8 || p[1] > 8 || p[2] > 8; });
}

std::string boxText(const Box &box)
{
    if (box.rect.isNull())
        return "n=0 box=none";
    std::ostringstream out;
    out << "n=" << box.n << " box=(" << box.rect.left() << ',' << box.rect.top() << ")-("
        << box.rect.right() << ',' << box.rect.bottom() << ')';
    return out.str();
}

std::string matrixText(const std::array<float, 9> &m)
{
    std::string text = "[";
    char buf[32];
    for (int i = 0; i < 9; ++i) {
        std::snprintf(buf, sizeof buf, "%s%.9g", i ? " " : "", m[i]);
        text += buf;
    }
    return text + "]";
}

bool matrixNear(const std::array<float, 9> &m, const std::array<double, 9> &expected)
{
    for (int i = 0; i < 9; ++i)
        if (std::abs(double(m[i]) - expected[i]) > 1e-6)
            return false;
    return true;
}

// A 64x64 mpeg4 movie of `frame` (`frames` frames at 30 fps), for the special
// composite's grabFrameAt paths, which do not open `%03d` sequences.
bool writeMovie(const QString &path, const QImage &frame, int frames = kFrames)
{
    const QImage rgb = frame.convertToFormat(QImage::Format_RGB888);
    libavcore::EncodeRequest request;
    request.width = rgb.width();
    request.height = rgb.height();
    request.fps = request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 2000000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request))
        return false;
    for (int i = 0; i < frames; ++i)
        if (!encoder.pushFrameRgb24(rgb.constBits(), rgb.bytesPerLine(), i))
            return false;
    return !encoder.finalize();
}

// Pixels that differ between two same-size images (in RGBA8888).
Box diffBox(const QImage &a, const QImage &b)
{
    Box box;
    if (a.isNull() || a.size() != b.size())
        return box;
    const QImage ra = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage rb = b.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < ra.height(); ++y)
        for (int x = 0; x < ra.width(); ++x) {
            if (ra.pixel(x, y) == rb.pixel(x, y))
                continue;
            ++box.n;
            box.rect = box.rect.isNull() ? QRect(x, y, 1, 1) : box.rect.united(QRect(x, y, 1, 1));
        }
    return box;
}

} // namespace

int runStabilizerExportSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const QString seqA = writeSequence(dir, QStringLiteral("a"), dotFrame({QPoint(10, 10)}));
    const QString seqB = writeSequence(dir, QStringLiteral("b"), dotFrame({QPoint(34, 10)}));
    const QString seqC = writeSequence(dir, QStringLiteral("c"), dotFrame({QPoint(40, 40)}));
    // G3 samples t = 2 s, so its clip (and source) runs 3 s (90 frames at 30 fps).
    const QString seqLong =
        writeSequence(dir, QStringLiteral("long"), dotFrame({QPoint(10, 10)}), 3 * kFps);
    const bool fixtureOk = tmp.isValid() && !seqA.isEmpty() && !seqB.isEmpty() && !seqC.isEmpty()
        && !seqLong.isEmpty();
    std::cerr << "stabilizer-export fixture ok=" << fixtureOk << " dir="
              << dir.toStdString() << '\n';

    const ClipInfo plainA = seqClip(seqA);
    const double t = 0.5;
    const QImage plain = exportAt({{plainA}}, t);
    std::cerr << "stabilizer-export plain format=" << int(plain.format()) << " size="
              << plain.width() << 'x' << plain.height() << " sha=" << sha(plain)
              << " white " << boxText(whiteBox(plain)) << '\n';
    const bool plainOk = !plain.isNull() && whiteBox(plain).rect == QRect(10, 10, 2, 2);

    // G1 (pin): all-zero keys leave the export byte-identical to no keys. Control: a
    // dx +4 key changes it (so the comparison can see a warp).
    {
        const QImage zero = exportAt({{withKeys(plainA, {key(0.0, 0.0)})}}, t);
        const QImage moved = exportAt({{withKeys(plainA, {key(0.0, 4.0)})}}, t);
        std::ostringstream d;
        d << "plainSha=" << sha(plain) << " zeroKeySha=" << sha(zero)
          << " dx4Sha=" << sha(moved);
        tally.gate(1, plainOk && sha(zero) == sha(plain) && sha(moved) != sha(plain), d.str());
    }

    // G2: one key dx +4 -> the dot moves from (10,10) to (14,10) (output x reads
    // input x - dx). HEAD: unchanged, the dot at (10,10).
    {
        const QImage moved = exportAt({{withKeys(plainA, {key(0.0, 4.0)})}}, t);
        const Box box = whiteBox(moved);
        std::ostringstream d;
        d << "dx4 format=" << int(moved.format()) << " sha=" << sha(moved) << " white "
          << boxText(box);
        tally.gate(2, plainOk && box.n == 4 && box.rect == QRect(14, 10, 2, 2), d.str());
    }

    // G3: keys (0 s, dx 0) and (1 s, dx 8): at 0.5 s dx 4 (left column 14 +-1), at
    // 2 s clamped to dx 8 (left column 18).
    {
        const ClipInfo ramp = withKeys(seqClip(seqLong, 3.0), {key(0.0, 0.0), key(1.0, 8.0)});
        const QImage half = exportAt({{ramp}}, 0.5);
        const QImage end = exportAt({{ramp}}, 2.0);
        const Box halfBox = whiteBox(half);
        const Box endBox = whiteBox(end);
        const Box endAny = nonBlackBox(end, QRect(0, 0, kSize, kSize));
        std::ostringstream d;
        d << "t0.5 white " << boxText(halfBox) << " t2.0 null=" << end.isNull() << " white "
          << boxText(endBox) << " nonBlack " << boxText(endAny);
        const bool halfOk = !halfBox.rect.isNull() && std::abs(halfBox.rect.left() - 14) <= 1;
        const bool endOk = !endBox.rect.isNull() && endBox.rect.left() == 18;
        tally.gate(3, plainOk && halfOk && endOk, d.str());
    }

    // G4: dx -40 -> output x reads input x + 40: x 0..23 = input 40..63, x 24..63 =
    // input column 63 (clamp-to-edge, black here), alpha 255 everywhere (never a
    // transparent fill). The key-less export is the input (identity fit).
    {
        const QImage warped = exportAt({{withKeys(plainA, {key(0.0, -40.0)})}}, t);
        int mismatches = 0;
        int nonOpaque = 0;
        bool sized = !warped.isNull() && warped.size() == plain.size();
        if (sized) {
            const QImage in = plain.convertToFormat(QImage::Format_RGBA8888);
            const QImage out = warped.convertToFormat(QImage::Format_RGBA8888);
            for (int y = 0; y < kSize; ++y) {
                const uchar *ir = in.constScanLine(y);
                const uchar *orow = out.constScanLine(y);
                for (int x = 0; x < kSize; ++x) {
                    const int sx = qMin(x + 40, kSize - 1);
                    for (int c = 0; c < 4; ++c)
                        if (orow[x * 4 + c] != ir[sx * 4 + c]) {
                            ++mismatches;
                            break;
                        }
                    if (orow[x * 4 + 3] != 255)
                        ++nonOpaque;
                }
            }
        }
        const QImage outRgba = sized ? warped.convertToFormat(QImage::Format_RGBA8888) : QImage();
        const uchar *mid = sized ? outRgba.constScanLine(10) + 40 * 4 : nullptr;
        std::ostringstream d;
        d << "dx-40 sized=" << sized << " mismatchesVsClampedInput=" << mismatches
          << " nonOpaque=" << nonOpaque;
        if (mid)
            d << " out(40,10)=" << int(mid[0]) << ',' << int(mid[1]) << ',' << int(mid[2])
              << ',' << int(mid[3]);
        tally.gate(4, plainOk && sized && mismatches == 0 && nonOpaque == 0, d.str());
    }

    // G5: (a) V1 only, (b) the matrix (pin), (c) pixel centres / rotation centre and
    // the non-square UV mapping, (d) no stabilization inside a nested pass.
    {
        std::ostringstream d;
        // (a) the same key on V2 leaves V2's export unchanged. V1 is frontmost and
        // opaque (US-1804), so it sits at videoDx -0.375 (output x 0..39) and V2's
        // dot (40,40) shows beside it (G6 (c)'s layout).
        ClipInfo v1Left = seqClip(seqB);
        v1Left.videoDx = -0.375;
        const ClipInfo plainC = seqClip(seqC);
        const QImage v2Plain = exportAt({{v1Left}, {plainC}}, t);
        const QImage v2Keyed = exportAt({{v1Left}, {withKeys(plainC, {key(0.0, 4.0)})}}, t);
        const QRect v2Area(40, 36, kSize - 40, kSize - 36);
        const bool aOk = !v2Plain.isNull() && whiteBox(v2Plain, v2Area).rect == QRect(40, 40, 2, 2)
            && sha(v2Plain) == sha(v2Keyed);
        d << "(a) v2Plain sha=" << sha(v2Plain) << " V2 white " << boxText(whiteBox(v2Plain, v2Area))
          << " v2Keyed sha=" << sha(v2Keyed) << " V2 white " << boxText(whiteBox(v2Keyed, v2Area))
          << " ok=" << aOk;

        // (b) constants from the base exe's m_stabMatrix (Step 0 #3).
        const stabwarp::StabSample p2{4.0, 3.0, 0.1, 1.1};
        const stabwarp::StabSample p1{4.0, 0.0, 0.0, 1.0};
        const auto m2 = stabwarp::inverseMatrixUv(p2, kSize, kSize);
        const auto m1 = stabwarp::inverseMatrixUv(p1, kSize, kSize);
        const stabwarp::StabSample p3{4.0, 3.0, 0.0, 1.0};
        const auto fb0 = stabwarp::inverseMatrixUv(p3, 0, 0);
        const auto fbHd = stabwarp::inverseMatrixUv(p3, 1920, 1080);
        const bool bOk =
            matrixNear(m2, {0.904549241, -0.0907576531, -0.0625, 0.0907576531, 0.904549241,
                            -0.046875, 0, 0, 1})
            && matrixNear(m1, {1, 0, -0.0625, 0, 1, 0, 0, 0, 1}) && fb0 == fbHd
            && std::abs(double(fb0[2]) - -0.00208333344) <= 1e-6
            && std::abs(double(fb0[5]) - -0.00277777785) <= 1e-6;
        // GLPreview copies these values into uStab: after one 64x64 upload, its p2
        // matrix equals inverseMatrixUv's.
        GLPreview glView;
        glView.displayFrame(dotFrame({}));
        glView.setStabilizerKeyframes({key(0.0, 4.0, 3.0, 0.1, 1.1)});
        glView.setStabilizerSourceTimeUs(0);
        const auto gl = glView.stabMatrixForTest();
        const bool glOk = gl == m2;
        d << " (b) p2=" << matrixText(m2) << " p1=" << matrixText(m1)
          << " fallback0x0=" << matrixText(fb0) << " eq1920x1080=" << (fb0 == fbHd)
          << " glStabMatrix=" << matrixText(gl) << " equalsInverseMatrixUv=" << glOk
          << " ok=" << (bOk && glOk);

        // (c) theta pi/2 on a 2x2 dot at (10,10): exactly 4 pixels >= 254 at x 10..11,
        // y 52..53 and the ring around them <= 1.
        const stabwarp::StabSample quarter{0.0, 0.0, std::acos(-1.0) / 2.0, 1.0};
        const QImage rotated = stabwarp::warpClampToEdge(
            dotFrame({QPoint(10, 10)}), stabwarp::inverseTransform(quarter, kSize, kSize));
        const Box bright = boxOf(rotated, QRect(0, 0, kSize, kSize),
                                 [](const uchar *p) { return p[0] >= 254; });
        int ringMax = 0;
        int othersAbove1 = 0;
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x) {
                const int v = qRed(rotated.pixel(x, y));
                const bool core = x >= 10 && x <= 11 && y >= 52 && y <= 53;
                const bool ring = !core && x >= 9 && x <= 12 && y >= 51 && y <= 54;
                if (ring)
                    ringMax = qMax(ringMax, v);
                if (!core && v > 1)
                    ++othersAbove1;
            }
        const bool posOk = bright.n == 4 && bright.rect == QRect(10, 52, 2, 2) && ringMax <= 1;
        // 64x32, theta 0.1: inverseTransform's image of (10,10) / (50,20) equals the
        // UV-space mapping done by hand with inverseMatrixUv.
        const stabwarp::StabSample tilt{3.0, -2.0, 0.1, 1.0};
        const QTransform wide = stabwarp::inverseTransform(tilt, 64, 32);
        const auto mw = stabwarp::inverseMatrixUv(tilt, 64, 32);
        double maxErr = 0.0;
        for (const QPointF &pt : {QPointF(10, 10), QPointF(50, 20)}) {
            const double u = (pt.x() + 0.5) / 64.0 - 0.5;
            const double v = (pt.y() + 0.5) / 32.0 - 0.5;
            const double qx = mw[0] * u + mw[1] * v + mw[2];
            const double qy = mw[3] * u + mw[4] * v + mw[5];
            const QPointF expected((qx + 0.5) * 64.0 - 0.5, (qy + 0.5) * 32.0 - 0.5);
            const QPointF got = wide.map(pt);
            maxErr = qMax(maxErr, qMax(std::abs(got.x() - expected.x()),
                                       std::abs(got.y() - expected.y())));
            d << " (c) map(" << pt.x() << ',' << pt.y() << ")=(" << got.x() << ','
              << got.y() << ") hand=(" << expected.x() << ',' << expected.y() << ')';
        }
        const bool cOk = posOk && maxErr <= 1e-6;
        d << " (c) quarter bright " << boxText(bright) << " ringMax=" << ringMax
          << " othersAbove1=" << othersAbove1 << " nonSquareMaxErr=" << maxErr
          << " ok=" << cOk;

        // (d) the G2 key on V1 of a child sequence C, referenced from V1 of P: the
        // nested pass does not stabilize, so P renders like the key-less C.
        const auto nestedSha = [&](const ClipInfo &child) {
            TimelineSequence c;
            c.id = QStringLiteral("child");
            c.name = c.id;
            c.videoTracks = {{child}};
            ClipInfo ref;
            ref.sequenceRefId = c.id;
            ref.filePath = timeline_nesting::sequenceClipFilePath(c.id);
            ref.displayName = c.id;
            ref.duration = ref.outPoint = kDuration;
            ref.opacity = 1.0;
            TimelineSequence p;
            p.id = QStringLiteral("parent");
            p.name = p.id;
            p.videoTracks = {{ref}};
            Timeline timeline;
            timeline.setProjectOutputConfig(kSize, kSize, false, kFps);
            timeline.setSequences(QVector<TimelineSequence>{p, c}, p.id);
            const QImage image =
                tlrender::renderFrameAt(&timeline, us(t), QSize(kSize, kSize));
            d << " (d) nested white " << boxText(whiteBox(image));
            return sha(image);
        };
        const std::string nestedKeyed = nestedSha(withKeys(plainA, {key(0.0, 4.0)}));
        const std::string nestedPlain = nestedSha(plainA);
        const bool dOk = nestedPlain != "null" && nestedKeyed == nestedPlain;
        d << " (d) keyedSha=" << nestedKeyed << " plainSha=" << nestedPlain << " ok=" << dOk;
        tally.gate(5, plainOk && aOk && bOk && glOk && cOk && dOk, d.str());
    }

    // G6: the brush is drawn before the warp, so it moves with the source: (a) export,
    // (b) the AfterBrush stage seam, (c) the special composite (V1 layer only), (d) the
// GL's bound brush on a special composite without the brushes (V1's brush, a V2
// brush, a V1 source twice the canvas size; not run without GL acceleration).
    {
        std::ostringstream d;
        const QRect brushRegion(20, 20, kSize - 20, kSize - 20);
        const QRect dotRegion(0, 0, kSize, 20);
        const ClipInfo brushed = withBrush(plainA, QPointF(30.0, 30.0));
        const ClipInfo brushedKeyed = withKeys(brushed, {key(0.0, 4.0)});

        // (a) export.
        const QImage b0Image = exportAt({{brushed}}, t);
        const QImage aImage = exportAt({{brushedKeyed}}, t);
        const Box b0 = nonBlackBox(b0Image, brushRegion);
        const Box aBrush = nonBlackBox(aImage, brushRegion);
        const Box aDot = whiteBox(aImage, dotRegion);
        const bool aOk = b0.n > 0 && aDot.rect == QRect(14, 10, 2, 2)
            && aBrush.n == b0.n && aBrush.rect == b0.rect.translated(4, 0);
        d << "(a) B0 " << boxText(b0) << " keyed brush " << boxText(aBrush) << " dot "
          << boxText(aDot) << " ok=" << aOk;

        // (b) renderClipLayerSourceStageForTest(AfterBrush).
        const auto stage = [&](const ClipInfo &clip) {
            const auto timeline = makeTimeline({{clip}});
            return tlrender::renderClipLayerSourceStageForTest(
                timeline.get(), 0, 0, us(t), tlrender::Stage::AfterBrush, QSize(kSize, kSize));
        };
        const QImage s0Image = stage(brushed);
        const QImage sImage = stage(brushedKeyed);
        const Box s0 = nonBlackBox(s0Image, brushRegion);
        const Box sBrush = nonBlackBox(sImage, brushRegion);
        const bool bOk = s0.n > 0 && sBrush.n == s0.n && sBrush.rect == s0.rect.translated(4, 0)
            && whiteBox(sImage, dotRegion).rect == QRect(14, 10, 2, 2);
        d << " (b) stage format=" << int(sImage.format()) << " size=" << sImage.width() << 'x'
          << sImage.height() << " B0s " << boxText(s0) << " keyed " << boxText(sBrush)
          << " dot " << boxText(whiteBox(sImage, dotRegion)) << " ok=" << bOk;

        // (c) the special composite: V1 at videoDx -0.375 (-24 px) with the source dot
        // at (34,10), the brush at source (54,30) and key dx +4; V2 key-less with the
        // dot at (40,40) outside V1 (which covers output x 0..39).
        bool cOk = false;
        bool dOk = false;
        bool dRan = true;
        {
            MainWindow window;
            window.hide();
            Timeline *mainTimeline = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
            if (mainTimeline) {
                // MainWindow's canvas size follows the project geometry restored on undo.
                emit mainTimeline->projectOutputConfigRestored(kSize, kSize, false);
                ClipInfo v1Plain = seqClip(seqB);
                v1Plain.videoDx = -0.375;
                const ClipInfo v1Brushed = withBrush(v1Plain, QPointF(54.0, 30.0));
                const ClipInfo v1Keyed = withKeys(v1Brushed, {key(0.0, 4.0)});
                const ClipInfo v2 = seqClip(seqC);
                const auto composite = [&](const ClipInfo &v1, bool *wantsSpecial) {
                    mainTimeline->setProjectOutputConfig(kSize, kSize, false, kFps);
                    mainTimeline->restoreFromProject({{v1}, {v2}}, QVector<QVector<ClipInfo>>{},
                                                     0.0, -1.0, -1.0, 100);
                    mainTimeline->refreshPlaybackSequence();
                    *wantsSpecial = tlrender::timelineBrushWantsSpecialPath(mainTimeline, t);
                    return window.buildSpecialClipComposite(t);
                };
                bool wants0 = false;
                bool wants1 = false;
                const QImage c0 = composite(v1Brushed, &wants0);
                const QImage c1 = composite(v1Keyed, &wants1);
                const QRect v1BrushRegion(16, 20, 24, kSize - 20);
                const QRect v2Region(40, 36, kSize - 40, kSize - 36);
                const Box b0c = nonBlackBox(c0, v1BrushRegion);
                const Box cBrush = nonBlackBox(c1, v1BrushRegion);
                const Box c0V2 = whiteBox(c0, v2Region);
                const Box c1V2 = whiteBox(c1, v2Region);
                const Box c0Dot = whiteBox(c0, dotRegion);
                const Box c1Dot = whiteBox(c1, dotRegion);
                const bool premise = wants0 && wants1 && !c0.isNull()
                    && c0.size() == QSize(kSize, kSize) && c0V2.rect == QRect(40, 40, 2, 2)
                    && c0Dot.rect == QRect(10, 10, 2, 2) && b0c.n > 0;
                cOk = premise && c1Dot.rect == QRect(14, 10, 2, 2) && cBrush.n == b0c.n
                    && cBrush.rect == b0c.rect.translated(4, 0) && c1V2.rect == QRect(40, 40, 2, 2);
                // Source time: export's sourceSecondForClipAtLocalTime (inPoint +
                // local * speed, no remap / reverse) and the composite's fxSourceSeconds
                // (the same value bounded to [inPoint, out]).
                const double exportSourceSec = v1Keyed.inPoint + t * v1Keyed.speed;
                const double compositeSourceSec =
                    qBound(v1Keyed.inPoint, v1Keyed.inPoint + t * qMax(v1Keyed.speed, 0.0001),
                           v1Keyed.outPoint);
                const QImage decoded = tlrender::decodeClipSourceFrame(
                    seqB, exportSourceSec, false, v1Keyed.inPoint);
                d << " (c) wantsSpecial=" << wants0 << '/' << wants1 << " compositeFormat="
                  << int(c1.format()) << " decodedFormat=" << int(decoded.format())
                  << " exportSourceSec=" << exportSourceSec
                  << " compositeSourceSec=" << compositeSourceSec << " keyless: V1 dot "
                  << boxText(c0Dot) << " B0c " << boxText(b0c) << " V2 " << boxText(c0V2)
                  << " keyed: V1 dot " << boxText(c1Dot) << " brush " << boxText(cBrush)
                  << " V2 " << boxText(c1V2) << " premise=" << premise << " ok=" << cOk;


                // (d) a special composite without the brushes (an identity time
                // remap, a constant opacity keyframe: not the brush path), shown
                // paused through refreshSpecialClipPreview with V1's brush bound: the
                // GL draws that brush on it (the upload's pixels that differ from
                // the composite), moved +4 with V1's dot. The remap path decodes
                // with grabFrameAt, so V1 is an mpeg4 movie with a 4x4 dot at (10,10).
                VideoPlayer *player = window.m_player;
                GLPreview *view = player ? player->glPreview() : nullptr;
                const QString moviePath = dir + QStringLiteral("/dot.mp4");
                const bool movieOk = writeMovie(
                    moviePath, dotFrame({QPoint(10, 10), QPoint(12, 10), QPoint(10, 12),
                                         QPoint(12, 12)}));
                ClipInfo dPlain = withBrush(seqClip(moviePath), QPointF(30.0, 30.0));
                dPlain.timeRemapCurve.addKey(0.0, 0.0);
                dPlain.timeRemapCurve.addKey(kDuration, kDuration);
                KeyframeTrack opacity(QStringLiteral("opacity"), 1.0);
                opacity.addKeyframe(0.0, 1.0);
                dPlain.keyframes.addTrack(opacity);
                const ClipInfo dKeyed = withKeys(dPlain, {key(0.0, 4.0)});
                // A brushed V2 clip on the same path (behind V1, which is opaque).
                ClipInfo dV2 = withBrush(seqClip(moviePath), QPointF(40.0, 40.0));
                dV2.timeRemapCurve.addKey(0.0, 0.0);
                dV2.timeRemapCurve.addKey(kDuration, kDuration);
                dV2.keyframes.addTrack(opacity);
                // V1 at 128x128 on the 64x64 canvas: an 8x8 dot at (20,20) shows as a
                // 4x4 dot at (10,10).
                const QString bigMoviePath = dir + QStringLiteral("/dot128.mp4");
                QVector<QPoint> bigDots;
                for (int y = 20; y < 28; y += 2)
                    for (int x = 20; x < 28; x += 2)
                        bigDots.append(QPoint(x, y));
                const bool bigMovieOk =
                    writeMovie(bigMoviePath, dotFrame(bigDots, 2 * kSize, 2 * kSize));
                ClipInfo dBigPlain = dPlain;
                dBigPlain.filePath = bigMoviePath;
                const ClipInfo dBigKeyed = withKeys(dBigPlain, {key(0.0, 4.0)});
                const auto brightBox = [](const QImage &image, const QRect &region) {
                    return boxOf(image, region, [](const uchar *p) { return p[0] >= 128; });
                };
                // `tracks` shown with the brush of clip 0 on `bindTrack` bound.
                const auto shown = [&](const QVector<QVector<ClipInfo>> &tracks, int bindTrack,
                                       QImage *composite, bool *premise) {
                    mainTimeline->setProjectOutputConfig(kSize, kSize, false, kFps);
                    mainTimeline->restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0.0,
                                                     -1.0, -1.0, 100);
                    mainTimeline->refreshPlaybackSequence();
                    mainTimeline->setPlayheadPosition(t);
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    const bool wants = tlrender::timelineBrushWantsSpecialPath(mainTimeline, t);
                    bool drew = true;
                    *composite = window.buildSpecialClipComposite(t, &drew);
                    window.syncBrushAnimationPreviewForClip(bindTrack, 0);
                    const bool bound = window.brushPreviewClipIdForTest()
                        == QStringLiteral("%1:0").arg(bindTrack);
                    window.refreshSpecialClipPreview();
                    const QImage upload = view->uploadFrameForTest();
                    const bool marked =
                        view->property(tlrender::kSpecialCompositeHoldsBrushProperty).toBool();
                    *premise = !wants && !composite->isNull() && !drew && bound
                        && !player->isPlaying() && view->compositeBakedMode() && !marked
                        && upload.size() == QSize(kSize, kSize);
                    d << " (d) wantsSpecial=" << wants << " built=" << !composite->isNull()
                      << " drew=" << drew << " bound=" << bound << " baked="
                      << view->compositeBakedMode() << " marked=" << marked << " uploadFormat="
                      << int(upload.format()) << " size=" << upload.width() << 'x'
                      << upload.height();
                    return upload;
                };
                // The leg's verdict: V1's dot moves by `dotShift` in the composite and the
                // bound brush (the upload's pixels that differ from it) by `brushShift`.
                const auto leg = [&](const char *name, const QVector<QVector<ClipInfo>> &plain,
                                     const QVector<QVector<ClipInfo>> &keyed, int bindTrack,
                                     int dotShift, int brushShift) {
                    bool premise0 = false;
                    bool premise1 = false;
                    QImage c0d, c1d;
                    const QImage u0 = shown(plain, bindTrack, &c0d, &premise0);
                    const QImage u1 = shown(keyed, bindTrack, &c1d, &premise1);
                    const Box d0Brush = diffBox(u0, c0d);
                    const Box d1Brush = diffBox(u1, c1d);
                    const Box d0Dot = brightBox(c0d, dotRegion);
                    const Box d1Dot = brightBox(c1d, dotRegion);
                    const bool premise = premise0 && premise1 && d0Brush.n > 0
                        && d0Dot.n > 0 && d0Dot.rect.intersects(QRect(10, 10, 4, 4))
                        && !d0Brush.rect.intersects(dotRegion);
                    const bool ok = premise && d1Dot.n == d0Dot.n
                        && d1Dot.rect == d0Dot.rect.translated(dotShift, 0)
                        && d1Brush.n == d0Brush.n
                        && d1Brush.rect == d0Brush.rect.translated(brushShift, 0);
                    d << " (d" << name << ") keyless: dot " << boxText(d0Dot) << " brush "
                      << boxText(d0Brush) << " keyed: dot " << boxText(d1Dot) << " brush "
                      << boxText(d1Brush) << " expect dot +" << dotShift << " brush +"
                      << brushShift << " premise=" << premise << " ok=" << ok;
                    return ok;
                };
                if (!(player && view && player->isGLAccelerated())) {
                    // An environment cause: (d) is beyond the AC's G6 (a)-(c), which
                    // still decide the gate.
                    dRan = false;
                    d << " (d) not run: GL preview unavailable (environment)";
                } else if (!movieOk || !bigMovieOk) {
                    d << " (d) movieOk=" << movieOk << " bigMovieOk=" << bigMovieOk << " ok=0";
                } else {
                    // V1's own brush moves with V1.
                    const bool d1 = leg("1", {{dPlain}}, {{dKeyed}}, 0, 4, 4);
                    // A V2 clip's brush bound: export warps only V1's layer, so it stays.
                    const bool d2 = leg("2", {{dPlain}, {dV2}}, {{dKeyed}, {dV2}}, 1, 4, 0);
                    // V1 decoded at twice the canvas size: dx 4 source px = 2 canvas px.
                    const bool d3 = leg("3", {{dBigPlain}}, {{dBigKeyed}}, 0, 2, 2);
                    dOk = d1 && d2 && d3;
                    d << " (d) ok=" << dOk;
                }
            } else {
                d << " (c) mainTimeline missing";
            }
        }
        tally.gate(6, plainOk && aOk && bOk && cOk && (dOk || !dRan), d.str());
    }

    return tally.finish("stabilizer-export");
}
