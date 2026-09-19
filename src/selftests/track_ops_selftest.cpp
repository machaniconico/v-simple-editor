#include "../Timeline.h"
#include "../UndoManager.h"
#include "../AudioMixer.h"
#include "../AudioBusRouting.h"
#include "../ProjectFile.h"
#include "../MainWindow.h"
#include <QJsonArray>
#include <memory>

#include <QCoreApplication>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QAction>
#include <QAbstractButton>
#include <QPointer>
#include <QSignalSpy>
#include <QTimer>
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

// Open the actual header context menu and trigger its QAction, including the
// nested confirmation dialog. No duplicate implementation of track mutation.
bool headerAction(Timeline &t, bool audio, int index, const QString &text,
                  bool enabled, bool trigger = false, bool confirm = true)
{
    QWidget *header = nullptr;
    const QString name = QStringLiteral("%1%2").arg(audio ? QStringLiteral("A") : QStringLiteral("V")).arg(index + 1);
    for (auto *label : t.findChildren<QLabel *>(QStringLiteral("timelineTrackName"))) {
        auto *candidate = label->parentWidget();
        if (label->property("defaultName").toString() == name
            && candidate->parentWidget()->layout()->indexOf(candidate) >= 0) {
            header = candidate;
            break;
        }
    }
    if (!header) return false;
    bool checked = false;
    QTimer::singleShot(0, &t, [&]() {
        auto *menu = header->findChild<QMenu *>(QString(), Qt::FindDirectChildrenOnly);
        if (!menu) return;
        for (auto *action : menu->actions()) {
            if (action->text() != text) continue;
            checked = action->isEnabled() == enabled;
            if (trigger && action->isEnabled()) {
                if (text == QStringLiteral("トラックを削除") && !t.trackAt(audio, index)->clips().isEmpty()) {
                    QTimer::singleShot(0, &t, [&]() {
                        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                        checked = checked && box && box->text().contains(QStringLiteral("1 個のクリップも削除されます"));
                        if (box) box->button(confirm ? QMessageBox::Yes : QMessageBox::No)->click();
                    });
                }
                action->trigger();
            }
            break;
        }
        menu->close();
    });
    QMetaObject::invokeMethod(header, "customContextMenuRequested", Qt::DirectConnection,
                              Q_ARG(QPoint, QPoint(1, 1)));
    return checked;
}

