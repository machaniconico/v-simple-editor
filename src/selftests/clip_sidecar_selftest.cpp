// US-1706 (clip-sidecar): the 3D text settings (ClipInfo::text3DConfig) and the brush
// animation (ClipInfo::brushData) live on the clip instead of MainWindow's "track:clip"
// keyed sidecars.
//   G1 the two clip JSON keys ("text3D" / "brush") round-trip through toJsonString /
//      fromJsonString and save / load. Detects: a key not written or not read back
//      (mutation build: clipToJson writes neither; HEAD has no fields).
//   G2 a legacy project with only the root sections "text3DClipEntries" /
//      "brushAnimations" ("0:1" / "0:0") moves each onto the clip it names, through both
//      ProjectFile::load and fromJsonString; the other clip gets none. Detects: no
//      migration (mutation build: migrateLegacyClipSidecars skips the two sections).
//   G3 save / toJsonString write neither root section (clip data set, and a migrated
//      legacy project re-saved). Detects: the root sections written again.
//   G4 after a clip delete, a track move and a track removal the values stay on the same
//      clip (by displayName) and the clip without values has none. Detects: values left at
//      the old position (the position-keyed rekey the clips no longer need).
//   G5 a split puts neither on the right half (the left keeps both) through
//      splitClipByIndex, bladeAllTracksAtPlayhead and splitAtPlayhead. Detects: the right
//      half inheriting them (mutation build: the secondHalf reset in splitClipAt removed).
//   G6 empty values write no "text3D" / "brush" key (the clip with values in the same
//      project does). Detects: empty keys written.
//   G7 setClipText3DConfig / setClipBrushData: one undo point per change (undo empties,
//      redo restores), none for an equal value (true), false for a locked track or a
//      missing clip. Detects: no / two undo points, or a locked track edited.
//   G8 render in place bakes the brush into the material: the files baked from a clip with
//      an animated / constant brush_progress differ from the plain bake (inside the stroke
//      box) where the progress is above 0 and not where it is 0; the replacement has no
//      brushData and no brush_progress track and keeps the 3D text live; brush bakes take
//      the ProRes 4444 path (.mov) from a default h264 request, a clip without a brush
//      keeps h264 (.mp4). The stroke-free reference is the brush at progress 0 (brush-export
//      pins that it draws nothing), so it shares the brush bakes' ProRes 4444 path and the
//      comparison does not measure one codec against another. Detects: the brush kept live on the replacement
//      and left out of the material (HEAD), or a brush bake left on the requested codec.
//   G9 duplicate and copy / paste carry both to the new clip. Detects: the clone dropping
//      them (mutation build: copyClipForPaste clears them).
//   G10 a clip graded with ColorCorrection.exposure = -0.5 and a brush renders the same
//      (renderFrameAt, stroke box) before and after render in place. Detects: the live
//      brush drawn on the replacement without the grade (HEAD, Step 0: 46.18), or the
//      strokes baked through a 4:2:0 / 4:2:2 codec (mutation build: 5.68).
//   G11 the G10 comparison with handles 0.5 s + speed 2 and with a reversed clip (animated
//      brush_progress, so the handle shift of the keys is visible; 0.3 / 0.7 s lie on the
//      30 fps bake grid, between its frames the file holds while the live brush grows),
//      and decompose restores
//      the brushData and the brush_progress keys. Detects: as G10 (HEAD reversed 46.18),
//      or progress keys not shifted with the handles.
#include "../BrushAnimation.h"
#include "../ProjectFile.h"
#include "../RenderInPlace.h"
#include "../Text3DLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "../libavcore/Probe.h"
#include "SelftestGate.h"

#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace {

ClipInfo sidecarClip(const QString &name, double duration)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("/selftests/clip-sidecar-%1.mp4").arg(name);
    clip.displayName = name;
    clip.duration = duration;
    clip.outPoint = duration;
    return clip;
}

void loadTimeline(Timeline &timeline, const QVector<QVector<ClipInfo>> &video)
{
    timeline.restoreFromProject(video, QVector<QVector<ClipInfo>>{{}}, 0.0, -1.0, -1.0, 100);
    timeline.undoManager()->clear();
    timeline.saveUndoState(QStringLiteral("Initial"));
}

QJsonObject text3DFor(const QString &text)
{
    Text3DLayer layer;
    layer.setText(text, QFont(QStringLiteral("Arial"), 48));
    return layer.toJson();
}

QJsonObject brushFor(const QString &text)
{
    BrushAnimation brush;
    brush.setText(text, QFont(QStringLiteral("Arial"), 48), QPointF(40.0, 120.0));
    brush.setBrushWidth(6.0);
    return brush.toJson();
}

QString textOf(const QJsonObject &obj)
{
    return obj.value(QStringLiteral("text")).toString();
}

