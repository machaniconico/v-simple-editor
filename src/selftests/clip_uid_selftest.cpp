#include "../ProjectDiff.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "SelftestGate.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>
#include <string>

// clip-uid (US-1118): the stable ClipInfo::uid minted by TimelineTrack.
//   G1: setClips (restoreFromProject) and addClip mint a non-empty uid, addClip
//       keeps a uid the clip already has (moves keep identity), splitClipAt
//       keeps the left half's uid and gives the right half a new one, and the
//       paste / duplicate clones get uids of their own. Detects: a funnel that
//       stops minting, a split or paste that copies the source uid verbatim
//       (a raw ClipInfo copy keeps it, printed as rawCopyAliases).
//   G2: save -> load keeps every uid, a uid-less clip saves without a "uid"
//       key, clipFromJson leaves it empty and restoreFromProject mints it, and
//       ProjectDiff ignores the uid at the top level and inside
//       renderInPlaceOriginal (control: a nested filePath change still diffs).
//   G3: setClips with two clips sharing a uid keeps it on the first, re-mints
//       the second and counts one repair; distinct uids are kept and counted 0.
//   G4: undo / redo of a paste restore the exact uids of each state.
namespace {
ClipInfo clipFor(const QString &path, double duration = 2.0)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = path;
    clip.duration = clip.outPoint = duration;
    return clip;
}

QStringList uidsOf(const QVector<ClipInfo> &clips)
{
    QStringList uids;
    for (const ClipInfo &clip : clips) uids.append(clip.uid);
    return uids;
}

bool allNonEmptyDistinct(const QStringList &uids)
{
    QSet<QString> seen;
    for (const QString &uid : uids) {
        if (uid.isEmpty() || seen.contains(uid)) return false;
        seen.insert(uid);
    }
    return true;
}

QString joined(const QStringList &uids) { return uids.join(QLatin1Char(',')); }

ProjectData dataOf(const Timeline &timeline)
{
    ProjectData data;
    data.videoTracks = timeline.allVideoTracks();
    data.audioTracks = timeline.allAudioTracks();
    return data;
}
} // namespace

