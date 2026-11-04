// US-2000: timeline markers and adjustment layers are project data. They go
// through ProjectData (populateProjectData / applyLoadedProjectData), ProjectFile
// save / load and toJsonString / fromJsonString, every load replaces both lists
// before the undo baseline, and the file-backed render queue
// (RenderQueue::resolveTimeline) applies the saved adjustment layers.
#include "../MainWindow.h"
#include "../ProjectFile.h"
#include "../RenderQueue.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "SelftestGate.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

// ProjectFile.cpp's externally-linked marker serializer (no header declares it).
QJsonArray projectFileMarkersToJson(const QVector<Marker> &markers);

namespace {

constexpr int kW = 160;
constexpr int kH = 90;

std::string b(bool v) { return v ? "1" : "0"; }
std::string n(qint64 v) { return std::to_string(v); }

Marker makeMarker(int id, qint64 timelineUs, const char *label, const char *note, qint64 durationUs)
{
    Marker m;
    m.id = id;
    m.timelineUs = timelineUs;
    m.label = QString::fromLatin1(label);
    m.color = QColor(QStringLiteral("#00ff00"));
    m.note = QString::fromLatin1(note);
    m.durationUs = durationUs;
    return m;
}

QVector<Marker> sampleMarkers()
{
    return {makeMarker(3, 1500000, "intro", "first note", 0),
            makeMarker(7, 4000000, "chorus", "second note", 500000)};
}

AdjustmentLayer sampleLayer()
{
    AdjustmentLayer l;
    l.id = 2;
    l.trackIndex = 0;
    l.timelineStartUs = 0;
    l.timelineEndUs = 2000000;
    l.gradingEnabled = true;
    l.lift[0] = 5.0;
    l.name = QStringLiteral("persist grade");
    return l;
}

bool sameMarkers(const QVector<Marker> &got, const QVector<Marker> &want, std::string &d)
{
    d += "markers=" + std::to_string(got.size()) + "/" + std::to_string(want.size());
    if (got.size() != want.size())
        return false;
    bool ok = true;
    for (int i = 0; i < want.size(); ++i) {
        const Marker &g = got[i];
        const Marker &w = want[i];
        const bool same = g.id == w.id && g.timelineUs == w.timelineUs && g.label == w.label
            && g.color.name() == w.color.name() && g.note == w.note && g.durationUs == w.durationUs;
        d += " [id=" + std::to_string(g.id) + " us=" + n(g.timelineUs) + " label=" + g.label.toStdString()
            + " color=" + g.color.name().toStdString() + " note=" + g.note.toStdString()
            + " dur=" + n(g.durationUs) + " same=" + b(same) + "]";
        ok = ok && same;
    }
    return ok;
}

bool sameLayers(const QVector<AdjustmentLayer> &got, const QVector<AdjustmentLayer> &want, std::string &d)
{
    d += " layers=" + std::to_string(got.size()) + "/" + std::to_string(want.size());
    if (got.size() != want.size())
        return false;
    bool ok = true;
    for (int i = 0; i < want.size(); ++i) {
        const AdjustmentLayer &g = got[i];
        const AdjustmentLayer &w = want[i];
        const bool same = g.id == w.id && g.timelineStartUs == w.timelineStartUs
            && g.timelineEndUs == w.timelineEndUs && g.trackIndex == w.trackIndex
            && g.gradingEnabled == w.gradingEnabled && g.lift[0] == w.lift[0] && g.name == w.name;
        d += " [id=" + std::to_string(g.id) + " start=" + n(g.timelineStartUs) + " end=" + n(g.timelineEndUs)
            + " track=" + std::to_string(g.trackIndex) + " grading=" + b(g.gradingEnabled)
            + " lift0=" + std::to_string(g.lift[0]) + " name=" + g.name.toStdString() + " same=" + b(same) + "]";
        ok = ok && same;
    }
    return ok;
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QStringList sortedKeys(const QJsonObject &o)
{
    QStringList keys = o.keys();
    keys.sort();
    return keys;
}

double meanAbsDiff(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return -1.0;
    const QImage x = a.convertToFormat(QImage::Format_RGB888);
    const QImage y = b.convertToFormat(QImage::Format_RGB888);
    double sum = 0.0;
    for (int row = 0; row < x.height(); ++row) {
        const uchar *p = x.constScanLine(row);
        const uchar *q = y.constScanLine(row);
        for (int i = 0; i < x.width() * 3; ++i)
            sum += std::abs(int(p[i]) - int(q[i]));
    }
    return sum / (double(x.width()) * x.height() * 3.0);
}

// A full-canvas grey rectangle on V1 (no media file), so the frame is never an
// empty V1 and the adjustment layer's lift has pixels to change.
ClipInfo greyClip()
{
    Shape rectangle;
    rectangle.type = ShapeType::Rectangle;
    rectangle.position = QPointF(kW / 2.0, kH / 2.0);
    rectangle.properties.size = QSizeF(kW * 2.0, kH * 2.0);
    rectangle.fill.color = QColor(110, 110, 110);
    rectangle.stroke.enabled = false;
    ClipInfo clip;
    clip.displayName = QStringLiteral("project-persist grey");
    clip.duration = clip.outPoint = 2.0;
    clip.shapes = {rectangle};
    return clip;
}

} // namespace

int runProjectPersistSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir dir;
    if (!dir.isValid()) {
        for (int g = 1; g <= 7; ++g)
            tally.gate(g, false, "temporary directory unavailable");
        return tally.finish("project-persist");
    }
    const auto tmp = [&](const char *name) { return QDir(dir.path()).filePath(QString::fromLatin1(name)); };
    const QVector<Marker> markers = sampleMarkers();
    const AdjustmentLayer layer = sampleLayer();

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    MainWindow window;
    Timeline &timeline = *window.m_timeline;
    UndoManager *undo = timeline.undoManager();
    const QString pathA = tmp("a.veditor");

