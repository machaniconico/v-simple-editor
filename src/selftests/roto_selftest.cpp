// --selftest=roto: carrier remapping, undo, persistence, and queue restoration.
#include "SelftestGate.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../ProjectFile.h"
#include "../TrackMatteKey.h"
#include "../RenderQueue.h"
#include "../SubtitleTrackRenderer.h"
#include "../libavcore/Encode.h"
#include "../AcesColor.h"
#include <atomic>
#include <QDateTime>
#include <QMutex>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QFile>
#include <QSet>
#include <QSignalSpy>
#include <iostream>

namespace {

QString gSyntheticClipPath;

bool writeSyntheticClip(const QString &path)
{
    libavcore::EncodeRequest request;
    request.width = 64;
    request.height = 64;
    request.fps = 10;
    request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoBitrateBits = 600000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(64, 64, QImage::Format_RGB888);
    frame.fill(QColor(32, 80, 160));
    for (int i = 0; i < 20; ++i)
        if (!encoder.pushFrame(frame, i)) return false;
    return !encoder.finalize().has_value() && QFile::exists(path);
}

ClipInfo clip(int id)
{
    ClipInfo c;
    c.filePath = gSyntheticClipPath;
    c.linkGroup = id + 1;
    c.displayName = QString::number(id);
    c.duration = 2.0;
    c.inPoint = 0.0;
    c.outPoint = 2.0;
    return c;
}

RotoClipEntry entry(int marker, bool mask = false)
{
    RotoClipEntry e;
    for (int i = 0; i < marker; ++i)
        e.path.points.append(RotoPoint{});
    RotoKeyframe frame;
    frame.frameNumber = marker;
    frame.path = e.path;
    e.keyframes.append(frame);
    if (mask) {
        e.brushMask = QImage(3, 2, QImage::Format_ARGB32);
        e.brushMask.fill(Qt::white);
    }
    return e;
}

void seed(Timeline &tl, bool matte)
{
    const QVector<QVector<ClipInfo>> video{
        {clip(100), clip(101)}, {clip(1), clip(2)}, {clip(3)}};
    const QVector<QVector<ClipInfo>> audio{{clip(100), clip(101)}};
    tl.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    QHash<QString, RotoClipEntry> rotos;
    rotos.insert(QStringLiteral("1:0"), entry(1));
    rotos.insert(QStringLiteral("1:1"), entry(2, true));
    rotos.insert(QStringLiteral("2:0"), entry(3));
    tl.setRotoEntries(rotos);
    if (matte) {
        QHash<QString, TimelineTrackMatteEntry> mattes;
        QHash<QString, QString> parents;
        for (const QString &key : rotos.keys()) {
            TimelineTrackMatteEntry m;
            m.matteType = TrackMatteType::AlphaMatte;
            m.matteSourceClipId = QStringLiteral("0:0");
            mattes.insert(key, m);
            parents.insert(key, QStringLiteral("0:0"));
        }
        tl.setTrackMatteEntries(mattes);
        tl.setClipParentEntries(parents);
    }
}

QSet<QString> keys(const QHash<QString, RotoClipEntry> &entries)
{
    QSet<QString> out;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        out.insert(it.key());
    return out;
}

template <typename T> QSet<QString> keys(const QHash<QString, T> &entries)
{
    QSet<QString> out;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        out.insert(it.key());
    return out;
}

QString show(QSet<QString> values)
{
    QStringList sorted;
    for (const QString &value : values) sorted.append(value);
    sorted.sort();
    return sorted.join(QLatin1Char(','));
}

bool invariant(const QHash<QString, RotoClipEntry> &entries)
{
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        if (it.value().clipId != it.key()) return false;
    return true;
}

QString whereMarker(const QHash<QString, RotoClipEntry> &entries, int marker)
{
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        if (it.value().path.points.size() == marker) return it.key();
    return QStringLiteral("missing");
}

QHash<QString, RotoClipEntry> restoreEntries(const QVector<RotoClipEntry> &values)
{
    QHash<QString, RotoClipEntry> out;
    for (const auto &value : values) out.insert(value.clipId, value);
    return out;
}

bool samePayload(const RotoClipEntry &a, const RotoClipEntry &b)
{
    return a.clipId == b.clipId && a.path.points.size() == b.path.points.size()
        && a.keyframes.size() == b.keyframes.size()
        && a.brushMask.size() == b.brushMask.size();
}

} // namespace

