// Clip LUT selftest.
//
// Verifies the GAP-4 contract: ProjectFile persists per-clip LUT fields,
// renderFrameAt consumes them, and clips without LUT state stay byte-identical.

#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector>

namespace {

constexpr int kClipW = 40;
constexpr int kClipH = 24;
constexpr int kFps = 10;
constexpr int kFrameCount = 10;
constexpr double kLutIntensity = 0.75;

bool near(double a, double b)
{
    return std::abs(a - b) <= 1e-9;
}

QImage makeFrame(int frameIndex)
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            frame.setPixelColor(x, y, QColor((x * 17 + frameIndex * 23) & 255,
                                             (y * 19 + frameIndex * 29) & 255,
                                             (x * 7 + y * 11 + frameIndex * 31) & 255));
        }
    }
    return frame;
}

bool writeSyntheticClip(const QString &path, QString *error)
{
    libavcore::EncodeRequest req;
    req.width = kClipW;
    req.height = kClipH;
    req.fps = kFps;
    req.fpsNum = kFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 600000;
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
        if (!encoder.pushFrame(makeFrame(i), i)) {
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

bool writeInvertCube(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("open cube failed: %1").arg(file.errorString());
        return false;
    }

    QTextStream out(&file);
    out << "TITLE \"clip-lut-invert\"\n";
    out << "LUT_3D_SIZE 2\n";
    out << "DOMAIN_MIN 0 0 0\n";
    out << "DOMAIN_MAX 1 1 1\n";
    for (int b = 0; b < 2; ++b) {
        for (int g = 0; g < 2; ++g) {
            for (int r = 0; r < 2; ++r) {
                out << (1 - r) << ' ' << (1 - g) << ' ' << (1 - b) << '\n';
            }
        }
    }
    return true;
}

ClipInfo makeClip(const QString &path, const QString &name)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = name;
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
}

ProjectData projectWithTrack(const QVector<ClipInfo> &clips)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{clips};
    data.audioTracks = QVector<QVector<ClipInfo>>{};
    return data;
}

QJsonObject clipObjectAt(const QString &json, int index)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    const QJsonArray videoTracks = doc.object().value(QStringLiteral("videoTracks")).toArray();
    if (videoTracks.isEmpty())
        return {};
    const QJsonArray firstTrack = videoTracks.at(0).toArray();
    if (index < 0 || index >= firstTrack.size())
        return {};
    return firstTrack.at(index).toObject();
}

bool equalRgbaBytes(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.isNull() || bb.isNull())
        return aa.isNull() == bb.isNull();
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

void setTimelineClips(Timeline &timeline, const QVector<ClipInfo> &clips)
{
    timeline.videoTracks().first()->setClips(clips);
    timeline.refreshPlaybackSequence();
}

} // namespace

int runClipLutSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[clip-lut] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[clip-lut] FAIL G%1 %2%3")
                .arg(gate).arg(QString::fromLatin1(name))
                .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
        }
    };

    check(1, "QApplication is available", QApplication::instance() != nullptr);
    if (failed)
        return failed;

    QTemporaryDir tmpDir;
    check(2, "temporary directory", tmpDir.isValid());
    if (!tmpDir.isValid())
        return failed;

    QString error;
    const QString mediaPath = tmpDir.filePath(QStringLiteral("clip_lut_src.mp4"));
    check(3, "synthetic media", writeSyntheticClip(mediaPath, &error), error);
    if (failed)
        return failed;

    const QString lutPath = tmpDir.filePath(QStringLiteral("clip_lut_invert.cube"));
    check(4, "cube LUT", writeInvertCube(lutPath, &error), error);
    if (failed)
        return failed;

    ClipInfo graded = makeClip(mediaPath, QStringLiteral("graded"));
    graded.lutFilePath = lutPath;
    graded.lutIntensity = kLutIntensity;
    ClipInfo plain = makeClip(mediaPath, QStringLiteral("plain"));

    const QVector<ClipInfo> lutTrack{graded, plain};
    const QVector<ClipInfo> plainTrack{plain, plain};

    const ProjectData saveData = projectWithTrack(lutTrack);
    const QString json = ProjectFile::toJsonString(saveData);
    const QJsonObject gradedObj = clipObjectAt(json, 0);
    const QJsonObject plainObj = clipObjectAt(json, 1);
    check(5, "ProjectFile JSON writes LUT keys only on graded clip",
          gradedObj.value(QStringLiteral("lutFilePath")).toString() == lutPath
              && near(gradedObj.value(QStringLiteral("lutIntensity")).toDouble(), kLutIntensity)
              && !plainObj.contains(QStringLiteral("lutFilePath"))
              && !plainObj.contains(QStringLiteral("lutIntensity")));

    ProjectData loadedFromString;
    const bool stringLoaded = ProjectFile::fromJsonString(json, loadedFromString);
    const bool stringRoundtrip = stringLoaded
        && loadedFromString.videoTracks.size() == 1
        && loadedFromString.videoTracks.first().size() == 2
        && loadedFromString.videoTracks.first().first().lutFilePath == lutPath
        && near(loadedFromString.videoTracks.first().first().lutIntensity, kLutIntensity)
        && loadedFromString.videoTracks.first().at(1).lutFilePath.isEmpty()
        && near(loadedFromString.videoTracks.first().at(1).lutIntensity, 1.0);
    check(6, "ProjectFile string round-trip restores LUT fields", stringRoundtrip);

    const QString projectPath = tmpDir.filePath(QStringLiteral("clip_lut.veditor"));
    ProjectData loadedFromFile;
    check(7, "ProjectFile save/load round-trip",
          ProjectFile::save(projectPath, saveData)
              && ProjectFile::load(projectPath, loadedFromFile)
              && loadedFromFile.videoTracks.size() == 1
              && loadedFromFile.videoTracks.first().size() == 2
              && loadedFromFile.videoTracks.first().first().lutFilePath == lutPath
              && near(loadedFromFile.videoTracks.first().first().lutIntensity, kLutIntensity),
          projectPath);

    Timeline lutTimeline;
    Timeline plainTimeline;
    setTimelineClips(lutTimeline, lutTrack);
    setTimelineClips(plainTimeline, plainTrack);

    const QSize outSize(kClipW, kClipH);
    const QImage gradedFrame = tlrender::renderFrameAt(&lutTimeline, 200000, outSize);
    const QImage gradedBaseline = tlrender::renderFrameAt(&plainTimeline, 200000, outSize);
    check(8, "renderFrameAt reflects selected clip LUT",
          !gradedFrame.isNull()
              && !gradedBaseline.isNull()
              && !equalRgbaBytes(gradedFrame, gradedBaseline));

    const QImage untouchedFrame = tlrender::renderFrameAt(&lutTimeline, 1200000, outSize);
    const QImage untouchedBaseline = tlrender::renderFrameAt(&plainTimeline, 1200000, outSize);
    check(9, "clip without LUT remains byte-identical in same timeline",
          !untouchedFrame.isNull()
              && !untouchedBaseline.isNull()
              && equalRgbaBytes(untouchedFrame, untouchedBaseline));

    qInfo().noquote() << QStringLiteral("[clip-lut] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
