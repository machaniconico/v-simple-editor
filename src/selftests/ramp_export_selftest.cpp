// US-2401 ramp-export: a top-level leaf clip with a non-identity speed ramp (no
// time remap, not reversed, not a sequence reference) maps its export source time
// through the shared ClipInfo mapper over its playback interval (the start and in
// point a transition borrowed, as VideoPlayer::entryLocalPositionUs maps the
// entry), clamped to that interval, in renderFrameAt (inside a transition's
// overlap, after it, the neighbour layer, upper tracks), its Echo samples,
// renderClipSourceAt / parentLayerAt and MainWindow::buildSpecialClipComposite.
// The fixture movies draw a 4x4 white box per frame (frame n at column n % 48,
// row n / 48), movie A in the rows from y 0, movie B in the rows from y 108, so
// a frame's box names its source frame and the two clips of a dissolve stay
// readable. 30 fps; every in point is offset by 1/240 s (1/8 frame) so no sample
// lands on a frame boundary. Expected frames are closed-form constants (the
// first frame at or after the source second): for a constant ramp s at speed v,
// source = inPoint + s·v·t_own + δ·v·(s − 1) with δ the seconds a transition
// borrowed before the in point; pins are constants taken on the base exe.
// G1 lone V1 {0, 1.5} at t 0 / 0.5 / 1.0 / 1.5 s + 1/240: the mapper frames
//    (HEAD: ramp-free), also through the 4-argument overload; the identity ramp
//    and no ramp pinned.
// G2 the ends: {0, 1.5} holds the interval's clipOut frame once the source is
//    used up (the file's last frame when the clipOut is the file end); {0, 0.5}
//    reaches half the source at the last timeline frame; two keyframes (0 s, 1.0)
//    / (1 s, 2.0) follow timelineToSourceUs. A B-frame H.264 source must hold
//    its actual last frame too (strict PTS filtering of delayed flush output).
// G3 consumers: (a) the borrowed origin (A | B, 1 s CrossDissolve) inside the
//    window, outside it and on the neighbour layer, speed 2, End / Start, A's
//    trail clamp (pixels at 1.7 s, source time at 2.2 s), V2; (b) a readable
//    ramped V2 (pixels, layer time, parentLayerAt); (c) Echo
//    samples, (c2) on a borrowed trail; (d) RollingShutter recorded; (e)
//    renderClipSourceAt; (f) the special composite: (f1) Echo (non-brush)
//    against its own decoder at the mapper time, (f2) brush byte-equal to renderFrameAt, (f2s) a brush clip
//    with s < 1 below its in point, (f3) the borrowed origin with Echo.
// G4 unchanged (pins): reversed + ramp, time remap + ramp (remap wins), speed 2
//    with an identity ramp, render in place with handles; handles 0 gates the
//    declared change 47/62/77 -> 55/77/100,
//    (p1) a ramped sequence reference, (p2) a ramped clip inside a nested
//    sequence, (p3) the same inside an overlap.
// G5 (GL) the paused preview read back against renderFrameAt: (a) G1's times,
//    (b) after the source is used up (export gated, preview recorded [B2]),
//    (c1)-(c4) the borrowed origin.
#include "../GLPreview.h"
#include "../BrushAnimation.h"
#include "../Keyframe.h"
#include "../MainWindow.h"
#include "../RenderInPlace.h"
#include "../RotoClipEntry.h"
#include "../SpeedRampData.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TimeRemap.h"
#include "../VideoEffect.h"
#include "../VideoPlayer.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"
#include "SelftestGl.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kW = 384;
constexpr int kH = 216;
constexpr int kFps = 30;
constexpr int kFrames = 300;
constexpr int kCols = 48;
constexpr int kRowsA = 0;
constexpr int kRowsB = 108;
constexpr double kOff = 1.0 / 240.0;

QPoint boxAt(int n, int rowBase) { return QPoint(8 * (n % kCols) + 2, rowBase + 8 * (n / kCols) + 2); }

bool writeBoxMovie(const QString &path, int rowBase)
{
    libavcore::EncodeRequest request;
    request.width = kW;
    request.height = kH;
    request.fps = request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 6000000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request))
        return false;
    for (int n = 0; n < kFrames; ++n) {
        QImage frame(kW, kH, QImage::Format_RGB888);
        frame.fill(Qt::black);
        const QPoint b = boxAt(n, rowBase);
        for (int y = b.y(); y < b.y() + 4; ++y)
            for (int x = b.x(); x < b.x() + 4; ++x)
                frame.setPixelColor(x, y, Qt::white);
        if (!encoder.pushFrameRgb24(frame.constBits(), frame.bytesPerLine(), n))
            return false;
    }
    return !encoder.finalize();
}

// Fixture generation only (no external decoding). The bundled DLL disables
// libx264; the installed ffmpeg encoder supplies a stream with reorder delay.
// Keep the generated boxes identical to writeBoxMovie and require real B-frames
// in G2, so an encoder fallback cannot silently make the flush test vacuous.
bool writeDelayedBoxMovie(const QString &path)
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) return false;
    QProcess encoder;
    encoder.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
        QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-f"),
        QStringLiteral("rawvideo"), QStringLiteral("-pixel_format"), QStringLiteral("rgb24"),
        QStringLiteral("-video_size"), QStringLiteral("384x216"), QStringLiteral("-framerate"),
        QStringLiteral("30"), QStringLiteral("-i"), QStringLiteral("pipe:0"),
        QStringLiteral("-an"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
        QStringLiteral("-bf"), QStringLiteral("3"), QStringLiteral("-g"), QStringLiteral("60"),
        QStringLiteral("-crf"), QStringLiteral("18"), QStringLiteral("-pix_fmt"),
        QStringLiteral("yuv420p"), path});
    if (!encoder.waitForStarted(10000)) return false;
    for (int n = 0; n < kFrames; ++n) {
        QImage frame(kW, kH, QImage::Format_RGB888);
        frame.fill(Qt::black);
        const QPoint b = boxAt(n, kRowsA);
        for (int y = b.y(); y < b.y() + 4; ++y)
            for (int x = b.x(); x < b.x() + 4; ++x)
                frame.setPixelColor(x, y, Qt::white);
        if (encoder.write(reinterpret_cast<const char *>(frame.constBits()), frame.sizeInBytes())
            != frame.sizeInBytes()) return false;
        while (encoder.bytesToWrite() > 0)
            if (!encoder.waitForBytesWritten(30000)) return false;
    }
    encoder.closeWriteChannel();
    const bool done = encoder.waitForFinished(60000);
    if (!done) {
        encoder.kill();
        encoder.waitForFinished(5000);
    }
    std::cerr << "delayed fixture encoder=" << ffmpeg.toStdString()
              << " libx264 bf=3 " << encoder.readAllStandardError().toStdString() << '\n';
    return done && encoder.exitStatus() == QProcess::NormalExit && encoder.exitCode() == 0;
}

