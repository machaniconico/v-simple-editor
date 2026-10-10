// --selftest=matte-ssot: Timeline matte carrier synchronization, remap and undo.
#include "SelftestGate.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../TrackMatteKey.h"
#include "../libavcore/Encode.h"

#include <QFile>
#include <QFuture>
#include <QImage>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QWaitCondition>
#include <QtConcurrent>

#include <iostream>
#include <string>

namespace {

using Entries = QHash<QString, TimelineTrackMatteEntry>;

bool containsOne(const Entries &entries, const QString &key, const QString &source,
                 TrackMatteType type = TrackMatteType::AlphaMatte)
{
    const auto it = entries.constFind(key);
    return entries.size() == 1 && it != entries.cend()
        && it->matteType == type && it->matteSourceClipId == source;
}

Entries matteAt(const QString &key, const QString &source)
{
    TimelineTrackMatteEntry entry;
    entry.matteType = TrackMatteType::AlphaMatte;
    entry.matteSourceClipId = source;
    return {{key, entry}};
}

bool writeSyntheticClip(const QString &path)
{
    libavcore::EncodeRequest request;
    request.width = 32;
    request.height = 32;
    request.fps = 10;
    request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoBitrateBits = 300000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(32, 32, QImage::Format_RGB888);
    frame.fill(QColor(64, 96, 128));
    for (int i = 0; i < 20; ++i)
        if (!encoder.pushFrame(frame, i)) return false;
    return !encoder.finalize().has_value() && QFile::exists(path);
}

ClipInfo clip(const QString &path, int id)
{
    ClipInfo c;
    c.filePath = path;
    c.displayName = QStringLiteral("matte-%1").arg(id);
    c.linkGroup = id;
    c.duration = 2.0;
    c.inPoint = 0.0;
    c.outPoint = 2.0;
    return c;
}

void seed(Timeline &tl, const QString &path)
{
    const QVector<QVector<ClipInfo>> video{
        {clip(path, 10)}, {clip(path, 1), clip(path, 2), clip(path, 3)}};
    const QVector<QVector<ClipInfo>> audio{{}};
    tl.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    tl.setTrackMatteEntries(matteAt(trackMatteClipKey(1, 1), trackMatteClipKey(1, 2)));
    RotoClipEntry roto;
    tl.setRotoEntries({{trackMatteClipKey(1, 1), roto}});
    tl.setClipParentEntries({{trackMatteClipKey(1, 1), trackMatteClipKey(0, 0)}});
}

bool remapped(const Timeline &tl, const QString &owner, const QString &source)
{
    return containsOne(tl.trackMatteEntries(), owner, source)
        && tl.rotoEntries().size() == 1 && tl.rotoEntries().contains(owner)
        && tl.clipParentEntries().value(owner) == trackMatteClipKey(0, 0);
}

std::string text(const QString &value)
{
    return value.toStdString();
}

} // namespace

