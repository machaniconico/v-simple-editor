#include "../EdlImport.h"
#include "../UndoManager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

int runEdlImportSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    QTemporaryDir dir;
    QVector<ClipInfo> source;
    for (int i = 0; i < 3; ++i) {
        ClipInfo clip;
        clip.filePath = dir.filePath(QStringLiteral("missing%1.mov").arg(i));
        clip.displayName = QFileInfo(clip.filePath).fileName();
        clip.inPoint = i + 1; clip.outPoint = clip.duration = i + 5;
        clip.leadInSec = i + 2; clip.speed = i == 1 ? 2.0 : 1.0;
        source.append(clip);
    }
    bool roundtrip = dir.isValid();
    for (double fps : {24.0, 25.0, 30.0, 30000.0 / 1001.0, 60000.0 / 1001.0}) {
        const auto original = edl::fromClips(source, QStringLiteral("往復"), fps, true);
        const QString path = dir.filePath(QStringLiteral("roundtrip.edl"));
        QFile file(path);
        const QByteArray text = edl::toCmx3600(original).toUtf8();
        const bool opened = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        roundtrip &= opened && file.write(text) == text.size();
        file.close();
        auto parsed = edl::readEdl(path, fps);
        roundtrip &= parsed.error.isEmpty() && parsed.videoTracks[0].size() == source.size();
        const auto exported = edl::fromClips(parsed.videoTracks[0], original.title, fps, true);
        roundtrip &= exported.events.size() == original.events.size();
        for (int i = 0; i < qMin(exported.events.size(), original.events.size()); ++i) {
            const auto &a = original.events[i]; const auto &b = exported.events[i];
            roundtrip &= a.srcInFrames == b.srcInFrames && a.srcOutFrames == b.srcOutFrames
                && a.recInFrames == b.recInFrames && a.recOutFrames == b.recOutFrames;
        }
        for (qint64 frame : {qint64(0), qint64(1800), qint64(17982), qint64(107892), qint64(2500000)}) {
            qint64 inverse = -1;
            roundtrip &= edl::timecodeToFrames(edl::framesToTimecode(frame, fps, true), fps, true, &inverse)
                && inverse == frame % edl::secToFrames(86400.0, fps);
        }
    }
    gate(1, roundtrip);

    const QString dissolve = QStringLiteral("TITLE: Dissolve\nFCM: NON-DROP FRAME\n"
        "001 OLD V C 00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"
        "002 OLD V C 00:00:01:00 00:00:01:00 00:00:01:00 00:00:01:00\n"
        "002 NEW V D 012 00:00:02:00 00:00:04:00 00:00:01:00 00:00:03:00\n"
        "* FROM CLIP NAME: new.mov\n"
        "003 SOUND A C 00:00:00:00 00:00:03:00 00:00:00:00 00:00:03:00\n");
    auto transitions = edl::parseEdl(dissolve, 24.0);
    edl::resolveClips(transitions, dir.path());
    gate(2, transitions.error.isEmpty() && transitions.videoTracks[0].size() == 2
        && transitions.videoTracks[0][1].leadIn.type == TransitionType::CrossDissolve
        && transitions.videoTracks[0][1].leadIn.duration == 0.5
        && transitions.videoTracks[0][1].inPoint == 2.0
        && transitions.audioTracks[0].size() == 1);

    auto missing = edl::parseEdl(QStringLiteral("001 UNKNOWN V C 00:00:00:00 00:00:01:00 00:00:02:00 00:00:03:00\nM2 unsupported\n"), 24.0);
    edl::resolveClips(missing, dir.path());
    // Use a real video fixture for resolution and the production relink API.
    const QString mediaPath = dir.filePath(QStringLiteral("found.mp4"));
    const bool fixture = QFile::copy(QDir::current().absoluteFilePath(QStringLiteral("test_assets/e2e_clip.mp4")), mediaPath);
    auto found = edl::parseEdl(QStringLiteral("001 FOUND V C 00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"
        "* FROM CLIP NAME: found.mp4\n"), 24.0);
    edl::resolveClips(found, dir.path());
    Timeline relink;
    bool repaired = fixture && missing.videoTracks[0].size() == 1 && relink.createSequence(missing.document.title, missing.videoTracks, missing.audioTracks);
    if (repaired) repaired = relink.relinkMediaPaths({{missing.videoTracks[0][0].filePath, mediaPath}});
    repaired = repaired && relink.currentState().videoTracks[0][0].filePath == mediaPath;
    qint64 ignored = 0;
    gate(3, repaired && found.resolvedFiles == 1 && found.unresolvedFiles == 0
        && found.warnings.isEmpty() && missing.error.isEmpty() && missing.unresolvedFiles == 1 && missing.resolvedFiles == 0
        && missing.warnings.size() == 2 && missing.videoTracks[0].size() == 1
        && !QFileInfo::exists(missing.videoTracks[0][0].filePath)
        && QFileInfo(missing.videoTracks[0][0].filePath).fileName() == QStringLiteral("UNKNOWN")
        && !edl::timecodeToFrames(QStringLiteral("00:01:00;00"), 29.97, true, &ignored)
        && !edl::readEdl(dir.filePath(QStringLiteral("absent.edl")), 24.0).error.isEmpty());

    Timeline timeline;
    const QVector<QVector<ClipInfo>> video{source};
    const QVector<QVector<ClipInfo>> audio{QVector<ClipInfo>{}};
    timeline.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    timeline.createSequence(QStringLiteral("別のシーケンス"));
    timeline.setActiveSequence(timeline.sequenceList().first().id);
    const auto before = timeline.currentState();
    const QString active = timeline.activeSequenceId();
    const auto count = timeline.sequenceList().size();
    const auto serial = timeline.undoManager()->saveSerial();
    const int index = timeline.undoManager()->currentIndex();
    const bool created = timeline.createSequence(missing.document.title, missing.videoTracks, missing.audioTracks);
    const QString imported = timeline.activeSequenceId();
    bool undo = created && timeline.sequenceList().size() == count + 1 && active != imported
        && timeline.undoManager()->saveSerial() == serial + 1;
    timeline.undo();
    const auto restored = timeline.currentState();
    undo &= timeline.activeSequenceId() == active && timeline.sequenceList().size() == count
        && timeline.undoManager()->currentIndex() == index
        && restored.clipParentEntries == before.clipParentEntries
        && restored.videoTracks[0].size() == source.size()
        && restored.videoTracks[0][0].filePath == source[0].filePath;
    timeline.redo();
    undo &= timeline.activeSequenceId() == imported && timeline.sequenceList().size() == count + 1;
    gate(4, undo);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
