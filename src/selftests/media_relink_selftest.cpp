#include "../MainWindow.h"
#include "../MediaPaths.h"
#include "../MediaRelinkDialog.h"
#include "../ClipGeometry.h"
#include "../ProjectCollector.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TrackMatteKey.h"
#include "../UndoManager.h"
#include "SelftestRegistry.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStatusBar>
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

QString pathSlotKey(const QString &sourcePath, const QString &category)
{
    return category + QLatin1Char('\x1f') + QDir::cleanPath(sourcePath);
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

    QVector<QString> collectOriginalSources;
    QVector<QString> collectOriginalCategories;
    const QVector<mediapaths::PathSlot> collectOriginalSlots =
        mediapaths::enumeratePathSlots(collectData);
    collectOriginalSources.reserve(collectOriginalSlots.size());
    collectOriginalCategories.reserve(collectOriginalSlots.size());
    for (const mediapaths::PathSlot &slot : collectOriginalSlots) {
        collectOriginalSources.append(slot.value());
        collectOriginalCategories.append(slot.category);
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
    const QString expectedVideoPath = QDir(collectedMediaDir).absoluteFilePath(
        QStringLiteral("shared.mov"));
    const QString expectedAudioPath = QDir(collectedMediaDir).absoluteFilePath(
        QStringLiteral("shared_2.mov"));
    QHash<QString, QString> expectedCollectedPaths;
    expectedCollectedPaths.insert(
        pathSlotKey(collectVideoPath, QStringLiteral("video.filePath")),
        expectedVideoPath);
    expectedCollectedPaths.insert(
        pathSlotKey(collectLutPath, QStringLiteral("video.lutFilePath")),
        QDir(collectedMediaDir).absoluteFilePath(QStringLiteral("top.cube")));
    expectedCollectedPaths.insert(
        pathSlotKey(collectAudioPath, QStringLiteral("audio.filePath")),
        expectedAudioPath);
    expectedCollectedPaths.insert(
        pathSlotKey(collectParticlePath, QStringLiteral("particle.clipFilePath")),
        QDir(collectedMediaDir).absoluteFilePath(QStringLiteral("particle.bin")));
    expectedCollectedPaths.insert(
        pathSlotKey(collectOverlayPath, QStringLiteral("overlay.image")),
        QDir(collectedMediaDir).absoluteFilePath(QStringLiteral("overlay.png")));
    expectedCollectedPaths.insert(
        pathSlotKey(collectNestedVideoPath,
                    QStringLiteral("nested.video.filePath")),
        QDir(collectedMediaDir).absoluteFilePath(QStringLiteral("nested.mov")));
    expectedCollectedPaths.insert(
        pathSlotKey(collectNestedLutPath,
                    QStringLiteral("nested.video.lutFilePath")),
        QDir(collectedMediaDir).absoluteFilePath(QStringLiteral("nested.cube")));

    bool everySlotMatchesExpected = collectedProjectLoaded
        && collectedSlots.size() == collectOriginalSources.size();
    bool internalReferencesSkipped = everySlotMatchesExpected;
    bool duplicateSourcesShareDestination = everySlotMatchesExpected;
    QHash<QString, QString> actualPathByOriginalSlot;
    QHash<QString, QString> destinationBySource;
    if (everySlotMatchesExpected) {
        const QString expectedMediaDir = QDir(collectedMediaDir).absolutePath();
        for (int i = 0; i < collectedSlots.size(); ++i) {
            const QString &source = collectOriginalSources.at(i);
            const QString &category = collectOriginalCategories.at(i);
            const mediapaths::PathSlot &collectedSlot = collectedSlots.at(i);
            const QString actualPath = QDir::cleanPath(collectedSlot.value());
            if (collectedSlot.category != category) {
                everySlotMatchesExpected = false;
                internalReferencesSkipped = false;
                continue;
            }

            if (clipgeom::isNullObjectFilePath(source)
                || timeline_nesting::isSequenceClipFilePath(source)) {
                internalReferencesSkipped = internalReferencesSkipped
                    && actualPath == QDir::cleanPath(source);
                continue;
            }

            const QString key = pathSlotKey(source, category);
            const auto expected = expectedCollectedPaths.constFind(key);
            const bool slotMatches = expected != expectedCollectedPaths.cend()
                && actualPath == QDir::cleanPath(expected.value())
                && QFileInfo(actualPath).absolutePath() == expectedMediaDir
                && QFileInfo::exists(actualPath);
            everySlotMatchesExpected = everySlotMatchesExpected && slotMatches;
            actualPathByOriginalSlot.insert(key, actualPath);

            const QString normalizedSource = QDir::cleanPath(source);
            const auto previousDestination =
                destinationBySource.constFind(normalizedSource);
            if (previousDestination != destinationBySource.cend()) {
                duplicateSourcesShareDestination =
                    duplicateSourcesShareDestination
                    && previousDestination.value() == actualPath;
            } else {
                destinationBySource.insert(normalizedSource, actualPath);
            }
        }
    }
    internalReferencesSkipped = internalReferencesSkipped
        && collector.warnings().isEmpty();

    const QString videoSlotKey = pathSlotKey(
        collectVideoPath, QStringLiteral("video.filePath"));
    const QString audioSlotKey = pathSlotKey(
        collectAudioPath, QStringLiteral("audio.filePath"));
    const bool collisionTargetsCorrect = collectedProjectLoaded
        && actualPathByOriginalSlot.value(videoSlotKey) == expectedVideoPath
        && actualPathByOriginalSlot.value(audioSlotKey) == expectedAudioPath
        && expectedVideoPath != expectedAudioPath
        && QFileInfo::exists(expectedVideoPath)
        && QFileInfo::exists(expectedAudioPath);
    const bool copiedOnceAndProgressed = collectedProjectLoaded
        && duplicateSourcesShareDestination
        && collector.bytesCopied() == collectorSources.size() * 5
        && QDir(collectedMediaDir).entryList(QDir::Files | QDir::NoDotAndDotDot).size()
               == collectorSources.size()
        && collectorProgress.contains(100);
    gate(collectorFilesReady && everySlotMatchesExpected
             && internalReferencesSkipped && collisionTargetsCorrect
             && copiedOnceAndProgressed,
         "G6",
         !collectorFinished
             ? QStringLiteral("ProjectCollector did not finish")
             : (!collectorOk
                    ? collectorMessage
                    : QStringLiteral("collected project paths were incomplete")));

    // G7: MainWindow loads a project with an offline path -> the timeline badges
    // every clip on it, the menu counts the unique path, a relink clears both and
    // an undo re-judges only the unknown (original) path.
    const QString g7Missing = QDir(fixtureDir.path()).filePath(
        QStringLiteral("g7-offline.mp4"));
    const QString g7Present = QDir(fixtureDir.path()).filePath(
        QStringLiteral("g7-present.mp4"));
    const QString g7Project = QDir(fixtureDir.path()).filePath(
        QStringLiteral("g7.vproj"));
    const QString g8Project = QDir(fixtureDir.path()).filePath(
        QStringLiteral("g8-online.vproj"));
    const auto makeClip = [](const QString &path) {
        ClipInfo clip;
        clip.filePath = path;
        clip.duration = 1.0;
        clip.outPoint = 1.0;
        return clip;
    };
    bool g7Ready = touchFile(g7Present);
    {
        ProjectData data;
        data.videoTracks = ProjectTrackClips{QVector<ClipInfo>{
            makeClip(g7Missing), makeClip(g7Missing),
            makeClip(clipgeom::nullObjectFilePath()), makeClip(QString()),
            makeClip(timeline_nesting::sequenceClipFilePath(QStringLiteral("g7seq")))}};
        g7Ready = g7Ready && ProjectFile::save(g7Project, data);
        ProjectData online;
        online.videoTracks = ProjectTrackClips{QVector<ClipInfo>{makeClip(g7Present)}};
        g7Ready = g7Ready && ProjectFile::save(g8Project, online);
    }

    MainWindow window;
    Timeline *timeline = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));
    QString g7Detail = QStringLiteral("fixture or MainWindow timeline unavailable");
    bool g7 = false;
    if (g7Ready && timeline) {
        QString loadError;
        const bool loaded = window.openProjectFromPath(g7Project, &loadError, false);
        const auto menuCount = [&]() {
            ProjectData data;
            window.populateProjectData(data);
            return mediapaths::missingFiles(data).size();
        };
        const auto clipPathSet = [&]() {
            QSet<QString> paths;
            for (const auto *tracks : {&timeline->videoTracks(), &timeline->audioTracks()}) {
                for (const TimelineTrack *track : *tracks) {
                    for (const ClipInfo &clip : track->clips()) {
                        if (!clip.filePath.isEmpty())
                            paths.insert(clip.filePath);
                    }
                }
            }
            return paths;
        };
        const int loadedBadges = timeline->offlineClipCount();
        const int loadedMenu = menuCount();
        const QStringList cachedKeys = timeline->offlineCachedPaths();
        const bool keysMatchClips = QSet<QString>(cachedKeys.cbegin(), cachedKeys.cend())
            == clipPathSet();
        const bool internalClean = !timeline->isPathOffline(clipgeom::nullObjectFilePath())
            && !timeline->isPathOffline(QString())
            && !timeline->isPathOffline(timeline_nesting::sequenceClipFilePath(QStringLiteral("g7seq")));
        // Undo straight after the full replacement: the path set is unchanged,
        // so nothing is judged again.
        const int probesBeforeFirstUndo = timeline->offlineProbeCount();
        timeline->setClipEnabled(TrackKind::Video, 0, 0, false);
        timeline->undo();
        const int firstUndoProbes = timeline->offlineProbeCount() - probesBeforeFirstUndo;
        const int afterFirstUndoBadges = timeline->offlineClipCount();

        QString relinkError;
        const bool relinked = window.relinkMediaPaths(
            QHash<QString, QString>{{g7Missing, g7Present}}, &relinkError);
        const int relinkedBadges = timeline->offlineClipCount();
        const int relinkedMenu = menuCount();
        const int probesBeforeUndo = timeline->offlineProbeCount();
        timeline->undo();
        const int undoProbes = timeline->offlineProbeCount() - probesBeforeUndo;
        const int undoBadges = timeline->offlineClipCount();
        const bool undoInternalClean = !timeline->isPathOffline(clipgeom::nullObjectFilePath())
            && !timeline->isPathOffline(QString())
            && !timeline->isPathOffline(timeline_nesting::sequenceClipFilePath(QStringLiteral("g7seq")));

        // Catch-all refresh: A appears on disk, then an existing file B is imported
        // through Timeline::importMedia (not a MainWindow wrapper). The mediaImported
        // signal must replace the cache: A is no longer badged and B is judged.
        const QString importPath = QDir::current().absoluteFilePath(
            QStringLiteral("test_assets/e2e_clip.mp4"));
        QString importError;
        const bool aCreated = touchFile(g7Missing);
        const bool bImported = timeline->importMedia(importPath, -1, -1.0, nullptr, &importError);
        QCoreApplication::processEvents();
        const int importBadges = timeline->offlineClipCount();
        const bool aOffline = timeline->isPathOffline(g7Missing);
        const bool bOffline = timeline->isPathOffline(importPath);
        const bool importRefreshed = aCreated && bImported && importBadges == 0
            && !aOffline && !bOffline;
        QFile::remove(g7Missing);
        std::cerr << "[media-relink] G7 import-refresh aCreated=" << aCreated
                  << " imported=" << bImported << " badges=" << importBadges
                  << " isPathOffline(A)=" << aOffline << " isPathOffline(B)=" << bOffline
                  << " error='" << importError.toStdString() << "'\n";

        // Sequence-creating import (the entry point of MCP import_edl and
        // fcpx::xml::applyImport): A is cached offline, then appears on disk.
        ClipInfo g7SeqClip;
        g7SeqClip.filePath = g7Missing;
        const QVector<QVector<ClipInfo>> g7SeqTracks{QVector<ClipInfo>{g7SeqClip}};
        QFile::remove(g7Missing);
        const bool seqPre = timeline->createSequence(QStringLiteral("g7pre"), g7SeqTracks, {{}});
        QCoreApplication::processEvents();
        timeline->setOfflinePaths(QStringList{g7Missing});
        const bool seqCachedOffline = timeline->isPathOffline(g7Missing);
        const bool seqCreatedA = touchFile(g7Missing);
        const bool seqMade = timeline->createSequence(QStringLiteral("g7edl"), g7SeqTracks, {{}});
        QCoreApplication::processEvents();
        const bool seqAOffline = timeline->isPathOffline(g7Missing);
        const int seqBadges = timeline->offlineClipCount();
        const bool seqRefreshed = seqPre && seqCachedOffline && seqCreatedA && seqMade
            && !seqAOffline && seqBadges == 0;
        QFile::remove(g7Missing);
        std::cerr << "[media-relink] G7 seq-import-refresh pre=" << seqPre
                  << " cachedOffline(A)=" << seqCachedOffline << " created=" << seqMade
                  << " isPathOffline(A)=" << seqAOffline << " badges=" << seqBadges << '\n';

        std::cerr << "[media-relink] G7 loaded=" << loaded
                  << " badges/menu on load=" << loadedBadges << '/' << loadedMenu
                  << " cacheKeysEqualClipPaths=" << keysMatchClips
                  << " firstUndoProbes=" << firstUndoProbes
                  << " badgesAfterFirstUndo=" << afterFirstUndoBadges
                  << " relinked=" << relinked << " badges/menu after relink="
                  << relinkedBadges << '/' << relinkedMenu
                  << " undoProbes=" << undoProbes << " badgesAfterUndo=" << undoBadges
                  << " internalClean=" << (internalClean && undoInternalClean) << '\n';
        g7 = loaded && loadedBadges == 2 && loadedMenu == 1 && keysMatchClips
            && internalClean && firstUndoProbes == 0 && afterFirstUndoBadges == 2
            && relinked && relinkedBadges == 0 && relinkedMenu == 0
            && undoProbes == 1 && undoBadges == 2 && undoInternalClean
            && importRefreshed && seqRefreshed;
        g7Detail = QStringLiteral("badges/menu load=%1/%2 keys=%3 firstUndoProbes=%4 "
                                  "relink=%5/%6 undoProbes=%7 undoBadges=%8 error=%9")
            .arg(loadedBadges).arg(loadedMenu).arg(keysMatchClips).arg(firstUndoProbes)
            .arg(relinkedBadges).arg(relinkedMenu).arg(undoProbes).arg(undoBadges)
            .arg(loadError + relinkError + importError);
    }
    gate(g7, "G7", g7Detail);

    // G8: File > メディアを再リンク… opens no dialog when nothing is offline and
    // opens a MediaRelinkDialog when something is (a timer rejects it).
    QAction *relinkAction = window.findChild<QAction *>(QStringLiteral("file.relink_media"));
    QString g8Modal;
    QString g8Status;
    QString g8Detail = QStringLiteral("menu action file.relink_media is missing");
    bool g8 = false;
    if (relinkAction && g7Ready) {
        // One periodic closer, started before each trigger and stopped right after,
        // so no timer outlives this block (rule 62).
        QTimer closer;
        closer.setInterval(50);
        QObject::connect(&closer, &QTimer::timeout, &closer, [&g8Modal]() {
            if (QWidget *modal = QApplication::activeModalWidget()) {
                g8Modal = QLatin1String(dynamic_cast<MediaRelinkDialog *>(modal)
                                            ? "MediaRelinkDialog" : "other")
                    + QLatin1Char('/') + modal->objectName();
                if (auto *dialog = dynamic_cast<QDialog *>(modal))
                    dialog->reject();
            }
        });
        QString loadError;
        bool onlineLoaded = window.openProjectFromPath(g8Project, &loadError, false);
        window.statusBar()->clearMessage();
        closer.start();
        relinkAction->trigger();
        closer.stop();
        const QString modalWhenOnline = g8Modal;
        g8Status = window.statusBar()->currentMessage();

        g8Modal.clear();
        onlineLoaded = onlineLoaded && window.openProjectFromPath(g7Project, &loadError, false);
        closer.start();
        relinkAction->trigger();
        closer.stop();
        const QString modalWhenOffline = g8Modal;
        const bool noModalLeft = QApplication::activeModalWidget() == nullptr;
        std::cerr << "[media-relink] G8 modalOnline='" << modalWhenOnline.toStdString()
                  << "' status='" << g8Status.toStdString() << "' modalOffline='"
                  << modalWhenOffline.toStdString() << "' noModalLeft=" << noModalLeft << '\n';
        g8 = onlineLoaded && modalWhenOnline.isEmpty()
            && g8Status == QStringLiteral("オフラインのメディアはありません")
            && modalWhenOffline == QStringLiteral("MediaRelinkDialog/mediaRelinkDialog")
            && noModalLeft;
        g8Detail = QStringLiteral("online modal='%1' status='%2' offline modal='%3' load=%4")
            .arg(modalWhenOnline, g8Status, modalWhenOffline, loadError);
    }
    gate(g8, "G8", g8Detail);

    std::cerr << "[media-relink] summary: " << passed << " PASS, "
              << failed << " FAIL\n";
    // The registry checks the declared "(N gates)" against this count.
    selftests::reportTally(passed, failed, 0);
    return failed;
}
