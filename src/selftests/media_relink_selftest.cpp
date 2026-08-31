#include "../MediaPaths.h"
#include "../ClipGeometry.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TrackMatteKey.h"
#include "../UndoManager.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool touchFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("media", 5) == 5;
}

bool hasSlot(const QVector<mediapaths::PathSlot> &pathSlots,
             const QString &value, const QString &category)
{
    for (const mediapaths::PathSlot &slot : pathSlots) {
        if (slot.value() == value && slot.category == category)
            return true;
    }
    return false;
}

} // namespace

int runMediaRelinkSelftest()
{
    int passed = 0;
    int failed = 0;
    const auto gate = [&](bool condition, const char *name,
                          const QString &message) {
        if (condition) {
            ++passed;
            std::cerr << "[media-relink] PASS " << name << '\n';
        } else {
            ++failed;
            std::cerr << "[media-relink] FAIL " << name << ": "
                      << message.toStdString() << '\n';
        }
    };

    QTemporaryDir fixtureDir;
    const QString oldPath = QDir(fixtureDir.path()).filePath(
        QStringLiteral("missing-source.mp4"));
    const QString oldLutPath = QDir(fixtureDir.path()).filePath(
        QStringLiteral("missing-look.cube"));
    const QString newPath = QDir(fixtureDir.path()).filePath(
        QStringLiteral("source.mp4"));
    const QString newLutPath = QDir(fixtureDir.path()).filePath(
        QStringLiteral("look.cube"));
    const bool fixturesReady = fixtureDir.isValid()
        && touchFile(newPath) && touchFile(newLutPath);

    ProjectData activeData;
    ClipInfo activeClip;
    activeClip.filePath = oldPath;
    activeClip.lutFilePath = oldLutPath;
    activeData.videoTracks = ProjectTrackClips{
        QVector<ClipInfo>{activeClip}
    };
    ClipInfo audioClip;
    audioClip.filePath = oldPath;
    activeData.audioTracks = ProjectTrackClips{
        QVector<ClipInfo>{audioClip}
    };
    ParticleClipEntry particle;
    particle.clipFilePath = oldPath;
    activeData.particleClipEntries.append(particle);
    OverlayItem imageOverlay;
    imageOverlay.type = QStringLiteral("image");
    imageOverlay.text = oldPath;
    activeData.overlays.append(imageOverlay);

    const QVector<mediapaths::PathSlot> activeSlots =
        mediapaths::enumeratePathSlots(activeData);
    const bool g1 = hasSlot(activeSlots, oldPath, QStringLiteral("video.filePath"))
        && hasSlot(activeSlots, oldLutPath, QStringLiteral("video.lutFilePath"))
        && hasSlot(activeSlots, oldPath, QStringLiteral("audio.filePath"))
        && hasSlot(activeSlots, oldPath, QStringLiteral("particle.clipFilePath"))
        && hasSlot(activeSlots, oldPath, QStringLiteral("overlay.image"));
    gate(g1, "G1", QStringLiteral("active timeline path slots were incomplete"));

    ProjectData nestedData;
    Timeline nestedTimeline;
    TimelineSequence nestedSequence;
    nestedSequence.id = QStringLiteral("nested");
    nestedSequence.name = QStringLiteral("Nested");
    ClipInfo nestedClip;
    nestedClip.filePath = oldPath;
    nestedClip.lutFilePath = oldLutPath;
    nestedSequence.videoTracks = {
        QVector<ClipInfo>{nestedClip}
    };
    nestedTimeline.setSequences({nestedSequence}, nestedSequence.id);
    const QHash<QString, QString> nestedParents =
        nestedTimeline.clipParentEntries();
    for (auto it = nestedParents.cbegin(); it != nestedParents.cend(); ++it) {
        ClipParentEntry entry;
        entry.clipId = it.key();
        entry.parentClipId = it.value();
        nestedData.clipParentEntries.append(entry);
    }
    const QVector<mediapaths::PathSlot> nestedSlots =
        mediapaths::enumeratePathSlots(nestedData);
    const bool g2 = hasSlot(nestedSlots, oldPath,
                            QStringLiteral("nested.video.filePath"))
        && hasSlot(nestedSlots, oldLutPath,
                   QStringLiteral("nested.video.lutFilePath"));
    gate(g2, "G2", QStringLiteral("nested sequence file/LUT slots were not reached"));

    ProjectData missingData;
    ClipInfo missingClip;
    missingClip.filePath = newPath;
    missingClip.lutFilePath = oldLutPath;
    missingData.videoTracks = ProjectTrackClips{
        QVector<ClipInfo>{missingClip}
    };
    ClipInfo sequenceReference;
    sequenceReference.filePath = timeline_nesting::sequenceClipFilePath(
        QStringLiteral("nested"));
    ClipInfo nullObjectReference;
    nullObjectReference.filePath = clipgeom::nullObjectFilePath();
    missingData.audioTracks = ProjectTrackClips{
        QVector<ClipInfo>{sequenceReference, nullObjectReference}
    };
    const QStringList missing = mediapaths::missingFiles(missingData);
    const bool g3 = fixturesReady && missing == QStringList{oldLutPath};
    gate(g3, "G3", QStringLiteral("missingFiles did not isolate the absent local path"));

    Timeline relinkTimeline;
    TimelineTrack *videoTrack = relinkTimeline.trackAt(false, 0);
    TimelineTrack *audioTrack = relinkTimeline.trackAt(true, 0);
    ClipInfo matteSource;
    matteSource.filePath = oldPath;
    matteSource.linkGroup = 77;
    matteSource.duration = 2.0;
    matteSource.outPoint = 2.0;
    ClipInfo matteTarget = matteSource;
    matteTarget.linkGroup = 0;
    ClipInfo linkedAudio = matteSource;
    if (videoTrack)
        videoTrack->setClips({matteSource, matteTarget});
    if (audioTrack)
        audioTrack->setClips({linkedAudio});

    TimelineTrackMatteEntry matteEntry;
    matteEntry.matteType = TrackMatteType::AlphaMatte;
    matteEntry.matteSourceClipId = trackMatteClipKey(0, 0);
    relinkTimeline.setTrackMatteEntries({
        {trackMatteClipKey(0, 1), matteEntry}
    });
    relinkTimeline.setClipParentEntries({
        {trackMatteClipKey(0, 1), trackMatteClipKey(0, 0)}
    });
    relinkTimeline.undoManager()->clear();
    relinkTimeline.undoManager()->saveState(
        relinkTimeline.currentState(), QStringLiteral("media relink baseline"));

    QString relinkError;
    const bool relinked = fixturesReady
        && relinkTimeline.relinkMediaPaths({{oldPath, newPath}}, &relinkError);
    const QHash<QString, TimelineTrackMatteEntry> relinkedMattes =
        relinkTimeline.trackMatteEntries();
    const QHash<QString, QString> relinkedParents =
        relinkTimeline.clipParentEntries();
    const bool mutationPreserved = relinked && videoTrack && audioTrack
        && videoTrack->clips().size() == 2
        && audioTrack->clips().size() == 1
        && videoTrack->clips().at(0).filePath == newPath
        && videoTrack->clips().at(1).filePath == newPath
        && audioTrack->clips().at(0).filePath == newPath
        && videoTrack->clips().at(0).linkGroup == 77
        && audioTrack->clips().at(0).linkGroup == 77
        && relinkedMattes.contains(trackMatteClipKey(0, 1))
        && relinkedMattes.value(trackMatteClipKey(0, 1)).matteSourceClipId
               == trackMatteClipKey(0, 0)
        && relinkedParents.value(trackMatteClipKey(0, 1))
               == trackMatteClipKey(0, 0)
        && relinkTimeline.undoManager()->canUndo();
    if (relinked)
        relinkTimeline.undo();
    const bool undoRestored = videoTrack && audioTrack
        && videoTrack->clips().size() == 2
        && audioTrack->clips().size() == 1
        && videoTrack->clips().at(0).filePath == oldPath
        && videoTrack->clips().at(1).filePath == oldPath
        && audioTrack->clips().at(0).filePath == oldPath
        && videoTrack->clips().at(0).linkGroup == 77
        && audioTrack->clips().at(0).linkGroup == 77
        && relinkTimeline.trackMatteEntries().contains(trackMatteClipKey(0, 1))
        && relinkTimeline.clipParentEntries().value(trackMatteClipKey(0, 1))
               == trackMatteClipKey(0, 0)
        && !relinkTimeline.undoManager()->canUndo();

    Timeline internalReferenceTimeline;
    TimelineTrack *internalTrack = internalReferenceTimeline.trackAt(false, 0);
    ClipInfo nullObjectClip;
    nullObjectClip.filePath = clipgeom::nullObjectFilePath();
    ClipInfo sequenceClip;
    sequenceClip.filePath = timeline_nesting::sequenceClipFilePath(
        QStringLiteral("internal"));
    if (internalTrack)
        internalTrack->setClips({nullObjectClip, sequenceClip});
    internalReferenceTimeline.undoManager()->clear();
    internalReferenceTimeline.undoManager()->saveState(
        internalReferenceTimeline.currentState(),
        QStringLiteral("internal reference baseline"));
    QString internalReferenceError;
    const bool nullObjectRejected = !internalReferenceTimeline.relinkMediaPaths(
        {{clipgeom::nullObjectFilePath(), newPath}}, &internalReferenceError);
    const bool sequenceReferenceRejected = !internalReferenceTimeline.relinkMediaPaths(
        {{sequenceClip.filePath, newPath}}, &internalReferenceError);
    const bool internalReferencesUnchanged = internalTrack
        && internalTrack->clips().size() == 2
        && internalTrack->clips().at(0).filePath == clipgeom::nullObjectFilePath()
        && internalTrack->clips().at(1).filePath == sequenceClip.filePath
        && !internalReferenceTimeline.undoManager()->canUndo();

    gate(mutationPreserved && undoRestored && nullObjectRejected
             && sequenceReferenceRejected && internalReferencesUnchanged,
         "G4",
         relinkError.isEmpty() ? QStringLiteral("relink or one-step undo lost references")
                               : relinkError);

    ProjectData saveData;
    ClipInfo savedClip;
    savedClip.filePath = newPath;
    savedClip.lutFilePath = newLutPath;
    saveData.videoTracks = ProjectTrackClips{
        QVector<ClipInfo>{savedClip}
    };
    const QString projectPath = QDir(fixtureDir.path()).filePath(
        QStringLiteral("media-relink-roundtrip.veditor"));
    ProjectData loadedData;
    const bool roundTrip = fixturesReady
        && ProjectFile::save(projectPath, saveData)
        && ProjectFile::load(projectPath, loadedData)
        && !loadedData.videoTracks.isEmpty()
        && !loadedData.videoTracks.first().isEmpty()
        && loadedData.videoTracks.first().first().filePath == newPath
        && loadedData.videoTracks.first().first().lutFilePath == newLutPath;
    gate(roundTrip, "G5", QStringLiteral("relinked paths did not survive save/load"));

    std::cerr << "[media-relink] summary: " << passed << " PASS, "
              << failed << " FAIL\n";
    return failed;
}
