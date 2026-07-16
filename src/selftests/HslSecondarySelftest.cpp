// HSL secondary selftest.
//
// Verifies COLOR-6: per-clip HSL secondary persistence, renderFrameAt export
// reflection, and default/off byte identity.

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

bool near(double a, double b)
{
    return std::abs(a - b) <= 1e-9;
}

QImage makeFrame(int frameIndex)
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            frame.setPixelColor(x, y, QColor((180 + frameIndex * 3 + x) & 255,
                                             (40 + y) & 255,
                                             (35 + x / 2) & 255));
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

HslSecondaryGrade activeSecondary()
{
    HslSecondaryGrade hsl;
    hsl.enabled = true;
    hsl.hueCenter = 0.0;
    hsl.hueRange = 180.0;
    hsl.satMin = 0.0;
    hsl.satMax = 1.0;
    hsl.lumaMin = 0.0;
    hsl.lumaMax = 1.0;
    hsl.softness = 10.0;
    hsl.liftG = 0.25;
    return hsl;
}

bool sameHsl(const HslSecondaryGrade &a, const HslSecondaryGrade &b)
{
    return a.enabled == b.enabled
        && near(a.hueCenter, b.hueCenter)
        && near(a.hueRange, b.hueRange)
        && near(a.satMin, b.satMin)
        && near(a.satMax, b.satMax)
        && near(a.lumaMin, b.lumaMin)
        && near(a.lumaMax, b.lumaMax)
        && near(a.softness, b.softness)
        && near(a.liftR, b.liftR)
        && near(a.liftG, b.liftG)
        && near(a.liftB, b.liftB)
        && near(a.gammaR, b.gammaR)
        && near(a.gammaG, b.gammaG)
        && near(a.gammaB, b.gammaB)
        && near(a.gainR, b.gainR)
        && near(a.gainG, b.gainG)
        && near(a.gainB, b.gainB);
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

int runHslSecondarySelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[hsl-secondary] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[hsl-secondary] FAIL G%1 %2%3")
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
    const QString mediaPath = tmpDir.filePath(QStringLiteral("hsl_secondary_src.mp4"));
    check(3, "synthetic media", writeSyntheticClip(mediaPath, &error), error);
    if (failed)
        return failed;

    ClipInfo graded = makeClip(mediaPath, QStringLiteral("hsl-secondary"));
    graded.hslSecondary = activeSecondary();
    ClipInfo identity = makeClip(mediaPath, QStringLiteral("identity"));
    ClipInfo plain = makeClip(mediaPath, QStringLiteral("plain"));

    const ProjectData saveData = projectWithTrack(QVector<ClipInfo>{graded, identity, plain});
    const QString json = ProjectFile::toJsonString(saveData);
    const QJsonObject gradedObj = clipObjectAt(json, 0);
    const QJsonObject identityObj = clipObjectAt(json, 1);
    const QJsonObject plainObj = clipObjectAt(json, 2);
    check(4, "ProjectFile JSON writes only non-default HSL secondary",
          gradedObj.contains(QStringLiteral("hslSecondary"))
              && !identityObj.contains(QStringLiteral("hslSecondary"))
              && !plainObj.contains(QStringLiteral("hslSecondary")));

    ProjectData loadedFromString;
    const bool stringLoaded = ProjectFile::fromJsonString(json, loadedFromString);
    const bool stringRoundtrip = stringLoaded
        && loadedFromString.videoTracks.size() == 1
        && loadedFromString.videoTracks.first().size() == 3
        && sameHsl(loadedFromString.videoTracks.first().first().hslSecondary,
                   graded.hslSecondary)
        && loadedFromString.videoTracks.first().at(1).hslSecondary.isDefault()
        && loadedFromString.videoTracks.first().at(2).hslSecondary.isDefault();
    check(5, "ProjectFile string round-trip restores HSL secondary", stringRoundtrip);

    const QString projectPath = tmpDir.filePath(QStringLiteral("hsl_secondary.veditor"));
    ProjectData loadedFromFile;
    const bool fileRoundtrip =
        ProjectFile::save(projectPath, saveData)
        && ProjectFile::load(projectPath, loadedFromFile)
        && loadedFromFile.videoTracks.size() == 1
        && loadedFromFile.videoTracks.first().size() == 3
        && sameHsl(loadedFromFile.videoTracks.first().first().hslSecondary,
                   graded.hslSecondary);
    check(6, "ProjectFile save/load round-trip restores HSL secondary",
          fileRoundtrip, projectPath);

    Timeline gradedTimeline;
    Timeline plainTimeline;
    setTimelineClips(gradedTimeline, QVector<ClipInfo>{graded, plain});
    setTimelineClips(plainTimeline, QVector<ClipInfo>{plain, plain});

    const QSize outSize(kClipW, kClipH);
    const QImage gradedFrame = tlrender::renderFrameAt(&gradedTimeline, 200000, outSize);
    const QImage baselineFrame = tlrender::renderFrameAt(&plainTimeline, 200000, outSize);
    check(7, "renderFrameAt reflects HSL secondary",
          !gradedFrame.isNull()
              && !baselineFrame.isNull()
              && !equalRgbaBytes(gradedFrame, baselineFrame));

    const QImage untouchedFrame = tlrender::renderFrameAt(&gradedTimeline, 1200000, outSize);
    const QImage untouchedBaseline = tlrender::renderFrameAt(&plainTimeline, 1200000, outSize);
    check(8, "clip without HSL secondary remains byte-identical",
          !untouchedFrame.isNull()
              && !untouchedBaseline.isNull()
              && equalRgbaBytes(untouchedFrame, untouchedBaseline));

    const QImage directDefault =
        VideoEffectProcessor::applyHslSecondary(makeFrame(0), HslSecondaryGrade{});
    check(9, "default CPU HSL secondary is byte-identical",
          equalRgbaBytes(makeFrame(0), directDefault));

    qInfo().noquote() << QStringLiteral("[hsl-secondary] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
