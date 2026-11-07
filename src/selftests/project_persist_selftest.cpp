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
        for (int g = 1; g <= 11; ++g)
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

    // ---- G8 / G9 (US-2103): load-time numeric repair and the version gate ----
    // A project document with `clips` clips (10 s each); `config` keys and per-clip
    // `patches` keys are written over the saved JSON, so the values are the file's, not the model's.
    const auto docWith = [&](const QJsonObject &config, const QVector<QJsonObject> &patches,
                             const QJsonValue &version = QJsonValue(QJsonValue::Undefined)) {
        ProjectData base;
        QVector<ClipInfo> clips;
        for (int i = 0; i < patches.size(); ++i) {
            ClipInfo c;
            c.filePath = QStringLiteral("/selftests/project-persist-%1.mp4").arg(i);
            c.displayName = QStringLiteral("c%1").arg(i);
            c.duration = 10.0;
            c.outPoint = 10.0;
            clips.append(c);
        }
        base.videoTracks = {clips};
        base.audioTracks = {{}};
        QJsonObject root = QJsonDocument::fromJson(ProjectFile::toJsonString(base).toUtf8()).object();
        QJsonObject cfg = root.value(QStringLiteral("config")).toObject();
        for (auto it = config.begin(); it != config.end(); ++it)
            cfg[it.key()] = it.value();
        root[QStringLiteral("config")] = cfg;
        QJsonArray track = root.value(QStringLiteral("videoTracks")).toArray().at(0).toArray();
        for (int i = 0; i < patches.size(); ++i) {
            QJsonObject clip = track.at(i).toObject();
            for (auto it = patches[i].begin(); it != patches[i].end(); ++it) {
                if (it.value() == QJsonValue(QStringLiteral("<remove>")))
                    clip.remove(it.key());
                else
                    clip[it.key()] = it.value();
            }
            track[i] = clip;
        }
        QJsonArray tracks;
        tracks.append(track);
        root[QStringLiteral("videoTracks")] = tracks;
        if (!version.isUndefined())
            root[QStringLiteral("version")] = version;
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    };
    {
        bool ok = true;
        std::string d;
        const auto check = [&](const std::string &name, bool passed, const std::string &value = std::string()) {
            ok = ok && passed;
            d += name + "=" + b(passed) + (value.empty() ? "" : "(" + value + ") ");
        };
        const auto closeTo = [](double got, double want) { return std::abs(got - want) <= 1e-9 * std::abs(want); };
        const QVector<QJsonValue> speeds = {QJsonValue(0), QJsonValue(-2), QJsonValue(QStringLiteral("x")),
                                            QJsonValue(1e308), QJsonValue(8.0), QJsonValue(0.05),
                                            QJsonValue(1.2e-7), QJsonValue(8.6e6), QJsonValue(1e-8),
                                            QJsonValue(QJsonValue::Null)};
        const double wantSpeed[] = {1.0, 1.0, 1.0, 1e7, 8.0, 0.05, 1.2e-7, 8.6e6, 1.0, 1.0};
        QVector<QJsonObject> patches;
        for (const QJsonValue &sp : speeds)
            patches.append(QJsonObject{{QStringLiteral("speed"), sp}});
        patches.append(QJsonObject{{QStringLiteral("volume"), -1}, {QStringLiteral("inPoint"), 5},
                                   {QStringLiteral("outPoint"), 2}});
        // duration 2 / inPoint 5 with the "use duration" outPoint spellings (0, missing, negative, null),
        // then the control: inPoint 3, outPoint 0, duration 10 stays valid.
        const int firstSentinel = patches.size();
        for (const QJsonValue &op : {QJsonValue(0), QJsonValue(QStringLiteral("<remove>")), QJsonValue(-1),
                                     QJsonValue(QJsonValue::Null)})
            patches.append(QJsonObject{{QStringLiteral("duration"), 2}, {QStringLiteral("inPoint"), 5},
                                       {QStringLiteral("outPoint"), op}});
        patches.append(QJsonObject{{QStringLiteral("duration"), 10}, {QStringLiteral("inPoint"), 3},
                                   {QStringLiteral("outPoint"), 0}});
        const QString hostile = docWith({{QStringLiteral("fps"), 0}, {QStringLiteral("width"), -5},
                                         {QStringLiteral("height"), QStringLiteral("x")}}, patches);
        ProjectData got;
        const bool parsed = ProjectFile::fromJsonString(hostile, got);
        check("parsed", parsed);
        check("fps", got.config.fps == 30, std::to_string(got.config.fps));
        check("width", got.config.width == 1920, std::to_string(got.config.width));
        check("height", got.config.height == 1080, std::to_string(got.config.height));
        const QVector<ClipInfo> clips = got.videoTracks.value(0);
        check("clips", clips.size() == patches.size(), std::to_string(clips.size()));
        std::string sp;
        bool speedsOk = clips.size() == patches.size();
        bool finite = speedsOk;
        for (int i = 0; i < speeds.size() && i < clips.size(); ++i) {
            speedsOk = speedsOk && closeTo(clips[i].speed, wantSpeed[i]);
            sp += std::to_string(clips[i].speed) + "/";
        }
        for (const ClipInfo &c : clips) {
            const double eff = c.effectiveDuration();
            finite = finite && std::isfinite(eff) && eff >= 0.0;
        }
        check("speeds", speedsOk, sp);
        check("effectiveDurationFinite", finite);
        if (clips.size() == patches.size()) {
            const ClipInfo &trim = clips.at(speeds.size());
            check("volume", trim.volume == 1.0, std::to_string(trim.volume));
            check("inOut", trim.inPoint == 5.0 && trim.outPoint == 5.0,
                  std::to_string(trim.inPoint) + "/" + std::to_string(trim.outPoint));
            const char *const names[] = {"zero", "missing", "negative", "null"};
            for (int k = 0; k < 4; ++k) {
                const ClipInfo &c = clips.at(firstSentinel + k);
                const double eff = c.effectiveDuration();
                check(std::string("sentinelOut-") + names[k],
                      c.inPoint == 5.0 && c.outPoint == 5.0 && std::isfinite(eff) && eff >= 0.0,
                      std::to_string(c.outPoint) + "/" + std::to_string(eff));
            }
            const ClipInfo &ctl = clips.last();
            check("sentinelKept", ctl.outPoint == 0.0 && closeTo(ctl.effectiveDuration(), 7.0),
                  std::to_string(ctl.outPoint) + "/" + std::to_string(ctl.effectiveDuration()));
        }
        // Controls: legitimate values are untouched, only the out-of-band ones move.
        struct FpsCase { double in; int want; };
        for (const FpsCase &f : {FpsCase{60, 60}, FpsCase{240, 240}, FpsCase{5000, 1000},
                                 FpsCase{29.97, 30}, FpsCase{23.976, 30}}) {
            ProjectData r;
            const bool p = ProjectFile::fromJsonString(
                docWith({{QStringLiteral("fps"), f.in}}, {}), r);
            check("fps" + std::to_string(f.in), p && r.config.fps == f.want, std::to_string(r.config.fps));
        }
        for (const int w : {3840, 8192}) {
            ProjectData r;
            const bool p = ProjectFile::fromJsonString(
                docWith({{QStringLiteral("width"), w}}, {}), r);
            check("width" + std::to_string(w), p && r.config.width == w, std::to_string(r.config.width));
        }
        ProjectData ctl;
        const bool pc = ProjectFile::fromJsonString(
            docWith({}, {QJsonObject{{QStringLiteral("speed"), 2.0}}, QJsonObject{{QStringLiteral("volume"), 3.5}},
                         QJsonObject{{QStringLiteral("volume"), 4.0}}}), ctl);
        const QVector<ClipInfo> cc = ctl.videoTracks.value(0);
        check("controls", pc && cc.size() == 3 && cc[0].speed == 2.0 && cc[1].volume == 3.5 && cc[2].volume == 4.0);
        tally.gate(8, ok, d);
    }
    {
        bool ok = true;
        std::string d;
        const QVector<QJsonObject> oneClip = {QJsonObject{}};
        struct VersionCase { const char *name; QJsonValue v; bool accept; bool newer; };
        const VersionCase cases[] = {
            {"3", QJsonValue(3), false, true},
            {"str", QJsonValue(QStringLiteral("x")), false, false},
            {"2.5", QJsonValue(2.5), false, false},
            {"0", QJsonValue(0), false, false},
            {"-1", QJsonValue(-1), false, false},
            {"2", QJsonValue(2), true, false},
            {"1", QJsonValue(1), true, false},
        };
        for (const VersionCase &c : cases) {
            const QString json = docWith({}, oneClip, c.v);
            ProjectData viaString;
            QString msgString;
            const bool s = ProjectFile::fromJsonString(json, viaString, &msgString);
            const QString path = tmp((std::string("version-") + c.name + ".veditor").c_str());
            QFile f(path);
            bool wrote = f.open(QIODevice::WriteOnly) && f.write(json.toUtf8()) > 0;
            f.close();
            ProjectData viaFile;
            QString msgFile;
            const bool l = wrote && ProjectFile::load(path, viaFile, &msgFile);
            const bool newerOk = !c.newer
                || (msgString.contains(QStringLiteral("新しい形式")) && msgFile.contains(QStringLiteral("新しい形式")));
            const bool reasonOk = c.accept ? (msgString.isEmpty() && msgFile.isEmpty())
                                           : (!msgString.isEmpty() && !msgFile.isEmpty());
            const bool one = wrote && s == c.accept && l == c.accept && newerOk && reasonOk;
            ok = ok && one;
            d += std::string(c.name) + ": string=" + b(s) + " load=" + b(l) + " reason=" + b(reasonOk && newerOk)
                + (one ? "" : " WRONG") + " ";
        }
        tally.gate(9, ok, d);
    }

    // ---- G10 / G11 (US-2110): adjustment layers are parked per sequence ----
    // A = the loaded root (2 video tracks, a grey V1), B = createSequence. HEAD keeps one
    // project-wide list, so L1 follows the switch to B and L2 follows the switch back.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        ProjectData data;
        data.config.width = kW;
        data.config.height = kH;
        data.config.fps = 30;
        data.config.explicitOutputResolution = true;
        data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{greyClip()}, QVector<ClipInfo>{}};
        const QString pathS = tmp("seq.veditor");
        const bool seeded = ProjectFile::save(pathS, data);
        MainWindow seqWindow;
        Timeline &tl = *seqWindow.m_timeline;
        QString err;
        const bool opened = seeded && seqWindow.openProjectFromPath(pathS, &err, false);
        const bool created = tl.createSequence(QStringLiteral("B"),
            QVector<QVector<ClipInfo>>{QVector<ClipInfo>{greyClip()}, QVector<ClipInfo>{}},
            QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}});
        const QString bId = tl.activeSequenceId();
        // A single-sequence project has no active id until createSequence enables the model.
        QString a;
        for (const TimelineSequence &sequence : tl.sequences())
            if (sequence.id != bId)
                a = sequence.id;
        const bool premise = opened && created && tl.sequences().size() == 2 && !a.isEmpty() && bId != a
            && tl.setActiveSequence(a)
            && tl.videoTrackCount() >= 2;
        const auto layersOf = [](const QVector<AdjustmentLayer> &v) {
            std::string out = "[";
            for (const AdjustmentLayer &l : v)
                out += (out.size() > 1 ? " " : "") + l.name.toStdString() + "#" + std::to_string(l.id);
            return out + "]";
        };
        const qint64 atUs = 1000000;
        const QSize outSize(kW, kH);

        // ---- G10: the switch parks A's layers, B renders without them, A gets them back ----
        AdjustmentLayer l1 = layer;
        l1.name = QStringLiteral("L1");
        l1.trackIndex = 1;
        l1.timelineStartUs = 0;
        l1.timelineEndUs = 2000000;
        l1.id = tl.addAdjustmentLayer(l1);
        // Sanity: L1 changes the grey frame of A (otherwise the B comparison is vacuous).
        const QImage aWith = tlrender::renderFrameAt(&tl, atUs, outSize);
        tl.setAdjustmentLayers({});
        const QImage aBare = tlrender::renderFrameAt(&tl, atUs, outSize);
        tl.setAdjustmentLayers({l1});
        const double aDiff = meanAbsDiff(aWith, aBare);
        const bool toB = tl.setActiveSequence(bId);
        const QVector<AdjustmentLayer> onB = tl.adjustmentLayers();
        const QImage bFrame = tlrender::renderFrameAt(&tl, atUs, outSize);
        tl.setAdjustmentLayers({});
        const QImage bBare = tlrender::renderFrameAt(&tl, atUs, outSize);
        tl.setAdjustmentLayers(onB);
        const bool bSame = !bFrame.isNull() && bFrame.format() == bBare.format() && bFrame == bBare;
        AdjustmentLayer l2 = layer;
        l2.name = QStringLiteral("L2");
        l2.trackIndex = 0;
        l2.lift[0] = 9.0;
        l2.id = tl.addAdjustmentLayer(l2);
        const bool toA = tl.setActiveSequence(a);
        std::string d10 = "premise=" + b(premise) + " A=" + a.toStdString() + " B=" + bId.toStdString()
            + " sanity meanAbsDiff(A+L1, A)=" + std::to_string(aDiff) + " toB=" + b(toB)
            + " live@B=" + layersOf(onB) + " render@B==bare=" + b(bSame) + " toA=" + b(toA) + " live@A";
        const bool backL1 = sameLayers(tl.adjustmentLayers(), {l1}, d10);
        const QVector<AdjustmentLayer> parkedB = tl.parkedSidecarsBySequence().value(bId).adjustments;
        const bool parkedBOk = parkedB.size() == 1 && parkedB[0].id == l2.id;
        d10 += " parked[B]=" + layersOf(parkedB) + " L1.id=" + std::to_string(l1.id)
            + " L2.id=" + std::to_string(l2.id);
        tally.gate(10, premise && aDiff > 1.0 && toB && onB.isEmpty() && bSame && toA && backL1
                           && parkedBOk && l2.id != l1.id, d10);

        // ---- G11: save / open, the old-file control, and undo across a switch ----
        std::string d11;
        const bool toB2 = tl.setActiveSequence(bId);
        const QString pathP = tmp("perseq.veditor");
        const bool saved = seqWindow.saveProjectToPath(pathP, &err);
        const QJsonObject root = readJson(pathP);
        QJsonObject seqA, seqB;
        for (const QJsonValue &v : root.value(QStringLiteral("sequences")).toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("id")).toString() == a) seqA = o;
            if (o.value(QStringLiteral("id")).toString() == bId) seqB = o;
        }
        d11 += "toB=" + b(toB2) + " saved=" + b(saved) + " root.activeSequenceId="
            + root.value(QStringLiteral("activeSequenceId")).toString().toStdString() + " root";
        const bool rootOk = sameLayers(adjustmentLayersFromJsonArray(
            root.value(QStringLiteral("adjustmentLayers")).toArray()), {l2}, d11);
        d11 += " | sequences[A]";
        const bool seqAOk = sameLayers(adjustmentLayersFromJsonArray(
            seqA.value(QStringLiteral("adjustmentLayers")).toArray()), {l1}, d11);
        const bool seqBNoKey = !seqB.isEmpty() && !seqB.contains(QStringLiteral("adjustmentLayers"));
        d11 += " | sequences[B] hasKey=" + b(!seqBNoKey);
        // Open after clearing both lists, so only the file can bring them back.
        tl.setAdjustmentLayers({});
        tl.setParkedSidecarsBySequence({});
        const bool reopened = seqWindow.openProjectFromPath(pathP, &err, false);
        const QString activeAfterOpen = tl.activeSequenceId();
        d11 += " | open=" + b(reopened) + " active=" + activeAfterOpen.toStdString() + " live";
        const bool openB = activeAfterOpen == bId && sameLayers(tl.adjustmentLayers(), {l2}, d11);
        const bool openToA = tl.setActiveSequence(a);
        d11 += " toA=" + b(openToA) + " live@A";
        const bool openA = sameLayers(tl.adjustmentLayers(), {l1}, d11);
        const int nextId = tl.addAdjustmentLayer(AdjustmentLayer{});
        const bool nextIdOk = nextId > std::max(l1.id, l2.id);
        d11 += " nextId=" + std::to_string(nextId);
        // Old file (no per-sequence key): the root list belongs to the active sequence only.
        QJsonObject oldRoot = root;
        QJsonArray oldSequences = oldRoot.value(QStringLiteral("sequences")).toArray();
        for (int i = 0; i < oldSequences.size(); ++i) {
            QJsonObject o = oldSequences[i].toObject();
            o.remove(QStringLiteral("adjustmentLayers"));
            oldSequences[i] = o;
        }
        oldRoot[QStringLiteral("sequences")] = oldSequences;
        const QString pathOld = tmp("perseq-old.veditor");
        QFile oldFile(pathOld);
        const bool oldWritten = oldFile.open(QIODevice::WriteOnly)
            && oldFile.write(QJsonDocument(oldRoot).toJson()) > 0;
        oldFile.close();
        const bool oldOpened = oldWritten && seqWindow.openProjectFromPath(pathOld, &err, false);
        d11 += " | old: open=" + b(oldOpened) + " active=" + tl.activeSequenceId().toStdString() + " live";
        const bool oldB = tl.activeSequenceId() == bId && sameLayers(tl.adjustmentLayers(), {l2}, d11);
        const bool oldToA = tl.setActiveSequence(a);
        const bool oldA = oldToA && tl.adjustmentLayers().isEmpty();
        d11 += " live@A=" + layersOf(tl.adjustmentLayers());
        // Undo across a switch (TimelineState carries no adjustment layers; the external
        // state does). Leg 1 is non-structural; leg 2 removes a track, so the external state
        // is applied inside restoreState before its parking runs and must not leak into B.
        const bool undoOpened = seqWindow.openProjectFromPath(pathP, &err, false);
        const bool undoToA = tl.setActiveSequence(a);
        const int tracksBase = tl.videoTrackCount();
        tl.saveUndoState(QStringLiteral("project-persist A base"));
        tl.addVideoTrack(true);
        AdjustmentLayer l3 = layer;
        l3.name = QStringLiteral("L3");
        l3.id = tl.addAdjustmentLayer(l3);
        tl.saveUndoState(QStringLiteral("project-persist L3"));
        const std::string liveBefore = layersOf(tl.adjustmentLayers());
        const bool undoToB = tl.setActiveSequence(bId);
        tl.undo();
        std::string du1 = " leg1: active=" + tl.activeSequenceId().toStdString() + " live";
        const bool leg1Live = tl.activeSequenceId() == a && sameLayers(tl.adjustmentLayers(), {l1}, du1);
        du1 += " parked[B]";
        const bool leg1B = sameLayers(tl.parkedSidecarsBySequence().value(bId).adjustments, {l2}, du1);
        const bool leg1NoA = !tl.parkedSidecarsBySequence().contains(a);
        const int tracksLeg1 = tl.videoTrackCount();
        // Navigation away and back keeps A following the redo state, so redo brings L3 back.
        const bool navToB = tl.setActiveSequence(bId);
        const bool navToA = tl.setActiveSequence(a);
        tl.redo();
        std::string dn = " | nav redo: active=" + tl.activeSequenceId().toStdString() + " live";
        const bool navRedoLive = tl.activeSequenceId() == a && sameLayers(tl.adjustmentLayers(), {l1, l3}, dn);
        dn += " parked[B]";
        const bool navRedoB = sameLayers(tl.parkedSidecarsBySequence().value(bId).adjustments, {l2}, dn);
        tl.undo();
        dn += " undo: live";
        const bool navUndoLive = tl.activeSequenceId() == a && sameLayers(tl.adjustmentLayers(), {l1}, dn);
        // A layer added in B with no undo point (the GUI add) must not reuse L3's id, which
        // the reachable redo state still holds; after the redo every id is distinct.
        const bool idToB = tl.setActiveSequence(bId);
        AdjustmentLayer l4 = layer;
        l4.name = QStringLiteral("L4");
        l4.id = tl.addAdjustmentLayer(l4);
        const bool idToA = tl.setActiveSequence(a);
        tl.redo();
        dn += " | L4 in B: id=" + std::to_string(l4.id) + " L3 id=" + std::to_string(l3.id) + " redo: live="
            + layersOf(tl.adjustmentLayers());
        QVector<int> allIds;
        for (const AdjustmentLayer &l : tl.adjustmentLayers())
            allIds.append(l.id);
        const auto parkedAfterRedo = tl.parkedSidecarsBySequence();
        for (auto it = parkedAfterRedo.cbegin(); it != parkedAfterRedo.cend(); ++it) {
            dn += " parked[" + it.key().toStdString() + "]=" + layersOf(it->adjustments);
            for (const AdjustmentLayer &l : it->adjustments)
                allIds.append(l.id);
        }
        bool idsDistinct = allIds.size() == 4;
        for (int i = 0; i < allIds.size(); ++i)
            for (int j = i + 1; j < allIds.size(); ++j)
                idsDistinct = idsDistinct && allIds[i] != allIds[j];
        dn += " ids=" + std::to_string(allIds.size()) + " distinct=" + b(idsDistinct);
        const bool idRedoLive = tl.activeSequenceId() == a && sameLayers(tl.adjustmentLayers(), {l1, l3}, dn);
        // Back to the leg-1 state for the legs below: undo, then drop L4 from B (no undo point).
        tl.undo();
        const bool idCleanup = tl.setActiveSequence(bId) && tl.removeAdjustmentLayer(l4.id)
            && tl.setActiveSequence(a) && sameLayers(tl.adjustmentLayers(), {l1}, dn);
        const bool navIdOk = navToB && navToA && navRedoLive && navRedoB && navUndoLive && idToB && idToA
            && l4.id > 0 && l4.id != l3.id && idsDistinct && idRedoLive && idCleanup;
        // Redo inside the landing sequence brings L3 back (undo + redo is the identity),
        // and undo again takes it away.
        tl.redo();
        du1 += " | redo: active=" + tl.activeSequenceId().toStdString() + " live";
        const bool redoLive = tl.activeSequenceId() == a && sameLayers(tl.adjustmentLayers(), {l1, l3}, du1);
        du1 += " parked[B]";
        const bool redoB = sameLayers(tl.parkedSidecarsBySequence().value(bId).adjustments, {l2}, du1);
        const bool redoNoA = !tl.parkedSidecarsBySequence().contains(a);
        tl.undo();
        du1 += " parkedHas(A)=" + b(!redoNoA) + " | undo again: active=" + tl.activeSequenceId().toStdString()
            + " live";
        const bool reUndoLive = tl.activeSequenceId() == a && sameLayers(tl.adjustmentLayers(), {l1}, du1);
        const bool undoToB2 = tl.setActiveSequence(bId);
        tl.undo();
        const int tracksAfter = tl.videoTrackCount();
        std::string du2 = " leg2: active=" + tl.activeSequenceId().toStdString() + " tracks "
            + std::to_string(tracksLeg1) + "->" + std::to_string(tracksAfter) + " (base "
            + std::to_string(tracksBase) + ") live";
        const bool leg2Live = tl.activeSequenceId() == a && sameLayers(tl.adjustmentLayers(), {l1}, du2);
        du2 += " parked[B]";
        const bool leg2B = sameLayers(tl.parkedSidecarsBySequence().value(bId).adjustments, {l2}, du2);
        d11 += " | undo: open=" + b(undoOpened) + " toA=" + b(undoToA) + " live+L3=" + liveBefore
            + " toB=" + b(undoToB) + "/" + b(undoToB2) + du1 + " parkedHas(A)=" + b(!leg1NoA) + dn + du2;
        const bool undoOk = undoOpened && undoToA && undoToB && undoToB2 && leg1Live && leg1B && leg1NoA
            && navIdOk && redoLive && redoB && redoNoA && reUndoLive
            && tracksLeg1 == tracksBase + 1 && tracksAfter == tracksBase && leg2Live && leg2B;

        // Undo of the first sequence creation (non-structural: the same track counts on both
        // sides, the timeline keeps 2 video tracks) gives the original timeline its layers
        // back; redo parks them under the default id again.
        ProjectData oneData = data;
        oneData.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{greyClip()}};
        oneData.audioTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{}};
        const QString pathOne = tmp("first-create.veditor");
        const bool oneOpened = ProjectFile::save(pathOne, oneData)
            && seqWindow.openProjectFromPath(pathOne, &err, false);
        const bool oneModelOff = tl.sequences().isEmpty();
        const std::string tracksOne = std::to_string(tl.videoTrackCount()) + "V/"
            + std::to_string(tl.audioTrackCount()) + "A";
        AdjustmentLayer c1 = layer;
        c1.name = QStringLiteral("L1");
        c1.id = tl.addAdjustmentLayer(c1);
        const bool firstCreated = tl.createSequence(QStringLiteral("B"));
        const QString firstB = tl.activeSequenceId();
        const std::string tracksCreated = std::to_string(tl.videoTrackCount()) + "V/"
            + std::to_string(tl.audioTrackCount()) + "A";
        tl.undo();
        const std::string tracksUndone = std::to_string(tl.videoTrackCount()) + "V/"
            + std::to_string(tl.audioTrackCount()) + "A";
        const bool undoneOff = tl.sequences().isEmpty()
            && (tl.activeSequenceId().isEmpty() || tl.activeSequenceId() == QLatin1String("main"));
        std::string dc = " | first creation: open=" + b(oneOpened) + " modelOff=" + b(oneModelOff)
            + " created=" + b(firstCreated) + " B=" + firstB.toStdString() + " tracks " + tracksOne + "->"
            + tracksCreated + "->" + tracksUndone + " undo: sequences=" + std::to_string(tl.sequences().size())
            + " active='" + tl.activeSequenceId().toStdString() + "' live";
        const bool undoneLive = sameLayers(tl.adjustmentLayers(), {c1}, dc);
        tl.redo();
        dc += " redo: active=" + tl.activeSequenceId().toStdString() + " live=" + layersOf(tl.adjustmentLayers())
            + " parked[main]";
        const bool redoneOk = tl.activeSequenceId() == firstB && !firstB.isEmpty() && tl.adjustmentLayers().isEmpty()
            && sameLayers(tl.parkedSidecarsBySequence().value(QStringLiteral("main")).adjustments, {c1}, dc);
        d11 += dc;
        const bool firstOk = oneOpened && oneModelOff && firstCreated && tracksCreated == tracksOne
            && tracksUndone == tracksOne && undoneOff && undoneLive && redoneOk;
        tally.gate(11, premise && toB2 && saved && rootOk && seqAOk && seqBNoKey && reopened && openB
                           && openToA && openA && nextIdOk && oldOpened && oldB && oldA && undoOk && firstOk,
                   d11);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    tally.gate(7, g7, g7Detail);
    return tally.finish("project-persist");
}
