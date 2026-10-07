// --selftest=roto: carrier remapping, undo, persistence, queue restoration,
// roto::applyEntryToFrame and the MainWindow single writer (storeRotoEntry).
#include "SelftestGate.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../ProjectFile.h"
#include "../TrackMatteKey.h"
#include "../RenderQueue.h"
#include "../SubtitleTrackRenderer.h"
#include "../libavcore/Encode.h"
#include "../AcesColor.h"
#include "../MainWindow.h"
#include "../RotoApply.h"
#include "../libavcore/FrameGrab.h"
#include <cmath>
#include <atomic>
#include <functional>
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


RotoPoint corner(double x, double y)
{
    RotoPoint p;
    p.position = p.handleIn = p.handleOut = QPointF(x, y);
    return p;
}

RotoPath rectPath(double x0, double y0, double x1, double y1, double feather = 0.0)
{
    RotoPath path;
    path.points = {corner(x0, y0), corner(x1, y0), corner(x1, y1), corner(x0, y1)};
    path.closed = true;
    path.feather = feather;
    return path;
}

// Pixel statistics of a roto result against its input. "Inside" pixels must be
// byte-identical (ARGB), "outside" pixels must be alpha 0 with RGB 0.
struct MaskStats {
    int insidePixels = 0;
    int insideDiff = 0;
    int insideMaxDiff = 0;
    int outsidePixels = 0;
    int outsideBad = 0;
    int edgePixels = 0;
    int edgeAlphaMin = 255;
    int edgeAlphaMax = 0;
};

template <typename InsideFn, typename OutsideFn>
MaskStats maskStats(const QImage &input, const QImage &output, InsideFn inside, OutsideFn outside)
{
    MaskStats stats;
    if (input.isNull() || output.size() != input.size()) return stats;
    for (int y = 0; y < input.height(); ++y) {
        for (int x = 0; x < input.width(); ++x) {
            const QRgb in = input.pixel(x, y);
            const QRgb out = output.pixel(x, y);
            const int alpha = qAlpha(out);
            if (alpha > 0 && alpha < 255) {
                ++stats.edgePixels;
                stats.edgeAlphaMin = qMin(stats.edgeAlphaMin, alpha);
                stats.edgeAlphaMax = qMax(stats.edgeAlphaMax, alpha);
            }
            if (inside(x, y)) {
                ++stats.insidePixels;
                const int diff = qMax(qMax(qAbs(qRed(in) - qRed(out)), qAbs(qGreen(in) - qGreen(out))),
                                      qMax(qAbs(qBlue(in) - qBlue(out)), qAbs(qAlpha(in) - alpha)));
                if (diff > 0) ++stats.insideDiff;
                stats.insideMaxDiff = qMax(stats.insideMaxDiff, diff);
            } else if (outside(x, y)) {
                ++stats.outsidePixels;
                if (alpha != 0 || qRed(out) != 0 || qGreen(out) != 0 || qBlue(out) != 0)
                    ++stats.outsideBad;
            }
        }
    }
    return stats;
}

bool inRect(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

void printStats(const char *label, const MaskStats &s)
{
    std::cerr << label << " inside=" << s.insidePixels << " insideDiff=" << s.insideDiff
              << " insideMaxDiff=" << s.insideMaxDiff << " outside=" << s.outsidePixels
              << " outsideBad=" << s.outsideBad << " edge=" << s.edgePixels
              << " edgeAlpha=" << (s.edgePixels ? s.edgeAlphaMin : 0) << ".."
              << s.edgeAlphaMax << '\n';
}

bool cleanMask(const MaskStats &s)
{
    return s.insidePixels > 0 && s.insideDiff == 0 && s.outsidePixels > 0 && s.outsideBad == 0;
}

// MainWindow fixture for G9-G12 (seedWindow): V1 [100, 101 (1 s gap)],
// V2 [1, 2 (1 s gap)], V3 [3], V4 [matte/parent source at 10 s]. Roto markers
// 1/2/3/4 sit on V2:0 / V2:1 / V3:0 / V1:1; the matte on the same key carries a
// marker-specific type so a roto/matte pairing shift is visible.
TrackMatteType matteTypeForMarker(int marker)
{
    switch (marker) {
    case 1: return TrackMatteType::AlphaMatte;
    case 2: return TrackMatteType::AlphaInvertedMatte;
    case 3: return TrackMatteType::LumaMatte;
    case 4: return TrackMatteType::LumaInvertedMatte;
    default: return TrackMatteType::None;
    }
}

QString layoutSignature(const Timeline *tl)
{
    QStringList parts;
    for (const auto *track : tl->videoTracks()) {
        QStringList clips;
        for (const ClipInfo &c : track->clips())
            clips.append(QStringLiteral("%1@%2+%3/%4-%5").arg(c.displayName)
                .arg(c.leadInSec, 0, 'f', 3).arg(c.effectiveDuration(), 0, 'f', 3)
                .arg(c.inPoint, 0, 'f', 3).arg(c.outPoint, 0, 'f', 3));
        parts.append(QLatin1Char('[') + clips.join(QLatin1Char(',')) + QLatin1Char(']'));
    }
    return parts.join(QLatin1Char(' '));
}

} // namespace