    // ---- G1: save -> open on the same MainWindow brings both lists back ----
    // The lists are cleared between save and open, so only the file can bring
    // them back (HEAD writes empty arrays and never reads them).
    bool g7Opened = false;
    std::string g7Detail;
    bool g7 = false;
    {
        timeline.setMarkers(markers);
        timeline.setAdjustmentLayers({layer});
        QString err;
        const bool saved = window.saveProjectToPath(pathA, &err);
        const QJsonObject root = readJson(pathA);
        const int jsonMarkers = root.value(QStringLiteral("markers")).toArray().size();
        const int jsonLayers = root.value(QStringLiteral("adjustmentLayers")).toArray().size();
        timeline.setMarkers({});
        timeline.setAdjustmentLayers({});
        QSignalSpy sequenceSpy(&timeline, &Timeline::sequenceChanged);
        const bool opened = window.openProjectFromPath(pathA, &err, false);
        g7Opened = opened;
        std::string d = "saved=" + b(saved) + " json.markers=" + std::to_string(jsonMarkers)
            + " json.adjustmentLayers=" + std::to_string(jsonLayers) + " opened=" + b(opened) + " ";
        const bool okMarkers = sameMarkers(timeline.markers(), markers, d);
        const bool okLayers = sameLayers(timeline.adjustmentLayers(), {layer}, d);
        tally.gate(1, saved && opened && jsonMarkers == 2 && jsonLayers == 1 && okMarkers && okLayers, d);

        // ---- G7 (measured here, reported last): the load is clean and in the baseline ----
        // Wait for the debounced sequenceChanged of the load so a late signal
        // that marks the project dirty is seen.
        if (sequenceSpy.isEmpty())
            sequenceSpy.wait(3000);
        QCoreApplication::processEvents();
        const bool modified = window.isWindowModified();
        const int index = undo->currentIndex();
        timeline.undo();
        const int layersAfterUndo = timeline.adjustmentLayers().size();
        const int markersAfterUndo = timeline.markers().size();
        // A History-panel jump (UndoManager::jumpTo) from a later structural
        // edit straight back to the baseline applies the baseline's own
        // adjustment layers: a non-structural undo point, a new track, then
        // jump to entry 0. Setters run after the baseline would leave it
        // without the layer, and the jump would drop it. This leg runs before
        // any structural edit at entry 0 (which re-captures the live layers
        // into the baseline).
        timeline.saveUndoState(QStringLiteral("project-persist edit"));
        timeline.addVideoTrack(true);
        const int indexBeforeJump = undo->currentIndex();
        const bool jumped = undo->jumpTo(0);
        const int indexAfterJump = undo->currentIndex();
        std::string dj;
        const bool layerAfterJump = sameLayers(timeline.adjustmentLayers(), {layer}, dj);
        const int markersAfterJump = timeline.markers().size();
        // A real edit after the load, then Ctrl+Z: the undo restores the
        // "Project loaded" baseline, which must already hold the layer.
        const int tracksBefore = timeline.videoTrackCount();
        timeline.addVideoTrack(true);
        const int indexAfterEdit = undo->currentIndex();
        timeline.undo();
        const int indexAfterEditUndo = undo->currentIndex();
        std::string dl;
        const bool layerAfterEditUndo = sameLayers(timeline.adjustmentLayers(), {layer}, dl);
        const int markersAfterEditUndo = timeline.markers().size();
        g7 = g7Opened && !modified && index == 0 && layersAfterUndo == 1 && markersAfterUndo == 2
            && indexAfterEdit == 1 && indexAfterEditUndo == 0 && layerAfterEditUndo
            && markersAfterEditUndo == 2 && indexBeforeJump == 2 && jumped && indexAfterJump == 0
            && layerAfterJump && markersAfterJump == 2;
        g7Detail = "opened=" + b(g7Opened) + " sequenceSignals=" + std::to_string(sequenceSpy.count())
            + " modified=" + b(modified) + " currentIndex=" + std::to_string(index)
            + " undo: layers=" + std::to_string(layersAfterUndo) + " markers=" + std::to_string(markersAfterUndo)
            + " | edit: tracks=" + std::to_string(tracksBefore) + " index=" + std::to_string(indexAfterEdit)
            + " undo index=" + std::to_string(indexAfterEditUndo) + dl
            + " markers=" + std::to_string(markersAfterEditUndo)
            + " | jump: index " + std::to_string(indexBeforeJump) + "->" + std::to_string(indexAfterJump)
            + " ok=" + b(jumped) + dj + " markers=" + std::to_string(markersAfterJump);
    }