QJsonObject rootOf(const QString &json)
{
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

QJsonObject firstClipJson(const QJsonObject &root, int clipIndex)
{
    return root.value(QStringLiteral("videoTracks")).toArray().at(0).toArray().at(clipIndex).toObject();
}

bool hasRootSections(const QJsonObject &root)
{
    return root.contains(QStringLiteral("text3DClipEntries"))
        || root.contains(QStringLiteral("brushAnimations"));
}

QString readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// "name:t3/br" per V1 clip, "-" for a missing value.
QString describe(const TimelineTrack *track)
{
    QStringList parts;
    if (!track) return QStringLiteral("<no track>");
    for (const ClipInfo &clip : track->clips()) {
        parts.append(clip.displayName + QLatin1Char(':')
                     + (clip.text3DConfig.isEmpty() ? QStringLiteral("-") : textOf(clip.text3DConfig))
                     + QLatin1Char('/')
                     + (clip.brushData.isEmpty() ? QStringLiteral("-") : textOf(clip.brushData)));
    }
    return parts.join(QLatin1Char(','));
}

// Mean absolute RGB difference over box (the whole image when box is empty); -1 when the
// images cannot be compared.
double meanAbsDiff(const QImage &a, const QImage &b, const QRect &box = QRect())
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) return -1.0;
    const QRect area = box.isEmpty() ? a.rect() : box.intersected(a.rect());
    if (area.isEmpty()) return -1.0;
    const QImage l = a.convertToFormat(QImage::Format_RGB888);
    const QImage r = b.convertToFormat(QImage::Format_RGB888);
    double sum = 0.0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        const uchar *pl = l.constScanLine(y);
        const uchar *pr = r.constScanLine(y);
        for (int x = area.left() * 3; x < (area.right() + 1) * 3; ++x) sum += std::abs(int(pl[x]) - int(pr[x]));
    }
    return sum / (double(area.width()) * area.height() * 3.0);
}

// Bounding box of the pixels where a and b differ by more than 8 in a channel.
QRect changedBox(const QImage &a, const QImage &b)
{
    QRect box;
    if (a.isNull() || b.isNull() || a.size() != b.size()) return box;
    const QImage l = a.convertToFormat(QImage::Format_RGB888);
    const QImage r = b.convertToFormat(QImage::Format_RGB888);
    for (int y = 0; y < l.height(); ++y) {
        const uchar *pl = l.constScanLine(y);
        const uchar *pr = r.constScanLine(y);
        for (int x = 0; x < l.width(); ++x) {
            for (int c = 0; c < 3; ++c) {
                if (std::abs(int(pl[x * 3 + c]) - int(pr[x * 3 + c])) > 8) {
                    box = box.united(QRect(x, y, 1, 1));
                    break;
                }
            }
        }
    }
    return box;
}

// The brush_progress keys (time and value) of both clips are the same.
bool sameProgressKeys(const ClipInfo &before, const ClipInfo &after)
{
    const KeyframeTrack *a = before.keyframes.track(QStringLiteral("brush_progress"));
    const KeyframeTrack *b = after.keyframes.track(QStringLiteral("brush_progress"));
    if (!a || !b || a->keyframes().size() != b->keyframes().size()) return false;
    for (int i = 0; i < a->keyframes().size(); ++i) {
        if (a->keyframes()[i].time != b->keyframes()[i].time
            || a->keyframes()[i].value != b->keyframes()[i].value)
            return false;
    }
    return true;
}

qint64 microsAt(double seconds)
{
    return qRound64(seconds * 1000000.0);
}

Timeline *loadSingleClip(Timeline &timeline, const ClipInfo &clip)
{
    timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, QVector<QVector<ClipInfo>>{}, 0, -1, -1, 10);
    timeline.undoManager()->clear();
    timeline.saveUndoState(QStringLiteral("Initial"));
    return &timeline;
}

} // namespace