// The boxes of one movie's rows, brightest first (frame number, mean grey).
std::vector<std::pair<int, int>> boxesOf(const QImage &src, int rowBase)
{
    std::vector<std::pair<int, int>> found;
    if (src.isNull())
        return found;
    QImage img = src.convertToFormat(QImage::Format_RGB32);
    if (img.size() != QSize(kW, kH))
        img = img.scaled(kW, kH, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    for (int n = 0; n < kCols * 7; ++n) {
        const QPoint b = boxAt(n, rowBase);
        int sum = 0;
        for (int y = b.y(); y < b.y() + 4; ++y)
            for (int x = b.x(); x < b.x() + 4; ++x)
                sum += qGray(img.pixel(x, y));
        if (sum / 16 > 40)
            found.push_back({n, sum / 16});
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto &a, const auto &b) { return a.second > b.second; });
    return found;
}

// The brightest box (-1 when none).
int boxOf(const QImage &img, int rowBase)
{
    const auto b = boxesOf(img, rowBase);
    return b.empty() ? -1 : b.front().first;
}

std::string boxText(const QImage &img, int rowBase)
{
    if (img.isNull())
        return "null";
    const auto b = boxesOf(img, rowBase);
    if (b.empty())
        return "none";
    std::ostringstream o;
    for (size_t i = 0; i < b.size() && i < 4; ++i)
        o << (i ? "," : "") << b[i].first << '(' << b[i].second << ')';
    return o.str();
}

// The frame the decoders pick for a source second (the first frame at or after it).
int frameAt(double sourceSec) { return int(std::ceil(sourceSec * kFps - 1e-6)); }

std::string sha16(const QImage &img)
{
    if (img.isNull())
        return "null";
    const QImage c = img.convertToFormat(QImage::Format_RGBA8888);
    QCryptographicHash h(QCryptographicHash::Sha256);
    for (int y = 0; y < c.height(); ++y)
        h.addData(QByteArrayView(reinterpret_cast<const char *>(c.constScanLine(y)), c.width() * 4));
    return h.result().toHex().left(16).toStdString();
}

ClipInfo movClip(const QString &path, double in, double length)
{
    ClipInfo c;
    c.filePath = path;
    c.displayName = QStringLiteral("ramp");
    c.duration = double(kFrames) / kFps;
    c.inPoint = in;
    c.outPoint = in + length;
    c.opacity = 1.0;
    return c;
}

speedramp::SpeedRamp ramp(double s)
{
    speedramp::SpeedRamp r;
    r.keyframes = {{0, s}};
    return r;
}

RotoClipEntry fullRoto(const QString &clipId)
{
    RotoClipEntry roto;
    roto.clipId = clipId;
    for (const QPointF &p : {QPointF(0, 0), QPointF(kW, 0), QPointF(kW, kH), QPointF(0, kH)}) {
        RotoPoint point;
        point.position = p;
        point.handleIn = p;
        point.handleOut = p;
        roto.path.points.append(point);
    }
    return roto;
}

// A | B on one track joined by a 1 s CrossDissolve of `alignment`.
void dissolve(ClipInfo &a, ClipInfo &b, TransitionAlignment alignment)
{
    a.trailOut.type = TransitionType::CrossDissolve;
    a.trailOut.duration = 1.0;
    a.trailOut.alignment = alignment;
    b.leadIn.type = TransitionType::CrossDissolve;
    b.leadIn.duration = 1.0;
}

qint64 usAt(double sec) { return qint64(std::llround(sec * 1e6)); }

// Pins taken on the base exe (HEAD 7a1efac, production unchanged; sha16 = the first 16 hex
// digits of the RGBA8888 SHA-256).
const std::map<std::string, std::string> kPins = {
    {"g1.identity0", "9a43676d1c261dfa"},
    {"g1.identity1", "a657f95555edc506"},
    {"g1.identity2", "093cdbc917c194a2"},
    {"g1.identity3", "90094fccf1e2908c"},
    {"g1.none0", "9a43676d1c261dfa"},
    {"g1.none1", "a657f95555edc506"},
    {"g1.none2", "093cdbc917c194a2"},
    {"g1.none3", "90094fccf1e2908c"},
    {"g3.a1.0", "76125fbd8ff5e7f8"},
    {"g3.a1.1", "1f882ff684e0a7ed"},
    {"g3.a1.2", "e8de3a5f26dbf16c"},
    {"g3.f1.identity0", "3f8eeeac39d89634"},
    {"g3.f1.identity1", "1922c953d7608657"},
    {"g3.f1.identity2", "b2197e11a1ac8f09"},
    {"g3.f1.identity3", "fd702307ab84d648"},
    {"g3.f2.identity0", "2740276d432c10dc"},
    {"g3.f2.identity1", "46db39d2045f7e84"},
    {"g3.f2.identity2", "faacecc5d45b77e0"},
    {"g3.f2.identity3", "2d13ba38b1dfe782"},
    {"g4.p1.0", "a657f95555edc506"},
    {"g4.p1.1", "093cdbc917c194a2"},
    {"g4.p1.2", "90094fccf1e2908c"},
    {"g4.p2.0", "a657f95555edc506"},
    {"g4.p2.1", "093cdbc917c194a2"},
    {"g4.p2.2", "90094fccf1e2908c"},
    {"g4.p3.0", "76125fbd8ff5e7f8"},
    {"g4.p3.1", "1f882ff684e0a7ed"},
    {"g4.p3.2", "e8de3a5f26dbf16c"},
    {"g4.remap0", "e46e5cecf94f59aa"},
    {"g4.remap1", "a657f95555edc506"},
    {"g4.remap2", "ff5844a534676827"},
    {"g4.reversed0", "f31de44ebc84c755"},
    {"g4.reversed1", "6c8a3ddbc9287a1c"},
    {"g4.reversed2", "caf0a208b5cd756b"},
    {"g4.rip0", "2ecee515b188d957"},
    {"g4.rip1", "66c2af59b757b310"},
    {"g4.rip2", "103766ec562fc5b4"},
    {"g4.speed20", "093cdbc917c194a2"},
    {"g4.speed21", "eaedaa7cde356991"},
    {"g4.speed22", "bba3945070e8d6a3"},
};

bool pinned(const std::string &name, const std::string &value, std::ostringstream &d)
{
    const auto it = kPins.find(name);
    const bool ok = it != kPins.end() && it->second == value;
    d << ' ' << name << '=' << value << (ok ? "" : " [pin mismatch]");
    return ok;
}

