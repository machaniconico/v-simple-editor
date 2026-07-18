// motion_blur_parity_selftest.cpp
// Export SSOT motion-blur off-path and averaging helper regression gates.
// Run via: --selftest=motion-blur-parity

#include "../FrameDiff.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"
#include "../playback/motionblur_flag.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include <QByteArray>
#include <QColor>
#include <QDebug>
#include <QImage>
#include <QString>
#include <QTemporaryDir>
#include <QtGlobal>
#include <QVector>

namespace {

constexpr int kClipW = 48;
constexpr int kClipH = 32;
constexpr int kFps = 12;
constexpr int kFrameCount = 12;

struct EnvGuard {
    EnvGuard()
        : hadBlur(qEnvironmentVariableIsSet("VEDITOR_MOTION_BLUR")),
          blurValue(qgetenv("VEDITOR_MOTION_BLUR")),
          hadSamples(qEnvironmentVariableIsSet("VEDITOR_MOTION_BLUR_SAMPLES")),
          samplesValue(qgetenv("VEDITOR_MOTION_BLUR_SAMPLES")),
          hadShutter(qEnvironmentVariableIsSet("VEDITOR_MOTION_BLUR_SHUTTER")),
          shutterValue(qgetenv("VEDITOR_MOTION_BLUR_SHUTTER"))
    {
    }

    ~EnvGuard()
    {
        restore("VEDITOR_MOTION_BLUR", hadBlur, blurValue);
        restore("VEDITOR_MOTION_BLUR_SAMPLES", hadSamples, samplesValue);
        restore("VEDITOR_MOTION_BLUR_SHUTTER", hadShutter, shutterValue);
    }

    static void restore(const char* name, bool had, const QByteArray& value)
    {
        if (had)
            qputenv(name, value);
        else
            qunsetenv(name);
    }

    bool hadBlur = false;
    QByteArray blurValue;
    bool hadSamples = false;
    QByteArray samplesValue;
    bool hadShutter = false;
    QByteArray shutterValue;
};

QImage makeFrame(int frameIndex)
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const int r = (x * 7 + frameIndex * 13) & 255;
            const int g = (y * 11 + frameIndex * 17) & 255;
            const int b = (x * 3 + y * 5 + frameIndex * 19) & 255;
            frame.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    return frame;
}

bool writeSyntheticClip(const QString& path, QString* error)
{
    libavcore::EncodeRequest req;
    req.width = kClipW;
    req.height = kClipH;
    req.fps = kFps;
    req.fpsNum = kFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 700000;
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

    for (int i = 0; i < kFrameCount; ++i) {
        if (!encoder.pushFrame(makeFrame(i), i)) {
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

ClipInfo makeClip(const QString& path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("motion_blur_parity");
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
}

ClipInfo makeNullClip(double duration)
{
    ClipInfo clip;
    clip.displayName = QStringLiteral("motion_blur_null_base");
    clip.duration = duration;
    clip.outPoint = duration;
    clip.motionBlurEnabled = true;
    return clip;
}

void setTimelineClip(Timeline& timeline, const ClipInfo& clip)
{
    timeline.videoTracks().first()->setClips(QVector<ClipInfo>{clip});
}

void setTwoTrackTimeline(Timeline& timeline, const ClipInfo& v1Clip, const ClipInfo& v2Clip)
{
    while (timeline.videoTracks().size() < 2)
        timeline.addVideoTrack();
    timeline.videoTracks()[0]->setClips(QVector<ClipInfo>{v1Clip});
    timeline.videoTracks()[1]->setClips(QVector<ClipInfo>{v2Clip});
}

bool bytesEqual(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull())
        return a.isNull() == b.isNull();
    if (a.size() != b.size()
        || a.format() != b.format()
        || a.bytesPerLine() != b.bytesPerLine()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<size_t>(a.bytesPerLine())) != 0) {
            return false;
        }
    }
    return true;
}

QImage makeOpaquePattern(QSize size)
{
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor((x * 31 + y * 7) & 255,
                                             (x * 5 + y * 29) & 255,
                                             (x * 13 + y * 17) & 255,
                                             255));
        }
    }
    return image;
}

} // namespace

int runMotionBlurParitySelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char* desc, bool ok, const QString& detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[motion-blur-parity] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL",
                    gate,
                    desc,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        ok ? ++passed : ++failed;
    };

    EnvGuard envGuard;
    qunsetenv("VEDITOR_MOTION_BLUR");
    qunsetenv("VEDITOR_MOTION_BLUR_SAMPLES");
    qunsetenv("VEDITOR_MOTION_BLUR_SHUTTER");

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        check(0, "temp dir", false, QStringLiteral("QTemporaryDir invalid"));
        return 1;
    }

    const QString clipPath = tempDir.filePath(QStringLiteral("motion_blur_src.mp4"));
    QString mediaError;
    if (!writeSyntheticClip(clipPath, &mediaError)) {
        check(0, "synthetic media", false, mediaError);
        return 1;
    }

    // G1: OFF/zero-duration 4-arg path returns the same bytes as the legacy
    // 3-arg path; the N=1 accumulation invariant is covered through the
    // shared helper without relying on env mutation.
    {
        Timeline timeline;
        setTimelineClip(timeline, makeClip(clipPath));
        const QSize outSize(48, 32);
        const qint64 usec = 250000;
        const QImage threeArg = tlrender::renderFrameAt(&timeline, usec, outSize);
        const QImage fourArgZero = tlrender::renderFrameAt(&timeline, usec, outSize, 0.0);
        const double mse = framediff::mse(threeArg, fourArgZero);

        const QImage single = makeOpaquePattern(QSize(8, 4));
        QVector<QImage> one;
        one.append(single);
        const QImage averagedOne = motionblur::averagePremultiplied(one);

        check(1, "OFF path and N=1 helper are byte-identical",
              mse == 0.0
                  && bytesEqual(threeArg, fourArgZero)
                  && bytesEqual(single, averagedOne),
              QStringLiteral("MSE=%1").arg(mse, 0, 'g', 12));
    }

    // G2: premultiplied accumulation produces a real blend, not either input.
    {
        QImage a(QSize(6, 6), QImage::Format_RGBA8888);
        QImage b(QSize(6, 6), QImage::Format_RGBA8888);
        a.fill(QColor(240, 20, 30, 255));
        b.fill(QColor(20, 30, 240, 255));
        QVector<QImage> samples;
        samples.append(a);
        samples.append(b);
        const QImage averaged = motionblur::averagePremultiplied(samples);
        const double mseA = framediff::mse(averaged, a);
        const double mseB = framediff::mse(averaged, b);
        check(2, "averagePremultiplied differs from both inputs",
              !averaged.isNull() && mseA > 0.0 && mseB > 0.0,
              QStringLiteral("MSE_A=%1 MSE_B=%2").arg(mseA, 0, 'g', 12).arg(mseB, 0, 'g', 12));
    }

    // G3: representative transformed frame still takes the default OFF path
    // verbatim through the 4-arg overload when frameDurationUs is zero.
    {
        ClipInfo clip = makeClip(clipPath);
        clip.videoScale = 0.85;
        clip.videoDx = 0.08;
        clip.videoDy = -0.06;
        clip.rotation2DDegrees = 6.0;
        clip.opacity = 0.72;

        Timeline timeline;
        setTimelineClip(timeline, clip);
        const QSize outSize(64, 48);
        const qint64 usec = 500000;
        const QImage threeArg = tlrender::renderFrameAt(&timeline, usec, outSize);
        const QImage fourArgZero = tlrender::renderFrameAt(&timeline, usec, outSize, 0.0);
        const double mse = framediff::mse(threeArg, fourArgZero);
        check(3, "representative OFF 3-arg and 4-arg render bytes match",
              mse == 0.0 && bytesEqual(threeArg, fourArgZero),
              QStringLiteral("MSE=%1").arg(mse, 0, 'g', 12));
    }

    // G4: shutter angle env parsing clamps to the supported export range.
    {
        qputenv("VEDITOR_MOTION_BLUR_SHUTTER", "-12.5");
        const double low = motionblur::shutterAngleFromEnv();
        qputenv("VEDITOR_MOTION_BLUR_SHUTTER", "900");
        const double high = motionblur::shutterAngleFromEnv();
        qputenv("VEDITOR_MOTION_BLUR_SHUTTER", "not-a-number");
        const double fallback = motionblur::shutterAngleFromEnv();
        qunsetenv("VEDITOR_MOTION_BLUR_SHUTTER");

        check(4, "shutter angle env clamps to [0,720]",
              low == 0.0 && high == 720.0 && fallback == 180.0,
              QStringLiteral("low=%1 high=%2 fallback=%3").arg(low).arg(high).arg(fallback));
    }

    // G5: a null temporal sample is skipped, not escalated to a failed frame.
    {
        qputenv("VEDITOR_MOTION_BLUR", "1");
        qputenv("VEDITOR_MOTION_BLUR_SAMPLES", "3");
        qputenv("VEDITOR_MOTION_BLUR_SHUTTER", "720");

        Timeline timeline;
        setTimelineClip(timeline, makeClip(clipPath));
        const QSize outSize(48, 32);
        const QImage blurred =
            tlrender::renderFrameAt(&timeline, 800000, outSize, 300000.0);

        QImage valid(QSize(1, 1), QImage::Format_RGBA8888);
        QImage mismatched(QSize(2, 1), QImage::Format_RGBA8888);
        valid.fill(QColor(4, 6, 8, 255));
        mismatched.fill(QColor(200, 210, 220, 255));
        QVector<QImage> invalidSamples;
        invalidSamples.append(valid);
        invalidSamples.append(QImage());
        invalidSamples.append(mismatched);
        const QImage averagedInvalid =
            motionblur::averagePremultiplied(invalidSamples);
        const QColor averagedInvalidPx =
            averagedInvalid.isNull() ? QColor() : averagedInvalid.pixelColor(0, 0);

        qunsetenv("VEDITOR_MOTION_BLUR");
        qunsetenv("VEDITOR_MOTION_BLUR_SAMPLES");
        qunsetenv("VEDITOR_MOTION_BLUR_SHUTTER");

        check(5, "motion blur skips null/mismatched temporal samples",
              !blurred.isNull()
                  && blurred.size() == outSize
                  && averagedInvalid.size() == valid.size()
                  && averagedInvalidPx == QColor(4, 6, 8, 255),
              QStringLiteral("null=%1 size=%2x%3")
                  .arg(blurred.isNull())
                  .arg(blurred.width())
                  .arg(blurred.height()));
    }

    // G6: channel averaging rounds to nearest instead of truncating dark.
    {
        QImage black(QSize(1, 1), QImage::Format_RGBA8888);
        QImage nearBlack(QSize(1, 1), QImage::Format_RGBA8888);
        black.fill(QColor(0, 0, 0, 255));
        nearBlack.fill(QColor(1, 1, 1, 255));
        QVector<QImage> samples;
        samples.append(black);
        samples.append(nearBlack);

        const QColor px = motionblur::averagePremultiplied(samples).pixelColor(0, 0);
        check(6, "averagePremultiplied rounds half-up",
              px.red() == 1 && px.green() == 1 && px.blue() == 1 && px.alpha() == 255,
              QStringLiteral("rgba=%1,%2,%3,%4")
                  .arg(px.red())
                  .arg(px.green())
                  .arg(px.blue())
                  .arg(px.alpha()));
    }

    // G7: shutter angle zero takes the exact single-frame path, with no
    // premultiplied conversion round-trip on semi-transparent composites.
    {
        qputenv("VEDITOR_MOTION_BLUR", "1");
        qputenv("VEDITOR_MOTION_BLUR_SAMPLES", "5");
        qputenv("VEDITOR_MOTION_BLUR_SHUTTER", "0");

        ClipInfo overlay = makeClip(clipPath);
        overlay.opacity = 0.5;

        Timeline timeline;
        setTwoTrackTimeline(timeline, makeNullClip(overlay.duration), overlay);
        const QSize outSize(48, 32);
        const qint64 usec = 250000;
        const QImage single = tlrender::renderFrameAt(&timeline, usec, outSize);
        const QImage zeroShutter =
            tlrender::renderFrameAt(&timeline, usec, outSize, 1000000.0 / kFps);

        qunsetenv("VEDITOR_MOTION_BLUR");
        qunsetenv("VEDITOR_MOTION_BLUR_SAMPLES");
        qunsetenv("VEDITOR_MOTION_BLUR_SHUTTER");

        check(7, "zero shutter is byte-identical to single-frame render",
              !single.isNull() && bytesEqual(single, zeroShutter),
              QStringLiteral("singleNull=%1 zeroNull=%2")
                  .arg(single.isNull())
                  .arg(zeroShutter.isNull()));
    }

    qInfo().noquote()
        << QStringLiteral("MOTION-BLUR-PARITY: %1/%2 PASS")
               .arg(passed)
               .arg(passed + failed);
    return failed == 0 ? 0 : 1;
}
