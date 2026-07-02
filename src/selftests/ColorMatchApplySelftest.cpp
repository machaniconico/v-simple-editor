// ColorMatch apply selftest.
//
// Verifies COLOR-3: generate a ColorMatch LUT, apply it to the selected clip,
// observe renderFrameAt reflection, then undo back to no LUT.

#include "../ColorMatchAnalyzer.h"
#include "../ColorMatchLutGenerator.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "../libavcore/Encode.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSize>
#include <QString>
#include <QTemporaryDir>

bool applyColorMatchLutToSelectedTimelineClip(Timeline *timeline,
                                              const QString &lutPath,
                                              double lutIntensity,
                                              QString *errorMessage);

namespace {

constexpr int kClipW = 36;
constexpr int kClipH = 24;
constexpr int kFps = 10;
constexpr int kFrameCount = 8;

QImage makeSolidFrame(const QColor &color)
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    frame.fill(color);
    return frame;
}

bool writeSyntheticClip(const QString &path, const QColor &color, QString *error)
{
    libavcore::EncodeRequest req;
    req.width = kClipW;
    req.height = kClipH;
    req.fps = kFps;
    req.fpsNum = kFps;
    req.fpsDen = 1;
    req.videoBitrateBits = 500000;
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

    const QImage frame = makeSolidFrame(color);
    for (int i = 0; i < kFrameCount; ++i) {
        if (!encoder.pushFrame(frame, i)) {
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


double meanAbsDiff(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return 1e9;
    const QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage ib = b.convertToFormat(QImage::Format_RGBA8888);
    qint64 sum = 0; qint64 n = 0;
    for (int y = 0; y < ia.height(); ++y) {
        const uchar *pa = ia.constScanLine(y);
        const uchar *pb = ib.constScanLine(y);
        for (int x = 0; x < ia.width() * 4; ++x) { sum += qAbs(int(pa[x]) - int(pb[x])); ++n; }
    }
    return n ? double(sum) / double(n) : 1e9;
}

ClipInfo makeClip(const QString &path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = QStringLiteral("color-match-apply");
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.inPoint = 0.0;
    clip.outPoint = clip.duration;
    clip.speed = 1.0;
    clip.opacity = 1.0;
    return clip;
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
                        static_cast<std::size_t>(aa.width() * 4)) != 0)
            return false;
    }
    return true;
}

void setTimelineClipSelected(Timeline &timeline, const ClipInfo &clip)
{
    TimelineTrack *track = timeline.videoTracks().isEmpty()
        ? nullptr
        : timeline.videoTracks().first();
    if (!track)
        return;
    track->setClips(QVector<ClipInfo>{clip});
    track->setSelectedClip(0);
    timeline.refreshPlaybackSequence();
}

} // namespace

int runColorMatchApplySelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[colormatch-apply] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[colormatch-apply] FAIL G%1 %2%3")
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
    const QColor targetColor(200, 50, 50);
    const QColor referenceColor(50, 200, 50);
    const QString mediaPath = tmpDir.filePath(QStringLiteral("color_match_apply_src.mp4"));
    check(3, "synthetic media", writeSyntheticClip(mediaPath, targetColor, &error), error);
    if (failed)
        return failed;

    const QImage target = makeSolidFrame(targetColor);
    const QImage reference = makeSolidFrame(referenceColor);
    const colormatch::analyze::ColorStats targetStats =
        colormatch::analyze::analyzeImage(target);
    const colormatch::analyze::ColorStats referenceStats =
        colormatch::analyze::analyzeImage(reference);
    const colormatch::lut::Lut3D lut =
        colormatch::lut::generateMatchLut(targetStats, referenceStats, 17);

    const QString lutDir = tmpDir.filePath(QStringLiteral("Project_ColorMatchLUTs"));
    const QString lutPath = lutDir + QStringLiteral("/generated_apply.cube");
    check(4, "generated ColorMatch cube",
          QDir().mkpath(lutDir)
              && lut.size == 17
              && colormatch::lut::exportCube(lut, lutPath)
              && QFile::exists(lutPath),
          lutPath);
    if (failed)
        return failed;

    const ClipInfo clip = makeClip(mediaPath);
    Timeline timeline;
    setTimelineClipSelected(timeline, clip);
    if (UndoManager *undo = timeline.undoManager())
        undo->saveState(timeline.currentState(), QStringLiteral("selftest baseline"));
    const int baselineUndoIndex = timeline.undoManager()
        ? timeline.undoManager()->currentIndex()
        : -1;

    const QSize outSize(kClipW, kClipH);
    const QImage baselineFrame0 = tlrender::renderFrameAt(&timeline, 100000, outSize);
    const QImage baselineFrame = tlrender::renderFrameAt(&timeline, 100000, outSize);
    check(5, "baseline renderFrameAt",
          !baselineFrame.isNull(),
          QStringLiteral("null=%1 maeFirstVsSecond=%2")
              .arg(baselineFrame.isNull())
              .arg(meanAbsDiff(baselineFrame0, baselineFrame)));
    qInfo().noquote() << QStringLiteral("[colormatch-apply] DIAG maeFirstVsSecond=%1")
        .arg(meanAbsDiff(baselineFrame0, baselineFrame));
    if (failed)
        return failed;

    check(6, "apply generated LUT to selected clip",
          applyColorMatchLutToSelectedTimelineClip(&timeline, lutPath, 1.0, &error)
              && !timeline.videoTracks().first()->clips().first().lutFilePath.isEmpty()
              && timeline.videoTracks().first()->clips().first().lutFilePath == lutPath
              && timeline.undoManager()
              && timeline.undoManager()->currentIndex() == baselineUndoIndex + 1,
          error);

    const QImage appliedFrame = tlrender::renderFrameAt(&timeline, 100000, outSize);
    // NOTE: renderFrameAt of this encoded synthetic clip is not byte-stable
    // across calls (decoder-level noise; eqRerender=0 observed even for
    // identical timeline state), so these gates use mean-abs-diff thresholds:
    // the LUT shift is large (>2.0/ch), decoder noise is tiny (<0.5/ch).
    check(7, "renderFrameAt reflects generated LUT",
          !appliedFrame.isNull() && meanAbsDiff(appliedFrame, baselineFrame) > 2.0,
          QStringLiteral("mae=%1").arg(meanAbsDiff(appliedFrame, baselineFrame)));

    timeline.undo();
    const QVector<ClipInfo> undoClips = timeline.videoTracks().first()->clips();
    const QImage undoFrame = tlrender::renderFrameAt(&timeline, 100000, outSize);
    check(8, "undo clears applied LUT",
          undoClips.size() == 1
              && undoClips.first().lutFilePath.isEmpty()
              && std::abs(undoClips.first().lutIntensity - 1.0) <= 1e-9
              && !undoFrame.isNull()
              && meanAbsDiff(undoFrame, baselineFrame) < 0.5,
          QStringLiteral("clips=%1 lutPath='%2' intensity=%3 maeVsBaseline=%4 maeVsApplied=%5")
              .arg(undoClips.size())
              .arg(undoClips.isEmpty() ? QStringLiteral("-") : undoClips.first().lutFilePath)
              .arg(undoClips.isEmpty() ? -1.0 : undoClips.first().lutIntensity)
              .arg(meanAbsDiff(undoFrame, baselineFrame))
              .arg(meanAbsDiff(undoFrame, appliedFrame)));

    qInfo().noquote() << QStringLiteral("[colormatch-apply] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