    // ---- G2: toJsonString -> fromJsonString, and the JSON marker shape ----
    {
        ProjectData data;
        data.markers = markers;
        data.adjustmentLayers = {layer};
        const QString json = ProjectFile::toJsonString(data);
        ProjectData back;
        const bool parsed = ProjectFile::fromJsonString(json, back);
        std::string d = "parsed=" + b(parsed) + " ";
        const bool okMarkers = sameMarkers(back.markers, markers, d);
        const bool okLayers = sameLayers(back.adjustmentLayers, {layer}, d);
        const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
        const QJsonArray arr = root.value(QStringLiteral("markers")).toArray();
        const int jsonLayers = root.value(QStringLiteral("adjustmentLayers")).toArray().size();
        bool keysOk = arr.size() == 2;
        for (int i = 0; i < arr.size() && i < markers.size(); ++i) {
            const QStringList got = sortedKeys(arr[i].toObject());
            const QStringList want = sortedKeys(projectFileMarkersToJson({markers[i]}).at(0).toObject());
            keysOk = keysOk && got == want && !want.isEmpty();
            d += " keys[" + std::to_string(i) + "]=" + got.join(QLatin1Char(',')).toStdString()
                + (got == want ? " (same)" : " want=" + want.join(QLatin1Char(',')).toStdString());
        }
        d += " json.markers=" + std::to_string(arr.size()) + " json.adjustmentLayers=" + std::to_string(jsonLayers);
        tally.gate(2, parsed && okMarkers && okLayers && keysOk && jsonLayers == 1, d);
    }