void pumpEvents(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
}

} // namespace

int runRampExportSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir dir;
    const QString movA = dir.filePath(QStringLiteral("ramp-a.mp4"));
    const QString movB = dir.filePath(QStringLiteral("ramp-b.mp4"));
    const bool mediaOk = dir.isValid() && writeBoxMovie(movA, kRowsA) && writeBoxMovie(movB, kRowsB);
    const QSize size(kW, kH);
    const double times[] = {kOff, 0.5 + kOff, 1.0 + kOff, 1.5 + kOff};
    // G1's mapper frames (inPoint 1 + 1/240, 1.5 x t_own) and HEAD's ramp-free ones.
    const int g1Mapper[] = {31, 53, 76, 98};

    MainWindow window;
    Timeline *timeline = window.m_timeline;
    window.m_projectConfig.width = kW;
    window.m_projectConfig.height = kH;
    window.m_projectConfig.fps = kFps;
    timeline->setProjectOutputConfig(kW, kH, false, kFps);
    const auto load = [&](const QVector<QVector<ClipInfo>> &tracks) {
        timeline->setRotoEntries({});
        timeline->restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    };
    const auto render = [&](double sec) { return tlrender::renderFrameAt(timeline, usAt(sec), size); };
    // The interval of clip `clipIdx` on track `track` (null when none).
    QVector<QVector<OverlapInterval>> intervals;
    const auto intervalOf = [&](int track, int clipIdx) -> const OverlapInterval * {
        intervals = timeline->videoOverlapIntervals();
        if (track < intervals.size())
            for (const OverlapInterval &iv : intervals[track])
                if (iv.clipIdx == clipIdx)
                    return &iv;
        return nullptr;
    };

    if (!mediaOk) {
        for (int g = 1; g <= 5; ++g)
            tally.gate(g, false, "fixture media failed");
        return tally.finish("ramp-export");
    }

    ClipInfo lone = movClip(movA, 1.0 + kOff, 4.0);
    lone.speedRamp = ramp(1.5);

    // ---- G1 ----
    {
        std::ostringstream d;
        bool ok = true;
        load({{lone}});
        for (int i = 0; i < 4; ++i) {
            const double t = times[i];
            const int single = boxOf(render(t), kRowsA);
            const int blurred = boxOf(
                tlrender::renderFrameAt(timeline, usAt(t), size, 1e6 / kFps), kRowsA);
            const int mapper = frameAt(lone.sourceSecondAtLocalTime(t));
            ok = ok && single == g1Mapper[i] && blurred == g1Mapper[i] && mapper == g1Mapper[i];
            d << " t=" << t << " renderFrameAt=" << single << " 4-arg=" << blurred
              << " mapper=" << mapper << " expect=" << g1Mapper[i];
        }
        ClipInfo identity = lone;
        identity.speedRamp = speedramp::SpeedRamp::identity();
        ClipInfo noRamp = lone;
        noRamp.speedRamp = speedramp::SpeedRamp{};
        for (const auto &[name, clip] : {std::make_pair("identity", identity), std::make_pair("none", noRamp)}) {
            load({{clip}});
            for (int i = 0; i < 4; ++i)
                ok = pinned(std::string("g1.") + name + std::to_string(i), sha16(render(times[i])), d) && ok;
        }
        tally.gate(1, ok, d.str());
    }

    // ---- G2 ----
    {
        std::ostringstream d;
        bool ok = true;
        // {0, 1.5}: the source (4 s) is used up at 2.667 s; then clipOut 5.00417 s, frame 151.
        load({{lone}});
        const std::pair<double, int> fast[] = {{2.5 + kOff, 143}, {3.0 + kOff, 151},
                                               {3.5 + kOff, 151}, {119.0 / kFps, 151}};
        for (const auto &[t, expect] : fast) {
            const int got = boxOf(render(t), kRowsA);
            ok = ok && got == expect;
            d << " s1.5 t=" << t << " got=" << got << " expect=" << expect;
        }
        // {0, 1.5} on the whole file (in 0, out 10 s = the file end): from 6.667 s it holds the
        // file's last frame (299; no frame starts at or after the clipOut, HEAD: ramp-free).
        {
            ClipInfo whole = movClip(movA, 0.0, double(kFrames) / kFps);
            whole.speedRamp = ramp(1.5);
            load({{whole}});
            for (const double t : {8.0 + kOff, 9.95}) {
                const int got = boxOf(render(t), kRowsA);
                ok = ok && got == kFrames - 1;
                d << " s1.5 whole t=" << t << " got=" << got << " expect=" << kFrames - 1;
            }
        }
        // A delayed-output source exercises the flush PTS filter (not just the
        // packet loop). No encoder / no actual B-frames is a blocker, not a pass.
        {
            const QString delayed = dir.filePath(QStringLiteral("delayed.mp4"));
            const bool written = writeDelayedBoxMovie(delayed);
            int hasBFrames = -1;
            AVFormatContext *fmt = nullptr;
            if (written && avformat_open_input(&fmt, delayed.toUtf8().constData(), nullptr, nullptr) >= 0) {
                if (avformat_find_stream_info(fmt, nullptr) >= 0)
                    for (unsigned j = 0; j < fmt->nb_streams; ++j)
                        if (fmt->streams[j]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                            hasBFrames = fmt->streams[j]->codecpar->video_delay;
                avformat_close_input(&fmt);
            }
            ClipInfo whole = movClip(delayed, 0.0, double(kFrames) / kFps);
            whole.speedRamp = ramp(1.5);
            load({{whole}});
            const int held = boxOf(render(9.95), kRowsA);
            const int last = boxOf(tlrender::decodeClipSourceFrame(
                delayed, double(kFrames) / kFps, true, 0.0), kRowsA);
            ok = ok && written && hasBFrames > 0 && held == kFrames - 1 && held == last;
            d << " delayed has_b_frames=" << hasBFrames << " held=" << held
              << " last=" << last << " expect=" << kFrames - 1;
            if (!written || hasBFrames <= 0)
                d << " [BLOCKER: no delayed-output encoder fixture]";
        }
        // {0, 0.5}: the last timeline frame 119/30 s reads source 1.00417 + 1.98333 s, frame 90.
        ClipInfo slow = lone;
        slow.speedRamp = ramp(0.5);
        load({{slow}});
        {
            const int got = boxOf(render(119.0 / kFps), kRowsA);
            ok = ok && got == 90;
            d << " s0.5 last got=" << got << " expect=90";
        }
        // (0 s, 1.0) / (1 s, 2.0): source t + t^2/2 up to 1 s, then 1.5 + 2(t - 1).
        ClipInfo twoKeys = lone;
        twoKeys.speedRamp.keyframes = {{0, 1.0}, {1000000, 2.0}};
        load({{twoKeys}});
        const std::pair<double, int> keyed[] = {{0.5 + kOff, 50}, {1.0 + kOff, 76}, {1.5 + kOff, 106}};
        for (const auto &[t, expect] : keyed) {
            const int got = boxOf(render(t), kRowsA);
            const int viaRamp = frameAt(
                twoKeys.inPoint + double(twoKeys.speedRamp.timelineToSourceUs(usAt(t))) / 1e6);
            ok = ok && got == expect && viaRamp == expect;
            d << " 2kf t=" << t << " got=" << got << " timelineToSourceUs=" << viaRamp
              << " expect=" << expect;
        }
        tally.gate(2, ok, d.str());
    }

    // ---- G3 ----
    {
        std::ostringstream d;
        // One flag per leg (printed), all required.
        std::vector<std::pair<std::string, bool>> legs;
        const auto leg = [&](const std::string &name, bool ok) { legs.push_back({name, ok}); };
        // The source second renderFrameAt gives upper-track layer (track, clip) at `t`
        // (observeLayerTime; -1 when that layer is not admitted).
        const auto observed = [&](double t, int track, int clipIdx) {
            tlrender::detail::setLayerTimeObservationForTest(true);
            (void)render(t);
            double src = -1.0;
            for (const auto &o : tlrender::detail::observedLayerTimesForTest())
                if (o.trackIdx == track && o.clipIdx == clipIdx)
                    src = o.sourceSec;
            tlrender::detail::setLayerTimeObservationForTest(false);
            return src;
        };
        // (a1) pin: ramp-free A | B (Center).
        ClipInfo a = movClip(movA, 2.0 + kOff, 3.5);
        ClipInfo b = movClip(movB, 3.0 + kOff, 5.0);
        dissolve(a, b, TransitionAlignment::Center);
        load({{a, b}});
        const double owns[] = {-0.25, 0.25, 1.0};
        {
            bool ok = true;
            for (int i = 0; i < 3; ++i)
                ok = pinned("g3.a1." + std::to_string(i), sha16(render(3.5 + owns[i])), d) && ok;
            leg("a1", ok);
        }
        // B's source at B's own time `own` (bStart + own), read from B's box on V1 or, on V2
        // (its picture is not readable in this fixture, S0-2 (e3)(5)), from the observed
        // layer time when B is the layer admitted there; and the shared function called
        // directly on B's interval. `rel` is the closed form's offset from B's in point.
        const auto originLeg = [&](const std::string &name, const QVector<QVector<ClipInfo>> &tracks,
                                   int track, const ClipInfo &bClip, double bStart,
                                   const std::vector<std::pair<double, double>> &at) {
            load(tracks);
            bool ok = true;
            for (const auto &[own, rel] : at) {
                const double t = bStart + own;
                const double expectSec = bClip.inPoint + rel;
                const OverlapInterval *iv = intervalOf(track, 1);
                const double direct = iv ? tlrender::rampedSourceSecondInInterval(
                                               bClip, iv, t - iv->timelineStart)
                                         : -1.0;
                bool legOk = std::abs(direct - expectSec) < 1e-6;
                d << ' ' << name << " own=" << own << " closed=" << expectSec << " direct=" << direct;
                if (track == 0) {
                    const int got = boxOf(render(t), kRowsB);
                    legOk = legOk && got == frameAt(expectSec);
                    d << " box=" << got << " expect=" << frameAt(expectSec);
                } else {
                    const double src = observed(t, track, 1);
                    if (own >= 1.0)
                        legOk = legOk && src >= 0.0 && std::abs(src - expectSec) < 1e-6;
                    else if (src >= 0.0)
                        legOk = legOk && std::abs(src - expectSec) < 1e-6;
                    d << " layer=" << (src >= 0.0 ? std::to_string(src) : std::string("not B (neighbour)"));
                    if (src < 0.0 && own < 1.0)
                        d << " [inside the window B is not the admitted V2 layer (the neighbour is): printed; direct= is gated]";
                }
                ok = ok && legOk;
            }
            leg(name, ok);
        };
        // (a2) B {0, 1.5} (Center, speed 1, δ 0.5): in - 0.125 (neighbour layer), in + 0.625
        // (window), in + 1.75 (after the window).
        ClipInfo b15 = b;
        b15.speedRamp = ramp(1.5);
        originLeg("a2", {{a, b15}}, 0, b15, 3.5, {{-0.25, -0.125}, {0.25, 0.625}, {1.0, 1.75}});
        // (a2s) speed 2 (inPoint 1.5 + 1/240, borrowed clipIn 0.50417): + 3.5.
        {
            ClipInfo a2 = a;
            ClipInfo b2 = movClip(movB, 1.5 + kOff, 8.0);
            b2.speed = 2.0;
            b2.speedRamp = ramp(1.5);
            dissolve(a2, b2, TransitionAlignment::Center);
            originLeg("a2s", {{a2, b2}}, 0, b2, 3.5, {{1.0, 3.5}});
        }
        // (a2e) End (δ 1.0): + 2.0; Start (δ 0): + 1.5.
        {
            ClipInfo aE = a, bE = b15;
            dissolve(aE, bE, TransitionAlignment::End);
            originLeg("a2e-end", {{aE, bE}}, 0, bE, 3.5, {{1.0, 2.0}});
            ClipInfo aS = a, bS = b15;
            dissolve(aS, bS, TransitionAlignment::Start);
            originLeg("a2e-start", {{aS, bS}}, 0, bS, 3.5, {{1.0, 1.5}});
        }
        // (a3) A {0, 1.5} (inPoint 1/240, out + 2.0, trail borrowed 0.5 s -> clipOut in + 2.5):
        // local 1.4 -> in + 2.1, 1.6 -> in + 2.4, 1.7 -> in + 2.5 (held; unclamped 2.55 would
        // be frame 77). 2.2 (in + 2.5) is recorded: A is not readable under 70 % of the
        // dissolve (the ramp-free pin (p3) reads no A box at 75 % either).
        ClipInfo a3 = movClip(movA, kOff, 2.0);
        a3.speedRamp = ramp(1.5);
        ClipInfo b3 = movClip(movB, 3.0 + kOff, 3.0);
        dissolve(a3, b3, TransitionAlignment::Center);
        load({{a3, b3}});
        {
            bool ok = true;
            for (const auto &[local, rel] : std::vector<std::pair<double, double>>{{1.4, 2.1}, {1.6, 2.4}, {1.7, 2.5}}) {
                const int got = boxOf(render(local), kRowsA);
                ok = ok && got == frameAt(a3.inPoint + rel);
                d << " (a3) local=" << local << " got=" << got << " expect=" << frameAt(a3.inPoint + rel);
            }
            const OverlapInterval *iv = intervalOf(0, 0);
            const double held = iv ? tlrender::rampedSourceSecondInInterval(a3, iv, 2.2) : -1.0;
            ok = ok && iv && std::abs(held - (a3.inPoint + 2.5)) < 1e-6;
            d << " (a3) local=2.2 source=" << held << " expectSource=" << a3.inPoint + 2.5
              << " pixels declared (unreadable at 70% dissolve): ";
            d << " (a3) local=2.2 recorded " << boxText(render(2.2), kRowsA) << " expect="
              << frameAt(a3.inPoint + 2.5);
            leg("a3", ok);
        }
        // (a3e) (a3)'s A with an Echo, roto on A, local 1.6: the special composite decodes the
        // mapped in + 2.4, clamped to the borrowed bounds, not again to A's own out point (in +
        // 2.0). Gated against its own decoder (grabFrameAt, as f1) at in + 2.4 and away from the
        // own-clamped decode; renderFrameAt's first-at-or-after frame (73) is printed.
        {
            ClipInfo aE = a3;
            aE.effects = {VideoEffect::createEcho(0.1, 1, 0.5, 0)};
            load({{aE, b3}});
            timeline->setRotoEntries({{QStringLiteral("0:0"), fullRoto(QStringLiteral("0:0"))}});
            const int got = boxOf(window.buildSpecialClipComposite(1.6), kRowsA);
            timeline->setRotoEntries({});
            ClipInfo wide = timeline->videoTracks().at(0)->clips().at(0);
            const int ownClamp = boxOf(window.decodeClipFrameAtSourceTime(wide, a3.inPoint + 2.4), kRowsA);
            wide.outPoint = wide.duration;
            const int own = boxOf(window.decodeClipFrameAtSourceTime(wide, a3.inPoint + 2.4), kRowsA);
            const int exp = boxOf(render(1.6), kRowsA);
            d << " (a3e) composite=" << got << " expect=" << frameAt(a3.inPoint + 2.4)
              << " decoder(in+2.4)=" << own << " decoder(own clamp)=" << ownClamp << " renderFrameAt=" << exp;
            leg("a3e", got >= 0 && got == own && own != ownClamp);
        }
        // (a4) V2 A | B with B {0, 1.5} over a V1 clip: the same constants.
        originLeg("a4", {{movClip(movA, 0.0, 9.0)}, {a, b15}}, 1, b15, 3.5,
                  {{-0.25, -0.125}, {0.25, 0.625}, {1.0, 1.75}});
        // (b) Move the opaque V1 off canvas so V2 box rows remain readable.
        // Measured on the c0b7dfd production probe (r6-base-probe.log): 31/46/61/76.
        {
            ClipInfo v2 = movClip(movB, 1.0 + kOff, 4.0);
            v2.speedRamp = ramp(1.5);
            ClipInfo back = movClip(movA, 0.0, 9.0);
            back.videoDx = 2.0;
            load({{back}, {v2}});
            const int headBoxes[] = {31, 46, 61, 76};
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
                const double expectSec = v2.sourceSecondAtLocalTime(times[i]);
                const double src = observed(times[i], 1, 0);
                const auto hit = tlrender::parentLayerAt(timeline, usAt(times[i]), 1, 0);
                const int picture = boxOf(render(times[i]), kRowsB);
                ok = ok && picture == frameAt(expectSec) && frameAt(expectSec) == g1Mapper[i] && std::abs(src - expectSec) < 1e-6
                    && hit && std::abs(hit->sourceSeconds - expectSec) < 1e-6;
                d << " (b) t=" << times[i] << " box=" << picture << " HEAD=" << headBoxes[i] << " layer=" << src << " parentLayerAt="
                  << (hit ? hit->sourceSeconds : -1.0) << " expect=" << expectSec;
            }
            leg("b", ok);
        }
        // (c) Echo (0.1 s, 1 sample, decay 0.5): base f(t), past f(t - 0.1).
        {
            ClipInfo echoed = lone;
            echoed.effects = {VideoEffect::createEcho(0.1, 1, 0.5, 0)};
            load({{echoed}});
            const int past[] = {-1, 49, 71, 94};
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
                const auto bx = boxesOf(render(times[i]), kRowsA);
                const int base = bx.empty() ? -1 : bx[0].first;
                const int echo = bx.size() > 1 ? bx[1].first : -1;
                ok = ok && base == g1Mapper[i] && echo == past[i];
                d << " (c) t=" << times[i] << " base=" << base << " past=" << echo
                  << " expect=" << g1Mapper[i] << '/' << past[i];
            }
            leg("c", ok);
            // (c2) (a3)'s A with the Echo at local 1.6: base in + 2.4 (73), past in + 2.25 (68).
            ClipInfo a3e = a3;
            a3e.effects = {VideoEffect::createEcho(0.1, 1, 0.5, 0)};
            load({{a3e, b3}});
            const auto bx = boxesOf(render(1.6), kRowsA);
            const int base = bx.empty() ? -1 : bx[0].first;
            const int echo = bx.size() > 1 ? bx[1].first : -1;
            d << " (c2) base=" << base << " past=" << echo << " expect=73/68";
            leg("c2", base == 73 && echo == 68);
        }
        // (d) RollingShutter: the warp mixes neighbour frames by optical flow, so a band's
        // frame is not readable as a box. Recorded only.
        {
            ClipInfo rs = lone;
            rs.effects = {VideoEffect::createRollingShutterRepair()};
            load({{rs}});
            d << " (d) recorded:";
            for (double t : times) {
                const QImage img = render(t);
                d << " t=" << t << ' ' << boxText(img, kRowsA) << " sha=" << sha16(img);
            }
        }
        // (e) renderClipSourceAt at renderFrameAt's time.
        {
            load({{lone}});
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
                const int src = boxOf(tlrender::renderClipSourceAt(timeline, 0, 0, usAt(times[i]), size), kRowsA);
                ok = ok && src == g1Mapper[i] && boxOf(render(times[i]), kRowsA) == src;
                d << " (e) t=" << times[i] << " renderClipSourceAt=" << src;
            }
            leg("e", ok);
        }
        // (f1) Echo (non-brush, decodeClipFrameAtSourceTime) through a full-frame roto, which
        // routes the frame through the special composite: its box is its own decoder at the
        // mapper time (gated); its difference to renderFrameAt is printed, not gated.
        for (const bool identity : {false, true}) {
            ClipInfo e = lone;
            if (identity)
                e.speedRamp = ramp(1.0);
            e.effects = {VideoEffect::createEcho(0.1, 1, 0.5, 0)};
            load({{e}});
            timeline->setRotoEntries({{QStringLiteral("0:0"), fullRoto(QStringLiteral("0:0"))}});
            const ClipInfo placed = timeline->videoTracks().at(0)->clips().at(0);
            bool mapOk = true, pinOk = true;
            for (int i = 0; i < 4; ++i) {
                const QImage comp = window.buildSpecialClipComposite(times[i]);
                const int got = boxOf(comp, kRowsA);
                const int exp = boxOf(render(times[i]), kRowsA);
                if (identity) {
                    pinOk = pinned("g3.f1.identity" + std::to_string(i), sha16(comp), d) && pinOk;
                    d << " (f1 identity) composite=" << got << " renderFrameAt=" << exp;
                    continue;
                }
                const int own = boxOf(window.decodeClipFrameAtSourceTime(
                                          placed, placed.sourceSecondAtLocalTime(times[i])),
                                      kRowsA);
                mapOk = mapOk && got >= 0 && got == own;
                d << " (f1) t=" << times[i] << " composite=" << got << " decoder(mapper)=" << own
                  << " renderFrameAt=" << exp << " diff=" << got - exp;
            }
            if (identity) {
                leg("f1-identity-pin", pinOk);
            } else {
                leg("f1-decoder", mapOk);
            }
        }
        // (f2) brush (decodeClipSourceFrame): byte-equal to renderFrameAt, box the mapper frame.
        const bool brushFontAvailable = [] {
            for (const QString &family : QFontDatabase::families())
                if (QFontDatabase::isSmoothlyScalable(family)) return true;
            return false;
        }();
        if (!brushFontAvailable) d << " [font unavailable: text brush legs unverified]";
        const auto brushed = [&](ClipInfo c) {
            BrushAnimation brush;
            brush.setText(QStringLiteral("P"), QFont(QStringLiteral("Arial"), 30), QPointF(330.0, 200.0));
            brush.setBrushWidth(4.0);
            c.brushData = brush.toJson();
            KeyframeTrack progress(QStringLiteral("brush_progress"), 0.0);
            progress.addKeyframe(0.0, 1.0);
            c.keyframes.addTrack(progress);
            return c;
        };
        if (brushFontAvailable) for (const bool identity : {false, true}) {
            ClipInfo c = brushed(lone);
            if (identity)
                c.speedRamp = ramp(1.0);
            load({{c}});
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
                bool drew = false;
                const QImage comp = window.buildSpecialClipComposite(times[i], &drew);
                if (identity) {
                    ok = pinned("g3.f2.identity" + std::to_string(i), sha16(comp), d) && ok;
                    continue;
                }
                const QImage exp = render(times[i]);
                const int got = boxOf(comp, kRowsA);
                ok = ok && drew && sha16(comp) == sha16(exp) && got == g1Mapper[i];
                d << " (f2) t=" << times[i] << " drew=" << drew << " composite=" << got
                  << " sha=" << sha16(comp) << " renderFrameAt sha=" << sha16(exp);
            }
            leg(identity ? "f2-identity-pin" : "f2", ok);
        }
        // (f2s) brush B {0, 0.25} after an End dissolve (δ 1.0, the window before B's start),
        // own +1.5 s (the brush path skips B's first lead-in second, whatever the alignment):
        // in + 0.25·1.5 + 1.0·(0.25 - 1) = in - 0.375 (2.62917 s, frame 79), below its in point
        // (decodeClipSourceFrame does not clamp a forward target to inPoint).
        if (brushFontAvailable) {
            ClipInfo aS = a;
            ClipInfo bS = brushed(b);
            bS.speedRamp = ramp(0.25);
            dissolve(aS, bS, TransitionAlignment::End);
            load({{aS, bS}});
            bool drew = false;
            const QImage comp = window.buildSpecialClipComposite(5.0, &drew);
            const QImage exp = render(5.0);
            const int got = boxOf(comp, kRowsB);
            d << " (f2s) drew=" << drew << " composite=" << got << " renderFrameAt=" << boxOf(exp, kRowsB)
              << " expect=" << frameAt(b.inPoint - 0.375) << " byteEqual=" << (sha16(comp) == sha16(exp));
            leg("f2s", drew && got == frameAt(b.inPoint - 0.375) && sha16(comp) == sha16(exp));
        }
        // (f2e) brush {0, 1.5} on the whole file at 9.95 s (source used up at the file end):
        // the composite holds the file's last frame as export does (base exe: no picture);
        // the preview's Echo source (renderClipSourceFrameForEcho) keeps HEAD's null there.
        if (brushFontAvailable) {
            ClipInfo whole = brushed(movClip(movA, 0.0, double(kFrames) / kFps));
            whole.speedRamp = ramp(1.5);
            load({{whole}});
            bool drew = false;
            const QImage comp = window.buildSpecialClipComposite(9.95, &drew);
            const QImage exp = render(9.95);
            const int got = boxOf(comp, kRowsA);
            const QImage echoSource = tlrender::renderClipSourceFrameForEcho(
                timeline, whole, whole.outPoint, 9.95, size, usAt(9.95));
            d << " (f2e) drew=" << drew << " composite=" << got << " renderFrameAt=" << boxOf(exp, kRowsA)
              << " expect=" << kFrames - 1 << " byteEqual=" << (sha16(comp) == sha16(exp))
              << " forEchoNull=" << echoSource.isNull();
            leg("f2e", drew && got == kFrames - 1 && sha16(comp) == sha16(exp));
            leg("f2e-forEcho-head", echoSource.isNull());
        }
        // Delayed-output EOF parity: use G2's libx264 -bf 3 movie so a
        // buffered frame before clipOut cannot masquerade as a successful decode.
        if (brushFontAvailable) {
            const QString delayed = dir.filePath(QStringLiteral("delayed.mp4"));
            ClipInfo whole = brushed(movClip(delayed, 0.0, double(kFrames) / kFps));
            whole.speedRamp = ramp(1.5);
            load({{whole}});
            bool drew = false;
            const QImage comp = window.buildSpecialClipComposite(9.95, &drew);
            const QImage exp = render(9.95);
            const int got = boxOf(comp, kRowsA);
            const int exported = boxOf(exp, kRowsA);
            const bool byteEqual = sha16(comp) == sha16(exp);
            d << " (f2-delayed-eof) drew=" << drew << " composite=" << got
              << " renderFrameAt=" << exported << " expect=299 byteEqual=" << byteEqual;
            leg("f2-delayed-eof", drew && got == 299 && exported == 299 && byteEqual);
        }
        // (f3) (a2)'s B with an Echo, roto on B, own +1.0 s: its decoder at in + 1.75 s (gated);
        // the difference to renderFrameAt's B box is printed, not gated.
        {
            ClipInfo bE = b15;
            bE.effects = {VideoEffect::createEcho(0.1, 1, 0.5, 0)};
            load({{a, bE}});
            timeline->setRotoEntries({{QStringLiteral("0:1"), fullRoto(QStringLiteral("0:1"))}});
            const ClipInfo placed = timeline->videoTracks().at(0)->clips().at(1);
            const int got = boxOf(window.buildSpecialClipComposite(4.5), kRowsB);
            const int own = boxOf(window.decodeClipFrameAtSourceTime(placed, b.inPoint + 1.75), kRowsB);
            const int exp = boxOf(render(4.5), kRowsB);
            d << " (f3) composite=" << got << " decoder(in+1.75)=" << own << " renderFrameAt=" << exp
              << " diff=" << got - exp;
            leg("f3-decoder", got >= 0 && got == own);
        }
        timeline->setRotoEntries({});
        bool all = true;
        d << " | legs:";
        for (const auto &[name, ok] : legs) {
            d << ' ' << name << '=' << ok;
            all = all && ok;
        }
        if (!all) tally.gate(3, false, d.str());
        else if (!brushFontAvailable) tally.skip(3, (d.str() + " [font unavailable]").c_str());
        else tally.gate(3, true, d.str());
    }

    // ---- G4 (pins: the base exe's values) ----
    {
        std::ostringstream d;
        bool ok = true;
        const double three[] = {0.5 + kOff, 1.0 + kOff, 1.5 + kOff};
        const auto pinLeg = [&](const std::string &name, const QVector<QVector<ClipInfo>> &tracks,
                                const double *at, int n) {
            load(tracks);
            for (int i = 0; i < n; ++i)
                ok = pinned(name + std::to_string(i), sha16(render(at[i])), d) && ok;
        };
        ClipInfo reversed = lone;
        reversed.reversed = true;
        pinLeg("g4.reversed", {{reversed}}, three, 3);
        // Time remap (0 s, 0) / (4 s, 2 s) + {0, 1.5}: the remap wins (source in + t / 2).
        ClipInfo remapped = lone;
        remapped.timeRemapCurve.keys = {{0.0, 0.0}, {4.0, 2.0}};
        pinLeg("g4.remap", {{remapped}}, three, 3);
        {
            load({{remapped}});
            for (double t : three) {
                const int got = boxOf(render(t), kRowsA);
                ok = ok && got == frameAt(remapped.inPoint + t / 2.0);
                d << " remap t=" << t << " got=" << got << " expect=" << frameAt(remapped.inPoint + t / 2.0);
            }
            const ClipInfo placed = timeline->videoTracks().at(0)->clips().at(0);
            d << " placed rampIdentity=" << placed.speedRamp.isIdentity()
              << " remapKeys=" << placed.timeRemapCurve.keys.size()
              << " [remap wins, preview differs: declared]";
        }
        ClipInfo speed2 = movClip(movA, 1.0 + kOff, 8.0);
        speed2.speed = 2.0;
        speed2.speedRamp = speedramp::SpeedRamp::identity();
        pinLeg("g4.speed2", {{speed2}}, three, 3);
        // Render in place of the ramped clip with 0.5 s handles: the bake's isolated clip
        // (identity ramp, a time remap of the shared mapper, RenderInPlace.cpp handles > 0
        // branch) and the replacement (identity ramp) are not ramp-mapped clips: pinned.
        // Without handles the bake renders the ramped clip itself, as export does: gated at the
        // ramped boxes 55 / 77 / 100 (HEAD 47 / 62 / 77, declared behaviour change).
        for (const double handles : {0.5, 0.0}) {
            load({{lone}});
            renderinplace::Options options;
            options.outputSize = size;
            options.fps = kFps;
            options.handlesSec = handles;
            options.outputDir = dir.filePath(QStringLiteral("rip%1").arg(handles));
            QString error;
            const bool baked = renderinplace::renderClipInPlace(*timeline, 0, 0, options, nullptr, &error);
            const ClipInfo r = timeline->videoTracks().at(0)->clips().value(0);
            d << " rip handles=" << handles << " baked=" << baked << ' ' << error.toStdString()
              << " replacement in=" << r.inPoint << " out=" << r.outPoint
              << " rampIdentity=" << r.speedRamp.isIdentity() << " remapKeys=" << r.timeRemapCurve.keys.size();
            if (handles > 0.0) {
                ok = ok && baked;
                for (int i = 0; i < 3; ++i)
                    ok = pinned("g4.rip" + std::to_string(i), sha16(render(three[i])), d) && ok;
            } else {
                const int expect[] = {55, 77, 100};
                ok = ok && baked;
                d << " boxes:";
                for (int i = 0; i < 3; ++i) {
                    const int box = boxOf(render(three[i]), kRowsA);
                    ok = ok && box == expect[i];
                    d << ' ' << box << "/expect=" << expect[i];
                }
                d << " HEAD=47/62/77 [declared behaviour change: handles 0 bakes the ramp, as export]";
            }
        }
        // (p1) a sequence reference with {0, 1.5}; (p2) a {0, 1.5} clip inside the nested
        // sequence; (p3) that clip inside a dissolve of the nested sequence.
        const auto nested = [&](const QVector<ClipInfo> &kidsV1, double parentLength,
                                const speedramp::SpeedRamp &parentRamp) {
            timeline->setRotoEntries({});
            TimelineSequence kids;
            kids.id = QStringLiteral("ramp-kids");
            kids.name = kids.id;
            kids.videoTracks = {kidsV1};
            ClipInfo parent;
            parent.sequenceRefId = kids.id;
            parent.filePath = timeline_nesting::sequenceClipFilePath(kids.id);
            parent.duration = parent.outPoint = parentLength;
            parent.opacity = 1.0;
            parent.speedRamp = parentRamp;
            TimelineSequence main;
            main.id = QStringLiteral("ramp-main");
            main.name = main.id;
            main.videoTracks = {{parent}};
            timeline->setSequences(QVector<TimelineSequence>{main, kids}, main.id);
            timeline->refreshPlaybackSequence();
        };
        const auto pinNow = [&](const std::string &name, const double *at, int n) {
            for (int i = 0; i < n; ++i) {
                const QImage img = render(at[i]);
                ok = pinned(name + std::to_string(i), sha16(img), d) && ok;
                d << '(' << boxText(img, kRowsA) << '|' << boxText(img, kRowsB) << ')';
            }
        };
        nested({movClip(movA, 1.0 + kOff, 4.0)}, 4.0, ramp(1.5));
        pinNow("g4.p1.", three, 3);
        nested({lone}, 4.0, speedramp::SpeedRamp::identity());
        pinNow("g4.p2.", three, 3);
        {
            ClipInfo x = movClip(movA, 2.0 + kOff, 3.5);
            ClipInfo kid = movClip(movB, 3.0 + kOff, 4.0);
            kid.speedRamp = ramp(1.5);
            dissolve(x, kid, TransitionAlignment::Center);
            nested({x, kid}, 7.5, speedramp::SpeedRamp::identity());
            const double window3[] = {3.25, 3.75, 4.5};
            pinNow("g4.p3.", window3, 3);
        }
        tally.gate(4, ok, d.str());
    }

    // ---- G5 (GL): the paused preview read back against renderFrameAt ----
    {
        std::ostringstream d;
        MainWindow gw;
        Timeline *gt = gw.m_timeline;
        VideoPlayer *wp = gw.m_player;
        GLPreview *gl = wp ? wp->glPreview() : nullptr;
        bool glOk = false;
        if (gt && wp && gl && wp->isGLAccelerated()) {
            gl->setFixedSize(kW, kH);
            gl->setFrameTextureNearestForTest(true);
            gw.m_projectConfig.width = kW;
            gw.m_projectConfig.height = kH;
            gw.m_projectConfig.fps = kFps;
            emit gt->projectOutputConfigRestored(kW, kH, false);
            gt->setProjectOutputConfig(kW, kH, false, kFps);
            gw.show();
            pumpEvents(300);
            glOk = selftest::glContextUsable(gl);
        }
        if (!glOk) {
            tally.skip(5, "GL preview not usable");
        } else {
            const auto grab = [&] {
                QApplication::processEvents(QEventLoop::AllEvents, 25);
                const QImage raw = gl->grabFramebuffer();
                const qreal dpr = gl->devicePixelRatioF();
                QImage frame(kW, kH, QImage::Format_RGB32);
                frame.fill(Qt::black);
                for (int y = 0; y < kH && !raw.isNull(); ++y)
                    for (int x = 0; x < kW; ++x) {
                        const int rx = qMin(raw.width() - 1, int(std::floor((x + 0.5) * dpr)));
                        const int ry = qMin(raw.height() - 1, int(std::floor((y + 0.5) * dpr)));
                        frame.setPixel(x, y, raw.pixel(rx, ry) | 0xff000000u);
                    }
                return frame;
            };
            const auto gload = [&](const QVector<QVector<ClipInfo>> &tracks) {
                gt->restoreFromProject(tracks, QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
                gt->refreshPlaybackSequence();
                pumpEvents(400);
                wp->pause();
            };
            // A paused precise seek to `ms`; the read-back and renderFrameAt at the same time.
            const auto seekShow = [&](int ms, int rowBase) {
                bool seeked = false;
                const auto conn = QObject::connect(wp, &VideoPlayer::positionChanged, wp,
                    [&](double pos) { if (std::abs(pos - ms / 1000.0) < 0.002) seeked = true; });
                wp->seek(ms);
                (void)QTest::qWaitFor([&] { return seeked; }, 15000);
                QObject::disconnect(conn);
                pumpEvents(150);
                const int shown = boxOf(grab(), rowBase);
                const int exported = boxOf(tlrender::renderFrameAt(gt, qint64(ms) * 1000, size), rowBase);
                return std::make_pair(shown, exported);
            };
            bool aOk = true, bOk = true, c1Ok = true, c2Ok = true, c3Ok = true;
            gload({{lone}});
            const int ms4[] = {4, 504, 1004, 1504};
            for (int i = 0; i < 4; ++i) {
                const auto [shown, exported] = seekShow(ms4[i], kRowsA);
                aOk = aOk && shown == g1Mapper[i] && exported == g1Mapper[i];
                d << " (a) ms=" << ms4[i] << " preview=" << shown << " export=" << exported
                  << " expect=" << g1Mapper[i];
            }
            // (b) 80% of the clip (3.2 s): export holds clipOut (151, gate); the preview recorded.
            {
                const auto [shown, exported] = seekShow(3200, kRowsA);
                bOk = exported == 151;
                d << " (b) preview=" << shown << " export=" << exported << " [B2 declared]";
            }
            // (c1) B own +1.0 s: in + 1.75 (143); (c2) own +0.25: export gated at the closed
            // form (109), the paused preview pinned at the base exe's B#79 (declared, the in-window
            // preview picture is VideoPlayer-side, wave25-carryover).
            ClipInfo a = movClip(movA, 2.0 + kOff, 3.5);
            ClipInfo b = movClip(movB, 3.0 + kOff, 5.0);
            b.speedRamp = ramp(1.5);
            dissolve(a, b, TransitionAlignment::Center);
            gload({{a, b}});
            {
                const auto [shown, exported] = seekShow(4500, kRowsB);
                c1Ok = shown == 143 && exported == 143;
                d << " (c1) preview=" << shown << " export=" << exported << " expect=143";
            }
            {
                const auto [shown, exported] = seekShow(3750, kRowsB);
                c2Ok = shown == 79 && exported == 109;
                d << " (c2) preview=" << shown << " pin=79 export=" << exported << " expect=109"
                  << " [declared: in-window preview picture is VideoPlayer-side, wave25-carryover]";
            }
            // (c3) the V2 A | B over a V1 clip, +1.0 s: gated when readable.
            gload({{movClip(movA, 0.0, 9.0)}, {a, b}});
            {
                const auto [shown, exported] = seekShow(4500, kRowsB);
                const bool readable = shown >= 0;
                if (readable)
                    c3Ok = shown == 143 && exported == 143;
                d << " (c3) preview=" << shown << " export=" << exported << " expect=143"
                  << (readable ? " gated" : " recorded (unreadable)");
            }
            // (c4) G3 (a3)'s A after its borrowed handle is used up (local 2.2): recorded.
            {
                ClipInfo a3 = movClip(movA, kOff, 2.0);
                a3.speedRamp = ramp(1.5);
                ClipInfo b3 = movClip(movB, 3.0 + kOff, 3.0);
                dissolve(a3, b3, TransitionAlignment::Center);
                gload({{a3, b3}});
                const auto [shown, exported] = seekShow(2200, kRowsA);
                d << " (c4) preview=" << shown << " export=" << exported << " [B2 declared]";
            }
            d << " | legs: a=" << aOk << " b=" << bOk << " c1=" << c1Ok << " c2=" << c2Ok << " c3=" << c3Ok;
            tally.gate(5, aOk && bOk && c1Ok && c2Ok && c3Ok, d.str());
        }
    }
    return tally.finish("ramp-export");
}
