#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"

#include <cstddef>
#include <cstring>

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTemporaryDir>

namespace {

constexpr int kClipW = 48;
constexpr int kClipH = 32;
constexpr int kFps = 12;
constexpr int kFrameCount = 12;

QImage makePatternFrame(int frameIndex)
{
    QImage frame(kClipW, kClipH, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            frame.setPixelColor(x, y,
                                QColor((x * 11 + frameIndex * 17) & 255,
                                       (y * 13 + frameIndex * 19) & 255,
                                       (x * 5 + y * 7 + frameIndex * 23) & 255));
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
    req.videoBitrateBits = 700000;
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
        if (!encoder.pushFrame(makePatternFrame(i), i)) {
            if (error)
                *error = QStringLiteral("FrameEncoder::pushFrame failed at %1").arg(i);
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

ClipInfo makeMediaClip(const QString &path, const QString &name)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = name;
    clip.duration = static_cast<double>(kFrameCount) / kFps;
    clip.outPoint = clip.duration;
    clip.opacity = 1.0;
    return clip;
}

ClipInfo makeSequenceRef(const QString &id, double duration)
{
    ClipInfo clip;
    clip.sequenceRefId = id;
    clip.filePath = timeline_nesting::sequenceClipFilePath(id);
    clip.displayName = id;
    clip.duration = duration;
    clip.outPoint = duration;
    clip.opacity = 1.0;
    return clip;
}

TimelineSequence makeSequence(const QString &id,
                              const QVector<QVector<ClipInfo>> &videoTracks,
                              const QVector<QVector<ClipInfo>> &audioTracks =
                                  QVector<QVector<ClipInfo>>())
{
    TimelineSequence sequence;
    sequence.id = id;
    sequence.name = id;
    sequence.videoTracks = videoTracks;
    sequence.audioTracks = audioTracks;
    return sequence;
}

QVector<QVector<ClipInfo>> oneTrack(const ClipInfo &clip)
{
    return QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
}

bool equalRgbaBytes(const QImage &a, const QImage &b)
{
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    if (aa.size() != bb.size())
        return false;
    for (int y = 0; y < aa.height(); ++y) {
        if (std::memcmp(aa.constScanLine(y), bb.constScanLine(y),
                        static_cast<std::size_t>(aa.width() * 4)) != 0)
            return false;
    }
    return true;
}

bool isTransparent(const QImage &image)
{
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull())
        return false;
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *row = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            if (row[x * 4 + 3] != 0)
                return false;
        }
    }
    return true;
}

bool containsSequenceId(const QJsonObject &store, const QString &id)
{
    const QJsonArray arr = store.value(QStringLiteral("sequences")).toArray();
    for (const QJsonValue &value : arr) {
        if (value.toObject().value(QStringLiteral("id")).toString() == id)
            return true;
    }
    return false;
}

} // namespace

int runNestSequenceSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[nest-sequence] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[nest-sequence] FAIL G%1 %2%3")
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
    const QString mediaPath = tmpDir.filePath(QStringLiteral("nest_sequence_src.mp4"));
    check(3, "synthetic media", writeSyntheticClip(mediaPath, &error), error);
    if (failed)
        return failed;

    const ClipInfo media = makeMediaClip(mediaPath, QStringLiteral("media"));
    const double duration = media.duration;
    const TimelineSequence leaf =
        makeSequence(QStringLiteral("leaf"), oneTrack(media), oneTrack(media));
    const TimelineSequence mid =
        makeSequence(QStringLiteral("mid"),
                     oneTrack(makeSequenceRef(QStringLiteral("leaf"), duration)),
                     oneTrack(makeSequenceRef(QStringLiteral("leaf"), duration)));
    const TimelineSequence main =
        makeSequence(QStringLiteral("main"),
                     oneTrack(makeSequenceRef(QStringLiteral("mid"), duration)),
                     oneTrack(makeSequenceRef(QStringLiteral("mid"), duration)));

    Timeline nestedTimeline;
    nestedTimeline.setSequences(QVector<TimelineSequence>{main, mid, leaf},
                                QStringLiteral("main"));

    Timeline directTimeline;
    directTimeline.videoTracks().first()->setClips(QVector<ClipInfo>{media});

    const QSize outSize(kClipW, kClipH);
    const qint64 usec = 250000;
    const QImage nested =
        tlrender::renderFrameAt(&nestedTimeline, usec, outSize);
    const QImage direct =
        tlrender::renderFrameAt(&directTimeline, usec, outSize);
    check(4, "recursive nested render matches direct leaf media",
          !nested.isNull() && equalRgbaBytes(nested, direct));

    const QVector<PlaybackEntry> videoEntries =
        nestedTimeline.computePlaybackSequence();
    check(5, "recursive video flatten resolves to media",
          videoEntries.size() == 1
              && videoEntries.first().filePath == mediaPath
              && !timeline_nesting::isSequenceClipFilePath(videoEntries.first().filePath));

    const QVector<PlaybackEntry> audioEntries =
        nestedTimeline.computeAudioPlaybackSequence();
    check(6, "recursive audio flatten resolves to media",
          audioEntries.size() == 1 && audioEntries.first().filePath == mediaPath);

    const QHash<QString, QString> carriers = nestedTimeline.clipParentEntries();
    const QJsonObject store = timeline_nesting::decodeSequenceStoreObject(
        carriers.value(timeline_nesting::sequenceStoreParentKey()));
    check(7, "sequence store roundtrip payload",
          store.value(QStringLiteral("activeSequenceId")).toString() == QStringLiteral("main")
              && containsSequenceId(store, QStringLiteral("main"))
              && containsSequenceId(store, QStringLiteral("mid"))
              && containsSequenceId(store, QStringLiteral("leaf")));

    Timeline addClipTimeline;
    check(8, "addSequence succeeds",
          addClipTimeline.addSequence(leaf)
              && addClipTimeline.addSequenceClip(QStringLiteral("leaf"), 0));
    check(9, "addSequenceClip creates audio reference",
          !addClipTimeline.audioTracks().isEmpty()
              && addClipTimeline.audioTracks().first()
              && addClipTimeline.audioTracks().first()->clipCount() == 1
              && addClipTimeline.audioTracks().first()->clips().first().isSequenceReference());

    Timeline cycleTimeline;
    const TimelineSequence cycleA =
        makeSequence(QStringLiteral("cycle-a"),
                     oneTrack(makeSequenceRef(QStringLiteral("cycle-b"), duration)));
    const TimelineSequence cycleB =
        makeSequence(QStringLiteral("cycle-b"),
                     oneTrack(makeSequenceRef(QStringLiteral("cycle-a"), duration)));
    const TimelineSequence cycleMain =
        makeSequence(QStringLiteral("cycle-main"),
                     oneTrack(makeSequenceRef(QStringLiteral("cycle-a"), duration)));
    cycleTimeline.setSequences(QVector<TimelineSequence>{cycleMain, cycleA, cycleB},
                               QStringLiteral("cycle-main"));
    const QImage cycleFrame =
        tlrender::renderFrameAt(&cycleTimeline, usec, outSize);
    check(10, "cycle guard returns transparent fallback",
          !cycleFrame.isNull() && isTransparent(cycleFrame));

    QVector<TimelineSequence> depthSequences;
    depthSequences.append(makeSequence(QStringLiteral("depth-leaf"), oneTrack(media)));
    for (int i = 9; i >= 0; --i) {
        const QString id = QStringLiteral("depth-%1").arg(i);
        const QString next = (i == 9)
            ? QStringLiteral("depth-leaf")
            : QStringLiteral("depth-%1").arg(i + 1);
        depthSequences.append(
            makeSequence(id, oneTrack(makeSequenceRef(next, duration))));
    }
    depthSequences.append(
        makeSequence(QStringLiteral("depth-main"),
                     oneTrack(makeSequenceRef(QStringLiteral("depth-0"), duration))));
    Timeline depthTimeline;
    depthTimeline.setSequences(depthSequences, QStringLiteral("depth-main"));
    const QImage depthFrame =
        tlrender::renderFrameAt(&depthTimeline, usec, outSize);
    check(11, "depth guard returns transparent fallback",
          !depthFrame.isNull() && isTransparent(depthFrame));

    const QImage decoded =
        tlrender::detail::decodeClipFrameNativeForTest(mediaPath, 0.25)
            .scaled(outSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    check(12, "legacy no-nest render remains decode-byte-identical",
          !direct.isNull() && equalRgbaBytes(direct, decoded));

    qInfo().noquote() << QStringLiteral("[nest-sequence] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
