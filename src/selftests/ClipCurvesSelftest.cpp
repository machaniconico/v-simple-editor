// Clip RGB/Luma curves selftest.
//
// Verifies COLOR-4: ProjectFile persists per-clip curve data, renderFrameAt
// consumes it, and unset/identity curves stay byte-identical.

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
#include <QVector>

namespace {

constexpr int kClipW = 40;
constexpr int kClipH = 24;
constexpr int kFps = 10;
constexpr int kFrameCount = 10;

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

ClipCurveData redLiftCurve()
{
    QVector<QVector<QPointF>> points = ClipCurveData::identityEditorPoints();
    points[ClipCurveData::ChannelR] = {
        QPointF(0.0, 0.0),
        QPointF(96.0, 150.0),
        QPointF(255.0, 255.0)
    };
    ClipCurveData curves;
    curves.setPoints(points);
    return curves;
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

bool sameCurveLuts(const ClipCurveData &a, const ClipCurveData &b)
{
    return a.toLuts() == b.toLuts();
}

} // namespace

int runClipCurvesSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[clip-curves] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[clip-curves] FAIL G%1 %2%3")
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
    const QString mediaPath = tmpDir.filePath(QStringLiteral("clip_curves_src.mp4"));
    check(3, "synthetic media", writeSyntheticClip(mediaPath, &error), error);
    if (failed)
        return failed;

    ClipInfo plain = makeClip(mediaPath, QStringLiteral("plain"));
    ClipInfo identity = makeClip(mediaPath, QStringLiteral("identity"));
    identity.colorCurves.setPoints(ClipCurveData::identityEditorPoints());
    ClipInfo curved = makeClip(mediaPath, QStringLiteral("curved"));
    curved.colorCurves = redLiftCurve();

    const ProjectData saveData = projectWithTrack(QVector<ClipInfo>{curved, identity, plain});
    const QString json = ProjectFile::toJsonString(saveData);
    const QJsonObject curvedObj = clipObjectAt(json, 0);
    const QJsonObject identityObj = clipObjectAt(json, 1);
    const QJsonObject plainObj = clipObjectAt(json, 2);
    check(4, "ProjectFile JSON writes only non-identity curves",
          curvedObj.contains(QStringLiteral("colorCurves"))
              && !identityObj.contains(QStringLiteral("colorCurves"))
              && !plainObj.contains(QStringLiteral("colorCurves")));

    ProjectData loadedFromString;
    const bool stringLoaded = ProjectFile::fromJsonString(json, loadedFromString);
    const bool stringRoundtrip = stringLoaded
        && loadedFromString.videoTracks.size() == 1
        && loadedFromString.videoTracks.first().size() == 3
        && loadedFromString.videoTracks.first().first().colorCurves.hasCurves()
        && sameCurveLuts(loadedFromString.videoTracks.first().first().colorCurves,
                         curved.colorCurves)
        && !loadedFromString.videoTracks.first().at(1).colorCurves.hasCurves()
        && !loadedFromString.videoTracks.first().at(2).colorCurves.hasCurves();
    check(5, "ProjectFile string round-trip restores curves", stringRoundtrip);

    const QString projectPath = tmpDir.filePath(QStringLiteral("clip_curves.veditor"));
    ProjectData loadedFromFile;
    const bool fileRoundtrip =
        ProjectFile::save(projectPath, saveData)
        && ProjectFile::load(projectPath, loadedFromFile)
        && loadedFromFile.videoTracks.size() == 1
        && loadedFromFile.videoTracks.first().size() == 3
        && sameCurveLuts(loadedFromFile.videoTracks.first().first().colorCurves,
                         curved.colorCurves);
    check(6, "ProjectFile save/load round-trip restores curves",
          fileRoundtrip, projectPath);

    Timeline plainTimeline;
    Timeline identityTimeline;
    Timeline curvedTimeline;
    setTimelineClips(plainTimeline, QVector<ClipInfo>{plain});
    setTimelineClips(identityTimeline, QVector<ClipInfo>{identity});
    setTimelineClips(curvedTimeline, QVector<ClipInfo>{curved});

    const QSize outSize(kClipW, kClipH);
    const QImage plainFrame = tlrender::renderFrameAt(&plainTimeline, 200000, outSize);
    const QImage identityFrame = tlrender::renderFrameAt(&identityTimeline, 200000, outSize);
    const QImage curvedFrame = tlrender::renderFrameAt(&curvedTimeline, 200000, outSize);

    check(7, "identity curves render byte-identical",
          !plainFrame.isNull()
              && !identityFrame.isNull()
              && equalRgbaBytes(plainFrame, identityFrame));
    check(8, "non-identity curves affect renderFrameAt",
          !plainFrame.isNull()
              && !curvedFrame.isNull()
              && !equalRgbaBytes(plainFrame, curvedFrame));

    qInfo().noquote() << QStringLiteral("[clip-curves] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