int runClipUidSelftest()
{
    selftest::GateTally tally;

    // G1: minting funnels, split and paste / duplicate clones.
    {
        Timeline timeline;
        timeline.restoreFromProject(
            QVector<QVector<ClipInfo>>{{clipFor(QStringLiteral("/selftests/uid-v0.mp4")),
                                        clipFor(QStringLiteral("/selftests/uid-v1.mp4"))}},
            QVector<QVector<ClipInfo>>{{clipFor(QStringLiteral("/selftests/uid-a0.wav")),
                                        clipFor(QStringLiteral("/selftests/uid-a1.wav"))}},
            0.0, -1.0, -1.0, 100);
        TimelineTrack *v1 = timeline.trackAt(false, 0);
        TimelineTrack *a1 = timeline.trackAt(true, 0);
        const QStringList loaded = uidsOf(v1->clips()) + uidsOf(a1->clips());
        const bool setClipsMinted = loaded.size() == 4 && allNonEmptyDistinct(loaded);

        v1->addClip(clipFor(QStringLiteral("/selftests/uid-v2.mp4")));
        ClipInfo carried = clipFor(QStringLiteral("/selftests/uid-v3.mp4"));
        carried.uid = QStringLiteral("carried-uid");
        v1->addClip(carried);
        const QString addedUid = v1->clips().value(2).uid;
        const bool addMinted = v1->clipCount() == 4 && !addedUid.isEmpty()
            && !loaded.contains(addedUid);
        const bool addKept = v1->clips().value(3).uid == QStringLiteral("carried-uid");

        const QString beforeSplit = v1->clips().value(0).uid;
        v1->splitClipAt(0, 0.8);
        const QString leftUid = v1->clips().value(0).uid;
        const QString rightUid = v1->clips().value(1).uid;
        const bool split = v1->clipCount() == 5 && leftUid == beforeSplit
            && !rightUid.isEmpty() && rightUid != beforeSplit;

        // Paste the selected V1 clip 0 (V at V1:1, A partner at A1:1).
        v1->setSelectedClip(0);
        timeline.copySelectedClip();
        const ClipInfo source = v1->clips().value(0);
        const ClipInfo rawCopy = source;
        const bool rawCopyAliases = rawCopy.uid == source.uid;
        timeline.pasteClip();
        const QString pastedVideo = v1->clips().value(1).uid;
        const QString pastedAudio = a1->clips().value(1).uid;
        const bool paste = v1->clipCount() == 6 && a1->clipCount() == 3
            && !pastedVideo.isEmpty() && pastedVideo != source.uid
            && !pastedAudio.isEmpty() && pastedAudio != pastedVideo
            && pastedAudio != source.uid;

        // Duplicate the selected clip(s) (paste leaves V1:1 / A1:1 selected);
        // each clone gets a uid that no clip had before.
        const QStringList beforeDuplicate = uidsOf(v1->clips()) + uidsOf(a1->clips());
        timeline.duplicateSelectedClips();
        QString duplicateUid;
        const QStringList afterDuplicate = uidsOf(v1->clips()) + uidsOf(a1->clips());
        for (const QString &uid : afterDuplicate)
            if (!beforeDuplicate.contains(uid)) duplicateUid = uid;
        const bool duplicate = afterDuplicate.size() > beforeDuplicate.size()
            && !duplicateUid.isEmpty() && allNonEmptyDistinct(afterDuplicate);

        const std::string detail = QString::asprintf(
            "setClipsMinted=%d loaded=[%s] addMinted=%d added=%s addKept=%d "
            "split=%d before=%s left=%s right=%s rawCopyAliases=%d paste=%d source=%s "
            "pastedV=%s pastedA=%s duplicate=%d duplicateUid=%s clips=%d->%d",
            int(setClipsMinted), qPrintable(joined(loaded)), int(addMinted), qPrintable(addedUid),
            int(addKept), int(split), qPrintable(beforeSplit), qPrintable(leftUid),
            qPrintable(rightUid), int(rawCopyAliases), int(paste), qPrintable(source.uid),
            qPrintable(pastedVideo), qPrintable(pastedAudio), int(duplicate),
            qPrintable(duplicateUid), int(beforeDuplicate.size()),
            int(afterDuplicate.size())).toStdString();
        tally.gate(1, setClipsMinted && addMinted && addKept && split && rawCopyAliases
                   && paste && duplicate, detail);
    }

    // G2: persistence and ProjectDiff.
    {
        Timeline timeline;
        timeline.restoreFromProject(
            QVector<QVector<ClipInfo>>{{clipFor(QStringLiteral("/selftests/uid-v0.mp4")),
                                        clipFor(QStringLiteral("/selftests/uid-v1.mp4"))}},
            QVector<QVector<ClipInfo>>{{clipFor(QStringLiteral("/selftests/uid-a0.wav"))}},
            0.0, -1.0, -1.0, 100);
        const ProjectData saved = dataOf(timeline);
        const QStringList savedUids = uidsOf(saved.videoTracks.value(0))
            + uidsOf(saved.audioTracks.value(0));
        const QString savedJson = ProjectFile::toJsonString(saved);
        const int uidKeys = savedJson.count(QStringLiteral("\"uid\""));
        ProjectData reloaded;
        const bool parsed = ProjectFile::fromJsonString(savedJson, reloaded);
        const QStringList parsedUids = uidsOf(reloaded.videoTracks.value(0))
            + uidsOf(reloaded.audioTracks.value(0));
        Timeline reopened;
        reopened.restoreFromProject(reloaded.videoTracks, reloaded.audioTracks,
                                    0.0, -1.0, -1.0, 100);
        const ProjectData reopenedData = dataOf(reopened);
        const QStringList reopenedUids = uidsOf(reopenedData.videoTracks.value(0))
            + uidsOf(reopenedData.audioTracks.value(0));
        const bool roundtrip = parsed && savedUids.size() == 3
            && allNonEmptyDistinct(savedUids) && uidKeys == 3
            && parsedUids == savedUids && reopenedUids == savedUids;

        // A clip without a uid (an old file) saves no key, loads empty and is
        // minted by restoreFromProject's setClips.
        ProjectData legacy;
        legacy.videoTracks.append(QVector<ClipInfo>{clipFor(QStringLiteral("/selftests/old.mp4"))});
        const QString legacyJson = ProjectFile::toJsonString(legacy);
        ProjectData legacyLoaded;
        const bool legacyParsed = ProjectFile::fromJsonString(legacyJson, legacyLoaded);
        const QString legacyLoadedUid = legacyLoaded.videoTracks.value(0).value(0).uid;
        Timeline legacyTimeline;
        legacyTimeline.restoreFromProject(legacyLoaded.videoTracks, legacyLoaded.audioTracks,
                                          0.0, -1.0, -1.0, 100);
        const QString legacyMinted = legacyTimeline.trackAt(false, 0)->clips().value(0).uid;
        const bool legacyOk = legacyParsed && !legacyJson.contains(QStringLiteral("\"uid\""))
            && legacyLoadedUid.isEmpty() && !legacyMinted.isEmpty();

        // ProjectDiff ignores the uid, including inside renderInPlaceOriginal.
        ClipInfo original = clipFor(QStringLiteral("/selftests/original.mp4"));
        ClipInfo baked = clipFor(QStringLiteral("/selftests/baked.mp4"));
        ClipInfo bakedA = baked;
        bakedA.uid = QStringLiteral("top-a");
        ClipInfo nestedA = original;
        nestedA.uid = QStringLiteral("nested-a");
        bakedA.renderInPlaceOriginal = std::make_shared<ClipInfo>(nestedA);
        ClipInfo bakedB = baked;
        bakedB.uid = QStringLiteral("top-b");
        bakedB.renderInPlaceOriginal = std::make_shared<ClipInfo>(original);
        ProjectData diffA, diffB;
        diffA.videoTracks.append(QVector<ClipInfo>{bakedA});
        diffB.videoTracks.append(QVector<ClipInfo>{bakedB});
        const auto uidOnly = projdiff::diff(diffA, diffB);
        const bool nestedSaved = ProjectFile::toJsonString(diffA).contains(QStringLiteral("nested-a"));
        ClipInfo nestedChanged = original;
        nestedChanged.filePath = QStringLiteral("/selftests/other-original.mp4");
        bakedB.renderInPlaceOriginal = std::make_shared<ClipInfo>(nestedChanged);
        diffB.videoTracks[0][0] = bakedB;
        const auto nestedFileDiff = projdiff::diff(diffA, diffB);
        const bool diffOk = uidOnly.isEmpty() && nestedSaved && nestedFileDiff.size() == 1;

        const std::string detail = QString::asprintf(
            "roundtrip=%d parsed=%d uidKeys=%d saved=[%s] parsedUids=[%s] reopened=[%s] "
            "legacyOk=%d legacyParsed=%d legacyLoadedUid='%s' legacyMinted=%s "
            "diffOk=%d uidOnlyChanges=%d nestedSaved=%d nestedFileChanges=%d",
            int(roundtrip), int(parsed), uidKeys, qPrintable(joined(savedUids)),
            qPrintable(joined(parsedUids)), qPrintable(joined(reopenedUids)), int(legacyOk),
            int(legacyParsed), qPrintable(legacyLoadedUid), qPrintable(legacyMinted),
            int(diffOk), int(uidOnly.size()), int(nestedSaved),
            int(nestedFileDiff.size())).toStdString();
        tally.gate(2, roundtrip && legacyOk && diffOk, detail);
    }

    // G3: duplicate uids within one setClips.
    {
        TimelineTrack track;
        ClipInfo first = clipFor(QStringLiteral("/selftests/dup-0.mp4"));
        first.uid = QStringLiteral("dup");
        ClipInfo second = clipFor(QStringLiteral("/selftests/dup-1.mp4"));
        second.uid = QStringLiteral("dup");
        ClipInfo third = clipFor(QStringLiteral("/selftests/dup-2.mp4"));
        third.uid = QStringLiteral("other");
        const quint64 before = track.duplicateUidRepairCountForTest();
        track.setClips({first, second, third});
        const QStringList repaired = uidsOf(track.clips());
        const quint64 afterDuplicate = track.duplicateUidRepairCountForTest();
        const bool repairedOk = before == 0 && afterDuplicate == 1
            && repaired.value(0) == QStringLiteral("dup")
            && !repaired.value(1).isEmpty() && repaired.value(1) != QStringLiteral("dup")
            && repaired.value(2) == QStringLiteral("other");

        // Control: distinct uids are kept and nothing is counted.
        const QStringList distinctUids{QStringLiteral("u0"), QStringLiteral("u1")};
        first.uid = distinctUids.value(0);
        second.uid = distinctUids.value(1);
        track.setClips({first, second});
        const QStringList kept = uidsOf(track.clips());
        const quint64 afterDistinct = track.duplicateUidRepairCountForTest();
        const bool controlOk = kept == distinctUids && afterDistinct == 1;

        const std::string detail = QString::asprintf(
            "before=%llu afterDuplicate=%llu repaired=[%s] controlKept=[%s] afterDistinct=%llu",
            static_cast<unsigned long long>(before), static_cast<unsigned long long>(afterDuplicate),
            qPrintable(joined(repaired)), qPrintable(joined(kept)),
            static_cast<unsigned long long>(afterDistinct)).toStdString();
        tally.gate(3, repairedOk && controlOk, detail);
    }

    // G4: undo / redo keep the uids of each state.
    {
        Timeline timeline;
        timeline.restoreFromProject(
            QVector<QVector<ClipInfo>>{{clipFor(QStringLiteral("/selftests/uid-v0.mp4")),
                                        clipFor(QStringLiteral("/selftests/uid-v1.mp4"))}},
            QVector<QVector<ClipInfo>>{{clipFor(QStringLiteral("/selftests/uid-a0.wav")),
                                        clipFor(QStringLiteral("/selftests/uid-a1.wav"))}},
            0.0, -1.0, -1.0, 100);
        TimelineTrack *v1 = timeline.trackAt(false, 0);
        TimelineTrack *a1 = timeline.trackAt(true, 0);
        auto snapshot = [&] { return uidsOf(v1->clips()) + QStringList{QStringLiteral("|")}
                                     + uidsOf(a1->clips()); };
        const QStringList beforeEdit = snapshot();
        v1->setSelectedClip(0);
        timeline.copySelectedClip();
        timeline.pasteClip();
        const QStringList afterEdit = snapshot();
        timeline.undo();
        const QStringList undone = snapshot();
        timeline.redo();
        const QStringList redone = snapshot();
        // Control: the paste really changed the state (one more V and A clip).
        const bool edited = afterEdit.size() == beforeEdit.size() + 2;
        const bool ok = edited && beforeEdit.size() == 5 && !beforeEdit.contains(QString())
            && undone == beforeEdit && redone == afterEdit;
        const std::string detail = QString::asprintf(
            "edited=%d before=[%s] after=[%s] undone=[%s] redone=[%s]", int(edited),
            qPrintable(joined(beforeEdit)), qPrintable(joined(afterEdit)),
            qPrintable(joined(undone)), qPrintable(joined(redone))).toStdString();
        tally.gate(4, ok, detail);
    }

    return tally.finish("clip-uid");
}