bool headerOperations()
{
    Timeline t;
    setup(t);
    bool ok = true;
    for (bool audio : {false, true}) {
        baseline(t);
        auto serial = t.undoManager()->saveSerial();
        ok = headerAction(t, audio, 0, QStringLiteral("上へ移動"), false) && ok;
        ok = headerAction(t, audio, 2, QStringLiteral("下へ移動"), false) && ok;
        ok = headerAction(t, audio, 1, QStringLiteral("トラックを削除"), true, true, false) && ok
            && t.undoManager()->saveSerial() == serial;
        ok = headerAction(t, audio, 1, QStringLiteral("トラックを削除"), true, true) && ok
            && (audio ? t.audioTrackCount() : t.videoTrackCount()) == 2
            && order(audio ? t.currentState().audioTracks : t.currentState().videoTracks, {0, 2})
            && t.undoManager()->saveSerial() == serial + 1;
        t.undo();
        ok = ok && order(audio ? t.currentState().audioTracks : t.currentState().videoTracks, {0, 1, 2})
            && !t.canUndo();
        t.trackAt(audio, 1)->setLocked(true);
        ok = headerAction(t, audio, 1, QStringLiteral("トラックを削除"), false) && ok;
        t.trackAt(audio, 1)->setLocked(false);
        for (int direction : {-1, 1}) {
            baseline(t);
            serial = t.undoManager()->saveSerial();
            ok = headerAction(t, audio, 1, direction < 0 ? QStringLiteral("上へ移動") : QStringLiteral("下へ移動"), true, true) && ok
                && order(audio ? t.currentState().audioTracks : t.currentState().videoTracks,
                         direction < 0 ? QVector<int>{1, 0, 2} : QVector<int>{0, 2, 1})
                && t.undoManager()->saveSerial() == serial + 1;
            t.undo();
            ok = ok && order(audio ? t.currentState().audioTracks : t.currentState().videoTracks, {0, 1, 2})
                && !t.canUndo();
        }
    }
    Timeline single;
    for (bool audio : {false, true}) {
        ok = headerAction(single, audio, 0, QStringLiteral("トラックを削除"), false) && ok;
        ok = headerAction(single, audio, 0, QStringLiteral("上へ移動"), false) && ok;
        ok = headerAction(single, audio, 0, QStringLiteral("下へ移動"), false) && ok;
    }
    return ok;
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
            && removed && removed->QWidget::isHidden();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ok = ok && removed.isNull();
        t.undo();
        ok = ok && t.videoTrackCount() == 3 && order(t.currentState().videoTracks, {0, 1, 2})
            && t.currentState().selectedVideoTrackIndex == 2;
        baseline(t);
        ok = t.removeTrack(true, 0, &err) && ok
            && order(t.currentState().audioTracks, {1, 2}) && layoutOrder(t, true);
        t.undo();
        gate(1, ok && t.audioTrackCount() == 3
            && order(t.currentState().audioTracks, {0, 1, 2}));
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
    {
        Timeline t;
        setup(t);
        QJsonObject external{{"trackIndex", 2}, {"payload", QStringLiteral("保持")}};
        t.setExternalTrackStateHooks([&]() { return external; },
            [&](const QJsonObject &state) { external = state; });
        QObject::connect(&t, &Timeline::trackIndicesRemapped, &t,
            [&](bool audio, const QVector<int> &map) {
                if (!audio) external["trackIndex"] = map.value(external["trackIndex"].toInt(), -1);
            });
        baseline(t);
        const QJsonObject original = external;
        bool ok = t.removeTrack(false, 0) && external["trackIndex"].toInt() == 1;
        t.undo();
        ok = ok && external == original;
        t.redo();
        ok = ok && external["trackIndex"].toInt() == 1;
        t.undo();
        ok = t.moveTrack(false, 2, 0) && ok && external["trackIndex"].toInt() == 0;
        t.undo();
        ok = ok && external == original;
        // Capture external edits made since the last timeline undo snapshot.
        external["payload"] = QStringLiteral("更新");
        ok = t.removeTrack(false, 2) && ok && external["trackIndex"].toInt() == -1;
        t.undo();
        gate(5, ok && external["trackIndex"].toInt() == 2
            && external["payload"].toString() == QStringLiteral("更新"));
    }
    {
        Timeline t;
        baseline(t);
        const auto serial = t.undoManager()->saveSerial();
        t.addVideoTrack();
        QPointer<TimelineTrack> added(t.videoTracks().last());
        bool ok = t.videoTrackCount() == 2 && t.undoManager()->saveSerial() == serial + 1;
        t.undo();
        ok = ok && t.videoTrackCount() == 1 && added && added->QWidget::isHidden()
            && t.currentState().activeVideoTrackIndex < t.videoTrackCount();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ok = ok && added.isNull();
        t.redo();
        ok = ok && t.videoTrackCount() == 2;
        ok = t.removeTrack(false, 1) && ok && t.videoTrackCount() == 1;
        t.undo();
        ok = ok && t.videoTrackCount() == 2;
        t.redo();
        ok = ok && t.videoTrackCount() == 1;
        t.addAudioTrack();
        ok = ok && t.audioTrackCount() == 2;
        t.undo();
        gate(6, ok && t.audioTrackCount() == 1 && layoutOrder(t, false) && layoutOrder(t, true));
    }
    {
        Timeline t;
        setup(t);
        auto mixer = std::make_unique<AudioMixer>();
        audiobus::AudioBusRouting routing;
        const int bus = routing.addBus(QStringLiteral("バス"));
        routing.assignTrackToBus(1, bus);
        routing.addAuxSend(audiobus::AuxSend{1, bus, 0.25, true});
        trackfx::Chain chain;
        chain.eqEnabled = true;
        chain.eq.low.gainDb = 3.0;
        mixer->setTrackChain(1, chain);
        mixer->setTrackGain(1, 0.4);
        mixer->setTrackMute(1, true);
        mixer->setTrackSolo(1, true);
        const auto initialMixer = mixer->collectTrackState();
        const auto initialRouting = routing.toJson();
        t.setExternalTrackStateHooks([&]() {
            return QJsonObject{{"mixer", mixer->collectTrackState()}, {"buses", routing.toJson()}};
        }, [&](const QJsonObject &state) {
            mixer->applyTrackState(state["mixer"].toObject());
            routing.fromJson(state["buses"].toObject());
        });
        QObject::connect(&t, &Timeline::trackIndicesRemapped, &t,
            [&](bool audio, const QVector<int> &map) {
                if (audio) { mixer->remapTrackIndices(map); routing.remapTrackIndices(map); }
            });
        baseline(t);
        bool ok = t.removeTrack(false, 0) && mixer->trackChain(1).toJson() == chain.toJson();
        ok = t.removeTrack(true, 0) && ok && mixer->trackChain(0).toJson() == chain.toJson()
            && mixer->trackChain(1).isDefault() && mixer->trackGain(0) == 0.4
            && routing.trackBus(0) == bus && routing.auxSends().size() == 1
            && routing.auxSends()[0].trackIndex == 0;
        ProjectData saved, loaded;
        saved.videoTracks = t.currentState().videoTracks;
        saved.audioTracks = t.currentState().audioTracks;
        saved.trackFx.insert(0, mixer->trackChain(0));
        saved.audioBusRouting = routing;
        ok = ProjectFile::fromJsonString(ProjectFile::toJsonString(saved), loaded) && ok
            && loaded.videoTracks.size() == 2 && loaded.audioTracks.size() == 2
            && order(loaded.videoTracks, {1, 2}) && order(loaded.audioTracks, {1, 2})
            && loaded.trackFx.value(0).toJson() == chain.toJson()
            && loaded.audioBusRouting.toJson() == routing.toJson();
        t.undo();
        ok = ok && t.audioTrackCount() == 3 && mixer->collectTrackState() == initialMixer
            && routing.toJson() == initialRouting;
        t.undo();
        ok = ok && t.videoTrackCount() == 3;
        ok = t.moveTrack(true, 1, 2) && ok && mixer->trackChain(2).toJson() == chain.toJson()
            && routing.trackBus(2) == bus;
        t.undo();
        gate(7, ok && mixer->collectTrackState() == initialMixer && routing.toJson() == initialRouting);
    }
    {
        Timeline t;
        setup(t);
        auto mixer = std::make_unique<AudioMixer>();
        int applied = 0;
        t.setExternalTrackStateHooks([&]() { return mixer->collectTrackState(); },
            [&](const QJsonObject &state) { ++applied; mixer->applyTrackState(state); });
        QObject::connect(&t, &Timeline::trackIndicesRemapped, &t,
            [&](bool audio, const QVector<int> &map) {
                if (audio) mixer->remapTrackIndices(map);
            });
        baseline(t);
        // An ordinary edit adjacent to a structural entry must not inherit
        // its restore policy, in either direction or through history jumps.
        bool ok = t.moveTrack(true, 1, 2)
            && t.setTrackCustomName(false, 0, QStringLiteral("改名"));
        mixer->setTrackSolo(1, true);
        mixer->setTrackMute(1, true);
        mixer->setTrackGain(1, 0.4);
        mixer->setTrackEqEnabled(1, true);
        const auto live = mixer->collectTrackState();
        t.undo();
        ok = ok && applied == 0 && mixer->collectTrackState() == live
            && mixer->collectTrackState()["states"].toArray()[1].toObject()["solo"].toBool();
        t.redo();
        ok = ok && applied == 0 && mixer->collectTrackState() == live;
        QObject::connect(t.undoManager(), &UndoManager::stateJumpRequested,
                         &t, &Timeline::restoreState);
        ok = t.undoManager()->jumpTo(1) && ok
            && applied == 0 && mixer->collectTrackState() == live;
        ok = t.undoManager()->jumpTo(0) && ok && applied == 1;
        ok = t.undoManager()->jumpTo(2) && ok && applied == 2;
        const bool headerOk = headerOperations();
        gate(8, ok && headerOk);
    }
    {
        // Exercise the real MainWindow hook, which used to overwrite the
        // carrier with its stale pre-split sidecar on redo.
        MainWindow window;
        auto *t = window.findChild<Timeline *>();
        bool ok = t != nullptr;
        if (t) {
            const QVector<QVector<ClipInfo>> video{{clip(0), clip(1)}};
            const QVector<QVector<ClipInfo>> audio{{}};
            t->restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            TimelineTrackMatteEntry matte;
            matte.matteType = TrackMatteType::AlphaMatte;
            matte.matteSourceClipId = QStringLiteral("0:0");
            t->setTrackMatteEntries({{QStringLiteral("0:1"), matte}});
            baseline(*t);
            // Synchronize the window's sidecar to the initial carrier via
            // the production restore notification, before splitting.
            t->restoreState(t->currentState());
            QString error;
            ok = t->splitClipByIndex(false, 0, 0, 1.0, &error) && ok
                && t->trackMatteEntries().contains(QStringLiteral("0:2"));
            t->undo();
            ok = ok && t->trackMatteEntries().contains(QStringLiteral("0:1"))
                && !t->trackMatteEntries().contains(QStringLiteral("0:2"));
            t->redo();
            ok = ok && t->trackMatteEntries().contains(QStringLiteral("0:2"))
                && !t->trackMatteEntries().contains(QStringLiteral("0:1"));
            // Also cross a structural boundary, where external apply runs.
            t->addVideoTrack();
            t->undo();
            ok = ok && t->videoTrackCount() == 1
                && t->trackMatteEntries().contains(QStringLiteral("0:2"));
        }
        gate(9, ok);
    }
    {
        Timeline t;
        t.setProjectOutputConfig(1920, 1080, false);
        baseline(t);
        const auto serial = t.undoManager()->saveSerial();
        // SNS uses a compound snapshot: adding V2 must not push its own undo.
        t.setProjectOutputConfig(1080, 1920, true);
        t.addVideoTrack(false);
        t.undoManager()->saveState(t.currentState(), QStringLiteral("Apply SNS preset"));
        bool ok = t.undoManager()->saveSerial() == serial + 1;
        t.undo();
        ok = ok && t.videoTrackCount() == 1 && !t.canUndo()
            && t.currentState().projectWidth == 1920
            && t.currentState().projectHeight == 1080
            && !t.currentState().projectExplicitOutput;
        t.redo();
        gate(10, ok && t.videoTrackCount() == 2
            && t.currentState().projectWidth == 1080
            && t.currentState().projectHeight == 1920);
    }
    {
        MainWindow window;
        auto *t = window.findChild<Timeline *>();
        bool ok = t != nullptr;
        if (t) {
            const QVector<QVector<ClipInfo>> video{{clip(0), clip(1)}, {clip(2)}};
            const QVector<QVector<ClipInfo>> audio{{}};
            t->restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            baseline(*t);
            t->restoreState(t->currentState());
            t->videoTracks()[0]->setSelectedClip(1);
            const auto serial = t->undoManager()->saveSerial();
            bool configured = false;
            QTimer::singleShot(0, &window, [&]() {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog) return;
                const auto combos = dialog->findChildren<QComboBox *>();
                if (combos.size() == 2) {
                    const int type = combos[0]->findData(int(TrackMatteType::AlphaMatte));
                    const int source = combos[1]->findData(QStringLiteral("1:0"));
                    configured = type >= 0 && source >= 0;
                    if (configured) {
                        combos[0]->setCurrentIndex(type);
                        combos[1]->setCurrentIndex(source);
                    }
                }
                dialog->done(configured ? QDialog::Accepted : QDialog::Rejected);
            });
            ok = QMetaObject::invokeMethod(&window, "configureTrackMatte", Qt::DirectConnection) && ok;
            const auto hasMatte = [&]() {
                const auto entries = t->trackMatteEntries();
                return entries.size() == 1 && entries.contains(QStringLiteral("0:1"))
                    && entries.value(QStringLiteral("0:1")).matteType == TrackMatteType::AlphaMatte
                    && entries.value(QStringLiteral("0:1")).matteSourceClipId == QStringLiteral("1:0");
            };
            // No baseline/restore/save after the dialog: capture must preserve
            // the unrecorded matte edit, including its reference to deleted V2.
            ok = ok && configured && hasMatte() && t->undoManager()->saveSerial() == serial;
            ok = t->removeTrack(false, 1) && ok && t->trackMatteEntries().isEmpty();
            t->undo();
            ok = ok && t->videoTrackCount() == 2 && hasMatte() && !t->canUndo();
            t->redo();
            ok = ok && t->videoTrackCount() == 1 && t->trackMatteEntries().isEmpty();
            t->undo();
            ok = ok && hasMatte();
        }
        gate(11, ok);
    }
    {
        Timeline t;
        auto mixer = std::make_unique<AudioMixer>();
        int applied = 0;
        t.setExternalTrackStateHooks([&]() { return mixer->collectTrackState(); },
            [&](const QJsonObject &state) { ++applied; mixer->applyTrackState(state); });
        t.setProjectOutputConfig(1920, 1080, false);
        baseline(t);
        trackfx::Chain chain;
        chain.eqEnabled = true;
        chain.eq.low.gainDb = 3.0;
        mixer->setTrackChain(0, chain);
        mixer->setTrackSolo(0, true);
        mixer->setTrackGain(0, 0.4);
        const auto live = mixer->collectTrackState();
        const auto serial = t.undoManager()->saveSerial();
        t.captureExternalTrackStateForCompoundEdit();
        bool ok = t.undoManager()->saveSerial() == serial && !t.canUndo();
        t.setProjectOutputConfig(1080, 1920, true);
        t.addVideoTrack(false);
        t.undoManager()->saveState(t.currentState(), QStringLiteral("Apply SNS preset"));
        ok = ok && t.videoTrackCount() == 2 && t.undoManager()->saveSerial() == serial + 1;
        t.undo();
        ok = ok && applied == 1 && mixer->collectTrackState() == live
            && t.videoTrackCount() == 1 && !t.canUndo()
            && t.currentState().projectWidth == 1920
            && t.currentState().projectHeight == 1080
            && !t.currentState().projectExplicitOutput;
        t.redo();
        gate(12, ok && applied == 2 && mixer->collectTrackState() == live
            && t.videoTrackCount() == 2 && t.currentState().projectWidth == 1080
            && t.currentState().projectHeight == 1920
            && t.currentState().projectExplicitOutput);
    }
    std::fprintf(stderr, "[track-ops] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
