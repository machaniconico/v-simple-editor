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
//   G8 render in place: the replacement clip keeps both, the baked picture is the one
//      baked from the same clip without them. Detects: the replacement losing them
//      (mutation build: the two copyComposition lines removed).
//   G9 duplicate and copy / paste carry both to the new clip. Detects: the clone dropping
//      them (mutation build: copyClipForPaste clears them).
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

double meanAbsDiff(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) return -1.0;
    const QImage l = a.convertToFormat(QImage::Format_RGB888);
    const QImage r = b.convertToFormat(QImage::Format_RGB888);
    double sum = 0.0;
    for (int y = 0; y < l.height(); ++y) {
        const uchar *pl = l.constScanLine(y);
        const uchar *pr = r.constScanLine(y);
        for (int x = 0; x < l.width() * 3; ++x) sum += std::abs(int(pl[x]) - int(pr[x]));
    }
    return sum / (double(l.width()) * l.height() * 3.0);
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

    // G8: render in place keeps both on the replacement and bakes the same picture as the
    // clip without them.
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
            plain.outPoint = 1.5;
            ClipInfo carrying = plain;
            carrying.text3DConfig = t3A;
            carrying.brushData = brA;
            // The brush draws only where brush_progress is above 0, so the replacement
            // must also carry the progress keys: an animated track and a constant one.
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
                tl.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, QVector<QVector<ClipInfo>>{},
                                      0, -1, -1, 10);
                tl.undoManager()->clear();
                tl.saveUndoState(QStringLiteral("Initial"));
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
            const bool plainBaked = bake(plain, QStringLiteral("plain"), &plainAfter, &plainPath, &plainError);
            const bool constantBaked = bake(constant, QStringLiteral("constant"), &constantAfter, &constantPath,
                                            &constantError);
            // Same keys (time and value) on the replacement, and the same nonzero progress
            // at interior times.
            auto sameKeys = [](const ClipInfo &before, const ClipInfo &after) {
                const KeyframeTrack *a = before.keyframes.track(QStringLiteral("brush_progress"));
                const KeyframeTrack *b = after.keyframes.track(QStringLiteral("brush_progress"));
                if (!a || !b || a->keyframes().size() != b->keyframes().size()) return false;
                for (int i = 0; i < a->keyframes().size(); ++i) {
                    if (a->keyframes()[i].time != b->keyframes()[i].time
                        || a->keyframes()[i].value != b->keyframes()[i].value)
                        return false;
                }
                return true;
            };
            QString progressShown;
            bool progressKept = carriedBaked && constantBaked && sameKeys(carrying, carriedAfter)
                && sameKeys(constant, constantAfter);
            for (const auto &pair : {std::make_pair(&carrying, &carriedAfter),
                                     std::make_pair(&constant, &constantAfter)}) {
                for (double t : {0.75, 1.25}) {
                    const double want = pair.first->keyframes.valueAt(QStringLiteral("brush_progress"), t);
                    const double got = pair.second->keyframes.valueAt(QStringLiteral("brush_progress"), t);
                    progressShown += QString::asprintf(" %.2f:%.3f/%.3f", t, got, want);
                    if (want <= 0.0 || got != want) progressKept = false;
                }
            }
            const bool replaced = carriedBaked && carriedAfter.filePath != carrying.filePath
                && carriedAfter.renderInPlaceOriginal;
            const bool kept = carriedAfter.text3DConfig == t3A && carriedAfter.brushData == brA
                && constantAfter.brushData == brA;
            // The baked files alone (no sidecars on either) at three ticks.
            double worstDiff = -1.0;
            bool framesOk = carriedBaked && plainBaked;
            for (qint64 tick : {100000LL, 500000LL, 900000LL}) {
                if (!framesOk) break;
                auto bakedOnly = [&](const QString &path) {
                    ClipInfo clip{};
                    clip.filePath = path;
                    clip.duration = plain.effectiveDuration();
                    Timeline tl;
                    tl.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}}, QVector<QVector<ClipInfo>>{},
                                          0, -1, -1, 10);
                    return tlrender::renderFrameAt(&tl, tick, options.outputSize);
                };
                const double diff = meanAbsDiff(bakedOnly(carriedPath), bakedOnly(plainPath));
                if (diff < 0.0) framesOk = false;
                worstDiff = std::max(worstDiff, diff);
            }
            const bool samePicture = framesOk && worstDiff >= 0.0 && worstDiff <= 0.5;
            tally.gate(8, replaced && kept && progressKept && samePicture, QString::asprintf(
                "baked=%d/%d/%d [%s|%s|%s] replaced=%d kept text3D=[%s] brush=[%s] (expect T3A/BRA) "
                "brush_progress keys kept=%d got/want at t:%s (expect equal and > 0) "
                "bakedVsPlain meanAbsDiff=%.4f (expect <= 0.5)", int(carriedBaked), int(plainBaked),
                int(constantBaked), qPrintable(carriedError), qPrintable(plainError), qPrintable(constantError),
                int(replaced), qPrintable(textOf(carriedAfter.text3DConfig)),
                qPrintable(textOf(carriedAfter.brushData)), int(progressKept), qPrintable(progressShown),
                worstDiff).toStdString());
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

    return tally.finish("clip-sidecar");
}