    // ---- G3: every load replaces both lists; New Project (resetProject) clears them ----
    {
        ProjectData empty;
        empty.config = window.m_projectConfig;
        const QString pathB = tmp("b.veditor");
        const bool savedB = ProjectFile::save(pathB, empty);
        const QJsonObject rootB = readJson(pathB);
        const bool bHasEmptyArrays = rootB.value(QStringLiteral("markers")).isArray()
            && rootB.value(QStringLiteral("markers")).toArray().isEmpty()
            && rootB.value(QStringLiteral("adjustmentLayers")).isArray()
            && rootB.value(QStringLiteral("adjustmentLayers")).toArray().isEmpty();
        QString err;
        bool ok = savedB && bHasEmptyArrays;
        // (a) open A, then B
        const bool openedA = window.openProjectFromPath(pathA, &err, false);
        const int aMarkers = timeline.markers().size();
        const int aLayers = timeline.adjustmentLayers().size();
        const bool openedB = window.openProjectFromPath(pathB, &err, false);
        const int bMarkers = timeline.markers().size();
        const int bLayers = timeline.adjustmentLayers().size();
        ok = ok && openedA && aMarkers == 2 && aLayers == 1 && openedB && bMarkers == 0 && bLayers == 0;
        // (b) open A, then New Project
        const bool openedA2 = window.openProjectFromPath(pathA, &err, false);
        const int a2Markers = timeline.markers().size();
        const int a2Layers = timeline.adjustmentLayers().size();
        window.resetProject(window.m_projectConfig);
        const int rMarkers = timeline.markers().size();
        const int rLayers = timeline.adjustmentLayers().size();
        ok = ok && openedA2 && a2Markers == 2 && a2Layers == 1 && rMarkers == 0 && rLayers == 0;
        tally.gate(3, ok, "savedB=" + b(savedB) + " bEmptyArrays=" + b(bHasEmptyArrays)
            + " (a) A opened=" + b(openedA) + " markers=" + std::to_string(aMarkers) + " layers=" + std::to_string(aLayers)
            + " -> B opened=" + b(openedB) + " markers=" + std::to_string(bMarkers) + " layers=" + std::to_string(bLayers)
            + " (b) A opened=" + b(openedA2) + " markers=" + std::to_string(a2Markers) + " layers=" + std::to_string(a2Layers)
            + " -> reset markers=" + std::to_string(rMarkers) + " layers=" + std::to_string(rLayers));
    }

    // ---- G4: old files (no keys) and empty arrays read as empty ----
    // Detects a reader that only assigns when the key is present: the target
    // data is pre-filled, so a skipped assignment leaves stale lists behind.
    {
        const QJsonObject base = QJsonDocument::fromJson(ProjectFile::toJsonString(ProjectData{}).toUtf8()).object();
        QJsonObject noKeys = base;
        noKeys.remove(QStringLiteral("markers"));
        noKeys.remove(QStringLiteral("adjustmentLayers"));
        const bool baseHasEmpty = base.value(QStringLiteral("markers")).isArray()
            && base.value(QStringLiteral("markers")).toArray().isEmpty()
            && base.value(QStringLiteral("adjustmentLayers")).isArray()
            && base.value(QStringLiteral("adjustmentLayers")).toArray().isEmpty();
        const bool noKeysPremise = !noKeys.contains(QStringLiteral("markers"))
            && !noKeys.contains(QStringLiteral("adjustmentLayers"));
        ProjectData fromOld;
        fromOld.markers = markers;
        fromOld.adjustmentLayers = {layer};
        const bool okOld = ProjectFile::fromJsonString(
            QString::fromUtf8(QJsonDocument(noKeys).toJson(QJsonDocument::Compact)), fromOld);
        ProjectData fromEmpty;
        fromEmpty.markers = markers;
        fromEmpty.adjustmentLayers = {layer};
        const bool okEmpty = ProjectFile::fromJsonString(
            QString::fromUtf8(QJsonDocument(base).toJson(QJsonDocument::Compact)), fromEmpty);
        const bool ok = baseHasEmpty && noKeysPremise && okOld && okEmpty
            && fromOld.markers.isEmpty() && fromOld.adjustmentLayers.isEmpty()
            && fromEmpty.markers.isEmpty() && fromEmpty.adjustmentLayers.isEmpty();
        tally.gate(4, ok, "premise emptyArrays=" + b(baseHasEmpty) + " noKeys=" + b(noKeysPremise)
            + " (old) parsed=" + b(okOld) + " markers=" + std::to_string(fromOld.markers.size())
            + " layers=" + std::to_string(fromOld.adjustmentLayers.size())
            + " (empty) parsed=" + b(okEmpty) + " markers=" + std::to_string(fromEmpty.markers.size())
            + " layers=" + std::to_string(fromEmpty.adjustmentLayers.size()));
    }

