#include "../FrameExport.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"

#include <cstring>

#include <QByteArray>
#include <QColor>
#include <QDebug>
#include <QImage>
#include <QTemporaryDir>

namespace {

constexpr int kClipW = 64;
constexpr int kClipH = 48;
constexpr int kFps = 12;
constexpr int kFrameCount = 18;

QImage makePatternFrame(int variant, int frameIndex)
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const int r = (x * (variant ? 9 : 5) + y * 3 + frameIndex * 7) & 255;
            const int g = (x * 2 + y * (variant ? 11 : 7) + frameIndex * 13) & 255;
            const int b = (x * y + (variant ? 41 : 17) + frameIndex * 19) & 255;
            frame.setPixelColor(x, y, QColor(r, g, b));
        }
    }
    return frame;
}

bool writeSyntheticClip(const QString &path, int variant, QString *error)
{
    libavcore::EncodeRequest req;
    req.width = kClipW;
    req.height = kClipH;
    req.fps = kFps;
    req.fpsNum = kFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 800000;
    req.outputPath = path.toStdString();
    req.videoCodecName = "mpeg4";
    req.hwVendorHint = "none";
    req.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (auto err = encoder.open(req)) {
        if (error)
            *error = QStringLiteral("FrameEncoder::open failed: ")
                + QString::fromStdString(*err);
        return false;
    }

    for (int i = 0; i < kFrameCount; ++i) {
        if (!encoder.pushFrame(makePatternFrame(variant, i), i)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrame failed at frame %1").arg(i);
            return false;
        }
    }

    if (auto err = encoder.finalize()) {
        if (error)
            *error = QStringLiteral("FrameEncoder::finalize failed: ")
                + QString::fromStdString(*err);
        return false;
    }

    return true;
}

ClipInfo makeClip(const QString &path, const QString &name, double opacity)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = name;
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = opacity;
    return clip;
}

bool equalRgbaBytes(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.size() != bb.size())
        return false;

    for (int y = 0; y < aa.height(); ++y) {
        if (std::memcmp(aa.constScanLine(y), bb.constScanLine(y),
                        static_cast<std::size_t>(aa.width() * 4)) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

int runFrameExportSelftest()
{
    int passed = 0;
    int failed = 0;

    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[frame-export] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[frame-export] FAIL G%1 %2%3")
                .arg(gate).arg(QString::fromLatin1(name))
                .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
        }
    };

    QTemporaryDir tmpDir;
    check(1, "temporary directory", tmpDir.isValid());
    if (!tmpDir.isValid())
        return 1;

    QString error;
    const QString basePath = tmpDir.filePath(QStringLiteral("frame_export_base.mp4"));
    const QString underPath = tmpDir.filePath(QStringLiteral("frame_export_under.mp4"));
    check(2, "synthetic source clips",
          writeSyntheticClip(basePath, 0, &error)
              && writeSyntheticClip(underPath, 1, &error),
          error);
    if (failed)
        return failed;

    Timeline timeline;
    if (timeline.videoTracks().isEmpty() || !timeline.videoTracks().first()) {
        check(3, "timeline V1 exists", false);
        return failed;
    }

    ClipInfo base = makeClip(basePath, QStringLiteral("frame-export-base"), 0.62);
    ClipInfo under = makeClip(underPath, QStringLiteral("frame-export-under"), 1.0);
    under.videoScale = 0.72;
    under.videoDx = 0.14;
    under.videoDy = -0.08;

    timeline.videoTracks().first()->setClips(QVector<ClipInfo>{base});
    timeline.addVideoTrack();
    TimelineTrack *underTrack = timeline.videoTracks().value(1, nullptr);
    check(4, "timeline V2 exists", underTrack != nullptr);
    if (!underTrack)
        return failed;
    underTrack->setClips(QVector<ClipInfo>{under});

    const QSize outSize(96, 54);
    const qint64 usec = 750000;
    const QImage rendered =
        tlrender::renderFrameAt(&timeline, usec, outSize).convertToFormat(QImage::Format_RGBA8888);
    check(5, "renderFrameAt composited frame",
          !rendered.isNull() && rendered.size() == outSize);
    if (failed)
        return failed;

    const QString defaultName =
        frameexport::defaultFileName(usec, kFps, frameexport::ImageFormat::Png);
    check(6, "default filename contains timecode",
          defaultName == QStringLiteral("frame_00-00-00-09.png"),
          defaultName);

    const QString pngPath = tmpDir.filePath(defaultName);
    error.clear();
    check(7, "save PNG",
          frameexport::saveFrameImage(rendered, pngPath,
                                      frameexport::ImageFormat::Png, &error),
          error);
    if (failed)
        return failed;

    const QImage rereadPng(pngPath);
    check(8, "saved PNG reread pixel-identical to SSOT renderFrameAt",
          !rereadPng.isNull() && equalRgbaBytes(rendered, rereadPng));

    const QString jpegPath = tmpDir.filePath(QStringLiteral("frame_export_jpeg"));
    error.clear();
    check(9, "save JPEG with inferred extension",
          frameexport::saveFrameImage(rendered, jpegPath,
                                      frameexport::ImageFormat::Jpeg, &error),
          error);
    const QImage rereadJpeg(tmpDir.filePath(QStringLiteral("frame_export_jpeg.jpg")));
    check(10, "JPEG rereads",
          !rereadJpeg.isNull() && rereadJpeg.size() == outSize);

    qInfo().noquote() << QStringLiteral("[frame-export] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
