#include "../Timeline.h"
#include "../UndoManager.h"

#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QPointer>
#include <QSignalSpy>
#include <QVBoxLayout>
#include <cstdio>

namespace {
ClipInfo clip(int id)
{
    ClipInfo c;
    c.filePath = QStringLiteral("track-ops-%1.mov").arg(id);
    c.displayName = QString::number(id);
    c.duration = id + 2.0;
    c.outPoint = c.duration;
    c.opacity = 0.5;
    return c;
}

void setup(Timeline &t)
{
    const QVector<QVector<ClipInfo>> rows{{clip(0)}, {clip(1)}, {clip(2)}};
    t.restoreFromProject(rows, rows, 0.0, -1.0, -1.0, 100);
}

void baseline(Timeline &t)
{
    t.undoManager()->clear();
    t.undoManager()->saveState(t.currentState(), QStringLiteral("基準"));
}

bool order(const QVector<QVector<ClipInfo>> &rows, const QVector<int> &ids)
{
    if (rows.size() < ids.size()) return false;
    for (int i = 0; i < ids.size(); ++i) {
        const ClipInfo expected = clip(ids[i]);
        if (rows[i].size() != 1) return false;
        const auto &c = rows[i][0];
        if (c.filePath != expected.filePath || c.displayName != expected.displayName
            || c.duration != expected.duration || c.outPoint != expected.outPoint
            || c.opacity != expected.opacity) return false;
    }
    return true;
}

bool layoutOrder(Timeline &t, bool audio)
{
    const auto tracks = audio ? t.audioTracks() : t.videoTracks();
    for (int i = 0; i < tracks.size(); ++i) {
        auto *layout = qobject_cast<QVBoxLayout *>(tracks[i]->parentWidget()->layout());
        const int row = audio ? t.videoTrackCount() + 2 + i : 1 + i;
        if (!layout || layout->indexOf(tracks[i]) != row) return false;
        const QString name = QStringLiteral("%1%2")
            .arg(audio ? QStringLiteral("A") : QStringLiteral("V")).arg(i + 1);
        bool headerFound = false;
        for (auto *label : t.findChildren<QLabel *>(QStringLiteral("timelineTrackName"))) {
            auto *header = label->parentWidget();
            if (label->property("defaultName").toString() != name) continue;
            auto *headers = header->parentWidget()->layout();
            if (headers->indexOf(header) < 0) continue; // awaiting deleteLater
            if (headers->indexOf(header) != row + 1) return false;
            headerFound = true;
        }
        if (!headerFound) return false;
    }
    return true;
}
}

int runTrackOpsSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "[track-ops] %s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    {
        Timeline t;
        setup(t);
        t.videoTracks()[2]->setSelectedClip(0);
        baseline(t);
        const auto serial = t.undoManager()->saveSerial();
        QPointer<TimelineTrack> removed(t.videoTracks()[1]);
        QString err;
        bool ok = t.removeTrack(false, 1, &err) && err.isEmpty()
            && t.videoTrackCount() == 2 && order(t.currentState().videoTracks, {0, 2})
            && t.currentState().selectedVideoTrackIndex == 1
            && t.currentState().activeVideoTrackIndex == 1
            && t.undoManager()->saveSerial() == serial + 1 && layoutOrder(t, false)
            && removed && removed->isHidden();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ok = ok && removed.isNull();
        t.undo();
        ok = ok && t.videoTrackCount() >= 3 && order(t.currentState().videoTracks, {0, 1, 2})
            && t.currentState().selectedVideoTrackIndex == 2;
        baseline(t);
        ok = t.removeTrack(true, 0, &err) && ok
            && order(t.currentState().audioTracks, {1, 2}) && layoutOrder(t, true);
        t.undo();
        gate(1, ok && order(t.currentState().audioTracks, {0, 1, 2}));
    }
    {
        Timeline t;
        setup(t);
        TimelineSequence inactive;
        inactive.id = QStringLiteral("inactive");
        inactive.name = QStringLiteral("非アクティブ");
        inactive.videoTracks = {{clip(9)}};
        t.addSequence(inactive);
        t.videoTracks()[0]->customName = QStringLiteral("先頭");
        t.videoTracks()[0]->setSelectedClip(0);
        baseline(t);
        auto *first = t.videoTracks()[0];
        const auto serial = t.undoManager()->saveSerial();
        bool ok = t.moveTrack(false, 0, 2) && t.videoTracks()[2] == first
            && order(t.currentState().videoTracks, {1, 2, 0})
            && t.currentState().selectedVideoTrackIndex == 2
            && t.currentState().selectedClip == -1
            && t.currentState().videoTrackNames[2] == QStringLiteral("先頭")
            && t.undoManager()->saveSerial() == serial + 1 && layoutOrder(t, false);
        for (const auto &s : t.sequences()) {
            if (s.id == inactive.id) ok = ok && order(s.videoTracks, {9});
            if (s.id == t.activeSequenceId()) ok = ok && order(s.videoTracks, {1, 2, 0});
        }
        t.undo();
        ok = ok && order(t.currentState().videoTracks, {0, 1, 2});
        t.audioTracks()[2]->setSelectedClip(0);
        baseline(t);
        ok = t.moveTrack(true, 2, 0) && ok && order(t.currentState().audioTracks, {2, 0, 1})
            && layoutOrder(t, true) && t.currentState().selectedAudioTrackIndex == 0
            && t.currentState().activeAudioTrackIndex == 0;
        t.undo();
        gate(2, ok && order(t.currentState().audioTracks, {0, 1, 2})
            && t.currentState().selectedAudioTrackIndex == 2
            && t.currentState().activeAudioTrackIndex == 2);
    }
    {
        Timeline t;
        setup(t);
        TimelineTrackMatteEntry matte;
        matte.matteType = TrackMatteType::AlphaMatte;
        matte.matteSourceClipId = QStringLiteral("1:0");
        t.setTrackMatteEntries({{QStringLiteral("2:0"), matte}});
        t.setClipParent(QStringLiteral("2:0"), QStringLiteral("1:0"));
        baseline(t);
        bool ok = t.removeTrack(false, 0)
            && t.trackMatteEntries().size() == 1
            && t.trackMatteEntries().value(QStringLiteral("1:0")).matteSourceClipId == QStringLiteral("0:0")
            && t.clipParentEntries().value(QStringLiteral("1:0")) == QStringLiteral("0:0");
        t.undo();
        ok = ok && t.trackMatteEntries().size() == 1
            && t.currentState().trackMatteEntries.value(QStringLiteral("2:0")).matteSourceClipId == QStringLiteral("1:0")
            && t.trackMatteEntries().value(QStringLiteral("2:0")).matteType == TrackMatteType::AlphaMatte
            && t.clipParentEntries().value(QStringLiteral("2:0")) == QStringLiteral("1:0");
        ok = t.removeTrack(false, 1) && ok && t.trackMatteEntries().isEmpty()
            && t.clipParentEntries().isEmpty();
        t.undo();
        ok = t.moveTrack(false, 0, 2) && ok
            && t.trackMatteEntries().value(QStringLiteral("1:0")).matteSourceClipId == QStringLiteral("0:0");
        gate(3, ok);
    }
    {
        Timeline t;
        QSignalSpy spy(&t, &Timeline::trackIndicesRemapped);
        QString err;
        const auto serial = t.undoManager()->saveSerial();
        bool ok = spy.isValid() && !t.removeTrack(false, 0, &err) && !err.isEmpty()
            && !t.removeTrack(true, 0, &err) && !err.isEmpty()
            && !t.removeTrack(false, -1, &err) && !err.isEmpty()
            && !t.moveTrack(true, 0, 1, &err) && !err.isEmpty()
            && t.moveTrack(false, 0, 0, &err) && err.isEmpty()
            && serial == t.undoManager()->saveSerial() && spy.isEmpty();
        setup(t);
        for (bool audio : {false, true}) {
            t.trackAt(audio, 1)->setLocked(true);
            const auto lockedSerial = t.undoManager()->saveSerial();
            ok = !t.removeTrack(audio, 1, &err) && !err.isEmpty() && ok
                && lockedSerial == t.undoManager()->saveSerial() && spy.isEmpty();
            t.trackAt(audio, 1)->setLocked(false);
        }
        ok = t.removeTrack(false, 1, &err) && ok && spy.size() == 1;
        if (spy.size() == 1) {
            ok = ok && !spy[0][0].toBool()
                && qvariant_cast<QVector<int>>(spy[0][1]) == QVector<int>({0, -1, 1});
        }
        spy.clear();
        ok = t.moveTrack(true, 0, 2, &err) && ok && spy.size() == 1;
        if (spy.size() == 1) {
            ok = ok && spy[0][0].toBool()
                && qvariant_cast<QVector<int>>(spy[0][1]) == QVector<int>({2, 0, 1});
        }
        gate(4, ok);
    }
    std::fprintf(stderr, "[track-ops] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
