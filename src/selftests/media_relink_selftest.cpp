#include "../MediaPaths.h"
#include "../ClipGeometry.h"
#include "../ProjectCollector.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TrackMatteKey.h"
#include "../UndoManager.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryDir>
#include <QTimer>

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

bool hasCollectedSlot(const QVector<mediapaths::PathSlot> &pathSlots,
                      const QString &category, const QString &mediaDir)
{
    const QString expectedDir = QDir(mediaDir).absolutePath();
    for (const mediapaths::PathSlot &slot : pathSlots) {
        const QString value = slot.value();
        if (slot.category == category && QFileInfo(value).absolutePath() == expectedDir
            && QFileInfo::exists(value)) {
            return true;
        }
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
    activeClip.duration = 1.0;
    activeClip.outPoint = 1.0;
    activeData.videoTracks = ProjectTrackClips{
        QVector<ClipInfo>{activeClip}
    };
    ClipInfo audioClip;
    audioClip.filePath = oldPath;
    audioClip.lutFilePath = oldLutPath;
    audioClip.duration = 1.0;
    audioClip.outPoint = 1.0;
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
        && hasSlot(activeSlots, oldLutPath, QStringLiteral("audio.lutFilePath"))
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
    nestedClip.duration = 1.0;
    nestedClip.outPoint = 1.0;
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

    Timeline persistenceTimeline;
    TimelineSequence activeSequence;
    activeSequence.id = QStringLiteral("active");
    activeSequence.name = QStringLiteral("Active");
    activeSequence.videoTracks = activeData.videoTracks;
    activeSequence.audioTracks = activeData.audioTracks;
    persistenceTimeline.setSequences(
        {activeSequence, nestedSequence}, activeSequence.id);
    persistenceTimeline.undoManager()->clear();
    persistenceTimeline.undoManager()->saveState(
        persistenceTimeline.currentState(),
        QStringLiteral("persistence baseline"));

    QVector<ParticleClipEntry> persistedParticles = activeData.particleClipEntries;
    QVector<OverlayItem> persistedOverlays = activeData.overlays;
    const auto relinkSidecars = [&persistedParticles, &persistedOverlays](
                                    const QHash<QString, QString> &mapping) {
        bool changed = false;
        for (ParticleClipEntry &entry : persistedParticles) {
            const auto replacement = mapping.constFind(entry.clipFilePath);
            if (replacement == mapping.cend()
                || replacement.value() == entry.clipFilePath) {
                continue;
            }
            entry.clipFilePath = replacement.value();
            changed = true;
        }
        for (OverlayItem &overlay : persistedOverlays) {
            if (overlay.type != QLatin1String("image"))
                continue;
            const auto replacement = mapping.constFind(overlay.text);
            if (replacement == mapping.cend()
                || replacement.value() == overlay.text) {
                continue;
            }
            overlay.text = replacement.value();
            changed = true;
        }
        return changed;
    };

    QString persistenceError;
    const bool persistenceRelinked = fixturesReady
        && persistenceTimeline.relinkMediaPaths(
            {{oldPath, newPath}, {oldLutPath, newLutPath}},
            &persistenceError, relinkSidecars);

    ProjectData saveData;
    if (persistenceRelinked) {
        saveData.videoTracks = persistenceTimeline.allVideoTracks();
        saveData.audioTracks = persistenceTimeline.allAudioTracks();
        saveData.particleClipEntries = persistedParticles;
        saveData.overlays = persistedOverlays;
        const QHash<QString, QString> parentEntries =
            persistenceTimeline.clipParentEntries();
        for (auto it = parentEntries.cbegin(); it != parentEntries.cend(); ++it) {
            ClipParentEntry entry;
            entry.clipId = it.key();
            entry.parentClipId = it.value();
            saveData.clipParentEntries.append(entry);
        }
    }
    const QString projectPath = QDir(fixtureDir.path()).filePath(
        QStringLiteral("media-relink-roundtrip.veditor"));
    ProjectData loadedData;
    QVector<mediapaths::PathSlot> loadedSlots;
    const bool savedAndLoaded = persistenceRelinked
        && ProjectFile::save(projectPath, saveData)
        && ProjectFile::load(projectPath, loadedData);
    if (savedAndLoaded)
        loadedSlots = mediapaths::enumeratePathSlots(loadedData);
    const bool roundTrip = savedAndLoaded
        && hasSlot(loadedSlots, newPath, QStringLiteral("video.filePath"))
        && hasSlot(loadedSlots, newLutPath, QStringLiteral("video.lutFilePath"))
        && hasSlot(loadedSlots, newPath, QStringLiteral("audio.filePath"))
        && hasSlot(loadedSlots, newLutPath, QStringLiteral("audio.lutFilePath"))
        && hasSlot(loadedSlots, newPath, QStringLiteral("particle.clipFilePath"))
        && hasSlot(loadedSlots, newPath, QStringLiteral("overlay.image"))
        && hasSlot(loadedSlots, newPath, QStringLiteral("nested.video.filePath"))
        && hasSlot(loadedSlots, newLutPath,
                   QStringLiteral("nested.video.lutFilePath"));
    gate(roundTrip, "G5",
         persistenceError.isEmpty()
             ? QStringLiteral("relinked paths did not survive save/load")
             : persistenceError);

    const QString collectorSourceA = QDir(fixtureDir.path()).filePath(
        QStringLiteral("collector-source-a"));
    const QString collectorSourceB = QDir(fixtureDir.path()).filePath(
        QStringLiteral("collector-source-b"));
    const bool collectorDirsReady = QDir().mkpath(collectorSourceA)
        && QDir().mkpath(collectorSourceB);
    const QString collectVideoPath = QDir(collectorSourceA).filePath(
        QStringLiteral("shared.mov"));
    const QString collectLutPath = QDir(collectorSourceA).filePath(
        QStringLiteral("top.cube"));
    const QString collectAudioPath = QDir(collectorSourceB).filePath(
        QStringLiteral("shared.mov"));
    const QString collectParticlePath = QDir(collectorSourceA).filePath(
        QStringLiteral("particle.bin"));
    const QString collectOverlayPath = QDir(collectorSourceA).filePath(
        QStringLiteral("overlay.png"));
    const QString collectNestedVideoPath = QDir(collectorSourceA).filePath(
        QStringLiteral("nested.mov"));
    const QString collectNestedLutPath = QDir(collectorSourceA).filePath(
        QStringLiteral("nested.cube"));
    const QStringList collectorSources{
        collectVideoPath, collectLutPath, collectAudioPath,
        collectParticlePath, collectOverlayPath,
        collectNestedVideoPath, collectNestedLutPath
    };
    bool collectorFilesReady = collectorDirsReady;
    for (const QString &path : collectorSources)
        collectorFilesReady = touchFile(path) && collectorFilesReady;

    ProjectData collectData;
    ClipInfo collectVideo;
    collectVideo.filePath = collectVideoPath;
    collectVideo.lutFilePath = collectLutPath;
    collectVideo.duration = 1.0;
    collectVideo.outPoint = 1.0;
    collectData.videoTracks = ProjectTrackClips{
        QVector<ClipInfo>{collectVideo}
    };
    ClipInfo collectAudio;
    collectAudio.filePath = collectAudioPath;
    collectAudio.duration = 1.0;
    collectAudio.outPoint = 1.0;
    ClipInfo collectNullObject;
    collectNullObject.filePath = clipgeom::nullObjectFilePath();
    collectNullObject.duration = 1.0;
    collectNullObject.outPoint = 1.0;
    ClipInfo collectSequenceReference;
    collectSequenceReference.filePath = timeline_nesting::sequenceClipFilePath(
        QStringLiteral("collector-internal"));
    collectSequenceReference.duration = 1.0;
    collectSequenceReference.outPoint = 1.0;
    collectData.audioTracks = ProjectTrackClips{
        QVector<ClipInfo>{collectAudio, collectNullObject, collectSequenceReference}
    };
    ParticleClipEntry collectParticle;
    collectParticle.clipFilePath = collectParticlePath;
    collectData.particleClipEntries.append(collectParticle);
    collectData.particleClipEntries.append(collectParticle);
    OverlayItem collectOverlay;
    collectOverlay.type = QStringLiteral("image");
    collectOverlay.text = collectOverlayPath;
    collectData.overlays.append(collectOverlay);

    Timeline collectorNestedTimeline;
    TimelineSequence collectorNestedSequence;
    collectorNestedSequence.id = QStringLiteral("collector-nested");
    collectorNestedSequence.name = QStringLiteral("Collector Nested");
    ClipInfo collectorNestedClip;
    collectorNestedClip.filePath = collectNestedVideoPath;
    collectorNestedClip.lutFilePath = collectNestedLutPath;
    collectorNestedClip.duration = 1.0;
    collectorNestedClip.outPoint = 1.0;
    collectorNestedSequence.videoTracks = {
        QVector<ClipInfo>{collectorNestedClip}
    };
    collectorNestedTimeline.setSequences(
        {collectorNestedSequence}, collectorNestedSequence.id);
    const QHash<QString, QString> collectorNestedParents =
        collectorNestedTimeline.clipParentEntries();
    for (auto it = collectorNestedParents.cbegin();
         it != collectorNestedParents.cend(); ++it) {
        ClipParentEntry entry;
        entry.clipId = it.key();
        entry.parentClipId = it.value();
        collectData.clipParentEntries.append(entry);
    }

    const QString collectDestDir = QDir(fixtureDir.path()).filePath(
        QStringLiteral("collected-project"));
    const QString collectedProjectName = QStringLiteral("collected.veditor");
    ProjectCollector collector;
    QEventLoop collectLoop;
    bool collectorFinished = false;
    bool collectorOk = false;
    QString collectorMessage;
    QVector<int> collectorProgress;
    QObject::connect(&collector, &ProjectCollector::progressChanged,
                     &collectLoop, [&](int percent) {
        collectorProgress.append(percent);
    });
    QObject::connect(&collector, &ProjectCollector::finished,
                     &collectLoop, [&](bool ok, const QString &message) {
        collectorFinished = true;
        collectorOk = ok;
        collectorMessage = message;
        collectLoop.quit();
    });
    if (collectorFilesReady) {
        collector.collect(collectData, collectDestDir, collectedProjectName);
        QTimer::singleShot(10000, &collectLoop, &QEventLoop::quit);
        if (!collectorFinished)
            collectLoop.exec();
    }

    ProjectData collectedData;
    QVector<mediapaths::PathSlot> collectedSlots;
    const QString collectedProjectPath = QDir(collectDestDir).filePath(
        collectedProjectName);
    const bool collectedProjectLoaded = collectorFinished && collectorOk
        && ProjectFile::load(collectedProjectPath, collectedData);
    if (collectedProjectLoaded)
        collectedSlots = mediapaths::enumeratePathSlots(collectedData);
    const QString collectedMediaDir = QDir(collectDestDir).filePath(
        QStringLiteral("media"));
    const bool allCategoriesCollected = collectedProjectLoaded
        && hasCollectedSlot(collectedSlots, QStringLiteral("video.filePath"),
                            collectedMediaDir)
        && hasCollectedSlot(collectedSlots, QStringLiteral("video.lutFilePath"),
                            collectedMediaDir)
        && hasCollectedSlot(collectedSlots, QStringLiteral("audio.filePath"),
                            collectedMediaDir)
        && hasCollectedSlot(collectedSlots, QStringLiteral("particle.clipFilePath"),
                            collectedMediaDir)
        && hasCollectedSlot(collectedSlots, QStringLiteral("overlay.image"),
                            collectedMediaDir)
        && hasCollectedSlot(collectedSlots, QStringLiteral("nested.video.filePath"),
                            collectedMediaDir)
        && hasCollectedSlot(collectedSlots, QStringLiteral("nested.video.lutFilePath"),
                            collectedMediaDir);
    const bool internalReferencesSkipped = collectedProjectLoaded
        && hasSlot(collectedSlots, collectNullObject.filePath,
                   QStringLiteral("audio.filePath"))
        && hasSlot(collectedSlots, collectSequenceReference.filePath,
                   QStringLiteral("audio.filePath"))
        && collector.warnings().isEmpty();
    const bool deduplicatedAndRenamed = collectedProjectLoaded
        && collector.bytesCopied() == collectorSources.size() * 5
        && QFile::exists(QDir(collectedMediaDir).filePath(QStringLiteral("shared.mov")))
        && QFile::exists(QDir(collectedMediaDir).filePath(QStringLiteral("shared_2.mov")))
        && collectorProgress.contains(100);
    gate(collectorFilesReady && allCategoriesCollected
             && internalReferencesSkipped && deduplicatedAndRenamed,
         "G6",
         !collectorFinished
             ? QStringLiteral("ProjectCollector did not finish")
             : (!collectorOk
                    ? collectorMessage
                    : QStringLiteral("collected project paths were incomplete")));

    std::cerr << "[media-relink] summary: " << passed << " PASS, "
              << failed << " FAIL\n";
    return failed;
}
