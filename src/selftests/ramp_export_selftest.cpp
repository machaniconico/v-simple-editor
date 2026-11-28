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
// US-2402 (export audio of a forward top-level clip that follows speed, the atempo flag
// or VEDITOR_AUDIO_ATEMPO, with a constant non-identity ramp s: resampled by
// f = entry.speed x s with the pitch, cut at the clip's timeline length):
// G6 (no ffmpeg) the plan: (a) the stage in both callers for s x speed, (a2) the trail
//    fade position, (b) unchanged chains pinned at the base exe, (c) the preview's
//    (ramp, atempo) arrays and the plan's factors name the same clip, (d) passthrough,
//    (e) the env force argument, (f) a borrowed entry's clipIn, (g) fixed-point stage
//    lengths for a short entry, (h) a fixed-point trail-fade start near 0 in both callers.
// G7 (ffmpeg) the rendered audio of a 440 Hz tone with a click every 0.5 s: click times
//    against an atempo-off control leg on the same m4a path, the pitch, the cut, the
//    trail fade, a lone untrimmed clip, (g) the track-FX path's reverb after the source
//    is used up and the next clip, (g2) the timeline length of a lone track-FX ramp
//    clip, (h) the (h) fade case of G6 exported on both paths, (i) a source shorter than
//    the span on the track-FX path (no reverb over the padding), (e) the A/V offset of a
//    video-only dissolve recorded; the preview mixer's clicks recorded beside them.
// G8 a two-keyframe ramp: the base exe's chain and one warning (none with atempo off),
//    also on a passthrough timeline.
#include "../AudioExportMix.h"
#include "../AudioMixer.h"
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
#include "../libavcore/AudioExtract.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"
#include "SelftestGl.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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
// A non-empty audioWav is muxed in as the audio stream (pcm_s16le, US-2402 G7).
bool writeDelayedBoxMovie(const QString &path, const QString &audioWav = QString())
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) return false;
    QProcess encoder;
    encoder.start(ffmpeg, QStringList{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
        QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-f"),
        QStringLiteral("rawvideo"), QStringLiteral("-pixel_format"), QStringLiteral("rgb24"),
        QStringLiteral("-video_size"), QStringLiteral("384x216"), QStringLiteral("-framerate"),
        QStringLiteral("30"), QStringLiteral("-i"), QStringLiteral("pipe:0")}
        + (audioWav.isEmpty()
               ? QStringList{QStringLiteral("-an")}
               : QStringList{QStringLiteral("-i"), audioWav, QStringLiteral("-map"),
                             QStringLiteral("0:v"), QStringLiteral("-map"), QStringLiteral("1:a"),
                             QStringLiteral("-c:a"), QStringLiteral("pcm_s16le")})
        + QStringList{QStringLiteral("-c:v"), QStringLiteral("libx264"),
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
    // US-2402 (G6 (b), G7 (c), G8): taken on the base exe HEAD f6b7ffd with the test-only
    // accessors of US-2402-raw/head-shim.py (chains: sha16 of the UTF-8 filter text; pcm: of the floats).
    {"g6.f2.atrimStart", "2.504167"},
    {"g6.i.off.post", "5ef6e65003bbfe77"},
    {"g6.i.off.single", "9e978c082cc3f3ec"},
    {"g6.i.rev.post", "5ef6e65003bbfe77"},
    {"g6.i.rev.single", "1d15f3d8b2e07455"},
    {"g6.identity.post", "cd25d8af550f8efe"},
    {"g6.identity.pre", "c35eb45542098172"},
    {"g6.identity.single", "8b2365d2a3080fb0"},
    {"g6.nestkid.post", "cd25d8af550f8efe"},
    {"g6.nestkid.pre", "c35eb45542098172"},
    {"g6.nestkid.single", "8b2365d2a3080fb0"},
    {"g6.nestparent.post", "cd25d8af550f8efe"},
    {"g6.nestparent.pre", "c35eb45542098172"},
    {"g6.nestparent.single", "8b2365d2a3080fb0"},
    {"g6.off15.post", "cd25d8af550f8efe"},
    {"g6.off15.pre", "c35eb45542098172"},
    {"g6.off15.single", "8b2365d2a3080fb0"},
    {"g6.reversed.post", "cd25d8af550f8efe"},
    {"g6.reversed.pre", "1aa2be76341a1ade"},
    {"g6.reversed.single", "7d90a3d2b47c2051"},
    {"g6.reversed15.post", "cd25d8af550f8efe"},
    {"g6.reversed15.pre", "1aa2be76341a1ade"},
    {"g6.reversed15.single", "7d90a3d2b47c2051"},
    {"g6.speed2identity.post", "cd25d8af550f8efe"},
    {"g6.speed2identity.pre", "c35eb45542098172"},
    {"g6.speed2identity.single", "8b2365d2a3080fb0"},
    {"g6.speed2off.post", "cd25d8af550f8efe"},
    {"g6.speed2off.pre", "c35eb45542098172"},
    {"g6.speed2off.single", "8b2365d2a3080fb0"},
    {"g6.speed2on.post", "cd25d8af550f8efe"},
    {"g6.speed2on.pre", "c35eb45542098172"},
    {"g6.speed2on.single", "8b2365d2a3080fb0"},
    {"g6.twokey.post", "cd25d8af550f8efe"},
    {"g6.twokey.pre", "c35eb45542098172"},
    {"g6.twokey.single", "8b2365d2a3080fb0"},
    {"g7.c.pcm", "b03a490ca7b32916"},
    {"g8.twokeyoff.post", "cd25d8af550f8efe"},
    {"g8.twokeyoff.pre", "c35eb45542098172"},
    {"g8.twokeyoff.single", "8b2365d2a3080fb0"},
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

// ---- US-2402 audio fixtures and measures ----
constexpr int kRate = 48000;
constexpr double kTone = 0.2;   // 440 Hz steady tone
constexpr double kClick = 0.7;  // 1 ms click every 0.5 s of the source (0.5, 1.0, ... 9.5)

// 10 s, 48 kHz stereo s16: the tone plus the clicks.
bool writeClickWav(const QString &path, int seconds = 10)
{
    const int frames = kRate * seconds;
    QByteArray pcm(frames * 4, '\0');
    auto *s = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        double v = kTone * std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / kRate);
        const int inHalf = i % (kRate / 2);
        if (i >= kRate / 2 && inHalf < kRate / 1000)
            v += kClick;
        const qint16 q = qint16(std::lround(qBound(-1.0, v, 1.0) * 32767.0));
        s[2 * i] = s[2 * i + 1] = q;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    bool ok = true;
    const auto put = [&](const char *data, qint64 size) { ok = f.write(data, size) == size && ok; };
    const auto u32 = [&](quint32 v) { put(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&](quint16 v) { put(reinterpret_cast<const char *>(&v), 2); };
    put("RIFF", 4); u32(36 + pcm.size()); put("WAVEfmt ", 8);
    u32(16); u16(1); u16(2); u32(kRate); u32(kRate * 4); u16(4); u16(16);
    put("data", 4); u32(pcm.size()); put(pcm.constData(), pcm.size());
    ok = f.flush() && ok;
    f.close();
    return ok && QFileInfo(path).size() == 44 + pcm.size();
}

// Mono float 48 kHz PCM of a file's first audio stream (empty on failure), through
// libavcore::extractAudioToWav (s16 mono, (L + R) / 2). Scaled by sqrt(2) so the levels
// match the L + R sum of ffmpeg's -ac 1 that the thresholds below were set on.
std::vector<float> decodeMono(const QString &path)
{
    const QString tmp = path + QStringLiteral(".decoded.wav");
    std::vector<double> mono;
    int rate = 0;
    const bool ok = libavcore::extractAudioToWav(path, tmp, kRate)
        && libavcore::readPcm16WavToMono(tmp, mono, rate) && rate == kRate;
    QFile::remove(tmp);
    if (!ok)
        return {};
    std::vector<float> pcm(mono.size());
    for (size_t i = 0; i < mono.size(); ++i)
        pcm[i] = float(mono[i] * std::sqrt(2.0));
    return pcm;
}

// Interleaved stereo s16 (the mixer probe) to mono float.
std::vector<float> monoOfS16(const QByteArray &pcm)
{
    const auto *s = reinterpret_cast<const qint16 *>(pcm.constData());
    std::vector<float> m(size_t(pcm.size()) / 4);
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = float((s[2 * i] + s[2 * i + 1]) / 65534.0);
    return m;
}

// Click onsets (|x| above the tone, 60 ms refractory) in seconds.
std::vector<double> clicksOf(const std::vector<float> &pcm)
{
    std::vector<double> t;
    for (size_t i = 0; i < pcm.size(); ++i) {
        if (std::abs(pcm[i]) > 0.45f) {
            t.push_back(double(i) / kRate);
            i += kRate * 60 / 1000;
        }
    }
    return t;
}

double rmsOf(const std::vector<float> &pcm, double t0, double t1)
{
    const size_t a = size_t(qMax(0.0, t0) * kRate), b = qMin(pcm.size(), size_t(qMax(0.0, t1) * kRate));
    double sum = 0.0;
    for (size_t i = a; i < b; ++i)
        sum += double(pcm[i]) * pcm[i];
    return b > a ? std::sqrt(sum / double(b - a)) : 0.0;
}

// Tone frequency from interpolated rising zero crossings in [t0, t1) (0 when none).
double toneHz(const std::vector<float> &pcm, double t0, double t1)
{
    const size_t a = size_t(qMax(0.0, t0) * kRate), b = qMin(pcm.size(), size_t(qMax(0.0, t1) * kRate));
    double first = -1.0, last = -1.0;
    int n = 0;
    for (size_t i = a + 1; i < b; ++i) {
        if (pcm[i - 1] < 0.0f && pcm[i] >= 0.0f) {
            const double x = double(i - 1) + pcm[i - 1] / double(pcm[i - 1] - pcm[i]);
            if (first < 0.0) first = x;
            last = x;
            ++n;
        }
    }
    return n > 1 ? (n - 1) * double(kRate) / (last - first) : 0.0;
}

// The source clicks inside (in, in + used) at timeline start + (m - in) / f + c.
std::vector<double> expectedClicks(double in, double used, double f, double start, double c)
{
    std::vector<double> t;
    for (int k = 1; k < 20; ++k) {
        const double m = 0.5 * k;
        if (m > in + 0.005 && m < in + used - 0.005)
            t.push_back(start + (m - in) / f + c);
    }
    return t;
}

bool clicksMatch(const std::vector<double> &got, const std::vector<double> &want, double tol,
                 std::ostringstream &d)
{
    bool ok = got.size() == want.size();
    double worst = 0.0;
    for (size_t i = 0; i < got.size() && i < want.size(); ++i)
        worst = std::max(worst, std::abs(got[i] - want[i]));
    ok = ok && worst <= tol;
    d << " clicks=" << got.size() << "/expect=" << want.size() << " worst_ms=" << worst * 1000.0
      << " first=" << (got.empty() ? -1.0 : got.front()) << " expect_first="
      << (want.empty() ? -1.0 : want.front()) << (ok ? "" : " [clicks mismatch]");
    return ok;
}

std::string sha16Of(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(16).toStdString();
}

std::atomic<int> g_rampWarnings{0};
QtMessageHandler g_previousRampHandler = nullptr;
void captureRampWarning(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg && message.contains(QStringLiteral("speed ramp of a clip that follows speed is not constant")))
        ++g_rampWarnings;
    if (g_previousRampHandler)
        g_previousRampHandler(type, context, message);
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
        for (int g = 1; g <= 8; ++g)
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
    // ==== US-2402: export audio of a clip that follows its constant speed ramp ====
    const QString wav = dir.filePath(QStringLiteral("ramp-click.wav"));
    const bool wavOk = writeClickWav(wav);
    const auto loadAV = [&](const QVector<QVector<ClipInfo>> &v, const QVector<QVector<ClipInfo>> &a) {
        timeline->setRotoEntries({});
        timeline->restoreFromProject(v, a, 0.0, -1.0, -1.0, 100);
    };
    // An audio clip: `length` source seconds from `in`; s > 0 sets the ramp {0, s}.
    const auto audioClip = [&](const QString &path, double in, double length, double s, bool atempo) {
        ClipInfo c;
        c.filePath = path;
        c.displayName = QStringLiteral("ramp-audio");
        c.duration = 10.0;
        c.inPoint = in;
        c.outPoint = in + length;
        if (s > 0.0)
            c.speedRamp = ramp(s);
        c.atempoEnabled = atempo;
        return c;
    };
    const auto planNow = [&]() {
        return audioexport::buildMixPlan(timeline, nullptr, audioexport::MixOptions{});
    };
    // The plan index of A1 clip `clipIdx` (-1 when none).
    const auto planIndex = [](const audioexport::MixPlan &plan, int clipIdx) {
        for (int i = 0; i < plan.entries.size(); ++i)
            if (plan.entries[i].sourceTrack == 0 && plan.entries[i].sourceClipIndex == clipIdx)
                return i;
        return -1;
    };
    const auto num = [](double v) { return QString::number(v, 'g', 15); };
    const auto chainsOf = [](const audioexport::MixPlan &plan, int i) {
        return QStringList{audioexport::singleChainForTest(plan, i),
                           audioexport::trackFxPreChainForTest(plan, i),
                           audioexport::trackFxPostChainForTest(plan, i)};
    };
    // The content (2) stage after an atrim of [in, end): resample by f, pad / cut at T.
    const auto stageText = [&](double in, double end, double f, double length) {
        return QStringLiteral("atrim=start=%1:end=%2,asetpts=PTS-STARTPTS,aresample=48000,asetrate=%3,"
                              "aresample=48000,asetpts=PTS-STARTPTS,apad=whole_dur=%4,atrim=end=%4,aformat=")
            .arg(audioexport::ffmpegNumber(in), audioexport::ffmpegNumber(end)).arg(qRound64(48000.0 * f))
            .arg(audioexport::ffmpegNumber(length));
    };
    // A V1 clip on another file: with V1 empty the export has no picture and
    // timelineAudioMatchesPassthrough answers passthrough before any audio rule.
    const QVector<QVector<ClipInfo>> v1Other = {{movClip(movA, 0.0, 4.0)}};

    // ---- G6 (plan, no ffmpeg) ----
    {
        std::ostringstream d;
        bool ok = wavOk;
        // (a) atempo on, s x entry.speed: both callers carry the stage, the factor is v * s.
        bool aOk = true;
        for (double v : {1.0, 2.0}) {
            for (double s : {0.5, 1.5, 2.0}) {
                ClipInfo a = audioClip(wav, 0.5, 4.0, s, true);
                a.speed = v;
                loadAV(v1Other, {{a}});
                const audioexport::MixPlan plan = planNow();
                const int i = planIndex(plan, 0);
                const double f = v * s;
                const double length = 4.0 / v;
                const double factor = i >= 0 ? plan.rampFactors.value(i, 1.0) : -1.0;
                const QStringList chains = i >= 0 ? chainsOf(plan, i) : QStringList{};
                bool legOk = i >= 0 && chains.size() == 3 && !plan.passthrough;
                if (std::abs(f - 1.0) <= 1e-9) {
                    // v 2 x s 0.5: f = 1, no stage (content (1)).
                    legOk = legOk && factor == 1.0 && !chains.join(QString()).contains(QStringLiteral("asetrate"));
                } else {
                    const QString stage = stageText(0.5, 0.5 + qMin(4.0, length * f), f, length);
                    legOk = legOk && std::abs(factor - f) <= 1e-12 && chains[0].contains(stage)
                        && chains[1].contains(stage);
                }
                legOk = legOk && !chains.join(QString()).contains(QStringLiteral("atempo"));
                d << " (a)v" << v << "s" << s << ":f=" << factor << (legOk ? "" : " [FAIL]");
                if (!legOk)
                    d << " entries=" << plan.entries.size() << " passthrough=" << plan.passthrough;
                if (!legOk && i >= 0)
                    d << " single=" << chains[0].toStdString() << " pre=" << chains[1].toStdString();
                aOk = aOk && legOk;
            }
        }
        // The factor is clamp(entry.speed x s, 0.1, 10), as the mixer's speedMul: the product
        // reaches both ends (setClipSpeed up to 4, the ramp dialog up to 5).
        {
            const double high = audioexport::exportAudioRampFactor(audioClip(wav, 0.5, 4.0, 5.0, true),
                                                                   false, 4.0, false);
            const double low = audioexport::exportAudioRampFactor(audioClip(wav, 0.5, 4.0, 0.25, true),
                                                                  false, 0.25, false);
            const bool clampOk = std::abs(high - 10.0) <= 1e-12 && std::abs(low - 0.1) <= 1e-12;
            d << " (a)clamp:v4s5=" << high << " v0.25s0.25=" << low << (clampOk ? "" : " [FAIL]");
            aOk = aOk && clampOk;
        }
        // (a2) the trail fade: s 0.5 -> T - 1, s 1.5 -> span / 1.5 - 1; the lead fade unchanged.
        bool a2Ok = true;
        for (double s : {0.5, 1.5}) {
            ClipInfo a = audioClip(wav, 0.5, 4.0, s, true);
            a.leadIn.type = TransitionType::FadeIn;
            a.leadIn.duration = 0.5;
            a.trailOut.type = TransitionType::CrossDissolve;
            a.trailOut.duration = 1.0;
            loadAV(v1Other, {{a}});
            const audioexport::MixPlan plan = planNow();
            const int i = planIndex(plan, 0);
            const QStringList chains = i >= 0 ? chainsOf(plan, i) : QStringList{};
            const QString trail = QStringLiteral("afade=t=out:curve=qsin:st=%1:d=1")
                                      .arg(num((s < 1.0 ? 4.0 : 4.0 / 1.5) - 1.0));
            const QString lead = QStringLiteral("afade=t=in:curve=qsin:d=0.5,afade=t=out");
            const bool legOk = chains.size() == 3 && chains[0].contains(trail) && chains[2].contains(trail)
                && chains[0].contains(lead) && chains[2].contains(lead);
            d << " (a2)s" << s << ":" << (legOk ? "ok" : "[FAIL]");
            if (chains.size() == 3)
                d << " post=" << chains[2].toStdString();
            a2Ok = a2Ok && legOk;
        }
        // (b) pins: the base exe's chains (single, track-FX PreFx, PostFx).
        bool bOk = true;
        const auto pinChains = [&](const std::string &name, int clipIdx) {
            const audioexport::MixPlan plan = planNow();
            const int i = planIndex(plan, clipIdx);
            const QStringList chains = i >= 0 ? chainsOf(plan, i) : QStringList{QString(), QString(), QString()};
            const double factor = i >= 0 ? plan.rampFactors.value(i, 1.0) : -1.0;
            bool pinOk = factor == 1.0;
            pinOk = pinned(name + ".single", sha16Of(chains[0].toUtf8()), d) && pinOk;
            pinOk = pinned(name + ".pre", sha16Of(chains[1].toUtf8()), d) && pinOk;
            pinOk = pinned(name + ".post", sha16Of(chains[2].toUtf8()), d) && pinOk;
            if (!pinOk)
                d << " f=" << factor << " single=" << chains[0].toStdString();
            return pinOk;
        };
        const auto lonePin = [&](const std::string &name, ClipInfo a) {
            loadAV(v1Other, {{a}});
            return pinChains(name, 0);
        };
        bOk = lonePin("g6.off15", audioClip(wav, 0.5, 4.0, 1.5, false)) && bOk;
        bOk = lonePin("g6.identity", audioClip(wav, 0.5, 4.0, 1.0, true)) && bOk;
        {
            ClipInfo a = audioClip(wav, 0.5, 4.0, 0.0, true);
            a.reversed = true;
            bOk = lonePin("g6.reversed", a) && bOk;
            a.speedRamp = ramp(1.5);
            bOk = lonePin("g6.reversed15", a) && bOk;
        }
        // Wave 25 (B5): speed=2 with atempo off stays pinned at 1x HEAD hash;
        // speed=2 with atempo on (no ramp or identity ramp) resamples at 2x and cuts at 2.0s.
        {
            ClipInfo a = audioClip(wav, 0.5, 4.0, 0.0, false);
            a.speed = 2.0;
            bOk = lonePin("g6.speed2off", a) && bOk;
        }
        for (double rampSpeed : {0.0, 1.0}) {
            ClipInfo a = audioClip(wav, 0.5, 4.0, rampSpeed, true);
            a.speed = 2.0;
            loadAV(v1Other, {{a}});
            const audioexport::MixPlan plan = planNow();
            const int i = planIndex(plan, 0);
            const QStringList chains = i >= 0 ? chainsOf(plan, i) : QStringList{};
            const bool spOk = i >= 0 && std::abs(plan.rampFactors.value(i, 1.0) - 2.0) <= 1e-12
                && chains.size() == 3
                && chains[0].contains(QStringLiteral("asetrate=96000"))
                && chains[0].contains(QStringLiteral("atrim=end=2.000000"))
                && chains[1].contains(QStringLiteral("asetrate=96000"))
                && chains[1].contains(QStringLiteral("atrim=end=2.000000"));
            if (!spOk)
                d << " speed2(on,r=" << rampSpeed << ")[FAIL] f="
                  << (i >= 0 ? plan.rampFactors.value(i, 1.0) : -1.0)
                  << " single=" << (chains.isEmpty() ? "" : chains[0].toStdString());
            bOk = bOk && spOk;
        }
        {
            ClipInfo a = audioClip(wav, 0.5, 4.0, 0.0, true);
            a.speedRamp.keyframes = {{0, 1.0}, {1000000, 2.0}};
            bOk = lonePin("g6.twokey", a) && bOk;
        }
        // The nested pins: a flattened entry names its parent reference clip.
        const auto nestedAudio = [&](const ClipInfo &kid, const speedramp::SpeedRamp &parentRamp) {
            timeline->setRotoEntries({});
            TimelineSequence kids;
            kids.id = QStringLiteral("ramp-audio-kids");
            kids.name = kids.id;
            kids.audioTracks = {{kid}};
            ClipInfo parent;
            parent.sequenceRefId = kids.id;
            parent.filePath = timeline_nesting::sequenceClipFilePath(kids.id);
            parent.duration = parent.outPoint = 4.0;
            parent.speedRamp = parentRamp;
            parent.atempoEnabled = true;
            TimelineSequence main;
            main.id = QStringLiteral("ramp-audio-main");
            main.name = main.id;
            main.videoTracks = v1Other;
            main.audioTracks = {{parent}};
            timeline->setSequences(QVector<TimelineSequence>{main, kids}, main.id);
            timeline->refreshPlaybackSequence();
        };
        nestedAudio(audioClip(wav, 0.5, 4.0, 0.0, false), ramp(1.5));
        {
            const audioexport::MixPlan plan = planNow();
            d << " nested1.entries=" << plan.entries.size();
            bOk = plan.entries.size() == 1 && pinChains("g6.nestparent", 0) && bOk;
        }
        nestedAudio(audioClip(wav, 0.5, 4.0, 1.5, true), speedramp::SpeedRamp::identity());
        {
            const audioexport::MixPlan plan = planNow();
            d << " nested2.entries=" << plan.entries.size();
            bOk = plan.entries.size() == 1 && pinChains("g6.nestkid", 0) && bOk;
        }
        // (c) the preview's (ramp, atempo) arrays and the plan's factors name the same A1 clip.
        bool cOk = false;
        {
            ClipInfo first = audioClip(wav, 0.5, 2.0, 0.0, false);
            ClipInfo second = audioClip(wav, 3.0, 3.0, 1.5, true);
            loadAV(v1Other, {{first, second}});
            timeline->refreshPlaybackSequence();
            pumpEvents(300); // past the 50 ms emitSequenceChanged debounce
            AudioMixer *mixer = window.m_player ? window.m_player->audioMixer() : nullptr;
            const QVector<PlaybackEntry> seq = timeline->computeAudioPlaybackSequence();
            const QVector<speedramp::SpeedRamp> ramps = mixer ? mixer->speedRampsForTest() : QVector<speedramp::SpeedRamp>{};
            const QVector<bool> flags = mixer ? mixer->atempoFlagsForTest() : QVector<bool>{};
            const audioexport::MixPlan plan = planNow();
            cOk = mixer && seq.size() == 2 && ramps.size() == 2 && flags.size() == 2 && plan.entries.size() == 2;
            for (int e = 0; cOk && e < seq.size(); ++e) {
                const int i = planIndex(plan, seq[e].sourceClipIndex);
                const bool ramped = !ramps[e].isIdentity();
                const double factor = i >= 0 ? plan.rampFactors.value(i, 1.0) : -1.0;
                d << " (c)entry" << e << ":clip=" << seq[e].sourceClipIndex << " previewRamp=" << ramped
                  << " previewAtempo=" << flags[e] << " planFactor=" << factor;
                cOk = i >= 0 && ramped == (seq[e].sourceClipIndex == 1) && flags[e] == ramped
                    && (ramped ? std::abs(factor - 1.5) <= 1e-12 : factor == 1.0);
            }
            if (!cOk)
                d << " (c) [FAIL] mixer=" << (mixer != nullptr) << " seq=" << seq.size() << " ramps=" << ramps.size();
        }
        // (d) passthrough: a lone untrimmed clip on V1's file.
        bool dOk = true;
        for (int form = 0; form < 3; ++form) {
            ClipInfo v = movClip(movA, 0.0, 10.0);
            ClipInfo a = audioClip(movA, 0.0, 10.0, form == 1 ? 1.0 : 1.5, form != 2);
            loadAV({{v}}, {{a}});
            const bool passthrough = planNow().passthrough;
            const bool want = form != 0; // 0: atempo on + {0, 1.5}; 1: identity; 2: atempo off
            d << " (d)" << (form == 0 ? "on15" : form == 1 ? "identity" : "off15") << ":passthrough="
              << passthrough << (passthrough == want ? "" : " [FAIL]");
            dOk = dOk && passthrough == want;
        }
        // (e) env force through the factor function's argument.
        bool eOk = false;
        {
            const ClipInfo a = audioClip(wav, 0.5, 4.0, 1.5, false);
            const double on = audioexport::exportAudioRampFactor(a, false, 1.0, true);
            const double off = audioexport::exportAudioRampFactor(a, false, 1.0, false);
            const QString chainOn = buildExportAudioMixEntryFilterChain(0, QStringLiteral("0.5"),
                QStringLiteral("4.5"), 0, QStringLiteral("1"), AudioChannelMode::Stereo, false, 1.0,
                TransitionType::None, 0.0, TransitionType::None, 0.0, on, 4.0);
            const QString chainOff = buildExportAudioMixEntryFilterChain(0, QStringLiteral("0.5"),
                QStringLiteral("4.5"), 0, QStringLiteral("1"), AudioChannelMode::Stereo, false, 1.0,
                TransitionType::None, 0.0, TransitionType::None, 0.0, off, 4.0);
            const QString head = buildExportAudioMixEntryFilterChain(0, QStringLiteral("0.5"),
                QStringLiteral("4.5"), 0, QStringLiteral("1"), AudioChannelMode::Stereo);
            // Inside one binary this only shows the f = 1 call equals the default-argument
            // call; byte-identity with the base exe comes from the audio-fixture comparison
            // (rule (11)(b)) and the base exe's pinned chain constants in (b).
            eOk = std::abs(on - 1.5) <= 1e-12 && off == 1.0 && chainOn.contains(QStringLiteral("asetrate=72000"))
                && chainOff == head;
            d << " (e)envOn=" << on << " envOff=" << off << " offChainEqualsDefaultArgs=" << (chainOff == head)
              << (eOk ? "" : " [FAIL]");
        }
        // (f) A | B, 1 s CrossDissolve (Center), B atempo on + {0, 1.5}: B's entry borrowed
        // 0.5 s (clipIn 3.0 - 0.5, start 3.5 - 0.5, end 7.5): closed form 2.5 / 7 / 72000 / 4.5,
        // plus an off-grid fixture (rule 132) with B's inPoint shifted by kOff = 1/240 s:
        // clipIn = 601/240 (2.504167), start = 3.0, end = 7.5, atrim end = 1681/240 (7.004167),
        // length = 4.5 (4.500000), asetrate = 72000.
        bool fOk = false;
        {
            ClipInfo a = audioClip(wav, 2.0, 3.5, 0.0, false);
            ClipInfo b = audioClip(wav, 3.0, 4.0, 1.5, true);
            dissolve(a, b, TransitionAlignment::Center);
            loadAV(v1Other, {{a, b}});
            const audioexport::MixPlan plan = planNow();
            const int i = planIndex(plan, 1);
            const QStringList chains = i >= 0 ? chainsOf(plan, i) : QStringList{};
            const QString stage = stageText(2.5, 7.0, 1.5, 4.5);
            bool f1Ok = i >= 0 && std::abs(plan.rampFactors.value(i, 1.0) - 1.5) <= 1e-12 && chains[0].contains(stage)
                && chains[1].contains(stage);
            d << " (f)";
            if (i >= 0)
                d << "clipIn=" << plan.entries[i].clipIn << " start=" << plan.entries[i].timelineStart
                  << " end=" << plan.entries[i].timelineEnd << " f=" << plan.rampFactors.value(i, 1.0)
                  << " atrimStart2.5=" << chains[0].contains(QStringLiteral("atrim=start=2.500000:"));
            if (!f1Ok)
                d << " [FAIL] single=" << (i >= 0 ? chains[0].toStdString() : std::string("none"));

            // Off-grid borrowed handle: B inPoint = 3.0 + 1/240 = 721/240.
            // Hand-computed closed-form literals: clipIn = 601/240, start = 3.0, end = 7.5,
            // length = 4.5, span = 4.5, min(span, length * 1.5) = 4.5, atrim end = 1681/240.
            ClipInfo a2 = audioClip(wav, 2.0, 3.5, 0.0, false);
            ClipInfo b2 = audioClip(wav, 3.0 + kOff, 4.0, 1.5, true);
            dissolve(a2, b2, TransitionAlignment::Center);
            loadAV(v1Other, {{a2, b2}});
            const audioexport::MixPlan plan2 = planNow();
            const int i2 = planIndex(plan2, 1);
            const QStringList chains2 = i2 >= 0 ? chainsOf(plan2, i2) : QStringList{};
            const QString stageOff = QStringLiteral(
                "atrim=start=2.504167:end=7.004167,asetpts=PTS-STARTPTS,aresample=48000,"
                "asetrate=72000,aresample=48000,asetpts=PTS-STARTPTS,"
                "apad=whole_dur=4.500000,atrim=end=4.500000,aformat=");
            const QRegularExpression startRe(QStringLiteral("atrim=start=([^:]+):"));
            const QRegularExpressionMatch startM = chains2.size() == 3 ? startRe.match(chains2[0]) : QRegularExpressionMatch();
            const std::string gotStart = startM.hasMatch() ? startM.captured(1).toStdString() : std::string();
            const bool startPinned = pinned("g6.f2.atrimStart", gotStart, d);
            const bool f2Ok = i2 >= 0
                && std::abs(plan2.entries[i2].clipIn - 601.0 / 240.0) <= 1e-12
                && std::abs(plan2.entries[i2].timelineStart - 3.0) <= 1e-12
                && std::abs(plan2.entries[i2].timelineEnd - 7.5) <= 1e-12
                && std::abs(plan2.rampFactors.value(i2, 1.0) - 1.5) <= 1e-12
                && chains2.size() == 3 && chains2[0].contains(stageOff) && chains2[1].contains(stageOff)
                && startPinned;
            d << " (f2)";
            if (i2 >= 0)
                d << "clipIn=" << plan2.entries[i2].clipIn << " start=" << plan2.entries[i2].timelineStart
                  << " end=" << plan2.entries[i2].timelineEnd << " f=" << plan2.rampFactors.value(i2, 1.0);
            if (!f2Ok)
                d << " [FAIL] single=" << (i2 >= 0 ? chains2[0].toStdString() : std::string("none"));
            fOk = f1Ok && f2Ok;
        }
        // (g) a short ramped entry (clipIn 0, T 0.00008, f 0.5): the stage's atrim / apad
        // lengths are fixed-point (FFmpeg's duration parser rejects an exponent like 4e-05).
        bool gOk = false;
        {
            const QString chain = buildExportAudioMixEntryPreFxFilterChain(0, QStringLiteral("0"),
                QStringLiteral("4"), AudioChannelMode::Stereo, false, 1.0, 0.5, 0.00008);
            const bool exponent = chain.contains(QRegularExpression(QStringLiteral("[0-9.][eE][-+]?[0-9]")));
            gOk = !exponent && chain.contains(QStringLiteral("atrim=start=0:end=0.000040,"))
                && chain.contains(QStringLiteral("apad=whole_dur=0.000080,atrim=end=0.000080,"));
            d << " (g)exponent=" << exponent << (gOk ? "" : " [FAIL] chain=" + chain.toStdString());
        }
        // (h) span 3.000015, f 1.5, a 2 s CrossDissolve trail: the fade starts at
        // 3.000015 / 1.5 - 2 = 1e-5, printed fixed-point in both callers ('g' gives 1e-05).
        bool hOk = false;
        {
            ClipInfo a = audioClip(wav, 0.5, 3.000015, 1.5, true);
            a.trailOut.type = TransitionType::CrossDissolve;
            a.trailOut.duration = 2.0;
            loadAV(v1Other, {{a}});
            const audioexport::MixPlan plan = planNow();
            const int i = planIndex(plan, 0);
            const QStringList chains = i >= 0 ? chainsOf(plan, i) : QStringList{};
            const QRegularExpression stRe(QStringLiteral("afade=t=out:curve=qsin:st=([^:]+):"));
            hOk = chains.size() == 3 && std::abs(plan.rampFactors.value(i, 1.0) - 1.5) <= 1e-12;
            for (int c : {0, 2}) {
                const QRegularExpressionMatch m = chains.size() == 3 ? stRe.match(chains[c]) : QRegularExpressionMatch();
                const QString st = m.hasMatch() ? m.captured(1) : QString();
                bool parsed = false;
                const double v = st.toDouble(&parsed);
                const bool stOk = m.hasMatch() && !st.contains(QLatin1Char('e')) && !st.contains(QLatin1Char('E'))
                    && parsed && std::abs(v - 1e-5) <= 1e-9;
                d << " (h)" << (c == 0 ? "single" : "post") << ".st=" << st.toStdString() << (stOk ? "" : " [FAIL]");
                if (!stOk && chains.size() == 3)
                    d << " chain=" << chains[c].toStdString();
                hOk = hOk && stOk;
            }
        }
        // (i) non-ramped near-zero fade start (atempo off + reversed, span 3.000015, 3.0 s FadeOut):
        // must remain byte-identical to HEAD ('g',15 exponent preserved), not fixed-point.
        bool iOk = true;
        {
            const QRegularExpression stRe(QStringLiteral("afade=t=out:curve=qsin:st=([^:]+):"));
            for (const bool rev : {false, true}) {
                ClipInfo a = audioClip(wav, 0.5, 3.000015, 1.5, rev);
                a.reversed = rev;
                a.trailOut.type = TransitionType::FadeOut;
                a.trailOut.duration = 3.0;
                loadAV(v1Other, {{a}});
                const audioexport::MixPlan plan = planNow();
                const int i = planIndex(plan, 0);
                const QStringList chains = i >= 0 ? chainsOf(plan, i) : QStringList{};
                const std::string tag = rev ? "g6.i.rev" : "g6.i.off";
                bool legOk = chains.size() == 3 && plan.rampFactors.value(i, 1.0) == 1.0;
                legOk = pinned(tag + ".single", chains.size() == 3 ? sha16Of(chains[0].toUtf8()) : std::string(), d) && legOk;
                legOk = pinned(tag + ".post", chains.size() == 3 ? sha16Of(chains[2].toUtf8()) : std::string(), d) && legOk;
                for (int c : {0, 2}) {
                    const QRegularExpressionMatch m = chains.size() == 3 ? stRe.match(chains[c]) : QRegularExpressionMatch();
                    const QString st = m.hasMatch() ? m.captured(1) : QString();
                    const bool expOk = m.hasMatch() && (st.contains(QLatin1Char('e')) || st.contains(QLatin1Char('E')));
                    d << " (i)" << (rev ? "rev" : "off") << (c == 0 ? ".single.st=" : ".post.st=")
                      << st.toStdString() << (expOk ? "" : " [FAIL]");
                    legOk = legOk && expOk;
                }
                iOk = iOk && legOk;
            }
            // US-2420 waveFix: direct builder calls where no ramp stage applies
            // (reversed=true or timelineLength=0 with rampFactor=2.0) must keep legacy 'g',15 fades.
            for (const bool revCase : {true, false}) {
                const QString ch = buildExportAudioMixEntryFilterChain(
                    0, QStringLiteral("0.500000"), QStringLiteral("3.500015"), 0,
                    QStringLiteral("1.000000"), AudioChannelMode::Stereo, revCase, 1.0,
                    TransitionType::None, 0.0, TransitionType::FadeOut, 3.0,
                    2.0, revCase ? 4.0 : 0.0);
                const QRegularExpressionMatch m = stRe.match(ch);
                const QString st = m.hasMatch() ? m.captured(1) : QString();
                const bool noStageExpOk = !ch.contains(QStringLiteral("asetrate="))
                    && m.hasMatch() && (st.contains(QLatin1Char('e')) || st.contains(QLatin1Char('E')));
                d << " (i)nostage-" << (revCase ? "rev" : "len0") << ".st=" << st.toStdString()
                  << (noStageExpOk ? "" : " [FAIL]");
                iOk = iOk && noStageExpOk;
            }
        }
        ok = ok && aOk && a2Ok && bOk && cOk && dOk && eOk && fOk && gOk && hOk && iOk;
        d << " legs: a=" << aOk << " a2=" << a2Ok << " b=" << bOk << " c=" << cOk << " d=" << dOk
          << " e=" << eOk << " f=" << fOk << " g=" << gOk << " h=" << hOk << " i=" << iOk;
        tally.gate(6, ok, d.str());
    }

    // ---- G7 (ffmpeg): the rendered clicks, pitch, cut and fade ----
    const QString ffmpeg = audioexport::findFfmpegBinary();
    // One file with the box video and the click audio, so a lone untrimmed clip can be
    // passthrough on HEAD (the audio file must be V1's file).
    const QString media = dir.filePath(QStringLiteral("ramp-av.mov"));
    const bool muxOk = !ffmpeg.isEmpty() && wavOk && writeDelayedBoxMovie(media, wav);
    if (ffmpeg.isEmpty()) {
        tally.skip(7, "ffmpeg not found (G7 renders the export audio)");
    } else if (!wavOk || !muxOk) {
        tally.gate(7, false, std::string("fixture creation failed: click wav=") + (wavOk ? "1" : "0")
                                 + " ramp-av.mov=" + (muxOk ? "1" : "0"));
    } else {
        std::ostringstream d;
        // The export audio of the loaded timeline: the source when passthrough, else the mix.
        bool lastPassthrough = false;
        const auto exportAudio = [&]() -> std::vector<float> {
            const audioexport::MixPlan plan = planNow();
            lastPassthrough = plan.passthrough;
            if (plan.passthrough)
                return decodeMono(media);
            QString error;
            const QString out = audioexport::executeMixPlan(plan, [] { return false; }, &error);
            if (out.isEmpty()) {
                d << " [mix failed: " << error.toStdString() << ']';
                return {};
            }
            std::vector<float> pcm = decodeMono(out);
            QFile::remove(out);
            return pcm;
        };
        const double in = 0.25 + kOff;
        const auto vClip = [&](double vin, double length, double s) {
            ClipInfo v = movClip(media, vin, length);
            if (s > 0.0)
                v.speedRamp = ramp(s);
            return v;
        };
        // (c) control: atempo off + {0, 1.5} -> 1x (pin); c = its first click's delay.
        loadAV({{vClip(in, 4.0, 1.5)}}, {{audioClip(media, in, 4.0, 1.5, false)}});
        const std::vector<float> pcmC = exportAudio();
        const std::vector<double> clicksC = clicksOf(pcmC);
        const double c = clicksC.empty() ? 0.0 : clicksC.front() - (0.5 - in);
        bool cOk = !pcmC.empty() && !lastPassthrough && c > -0.005 && c < 0.06;
        d << " (c)c_ms=" << c * 1000.0 << " passthrough=" << lastPassthrough;
        cOk = clicksMatch(clicksC, expectedClicks(in, 4.0, 1.0, 0.0, c), 0.010, d) && cOk;
        const double hzC = toneHz(pcmC, 0.5 - in + c + 0.03, 1.0 - in + c - 0.03);
        d << " hz=" << hzC;
        cOk = cOk && std::abs(hzC - 440.0) <= 440.0 * 0.03;
        cOk = pinned("g7.c.pcm", sha16Of(QByteArray(reinterpret_cast<const char *>(pcmC.data()),
                                                    int(pcmC.size() * sizeof(float)))), d) && cOk;
        // (a) atempo on + {0, 1.5}: clicks at m / 1.5 + c, 660 Hz, the video frame, silence after.
        bool aOk = false;
        {
            loadAV({{vClip(in, 4.0, 1.5)}}, {{audioClip(media, in, 4.0, 1.5, true)}});
            const std::vector<float> pcm = exportAudio();
            const std::vector<double> got = clicksOf(pcm);
            const std::vector<double> want = expectedClicks(in, 4.0, 1.5, 0.0, c);
            d << " (a)";
            aOk = !pcm.empty() && clicksMatch(got, want, 0.010, d);
            const double hz = want.size() > 1 ? toneHz(pcm, want[0] + 0.03, want[1] - 0.03) : 0.0;
            // rmsOf is 0 past the PCM's end: the silence window must be covered.
            const bool covered = pcm.size() >= size_t((4.0 - 0.03) * kRate);
            const double silence = rmsOf(pcm, 4.0 / 1.5 + 0.03, 4.0 - 0.03);
            d << " hz=" << hz << " pcm_s=" << double(pcm.size()) / kRate << (covered ? "" : " [short pcm]")
              << " silence_rms=" << silence << " frames:";
            aOk = aOk && std::abs(hz - 660.0) <= 660.0 * 0.03 && covered && silence < 0.005;
            bool framesOk = got.size() == want.size();
            for (size_t k = 0; framesOk && k < got.size(); ++k) {
                const double m = in + (want[k] - c) * 1.5; // the click's source second
                const int box = boxOf(render(got[k] - c), kRowsA);
                d << ' ' << box << '/' << frameAt(m);
                framesOk = std::abs(box - frameAt(m)) <= 1;
            }
            aOk = aOk && framesOk;
        }
        // (b) atempo on + {0, 0.5} followed by a plain clip at 4 s: clicks at m / 0.5 + c,
        // 220 Hz, cut at 4 s (no click of the ramped clip after it), the next clip's clicks
        // measured and gated separately from the ramped clip's clicks.
        bool bOk = false;
        {
            const double in2 = 5.35;
            loadAV({{vClip(in, 4.0, 0.5), vClip(in2, 2.0, 0.0)}},
                   {{audioClip(media, in, 4.0, 0.5, true), audioClip(media, in2, 2.0, 0.0, false)}});
            const std::vector<float> pcm = exportAudio();
            const std::vector<double> mine = expectedClicks(in, 2.0, 0.5, 0.0, c);
            const std::vector<double> next = expectedClicks(in2, 2.0, 1.0, 4.0, c);
            const std::vector<double> allGot = clicksOf(pcm);
            std::vector<double> gotOwn, gotNext;
            for (double t : allGot) {
                if (t >= 4.0 - 0.005)
                    gotNext.push_back(t);
                else
                    gotOwn.push_back(t);
            }
            d << " (b)own:";
            const bool ownOk = clicksMatch(gotOwn, mine, 0.010, d);
            d << " next_measured=";
            for (size_t k = 0; k < gotNext.size(); ++k)
                d << (k ? "," : "") << gotNext[k];
            double nextMaxDev = 0.0;
            for (size_t k = 0; k < gotNext.size() && k < next.size(); ++k)
                nextMaxDev = std::max(nextMaxDev, std::abs(gotNext[k] - next[k]));
            d << " next_max_dev_ms=" << nextMaxDev * 1000.0 << " next:";
            const bool nextOk = clicksMatch(gotNext, next, 0.010, d);
            const double hz = mine.size() > 1 ? toneHz(pcm, mine[0] + 0.03, mine[1] - 0.03) : 0.0;
            d << " hz=" << hz;
            bOk = !pcm.empty() && ownOk && nextOk && std::abs(hz - 220.0) <= 220.0 * 0.03;
        }
        // (b2) (b)'s clip alone with a 1 s trail fade: the tone falls over its last second.
        bool b2Ok = false;
        {
            ClipInfo a = audioClip(media, in, 4.0, 0.5, true);
            a.trailOut.type = TransitionType::FadeOut;
            a.trailOut.duration = 1.0;
            loadAV({{vClip(in, 4.0, 0.5)}}, {{a}});
            const std::vector<float> pcm = exportAudio();
            const std::vector<double> clicks = expectedClicks(in, 2.0, 0.5, 0.0, c);
            // RMS of [t0, t1) leaving out 20 ms around each click.
            const auto toneRms = [&](double t0, double t1) {
                std::vector<float> part;
                for (size_t i = size_t(t0 * kRate); i < size_t(t1 * kRate) && i < pcm.size(); ++i) {
                    const double t = double(i) / kRate;
                    bool nearClick = false;
                    for (double k : clicks)
                        nearClick = nearClick || std::abs(t - k) < 0.020;
                    if (!nearClick)
                        part.push_back(pcm[i]);
                }
                return rmsOf(part, 0.0, double(part.size()) / kRate);
            };
            const double mid = toneRms(1.8, 2.2);
            const bool covered = pcm.size() >= size_t(4.0 * kRate);
            bool falling = covered && mid > 0.05;
            double prev = mid * 1.05;
            d << " (b2)pcm_s=" << double(pcm.size()) / kRate << (covered ? "" : " [short pcm]")
              << " mid=" << mid << " fade:";
            for (int w = 0; w < 10; ++w) {
                const double r = toneRms(3.0 + 0.1 * w, 3.1 + 0.1 * w);
                d << ' ' << r;
                falling = falling && r < prev;
                prev = r;
            }
            const double tail = toneRms(3.95, 4.0);
            d << " last50ms=" << tail;
            b2Ok = falling && tail <= 0.1 * mid;
        }
        // (d) a lone untrimmed clip (passthrough on HEAD), atempo on + {0, 1.5}.
        bool dOk = false;
        {
            loadAV({{vClip(0.0, 10.0, 1.5)}}, {{audioClip(media, 0.0, 10.0, 1.5, true)}});
            const std::vector<float> pcm = exportAudio();
            const std::vector<double> want = expectedClicks(0.0, 10.0, 1.5, 0.0, c);
            d << " (d)passthrough=" << lastPassthrough;
            const std::vector<double> got = clicksOf(pcm);
            dOk = !pcm.empty() && !lastPassthrough && clicksMatch(got, want, 0.010, d);
            const double hz = want.size() > 1 ? toneHz(pcm, want[0] + 0.03, want[1] - 0.03) : 0.0;
            const bool covered = pcm.size() >= size_t((10.0 - 0.03) * kRate);
            const double silence = rmsOf(pcm, 10.0 / 1.5 + 0.03, 9.97);
            d << " hz=" << hz << " pcm_s=" << double(pcm.size()) / kRate << (covered ? "" : " [short pcm]")
              << " silence_rms=" << silence << " frames:";
            dOk = dOk && std::abs(hz - 660.0) <= 660.0 * 0.03 && covered && silence < 0.005;
            bool framesOk = got.size() == want.size();
            for (size_t k = 0; framesOk && k < got.size(); ++k) {
                const double m = (want[k] - c) * 1.5; // the click's source second (in = 0)
                const int box = boxOf(render(got[k] - c), kRowsA);
                d << ' ' << box << '/' << frameAt(m);
                framesOk = std::abs(box - frameAt(m)) <= 1;
            }
            dOk = dOk && framesOk;
        }
        // (g) the track-FX path (reverb on A1): A (atempo on + {0, 1.5}) uses its 4 s of source
        // up at 4 / 1.5 s of its 4 s on the timeline, then B. The DSP must not run over A's
        // padding (a gap): no reverb ring after the source ends. B is compared with the same B
        // after a non-ramped A of the same source length by its click times and by its RMS once
        // A's reverb tail has decayed (the reverb history carries A's different content into B
        // and the AAC encoder's state differs, so B can not be sha-equal; B_maxdiff is recorded).
        bool gOk = false, g2Ok = false, hOk = false, iOk = false;
        {
            trackfx::Chain rev;
            rev.reverbEnabled = rev.reverb.enabled = true;
            rev.reverb.mixRatio = 0.5;
            rev.reverb.decaySeconds = 1.3;
            AudioMixer fxMixer;
            fxMixer.setTrackChain(0, rev);
            const double in2 = 5.35, lenB = 4.5;
            const auto fxExport = [&](double sA, double *factor, bool *trackFx) -> std::vector<float> {
                loadAV({{vClip(in, 4.0, sA), vClip(in2, lenB, 0.0)}},
                       {{audioClip(media, in, 4.0, sA, true), audioClip(media, in2, lenB, 0.0, false)}});
                const audioexport::MixPlan plan =
                    audioexport::buildMixPlan(timeline, &fxMixer, audioexport::MixOptions{});
                const int a = planIndex(plan, 0);
                *factor = a >= 0 ? plan.rampFactors.value(a, 1.0) : -1.0;
                *trackFx = plan.useTrackFx && !plan.passthrough;
                QString error;
                const QString out = audioexport::executeMixPlan(plan, [] { return false; }, &error);
                if (out.isEmpty()) {
                    d << " [mix failed: " << error.toStdString() << ']';
                    return {};
                }
                std::vector<float> pcm = decodeMono(out);
                QFile::remove(out);
                return pcm;
            };
            double fRamp = 0.0, fPlain = 0.0;
            bool fxRamp = false, fxPlain = false;
            const std::vector<float> pcm = fxExport(1.5, &fRamp, &fxRamp);
            const std::vector<float> plain = fxExport(0.0, &fPlain, &fxPlain);
            const bool covered = pcm.size() >= size_t((4.0 - 0.03) * kRate);
            const double silence = rmsOf(pcm, 4.0 / 1.5 + 0.05, 4.0 - 0.03);
            const double b0 = 4.0 + 3.0, b1 = 4.0 + lenB - 0.05;
            const bool bCovered = pcm.size() >= size_t(b1 * kRate) && plain.size() >= size_t(b1 * kRate);
            const double rmsB = rmsOf(pcm, b0, b1), rmsPlainB = rmsOf(plain, b0, b1);
            double maxDiff = 0.0;
            for (size_t i = size_t(b0 * kRate); bCovered && i < size_t(b1 * kRate); ++i)
                maxDiff = std::max(maxDiff, double(std::abs(pcm[i] - plain[i])));
            // B's clicks (from 4 s) against the plain export's.
            const auto clicksFrom4 = [&](const std::vector<float> &x) {
                std::vector<double> t;
                for (double k : clicksOf(x))
                    if (k > 4.0 - 0.005)
                        t.push_back(k);
                return t;
            };
            const std::vector<double> clicksB = clicksFrom4(pcm), clicksPlainB = clicksFrom4(plain);
            bool clicksSame = !clicksPlainB.empty() && clicksB.size() == clicksPlainB.size();
            for (size_t k = 0; clicksSame && k < clicksB.size(); ++k)
                clicksSame = std::abs(clicksB[k] - clicksPlainB[k]) <= 0.001;
            d << " (g)trackFx=" << fxRamp << '/' << fxPlain << " f=" << fRamp << '/' << fPlain
              << " pcm_s=" << double(pcm.size()) / kRate << (covered ? "" : " [short pcm]")
              << " silence_rms=" << silence << " B_rms=" << rmsB << "/plain=" << rmsPlainB
              << " B_maxdiff=" << maxDiff << " [recorded]" << (bCovered ? "" : " [short B pcm]")
              << " B_clicks=" << clicksB.size() << "/plain=" << clicksPlainB.size()
              << (clicksSame ? "" : " [B clicks differ]");
            gOk = fxRamp && fxPlain && std::abs(fRamp - 1.5) < 1e-9 && fPlain == 1.0 && covered
                && silence < 0.005 && bCovered && rmsPlainB > 0.05
                && std::abs(rmsB - rmsPlainB) <= 0.01 * rmsPlainB && clicksSame;
            // (g2) the same ramped A alone (no B): the track-FX mix keeps A's 4 s timeline
            // length (the cut before the DSP is padded back), silent after the source ends.
            loadAV({{vClip(in, 4.0, 1.5)}}, {{audioClip(media, in, 4.0, 1.5, true)}});
            const audioexport::MixPlan lonePlan =
                audioexport::buildMixPlan(timeline, &fxMixer, audioexport::MixOptions{});
            const int lone = planIndex(lonePlan, 0);
            const double fLone = lone >= 0 ? lonePlan.rampFactors.value(lone, 1.0) : -1.0;
            const bool fxLone = lonePlan.useTrackFx && !lonePlan.passthrough;
            QString loneError;
            const QString loneOut = audioexport::executeMixPlan(lonePlan, [] { return false; }, &loneError);
            std::vector<float> lonePcm;
            if (loneOut.isEmpty()) {
                d << " [mix failed: " << loneError.toStdString() << ']';
            } else {
                lonePcm = decodeMono(loneOut);
                QFile::remove(loneOut);
            }
            const long long loneDiff = static_cast<long long>(lonePcm.size()) - 4LL * kRate;
            const double loneSilence = rmsOf(lonePcm, 4.0 / 1.5 + 0.05, 4.0 - 0.03);
            g2Ok = fxLone && std::abs(fLone - 1.5) < 1e-9 && loneDiff <= kRate / kFps && loneDiff >= -(kRate / kFps)
                && loneSilence < 0.005;
            d << " (g2)trackFx=" << fxLone << " f=" << fLone << " pcm_samples=" << lonePcm.size()
              << " want=" << 4 * kRate << " diff=" << loneDiff << " silence_rms=" << loneSilence
              << (g2Ok ? "" : " [FAIL]");
            // The mix of the loaded A1 timeline (single chain, or track FX with fxMixer); the
            // PCM is empty when FFmpeg or the mix failed.
            const auto mixPcm = [&](bool trackFx, audioexport::MixPlan *planOut) -> std::vector<float> {
                *planOut = audioexport::buildMixPlan(timeline, trackFx ? &fxMixer : nullptr,
                                                     audioexport::MixOptions{});
                QString error;
                const QString out = audioexport::executeMixPlan(*planOut, [] { return false; }, &error);
                if (out.isEmpty()) {
                    d << " [mix failed: " << error.toStdString() << ']';
                    return {};
                }
                std::vector<float> pcm = decodeMono(out);
                QFile::remove(out);
                return pcm;
            };
            // (h) the G6 (h) clip (span 3.000015, f 1.5, 2 s trail, fade start 1e-5) exported:
            // FFmpeg accepts the fade start on the single-chain and the track-FX path.
            {
                ClipInfo a = audioClip(media, in, 3.000015, 1.5, true);
                a.trailOut.type = TransitionType::CrossDissolve;
                a.trailOut.duration = 2.0;
                loadAV(v1Other, {{a}});
                hOk = true;
                for (bool trackFx : {false, true}) {
                    audioexport::MixPlan plan;
                    const std::vector<float> pcm = mixPcm(trackFx, &plan);
                    const int i = planIndex(plan, 0);
                    const bool legOk = i >= 0 && !plan.passthrough && plan.useTrackFx == trackFx
                        && std::abs(plan.rampFactors.value(i, 1.0) - 1.5) < 1e-9
                        && pcm.size() >= size_t((3.000015 - 0.03) * kRate);
                    d << " (h)" << (trackFx ? "trackFx" : "single") << ":pcm_s=" << double(pcm.size()) / kRate
                      << (legOk ? "" : " [FAIL]");
                    hOk = hOk && legOk;
                }
            }
            // (i) a 1 s source under a 4 s span, f 1.5, track reverb: the source ends at
            // 1 / 1.5 s; the padding after it is a gap the reverb does not run over.
            {
                const QString shortWav = dir.filePath(QStringLiteral("ramp-click-1s.wav"));
                const bool shortOk = writeClickWav(shortWav, 1);
                loadAV(v1Other, {{audioClip(shortWav, 0.0, 4.0, 1.5, true)}});
                audioexport::MixPlan plan;
                const std::vector<float> pcm = shortOk ? mixPcm(true, &plan) : std::vector<float>{};
                const int i = planIndex(plan, 0);
                const double f = i >= 0 ? plan.rampFactors.value(i, 1.0) : -1.0;
                const double length = i >= 0 ? plan.entries[i].timelineEnd - plan.entries[i].timelineStart : -1.0;
                const long long diff = static_cast<long long>(pcm.size()) - qRound64(length * kRate);
                const double source = rmsOf(pcm, 0.1, 1.0 / 1.5 - 0.05);
                const double tail = rmsOf(pcm, 1.0 / 1.5 + 0.05, 4.0 / 1.5 - 0.03);
                iOk = shortOk && i >= 0 && plan.useTrackFx && !plan.passthrough && std::abs(f - 1.5) < 1e-9
                    && std::abs(length - 4.0) < 1e-9 && diff <= kRate / kFps && diff >= -(kRate / kFps)
                    && source > 0.05 && tail < 0.005;
                d << " (i)trackFx=" << plan.useTrackFx << " f=" << f << " length=" << length
                  << " pcm_samples=" << pcm.size() << " diff=" << diff << " source_rms=" << source
                  << " after_source_rms=" << tail << (iOk ? "" : " [FAIL]");
            }
        }
        // (e) [recorded] a video-only dissolve; B's audio (atempo on + {0, 1.5}) borrows nothing.
        {
            ClipInfo av = vClip(2.0, 3.5, 0.0);
            ClipInfo bv = vClip(3.0, 4.0, 1.5);
            dissolve(av, bv, TransitionAlignment::Center);
            loadAV({{av, bv}}, {{audioClip(media, 2.0, 3.5, 0.0, false), audioClip(media, 3.0, 4.0, 1.5, true)}});
            const std::vector<float> pcm = exportAudio();
            const std::vector<double> got = clicksOf(pcm);
            // The first click of B after the dissolve (B's audio starts at 3.5, source 3.0).
            double tClick = -1.0;
            for (double t : got)
                if (t - c > 4.05 && tClick < 0.0)
                    tClick = t - c;
            const double m = tClick > 0.0 ? 3.0 + (tClick - 3.5) * 1.5 : -1.0;
            const int box = tClick > 0.0 ? boxOf(render(tClick), kRowsA) : -1;
            d << " (e)click=" << tClick << " source=" << m << " box=" << box;
            if (tClick <= 0.0 || box < 0) {
                d << " av_offset_ms=missing reason=" << (tClick <= 0.0 ? "no_click" : "box_unreadable");
            } else {
                d << " av_offset_ms=" << std::lround((box / double(kFps) - m) / 1.5 * 1000.0) << " [F45 declared]";
            }
            d << " no preview value to compare: S0-2 (e3)(3) could not separate the"
                 " preview's audible - playhead from the playback delay";
        }
        // The preview mixer on the same timeline (recorded; the premise of (a) / (c)).
        {
            for (bool atempo : {true, false}) {
                loadAV({{vClip(in, 4.0, 1.5)}}, {{audioClip(media, in, 4.0, 1.5, atempo)}});
                AudioMixer mixer;
                mixer.setSequence(timeline->computeAudioPlaybackSequence());
                mixer.setSpeedRamps({ramp(1.5)});
                mixer.setAtempoFlags({atempo});
                const std::vector<float> pcm = monoOfS16(mixer.renderPcmForTest(kRate * 4));
                const std::vector<double> got = clicksOf(pcm);
                const double hz = got.size() > 1 ? toneHz(pcm, got[0] + 0.03, got[1] - 0.03) : 0.0;
                d << " preview(" << (atempo ? "on" : "off") << ") clicks=" << got.size() << " first="
                  << (got.empty() ? -1.0 : got.front()) << " second=" << (got.size() > 1 ? got[1] : -1.0)
                  << " hz=" << hz;
            }
        }
        const bool ok = cOk && aOk && bOk && b2Ok && dOk && gOk && g2Ok && hOk && iOk;
        d << " legs: c=" << cOk << " a=" << aOk << " b=" << bOk << " b2=" << b2Ok
          << " d=" << dOk << " g=" << gOk << " g2=" << g2Ok << " h=" << hOk << " i=" << iOk << " (c is from the m4a mix path, the same container as every leg)";
        tally.gate(7, ok, d.str());
    }

    // ---- G8: a ramp that is not constant: the base exe's chain and one warning ----
    {
        std::ostringstream d;
        bool ok = wavOk;
        for (bool atempo : {true, false}) {
            ClipInfo a = audioClip(wav, 0.5, 4.0, 0.0, atempo);
            a.speedRamp.keyframes = {{0, 1.0}, {1000000, 2.0}};
            loadAV(v1Other, {{a}});
            g_rampWarnings = 0;
            g_previousRampHandler = qInstallMessageHandler(captureRampWarning);
            const audioexport::MixPlan plan = planNow();
            qInstallMessageHandler(g_previousRampHandler);
            const int warnings = g_rampWarnings.load();
            const int i = planIndex(plan, 0);
            const QStringList chains = i >= 0 ? chainsOf(plan, i) : QStringList{QString(), QString(), QString()};
            const std::string name = atempo ? "g6.twokey" : "g8.twokeyoff";
            d << (atempo ? " on:" : " off:") << "warnings=" << warnings;
            ok = warnings == (atempo ? 1 : 0) && ok;
            ok = pinned(name + ".single", sha16Of(chains[0].toUtf8()), d) && ok;
            ok = pinned(name + ".pre", sha16Of(chains[1].toUtf8()), d) && ok;
            ok = pinned(name + ".post", sha16Of(chains[2].toUtf8()), d) && ok;
        }
        // A lone untrimmed clip on V1's file is passthrough: the warning still fires (atempo on).
        for (bool atempo : {true, false}) {
            ClipInfo a = audioClip(movA, 0.0, 10.0, 0.0, atempo);
            a.speedRamp.keyframes = {{0, 1.0}, {1000000, 2.0}};
            loadAV({{movClip(movA, 0.0, 10.0)}}, {{a}});
            g_rampWarnings = 0;
            g_previousRampHandler = qInstallMessageHandler(captureRampWarning);
            const bool passthrough = planNow().passthrough;
            qInstallMessageHandler(g_previousRampHandler);
            const int warnings = g_rampWarnings.load();
            d << (atempo ? " passthrough-on:" : " passthrough-off:") << "passthrough=" << passthrough
              << " warnings=" << warnings;
            ok = passthrough && warnings == (atempo ? 1 : 0) && ok;
        }
        // A project-loaded ramp whose first key starts after clip time 0 holds source 0
        // before that key, so it is not a constant speed from timeline 0: factor 1, base-exe
        // chain, and one warning when atempo is on (none when off).
        {
            const QJsonObject lateJson = QJsonDocument::fromJson(
                QByteArrayLiteral(R"({"keyframes":[{"t":1000000,"s":1.5}]})")).object();
            const speedramp::SpeedRamp lateRamp = speedramp::SpeedRamp::fromJson(lateJson);
            const bool fixtureOk = lateRamp.keyframes.size() == 1
                && lateRamp.keyframes.first().clipTimeUs == 1000000
                && std::abs(lateRamp.keyframes.first().speed - 1.5) <= 1e-9;
            ok = fixtureOk && ok;
            for (bool atempo : {true, false}) {
                ClipInfo a = audioClip(wav, 0.5, 4.0, 0.0, atempo);
                a.speedRamp = lateRamp;
                loadAV(v1Other, {{a}});
                g_rampWarnings = 0;
                g_previousRampHandler = qInstallMessageHandler(captureRampWarning);
                const audioexport::MixPlan plan = planNow();
                qInstallMessageHandler(g_previousRampHandler);
                const int warnings = g_rampWarnings.load();
                const int i = planIndex(plan, 0);
                const double factor = i >= 0 ? plan.rampFactors.value(i, 1.0) : -1.0;
                const QStringList chains = i >= 0 ? chainsOf(plan, i) : QStringList{QString(), QString(), QString()};
                const bool legOk = factor == 1.0 && warnings == (atempo ? 1 : 0);
                d << (atempo ? " late-on:" : " late-off:") << "f=" << factor << " warnings=" << warnings
                  << (legOk ? "" : " [FAIL]");
                ok = legOk && ok;
                ok = pinned("g6.off15.single", sha16Of(chains[0].toUtf8()), d) && ok;
                ok = pinned("g6.off15.pre", sha16Of(chains[1].toUtf8()), d) && ok;
                ok = pinned("g6.off15.post", sha16Of(chains[2].toUtf8()), d) && ok;
            }
        }
        tally.gate(8, ok, d.str());
    }
    return tally.finish("ramp-export");
}