int runClipSidecarSelftest()
{
    selftest::GateTally tally;
    const QJsonObject t3A = text3DFor(QStringLiteral("T3A"));
    const QJsonObject brA = brushFor(QStringLiteral("BRA"));
    const QJsonObject t3B = text3DFor(QStringLiteral("T3B"));
    const QJsonObject brB = brushFor(QStringLiteral("BRB"));
    QTemporaryDir tempDir;

    // G1: the clip JSON keys round-trip.
    {
        ProjectData data;
        data.config.width = 640;
        data.config.height = 360;
        data.config.fps = 30;
        ClipInfo a = sidecarClip(QStringLiteral("a"), 4.0);
        a.text3DConfig = t3A;
        a.brushData = brA;
        data.videoTracks = {{a, sidecarClip(QStringLiteral("b"), 2.0)}};
        data.audioTracks = {{}};
        const QString json = ProjectFile::toJsonString(data);
        const QJsonObject clipJson = firstClipJson(rootOf(json), 0);
        ProjectData loaded;
        const bool parsed = ProjectFile::fromJsonString(json, loaded);
        const ClipInfo back = parsed && !loaded.videoTracks.isEmpty() && !loaded.videoTracks[0].isEmpty()
            ? loaded.videoTracks[0][0] : ClipInfo{};
        const QString path = tempDir.filePath(QStringLiteral("g1.veditor"));
        ProjectData fileLoaded;
        const bool fileOk = tempDir.isValid() && ProjectFile::save(path, data)
            && ProjectFile::load(path, fileLoaded) && !fileLoaded.videoTracks.isEmpty()
            && fileLoaded.videoTracks[0].size() == 2;
        const ClipInfo fileBack = fileOk ? fileLoaded.videoTracks[0][0] : ClipInfo{};
        const bool ok = clipJson.value(QStringLiteral("text3D")).toObject() == t3A
            && clipJson.value(QStringLiteral("brush")).toObject() == brA
            && back.text3DConfig == t3A && back.brushData == brA
            && fileBack.text3DConfig == t3A && fileBack.brushData == brA;
        tally.gate(1, ok, QString::asprintf(
            "keys text3D=%d brush=%d string=[%s/%s] file(%d)=[%s/%s] (expect T3A/BRA)",
            int(clipJson.contains(QStringLiteral("text3D"))), int(clipJson.contains(QStringLiteral("brush"))),
            qPrintable(textOf(back.text3DConfig)), qPrintable(textOf(back.brushData)), int(fileOk),
            qPrintable(textOf(fileBack.text3DConfig)), qPrintable(textOf(fileBack.brushData))).toStdString());
    }

    // G2 / G3: a legacy project with only the two root sections.
    {
        // The base is a current-format project written by toJsonString (no clip keys, the
        // two clips have no values); the root sections are then added by hand.
        ProjectData legacyData;
        legacyData.config.width = 640;
        legacyData.config.height = 360;
        legacyData.config.fps = 30;
        legacyData.videoTracks = {{sidecarClip(QStringLiteral("c0"), 2.0), sidecarClip(QStringLiteral("c1"), 2.0)}};
        legacyData.audioTracks = {{}};
        QJsonObject legacyRoot = rootOf(ProjectFile::toJsonString(legacyData));
        legacyRoot.insert(QStringLiteral("text3DClipEntries"),
                          QJsonArray{QJsonObject{{QStringLiteral("clipId"), QStringLiteral("0:1")},
                                                 {QStringLiteral("config"), t3A}}});
        legacyRoot.insert(QStringLiteral("brushAnimations"),
                          QJsonArray{QJsonObject{{QStringLiteral("clipId"), QStringLiteral("0:0")},
                                                 {QStringLiteral("brushData"), brA}}});
        const QString legacyJson = QString::fromUtf8(QJsonDocument(legacyRoot).toJson(QJsonDocument::Compact));
        const auto migrated = [&](const ProjectData &data) {
            return data.text3DClipEntries.isEmpty() && data.brushAnimations.isEmpty()
                && data.videoTracks.size() == 1 && data.videoTracks[0].size() == 2
                && data.videoTracks[0][0].brushData == brA && data.videoTracks[0][0].text3DConfig.isEmpty()
                && data.videoTracks[0][1].text3DConfig == t3A && data.videoTracks[0][1].brushData.isEmpty();
        };
        const auto shown = [](const ProjectData &data) {
            QStringList parts;
            for (const ClipInfo &clip : data.videoTracks.value(0))
                parts.append(clip.displayName + QLatin1Char(':') + textOf(clip.text3DConfig)
                             + QLatin1Char('/') + textOf(clip.brushData));
            return parts.join(QLatin1Char(','));
        };
        ProjectData fromString;
        const bool stringOk = ProjectFile::fromJsonString(legacyJson, fromString);
        const QString legacyPath = tempDir.filePath(QStringLiteral("g2-legacy.veditor"));
        {
            QFile file(legacyPath);
            if (file.open(QIODevice::WriteOnly)) file.write(legacyJson.toUtf8());
        }
        ProjectData fromFile;
        const bool fileOk = ProjectFile::load(legacyPath, fromFile);
        const bool ok2 = stringOk && fileOk && migrated(fromString) && migrated(fromFile);
        tally.gate(2, ok2, QString::asprintf(
            "fromJsonString(%d)=[%s] load(%d)=[%s] (expect c0:/BRA,c1:T3A/)", int(stringOk),
            qPrintable(shown(fromString)), int(fileOk), qPrintable(shown(fromFile))).toStdString());

        // G3: neither writer emits the root sections, for clip data set directly and for
        // the migrated legacy project; the clip keys carry the values instead.
        ProjectData direct;
        direct.config.width = 640;
        direct.config.height = 360;
        direct.config.fps = 30;
        ClipInfo a = sidecarClip(QStringLiteral("a"), 2.0);
        a.text3DConfig = t3A;
        a.brushData = brA;
        direct.videoTracks = {{a}};
        direct.audioTracks = {{}};
        const QJsonObject directString = rootOf(ProjectFile::toJsonString(direct));
        const QJsonObject resavedString = rootOf(ProjectFile::toJsonString(fromString));
        const QString directPath = tempDir.filePath(QStringLiteral("g3-direct.veditor"));
        const QString resavedPath = tempDir.filePath(QStringLiteral("g3-resaved.veditor"));
        const bool saved = ProjectFile::save(directPath, direct) && ProjectFile::save(resavedPath, fromFile);
        const QJsonObject directFile = rootOf(readFile(directPath));
        const QJsonObject resavedFile = rootOf(readFile(resavedPath));
        const bool clipKeys = firstClipJson(directString, 0).contains(QStringLiteral("text3D"))
            && firstClipJson(resavedFile, 1).value(QStringLiteral("text3D")).toObject() == t3A
            && firstClipJson(resavedFile, 0).value(QStringLiteral("brush")).toObject() == brA;
        const bool ok3 = saved && !directString.isEmpty() && !resavedFile.isEmpty()
            && !hasRootSections(directString) && !hasRootSections(resavedString)
            && !hasRootSections(directFile) && !hasRootSections(resavedFile) && clipKeys;
        tally.gate(3, ok3, QString::asprintf(
            "saved=%d rootSections toJsonString direct=%d resaved=%d save direct=%d resaved=%d (expect 0) "
            "clipKeys=%d", int(saved), int(hasRootSections(directString)), int(hasRootSections(resavedString)),
            int(hasRootSections(directFile)), int(hasRootSections(resavedFile)), int(clipKeys)).toStdString());
    }

    // G4: track-ops keep the values on their clip.
    {
        Timeline tl;
        ClipInfo b = sidecarClip(QStringLiteral("b"), 2.0);
        b.text3DConfig = t3B;
        b.brushData = brB;
        loadTimeline(tl, {{sidecarClip(QStringLiteral("x"), 6.0)},
                          {sidecarClip(QStringLiteral("a"), 2.0), b},
                          {sidecarClip(QStringLiteral("c"), 2.0)}});
        // (i) delete a (before b on V2): b becomes "1:0".
        QString err;
        const bool deleted = tl.deleteClipByIndex(false, 1, 0, false, &err);
        const QString afterDelete = describe(tl.trackAt(false, 1));
        // (ii) move V2 to the top (index 2): b's track is now V3.
        const bool moved = tl.moveTrack(false, 1, 2, &err);
        const QString afterMoveV2 = describe(tl.trackAt(false, 1));
        const QString afterMoveV3 = describe(tl.trackAt(false, 2));
        // (iii) remove the track now at V2 (c's): b's track becomes V2 again.
        const bool removed = tl.removeTrack(false, 1, &err);
        const QString afterRemove = describe(tl.trackAt(false, 1));
        const QString x = describe(tl.trackAt(false, 0));
        const bool ok = deleted && afterDelete == QStringLiteral("b:T3B/BRB")
            && moved && afterMoveV2 == QStringLiteral("c:-/-") && afterMoveV3 == QStringLiteral("b:T3B/BRB")
            && removed && afterRemove == QStringLiteral("b:T3B/BRB") && x == QStringLiteral("x:-/-");
        tally.gate(4, ok, QString::asprintf(
            "delete(%d)=[%s] move(%d) V2=[%s] V3=[%s] remove(%d) V2=[%s] V1=[%s] (expect b:T3B/BRB, c and x "
            "none)", int(deleted), qPrintable(afterDelete), int(moved), qPrintable(afterMoveV2),
            qPrintable(afterMoveV3), int(removed), qPrintable(afterRemove), qPrintable(x)).toStdString());
    }

    // G5: the right half of a split carries neither, on all three split paths.
    {
        ClipInfo s = sidecarClip(QStringLiteral("s"), 4.0);
        s.text3DConfig = t3A;
        s.brushData = brA;
        const QString expected = QStringLiteral("s:T3A/BRA,s:-/-");
        QString err;
        Timeline byIndex;
        loadTimeline(byIndex, {{s}});
        const bool indexSplit = byIndex.splitClipByIndex(false, 0, 0, 1.5, &err);
        const QString indexShown = describe(byIndex.trackAt(false, 0));
        Timeline blade;
        loadTimeline(blade, {{s}});
        blade.setPlayheadPosition(1.5);
        blade.bladeAllTracksAtPlayhead();
        const QString bladeShown = describe(blade.trackAt(false, 0));
        Timeline atPlayhead;
        loadTimeline(atPlayhead, {{s}});
        const bool selected = atPlayhead.selectClipByIndex(false, 0, 0, &err);
        atPlayhead.setPlayheadPosition(1.5);
        atPlayhead.splitAtPlayhead();
        const QString playheadShown = describe(atPlayhead.trackAt(false, 0));
        const bool ok = indexSplit && indexShown == expected && bladeShown == expected
            && selected && playheadShown == expected;
        tally.gate(5, ok, QString::asprintf(
            "splitClipByIndex(%d)=[%s] bladeAllTracksAtPlayhead=[%s] splitAtPlayhead(sel %d)=[%s] "
            "(expect %s)", int(indexSplit), qPrintable(indexShown), qPrintable(bladeShown), int(selected),
            qPrintable(playheadShown), qPrintable(expected)).toStdString());
    }

    // G6: empty values write no key; the clip with values in the same project does.
    {
        ProjectData data;
        data.config.width = 640;
        data.config.height = 360;
        data.config.fps = 30;
        ClipInfo full = sidecarClip(QStringLiteral("full"), 2.0);
        full.text3DConfig = t3A;
        full.brushData = brA;
        data.videoTracks = {{sidecarClip(QStringLiteral("empty"), 2.0), full}};
        data.audioTracks = {{}};
        const QJsonObject root = rootOf(ProjectFile::toJsonString(data));
        const QJsonObject emptyJson = firstClipJson(root, 0);
        const QJsonObject fullJson = firstClipJson(root, 1);
        const bool ok = !emptyJson.isEmpty()
            && !emptyJson.contains(QStringLiteral("text3D")) && !emptyJson.contains(QStringLiteral("brush"))
            && fullJson.contains(QStringLiteral("text3D")) && fullJson.contains(QStringLiteral("brush"));
        tally.gate(6, ok, QString::asprintf(
            "empty text3D=%d brush=%d (expect 0) full text3D=%d brush=%d (expect 1)",
            int(emptyJson.contains(QStringLiteral("text3D"))), int(emptyJson.contains(QStringLiteral("brush"))),
            int(fullJson.contains(QStringLiteral("text3D"))), int(fullJson.contains(QStringLiteral("brush"))))
            .toStdString());
    }

    // G7: the setters' undo rule.
    {
        Timeline tl;
        loadTimeline(tl, {{sidecarClip(QStringLiteral("a"), 2.0)}, {sidecarClip(QStringLiteral("v2"), 2.0)}});
        UndoManager *undo = tl.undoManager();
        const int base = undo->currentIndex();
        const bool setT3 = tl.setClipText3DConfig(0, 0, t3A);
        const int afterT3 = undo->currentIndex() - base;
        const bool sameT3 = tl.setClipText3DConfig(0, 0, t3A);
        const int afterSameT3 = undo->currentIndex() - base;
        const bool setBr = tl.setClipBrushData(0, 0, brA);
        const int afterBr = undo->currentIndex() - base;
        const bool sameBr = tl.setClipBrushData(0, 0, brA);
        const int afterSameBr = undo->currentIndex() - base;
        tl.undo();
        const QString afterUndo1 = describe(tl.trackAt(false, 0));
        tl.undo();
        const QString afterUndo2 = describe(tl.trackAt(false, 0));
        tl.redo();
        tl.redo();
        const QString afterRedo = describe(tl.trackAt(false, 0));
        const bool missingT3 = tl.setClipText3DConfig(0, 5, t3B);
        const bool missingBr = tl.setClipBrushData(7, 0, brB);
        tl.trackAt(false, 1)->setLocked(true);
        const int beforeLocked = undo->currentIndex();
        const bool lockedT3 = tl.setClipText3DConfig(1, 0, t3B);
        const bool lockedBr = tl.setClipBrushData(1, 0, brB);
        const QString lockedShown = describe(tl.trackAt(false, 1));
        const int lockedDepth = undo->currentIndex() - beforeLocked;
        const bool ok = setT3 && afterT3 == 1 && sameT3 && afterSameT3 == 1 && setBr && afterBr == 2
            && sameBr && afterSameBr == 2 && afterUndo1 == QStringLiteral("a:T3A/-")
            && afterUndo2 == QStringLiteral("a:-/-") && afterRedo == QStringLiteral("a:T3A/BRA")
            && !missingT3 && !missingBr && !lockedT3 && !lockedBr
            && lockedShown == QStringLiteral("v2:-/-") && lockedDepth == 0;
        tally.gate(7, ok, QString::asprintf(
            "undo +%d (same +%d) / +%d (same +%d) (expect 1 1 2 2) undo1=[%s] undo2=[%s] redo=[%s] "
            "missing=%d/%d locked=%d/%d [%s] +%d (expect 0)", afterT3, afterSameT3, afterBr, afterSameBr,
            qPrintable(afterUndo1), qPrintable(afterUndo2), qPrintable(afterRedo), int(missingT3),
            int(missingBr), int(lockedT3), int(lockedBr), qPrintable(lockedShown), lockedDepth).toStdString());
    }

    // G8: render in place bakes the brush into the material and leaves the replacement
    // without it; the 3D text stays live on the replacement.
    {
        ClipInfo plain{};
        plain.filePath = QFileInfo(QStringLiteral("test_assets/e2e_clip.mp4")).absoluteFilePath();
        plain.displayName = QStringLiteral("sidecar-bake");
        const auto sourceDuration = libavcore::probeDurationMicroseconds(plain.filePath.toStdString());
        plain.duration = sourceDuration ? double(*sourceDuration) / 1000000.0 : 0.0;
        if (!tempDir.isValid() || !QFileInfo::exists(plain.filePath) || plain.duration < 2.0) {
            tally.gate(8, false, QString::asprintf(
                "prerequisite: fixture=%s exists=%d duration=%.3f (run from the repository root)",
                qPrintable(plain.filePath), int(QFileInfo::exists(plain.filePath)), plain.duration).toStdString());
        } else {
            plain.inPoint = 0.5;
            plain.outPoint = 2.0;
            ClipInfo carrying = plain;
            carrying.text3DConfig = t3A;
            carrying.brushData = brA;
            // The brush draws only where brush_progress is above 0: an animated track (0 up
            // to local 0.5 s) and a constant one.
            KeyframeTrack animatedProgress(QStringLiteral("brush_progress"));
            animatedProgress.addKeyframe(0.5, 0.0);
            animatedProgress.addKeyframe(1.5, 1.0);
            carrying.keyframes.addTrack(animatedProgress);
            ClipInfo constant = carrying;
            KeyframeTrack constantProgress(QStringLiteral("brush_progress"));
            constantProgress.addKeyframe(0.5, 0.6);
            constant.keyframes.removeTrack(QStringLiteral("brush_progress"));
            constant.keyframes.addTrack(constantProgress);
            renderinplace::Options options;
            options.outputSize = QSize(640, 360);
            options.fps = 30;
            options.handlesSec = 0.0;
            auto bake = [&](const ClipInfo &clip, const QString &dirName, ClipInfo *after, QString *bakedPath,
                            QString *error) {
                Timeline tl;
                loadSingleClip(tl, clip);
                renderinplace::Options local = options;
                local.outputDir = tempDir.filePath(dirName);
                const bool baked = renderinplace::renderClipInPlace(tl, 0, 0, local, bakedPath, error);
                *after = baked ? tl.videoTracks()[0]->clips()[0] : ClipInfo{};
                return baked;
            };
            ClipInfo carriedAfter, plainAfter, constantAfter;
            QString carriedPath, plainPath, constantPath, carriedError, plainError, constantError;
            const bool carriedBaked = bake(carrying, QStringLiteral("carrying"), &carriedAfter, &carriedPath,
                                           &carriedError);
            // Stroke-free picture through the brush bakes' codec path: the brush at progress 0.
            ClipInfo strokeFree = plain;
            strokeFree.brushData = brA;
            KeyframeTrack zeroProgress(QStringLiteral("brush_progress"));
            zeroProgress.addKeyframe(0.0, 0.0);
            strokeFree.keyframes.addTrack(zeroProgress);
            const bool plainBaked = bake(strokeFree, QStringLiteral("plain"), &plainAfter, &plainPath, &plainError);
            ClipInfo keptAfter;
            QString keptPath, keptError;
            const bool keptBaked = bake(plain, QStringLiteral("plain-h264"), &keptAfter, &keptPath, &keptError);
            const bool constantBaked = bake(constant, QStringLiteral("constant"), &constantAfter, &constantPath,
                                            &constantError);
            // The stroke box: the unbaked clip with the brush at full progress against the
            // plain clip.
            ClipInfo full = plain;
            full.brushData = brA;
            KeyframeTrack fullProgress(QStringLiteral("brush_progress"));
            fullProgress.addKeyframe(0.0, 1.0);
            full.keyframes.addTrack(fullProgress);
            QRect box;
            {
                Timeline withBrush, without;
                box = changedBox(tlrender::renderFrameAt(loadSingleClip(withBrush, full), microsAt(0.75),
                                                         options.outputSize),
                                 tlrender::renderFrameAt(loadSingleClip(without, plain), microsAt(0.75),
                                                         options.outputSize));
            }
            const bool replaced = carriedBaked && constantBaked && carriedAfter.filePath != carrying.filePath
                && carriedAfter.renderInPlaceOriginal && constantAfter.renderInPlaceOriginal;
            const auto brushFree = [](const ClipInfo &clip) {
                return clip.brushData.isEmpty() && !clip.keyframes.hasTrack(QStringLiteral("brush_progress"));
            };
            const bool dropped = replaced && brushFree(carriedAfter) && brushFree(constantAfter);
            const bool textKept = carriedAfter.text3DConfig == t3A && constantAfter.text3DConfig == t3A;
            // The baked files alone (no sidecars on either), inside the stroke box.
            const auto bakedOnly = [&](const QString &path, double seconds) {
                ClipInfo clip{};
                clip.filePath = path;
                clip.duration = plain.effectiveDuration();
                Timeline tl;
                return tlrender::renderFrameAt(loadSingleClip(tl, clip), microsAt(seconds), options.outputSize);
            };
            const bool codecsOk = carriedPath.endsWith(QStringLiteral(".mov"))
                && constantPath.endsWith(QStringLiteral(".mov")) && keptBaked
                && keptPath.endsWith(QStringLiteral(".mp4"));
            const bool framesOk = carriedBaked && plainBaked && constantBaked && !box.isEmpty();
            const auto bakedDiff = [&](const QString &path, double seconds) {
                return framesOk ? meanAbsDiff(bakedOnly(path, seconds), bakedOnly(plainPath, seconds), box) : -1.0;
            };
            const double atZero = bakedDiff(carriedPath, 0.1);
            const double early = bakedDiff(carriedPath, 0.75);
            const double late = bakedDiff(carriedPath, 1.25);
            const double constantDiff = bakedDiff(constantPath, 0.1);
            const bool picture = framesOk && atZero >= 0.0 && atZero <= 0.5 && early > 2.0
                && late > 2.0 && constantDiff > 2.0;
            tally.gate(8, dropped && textKept && picture && codecsOk, QString::asprintf(
                "baked=%d/%d/%d [%s|%s|%s] replaced=%d codecs ok=%d (brush .mov, no brush h264 .mp4) [%s] replacement brushData carrying=%d constant=%d "
                "brush_progress carrying=%d constant=%d (expect 0) text3D=[%s/%s] (expect T3A) box=(%d,%d %dx%d) "
                "baked carrying vs plain meanAbsDiff 0.1s(progress 0)=%.4f (expect <= 0.5) 0.75s=%.4f 1.25s=%.4f "
                "constant 0.1s=%.4f (expect > 2.0)", int(carriedBaked), int(plainBaked), int(constantBaked),
                qPrintable(carriedError), qPrintable(plainError), qPrintable(constantError), int(replaced), int(codecsOk), qPrintable(keptError),
                int(!carriedAfter.brushData.isEmpty()), int(!constantAfter.brushData.isEmpty()),
                int(carriedAfter.keyframes.hasTrack(QStringLiteral("brush_progress"))),
                int(constantAfter.keyframes.hasTrack(QStringLiteral("brush_progress"))),
                qPrintable(textOf(carriedAfter.text3DConfig)), qPrintable(textOf(constantAfter.text3DConfig)),
                box.x(), box.y(), box.width(), box.height(), atZero, early, late, constantDiff).toStdString());
        }
    }

    // G9: duplicate and copy / paste carry both to the new clip; the clone of a clip
    // without them has none.
    {
        Timeline tl;
        ClipInfo a = sidecarClip(QStringLiteral("a"), 2.0);
        a.text3DConfig = t3A;
        a.brushData = brA;
        loadTimeline(tl, {{a, sidecarClip(QStringLiteral("n"), 2.0)}});
        TimelineTrack *v1 = tl.trackAt(false, 0);
        v1->setSelectedClip(0);
        tl.copySelectedClip();
        tl.pasteClip();
        const QString afterPaste = describe(v1);
        int carriedClones = 0;
        const QString sourceUid = v1->clips().value(0).uid;
        for (const ClipInfo &clip : v1->clips())
            if (clip.displayName == QStringLiteral("a") && clip.uid != sourceUid
                && clip.text3DConfig == t3A && clip.brushData == brA)
                ++carriedClones;
        const int afterPasteClones = carriedClones;
        v1->setSelectedClip(0);
        tl.duplicateSelectedClips();
        const QString afterDuplicate = describe(v1);
        carriedClones = 0;
        for (const ClipInfo &clip : v1->clips())
            if (clip.displayName == QStringLiteral("a") && clip.uid != sourceUid
                && clip.text3DConfig == t3A && clip.brushData == brA)
                ++carriedClones;
        // The clone of n (no values) carries none.
        int nIndex = -1;
        for (int i = 0; i < v1->clipCount(); ++i)
            if (v1->clips().at(i).displayName == QStringLiteral("n")) nIndex = i;
        v1->setSelectedClip(nIndex);
        tl.duplicateSelectedClips();
        int nWithValues = 0, nCount = 0;
        for (const ClipInfo &clip : v1->clips()) {
            if (clip.displayName != QStringLiteral("n")) continue;
            ++nCount;
            if (!clip.text3DConfig.isEmpty() || !clip.brushData.isEmpty()) ++nWithValues;
        }
        const bool ok = afterPasteClones == 1 && carriedClones == 2 && nCount == 2 && nWithValues == 0;
        tally.gate(9, ok, QString::asprintf(
            "paste=[%s] clones %d (expect 1) duplicate=[%s] clones %d (expect 2) n copies %d with values %d "
            "(expect 2 / 0)", qPrintable(afterPaste), afterPasteClones, qPrintable(afterDuplicate),
            carriedClones, nCount, nWithValues).toStdString());
    }

    // G10 / G11: a clip graded with exposure -0.5 and a brush renders the same before and
    // after render in place (the brush is baked under the grade, the replacement draws none).
    {
        ClipInfo graded{};
        graded.filePath = QFileInfo(QStringLiteral("test_assets/e2e_clip.mp4")).absoluteFilePath();
        graded.displayName = QStringLiteral("sidecar-graded");
        const auto sourceDuration = libavcore::probeDurationMicroseconds(graded.filePath.toStdString());
        graded.duration = sourceDuration ? double(*sourceDuration) / 1000000.0 : 0.0;
        graded.inPoint = 0.5;
        graded.outPoint = 1.5;
        graded.colorCorrection.exposure = -0.5;
        graded.brushData = brushFor(QStringLiteral("Brush"));
        const QSize canvas(640, 360);
        struct GradedBake {
            bool baked = false;
            QString error;
            QRect box;
            QVector<double> times;
            QVector<double> diffs;
            bool replacementBrushFree = false;
            bool decomposed = false;
        };
        // Bakes clip, compares renderFrameAt before / after inside the stroke box (the
        // brush clip against the same clip without the brush, before the bake), then
        // decomposes and compares the restored brush with the original.
        const auto gradedBake = [&](const ClipInfo &clip, double handlesSec, const QVector<double> &times,
                                    const QString &dirName) {
            GradedBake result;
            result.times = times;
            ClipInfo withoutBrush = clip;
            withoutBrush.brushData = QJsonObject();
            withoutBrush.keyframes.removeTrack(QStringLiteral("brush_progress"));
            Timeline tl, tlPlain;
            loadSingleClip(tl, clip);
            loadSingleClip(tlPlain, withoutBrush);
            QVector<QImage> before;
            for (double t : times) {
                before.append(tlrender::renderFrameAt(&tl, microsAt(t), canvas));
                result.box = result.box.united(
                    changedBox(before.last(), tlrender::renderFrameAt(&tlPlain, microsAt(t), canvas)));
            }
            renderinplace::Options options;
            options.outputSize = canvas;
            options.fps = 30;
            options.handlesSec = handlesSec;
            options.outputDir = tempDir.filePath(dirName);
            result.baked = renderinplace::renderClipInPlace(tl, 0, 0, options, nullptr, &result.error);
            if (!result.baked) return result;
            const ClipInfo after = tl.videoTracks()[0]->clips()[0];
            result.replacementBrushFree = after.brushData.isEmpty()
                && !after.keyframes.hasTrack(QStringLiteral("brush_progress"));
            for (int i = 0; i < times.size(); ++i)
                result.diffs.append(meanAbsDiff(before[i], tlrender::renderFrameAt(&tl, microsAt(times[i]), canvas),
                                                result.box));
            const bool decomposed = renderinplace::decomposeRenderInPlace(tl, 0, 0);
            const ClipInfo restored = tl.videoTracks()[0]->clips()[0];
            result.decomposed = decomposed && restored.brushData == clip.brushData
                && sameProgressKeys(clip, restored);
            return result;
        };
        const auto within = [](const GradedBake &r) {
            if (!r.baked || r.box.isEmpty() || r.diffs.isEmpty()) return false;
            for (double d : r.diffs)
                if (d < 0.0 || d > 2.0) return false;
            return true;
        };
        const auto shown = [](const GradedBake &r) {
            QString diffs;
            for (int i = 0; i < r.diffs.size(); ++i)
                diffs += QString::asprintf(" %.2fs=%.4f", r.times[i], r.diffs[i]);
            return QString::asprintf("baked=%d [%s] box=(%d,%d %dx%d) replacement brush-free=%d "
                                     "before vs after meanAbsDiff%s (expect <= 2.0) decompose=%d",
                                     int(r.baked), qPrintable(r.error), r.box.x(), r.box.y(), r.box.width(),
                                     r.box.height(), int(r.replacementBrushFree), qPrintable(diffs),
                                     int(r.decomposed));
        };
        if (!tempDir.isValid() || !QFileInfo::exists(graded.filePath) || graded.duration < 3.0) {
            const std::string missing = QString::asprintf(
                "prerequisite: fixture=%s exists=%d duration=%.3f (run from the repository root)",
                qPrintable(graded.filePath), int(QFileInfo::exists(graded.filePath)), graded.duration).toStdString();
            tally.gate(10, false, missing);
            tally.gate(11, false, missing);
        } else {
            // G10: constant brush_progress 1, no handles.
            ClipInfo constantClip = graded;
            KeyframeTrack constantProgress(QStringLiteral("brush_progress"));
            constantProgress.addKeyframe(0.0, 1.0);
            constantClip.keyframes.addTrack(constantProgress);
            const GradedBake plainBake = gradedBake(constantClip, 0.0, {0.25, 0.75}, QStringLiteral("graded"));
            tally.gate(10, within(plainBake), QString::asprintf(
                "exposure -0.5 %s (HEAD Step 0: 46.18)", qPrintable(shown(plainBake))).toStdString());

            // G11: animated brush_progress (0 at local 0 to 1 at local 1), handles 0.5 s with
            // speed 2, and a reversed clip.
            ClipInfo animated = graded;
            KeyframeTrack animatedProgress(QStringLiteral("brush_progress"));
            animatedProgress.addKeyframe(0.0, 0.0);
            animatedProgress.addKeyframe(1.0, 1.0);
            animated.keyframes.addTrack(animatedProgress);
            ClipInfo fast = animated;
            fast.outPoint = 2.5;
            fast.speed = 2.0;
            ClipInfo reversed = animated;
            reversed.reversed = true;
            const GradedBake handled = gradedBake(fast, 0.5, {0.3, 0.7}, QStringLiteral("graded-handles"));
            const GradedBake backward = gradedBake(reversed, 0.0, {0.3, 0.7}, QStringLiteral("graded-reversed"));
            tally.gate(11, within(handled) && handled.decomposed && within(backward) && backward.decomposed,
                QString::asprintf("handles 0.5 + speed 2: %s | reversed: %s (HEAD Step 0 reversed: 46.18)",
                                  qPrintable(shown(handled)), qPrintable(shown(backward))).toStdString());
        }
    }

    return tally.finish("clip-sidecar");
}