    // ---- G5: trackIndex clamp on a 2-video-track project, and ids after load ----
    {
        ProjectData data;
        data.config = window.m_projectConfig;
        data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{greyClip()}, QVector<ClipInfo>{}};
        data.markers = markers;
        AdjustmentLayer high = layer;
        high.trackIndex = 5;
        data.adjustmentLayers = {high};
        const QString pathC = tmp("c.veditor");
        const bool saved = ProjectFile::save(pathC, data);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        MainWindow fresh;
        Timeline &freshTimeline = *fresh.m_timeline;
        QString err;
        const int tracksBefore = freshTimeline.videoTrackCount();
        const bool opened = fresh.openProjectFromPath(pathC, &err, false);
        const int tracks = freshTimeline.videoTrackCount();
        const int loadedLayers = freshTimeline.adjustmentLayers().size();
        const int trackIndex = loadedLayers == 1 ? freshTimeline.adjustmentLayers()[0].trackIndex : -1;
        const int markerMax = freshTimeline.markers().isEmpty() ? -1 : std::max_element(
            freshTimeline.markers().cbegin(), freshTimeline.markers().cend(),
            [](const Marker &x, const Marker &y) { return x.id < y.id; })->id;
        const int newMarker = freshTimeline.addMarker(6000000, QStringLiteral("new"));
        const int newLayer = freshTimeline.addAdjustmentLayer(AdjustmentLayer{});
        const bool ok = saved && opened && tracks == 2 && loadedLayers == 1
            && trackIndex == 1 && markerMax == 7 && newMarker == 8 && newLayer == layer.id + 1;
        tally.gate(5, ok, "saved=" + b(saved) + " opened=" + b(opened) + " videoTracks " + std::to_string(tracksBefore)
            + "->" + std::to_string(tracks) + " layers=" + std::to_string(loadedLayers)
            + " trackIndex 5->" + std::to_string(trackIndex) + " markerMaxId=" + std::to_string(markerMax)
            + " addMarker=" + std::to_string(newMarker) + " addAdjustmentLayer=" + std::to_string(newLayer)
            + " (want " + std::to_string(layer.id + 1) + ")");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // ---- G6: the file-backed render queue Timeline applies the saved layer ----
    {
        ProjectData data;
        data.config.width = kW;
        data.config.height = kH;
        data.config.fps = 30;
        data.config.explicitOutputResolution = true;
        data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{greyClip()}};
        data.adjustmentLayers = {layer};
        const QString pathD = tmp("d.veditor");
        const bool saved = ProjectFile::save(pathD, data);
        RenderJob job;
        job.timeline = nullptr;
        job.projectFilePath = pathD;
        Timeline *owned = nullptr;
        Timeline *tl = RenderQueue::resolveTimeline(job, &owned);
        const bool resolved = tl && owned == tl;
        const int fileLayers = tl ? tl->adjustmentLayers().size() : -1;
        const qint64 atUs = 1000000;
        QImage fromFile, without, inMemory;
        if (resolved) {
            fromFile = tlrender::renderFrameAt(tl, atUs, QSize(kW, kH));
            tl->setAdjustmentLayers({});
            without = tlrender::renderFrameAt(tl, atUs, QSize(kW, kH));
            tl->setAdjustmentLayers({layer});
            inMemory = tlrender::renderFrameAt(tl, atUs, QSize(kW, kH));
        }
        delete owned;
        const double diff = meanAbsDiff(fromFile, without);
        const bool sameAsMemory = !fromFile.isNull() && fromFile.format() == inMemory.format()
            && fromFile == inMemory;
        const bool ok = saved && resolved && !fromFile.isNull() && !without.isNull() && diff > 1.0 && sameAsMemory;
        tally.gate(6, ok, "saved=" + b(saved) + " resolved=" + b(resolved) + " fileLayers=" + std::to_string(fileLayers)
            + " meanAbsDiff(file, noLayer)=" + std::to_string(diff) + " format=" + std::to_string(int(fromFile.format()))
            + "/" + std::to_string(int(inMemory.format())) + " sameAsInMemory=" + b(sameAsMemory));
    }

    tally.gate(7, g7, g7Detail);
    return tally.finish("project-persist");
}
