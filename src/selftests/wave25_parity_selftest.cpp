#include "../AudioExportMix.h"
#include "../AudioMixer.h"
#include "../GLPreview.h"
#include "../MainWindow.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoPlayer.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <cmath>
#include <cstdio>

namespace {

bool writeSimpleToneWav(const QString &path, double durationSec)
{
    const int sr = 48000;
    const int ch = 2;
    const int frames = qMax(1, qRound(durationSec * sr));
    const quint32 dataBytes = static_cast<quint32>(frames * ch * 2);
    const quint32 riffSize = 36 + dataBytes;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    auto w32 = [&](quint32 v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto w16 = [&](quint16 v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    f.write("RIFF", 4); w32(riffSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(ch); w32(sr);
    w32(sr * ch * 2); w16(ch * 2); w16(16);
    f.write("data", 4); w32(dataBytes);
    QByteArray pcm(static_cast<int>(dataBytes), '\0');
    auto *dst = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        const qint16 s = static_cast<qint16>(std::sin(2.0 * 3.141592653589793 * 440.0 * i / sr) * 8000.0);
        dst[i * 2] = s;
        dst[i * 2 + 1] = s;
    }
    return f.write(pcm) == pcm.size();
}

bool writeIndexEncodedMp4(const QString &path, int width, int height, int fps, int numFrames)
{
    libavcore::EncodeRequest req;
    req.width = width;
    req.height = height;
    req.fps = fps;
    req.videoBitrateBits = 2'000'000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;

    libavcore::FrameEncoder enc;
    if (enc.open(req).has_value())
        return false;

    for (int f = 0; f < numFrames; ++f) {
        QImage img(width, height, QImage::Format_RGB888);
        // Encode frame index smoothly into RGB channels so even H.264 preserves it cleanly.
        const int r = qBound(0, (f * 3) % 256, 255);
        const int g = qBound(0, (f / 20) * 40, 255);
        const int b = (f >= numFrames - 5) ? 220 : 40;
        img.fill(QColor(r, g, b));
        if (!enc.pushFrameRgb24(img.constBits(), img.bytesPerLine(), f))
            return false;
    }
    return !enc.finalize().has_value();
}

QColor centerPixel(const QImage &img)
{
    if (img.isNull())
        return QColor();
    return img.pixelColor(img.width() / 2, img.height() / 2);
}

bool colorsNear(const QColor &a, const QColor &b, int tol = 22)
{
    return a.isValid() && b.isValid()
        && std::abs(a.red() - b.red()) <= tol
        && std::abs(a.green() - b.green()) <= tol
        && std::abs(a.blue() - b.blue()) <= tol;
}

} // namespace

int runWave25ParitySelftest()
{
    selftest::GateTally tally;
    auto rec = [&](int g, bool ok, const QString &detail) {
        tally.gate(g, ok, detail.toStdString());
    };

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        std::fprintf(stderr, "wave25-parity: temp dir failed\n");
        return 1;
    }

    // 4-second (120-frame @ 30fps) color-coded video.
    const QString movPath = tmpDir.filePath(QStringLiteral("ramp_test.mp4"));
    const bool movOk = writeIndexEncodedMp4(movPath, 160, 90, 30, 120);

    // G1 & G2 & G3: MainWindow + VideoPlayer preview vs TimelineFrameRenderer export parity.
    {
        MainWindow win;
        win.resize(960, 640);
        win.show();
        QApplication::processEvents();
        QTest::qWait(150);

        Timeline *tl = win.findChild<Timeline *>();
        VideoPlayer *vp = win.findChild<VideoPlayer *>();
        GLPreview *gl = vp ? vp->glPreview() : nullptr;

        auto grabPreview = [&]() -> QImage {
            if (!gl) return {};
            QImage raw = gl->grabFramebuffer();
            return raw.isNull() ? QImage() : raw.convertToFormat(QImage::Format_RGB888);
        };
        auto loadClip = [&](const ClipInfo &c) {
            tl->restoreFromProject({{c}}, QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
            tl->refreshPlaybackSequence();
            QTest::qWait(250);
            vp->pause();
        };
        auto seekAndGrab = [&](int ms) -> QPair<QImage, QImage> {
            bool got = false;
            const auto conn = QObject::connect(vp, &VideoPlayer::positionChanged, vp,
                [&](double p) { if (std::abs(p - ms / 1000.0) < 0.005) got = true; });
            vp->seek(ms);
            (void)QTest::qWaitFor([&] { return got; }, 10000);
            QObject::disconnect(conn);
            QTest::qWait(120);
            QImage prev = grabPreview();
            QImage exp = tlrender::renderFrameAt(tl, qint64(ms) * 1000, QSize(160, 90));
            return {prev, exp};
        };

        // G1: B2 trimmed ramp > 1 (in=0.5, out=2.0 inside 4.0s clip, 2.0x ramp).
        // Timeline length = 1.5s. At t=1.2s, ramped source = 0.5 + 2.4 = 2.9s > clipOut(2.0s).
        // Both preview and export must clamp to clipOut=2.0s (frame 60: R~180, G~120, B~40).
        {
            ClipInfo c;
            c.filePath = movPath;
            c.duration = 4.0;
            c.inPoint = 0.5;
            c.outPoint = 2.0;
            c.speed = 1.0;
            c.speedRamp.keyframes = {{0, 2.0}};
            loadClip(c);
            const auto [prev, exp] = seekAndGrab(1200);
            const QColor pc = centerPixel(prev);
            const QColor ec = centerPixel(exp);
            const bool ok = movOk && tl && vp && gl && !prev.isNull() && !exp.isNull()
                && colorsNear(pc, ec, 24);
            rec(1, ok, QStringLiteral("prev=(%1,%2,%3) exp=(%4,%5,%6)")
                           .arg(pc.red()).arg(pc.green()).arg(pc.blue())
                           .arg(ec.red()).arg(ec.green()).arg(ec.blue()));
        }

        // G2: B2 untrimmed ramp > 1 (in=0.0, out=4.0, 2.0x ramp).
        // Seek first to t=0.5s (early blue=40), then to t=3.5s (past EOF exhaustion).
        // Preview must show tail frame (blue~220), NOT stale t=0.5s frame (blue~40).
        {
            ClipInfo c;
            c.filePath = movPath;
            c.duration = 4.0;
            c.inPoint = 0.0;
            c.outPoint = 4.0;
            c.speed = 1.0;
            c.speedRamp.keyframes = {{0, 2.0}};
            loadClip(c);
            const auto [earlyPrev, earlyExp] = seekAndGrab(500);
            const auto [tailPrev, tailExp] = seekAndGrab(3500);
            const QColor earlyC = centerPixel(earlyPrev);
            const QColor tailPC = centerPixel(tailPrev);
            const QColor tailEC = centerPixel(tailExp);
            const bool ok = movOk && !tailPrev.isNull() && !tailExp.isNull()
                && earlyC.blue() < 100
                && tailPC.blue() > 160
                && colorsNear(tailPC, tailEC, 26);
            rec(2, ok, QStringLiteral("earlyB=%1 tailPrev=(%2,%3,%4) tailExp=(%5,%6,%7)")
                           .arg(earlyC.blue())
                           .arg(tailPC.red()).arg(tailPC.green()).arg(tailPC.blue())
                           .arg(tailEC.red()).arg(tailEC.green()).arg(tailEC.blue()));
        }

        // G3: CHANGE 6 time remap + speed ramp precedence: time remap wins in BOTH preview & export.
        // Clip has 2.0x speed ramp AND timeRemap holding source at 3.85s (tail blue~220) at t=0.2s.
        {
            ClipInfo c;
            c.filePath = movPath;
            c.duration = 4.0;
            c.inPoint = 0.0;
            c.outPoint = 4.0;
            c.speed = 1.0;
            c.speedRamp.keyframes = {{0, 2.0}};
            c.timeRemapCurve.keys = {{0.0, 3.85}, {4.0, 3.85}};
            loadClip(c);
            const auto [prev, exp] = seekAndGrab(200);
            const QColor pc = centerPixel(prev);
            const QColor ec = centerPixel(exp);
            const bool ok = movOk && !prev.isNull() && !exp.isNull()
                && pc.blue() > 160 && ec.blue() > 160
                && colorsNear(pc, ec, 24);
            rec(3, ok, QStringLiteral("remapPrev=(%1,%2,%3) remapExp=(%4,%5,%6)")
                           .arg(pc.red()).arg(pc.green()).arg(pc.blue())
                           .arg(ec.red()).arg(ec.green()).arg(ec.blue()));
        }

        // G6: AudioExportMix::buildMixPlan for a 2-clip sequence where clip0 has uniform speed=2.0
        // (atempo enabled) produces a resampled chain trimmed to clip0's timeline length (2.0s)
        // so clip0 cannot spill into clip1.
        {
            const QString wavPath = tmpDir.filePath(QStringLiteral("tone4s.wav"));
            const bool wavOk = writeSimpleToneWav(wavPath, 4.0);
            AudioMixer mixer;
            ClipInfo v0;
            v0.filePath = movPath;
            v0.duration = 4.0;
            v0.inPoint = 0.0;
            v0.outPoint = 4.0;

            ClipInfo a0;
            a0.filePath = wavPath;
            a0.duration = 4.0;
            a0.inPoint = 0.0;
            a0.outPoint = 4.0;
            a0.speed = 2.0; // timeline duration = 2.0s
            a0.atempoEnabled = true;
            ClipInfo a1;
            a1.filePath = wavPath;
            a1.duration = 2.0;
            a1.inPoint = 0.0;
            a1.outPoint = 2.0;
            a1.speed = 1.0; // starts at t=2.0s
            tl->restoreFromProject(QVector<QVector<ClipInfo>>{{v0}},
                                   QVector<QVector<ClipInfo>>{{a0, a1}}, 0.0, -1.0, -1.0, 100);

            const audioexport::MixPlan plan = audioexport::buildMixPlan(tl, &mixer, audioexport::MixOptions{});
            const QString ch0 = audioexport::singleChainForTest(plan, 0);
            const QString ch1 = audioexport::singleChainForTest(plan, 1);
            const bool ok = wavOk && plan.entries.size() == 2
                && ch0.contains(QStringLiteral("asetrate=96000"))
                && ch0.contains(QStringLiteral("atrim=end=2.000000"))
                && !ch1.contains(QStringLiteral("asetrate"))
                && ch1.contains(QStringLiteral("adelay=2000:all=1"));
            rec(6, ok, QStringLiteral("entries=%1 ch0Resample=%2 ch0Trim=%3 ch1Delay=%4")
                           .arg(plan.entries.size())
                           .arg(ch0.contains(QStringLiteral("asetrate=96000")) ? 1 : 0)
                           .arg(ch0.contains(QStringLiteral("atrim=end=2.000000")) ? 1 : 0)
                           .arg(ch1.contains(QStringLiteral("adelay=2000:all=1")) ? 1 : 0));
        }

        win.hide();
        QApplication::processEvents();
    }
    QApplication::processEvents();
    QThreadPool::globalInstance()->waitForDone(5000);
    QTest::qWait(50);

    // G4: B5 exportAudioRampFactor for forward uniform speed != 1.0 with atempo enabled
    // returns uniform speed (bounded to [0.1, 10]), while atempo off / speed 1.0 stay 1.0.
    {
        ClipInfo c2on;
        c2on.speed = 2.0;
        c2on.atempoEnabled = true;
        ClipInfo c2off = c2on;
        c2off.atempoEnabled = false;
        ClipInfo c1on;
        c1on.speed = 1.0;
        c1on.atempoEnabled = true;
        ClipInfo c025on;
        c025on.speed = 0.25;
        c025on.atempoEnabled = true;

        const double f2on = audioexport::exportAudioRampFactor(c2on, false, 2.0, false);
        const double f2off = audioexport::exportAudioRampFactor(c2off, false, 2.0, false);
        const double f1on = audioexport::exportAudioRampFactor(c1on, false, 1.0, false);
        const double f025 = audioexport::exportAudioRampFactor(c025on, false, 0.25, false);

        const QString c2x = buildExportAudioMixEntryFilterChain(
            0, QStringLiteral("0.000000"), QStringLiteral("4.000000"), 0,
            QStringLiteral("1.000000"), AudioChannelMode::Stereo,
            /*reversed=*/false, /*speed=*/2.0,
            TransitionType::None, 0.0, TransitionType::None, 0.0,
            /*rampFactor=*/f2on, /*timelineLength=*/2.0);
        const QString c1x = buildExportAudioMixEntryPreFxFilterChain(
            0, QStringLiteral("0.000000"), QStringLiteral("4.000000"),
            AudioChannelMode::Stereo, false, 1.0, f1on, 4.0);

        const bool ok = std::abs(f2on - 2.0) < 1e-9
            && std::abs(f2off - 1.0) < 1e-9
            && std::abs(f1on - 1.0) < 1e-9
            && std::abs(f025 - 0.25) < 1e-9
            && c2x.contains(QStringLiteral("asetrate=96000"))
            && c2x.contains(QStringLiteral("atrim=end=2.000000"))
            && !c1x.contains(QStringLiteral("asetrate"))
            && !c1x.contains(QStringLiteral("atempo"));
        rec(4, ok, QStringLiteral("f2on=%1 f2off=%2 f1on=%3 f025=%4")
                       .arg(f2on, 0, 'f', 2).arg(f2off, 0, 'f', 2)
                       .arg(f1on, 0, 'f', 2).arg(f025, 0, 'f', 2));
    }

    // G5: C-03 RampSourceMapping shifts Echo sample times inside a borrowed transition interval.
    {
        ClipInfo clip;
        clip.inPoint = 2.0;
        clip.outPoint = 5.0;
        clip.duration = 6.0;
        clip.speed = 1.0;
        clip.speedRamp.keyframes = {{0, 2.0}};
        VideoEffect echoFx;
        echoFx.type = VideoEffectType::Echo;
        echoFx.enabled = true;
        echoFx.param1 = 0.2; // delay 0.2s
        echoFx.param2 = 1.0; // 1 echo
        echoFx.param3 = 0.5; // decay
        clip.effects = {echoFx};

        // Borrowed interval starts 0.5s earlier (clipIn = 1.5s)
        OverlapInterval borrowed{};
        borrowed.timelineStart = 0.0;
        borrowed.timelineEnd = 3.5;
        borrowed.clipIn = 1.5;
        borrowed.clipOut = 5.0;
        borrowed.speed = 1.0;
        tlrender::RampSourceMapping map{&borrowed, 0.0};

        const double curLocal = 0.5;
        const double curSrc = tlrender::rampedSourceSecondInInterval(clip, &borrowed, curLocal);
        double sampledSrc = -1.0;
        QImage base(32, 32, QImage::Format_RGBA8888);
        base.fill(QColor(20, 20, 20, 255));
        const tlrender::EchoFrameProvider provider = [&](double srcSec, double) {
            sampledSrc = srcSec;
            QImage f(32, 32, QImage::Format_RGBA8888);
            f.fill(QColor(40, 40, 40, 255));
            return f;
        };
        (void)tlrender::applyClipFxStackWithEchoFromSource(
            base, clip, curLocal, curSrc, provider, &map);
        // At 2.0x ramp from clipIn=1.5: curSrc=2.5, sample at curLocal-0.2=0.3 -> 1.5+0.6=2.1
        const bool ok = std::abs(curSrc - 2.5) < 1e-4 && std::abs(sampledSrc - 2.1) < 1e-4;
        rec(5, ok, QStringLiteral("curSrc=%1 sampledSrc=%2")
                       .arg(curSrc, 0, 'f', 3).arg(sampledSrc, 0, 'f', 3));
    }

    return tally.finish("wave25-parity");
}
