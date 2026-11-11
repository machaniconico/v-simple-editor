// US-2111 (stabilizer-export): V1's stabilizer keyframes reach the export
// (tlrender::renderFrameAt) through stabwarp::applyForExport, after the brush and
// before FX, with the matrix the GL preview copies (stabwarp::inverseMatrixUv) and
// clamp-to-edge bilinear sampling; the special-composite preview warps V1's layer
// the same way. Fixture: a 64x64 project, 60-frame `%03d` PNG sequences (black,
// opaque, 2x2 white dots) written here, so the fit is the identity and a source
// pixel lands on the same output pixel. US-2300: every layer that carries keys is
// warped (upper tracks, a nested child at its own source time); G13 / G14.
#include "../BrushAnimation.h"
#include "../Camera3D.h"
#include "../OnionSkin.h"
#include "../GLPreview.h"
#include "../Keyframe.h"
#include "../MainWindow.h"
#include "../ShapeLayer.h"
#include "../StabilizerWarp.h"
#include "../StillCompare.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TextManager.h"
#include "../VideoPlayer.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"
#include "SelftestGl.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
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

    // G5: (a) a V2 key applies to the export too (US-2300; was "V1 only"), (b) the
    // matrix (pin), (c) pixel centres / rotation centre and the non-square UV
    // mapping, (d) a nested child's V1 moves by its key (US-2300; was "no
    // stabilization inside a nested pass").
    {
        std::ostringstream d;
        // (a) the same key on V2 moves V2's dot (40,40) -> (44,40). V1 is frontmost
        // and opaque (US-1804), so it sits at videoDx -0.375 (output x 0..39) and
        // V2's dot shows beside it (G6 (c)'s layout).
        ClipInfo v1Left = seqClip(seqB);
        v1Left.videoDx = -0.375;
        const ClipInfo plainC = seqClip(seqC);
        const QImage v2Plain = exportAt({{v1Left}, {plainC}}, t);
        const QImage v2Keyed = exportAt({{v1Left}, {withKeys(plainC, {key(0.0, 4.0)})}}, t);
        const QRect v2Area(40, 36, kSize - 40, kSize - 36);
        const Box v2KeyedBox = whiteBox(v2Keyed, v2Area);
        const bool aOk = !v2Plain.isNull() && whiteBox(v2Plain, v2Area).rect == QRect(40, 40, 2, 2)
            && v2KeyedBox.n == 4 && v2KeyedBox.rect == QRect(44, 40, 2, 2);
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
        // nested pass stabilizes the child, so P's dot moves (10,10) -> (14,10).
        Box nestedBox;
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
            nestedBox = whiteBox(image);
            d << " (d) nested white " << boxText(nestedBox);
            return sha(image);
        };
        const std::string nestedKeyed = nestedSha(withKeys(plainA, {key(0.0, 4.0)}));
        const Box nestedKeyedBox = nestedBox;
        const std::string nestedPlain = nestedSha(plainA);
        const bool dOk = nestedPlain != "null" && nestedBox.rect == QRect(10, 10, 2, 2)
            && nestedKeyedBox.n == 4 && nestedKeyedBox.rect == QRect(14, 10, 2, 2);
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

    // US-2202 G7: dx / dy are pixels of the analysed size (128x96) while the preview
    // shows a 64x48 proxy, so dx +4 is 2 pixels there: (a) the GL matrix, (b) the CPU
    // warp, (c) control: no stored size -> the frame's (+4 px, -0.0625; the export
    // passes none), (d) an entry built by the flatten whose filePath is the proxy,
    // presented on the player, (e) the special composite's layer and brush size.
    {
        std::ostringstream d;
        const QSize analysed(128, 96);
        constexpr int kProxyW = 64;
        constexpr int kProxyH = 48;
        const QImage proxyFrame = dotFrame({QPoint(10, 10)}, kProxyW, kProxyH);
        const auto m2Near = [](const std::array<float, 9> &m, double v) {
            return std::abs(double(m[2]) - v) <= 1e-7;
        };

        // (a) GL: a 64x48 upload, keys of the 128x96 analysis.
        GLPreview glA;
        glA.displayFrame(proxyFrame);
        glA.setStabilizerKeyframes({key(0.0, 4.0)});
        glA.setStabilizerSourceSize(analysed);
        glA.setStabilizerSourceTimeUs(0);
        const auto ma = glA.stabMatrixForTest();
        const bool aOk = m2Near(ma, -0.03125) && matrixNear(ma, {1, 0, -0.03125, 0, 1, 0, 0, 0, 1})
            && glA.stabEnabledForTest();
        d << "(a) gl m=" << matrixText(ma) << " enabled=" << glA.stabEnabledForTest()
          << " ok=" << aOk;

        // (b) CPU, and (c) the control without a stored size.
        ClipInfo sized = withKeys(seqClip(seqA), {key(0.0, 4.0)});
        sized.stabilizerSourceSize = analysed;
        const Box bBox = whiteBox(
            stabwarp::applyForExport(proxyFrame, sized, 0, 0, t, sized.stabilizerSourceSize));
        const bool bOk = bBox.n == 4 && bBox.rect == QRect(12, 10, 2, 2);
        const Box cBox = whiteBox(stabwarp::applyForExport(proxyFrame, sized, 0, 0, t));
        GLPreview glC;
        glC.displayFrame(proxyFrame);
        glC.setStabilizerKeyframes({key(0.0, 4.0)});
        glC.setStabilizerSourceSize(QSize());
        glC.setStabilizerSourceTimeUs(0);
        const auto mc = glC.stabMatrixForTest();
        const bool cOk = cBox.n == 4 && cBox.rect == QRect(14, 10, 2, 2) && m2Near(mc, -0.0625);
        d << " (b) cpu stored 128x96 white " << boxText(bBox) << " ok=" << bOk
          << " (c) pin: no size white " << boxText(cBox) << " gl m=" << matrixText(mc)
          << " ok=" << cOk;

        // (d) the flatten's entry (top-level V1), its filePath swapped to the 64x48
        // proxy as MainWindow's proxy pass does, composed and presented on `player`.
        bool dOk = false;
        bool dRan = false;
        {
            const QString proxySeq =
                writeSequence(dir, QStringLiteral("proxy48"), proxyFrame);
            const auto timeline = makeTimeline({{sized}});
            QVector<PlaybackEntry> seq = timeline->computePlaybackSequence();
            VideoPlayer player;
            GLPreview *view = player.glPreview();
            if (!(view && player.isGLAccelerated())) {
                d << " (d) not run: GL preview unavailable (environment)";
            } else if (seq.size() != 1 || proxySeq.isEmpty()) {
                dRan = true;
                d << " (d) entries=" << seq.size() << " proxyOk=" << !proxySeq.isEmpty()
                  << " ok=0";
            } else {
                dRan = true;
                seq[0].filePath = proxySeq;
                QImage canvasBase(kProxyW, kProxyH, QImage::Format_ARGB32_Premultiplied);
                canvasBase.fill(Qt::black);
                // V1 the way populateLayerMetadata fills it (no style, fit, matte or
                // parent here).
                VideoPlayer::DecodedLayer v1;
                v1.rgb = proxyFrame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                v1.sourceTrack = seq[0].sourceTrack;
                v1.sourceClipIndex = seq[0].sourceClipIndex;
                v1.sequenceIdx = 0;
                v1.fitContain = seq[0].fitContain;
                v1.fitCover = seq[0].fitCover;
                const QImage shown =
                    player.composeAndPresentForTest(canvasBase, {v1}, seq, 0, us(t));
                const auto md = view->stabMatrixForTest();
                const Box shownBox = whiteBox(shown, QRect(0, 0, kProxyW, kProxyH));
                dOk = seq[0].stabilizerSourceSize == analysed && m2Near(md, -0.03125)
                    && view->stabEnabledForTest() && shownBox.rect == QRect(10, 10, 2, 2);
                d << " (d) entry sourceSize=" << seq[0].stabilizerSourceSize.width() << 'x'
                  << seq[0].stabilizerSourceSize.height() << " track=" << seq[0].sourceTrack
                  << " composed white " << boxText(shownBox) << " player gl m="
                  << matrixText(md) << " enabled=" << view->stabEnabledForTest()
                  << " ok=" << dOk;
            }
        }

        // (e) the special composite on a 64x48 project decoding the 64x48 file with
        // keys of the 128x96 analysis (a brush puts it on the special path).
        bool eOk = false;
        {
            const QString seq48 = writeSequence(dir, QStringLiteral("e48"), proxyFrame);
            MainWindow window;
            window.hide();
            Timeline *mainTimeline =
                window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
            if (mainTimeline && !seq48.isEmpty()) {
                emit mainTimeline->projectOutputConfigRestored(kProxyW, kProxyH, false);
                ClipInfo eClip = withBrush(withKeys(seqClip(seq48), {key(0.0, 4.0)}),
                                           QPointF(40.0, 30.0));
                const auto build = [&](const ClipInfo &clip, QSize *size) {
                    mainTimeline->setProjectOutputConfig(kProxyW, kProxyH, false, kFps);
                    mainTimeline->restoreFromProject({{clip}}, QVector<QVector<ClipInfo>>{}, 0.0,
                                                     -1.0, -1.0, 100);
                    mainTimeline->refreshPlaybackSequence();
                    stabwarp::StabSample stab;
                    QString id;
                    return window.buildSpecialClipComposite(t, nullptr, &stab, size, &id);
                };
                QSize plainSize;
                QSize storedSize;
                const QImage ePlain = build(eClip, &plainSize);
                eClip.stabilizerSourceSize = analysed;
                const QImage eStored = build(eClip, &storedSize);
                const QRect dotArea(0, 0, kProxyW, 20);
                const Box plainDot = whiteBox(ePlain, dotArea);
                const Box storedDot = whiteBox(eStored, dotArea);
                eOk = plainSize == QSize(kProxyW, kProxyH) && plainDot.rect == QRect(14, 10, 2, 2)
                    && storedSize == analysed && storedDot.n == 4
                    && storedDot.rect == QRect(12, 10, 2, 2);
                d << " (e) no stored size: brushSize=" << plainSize.width() << 'x'
                  << plainSize.height() << " dot " << boxText(plainDot) << "; stored 128x96: "
                  << "brushSize=" << storedSize.width() << 'x' << storedSize.height()
                  << " dot " << boxText(storedDot) << " ok=" << eOk;
            } else {
                d << " (e) mainTimeline=" << (mainTimeline != nullptr)
                  << " seqOk=" << !seq48.isEmpty() << " ok=0";
            }
        }
        // Without GL only (d) is not run: the gate is a SKIP when the other legs
        // pass and a FAIL when one of them does not.
        if (!dRan && aOk && bOk && cOk && eOk)
            tally.skip(7, ("GL preview unavailable (environment); " + d.str()).c_str());
        else
            tally.gate(7, aOk && bOk && cOk && dOk && eOk, d.str());
    }

    // US-2202 G8: a size change rebuilds the matrix in displayFrame: 64x64 upload +
    // key dx +4 / time -> -0.0625, then a 128x96 frame -> -0.03125 with no new time.
    // Replacement leg: replaceClipMedia with a 64x48 file clears the clip's 128x96
    // stabilizerSourceSize, so the flattened entry has none and the GL preview falls
    // back to the frame size like stabwarp::applyForExport (+4 px, m[2] -0.0625);
    // undo restores 128x96 and redo clears it again.
    {
        GLPreview glView;
        glView.displayFrame(dotFrame({}));
        glView.setStabilizerKeyframes({key(0.0, 4.0)});
        glView.setStabilizerSourceTimeUs(0);
        const auto before = glView.stabMatrixForTest();
        glView.displayFrame(dotFrame({}, 128, 96));
        const auto after = glView.stabMatrixForTest();
        std::ostringstream d;
        d << "64x64 m=" << matrixText(before) << " after 128x96 displayFrame m="
          << matrixText(after) << " enabled=" << glView.stabEnabledForTest();
        bool ok = std::abs(double(before[2]) - -0.0625) <= 1e-7
            && std::abs(double(after[2]) - -0.03125) <= 1e-7 && glView.stabEnabledForTest();

        const QSize analysed(128, 96);
        const QImage frame48 = dotFrame({QPoint(10, 10)}, 64, 48);
        const QString movie48 = dir + QStringLiteral("/replace48.mp4");
        ClipInfo sized = withKeys(seqClip(seqA), {key(0.0, 4.0)});
        sized.stabilizerSourceSize = analysed;
        const auto timeline = makeTimeline({{sized}});
        const auto clipSize = [&] {
            return timeline->videoTracks().at(0)->clips().at(0).stabilizerSourceSize;
        };
        const auto sizeText = [](const QSize &size) {
            return size.isValid() ? std::to_string(size.width()) + 'x'
                    + std::to_string(size.height())
                                  : std::string("invalid");
        };
        QString message;
        const bool movieOk = writeMovie(movie48, frame48);
        const bool replaced = movieOk
            && timeline->replaceClipMedia(TrackKind::Video, 0, 0, movie48, QString(),
                                          kDuration, &message);
        const QSize afterReplace = clipSize();
        const bool r1 = replaced && !afterReplace.isValid();
        d << "; replace: movieOk=" << movieOk << " replaced=" << replaced << " msg="
          << message.toStdString() << " (1) clip size=" << sizeText(afterReplace)
          << " ok=" << r1;

        QVector<PlaybackEntry> seq = timeline->computePlaybackSequence();
        const QSize entrySize = seq.size() == 1 ? seq[0].stabilizerSourceSize : analysed;
        const Box cpuBox = seq.size() == 1
            ? whiteBox(stabwarp::applyForExport(frame48, timeline->videoTracks().at(0)
                                                             ->clips().at(0),
                                                0, 0, t))
            : Box{};
        bool r2 = seq.size() == 1 && !entrySize.isValid() && cpuBox.n == 4
            && cpuBox.rect == QRect(14, 10, 2, 2);
        bool glRan = false;
        {
            VideoPlayer player;
            GLPreview *view = player.glPreview();
            if (view && player.isGLAccelerated() && seq.size() == 1) {
                glRan = true;
                QImage canvasBase(64, 48, QImage::Format_ARGB32_Premultiplied);
                canvasBase.fill(Qt::black);
                VideoPlayer::DecodedLayer v1;
                v1.rgb = frame48.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                v1.sourceTrack = seq[0].sourceTrack;
                v1.sourceClipIndex = seq[0].sourceClipIndex;
                v1.sequenceIdx = 0;
                v1.fitContain = seq[0].fitContain;
                v1.fitCover = seq[0].fitCover;
                player.composeAndPresentForTest(canvasBase, {v1}, seq, 0, us(t));
                const auto m = view->stabMatrixForTest();
                r2 = r2 && view->stabEnabledForTest()
                    && std::abs(double(m[2]) - -0.0625) <= 1e-7;
                d << " (2) entry size=" << sizeText(entrySize) << " export white "
                  << boxText(cpuBox) << " gl m=" << matrixText(m)
                  << " enabled=" << view->stabEnabledForTest() << " ok=" << r2;
            } else {
                d << " (2) entry size=" << sizeText(entrySize) << " export white "
                  << boxText(cpuBox) << " gl not run: GL preview unavailable (environment)"
                  << " cpuOk=" << r2;
            }
        }

        timeline->undo();
        const QSize afterUndo = clipSize();
        timeline->redo();
        const QSize afterRedo = clipSize();
        const bool r3 = afterUndo == analysed && !afterRedo.isValid();
        d << " (3) undo size=" << sizeText(afterUndo) << " redo size=" << sizeText(afterRedo)
          << " ok=" << r3;
        ok = ok && r1 && r2 && r3;
        // Without GL only the GL m[2] check of (2) is not run: a SKIP when the other
        // checks pass and a FAIL when one of them does not.
        if (!glRan && ok)
            tally.skip(8, ("GL preview unavailable (environment); " + d.str()).c_str());
        else
            tally.gate(8, ok && glRan, d.str());
    }

    // US-2202 G9 (pin: (12) not reproducible at HEAD, F14 / F24): the GL stabilizer
    // state around a still compare, (a) on a player whose provider shows a special
    // composite (no keys throughout), (c) then on that same player once the provider
    // returns no image (keys and -0.0625 on the next present, none while comparing,
    // back after), and (b) on a fresh player showing the ordinary frame (keys and
    // -0.0625 from the first present, none while comparing, back after).
    {
        std::ostringstream d;
        d << "pin: (12) not reproducible at HEAD (F14/F24)";
        const QImage v1Frame = dotFrame({QPoint(10, 10)});
        const QImage stillImage = dotFrame({QPoint(50, 40)});
        const QImage specialImage = dotFrame({QPoint(30, 30)});
        bool ran = false;
        bool ok = true;
        for (int providerSpecial = 1; providerSpecial >= 0; --providerSpecial) {
            VideoPlayer player;
            GLPreview *view = player.glPreview();
            if (!(view && player.isGLAccelerated()))
                break;
            ran = true;
            int calls = 0;
            player.setSpecialCompositeProvider([&](qint64) {
                ++calls;
                return providerSpecial ? specialImage : QImage();
            });
            PlaybackEntry e;
            e.filePath = seqA;
            e.clipIn = 0.0;
            e.clipOut = kDuration;
            e.timelineStart = 0.0;
            e.timelineEnd = kDuration;
            e.speed = 1.0;
            e.sourceTrack = 0;
            e.sourceClipIndex = 0;
            e.stabilizerKeyframes = {key(0.0, 4.0)};
            QImage canvasBase(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
            canvasBase.fill(Qt::black);
            VideoPlayer::DecodedLayer v1;
            v1.rgb = v1Frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            v1.sequenceIdx = 0;
            const auto identity = [&] {
                return matrixNear(view->stabMatrixForTest(), {1, 0, 0, 0, 1, 0, 0, 0, 1});
            };
            const auto keyed = [&] {
                return view->stabEnabledForTest()
                    && matrixNear(view->stabMatrixForTest(), {1, 0, -0.0625, 0, 1, 0, 0, 0, 1});
            };
            const char *leg = providerSpecial ? "(a)" : "(b)";
            const auto state = [&](const char *tag) {
                d << ' ' << leg << ' ' << tag
                  << ": enabled=" << view->stabEnabledForTest()
                  << " m=" << matrixText(view->stabMatrixForTest()) << " calls=" << calls;
            };
            stillcompare::Config on;
            on.enabled = true;
            on.still = stillImage;
            stillcompare::Config off;
            const QImage shown = player.composeAndPresentForTest(canvasBase, {v1}, {e}, 0, us(t));
            state("present 1");
            const bool composedOk = whiteBox(shown).rect == QRect(10, 10, 2, 2);
            bool legOk = composedOk && calls == 1;
            if (providerSpecial) {
                legOk = legOk && !view->stabEnabledForTest() && identity();
                player.setStillCompare(on);
                state("compare on");
                legOk = legOk && calls == 2 && !view->stabEnabledForTest() && identity();
                player.setStillCompare(off);
                state("compare off");
                legOk = legOk && calls == 3 && !view->stabEnabledForTest() && identity();
                // (c) the same player, the provider now returning no image.
                leg = "(c)";
                player.setSpecialCompositeProvider([&](qint64) {
                    ++calls;
                    return QImage();
                });
                player.composeAndPresentForTest(canvasBase, {v1}, {e}, 0, us(t));
                state("present");
                legOk = legOk && calls == 4 && keyed();
                player.setStillCompare(on);
                state("compare on");
                legOk = legOk && calls == 5 && !view->stabEnabledForTest() && identity();
                player.setStillCompare(off);
                state("compare off");
                legOk = legOk && calls == 6 && keyed();
            } else {
                legOk = legOk && keyed();
                player.composeAndPresentForTest(canvasBase, {v1}, {e}, 0, us(t));
                state("present 2");
                legOk = legOk && keyed();
                player.setStillCompare(on);
                state("compare on");
                legOk = legOk && calls == 3 && !view->stabEnabledForTest() && identity();
                player.setStillCompare(off);
                state("compare off");
                legOk = legOk && calls == 4 && keyed();
            }
            d << " composedOk=" << composedOk << " legOk=" << legOk;
            ok = ok && legOk;
        }
        if (!ran)
            tally.skip(9, "GL preview unavailable (environment)");
        else
            tally.gate(9, ok, d.str());
    }

    // US-2203 G10: with other layers in the composite only V1's layer is stabilized,
    // on the CPU, and the GL warps nothing, the fact travelling with the image: (a)
    // G6 (c)'s layout without the brushes (V1 at videoDx -0.375, source dot (34,10),
    // key dx +4; V2 key-less, dot (40,40)) composed and presented on `player`: V1's
    // dot at (14,10), V2's still at (40,40); (a') control: V1 given as the canvas is
    // not warped; (b) the GL got no keys; (c) then V1 alone: keys and -0.0625 again;
    // (d) a re-present of the (a) frame (setTextOverlays) keeps the keys off; (e) the
    // composite frame cache returns the flag each image was put with; (f) the nested
    // SSOT present of a keyed V1 plus a disabled V2 (renderFrameAt has stabilized V1)
    // shows the dot at (14,10) with no GL keys.
    {
        std::ostringstream d;
        // (e) the cache payload (no GL needed).
        bool eOk = false;
        {
            playback::CompositeFrameCache cache;
            const QImage img = dotFrame({QPoint(10, 10)});
            const auto keyAt = [](qint64 ms) {
                return playback::CompositeFrameKey{1, ms, kSize, kSize, 0, false};
            };
            cache.put(keyAt(1), img, true);
            cache.put(keyAt(2), img, false);
            cache.put(keyAt(3), img);
            QImage o1, o2, o3;
            bool f1 = false;
            bool f2 = true;
            bool f3 = true;
            const bool h1 = cache.tryGet(keyAt(1), o1, &f1);
            const bool h2 = cache.tryGet(keyAt(2), o2, &f2);
            const bool h3 = cache.tryGet(keyAt(3), o3, &f3);
            eOk = h1 && h2 && h3 && f1 && !f2 && !f3 && o1 == img && o2 == img && o3 == img;
            d << "(e) hits=" << h1 << h2 << h3 << " flags k1=" << f1 << " k2=" << f2
              << " k3(2-arg put)=" << f3 << " ok=" << eOk;
        }

        bool ran = false;
        bool aOk = false, a2Ok = false, bOk = false, cOk = false, dOk = false, fOk = false;
        bool fRan = false;
        {
            VideoPlayer player;
            GLPreview *view = player.glPreview();
            if (view && player.isGLAccelerated()) {
                ran = true;
                const auto entry = [&](const QString &file, int track) {
                    PlaybackEntry e;
                    e.filePath = file;
                    e.clipIn = 0.0;
                    e.clipOut = kDuration;
                    e.timelineStart = 0.0;
                    e.timelineEnd = kDuration;
                    e.speed = 1.0;
                    e.sourceTrack = track;
                    e.sourceClipIndex = 0;
                    return e;
                };
                PlaybackEntry e1 = entry(seqB, 0);
                e1.stabilizerKeyframes = {key(0.0, 4.0)};
                const QVector<PlaybackEntry> seq = {e1, entry(seqC, 1)};
                QImage black(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
                black.fill(Qt::black);
                VideoPlayer::DecodedLayer v1;
                v1.rgb = dotFrame({QPoint(34, 10)})
                             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
                v1.videoDx = -0.375;
                v1.sourceTrack = 0;
                v1.sequenceIdx = 0;
                VideoPlayer::DecodedLayer v2;
                v2.rgb = dotFrame({QPoint(40, 40)})
                             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
                v2.sourceTrack = 1;
                v2.sequenceIdx = 1;
                const QRect dotArea(0, 0, kSize, 20);
                const QRect v2Area(40, 36, kSize - 40, kSize - 36);
                const auto keyedM2 = [&] {
                    return view->stabEnabledForTest()
                        && matrixNear(view->stabMatrixForTest(),
                                      {1, 0, -0.0625, 0, 1, 0, 0, 0, 1});
                };
                const auto glState = [&] {
                    std::ostringstream g;
                    g << "enabled=" << view->stabEnabledForTest()
                      << " m=" << matrixText(view->stabMatrixForTest());
                    return g.str();
                };
                // V2 paints first (backmost), V1 last, as the production sort orders them.
                const QImage shownA =
                    player.composeAndPresentForTest(black, {v2, v1}, seq, 0, us(t));
                const Box aDot = whiteBox(shownA, dotArea);
                const Box aV2 = whiteBox(shownA, v2Area);
                aOk = aDot.n == 4 && aDot.rect == QRect(14, 10, 2, 2) && aV2.n == 4
                    && aV2.rect == QRect(40, 40, 2, 2);
                bOk = !view->stabEnabledForTest();
                d << " (a) V1 dot " << boxText(aDot) << " V2 dot " << boxText(aV2)
                  << " ok=" << aOk << " (b) gl " << glState() << " ok=" << bOk;

                player.composeAndPresentForTest(black, {v1}, seq, 0, us(t));
                cOk = keyedM2();
                d << " (c) V1 alone gl " << glState() << " ok=" << cOk;

                player.composeAndPresentForTest(black, {v2, v1}, seq, 0, us(t));
                const bool dPremise = !view->stabEnabledForTest();
                EnhancedTextOverlay overlay;
                overlay.text = QStringLiteral("A");
                player.setTextOverlays({overlay});
                dOk = dPremise && !view->stabEnabledForTest();
                d << " (d) before re-present off=" << dPremise << " after setTextOverlays gl "
                  << glState() << " ok=" << dOk;
                player.setTextOverlays({});

                // (a') V1 as the canvas, a V2 layer transparent but for its dot.
                QImage canvasV1 = dotFrame({QPoint(10, 10)})
                                      .convertToFormat(QImage::Format_ARGB32_Premultiplied);
                VideoPlayer::DecodedLayer v2Clear = v2;
                v2Clear.rgb = QImage(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
                v2Clear.rgb.fill(Qt::transparent);
                for (int y = 40; y < 42; ++y)
                    for (int x = 40; x < 42; ++x)
                        v2Clear.rgb.setPixelColor(x, y, QColor(255, 255, 255, 255));
                const QImage shownA2 =
                    player.composeAndPresentForTest(canvasV1, {v2Clear}, seq, 0, us(t));
                const Box a2Dot = whiteBox(shownA2, dotArea);
                const Box a2V2 = whiteBox(shownA2, v2Area);
                a2Ok = a2Dot.n == 4 && a2Dot.rect == QRect(10, 10, 2, 2) && a2V2.n == 4
                    && a2V2.rect == QRect(40, 40, 2, 2);
                d << " (a') canvas V1 dot " << boxText(a2Dot) << " V2 dot " << boxText(a2V2)
                  << " ok=" << a2Ok;

                // (f) the nested-sequence SSOT present (displayNestedSequenceFrameAt) of
                // a keyed V1 and a disabled V2 clip, read back from the GL.
                view->setFixedSize(kSize, kSize);
                view->setFrameTextureNearestForTest(true);
                player.show();
                if (!selftest::glContextUsable(view)) {
                    d << " (f) not run: GL framebuffer cannot be grabbed (environment)";
                } else {
                    fRan = true;
                    ClipInfo v1Clip = withKeys(seqClip(seqA), {key(0.0, 4.0)});
                    ClipInfo v2Off = seqClip(seqC);
                    v2Off.visible = false;
                    const auto timeline = makeTimeline({{v1Clip}, {v2Off}});
                    PlaybackEntry ef = entry(seqA, 0);
                    ef.stabilizerKeyframes = v1Clip.stabilizerKeyframes;
                    player.setProjectOutputSize(QSize(kSize, kSize));
                    player.m_sequence = {ef};
                    player.m_activeEntry = 0;
                    player.m_timelinePositionUs = us(t);
                    const bool served =
                        player.displayNestedSequenceFrameAt(timeline.get(), us(t));
                    QApplication::processEvents(QEventLoop::AllEvents, 25);
                    const QImage raw = view->grabFramebuffer();
                    const qreal dpr = view->devicePixelRatioF();
                    QImage grabbed(kSize, kSize, QImage::Format_RGB32);
                    grabbed.fill(Qt::black);
                    for (int y = 0; y < kSize && !raw.isNull(); ++y)
                        for (int x = 0; x < kSize; ++x) {
                            const int rx = qMin(raw.width() - 1, int(std::floor((x + 0.5) * dpr)));
                            const int ry = qMin(raw.height() - 1, int(std::floor((y + 0.5) * dpr)));
                            grabbed.setPixel(x, y, raw.pixel(rx, ry) | 0xff000000u);
                        }
                    const Box fDot = whiteBox(grabbed);
                    fOk = served && !raw.isNull() && fDot.n == 4
                        && fDot.rect == QRect(14, 10, 2, 2) && !view->stabEnabledForTest();
                    d << " (f) served=" << served << " grab " << raw.width() << 'x'
                      << raw.height() << " dpr=" << dpr << " white " << boxText(fDot) << " gl "
                      << glState() << " ok=" << fOk;
                }
            }
        }
        const bool glLegsOk = aOk && a2Ok && bOk && cOk && dOk;
        if (!ran && eOk)
            tally.skip(10, ("GL preview unavailable (environment); " + d.str()).c_str());
        else if (ran && !fRan && glLegsOk && eOk)
            tally.skip(10, ("GL framebuffer unavailable (environment); " + d.str()).c_str());
        else
            tally.gate(10, glLegsOk && fOk && eOk, d.str());
    }

    // US-2203 G11 (pin): V1 alone stays on the GL: the matrix is G5 (b)'s -0.0625, the
    // stabilizer is on and the composed canvas is V1's image unchanged.
    {
        std::ostringstream d;
        VideoPlayer player;
        GLPreview *view = player.glPreview();
        if (!(view && player.isGLAccelerated())) {
            tally.skip(11, "GL preview unavailable (environment)");
        } else {
            PlaybackEntry e;
            e.filePath = seqA;
            e.clipIn = 0.0;
            e.clipOut = kDuration;
            e.timelineStart = 0.0;
            e.timelineEnd = kDuration;
            e.speed = 1.0;
            e.sourceTrack = 0;
            e.sourceClipIndex = 0;
            e.stabilizerKeyframes = {key(0.0, 4.0)};
            QImage black(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
            black.fill(Qt::black);
            VideoPlayer::DecodedLayer v1;
            v1.rgb = dotFrame({QPoint(10, 10)}).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            v1.sourceTrack = 0;
            v1.sequenceIdx = 0;
            const QImage shown = player.composeAndPresentForTest(black, {v1}, {e}, 0, us(t));
            const auto m = view->stabMatrixForTest();
            const bool same = shown == v1.rgb;
            const bool ok = view->stabEnabledForTest()
                && matrixNear(m, {1, 0, -0.0625, 0, 1, 0, 0, 0, 1}) && same;
            d << "pin: gl enabled=" << view->stabEnabledForTest() << " m=" << matrixText(m)
              << " composed format=" << int(shown.format()) << " sha=" << sha(shown)
              << " v1 sha=" << sha(v1.rgb) << " byteIdentical=" << same << " white "
              << boxText(whiteBox(shown));
            tally.gate(11, ok, d.str());
        }
    }

    // US-2204 G12: a V1 clip with a clip-local CPU FX (Echo, 2 samples 0.25 s apart) is
    // stabilized before the FX in the ordinary preview, each echo sample at its own
    // source time like renderFrameAt: keys t0 dx 0, t1 dx +8 at t = 1 s put the dot at
    // x 18 and its trail at x 16 and 14 (a box (14,10)-(19,11)); HEAD warped the
    // finished echo once (+8: the whole trail at x 18..). The prepared layer goes
    // through the production compose + present, which must not warp it again and
    // must leave the GL no keys. Negative control (pin): without keys the preview is
    // byte-identical to the export.
    {
        std::ostringstream d;
        VideoPlayer player;
        GLPreview *view = player.glPreview();
        if (!(view && player.isGLAccelerated())) {
            tally.skip(12, "GL preview unavailable (environment)");
        } else {
            const double tEcho = 1.0;
            ClipInfo base = seqClip(seqA);
            base.effects = {VideoEffect::createEcho(0.25, 2, 0.5, 2)};
            const QImage raw = dotFrame({QPoint(10, 10)});
            QImage black(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
            black.fill(Qt::black);
            const auto maxDiff = [](const QImage &a, const QImage &b) {
                if (a.isNull() || b.isNull() || a.size() != b.size())
                    return 255;
                const QImage x = a.convertToFormat(QImage::Format_RGBA8888);
                const QImage y = b.convertToFormat(QImage::Format_RGBA8888);
                int worst = 0;
                for (int row = 0; row < x.height(); ++row)
                    for (int col = 0; col < x.width() * 4; ++col)
                        worst = qMax(worst, std::abs(int(x.constScanLine(row)[col])
                                                     - int(y.constScanLine(row)[col])));
                return worst;
            };
            struct Shown {
                QImage prepared, composed, exported;
                bool applied = false, glKeysOff = false;
            };
            const auto run = [&](const ClipInfo &clip) {
                Shown out;
                auto timeline = makeTimeline({{clip}});
                view->setTimeline(timeline.get());
                PlaybackEntry e;
                e.filePath = seqA;
                e.clipIn = 0.0;
                e.clipOut = kDuration;
                e.timelineStart = 0.0;
                e.timelineEnd = kDuration;
                e.speed = 1.0;
                e.sourceTrack = 0;
                e.sourceClipIndex = 0;
                e.stabilizerKeyframes = clip.stabilizerKeyframes;
                player.m_sequence = {e};
                player.m_activeEntry = 0;
                player.m_timelinePositionUs = us(tEcho);
                out.prepared = player.preparePreviewClipFrame(
                    raw, e, 0, tEcho, QSize(kSize, kSize), us(tEcho), &out.applied);
                VideoPlayer::DecodedLayer v1;
                v1.rgb = out.prepared.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                v1.sourceTrack = 0;
                v1.sequenceIdx = 0;
                out.composed = player.composeAndPresentForTest(black, {v1}, {e}, 0, us(tEcho));
                out.glKeysOff = !view->stabEnabledForTest();
                view->setTimeline(nullptr);
                out.exported = tlrender::renderFrameAt(timeline.get(), us(tEcho),
                                                       QSize(kSize, kSize));
                return out;
            };
            const Shown keyed =
                run(withKeys(base, {key(0.0, 0.0), key(1.0, 8.0)}));
            const Shown plainRun = run(base);
            // Fractional key (+7.5 at t1) and a non-default grade: each echo sample is
            // warped raw, then graded (export order). Warping the graded sample
            // blends the dot's edge pixels with a different black, > 2 levels apart.
            ClipInfo gradedBase = base;
            gradedBase.colorCorrection.brightness = -30.0;
            const Shown gradedRun =
                run(withKeys(gradedBase, {key(0.0, 0.0), key(1.0, 7.5)}));
            const Shown gradedNoKeys = run(gradedBase);
            const int gradedDiff = maxDiff(gradedRun.composed, gradedRun.exported);
            const int gradeShows = maxDiff(gradedNoKeys.exported, plainRun.exported);
            const int warpShows = maxDiff(gradedRun.exported, gradedNoKeys.exported);
            const bool gradedOk = gradedRun.applied && gradedRun.glKeysOff && gradedDiff <= 2
                && gradeShows > 8 && warpShows > 8;
            const QRect full(0, 0, kSize, kSize);
            const Box expBox = nonBlackBox(keyed.exported, full);
            const Box prevBox = nonBlackBox(keyed.composed, full);
            const int keyedDiff = maxDiff(keyed.composed, keyed.exported);
            const int plainDiff = maxDiff(plainRun.composed, plainRun.exported);
            const bool keyedOk = gradedOk && keyed.applied && keyed.glKeysOff
                && expBox.rect == QRect(14, 10, 6, 2) && prevBox.rect == expBox.rect
                && keyedDiff <= 2;
            const bool plainOk2 = plainRun.applied && plainDiff == 0
                && nonBlackBox(plainRun.exported, full).rect == QRect(10, 10, 2, 2)
                && maxDiff(keyed.exported, plainRun.exported) > 8;  // the control can see a warp
            // Seek-conformed present of the same prepared (already stabilized) frame, with
            // an invalid (automatic) and a set project output size: the GL gets no keys.
            bool seekOk = true;
            std::ostringstream sd;
            {
                const ClipInfo keyedClip = withKeys(base, {key(0.0, 0.0), key(1.0, 8.0)});
                auto timeline = makeTimeline({{keyedClip}});
                view->setTimeline(timeline.get());
                PlaybackEntry e;
                e.filePath = seqA;
                e.clipIn = 0.0;
                e.clipOut = kDuration;
                e.timelineStart = 0.0;
                e.timelineEnd = kDuration;
                e.speed = 1.0;
                e.sourceTrack = 0;
                e.sourceClipIndex = 0;
                e.stabilizerKeyframes = keyedClip.stabilizerKeyframes;
                player.m_sequence = {e};
                player.m_activeEntry = 0;
                player.m_timelinePositionUs = us(tEcho);
                for (const QSize outSize : {QSize(), QSize(kSize, kSize)}) {
                    player.m_projectOutputSize = outSize;
                    player.displaySeekFrameConformed(keyed.prepared, us(tEcho));
                    const bool off = !view->stabEnabledForTest();
                    sd << " seek(" << (outSize.isValid() ? "set" : "auto") << ") glKeysOff=" << off;
                    seekOk = seekOk && off;
                }
                player.m_projectOutputSize = QSize();
                view->setTimeline(nullptr);
            }
            d << "graded fractional: maxDiff=" << gradedDiff << " gradeShows=" << gradeShows
              << " warpShows=" << warpShows << " ok=" << gradedOk
              << "; keyed: applied=" << keyed.applied << " glKeysOff=" << keyed.glKeysOff
              << " export " << boxText(expBox) << " preview " << boxText(prevBox)
              << " maxDiff=" << keyedDiff << " ok=" << keyedOk << "; key-less pin: applied="
              << plainRun.applied << " maxDiff=" << plainDiff << " export "
              << boxText(nonBlackBox(plainRun.exported, full)) << " ok=" << plainOk2
              << ";" << sd.str() << " ok=" << seekOk;
            tally.gate(12, keyedOk && plainOk2 && seekOk, d.str());
        }
    }

    // US-2300 G13: a nested child's key is sampled at the child's own source time.
    // The child clip starts at source 0.4 s (inPoint) and carries keys (source 0.4 s
    // dx 0, 1.4 s dx +10); P = V1 [filler 0.5 s, reference to C]. (a) At timeline
    // 0.8 s C is at child-local 0.3 s = source 0.7 s: dx +3 (the child-local time
    // would read 0, the timeline time +4); (b) the reference at speed 2: child 0.6 s =
    // source 1.0 s, +6; (c) the same keyed clip on C's V2 (C's V1 key-less); (d) two
    // levels (C = V1 [filler 0.2 s, reference to G], G holds the clip) at 1.0 s:
    // G 0.3 s = source 0.7 s, +3; (e) control: the key-less fixtures render
    // byte-identical to the base exe 0335b53 (S0-1 (a) shas). HEAD: (a)-(d) unmoved.
    {
        std::ostringstream d;
        const auto seqOf = [](const QString &id, const QVector<QVector<ClipInfo>> &tracks) {
            TimelineSequence s;
            s.id = id;
            s.name = id;
            s.videoTracks = tracks;
            return s;
        };
        const auto refTo = [](const TimelineSequence &c, double length, double speed) {
            ClipInfo ref;
            ref.sequenceRefId = c.id;
            ref.filePath = timeline_nesting::sequenceClipFilePath(c.id);
            ref.displayName = c.id;
            ref.duration = ref.outPoint = length;
            ref.speed = speed;
            ref.opacity = 1.0;
            return ref;
        };
        // P = V1 [filler `lead` s, reference to children[0]] rendered at `sec`.
        const auto renderNested = [&](QVector<TimelineSequence> children, double lead,
                                      double length, double speed, double sec) {
            const TimelineSequence p = seqOf(
                QStringLiteral("parent"),
                {{seqClip(seqC, lead), refTo(children.first(), length, speed)}});
            children.prepend(p);
            Timeline timeline;
            timeline.setProjectOutputConfig(kSize, kSize, false, kFps);
            timeline.setSequences(children, p.id);
            return tlrender::renderFrameAt(&timeline, us(sec), QSize(kSize, kSize));
        };
        ClipInfo childPlain = seqClip(seqA);
        childPlain.inPoint = 0.4;
        const ClipInfo childKeyed = withKeys(childPlain, {key(0.4, 0.0), key(1.4, 10.0)});
        ClipInfo c2Plain = seqClip(seqC);
        c2Plain.inPoint = 0.4;
        const ClipInfo c2Keyed = withKeys(c2Plain, {key(0.4, 0.0), key(1.4, 10.0)});
        ClipInfo v1Left = seqClip(seqB);
        v1Left.videoDx = -0.375;
        const QString childId = QStringLiteral("child");
        const QString grandId = QStringLiteral("grand");
        const auto twoLevel = [&](const ClipInfo &clip) {
            const TimelineSequence g = seqOf(grandId, {{clip}});
            const TimelineSequence c = seqOf(childId, {{seqClip(seqC, 0.2), refTo(g, 1.6, 1.0)}});
            return renderNested({c, g}, 0.5, 1.8, 1.0, 1.0);
        };
        const QRect topArea(0, 0, kSize, 30);
        const QRect v2Area(40, 36, kSize - 40, kSize - 36);
        struct Leg {
            const char *name;
            QImage keyed, plain;
            QRect area;
            QPoint expect;
        };
        const QVector<Leg> legs = {
            {"(a)", renderNested({seqOf(childId, {{childKeyed}})}, 0.5, 1.6, 1.0, 0.8),
             renderNested({seqOf(childId, {{childPlain}})}, 0.5, 1.6, 1.0, 0.8), topArea,
             QPoint(13, 10)},
            {"(b)", renderNested({seqOf(childId, {{childKeyed}})}, 0.5, 1.6, 2.0, 0.8),
             renderNested({seqOf(childId, {{childPlain}})}, 0.5, 1.6, 2.0, 0.8), topArea,
             QPoint(16, 10)},
            {"(c)", renderNested({seqOf(childId, {{v1Left}, {c2Keyed}})}, 0.5, 1.6, 1.0, 0.8),
             renderNested({seqOf(childId, {{v1Left}, {c2Plain}})}, 0.5, 1.6, 1.0, 0.8), v2Area,
             QPoint(43, 40)},
            {"(d)", twoLevel(childKeyed), twoLevel(childPlain), topArea, QPoint(13, 10)},
        };
        // S0-1 (a): sha() of each key-less fixture on the base exe 0335b53.
        const char *const baseKeylessSha[] = {"1b7195d757a492af", "1b7195d757a492af",
                                              "fd326ad3deb46e6b", "1b7195d757a492af"};
        bool movedOk = true;
        bool controlOk = true;
        for (int i = 0; i < legs.size(); ++i) {
            const Leg &leg = legs[i];
            const Box kBox = whiteBox(leg.keyed, leg.area);
            const Box pBox = whiteBox(leg.plain, leg.area);
            const QPoint plainAt = leg.area == v2Area ? QPoint(40, 40) : QPoint(10, 10);
            const bool moved = kBox.n == 4 && kBox.rect == QRect(leg.expect, QSize(2, 2));
            const bool still = pBox.n == 4 && pBox.rect == QRect(plainAt, QSize(2, 2));
            const bool base = sha(leg.plain) == baseKeylessSha[i];
            // (c): C's key-less V1 stays where it was.
            const bool v1Still = i != 2
                || whiteBox(leg.keyed, QRect(0, 0, 40, 30)).rect == QRect(10, 10, 2, 2);
            movedOk = movedOk && moved && v1Still;
            controlOk = controlOk && still && base;
            d << ' ' << leg.name << " keyed " << boxText(kBox) << " expect (" << leg.expect.x()
              << ',' << leg.expect.y() << ") v1Still=" << v1Still << " key-less "
              << boxText(pBox) << " sha=" << sha(leg.plain) << " base=" << baseKeylessSha[i]
              << " moved=" << moved << " (e) control=" << (still && base);
        }
        d << " movedOk=" << movedOk << " controlOk=" << controlOk;
        tally.gate(13, movedOk && controlOk, d.str());
    }

    // US-2300 G14: keys on an upper track, wherever renderFrameAt or the special
    // composite draws the frame. G5 (a)'s layout: V1 at videoDx -0.375 (source dot
    // (34,10) -> (10,10), covering x 0..39), V2's dot (40,40) beside it. (a) export
    // with V2 dx +4 (V1 key-less): V2 +4, V1 still; (b) V1 +8 and V2 +4: each its own;
    // (c) the special composite (V1 brushed) equals renderFrameAt (maxDiff 0) on (a)
    // and (b) and shows V2 +4; (d) the brush record follows the bound brush clip's
    // keyed layer (V1 +8, V2 +4 analysed at 128x128, V3 key-less): V2 bound -> +4 and
    // 128x128, V1 / V3 / none bound -> V1's +8; (e) an Echo on V2 with keys (0 s, 0)
    // / (1 s, +8) at 1 s: each sample at its own source time (G12's leg on V2);
    // (f) the preview the timeline renderer serves (S1-S3 of the design's F4) with a
    // shape clip active: (f1) the paused SSOT display and its refresh after a key
    // edit, (f2) a playback tick, each equal to renderFrameAt with no GL keys;
    // (f3) pin of the US-2300 guard: VideoPlayer's own warp leaves an active keyed
    // V2 composed with another layer unmoved. HEAD: (a)(b)(c) V2 unmoved, (d) V2
    // bound gives V1's +8, (e) V2's trail unmoved, (f1)(f2) V2 unmoved.
    {
        std::ostringstream d;
        ClipInfo v1Left = seqClip(seqB);
        v1Left.videoDx = -0.375;
        const ClipInfo v2Plain = seqClip(seqC);
        const QRect v1Area(0, 0, 40, 20);
        const QRect v2Area(40, 36, kSize - 40, kSize - 36);
        const auto dotAt = [](const Box &b, int x, int y) {
            return b.n == 4 && b.rect == QRect(x, y, 2, 2);
        };
        const auto maxDiff = [](const QImage &a, const QImage &b) {
            if (a.isNull() || b.isNull() || a.size() != b.size())
                return 255;
            const QImage x = a.convertToFormat(QImage::Format_RGBA8888);
            const QImage y = b.convertToFormat(QImage::Format_RGBA8888);
            int worst = 0;
            for (int row = 0; row < x.height(); ++row)
                for (int col = 0; col < x.width() * 4; ++col)
                    worst = qMax(worst, std::abs(int(x.constScanLine(row)[col])
                                                 - int(y.constScanLine(row)[col])));
            return worst;
        };
        const ClipInfo v1Keyed8 = withKeys(v1Left, {key(0.0, 8.0)});
        const ClipInfo v2Keyed4 = withKeys(v2Plain, {key(0.0, 4.0)});

        // (a) / (b) export.
        const QImage aPlain = exportAt({{v1Left}, {v2Plain}}, t);
        const QImage aKeyed = exportAt({{v1Left}, {v2Keyed4}}, t);
        const QImage bKeyed = exportAt({{v1Keyed8}, {v2Keyed4}}, t);
        const bool aOk = dotAt(whiteBox(aPlain, v2Area), 40, 40)
            && dotAt(whiteBox(aPlain, v1Area), 10, 10) && dotAt(whiteBox(aKeyed, v2Area), 44, 40)
            && dotAt(whiteBox(aKeyed, v1Area), 10, 10);
        const bool bOk = dotAt(whiteBox(bKeyed, v1Area), 18, 10)
            && dotAt(whiteBox(bKeyed, v2Area), 44, 40);
        d << "(a) export V2 " << boxText(whiteBox(aKeyed, v2Area)) << " V1 "
          << boxText(whiteBox(aKeyed, v1Area)) << " key-less V2 "
          << boxText(whiteBox(aPlain, v2Area)) << " ok=" << aOk << " (b) V1 "
          << boxText(whiteBox(bKeyed, v1Area)) << " V2 " << boxText(whiteBox(bKeyed, v2Area))
          << " ok=" << bOk;

        // (c) / (d) on MainWindow.
        bool cOk = false;
        bool dOk = false;
        {
            MainWindow window;
            window.hide();
            Timeline *mainTimeline = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
            if (mainTimeline) {
                emit mainTimeline->projectOutputConfigRestored(kSize, kSize, false);
                const auto load = [&](const QVector<QVector<ClipInfo>> &tracks) {
                    mainTimeline->setProjectOutputConfig(kSize, kSize, false, kFps);
                    mainTimeline->restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0.0,
                                                     -1.0, -1.0, 100);
                    mainTimeline->refreshPlaybackSequence();
                };
                const ClipInfo v1Brushed = withBrush(v1Left, QPointF(54.0, 30.0));
                const ClipInfo v1Brushed8 = withKeys(v1Brushed, {key(0.0, 8.0)});
                bool cAll = true;
                const auto cLeg = [&](const char *name, const ClipInfo &v1, const ClipInfo &v2,
                                      QPoint v1Dot, QPoint v2Dot) {
                    load({{v1}, {v2}});
                    const bool wants = tlrender::timelineBrushWantsSpecialPath(mainTimeline, t);
                    const QImage composite = window.buildSpecialClipComposite(t);
                    const QImage exported =
                        tlrender::renderFrameAt(mainTimeline, us(t), QSize(kSize, kSize));
                    const int diff = maxDiff(composite, exported);
                    const Box cV1 = whiteBox(composite, v1Area);
                    const Box cV2 = whiteBox(composite, v2Area);
                    const bool ok = wants && diff == 0 && dotAt(cV1, v1Dot.x(), v1Dot.y())
                        && dotAt(cV2, v2Dot.x(), v2Dot.y());
                    d << " (c" << name << ") wants=" << wants << " maxDiff=" << diff << " V1 "
                      << boxText(cV1) << " V2 " << boxText(cV2) << " ok=" << ok;
                    return ok;
                };
                // The key-less control leg pins the parity itself.
                cAll = cLeg("0", v1Brushed, v2Plain, QPoint(10, 10), QPoint(40, 40)) && cAll;
                cAll = cLeg("a", v1Brushed, v2Keyed4, QPoint(10, 10), QPoint(44, 40)) && cAll;
                cAll = cLeg("b", v1Brushed8, v2Keyed4, QPoint(18, 10), QPoint(44, 40)) && cAll;
                cOk = cAll;

                // (d) every layer brushed, so each can be bound.
                ClipInfo dV2 = withBrush(v2Keyed4, QPointF(50.0, 50.0));
                dV2.stabilizerSourceSize = QSize(128, 128);
                const ClipInfo dV3 = withBrush(seqClip(seqA), QPointF(20.0, 50.0));
                load({{v1Brushed8}, {dV2}, {dV3}});
                bool dAll = true;
                const auto dLeg = [&](const char *name, int bindTrack, double dx, QSize size,
                                      const QString &id) {
                    window.syncBrushAnimationPreviewForClip(bindTrack, bindTrack < 0 ? -1 : 0);
                    const QString bound = window.brushPreviewClipIdForTest();
                    stabwarp::StabSample stab;
                    QSize stabSize;
                    QString stabId;
                    const QImage composite =
                        window.buildSpecialClipComposite(t, nullptr, &stab, &stabSize, &stabId);
                    const bool boundOk = bindTrack < 0
                        ? bound.isEmpty() : bound == QStringLiteral("%1:0").arg(bindTrack);
                    const bool ok = boundOk && !composite.isNull() && std::abs(stab.dx - dx) < 1e-9
                        && stabSize == size && stabId == id;
                    d << " (d" << name << ") bound=" << bound.toStdString() << " record dx="
                      << stab.dx << " size=" << stabSize.width() << 'x' << stabSize.height()
                      << " id=" << stabId.toStdString() << " expect dx=" << dx << ' '
                      << size.width() << 'x' << size.height() << ' ' << id.toStdString()
                      << " ok=" << ok;
                    return ok;
                };
                dAll = dLeg("V2", 1, 4.0, QSize(128, 128), QStringLiteral("1:0")) && dAll;
                dAll = dLeg("V1", 0, 8.0, QSize(kSize, kSize), QStringLiteral("0:0")) && dAll;
                dAll = dLeg("V3", 2, 8.0, QSize(kSize, kSize), QStringLiteral("0:0")) && dAll;
                dAll = dLeg("none", -1, 8.0, QSize(kSize, kSize), QStringLiteral("0:0")) && dAll;
                dOk = dAll;
            } else {
                d << " (c)(d) mainTimeline missing";
            }
        }

        // (e) Echo on V2, keys (0 s, 0) / (1 s, +8), at 1 s: the dot at x 48 and its
        // samples 0.25 / 0.5 s back at x 46 / 44 (a box (44,40)-(49,41)); warping the
        // finished echo once would put the whole trail at x 48.
        bool eOk = false;
        {
            ClipInfo echoed = v2Plain;
            echoed.effects = {VideoEffect::createEcho(0.25, 2, 0.5, 2)};
            const QImage ePlain = exportAt({{v1Left}, {echoed}}, 1.0);
            const QImage eKeyed =
                exportAt({{v1Left}, {withKeys(echoed, {key(0.0, 0.0), key(1.0, 8.0)})}}, 1.0);
            const Box epBox = nonBlackBox(ePlain, v2Area);
            const Box ekBox = nonBlackBox(eKeyed, v2Area);
            // Three separate 2x2 samples (12 pixels), one in each of x 44 / 46 / 48.
            const int s44 = nonBlackBox(eKeyed, QRect(44, 40, 2, 2)).n;
            const int s46 = nonBlackBox(eKeyed, QRect(46, 40, 2, 2)).n;
            const int s48 = nonBlackBox(eKeyed, QRect(48, 40, 2, 2)).n;
            eOk = epBox.rect == QRect(40, 40, 2, 2) && epBox.n == 4
                && ekBox.rect == QRect(44, 40, 6, 2) && ekBox.n == 12 && s44 > 0 && s46 > 0
                && s48 > 0 && dotAt(whiteBox(eKeyed, v1Area), 10, 10);
            d << " (e) echo V2 keyed " << boxText(ekBox) << " samples x44/46/48 n=" << s44
              << '/' << s46 << '/' << s48 << " key-less " << boxText(epBox) << " ok=" << eOk;
        }

        // (f) V1 key-less (or +8), V2 a constant +4 (both keys), a shape clip on V3:
        // a static picture, so every frame is renderFrameAt(t)'s.
        Shape square;
        square.type = ShapeType::Rectangle;
        square.properties.size = QSizeF(8.0, 8.0);
        square.position = QPointF(56.0, 8.0);
        square.fill.color = QColor(200, 30, 30, 255);
        square.fill.opacity = 1.0;
        square.fill.enabled = true;
        square.fill.gradient = false;
        square.stroke.enabled = false;
        ClipInfo shapeClip;
        shapeClip.displayName = QStringLiteral("shape");
        shapeClip.duration = shapeClip.outPoint = kDuration;
        shapeClip.shapes = {square};
        const ClipInfo v2Const4 = withKeys(v2Plain, {key(0.0, 4.0), key(kDuration, 4.0)});
        bool fRan = false;
        bool f1Ok = false;
        bool f2Ok = false;
        {
            VideoPlayer player;
            GLPreview *view = player.glPreview();
            if (!(view && player.isGLAccelerated())) {
                d << " (f1)(f2) not run: GL preview unavailable (environment)";
            } else {
                fRan = true;
                player.setCanvasSize(kSize, kSize);
                player.setProjectOutputSize(QSize(kSize, kSize));
                int shownCount = 0;
                QImage shown;
                QObject::connect(&player, &VideoPlayer::frameComposited,
                                 [&](const QImage &image) {
                                     ++shownCount;
                                     shown = image;
                                 });
                bool all = true;
                // (f1) on a timeline with V1 `v1`.
                const auto pausedLeg = [&](const char *name, const ClipInfo &v1, QPoint v1Dot,
                                           bool refreshLeg) {
                    auto timeline = makeTimeline({{v1}, {v2Const4}, {shapeClip}});
                    view->setTimeline(timeline.get());
                    player.m_sequence = timeline->computePlaybackSequence();
                    player.m_activeEntry = -1;
                    for (int i = 0; i < player.m_sequence.size(); ++i)
                        if (player.m_sequence[i].sourceTrack == 0)
                            player.m_activeEntry = i;
                    player.m_timelinePositionUs = us(t);
                    const int before = shownCount;
                    const bool served = player.displayNestedSequenceFrameAt(timeline.get(), us(t));
                    const QImage expected =
                        tlrender::renderFrameAt(timeline.get(), us(t), QSize(kSize, kSize));
                    const int diff = maxDiff(shown, expected);
                    const bool glOff = !view->stabEnabledForTest();
                    bool ok = served && shownCount == before + 1 && diff <= 1
                        && dotAt(whiteBox(shown, v2Area), 44, 40)
                        && dotAt(whiteBox(shown, v1Area), v1Dot.x(), v1Dot.y()) && glOff
                        && player.m_activeEntry >= 0;
                    d << " (f1" << name << ") served=" << served << " shown+="
                      << (shownCount - before) << " maxDiff=" << diff << " V1 "
                      << boxText(whiteBox(shown, v1Area)) << " V2 "
                      << boxText(whiteBox(shown, v2Area)) << " glKeys="
                      << view->stabEnabledForTest() << " ok=" << ok;
                    if (refreshLeg) {
                        // V2's keys +4 -> +6 on the timeline (no setSequence), then refresh.
                        QVector<ClipInfo> clips = timeline->videoTracks().at(1)->clips();
                        clips[0].stabilizerKeyframes = {key(0.0, 6.0), key(kDuration, 6.0)};
                        timeline->videoTracks().at(1)->setClips(clips);
                        const int beforeRefresh = shownCount;
                        player.refreshDisplayedFrame();
                        const QImage expected6 =
                            tlrender::renderFrameAt(timeline.get(), us(t), QSize(kSize, kSize));
                        const int diff6 = maxDiff(shown, expected6);
                        const bool refreshOk = shownCount == beforeRefresh + 1 && diff6 <= 1
                            && dotAt(whiteBox(expected6, v2Area), 46, 40)
                            && dotAt(whiteBox(shown, v2Area), 46, 40)
                            && !view->stabEnabledForTest();
                        d << " refresh: shown+=" << (shownCount - beforeRefresh)
                          << " maxDiff=" << diff6 << " V2 " << boxText(whiteBox(shown, v2Area))
                          << " glKeys=" << view->stabEnabledForTest() << " ok=" << refreshOk;
                        ok = ok && refreshOk;
                    }
                    view->setTimeline(nullptr);
                    return ok;
                };
                all = pausedLeg("", v1Left, QPoint(10, 10), true) && all;
                all = pausedLeg(" V1+8", v1Keyed8, QPoint(18, 10), false) && all;
                f1Ok = all;
            }
        }
        if (fRan) {
            // (f2) a playback tick (shape-clip G5's form).
            auto timeline = makeTimeline({{v1Left}, {v2Const4}, {shapeClip}});
            const QImage expected =
                tlrender::renderFrameAt(timeline.get(), us(t), QSize(kSize, kSize));
            VideoPlayer player;
            player.setCanvasSize(kSize, kSize);
            player.setProjectOutputSize(QSize(kSize, kSize));
            player.glPreview()->setTimeline(timeline.get());
            player.setSequence(timeline->computePlaybackSequence());
            int ticks = 0;
            QImage tick;
            QObject::connect(&player, &VideoPlayer::frameComposited, [&](const QImage &image) {
                ++ticks;
                tick = image;
            });
            const qint64 before = player.timelinePositionUs();
            player.play();
            QElapsedTimer guard;
            guard.start();
            // Until a tick after the playhead moved (play() itself may show one frame).
            while (guard.elapsed() < 2000
                   && (ticks < 2 || player.timelinePositionUs() <= before))
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            player.pause();
            const qint64 after = player.timelinePositionUs();
            const int diff = maxDiff(tick, expected);
            // GL keys on during playback would warp the tick a second time on screen.
            const bool stabOn = player.glPreview()->stabEnabledForTest();
            f2Ok = ticks >= 2 && after > before && diff <= 1
                && dotAt(whiteBox(tick, v2Area), 44, 40) && dotAt(whiteBox(tick, v1Area), 10, 10)
                && !stabOn;
            d << " (f2) ticks=" << ticks << " position " << before << "->" << after
              << " maxDiff=" << diff << " V2 " << boxText(whiteBox(tick, v2Area)) << " V1 "
              << boxText(whiteBox(tick, v1Area)) << " enabled=" << stabOn << " ok=" << f2Ok;
            player.glPreview()->setTimeline(nullptr);
        }

        // (f3) the keyed V2 as the active entry (V1 empty there; V2 and V3 start
        // together, so the lower track is active, F15) with a V3 layer present:
        // VideoPlayer's own warp (warpV1ForCpuComposite) keeps it unmoved while
        // the US-2300 guard stands; US-2301 rewrites this leg to +4 (US-2301 がこの
        // 脚を +4 に書き換える). Control: the same entry on V1 is warped +4.
        bool f3Ok = false;
        {
            VideoPlayer player;
            const auto entry = [&](const QString &file, int track) {
                PlaybackEntry e;
                e.filePath = file;
                e.clipIn = 0.0;
                e.clipOut = kDuration;
                e.timelineStart = 0.0;
                e.timelineEnd = kDuration;
                e.speed = 1.0;
                e.sourceTrack = track;
                e.sourceClipIndex = 0;
                return e;
            };
            QImage black(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
            black.fill(Qt::black);
            const auto composeV2 = [&](int track) {
                PlaybackEntry e2 = entry(seqC, track);
                e2.stabilizerKeyframes = {key(0.0, 4.0)};
                const PlaybackEntry e3 = entry(seqA, track + 1);
                VideoPlayer::DecodedLayer l2;
                l2.rgb = dotFrame({QPoint(40, 40)})
                             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
                l2.sourceTrack = track;
                l2.sequenceIdx = 0;
                VideoPlayer::DecodedLayer l3;
                l3.rgb = dotFrame({QPoint(10, 10)})
                             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
                l3.sourceTrack = track + 1;
                l3.sequenceIdx = 1;
                // V3 paints first (backmost), the active V2 last.
                return player.composeAndPresentForTest(black, {l3, l2}, {e2, e3}, 0, us(t));
            };
            const Box upper = whiteBox(composeV2(1), v2Area);
            const Box lower = whiteBox(composeV2(0), v2Area);
            f3Ok = dotAt(upper, 40, 40) && dotAt(lower, 44, 40);
            d << " (f3) pin: active keyed V2 + V3 layer V2 " << boxText(upper)
              << " (US-2301 rewrites this leg to +4); control on V1 " << boxText(lower)
              << " ok=" << f3Ok;
        }
        const bool cpuOk = aOk && bOk && cOk && dOk && eOk && f3Ok;
        if (!fRan && cpuOk)
            tally.skip(14, ("GL preview unavailable (environment); " + d.str()).c_str());
        else
            tally.gate(14, cpuOk && f1Ok && f2Ok, d.str());

        // US-2300 diagnostic (not part of any gate): F4's S4-S6 on (f)'s fixture
        // (V1 key-less, V2 constant +4, shape on V3), printed for the result log.
        {
            auto tl = makeTimeline({{v1Left}, {v2Const4}, {shapeClip}});
            const QImage ref = tlrender::renderFrameAt(tl.get(), us(t), QSize(kSize, kSize));
            std::cerr << "US-2300 probe renderFrameAt(t) V2 " << boxText(whiteBox(ref, v2Area))
                      << " V1 " << boxText(whiteBox(ref, v1Area)) << '\n';
            PlaybackEntry e1;
            e1.filePath = seqB;
            e1.clipIn = 0.0;
            e1.clipOut = kDuration;
            e1.timelineStart = 0.0;
            e1.timelineEnd = kDuration;
            e1.speed = 1.0;
            e1.sourceTrack = 0;
            e1.sourceClipIndex = 0;
            PlaybackEntry e2 = e1;
            e2.filePath = seqC;
            e2.sourceTrack = 1;
            e2.stabilizerKeyframes = v2Const4.stabilizerKeyframes;
            QImage black(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
            black.fill(Qt::black);
            VideoPlayer::DecodedLayer l1;
            l1.rgb = dotFrame({QPoint(34, 10)}).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            l1.videoDx = -0.375;
            l1.sourceTrack = 0;
            l1.sequenceIdx = 0;
            VideoPlayer::DecodedLayer l2;
            l2.rgb = dotFrame({QPoint(40, 40)}).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            l2.sourceTrack = 1;
            l2.sequenceIdx = 1;
            const auto fields = [&](const QImage &image) {
                return "V2 " + boxText(whiteBox(image, v2Area)) + " V1 "
                    + boxText(whiteBox(image, v1Area));
            };
            {   // S4: the still-compare display as the GL view receives it (bypass path).
                VideoPlayer player;
                GLPreview *view = player.glPreview();
                if (view && player.isGLAccelerated()) {
                    player.setCanvasSize(kSize, kSize);
                    view->setTimeline(tl.get());
                    // Vertical wipe at 0.25: rows 48-63 show the still, rows 0-47 (both
                    // dots and the shape) the live frame.
                    QImage still(kSize, kSize, QImage::Format_RGB32);
                    still.fill(QColor(60, 60, 60));
                    stillcompare::Config on;
                    on.enabled = true;
                    on.mode = stillcompare::Mode::WipeVertical;
                    on.position = 0.25;
                    on.still = still;
                    player.setStillCompare(on);
                    player.composeAndPresentForTest(black, {l2, l1}, {e1, e2}, 0, us(t));
                    const bool drawn = selftest::glContextUsable(view);
                    if (drawn)
                        view->grabFramebuffer();
                    const QImage upload = view->uploadFrameForTest();
                    const QImage expect = stillcompare::apply(ref, still, on.mode, on.position);
                    const QRect live(0, 0, kSize, 48);
                    std::cerr << "US-2300 probe S4 still-compare GL upload drawn=" << drawn
                              << " viewTimeline=" << (view->timeline() != nullptr)
                              << " upload vs renderFrameAt (live rows 0-47) maxDiff="
                              << maxDiff(upload.copy(live), ref.copy(live))
                              << " upload vs apply(renderFrameAt, still) maxDiff="
                              << maxDiff(upload, expect) << ' ' << fields(upload)
                              << " stabEnabled=" << view->stabEnabledForTest() << '\n';
                    player.setStillCompare(stillcompare::Config{});
                    view->setTimeline(nullptr);
                } else {
                    std::cerr << "US-2300 probe S4 not run (GL preview unavailable)\n";
                }
            }
            {   // S5: the project-camera (trueProjection) frame uploaded to the GL view.
                Camera3DState state;
                state.trueProjection = true;
                const Camera3D camera{state};
                tl->setProjectCamera(camera);
                VideoPlayer player;
                GLPreview *view = player.glPreview();
                if (view && player.isGLAccelerated() && selftest::glContextUsable(view)) {
                    player.setProjectOutputSize(QSize(kSize, kSize));
                    view->setTimeline(tl.get());
                    view->setProjectCamera(camera);
                    view->grabFramebuffer();
                    const QImage upload = view->uploadFrameForTest();
                    const qint64 at = player.timelinePositionUs();
                    const QImage ref5 = tlrender::renderFrameAt(tl.get(), at, QSize(kSize, kSize));
                    std::cerr << "US-2300 probe S5 project-camera upload t=" << at << " size="
                              << upload.width() << 'x' << upload.height()
                              << " maxDiff=" << maxDiff(upload, ref5) << ' ' << fields(upload)
                              << " stabEnabled=" << view->stabEnabledForTest() << '\n';
                    view->setTimeline(nullptr);
                } else {
                    std::cerr << "US-2300 probe S5 not run (GL preview unavailable)\n";
                }
                tl->setProjectCamera(Camera3D{Camera3DState{}});
            }
            {   // S6: the onion-skin ghost at t-1f, and the upload carrying it.
                VideoPlayer player;
                GLPreview *view = player.glPreview();
                if (view && player.isGLAccelerated()) {
                    player.setCanvasSize(kSize, kSize);
                    view->setTimeline(tl.get());
                    onionskin::Config cfg;
                    cfg.enabled = true;
                    cfg.framesBefore = 1;
                    cfg.framesAfter = 0;
                    player.setOnionSkinConfig(cfg);
                    player.composeAndPresentForTest(black, {l2, l1}, {e1, e2}, 0, us(t));
                    const QImage upload = view->uploadFrameForTest();
                    const qint64 frameUs =
                        player.m_frameDurationUs > 0 ? player.m_frameDurationUs : AV_TIME_BASE / 30;
                    const QImage ghost =
                        tlrender::renderFrameAt(tl.get(), us(t) - frameUs, QSize(kSize, kSize));
                    const QImage expect =
                        onionskin::compose(player.m_currentFrameImage, {ghost}, {}, cfg);
                    std::cerr << "US-2300 probe S6 onion ghost(t-1f) " << fields(ghost)
                              << " ghost vs renderFrameAt(t) maxDiff=" << maxDiff(ghost, ref)
                              << " upload vs compose(current, ghost) maxDiff="
                              << maxDiff(upload, expect) << " upload (blended) nonBlack V2 "
                              << boxText(nonBlackBox(upload, v2Area)) << " V1 "
                              << boxText(nonBlackBox(upload, v1Area)) << " stabEnabled=" << view->stabEnabledForTest() << '\n';
                    player.setOnionSkinConfig(onionskin::Config{});
                    view->setTimeline(nullptr);
                } else {
                    std::cerr << "US-2300 probe S6 not run (GL preview unavailable)\n";
                }
            }
        }
    }

    return tally.finish("stabilizer-export");
}