int runRotoSelftest()
{
    selftest::GateTally gates;
    QTemporaryDir media;
    gSyntheticClipPath = media.filePath(QStringLiteral("roto-synthetic.mp4"));
    QString unavailable;
    if (!media.isValid() || !selftest::requireEncoder(QStringLiteral("mpeg4"), &unavailable)) {
        for (int gate = 1; gate <= 6; ++gate)
            gates.skip(gate, media.isValid() ? "mpeg4 fixture encoder unavailable"
                                             : "temporary directory unavailable");
        return gates.finish("roto");
    }
    if (!writeSyntheticClip(gSyntheticClipPath)) {
        for (int gate = 1; gate <= 6; ++gate)
            gates.gate(gate, false, "mpeg4 fixture creation failed");
        return gates.finish("roto");
    }

    // G1 detects missing catch-all wiring on any index-changing operation.
    bool g1 = true;
    const QStringList operations{
        QStringLiteral("delete"), QStringLiteral("split"),
        QStringLiteral("move"), QStringLiteral("ripple"),
        QStringLiteral("range-ripple"), QStringLiteral("paste"),
        QStringLiteral("track-add"), QStringLiteral("track-remove"),
        QStringLiteral("nudge")};
    for (bool withMatte : {false, true}) {
        for (const QString &operation : operations) {
            Timeline tl;
            seed(tl, withMatte);
            if (operation == QStringLiteral("paste")) {
                auto extra = tl.rotoEntries();
                extra.insert(QStringLiteral("0:1"), entry(4));
                tl.setRotoEntries(extra);
                if (withMatte) {
                    auto mattes = tl.trackMatteEntries();
                    TimelineTrackMatteEntry m;
                    m.matteType = TrackMatteType::AlphaMatte;
                    m.matteSourceClipId = QStringLiteral("0:0");
                    mattes.insert(QStringLiteral("0:1"), m);
                    tl.setTrackMatteEntries(mattes);
                }
            }
            if (operation == QStringLiteral("nudge")) {
                auto clips = tl.videoTracks()[1]->clips();
                clips[1].leadInSec = 1.0;
                tl.videoTracks()[1]->setClips(clips);
            }
            QString error;
            bool changed = true;
            if (operation == QStringLiteral("delete"))
                changed = tl.deleteClipByIndex(false, 1, 0, false, &error);
            else if (operation == QStringLiteral("split"))
                changed = tl.splitClipByIndex(false, 1, 0, 1.0, &error);
            else if (operation == QStringLiteral("move")) {
                double settled = 0.0;
                changed = tl.moveClipByIndex(false, 1, 0, 4.0, &settled, &error);
            } else if (operation == QStringLiteral("ripple"))
                changed = tl.deleteClipByIndex(false, 1, 0, true, &error);
            else if (operation == QStringLiteral("range-ripple")) {
                changed = tl.selectClipByIndex(false, 1, 0, &error);
                if (changed) tl.rippleDeleteTimeRangeActive(0.0, 2.0);
            } else if (operation == QStringLiteral("paste")) {
                changed = tl.selectClipByIndex(false, 0, 0, &error);
                if (changed) {
                    tl.copySelectedClip();
                    tl.setPlayheadPosition(0.0);
                    tl.pasteClip();
                }
            } else if (operation == QStringLiteral("track-add")) {
                tl.addVideoTrack();
                changed = tl.videoTrackCount() == 4;
            } else if (operation == QStringLiteral("track-remove"))
                changed = tl.removeTrack(false, 1, &error);
            else if (operation == QStringLiteral("nudge")) {
                tl.videoTracks()[1]->setSelectedClip(0);
                tl.videoTracks()[2]->toggleClipSelection(0);
                tl.nudgeSelectedClips(1);
                changed = tl.videoTracks()[1]->clips()[0].leadInSec > 0.0
                    && tl.videoTracks()[2]->clips()[0].leadInSec > 0.0;
            }
            const auto rotos = tl.rotoEntries();
            const auto actual = keys(rotos);
            QSet<QString> expected{QStringLiteral("1:0"), QStringLiteral("1:1"),
                                   QStringLiteral("2:0")};
            if (operation == QStringLiteral("delete") || operation == QStringLiteral("ripple")
                || operation == QStringLiteral("range-ripple"))
                expected = {QStringLiteral("1:0"), QStringLiteral("2:0")};
            if (operation == QStringLiteral("split"))
                expected = {QStringLiteral("1:0"), QStringLiteral("1:2"), QStringLiteral("2:0")};
            if (operation == QStringLiteral("move"))
                expected = {QStringLiteral("1:1"), QStringLiteral("2:0")};
            if (operation == QStringLiteral("track-remove"))
                expected = {QStringLiteral("1:0")};
            if (operation == QStringLiteral("paste"))
                expected.insert(QStringLiteral("0:2"));
            const auto peer = operation == QStringLiteral("range-ripple")
                ? keys(tl.clipParentEntries()) : keys(tl.trackMatteEntries());
            bool movedPayload = true;
            if (operation == QStringLiteral("delete") || operation == QStringLiteral("ripple")
                || operation == QStringLiteral("range-ripple"))
                movedPayload = rotos.value(QStringLiteral("1:0")).path.points.size() == 2;
            if (operation == QStringLiteral("split"))
                movedPayload = rotos.value(QStringLiteral("1:2")).path.points.size() == 2;
            if (operation == QStringLiteral("move"))
                movedPayload = rotos.value(QStringLiteral("1:1")).path.points.size() == 1
                    && whereMarker(rotos, 2) == QStringLiteral("missing");
            if (operation == QStringLiteral("track-remove"))
                movedPayload = rotos.value(QStringLiteral("1:0")).path.points.size() == 3;
            if (operation == QStringLiteral("paste"))
                movedPayload = rotos.value(QStringLiteral("0:2")).path.points.size() == 4;
            const bool one = changed && actual == expected && invariant(rotos) && movedPayload
                && (!withMatte || actual == peer);
            std::cerr << "G1 " << (withMatte ? "matte " : "roto ")
                      << operation.toStdString() << " actual=" << show(actual).toStdString()
                      << " expected=" << show(expected).toStdString()
                      << " peer=" << show(peer).toStdString()
                      << " marker2At=" << whereMarker(rotos, 2).toStdString()
                      << " marker3At=" << whereMarker(rotos, 3).toStdString()
                      << " marker4At=" << whereMarker(rotos, 4).toStdString()
                      << " payloadOk=" << movedPayload
                      << " changed=" << changed << " error=" << error.toStdString() << '\n';
            g1 = g1 && one;
        }
    }
    // A linkGroup-0 paste duplicates X exactly. The audio track has no clips,
    // so pasteClip's insertAt clamp places X' before [B, X]. The monotonic
    // alignment must keep the original X's carriers on index 2.
    Timeline duplicate;
    ClipInfo duplicateSource = clip(51);
    duplicateSource.linkGroup = 0;
    const QVector<QVector<ClipInfo>> duplicateVideo{
        {clip(50), duplicateSource}, {clip(60)}};
    const QVector<QVector<ClipInfo>> duplicateAudio{{}};
    duplicate.restoreFromProject(duplicateVideo, duplicateAudio, 0.0, -1.0, -1.0, 100);
    QHash<QString, RotoClipEntry> duplicateRoto;
    duplicateRoto.insert(QStringLiteral("0:1"), entry(5));
    duplicate.setRotoEntries(duplicateRoto);
    TimelineTrackMatteEntry duplicateMatte;
    duplicateMatte.matteType = TrackMatteType::AlphaMatte;
    duplicateMatte.matteSourceClipId = QStringLiteral("1:0");
    QHash<QString, TimelineTrackMatteEntry> duplicateMattes;
    duplicateMattes.insert(QStringLiteral("0:1"), duplicateMatte);
    duplicate.setTrackMatteEntries(duplicateMattes);
    QHash<QString, QString> duplicateParents;
    duplicateParents.insert(QStringLiteral("0:1"), QStringLiteral("1:0"));
    duplicate.setClipParentEntries(duplicateParents);
    QString duplicateError;
    const bool selectedX = duplicate.selectClipByIndex(false, 0, 1, &duplicateError);
    if (selectedX) {
        duplicate.copySelectedClip();
        duplicate.pasteClip();
    }
    const auto duplicateClips = duplicate.videoTracks()[0]->clips();
    const auto duplicateRotoAfter = duplicate.rotoEntries();
    const auto duplicateMatteAfter = duplicate.trackMatteEntries();
    const auto duplicateParentAfter = duplicate.clipParentEntries();
    const QSet<QString> originalAtTwo{QStringLiteral("0:2")};
    const bool duplicateOrder = duplicateClips.size() == 3
        && duplicateClips[0].linkGroup == 0
        && duplicateClips[1].linkGroup == clip(50).linkGroup
        && duplicateClips[2].linkGroup == 0;
    const bool duplicatePreserved = selectedX && duplicateOrder
        && keys(duplicateRotoAfter) == originalAtTwo
        && keys(duplicateMatteAfter) == originalAtTwo
        && keys(duplicateParentAfter) == originalAtTwo
        && duplicateRotoAfter.value(QStringLiteral("0:2")).path.points.size() == 5
        && duplicateMatteAfter.value(QStringLiteral("0:2")).matteSourceClipId == QStringLiteral("1:0")
        && duplicateParentAfter.value(QStringLiteral("0:2")) == QStringLiteral("1:0")
        && invariant(duplicateRotoAfter);
    std::cerr << "G1 duplicate [B,X]->[X',B,X] selected=" << selectedX
              << " order=" << duplicateOrder
              << " roto=" << show(keys(duplicateRotoAfter)).toStdString()
              << " matte=" << show(keys(duplicateMatteAfter)).toStdString()
              << " parent=" << show(keys(duplicateParentAfter)).toStdString()
              << " expected=" << show(originalAtTwo).toStdString()
              << " marker5At=" << whereMarker(duplicateRotoAfter, 5).toStdString()
              << " error=" << duplicateError.toStdString() << '\n';
    g1 = g1 && duplicatePreserved;
    gates.gate(1, g1, "mutation key set, peer, or clipId differed");

    // G2 detects a second alignment against an already-remapped carrier.
    Timeline twice;
    seed(twice, false);
    const TrackClipSnapshot before = snapshotTrackClips(&twice);
    twice.videoTracks()[1]->removeClip(0);
    twice.remapRotoEntriesAfterMutation(before);
    const auto once = twice.rotoEntries();
    twice.remapRotoEntriesAfterMutation(before);
    const auto twiceResult = twice.rotoEntries();
    const bool repeatedPayload = samePayload(once.value(QStringLiteral("1:0")),
                                             twiceResult.value(QStringLiteral("1:0")));
    bool g2 = keys(once) == keys(twiceResult) && invariant(twiceResult)
        && once.value(QStringLiteral("1:0")).path.points.size() == 2
        && repeatedPayload;
    Timeline nested;
    seed(nested, false);
    nested.videoTracks()[1]->addClip(clip(4));
    const TrackClipSnapshot outerBefore = snapshotTrackClips(&nested);
    QString moveError;
    double settled = 0.0;
    const bool moveOne = nested.moveClipByIndex(false, 1, 1, 6.0, &settled, &moveError);
    const bool moveTwo = nested.moveClipByIndex(false, 1, 0, 8.0, &settled, &moveError);
    const auto innerResult = nested.rotoEntries();
    nested.remapRotoEntriesAfterMutation(outerBefore);
    const auto outerResult = nested.rotoEntries();
    const bool nestedPayload = samePayload(innerResult.value(QStringLiteral("1:2")),
                                           outerResult.value(QStringLiteral("1:2")))
        && samePayload(innerResult.value(QStringLiteral("2:0")),
                       outerResult.value(QStringLiteral("2:0")));
    g2 = g2 && moveOne && moveTwo && keys(innerResult) == keys(outerResult)
        && keys(innerResult) == QSet<QString>{QStringLiteral("1:2"),
            QStringLiteral("2:0")}
        && innerResult.value(QStringLiteral("1:2")).path.points.size() == 1
        && whereMarker(innerResult, 2) == QStringLiteral("missing")
        && nestedPayload && invariant(outerResult);
    std::cerr << "G2 once=" << show(keys(once)).toStdString()
              << " repeated=" << show(keys(twiceResult)).toStdString()
              << " internal=" << show(keys(innerResult)).toStdString()
              << " outer=" << show(keys(outerResult)).toStdString()
              << " moveOne=" << moveOne << " moveTwo=" << moveTwo
              << " repeatedPayload=" << repeatedPayload
              << " nestedPayload=" << nestedPayload
              << " repeatedMarker=" << twiceResult.value(QStringLiteral("1:0")).path.points.size()
              << " moved-key=1:0->1:2 marker1At="
              << whereMarker(innerResult, 1).toStdString()
              << " marker2At=" << whereMarker(innerResult, 2).toStdString()
              << " outerMarker1At=" << whereMarker(outerResult, 1).toStdString()
              << " error=" << moveError.toStdString() << '\n';
    gates.gate(2, g2, "double remap changed surviving keys");

    // G3 detects undo snapshots that omit the carrier or its mask payload.
    Timeline undo;
    seed(undo, false);
    undo.undoManager()->clear();
    undo.undoManager()->saveState(undo.currentState(), QStringLiteral("基準"));
    QString undoError;
    const bool deleted = undo.deleteClipByIndex(false, 1, 0, false, &undoError);
    const auto afterDelete = undo.rotoEntries();
    undo.undo();
    const auto afterUndo = undo.rotoEntries();
    undo.redo();
    const auto afterRedo = undo.rotoEntries();
    std::cerr << "G3 delete=" << show(keys(afterDelete)).toStdString()
              << " undo=" << show(keys(afterUndo)).toStdString()
              << " redo=" << show(keys(afterRedo)).toStdString()
              << " points=" << afterUndo.value(QStringLiteral("1:1")).path.points.size()
              << "/" << afterRedo.value(QStringLiteral("1:0")).path.points.size()
              << " keyframes=" << afterUndo.value(QStringLiteral("1:1")).keyframes.size()
              << "/" << afterRedo.value(QStringLiteral("1:0")).keyframes.size() << '\n';
    gates.gate(3, deleted && keys(afterUndo).size() == 3
        && afterUndo.value(QStringLiteral("1:1")).path.points.size() == 2
        && afterUndo.value(QStringLiteral("1:1")).keyframes.size() == 1
        && keys(afterDelete) == keys(afterRedo)
        && afterRedo.value(QStringLiteral("1:0")).path.points.size() == 2
        && invariant(afterUndo) && invariant(afterRedo), "undo or redo lost roto payload");

    // G4 detects stale value clipIds after reindexing and lost brush masks.
    QTemporaryDir temporary;
    Timeline persisted;
    seed(persisted, false);
    ProjectData data;
    data.videoTracks = persisted.allVideoTracks();
    data.audioTracks = persisted.allAudioTracks();
    data.rotoClipEntries = {persisted.rotoEntries().value(QStringLiteral("1:0")),
                            persisted.rotoEntries().value(QStringLiteral("1:1"))};
    const QString path = temporary.filePath(QStringLiteral("roto.veditor"));
    ProjectData disk, jsonData;
    const bool saved = temporary.isValid() && ProjectFile::save(path, data);
    const bool loaded = saved && ProjectFile::load(path, disk);
    const QString json = ProjectFile::toJsonString(data);
    const bool jsonLoaded = ProjectFile::fromJsonString(json, jsonData);
    Timeline diskTimeline, jsonTimeline;
    diskTimeline.setRotoEntries(restoreEntries(disk.rotoClipEntries));
    jsonTimeline.setRotoEntries(restoreEntries(jsonData.rotoClipEntries));
    const auto diskEntries = diskTimeline.rotoEntries();
    const auto jsonEntries = jsonTimeline.rotoEntries();
    const QJsonArray serialized = QJsonDocument::fromJson(json.toUtf8()).object()
        .value(QStringLiteral("rotoClipEntries")).toArray();
    QSet<QString> fields;
    for (const auto &value : serialized)
        for (const auto &key : value.toObject().keys()) fields.insert(key);
    const QSet<QString> expectedPersistedKeys = keys(restoreEntries(data.rotoClipEntries));
    const bool diskFirst = samePayload(
        diskEntries.value(QStringLiteral("1:0")), data.rotoClipEntries[0]);
    const bool diskSecond = samePayload(
        diskEntries.value(QStringLiteral("1:1")), data.rotoClipEntries[1]);
    const bool jsonFirst = samePayload(
        jsonEntries.value(QStringLiteral("1:0")), data.rotoClipEntries[0]);
    const bool jsonSecond = samePayload(
        jsonEntries.value(QStringLiteral("1:1")), data.rotoClipEntries[1]);
    bool g4 = loaded && jsonLoaded
        && expectedPersistedKeys == QSet<QString>{QStringLiteral("1:0"), QStringLiteral("1:1")}
        && keys(diskEntries) == expectedPersistedKeys
        && keys(jsonEntries) == expectedPersistedKeys
        && diskFirst && diskSecond && jsonFirst && jsonSecond
        && serialized.size() == 2
        && !serialized[0].toObject().contains(QStringLiteral("brushMaskPngBase64"))
        && serialized[1].toObject().contains(QStringLiteral("brushMaskPngBase64"))
        && fields == QSet<QString>{QStringLiteral("clipId"), QStringLiteral("path"),
            QStringLiteral("keyframes"), QStringLiteral("brushMaskPngBase64")};
    QString editError;
    const bool edited = persisted.deleteClipByIndex(false, 1, 0, false, &editError);
    ProjectData editedData = data;
    editedData.videoTracks = persisted.allVideoTracks();
    editedData.rotoClipEntries.clear();
    for (const auto &value : persisted.rotoEntries())
        editedData.rotoClipEntries.append(value);
    const QString editedPath = temporary.filePath(QStringLiteral("edited.veditor"));
    ProjectData editedLoaded;
    const bool editedSaved = ProjectFile::save(editedPath, editedData);
    const bool editedRead = editedSaved && ProjectFile::load(editedPath, editedLoaded);
    Timeline editedTimeline;
    editedTimeline.setRotoEntries(restoreEntries(editedLoaded.rotoClipEntries));
    const auto editedEntry = editedTimeline.rotoEntries().value(QStringLiteral("1:0"));
    const QJsonArray editedSerialized = QJsonDocument::fromJson(
        ProjectFile::toJsonString(editedData).toUtf8()).object()
        .value(QStringLiteral("rotoClipEntries")).toArray();
    bool editedJsonClipId = false;
    for (const auto &value : editedSerialized) {
        const auto object = value.toObject();
        if (object.value(QStringLiteral("clipId")).toString() == QStringLiteral("1:0")
            && object.contains(QStringLiteral("brushMaskPngBase64")))
            editedJsonClipId = true;
    }
    g4 = g4 && edited && editedRead && editedEntry.clipId == QStringLiteral("1:0")
        && editedEntry.path.points.size() == 2 && editedEntry.brushMask.size() == QSize(3, 2)
        && editedJsonClipId;
    std::cerr << "G4 disk=" << show(keys(diskEntries)).toStdString()
              << " json=" << show(keys(jsonEntries)).toStdString()
              << " expected=" << show(expectedPersistedKeys).toStdString()
              << " payload disk=" << diskFirst << '/' << diskSecond
              << " json=" << jsonFirst << '/' << jsonSecond
              << " fields=" << show(fields).toStdString()
              << " edited=" << show(keys(editedTimeline.rotoEntries())).toStdString()
              << " saved=" << saved << " loaded=" << loaded
              << " jsonLoaded=" << jsonLoaded << " editedRead=" << editedRead
              << " editedJsonClipId=" << editedJsonClipId
              << " clipId=" << editedEntry.clipId.toStdString()
              << " points=" << editedEntry.path.points.size()
              << " mask=" << editedEntry.brushMask.width() << 'x'
              << editedEntry.brushMask.height() << '\n';
    gates.gate(4, g4, "project roundtrip or edited clipId failed");

    // G5 detects a file-backed queue resolver that omits or fails to normalize roto.
    ProjectData queueData = data;
    queueData.rotoClipEntries[0].clipId = QStringLiteral("01:00");
    const QString queuePath = temporary.filePath(QStringLiteral("queue.veditor"));
    const bool queueSaved = ProjectFile::save(queuePath, queueData);
    RenderJob job;
    job.projectFilePath = queuePath;
    job.outputPath = temporary.filePath(QStringLiteral("queue-output.mp4"));
    job.width = 64;
    job.height = 64;
    job.codec = QStringLiteral("mpeg4");
    job.bitrateBps = 600000;
    job.endUs = 100000; // one frame is enough to exercise the file-backed resolver
    job.exportConfig.insert(QStringLiteral("fps"), 10);
    RenderQueue queue;
    queue.setProperty("captureResolvedRotoClipIdsForTest", true);
    QSignalSpy finished(&queue, &RenderQueue::jobCompletedUuid);
    if (queueSaved) {
        queue.addJob(job);
        queue.start();
        if (finished.isEmpty()) finished.wait(60000);
    }
    const QJsonObject resolvedIds =
        queue.property("resolvedRotoClipIdsForTest").toJsonObject();
    QSet<QString> resolvedKeys;
    for (const QString &key : resolvedIds.keys()) resolvedKeys.insert(key);
    const bool completed = !finished.isEmpty() && finished.first().value(1).toBool();
    const bool g5 = queueSaved && completed && resolvedIds.size() == 2
        && resolvedIds.value(QStringLiteral("1:0")).toString() == QStringLiteral("1:0")
        && resolvedIds.value(QStringLiteral("1:1")).toString() == QStringLiteral("1:1");
    std::cerr << "G5 restored=" << show(resolvedKeys).toStdString()
              << " count=" << resolvedIds.size()
              << " canonicalClipId="
              << resolvedIds.value(QStringLiteral("1:0")).toString().toStdString()
              << " completed=" << completed
              << " error=" << (finished.isEmpty() ? QStringLiteral("timeout")
                  : finished.first().value(2).toString()).toStdString() << '\n';
    gates.gate(5, g5, "RenderQueue file resolver omitted or failed to canonicalize roto");

    // G6 detects the roto-only early-return bug in the clipMoved handler.
    Timeline reordered;
    seed(reordered, false);
    const auto oldKeys = keys(reordered.rotoEntries());
    reordered.videoTracks()[1]->moveClip(0, 1);
    const auto newEntries = reordered.rotoEntries();
    const bool g6 = keys(newEntries) == oldKeys && invariant(newEntries)
        && newEntries.value(QStringLiteral("1:0")).path.points.size() == 2
        && newEntries.value(QStringLiteral("1:1")).path.points.size() == 1;
    std::cerr << "G6 from=0 to=1 before=" << show(oldKeys).toStdString()
              << " after=" << show(keys(newEntries)).toStdString()
              << " markers=" << newEntries.value(QStringLiteral("1:0")).path.points.size()
              << '/' << newEntries.value(QStringLiteral("1:1")).path.points.size() << '\n';
    gates.gate(6, g6, "clipMoved failed to reorder the roto carrier");
    return gates.finish("roto");
}
