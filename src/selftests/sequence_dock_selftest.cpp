#include "../SequenceListDock.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../ProjectFile.h"

#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <cstdio>

int runSequenceDockSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    Timeline timeline;
    SequenceListDock dock(&timeline);
    auto *list = dock.findChild<QListWidget *>(QStringLiteral("sequenceList"));
    const bool implicitRoot = list && list->count() == 1
        && list->item(0)->text() == QStringLiteral("メインシーケンス")
        && list->item(0)->font().bold() && timeline.sequences().isEmpty();
    ClipInfo clip;
    clip.filePath = QStringLiteral("test_assets/sequence-dock.mp4");
    clip.displayName = QStringLiteral("子クリップ");
    clip.duration = 2.0;
    clip.outPoint = 2.0;
    TimelineSequence child;
    child.id = QStringLiteral("child");
    child.name = QStringLiteral("子シーケンス");
    child.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    const bool added = timeline.addSequence(child) && timeline.addSequenceClip(child.id);
    gate(1, implicitRoot && added && list->count() == 2
        && list->item(0)->text() == QStringLiteral("メインシーケンス")
        && list->item(1)->text() == child.name
        && list->item(0)->font().bold() && !list->item(1)->font().bold());

    QSignalSpy changed(&timeline, &Timeline::sequencesChanged);
    const auto serial = timeline.undoManager()->saveSerial();
    const bool renamed = timeline.renameSequence(child.id, QStringLiteral("変更後"));
    const bool renameState = renamed && changed.count() == 1
        && list->item(1)->text() == QStringLiteral("変更後")
        && timeline.undoManager()->saveSerial() == serial + 1;
    timeline.undo();
    const bool undoState = timeline.sequenceList().size() == 2
        && timeline.sequenceList().at(1).name == child.name
        && changed.count() == 2;
    const auto createSerial = timeline.undoManager()->saveSerial();
    auto *create = dock.findChild<QPushButton *>(QStringLiteral("createSequence"));
    if (create) create->click();
    const bool created = create && timeline.sequenceList().size() == 3
        && timeline.activeSequenceId() != QStringLiteral("main")
        && timeline.undoManager()->saveSerial() == createSerial + 1;
    timeline.undo();
    Timeline fresh;
    const auto freshSerial = fresh.undoManager()->saveSerial();
    const bool rootRenamed = fresh.renameSequence(QStringLiteral("main"), QStringLiteral("ルート名"))
        && fresh.sequenceList().first().name == QStringLiteral("ルート名")
        && fresh.undoManager()->saveSerial() == freshSerial + 1;
    fresh.undo();
    const bool rootUndone = fresh.sequences().isEmpty()
        && fresh.sequenceList().first().name == QStringLiteral("メインシーケンス");
    const auto invalidSerial = fresh.undoManager()->saveSerial();
    const bool invalidRejected = !fresh.renameSequence(QStringLiteral("missing"), QStringLiteral("名前"))
        && !fresh.renameSequence(QStringLiteral("main"), QStringLiteral("  "))
        && fresh.undoManager()->saveSerial() == invalidSerial && fresh.sequences().isEmpty();
    const bool firstCreated = fresh.createSequence(QStringLiteral("最初の子"))
        && fresh.sequenceList().size() == 2
        && fresh.undoManager()->saveSerial() == invalidSerial + 1;
    fresh.undo();
    gate(2, changed.isValid() && renameState && undoState && created
        && list->count() == 2 && timeline.activeSequenceId() == QStringLiteral("main")
        && rootRenamed && rootUndone && invalidRejected && firstCreated
        && fresh.sequences().isEmpty());

    const auto switchSerial = timeline.undoManager()->saveSerial();
    const bool switched = timeline.setActiveSequence(child.id);
    const auto state = timeline.currentState();
    const bool childState = !state.videoTracks.isEmpty() && state.videoTracks[0].size() == 1
        && state.videoTracks[0][0].filePath == clip.filePath;
    const bool bold = list->item(1)->font().bold() && !list->item(0)->font().bold();
    const bool back = timeline.setActiveSequence(QStringLiteral("main"));
    const auto root = timeline.currentState();
    gate(3, switched && childState && bold && back
        && !root.videoTracks.isEmpty() && root.videoTracks[0].size() == 1
        && root.videoTracks[0][0].sequenceRefId == child.id
        && timeline.undoManager()->saveSerial() == switchSerial
        && !timeline.setActiveSequence(QStringLiteral("missing")));

    timeline.renameSequence(child.id, QStringLiteral("保存する名前"));
    ProjectData data;
    data.videoTracks = timeline.allVideoTracks();
    data.audioTracks = timeline.allAudioTracks();
    const auto entries = timeline.clipParentEntries();
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        ClipParentEntry entry;
        entry.clipId = it.key();
        entry.parentClipId = it.value();
        data.clipParentEntries.append(entry);
    }
    QTemporaryDir dir;
    ProjectData loaded;
    const bool roundtrip = dir.isValid()
        && ProjectFile::save(dir.filePath(QStringLiteral("sequence.vsep")), data)
        && ProjectFile::load(dir.filePath(QStringLiteral("sequence.vsep")), loaded);
    Timeline restored;
    restored.restoreFromProject(loaded.videoTracks, loaded.audioTracks, 0.0, -1.0, -1.0, 100);
    QHash<QString, QString> loadedEntries;
    for (const auto &entry : loaded.clipParentEntries)
        loadedEntries.insert(entry.clipId, entry.parentClipId);
    restored.setClipParentEntries(loadedEntries);
    const auto sequences = restored.sequenceList();
    gate(4, roundtrip && sequences.size() == 2
        && sequences[1].id == child.id && sequences[1].name == QStringLiteral("保存する名前"));
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