int runMatteSsotSelftest()
{
    selftest::GateTally tally;

    // G1 catches a torn/invalid getter snapshot under concurrent publication.
    {
        Timeline tl;
        const Entries a = matteAt(QStringLiteral("1:1"), QStringLiteral("1:2"));
        const Entries b = matteAt(QStringLiteral("1:1"), QStringLiteral("0:0"));
        tl.setTrackMatteEntries(a);
        QThreadPool pool;
        pool.setMaxThreadCount(2);
        QMutex startMutex;
        QWaitCondition startCondition;
        int ready = 0;
        const auto waitForBothWorkers = [&] {
            QMutexLocker lock(&startMutex);
            if (++ready == 2) startCondition.wakeAll();
            else while (ready < 2) startCondition.wait(&startMutex);
        };
        int setterCount = 0;
        int getterCount = 0;
        int inconsistent = 0;
        auto writer = QtConcurrent::run(&pool, [&] {
            waitForBothWorkers();
            for (int i = 0; i < 100; ++i) {
                tl.setTrackMatteEntries(i % 2 ? a : b);
                ++setterCount;
            }
        });
        auto reader = QtConcurrent::run(&pool, [&] {
            waitForBothWorkers();
            for (int i = 0; i < 1000; ++i) {
                const Entries seen = tl.trackMatteEntries();
                if (!containsOne(seen, QStringLiteral("1:1"), QStringLiteral("1:2"))
                    && !containsOne(seen, QStringLiteral("1:1"), QStringLiteral("0:0")))
                    ++inconsistent;
                ++getterCount;
            }
        });
        writer.waitForFinished();
        reader.waitForFinished();
        const bool roundTrip = containsOne(tl.trackMatteEntries(),
                                           QStringLiteral("1:1"), QStringLiteral("1:2"));
        const bool invalidRejected = !containsOne(
            matteAt(QStringLiteral("1:1"), QStringLiteral("9:9")),
            QStringLiteral("1:1"), QStringLiteral("1:2"));
        tally.gate(1, setterCount == 100 && getterCount == 1000
                   && inconsistent == 0 && roundTrip && invalidRejected,
                   "setter=" + std::to_string(setterCount)
                   + " getter=" + std::to_string(getterCount)
                   + " inconsistent=" + std::to_string(inconsistent)
                   + " roundTrip=" + std::to_string(roundTrip)
                   + " invalidRejected=" + std::to_string(invalidRejected));
    }

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("matte-ssot.mp4"));
    const bool mediaReady = dir.isValid() && writeSyntheticClip(path);

    // G2 catches owner/source key drift after an edit before the matte clip.
    {
        bool deleted = false, split = false, external = false;
        QString deleteError, splitError;
        if (mediaReady) {
            Timeline tl;
            seed(tl, path);
            deleted = tl.deleteClipByIndex(false, 1, 0, false, &deleteError)
                && remapped(tl, trackMatteClipKey(1, 0), trackMatteClipKey(1, 1));
        }
        if (mediaReady) {
            Timeline tl;
            seed(tl, path);
            split = tl.splitClipByIndex(false, 1, 0, 1.0, &splitError)
                && remapped(tl, trackMatteClipKey(1, 2), trackMatteClipKey(1, 3));
        }
        if (mediaReady) {
            Timeline tl;
            seed(tl, path);
            const TrackClipSnapshot before = snapshotTrackClips(&tl);
            auto clips = tl.videoTracks().at(1)->clips();
            clips.removeAt(0);
            tl.videoTracks().at(1)->setClips(clips);
            tl.remapCarriersAfterExternalMutation(before);
            external = remapped(tl, trackMatteClipKey(1, 0), trackMatteClipKey(1, 1));
        }
        const std::string detail = "mediaReady=" + std::to_string(mediaReady)
            + " delete 1:1->1:0 source 1:2->1:1=" + std::to_string(deleted)
            + " split 1:1->1:2 source 1:2->1:3=" + std::to_string(split)
            + " external 1:1->1:0 source 1:2->1:1=" + std::to_string(external)
            + " deleteError=" + text(deleteError) + " splitError=" + text(splitError);
        tally.gate(2, mediaReady && deleted && split && external, detail);
    }

    // G3 catches missing TimelineState matte snapshots during undo and redo.
    {
        bool initial = false, removed = false, restored = false, redone = false;
        QString error;
        if (mediaReady) {
            Timeline tl;
            seed(tl, path);
            tl.saveUndoState(QStringLiteral("マットを設定"));
            initial = containsOne(tl.currentState().trackMatteEntries,
                                  trackMatteClipKey(1, 1), trackMatteClipKey(1, 2));
            removed = tl.deleteClipByIndex(false, 1, 1, false, &error)
                && tl.trackMatteEntries().isEmpty();
            tl.undo();
            restored = containsOne(tl.trackMatteEntries(),
                                   trackMatteClipKey(1, 1), trackMatteClipKey(1, 2));
            tl.redo();
            redone = tl.trackMatteEntries().isEmpty();
        }
        tally.gate(3, mediaReady && initial && removed && restored && redone,
                   "initial=" + std::to_string(initial)
                   + " removed=" + std::to_string(removed)
                   + " undo=" + std::to_string(restored)
                   + " redo=" + std::to_string(redone)
                   + " error=" + text(error));
    }
    return tally.finish("matte-ssot");
}
