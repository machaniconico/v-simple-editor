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

void setTimelineClip(Timeline& timeline, const ClipInfo& clip)
{
    timeline.videoTracks().first()->setClips(QVector<ClipInfo>{clip});
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

    qInfo().noquote()
        << QStringLiteral("MOTION-BLUR-PARITY: %1/%2 PASS")
               .arg(passed)
               .arg(passed + failed);
    return failed == 0 ? 0 : 1;
}