int runRotoSelftest()
{
    selftest::GateTally gates;
    QTemporaryDir media;
    gSyntheticClipPath = media.filePath(QStringLiteral("roto-synthetic.mp4"));
    QString unavailable;
    if (!media.isValid() || !selftest::requireEncoder(QStringLiteral("mpeg4"), &unavailable)) {
        for (int gate = 1; gate <= 12; ++gate)
            gates.skip(gate, media.isValid() ? "mpeg4 fixture encoder unavailable"
                                             : "temporary directory unavailable");
        return gates.finish("roto");
    }
    if (!writeSyntheticClip(gSyntheticClipPath)) {
        for (int gate = 1; gate <= 12; ++gate)
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

    // G7 detects an inverted or shifted roto mask (interior must be kept) and
    // keyframes losing priority over the single path.
    const QImage source = libavcore::grabFrameAt(gSyntheticClipPath, 0.5);
    RotoClipEntry hard;
    hard.path = rectPath(16, 16, 48, 48);
    const QImage hardOut = roto::applyEntryToFrame(hard, source, 5);
    const MaskStats hardStats = maskStats(source, hardOut,
        [](int x, int y) { return inRect(x, y, 18, 18, 46, 46); },
        [](int x, int y) { return !inRect(x, y, 14, 14, 50, 50); });
    RotoClipEntry soft;
    soft.path = rectPath(16, 16, 48, 48, 3.0);
    const QImage softOut = roto::applyEntryToFrame(soft, source, 5);
    // Three box-blur passes of radius 3 reach 9 px past the edge.
    const MaskStats softStats = maskStats(source, softOut,
        [](int x, int y) { return inRect(x, y, 26, 26, 38, 38); },
        [](int x, int y) { return !inRect(x, y, 6, 6, 58, 58); });
    RotoClipEntry keyed;
    keyed.path = rectPath(16, 16, 48, 48);
    RotoKeyframe keyedFrame;
    keyedFrame.frameNumber = 0;
    keyedFrame.path = rectPath(0, 0, 24, 64);
    keyed.keyframes.append(keyedFrame);
    const QImage keyedOut = roto::applyEntryToFrame(keyed, source, 5);
    const MaskStats keyedStats = maskStats(source, keyedOut,
        [](int x, int y) { return inRect(x, y, 2, 2, 22, 62); },
        [](int x, int y) { return inRect(x, y, 26, 18, 46, 46); });
    printStats("G7 path feather=0", hardStats);
    printStats("G7 path feather=3", softStats);
    printStats("G7 keyframes>path", keyedStats);
    std::cerr << "G7 source=" << source.width() << 'x' << source.height()
              << " format=" << int(hardOut.format()) << '\n';
    const bool g7 = source.size() == QSize(64, 64)
        && cleanMask(hardStats) && hardStats.edgePixels == 0
        && cleanMask(softStats) && softStats.edgePixels > 0
        && cleanMask(keyedStats);
    gates.gate(7, g7, "roto interior not kept, exterior not cleared, or keyframes lost priority");

    // G8 detects a dropped brush max-combine, an altered pass-through for
    // empty entries / null frames, and a preview that bypasses
    // roto::sourceFrameIndexFor (its call counter stays flat).
    QImage brush(64, 64, QImage::Format_Grayscale8);
    brush.fill(0);
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x)
            brush.scanLine(y)[x] = 255;
    RotoClipEntry brushOnly;
    brushOnly.brushMask = brush;
    const MaskStats brushStats = maskStats(source, roto::applyEntryToFrame(brushOnly, source, 5),
        [](int x, int y) { return inRect(x, y, 8, 8, 24, 24); },
        [](int x, int y) { return !inRect(x, y, 6, 6, 26, 26); });
    RotoClipEntry pathBrush;
    pathBrush.path = rectPath(32, 32, 56, 56);
    pathBrush.brushMask = brush;
    const MaskStats unionStats = maskStats(source, roto::applyEntryToFrame(pathBrush, source, 5),
        [](int x, int y) { return inRect(x, y, 8, 8, 24, 24) || inRect(x, y, 34, 34, 54, 54); },
        [](int x, int y) { return !inRect(x, y, 6, 6, 26, 26) && !inRect(x, y, 30, 30, 58, 58); });
    printStats("G8 brush-only", brushStats);
    printStats("G8 path+brush", unionStats);
    const bool emptySame = roto::applyEntryToFrame(RotoClipEntry{}, source, 5) == source;
    const bool nullSame = roto::applyEntryToFrame(hard, QImage(), 5).isNull();
    struct IndexCase { double sec; double fps; int expected; };
    const IndexCase indexCases[] = {
        {1.0, 30.0, 30}, {2.0, 29.97, 60}, {-0.4, 30.0, 0}, {0.25, 10.0, 3}, {0.0, 24.0, 0}};
    bool indexOk = true;
    for (const auto &c : indexCases) {
        const int actual = roto::sourceFrameIndexFor(c.sec, c.fps);
        const int viaLlround = qMax(0, static_cast<int>(std::llround(c.sec * c.fps)));
        std::cerr << "G8 sourceFrameIndexFor(" << c.sec << ',' << c.fps << ")=" << actual
                  << " llround=" << viaLlround << " expected=" << c.expected << '\n';
        indexOk = indexOk && actual == viaLlround && actual == c.expected;
    }
    MainWindow window;
    Timeline *wtl = window.m_timeline;
    window.m_projectConfig.width = 64;
    window.m_projectConfig.height = 64;
    window.m_projectConfig.fps = 10;
    ClipInfo remapped = clip(70);
    remapped.timeRemapCurve.sourceFps = 10.0;
    remapped.timeRemapCurve.addKey(0.0, 0.0);
    remapped.timeRemapCurve.addKey(1.0, 1.0);
    wtl->restoreFromProject(QVector<QVector<ClipInfo>>{{remapped}},
                            QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    wtl->setRotoEntries({});
    const quint64 remapBefore = roto::sourceFrameIndexForCallCount();
    const QImage remapPreview = window.buildSpecialClipComposite(0.5);
    const quint64 remapCalls = roto::sourceFrameIndexForCallCount() - remapBefore;
    wtl->restoreFromProject(QVector<QVector<ClipInfo>>{{clip(71)}},
                            QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
    QHash<QString, RotoClipEntry> plainRoto;
    plainRoto.insert(QStringLiteral("0:0"), hard);
    wtl->setRotoEntries(plainRoto);
    const quint64 plainBefore = roto::sourceFrameIndexForCallCount();
    const QImage plainPreview = window.buildSpecialClipComposite(0.5);
    const quint64 plainCalls = roto::sourceFrameIndexForCallCount() - plainBefore;
    wtl->setRotoEntries({});
    std::cerr << "G8 emptySame=" << emptySame << " nullSame=" << nullSame
              << " indexOk=" << indexOk << " remapPreviewCalls=" << remapCalls
              << " remapPreviewNull=" << remapPreview.isNull()
              << " plainRotoPreviewCalls=" << plainCalls
              << " plainPreviewNull=" << plainPreview.isNull() << '\n';
    const bool g8 = cleanMask(brushStats) && cleanMask(unionStats) && emptySame && nullSame
        && indexOk && remapCalls > 0 && !remapPreview.isNull()
        && plainCalls > 0 && !plainPreview.isNull();
    gates.gate(8, g8, "brush max-combine, pass-through or shared source frame index failed");

    const auto seedWindow = [](MainWindow &window) {
        Timeline *tl = window.m_timeline;
        ClipInfo gappedV1 = clip(101);
        gappedV1.leadInSec = 1.0;
        ClipInfo gappedV2 = clip(2);
        gappedV2.leadInSec = 1.0;
        ClipInfo source = clip(4);
        source.leadInSec = 10.0;
        const QVector<QVector<ClipInfo>> video{
            {clip(100), gappedV1}, {clip(1), gappedV2}, {clip(3)}, {source}};
        const QVector<QVector<ClipInfo>> audio{{}};
        tl->clearSelection();
        tl->restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
        QHash<QString, RotoClipEntry> rotos;
        rotos.insert(QStringLiteral("1:0"), entry(1));
        rotos.insert(QStringLiteral("1:1"), entry(2, true));
        rotos.insert(QStringLiteral("2:0"), entry(3));
        rotos.insert(QStringLiteral("0:1"), entry(4));
        tl->setRotoEntries(rotos);
        window.m_trackMatteClipEntries.clear();
        QHash<QString, TimelineTrackMatteEntry> carrier;
        QHash<QString, QString> parents;
        for (auto it = rotos.cbegin(); it != rotos.cend(); ++it) {
            TrackMatteClipEntry m;
            m.clipId = it.key();
            m.matteType = matteTypeForMarker(it.value().path.points.size());
            m.matteSourceClipId = QStringLiteral("3:0");
            window.m_trackMatteClipEntries.insert(it.key(), m);
            TimelineTrackMatteEntry c;
            c.matteType = m.matteType;
            c.matteSourceClipId = m.matteSourceClipId;
            carrier.insert(it.key(), c);
            parents.insert(it.key(), QStringLiteral("3:0"));
        }
        tl->setTrackMatteEntries(carrier);
        tl->setClipParentEntries(parents);
        tl->undoManager()->clear();
        tl->undoManager()->saveState(tl->currentState(), QStringLiteral("基準"));
    };

    // G9 detects a writer that bypasses the carrier, keeps a stale value
    // clipId, or skips the single undo step.
    seedWindow(window);
    UndoManager *um = wtl->undoManager();
    const quint64 serialBefore = um->saveSerial();
    RotoClipEntry stored = entry(6);
    stored.clipId = QStringLiteral("9:9");
    window.storeRotoEntry(QStringLiteral("0:0"), stored);
    const quint64 serialAfter = um->saveSerial();
    const auto afterStore = wtl->rotoEntries();
    wtl->undo();
    const auto afterStoreUndo = wtl->rotoEntries();
    wtl->redo();
    const auto afterStoreRedo = wtl->rotoEntries();
    ProjectData populated;
    window.populateProjectData(populated);
    const auto populatedEntries = restoreEntries(populated.rotoClipEntries);
    bool populatedSame = populatedEntries.size() == afterStoreRedo.size()
        && populated.rotoClipEntries.size() == afterStoreRedo.size();
    for (auto it = afterStoreRedo.cbegin(); it != afterStoreRedo.cend(); ++it)
        populatedSame = populatedSame && samePayload(populatedEntries.value(it.key()), it.value());
    const QSet<QString> seeded{QStringLiteral("0:1"), QStringLiteral("1:0"),
                               QStringLiteral("1:1"), QStringLiteral("2:0")};
    QSet<QString> withStored = seeded;
    withStored.insert(QStringLiteral("0:0"));
    std::cerr << "G9 store=" << show(keys(afterStore)).toStdString()
              << " clipId=" << afterStore.value(QStringLiteral("0:0")).clipId.toStdString()
              << " saveSerial=" << serialBefore << "->" << serialAfter
              << " undo=" << show(keys(afterStoreUndo)).toStdString()
              << " redo=" << show(keys(afterStoreRedo)).toStdString()
              << " redoPoints=" << afterStoreRedo.value(QStringLiteral("0:0")).path.points.size()
              << " populated=" << show(keys(populatedEntries)).toStdString()
              << " populatedSame=" << populatedSame << '\n';
    const bool g9 = keys(afterStore) == withStored
        && afterStore.value(QStringLiteral("0:0")).clipId == QStringLiteral("0:0")
        && afterStore.value(QStringLiteral("0:0")).path.points.size() == 6
        && serialAfter == serialBefore + 1
        && keys(afterStoreUndo) == seeded
        && keys(afterStoreRedo) == withStored
        && afterStoreRedo.value(QStringLiteral("0:0")).path.points.size() == 6
        && invariant(afterStore) && invariant(afterStoreRedo) && populatedSame;
    gates.gate(9, g9, "storeRotoEntry carrier, clipId, undo serial or populateProjectData mismatch");

    // G10 detects roto keys drifting from the MainWindow matte keys on the GUI
    // edit paths (each path remaps twice: inside Timeline and in MainWindow).
    // The text-based range ripple compares against parents (matte does not
    // follow that path, an existing defect).
    struct EditPath { const char *name; std::function<void()> run; bool parentPeer; };
    const auto select = [&](int track, int clipIndex) {
        wtl->clearSelection();
        QString error;
        wtl->selectClipByIndex(false, track, clipIndex, &error);
    };
    const QVector<EditPath> editPaths{
        // MainWindow::deleteClip only runs with a V1 (primary track) selection.
        {"deleteClip", [&] { select(1, 0); wtl->videoTracks()[0]->toggleClipSelection(0);
                             window.deleteClip(); }, false},
        {"splitClip", [&] { wtl->setPlayheadPosition(1.0); select(1, 0); window.splitClip(); }, false},
        {"rippleDelete", [&] { select(1, 0); window.rippleDelete(); }, false},
        // Timeline::pasteClip copies from / inserts into V1 (the audio track is
        // empty, so the insert lands at V1:0 and shifts the V1:1 roto).
        {"pasteClip", [&] { select(0, 0); window.copyClip(); wtl->setPlayheadPosition(0.0);
                            window.pasteClip(); }, false},
        {"moveClipHeadToPlayhead", [&] { wtl->setPlayheadPosition(6.0); select(1, 0);
                                         window.m_moveClipHeadAction->trigger(); }, false},
        {"duplicate", [&] { select(1, 0); window.m_duplicateAction->trigger(); }, false},
        {"nudge-1", [&] { select(1, 1); window.m_nudgeLeftAction->trigger(); }, false},
        {"nudge+1", [&] { select(1, 1); window.m_nudgeRightAction->trigger(); }, false},
        {"nudge-10", [&] { select(1, 1); window.m_nudgeLeft10Action->trigger(); }, false},
        {"nudge+10", [&] { select(1, 0); window.m_nudgeRight10Action->trigger(); }, false},
        {"closeAllGaps", [&] { window.m_closeAllGapsAction->trigger(); }, false},
        {"bladeAll", [&] { wtl->setPlayheadPosition(1.0); window.m_bladeAllAction->trigger(); }, false},
        {"lift", [&] { select(1, 0); window.m_liftAction->trigger(); }, false},
        {"textRangeRipple", [&] { select(1, 0); wtl->rippleDeleteTimeRangeActive(0.0, 2.0); }, true},
    };
    bool g10 = true;
    for (const auto &path : editPaths) {
        seedWindow(window);
        const QString layoutBefore = layoutSignature(wtl);
        path.run();
        const QString layoutAfter = layoutSignature(wtl);
        const auto rotos = wtl->rotoEntries();
        const QSet<QString> rotoKeys = keys(rotos);
        const QSet<QString> peerKeys = path.parentPeer
            ? keys(wtl->clipParentEntries()) : keys(window.m_trackMatteClipEntries);
        QStringList pairing;
        bool paired = true;
        if (!path.parentPeer) {
            for (auto it = rotos.cbegin(); it != rotos.cend(); ++it) {
                const int marker = it.value().path.points.size();
                const TrackMatteType matte = window.m_trackMatteClipEntries.value(it.key()).matteType;
                if (matte != matteTypeForMarker(marker)) {
                    paired = false;
                    pairing.append(QStringLiteral("%1:marker%2/matte%3").arg(it.key())
                                       .arg(marker).arg(int(matte)));
                }
            }
        }
        const bool changed = layoutBefore != layoutAfter;
        const bool one = changed && rotoKeys == peerKeys && invariant(rotos) && paired
            && rotoKeys.size() >= 2;
        std::cerr << "G10 " << path.name << " roto=" << show(rotoKeys).toStdString()
                  << " peer(" << (path.parentPeer ? "parent" : "matte") << ")="
                  << show(peerKeys).toStdString()
                  << " rotoOnly=" << show(rotoKeys - peerKeys).toStdString()
                  << " peerOnly=" << show(peerKeys - rotoKeys).toStdString()
                  << " marker1At=" << whereMarker(rotos, 1).toStdString()
                  << " marker2At=" << whereMarker(rotos, 2).toStdString()
                  << " marker3At=" << whereMarker(rotos, 3).toStdString()
                  << " marker4At=" << whereMarker(rotos, 4).toStdString()
                  << " pairingMismatch=" << pairing.join(QLatin1Char(',')).toStdString()
                  << " clipIdOk=" << invariant(rotos) << " changed=" << changed
                  << " layout=" << layoutAfter.toStdString() << '\n';
        g10 = g10 && one;
    }
    gates.gate(10, g10, "roto keys drifted from matte/parent keys on a MainWindow edit path");

    // G11 detects a project load that sets roto after the undo baseline (an
    // edit + undo would then wipe the loaded roto).
    seedWindow(window);
    ProjectData project;
    window.populateProjectData(project);
    wtl->setRotoEntries({});
    window.applyLoadedProjectData(project, QString());
    const auto afterLoad = wtl->rotoEntries();
    const bool canUndoAfterLoad = wtl->undoManager()->canUndo();
    wtl->undo();
    const auto afterLoadUndo = wtl->rotoEntries();
    window.storeRotoEntry(QStringLiteral("0:0"), entry(7));
    const auto afterLoadEdit = wtl->rotoEntries();
    wtl->undo();
    const auto afterLoadEditUndo = wtl->rotoEntries();
    std::cerr << "G11 projectRoto=" << project.rotoClipEntries.size()
              << " load=" << show(keys(afterLoad)).toStdString()
              << " canUndoAfterLoad=" << canUndoAfterLoad
              << " undo=" << show(keys(afterLoadUndo)).toStdString()
              << " edit=" << show(keys(afterLoadEdit)).toStdString()
              << " editUndo=" << show(keys(afterLoadEditUndo)).toStdString()
              << " marker2At=" << whereMarker(afterLoadEditUndo, 2).toStdString() << '\n';
    QSet<QString> loadEdited = seeded;
    loadEdited.insert(QStringLiteral("0:0"));
    const bool g11 = project.rotoClipEntries.size() == 4 && keys(afterLoad) == seeded
        && keys(afterLoadUndo) == seeded && keys(afterLoadEdit) == loadEdited
        && keys(afterLoadEditUndo) == seeded
        && whereMarker(afterLoadEditUndo, 2) == QStringLiteral("1:1")
        && invariant(afterLoadEditUndo);
    gates.gate(11, g11, "loaded roto missing from the post-load undo baseline");

    // G12 detects an empty dialog result that leaves the entry in the carrier
    // or removes it outside the undo history.
    seedWindow(window);
    const quint64 clearSerialBefore = um->saveSerial();
    window.storeRotoEntry(QStringLiteral("1:1"), RotoClipEntry{});
    const quint64 clearSerialAfter = um->saveSerial();
    const auto afterClear = wtl->rotoEntries();
    wtl->undo();
    const auto afterClearUndo = wtl->rotoEntries();
    const RotoClipEntry restored = afterClearUndo.value(QStringLiteral("1:1"));
    std::cerr << "G12 clear=" << show(keys(afterClear)).toStdString()
              << " saveSerial=" << clearSerialBefore << "->" << clearSerialAfter
              << " undo=" << show(keys(afterClearUndo)).toStdString()
              << " points=" << restored.path.points.size()
              << " mask=" << restored.brushMask.width() << 'x' << restored.brushMask.height() << '\n';
    const bool g12 = keys(afterClear) == QSet<QString>{QStringLiteral("0:1"), QStringLiteral("1:0"),
                                                       QStringLiteral("2:0")}
        && clearSerialAfter == clearSerialBefore + 1
        && keys(afterClearUndo) == seeded && restored.clipId == QStringLiteral("1:1")
        && restored.path.points.size() == 2 && restored.brushMask.size() == QSize(3, 2);
    gates.gate(12, g12, "empty roto entry not removed from the carrier or not undoable");
    return gates.finish("roto");
}
