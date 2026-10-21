#include "../Timeline.h"
#include "../UndoManager.h"
#include "../AudioMixer.h"
#include "../AudioBusRouting.h"
#include "../ProjectFile.h"
#include "../TrimOps.h"
#include "../MainWindow.h"
#include "../RotoClipEntry.h"
#include "../TrackMatteKey.h"
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
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTimer>
#include <QVBoxLayout>
#include <QDropEvent>
#include <QJsonDocument>
#include <QMimeData>
#include <QMouseEvent>
#include <QtTest/QTest>
#include "SelftestRegistry.h"
#include <cmath>
#include <cstdio>
#include <string>

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

// US-1203 storyline-mode helpers. Clips are named; positions are compared by
// name so a reorder, an evacuation or a missing clip shows in the layout text.
ClipInfo storyClip(const QString &name, double durationSec, int linkGroup = 0,
                   double leadInSec = 0.0)
{
    ClipInfo c;
    c.filePath = QStringLiteral("track-ops-story-%1.mov").arg(name);
    c.displayName = name;
    c.duration = durationSec;
    c.outPoint = durationSec;
    c.linkGroup = linkGroup;
    c.leadInSec = leadInSec;
    return c;
}

// US-1306: pins QSettings "VSimpleEditor/Preferences" importPlacement for one fixture and
// puts the previous value (or absence) back on scope exit.
struct ImportPlacementPin {
    QSettings prefs{QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences")};
    QVariant saved = prefs.value(QStringLiteral("importPlacement"));
    explicit ImportPlacementPin(ImportPlacement placement)
    {
        prefs.setValue(QStringLiteral("importPlacement"), static_cast<int>(placement));
        prefs.sync();
    }
    ~ImportPlacementPin()
    {
        if (saved.isValid()) prefs.setValue(QStringLiteral("importPlacement"), saved);
        else prefs.remove(QStringLiteral("importPlacement"));
        prefs.sync();
    }
};

QString importClipsJson(const TimelineTrack *track)
{
    ProjectData data;
    data.audioTracks = {track->clips()};
    return ProjectFile::toJsonString(data);
}

const ClipInfo *storyFind(const TimelineTrack *track, const QString &name, double *startSec = nullptr)
{
    double cursor = 0.0;
    for (const ClipInfo &c : track->clips()) {
        const double start = cursor + c.leadInSec;
        cursor = start + c.effectiveDuration();
        if (c.displayName != name) continue;
        if (startSec) *startSec = start;
        return &c;
    }
    return nullptr;
}

// Start of the named clip on the track, -1 when it is not there.
double storyStart(const TimelineTrack *track, const QString &name)
{
    double start = -1.0;
    return storyFind(track, name, &start) ? start : -1.0;
}

bool storyAt(const TimelineTrack *track, const QString &name, double expectedSec)
{
    return std::abs(storyStart(track, name) - expectedSec) < 1e-6;
}

// "name@start ..." of every clip on the track, in order.
std::string storyLayout(const TimelineTrack *track)
{
    QStringList parts;
    double cursor = 0.0;
    for (const ClipInfo &c : track->clips()) {
        const double start = cursor + c.leadInSec;
        cursor = start + c.effectiveDuration();
        parts.append(QStringLiteral("%1@%2").arg(c.displayName).arg(start, 0, 'f', 2));
    }
    return parts.join(QLatin1Char(' ')).toStdString();
}

void storyBaseline(Timeline &t)
{
    // saveUndoState (not saveState) so the connection baseline is taken too.
    t.undoManager()->clear();
    t.saveUndoState(QStringLiteral("基準"));
}

// Moves src's clip srcIdx onto dest at timeSec through the cross-track drop path.
bool storyClipDrop(TimelineTrack *src, int srcIdx, TimelineTrack *dest, double timeSec)
{
    QMimeData mime;
    mime.setData(QStringLiteral("application/x-vse-clip"),
                 QByteArray::number(srcIdx) + QByteArray(":0"));
    QDropEvent event(QPointF(timeSec * dest->pixelsPerSecond(), 4.0), Qt::MoveAction, &mime,
                     Qt::LeftButton, Qt::NoModifier);
    event.ignore();
    TimelineTrack::setSelftestDropSource(src);
    static_cast<QObject *>(dest)->event(&event);
    TimelineTrack::setSelftestDropSource(nullptr);
    return event.isAccepted();
}

// US-1305: press on the left / right trim handle of clip clipIdx, move by deltaSec and
// release (QTest mouse events, as in linked_trim_selftest). Returns the delta in
// timeline seconds the pixels actually carry.
double storyTrimEdge(TimelineTrack *track, int clipIdx, bool rightEdge, double deltaSec)
{
    const double pps = track->pixelsPerSecond();
    const ClipInfo &c = track->clips()[clipIdx];
    const int width = qMax(20, static_cast<int>(c.effectiveDuration() * pps));
    const int x0 = track->clipStartX(clipIdx) + (rightEdge ? width - 2 : 2);
    const int y = track->rowHeight() / 2;
    const int dx = static_cast<int>(std::lround(deltaSec * pps));
    QTest::mousePress(track, Qt::LeftButton, Qt::NoModifier, QPoint(x0, y));
    QTest::mouseMove(track, QPoint(x0 + dx, y));
    QTest::mouseRelease(track, Qt::LeftButton, Qt::NoModifier, QPoint(x0 + dx, y));
    return dx / pps;
}

double storyLength(const TimelineTrack *track, const QString &name)
{
    const ClipInfo *c = storyFind(track, name);
    return c ? c->effectiveDuration() : -1.0;
}

// Every clip on the track starts at or after the end of the one before it.
bool storyNoOverlap(const TimelineTrack *track)
{
    for (const ClipInfo &c : track->clips())
        if (c.leadInSec < -1e-9) return false;
    return true;
}

// Press on the clip body at fromSec, move to toSec and release (an in-track move drag).
void storyDrag(TimelineTrack *track, double fromSec, double toSec)
{
    const double y = 10.0;
    const QPointF press(fromSec * track->pixelsPerSecond(), y);
    const QPointF to(toSec * track->pixelsPerSecond(), y);
    QMouseEvent down(QEvent::MouseButtonPress, press, press, Qt::LeftButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(track, &down);
    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(track, &move);
    QMouseEvent up(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton,
                   Qt::NoModifier);
    QApplication::sendEvent(track, &up);
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
        auto *t = window.findChild<Timeline *>("mainTimeline");
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
        auto *t = window.findChild<Timeline *>("mainTimeline");
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
                auto *typeCombo = dialog->findChild<QComboBox *>(QStringLiteral("trackMatteTypeCombo"));
                auto *sourceCombo = dialog->findChild<QComboBox *>(QStringLiteral("trackMatteSourceCombo"));
                const auto typeOnly = dialog->findChildren<QComboBox *>();
                std::fprintf(stderr, "[track-ops] matte-dialog found=%s/%s typeOnlyFirst=%s\n",
                             typeCombo ? qPrintable(typeCombo->objectName()) : "(null)",
                             sourceCombo ? qPrintable(sourceCombo->objectName()) : "(null)",
                             typeOnly.isEmpty() ? "(none)" : qPrintable(typeOnly.first()->objectName()));
                if (typeCombo && sourceCombo && typeCombo != sourceCombo) {
                    const int type = typeCombo->findData(int(TrackMatteType::AlphaMatte));
                    const int source = sourceCombo->findData(QStringLiteral("1:0"));
                    configured = type >= 0 && source >= 0;
                    if (configured) {
                        typeCombo->setCurrentIndex(type);
                        sourceCombo->setCurrentIndex(source);
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
    // US-1203 G13: storyline mode, a clip dropped in the middle of V1 is an
    // insert on V1 only: the V1 clips after it are pushed, the unconnected V2
    // clip and the unlinked A1 clip stay. Control: mode off, the same drop
    // (onto a V1 clip) is refused and V1 is unchanged. An all-track insert would
    // move E / BGM; a missing mode branch fails the ON half.
    {
        bool ok = true;
        for (const bool on : {true, false}) {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("A", 4.0), storyClip("B", 4.0)},
                {storyClip("D", 3.0), storyClip("E", 3.0, 0, 7.0)}};
            const QVector<QVector<ClipInfo>> audio{{storyClip("BGM", 5.0, 0, 9.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(on);
            storyBaseline(t);
            const auto serial = t.undoManager()->saveSerial();
            const bool accepted = storyClipDrop(t.trackAt(false, 1), 0, t.trackAt(false, 0), 2.0);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const bool fixedOk = storyAt(t.trackAt(false, 1), "E", 10.0)
                && storyAt(t.trackAt(true, 0), "BGM", 9.0);
            const bool caseOk = on
                ? accepted && v1->clipCount() == 3 && storyAt(v1, "A", 0.0) && storyAt(v1, "D", 4.0)
                    && storyAt(v1, "B", 7.0) && fixedOk && t.undoManager()->saveSerial() == serial + 1
                : !accepted && v1->clipCount() == 2 && storyAt(v1, "A", 0.0) && storyAt(v1, "B", 4.0)
                    && storyAt(t.trackAt(false, 1), "D", 0.0) && fixedOk
                    && t.undoManager()->saveSerial() == serial;
            std::fprintf(stderr, "[track-ops] G13 %s: accepted=%d V1 [%s] V2 [%s] A1 [%s] (expect %s)\n",
                         on ? "on" : "off", accepted, storyLayout(v1).c_str(),
                         storyLayout(t.trackAt(false, 1)).c_str(), storyLayout(t.trackAt(true, 0)).c_str(),
                         on ? "1, A@0 D@4 B@7, E@10, BGM@9" : "0, A@0 B@4, D@0 E@10, BGM@9");
            ok = ok && caseOk;
        }
        gate(13, ok);
    }
    // US-1203 G14: storyline mode, Delete of a V1 clip closes V1 only: the
    // linked A of the deleted clip goes with it, the linked A of the next clip
    // and the V2 clip connected to it follow, the A1 BGM stays. Control: mode
    // off, the same Delete leaves the gap (C, Ca and K stay).
    {
        bool ok = true;
        for (const bool on : {true, false}) {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("A", 3.0, 1), storyClip("B", 3.0, 2), storyClip("C", 4.0, 3)},
                {storyClip("K", 2.0, 0, 7.0)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Aa", 3.0, 1), storyClip("Ba", 3.0, 2), storyClip("Ca", 4.0, 3),
                 storyClip("BGM", 3.0, 0, 2.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(on);
            storyBaseline(t);
            QString err;
            const QString anchorUid = t.trackAt(false, 0)->clips().at(2).uid;
            ok = t.connectClip(TrackKind::Video, 1, 0, anchorUid, &err) && ok;
            t.trackAt(false, 0)->setSelectedClip(1);
            const bool partnerSelected = t.trackAt(true, 0)->isClipSelected(1);
            const auto serial = t.undoManager()->saveSerial();
            t.deleteSelectedClip();
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const ClipInfo *k = storyFind(t.trackAt(false, 1), "K");
            const double shift = on ? 3.0 : 0.0;
            const bool caseOk = partnerSelected && v1->clipCount() == 2 && a1->clipCount() == 3
                && storyStart(v1, "B") < 0 && storyStart(a1, "Ba") < 0
                && storyAt(v1, "A", 0.0) && storyAt(v1, "C", 6.0 - shift)
                && storyAt(a1, "Aa", 0.0) && storyAt(a1, "Ca", 6.0 - shift) && storyAt(a1, "BGM", 12.0)
                && k && k->anchorUid == anchorUid && storyAt(t.trackAt(false, 1), "K", 7.0 - shift)
                && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G14 %s: partnerSelected=%d V1 [%s] A1 [%s] V2 [%s] "
                         "K connected=%d (expect %s)\n",
                         on ? "on" : "off", partnerSelected, storyLayout(v1).c_str(),
                         storyLayout(a1).c_str(), storyLayout(t.trackAt(false, 1)).c_str(),
                         k && k->anchorUid == anchorUid,
                         on ? "A@0 C@3, Aa@0 Ca@3 BGM@12, K@4 connected"
                            : "A@0 C@6, Aa@0 Ca@6 BGM@12, K@7 connected");
            ok = ok && caseOk;
        }
        gate(14, ok);
    }
    // US-1203 G15: storyline mode, dragging the V1 clip B (after a gap) to the
    // timeline start reorders V1 to B A C with no gap, one undo point. Control:
    // mode off, the same drag only moves B's leadIn to the clip before it and
    // the gap stays after B (C does not move). ON, the linked A1 audio follows
    // the reorder and B's linked Ba stays selected, so Delete removes B with Ba
    // (a selection dropped by the follow would leave Ba behind).
    {
        bool ok = true;
        for (const bool on : {true, false}) {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("A", 4.0, 1), storyClip("B", 4.0, 2, 4.0), storyClip("C", 4.0, 3)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Aa", 4.0, 1), storyClip("Ba", 4.0, 2, 4.0), storyClip("Ca", 4.0, 3)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(on);
            storyBaseline(t);
            const auto serial = t.undoManager()->saveSerial();
            TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            v1->setSelectedClip(1);
            const bool baSyncedBefore = a1->isClipSelected(1);
            storyDrag(v1, 10.0, 1.0);
            QStringList order;
            for (const ClipInfo &c : v1->clips()) order.append(c.displayName);
            const std::string v1Drag = storyLayout(v1), a1Drag = storyLayout(a1);
            const int baIndex = [&]() {
                for (int i = 0; i < a1->clipCount(); ++i)
                    if (a1->clips().at(i).displayName == QStringLiteral("Ba")) return i;
                return -1;
            }();
            const bool baSelected = baIndex >= 0 && a1->isClipSelected(baIndex);
            const bool caseOk = on
                ? order == QStringList{"B", "A", "C"} && storyAt(v1, "B", 0.0) && storyAt(v1, "A", 4.0)
                    && storyAt(v1, "C", 8.0) && t.undoManager()->saveSerial() == serial + 1
                    && storyAt(a1, "Ba", 0.0) && storyAt(a1, "Aa", 4.0) && storyAt(a1, "Ca", 8.0)
                    && baSelected
                : order == QStringList{"A", "B", "C"} && storyAt(v1, "A", 0.0) && storyAt(v1, "B", 4.0)
                    && storyAt(v1, "C", 12.0) && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G15 %s: V1 [%s] A1 [%s] BaSyncedBefore=%d BaSelected=%d "
                         "(expect %s)\n", on ? "on" : "off", v1Drag.c_str(), a1Drag.c_str(),
                         baSyncedBefore, baSelected,
                         on ? "B@0 A@4 C@8, Ba@0 Aa@4 Ca@8, 1, 1" : "A@0 B@4 C@12 (gap 8-12), 1");
            bool deleteOk = true;
            if (on) {
                t.deleteSelectedClip();
                deleteOk = storyStart(v1, "B") < 0 && storyStart(a1, "Ba") < 0 && v1->clipCount() == 2
                    && a1->clipCount() == 2 && storyAt(v1, "A", 0.0) && storyAt(v1, "C", 4.0)
                    && storyAt(a1, "Aa", 0.0) && storyAt(a1, "Ca", 4.0);
                std::fprintf(stderr, "[track-ops] G15 on Delete: V1 [%s] A1 [%s] "
                             "(expect A@0 C@4, Aa@0 Ca@4, no B / Ba)\n",
                             storyLayout(v1).c_str(), storyLayout(a1).c_str());
            }
            ok = ok && caseOk && baSyncedBefore && deleteOk;
        }
        gate(15, ok);
    }
    // US-1203 G16: the toggle (Edit menu action_storyline_mode, checkable,
    // default off) is saved as "storylineMode" only when on, restored by a
    // load (and reset by loading a file without it), and is no undo state: the
    // toggle adds no undo point and undoing an edit made after it keeps it on
    // (a TimelineState-carried mode would go back to off).
    {
        MainWindow window;
        auto *t = window.findChild<Timeline *>("mainTimeline");
        auto *action = window.findChild<QAction *>(QStringLiteral("action_storyline_mode"));
        bool ok = t && action && action->isCheckable() && !action->isChecked()
            && action->text() == QStringLiteral("主ストーリーライン (マグネティック)")
            && action->shortcut().isEmpty();
        bool offKey = true, onKey = false, restored = false, reset = false, undoKeeps = false;
        if (ok) {
            ProjectData off;
            window.populateProjectData(off);
            const QString offJson = ProjectFile::toJsonString(off);
            offKey = QJsonDocument::fromJson(offJson.toUtf8()).object().contains(QStringLiteral("storylineMode"));
            action->trigger();
            ProjectData onData;
            window.populateProjectData(onData);
            const QString onJson = ProjectFile::toJsonString(onData);
            onKey = t->storylineMode()
                && QJsonDocument::fromJson(onJson.toUtf8()).object().value(QStringLiteral("storylineMode")).toBool();
            // Load into the same window with the mode off: restored; then a file
            // without the key turns it off again.
            action->trigger();
            ProjectData loaded;
            if (!t->storylineMode() && ProjectFile::fromJsonString(onJson, loaded)) {
                window.applyLoadedProjectData(loaded, QString());
                restored = loaded.storylineMode && t->storylineMode() && action->isChecked();
                ProjectData loadedOff;
                if (ProjectFile::fromJsonString(offJson, loadedOff)) {
                    window.applyLoadedProjectData(loadedOff, QString());
                    reset = !t->storylineMode() && !action->isChecked();
                }
            }
            // Undo: an off baseline, the toggle, an edit, undo / redo.
            const QVector<QVector<ClipInfo>> video{{storyClip("A", 4.0)}};
            const QVector<QVector<ClipInfo>> audio{{}};
            t->restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            storyBaseline(*t);
            const auto serial = t->undoManager()->saveSerial();
            action->trigger();
            const bool toggleNoUndo = t->storylineMode() && t->undoManager()->saveSerial() == serial;
            QString err;
            const bool split = t->splitClipByIndex(false, 0, 0, 1.0, &err);
            t->undo();
            const bool afterUndo = t->storylineMode() && action->isChecked()
                && t->trackAt(false, 0)->clipCount() == 1;
            t->redo();
            undoKeeps = toggleNoUndo && split && afterUndo && t->storylineMode()
                && t->trackAt(false, 0)->clipCount() == 2;
        }
        std::fprintf(stderr, "[track-ops] G16: action=%d defaultKey=%d (expect 0) onKey=%d restored=%d "
                     "resetByOffFile=%d undoKeepsMode=%d\n",
                     ok, offKey, onKey, restored, reset, undoKeeps);
        gate(16, ok && !offKey && onKey && restored && reset && undoKeeps);
    }
    // US-1203 G17 (3 items), storyline mode:
    //   (a) a clip with linked audio dropped into V1: the audio lands on A1 at
    //       the clip's start, the A1 BGM stays, the next clip's linked audio
    //       (Qa) that would hit the BGM moves to A2 at its clip's new start;
    //       with one audio track, a dropped V2 clip's audio does not block the
    //       pushed Qa (nor Qa the dropped audio), and undo restores all three;
    //   (b) Q (ripple trim in, applyTrimActive RippleIn) by d = 1 on V1: V1
    //       closes, the linked audio keeps its trim and sync, the A1 BGM stays,
    //       the V2 clip connected to the next clip moves -d. Control: mode off,
    //       the BGM moves with A1 (the linked trim ripples A1 too); on a V1 with
    //       a gap the linked audio follows the closed V1 (no sync error);
    //   (b, V1 only) Q with only a V2 clip selected is refused: V1 and V2 stay
    //       (a fallback to the selected track would ripple V2);
    //   (c) a J cut (audio 1 s before its picture) copied and pasted after X:
    //       the pasted audio starts 1 s before the pasted clip and the pushed J
    //       pair keeps its -1 s (a re-derived offset of 0 would put either at
    //       its clip's start);
    //   (d) W (ripple trim out) on a gap-free V1: +1 moves the next V1 clip by
    //       exactly +1 (no gap; US-1305: trimops' RippleOut no longer also moves
    //       the next leadIn), -1 moves it back by 1, the A1 BGM
    //       stays, the next clip's linked audio on A2 follows; +3 is refused
    //       (it would push the BGM) and changes nothing; with Ya right after Xa
    //       on A1, W+1 is accepted: Ya ripples with Y, the BGM stays (undo too);
    //   (e) two linked audio clips sharing a uid on A1 / A2 both follow a V1
    //       delete: the A1 one, blocked by an A1 clip, may not evacuate onto A2
    //       while the A2 one is pending, so no track ends up holding the uid twice;
    //       likewise a drop's audio taken off A2 keeps an A1 follower sharing its uid off A2;
    //   (f) audio-origin linked drags: Ba dragged A1 -> A2 takes B off V1 and V1
    //       closes (C@4, Ca follows); Ba dragged A2 -> A1 inserts B into V1 and
    //       Ba lands at B's start; Ca dragged +2 s on A1 cannot move its V1
    //       picture C. Each has a mode-off control (gap kept / B appended with Ba
    //       out of sync / C and Ca moved).
    {
        bool aOk = false, bOk = false, bOffOk = false, bV1Ok = false, cOk = false, dOk = false,
             eOk = false, fOk = false, fOffOk = false, fA1Ok = false, fA1OffOk = false,
             fDragOk = false, fDragOffOk = false;
        {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("P", 4.0, 1), storyClip("Q", 4.0, 2)},
                {storyClip("N", 3.0, 3, 20.0)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Pa", 4.0, 1), storyClip("Qa", 4.0, 2), storyClip("BGM", 6.0)},
                {storyClip("Na", 3.0, 3, 20.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            const auto serial = t.undoManager()->saveSerial();
            const bool accepted = storyClipDrop(t.trackAt(false, 1), 0, t.trackAt(false, 0), 2.0);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const TimelineTrack *a2 = t.trackAt(true, 1);
            aOk = accepted && storyAt(v1, "P", 0.0) && storyAt(v1, "N", 4.0) && storyAt(v1, "Q", 7.0)
                && storyAt(a1, "Pa", 0.0) && storyAt(a1, "Na", 4.0) && storyAt(a1, "BGM", 8.0)
                && storyAt(a2, "Qa", 7.0) && t.trackAt(false, 1)->clipCount() == 0
                && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G17a: accepted=%d V1 [%s] A1 [%s] A2 [%s] "
                         "(expect P@0 N@4 Q@7, Pa@0 Na@4 BGM@8, Qa@7)\n", accepted,
                         storyLayout(v1).c_str(), storyLayout(a1).c_str(), storyLayout(a2).c_str());
        }
        {
            // One audio track: the dropped clip's audio (Na@10) must not block Qa
            // (7-11), and Qa must not block Na (4-7); both fit on A1.
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("P", 4.0, 1), storyClip("Q", 4.0, 2)},
                {storyClip("N", 3.0, 3, 10.0)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Pa", 4.0, 1), storyClip("Qa", 4.0, 2), storyClip("Na", 3.0, 3, 2.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            const auto serial = t.undoManager()->saveSerial();
            const bool accepted = storyClipDrop(t.trackAt(false, 1), 0, t.trackAt(false, 0), 2.0);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *v2 = t.trackAt(false, 1);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const auto serialAfter = t.undoManager()->saveSerial();
            const bool dropOk = accepted && storyAt(v1, "P", 0.0) && storyAt(v1, "N", 4.0)
                && storyAt(v1, "Q", 7.0) && storyAt(a1, "Pa", 0.0) && storyAt(a1, "Na", 4.0)
                && storyAt(a1, "Qa", 7.0) && a1->clipCount() == 3 && v2->clipCount() == 0
                && serialAfter == serial + 1;
            const std::string v1Drop = storyLayout(v1), v2Drop = storyLayout(v2), a1Drop = storyLayout(a1);
            const int audioTracks = t.audioTrackCount();
            t.undo();
            v1 = t.trackAt(false, 0);
            v2 = t.trackAt(false, 1);
            a1 = t.trackAt(true, 0);
            const bool undoOk = storyAt(v1, "P", 0.0) && storyAt(v1, "Q", 4.0) && v1->clipCount() == 2
                && storyAt(v2, "N", 10.0) && storyAt(a1, "Pa", 0.0) && storyAt(a1, "Qa", 4.0)
                && storyAt(a1, "Na", 10.0) && a1->clipCount() == 3;
            std::fprintf(stderr, "[track-ops] G17a one A track: accepted=%d audioTracks=%d serial +%d "
                         "V1 [%s] V2 [%s] A1 [%s] (expect P@0 N@4 Q@7, empty, Pa@0 Na@4 Qa@7, +1); "
                         "undo V1 [%s] V2 [%s] A1 [%s] (expect P@0 Q@4, N@10, Pa@0 Qa@4 Na@10)\n",
                         accepted, audioTracks, int(serialAfter - serial), v1Drop.c_str(),
                         v2Drop.c_str(), a1Drop.c_str(), storyLayout(v1).c_str(),
                         storyLayout(v2).c_str(), storyLayout(a1).c_str());
            aOk = aOk && dropOk && undoOk;
        }
        for (const bool on : {true, false}) {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("X", 4.0, 1), storyClip("Y", 4.0, 2)},
                {storyClip("K", 2.0, 0, 5.0)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Xa", 4.0, 1), storyClip("Ya", 4.0, 2), storyClip("BGM", 3.0, 0, 1.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(on);
            storyBaseline(t);
            QString err;
            const QString anchorUid = t.trackAt(false, 0)->clips().at(1).uid;
            bool ok = t.connectClip(TrackKind::Video, 1, 0, anchorUid, &err);
            t.trackAt(false, 0)->setSelectedClip(0);
            ok = t.applyTrimActive(trimops::TrimType::RippleIn, 1.0, &err) && ok;
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const ClipInfo *x = storyFind(v1, "X");
            const ClipInfo *xa = storyFind(a1, "Xa");
            ok = ok && x && xa && std::abs(x->inPoint - 1.0) < 1e-6 && std::abs(xa->inPoint - 1.0) < 1e-6
                && storyAt(v1, "X", 0.0) && storyAt(v1, "Y", 3.0) && storyAt(a1, "Xa", 0.0)
                && storyAt(a1, "Ya", 3.0) && storyAt(t.trackAt(false, 1), "K", 4.0)
                && storyAt(a1, "BGM", on ? 9.0 : 8.0);
            std::fprintf(stderr, "[track-ops] G17b %s: err='%s' V1 [%s] A1 [%s] V2 [%s] "
                         "(expect X@0 Y@3, Xa@0 Ya@3 BGM@%s, K@4)\n", on ? "on" : "off",
                         err.toUtf8().constData(), storyLayout(v1).c_str(), storyLayout(a1).c_str(),
                         storyLayout(t.trackAt(false, 1)).c_str(), on ? "9" : "8");
            (on ? bOk : bOffOk) = ok;
        }
        {
            // A V1 that already has a gap (turning the mode on closes none): Q
            // closes it with the trim, and Ya follows Y to its final start (a
            // target of its old start + the trim alone would leave Ya at 7).
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("X", 4.0, 1), storyClip("Y", 4.0, 2, 4.0)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Xa", 4.0, 1), storyClip("Ya", 4.0, 2, 4.0), storyClip("BGM", 3.0, 0, 1.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            t.trackAt(false, 0)->setSelectedClip(0);
            QString err;
            const auto serial = t.undoManager()->saveSerial();
            const bool trimmed = t.applyTrimActive(trimops::TrimType::RippleIn, 1.0, &err);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const bool gapOk = trimmed && storyAt(v1, "X", 0.0) && storyAt(v1, "Y", 3.0)
                && storyAt(a1, "Xa", 0.0) && storyAt(a1, "Ya", 3.0) && storyAt(a1, "BGM", 13.0)
                && a1->clipCount() == 3 && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G17b gapped V1: ok=%d err='%s' V1 [%s] A1 [%s] "
                         "(expect X@0 Y@3, Xa@0 Ya@3 BGM@13)\n", trimmed, err.toUtf8().constData(),
                         storyLayout(v1).c_str(), storyLayout(a1).c_str());
            bOk = bOk && gapOk;
        }
        {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("X", 4.0, 1), storyClip("Y", 4.0, 2)},
                {storyClip("K", 2.0, 0, 5.0)}};
            const QVector<QVector<ClipInfo>> audio{{storyClip("Xa", 4.0, 1), storyClip("Ya", 4.0, 2)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            t.trackAt(false, 1)->setSelectedClip(0);
            t.trackAt(false, 0)->setSelectedClip(-1);
            const std::string v1Before = storyLayout(t.trackAt(false, 0));
            const ClipInfo *k = storyFind(t.trackAt(false, 1), "K");
            const double kInBefore = k ? k->inPoint : -1.0;
            const auto serial = t.undoManager()->saveSerial();
            QString err;
            const bool trimmed = t.applyTrimActive(trimops::TrimType::RippleIn, 1.0, &err);
            k = storyFind(t.trackAt(false, 1), "K");
            const ClipInfo *x = storyFind(t.trackAt(false, 0), "X");
            bV1Ok = !trimmed && !err.isEmpty() && k && std::abs(k->inPoint - kInBefore) < 1e-6
                && storyAt(t.trackAt(false, 1), "K", 5.0) && x && std::abs(x->inPoint) < 1e-6
                && storyLayout(t.trackAt(false, 0)) == v1Before
                && t.undoManager()->saveSerial() == serial;
            std::fprintf(stderr, "[track-ops] G17b V1 only: trimmed=%d err='%s' V1 [%s] (before [%s]) "
                         "V2 [%s] K.in=%.2f (expect 0, error, V1 unchanged, K@5 in 0)\n", trimmed,
                         err.toUtf8().constData(), storyLayout(t.trackAt(false, 0)).c_str(),
                         v1Before.c_str(), storyLayout(t.trackAt(false, 1)).c_str(), k ? k->inPoint : -1.0);
        }
        {
            Timeline t;
            ClipInfo x = storyClip("X", 8.0, 1);
            x.outPoint = 4.0;
            ClipInfo xa = storyClip("Xa", 8.0, 1);
            xa.outPoint = 4.0;
            const QVector<QVector<ClipInfo>> video{{x, storyClip("Y", 4.0, 2)}};
            const QVector<QVector<ClipInfo>> audio{
                {xa, storyClip("BGM", 3.0, 0, 2.0)}, {storyClip("Ya", 4.0, 2, 4.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const TimelineTrack *a2 = t.trackAt(true, 1);
            const auto yLeadIn = [&]() {
                const ClipInfo *y = storyFind(v1, "Y");
                return y ? y->leadInSec : -1.0;
            };
            QString errExtend, errShorten, errPush;
            t.trackAt(false, 0)->setSelectedClip(0);
            auto serial = t.undoManager()->saveSerial();
            const bool extended = t.applyTrimActive(trimops::TrimType::RippleOut, 1.0, &errExtend);
            const bool extendOk = extended && storyAt(v1, "X", 0.0) && storyAt(v1, "Y", 5.0)
                && std::abs(yLeadIn()) < 1e-6 && storyAt(a1, "Xa", 0.0) && storyAt(a1, "BGM", 6.0)
                && storyAt(a2, "Ya", 5.0) && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G17d W+1: ok=%d err='%s' V1 [%s] Y.leadIn=%.2f A1 [%s] A2 [%s] "
                         "(expect X@0 Y@5 leadIn 0, Xa@0 BGM@6, Ya@5)\n", extended,
                         errExtend.toUtf8().constData(), storyLayout(v1).c_str(), yLeadIn(),
                         storyLayout(a1).c_str(), storyLayout(a2).c_str());
            t.trackAt(false, 0)->setSelectedClip(0);
            serial = t.undoManager()->saveSerial();
            const bool shortened = t.applyTrimActive(trimops::TrimType::RippleOut, -1.0, &errShorten);
            const bool shortenOk = shortened && storyAt(v1, "X", 0.0) && storyAt(v1, "Y", 4.0)
                && std::abs(yLeadIn()) < 1e-6 && storyAt(a1, "Xa", 0.0) && storyAt(a1, "BGM", 6.0)
                && storyAt(a2, "Ya", 4.0) && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G17d W-1: ok=%d err='%s' V1 [%s] Y.leadIn=%.2f A1 [%s] A2 [%s] "
                         "(expect X@0 Y@4 leadIn 0, Xa@0 BGM@6, Ya@4)\n", shortened,
                         errShorten.toUtf8().constData(), storyLayout(v1).c_str(), yLeadIn(),
                         storyLayout(a1).c_str(), storyLayout(a2).c_str());
            t.trackAt(false, 0)->setSelectedClip(0);
            serial = t.undoManager()->saveSerial();
            const std::string v1Before = storyLayout(v1), a1Before = storyLayout(a1);
            const bool pushed = t.applyTrimActive(trimops::TrimType::RippleOut, 3.0, &errPush);
            const ClipInfo *xNow = storyFind(v1, "X");
            const ClipInfo *xaNow = storyFind(a1, "Xa");
            const bool pushOk = !pushed && !errPush.isEmpty() && storyLayout(v1) == v1Before
                && storyLayout(a1) == a1Before && xNow && std::abs(xNow->outPoint - 4.0) < 1e-6
                && xaNow && std::abs(xaNow->outPoint - 4.0) < 1e-6 && storyAt(a1, "BGM", 6.0)
                && t.undoManager()->saveSerial() == serial;
            std::fprintf(stderr, "[track-ops] G17d W+3: ok=%d err='%s' V1 [%s] A1 [%s] "
                         "(expect refused, V1 / A1 unchanged)\n", pushed, errPush.toUtf8().constData(),
                         storyLayout(v1).c_str(), storyLayout(a1).c_str());
            dOk = extendOk && shortenOk && pushOk;
        }
        {
            // The contiguous linked layout of an imported V+A pair: Ya ripples with
            // Y (it is not pushed), so W+1 is accepted and only the BGM is held.
            Timeline t;
            ClipInfo x = storyClip("X", 8.0, 1);
            x.outPoint = 4.0;
            ClipInfo xa = storyClip("Xa", 8.0, 1);
            xa.outPoint = 4.0;
            const QVector<QVector<ClipInfo>> video{{x, storyClip("Y", 4.0, 2)}};
            const QVector<QVector<ClipInfo>> audio{
                {xa, storyClip("Ya", 4.0, 2), storyClip("BGM", 3.0, 0, 2.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            t.trackAt(false, 0)->setSelectedClip(0);
            const auto serial = t.undoManager()->saveSerial();
            QString err;
            const bool extended = t.applyTrimActive(trimops::TrimType::RippleOut, 1.0, &err);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const ClipInfo *y = storyFind(v1, "Y");
            const double yLeadIn = y ? y->leadInSec : -1.0;
            const auto serialAfter = t.undoManager()->saveSerial();
            const bool extendOk = extended && storyAt(v1, "X", 0.0) && storyAt(v1, "Y", 5.0)
                && std::abs(yLeadIn) < 1e-6 && storyAt(a1, "Xa", 0.0) && storyAt(a1, "Ya", 5.0)
                && storyAt(a1, "BGM", 10.0) && a1->clipCount() == 3 && serialAfter == serial + 1;
            const std::string v1Trim = storyLayout(v1), a1Trim = storyLayout(a1);
            t.undo();
            v1 = t.trackAt(false, 0);
            a1 = t.trackAt(true, 0);
            const bool undoOk = storyAt(v1, "X", 0.0) && storyAt(v1, "Y", 4.0) && storyAt(a1, "Xa", 0.0)
                && storyAt(a1, "Ya", 4.0) && storyAt(a1, "BGM", 10.0) && a1->clipCount() == 3;
            std::fprintf(stderr, "[track-ops] G17d W+1 linked A1: ok=%d err='%s' serial +%d V1 [%s] "
                         "Y.leadIn=%.2f A1 [%s] (expect X@0 Y@5 leadIn 0, Xa@0 Ya@5 BGM@10, +1); "
                         "undo V1 [%s] A1 [%s] (expect X@0 Y@4, Xa@0 Ya@4 BGM@10)\n", extended,
                         err.toUtf8().constData(), int(serialAfter - serial), v1Trim.c_str(), yLeadIn,
                         a1Trim.c_str(), storyLayout(v1).c_str(), storyLayout(a1).c_str());
            dOk = dOk && extendOk && undoOk;
        }
        {
            Timeline t;
            ClipInfo pa = storyClip("Pa", 4.0, 1, 1.0);
            pa.uid = QStringLiteral("track-ops-shared-uid");
            ClipInfo qa = storyClip("Qa", 4.0, 2, 6.0);
            qa.uid = pa.uid;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("Z", 2.0), storyClip("P", 4.0, 1), storyClip("Q", 4.0, 2)}};
            const QVector<QVector<ClipInfo>> audio{{storyClip("BGM", 1.0), pa}, {qa}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            t.trackAt(false, 0)->setSelectedClip(0);
            t.deleteSelectedClip();
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const TimelineTrack *a2 = t.trackAt(true, 1);
            const auto uidCount = [&](const TimelineTrack *track) {
                int n = 0;
                for (const ClipInfo &c : track->clips())
                    n += c.uid == pa.uid ? 1 : 0;
                return n;
            };
            eOk = storyAt(v1, "P", 0.0) && storyAt(v1, "Q", 4.0) && storyAt(a1, "BGM", 0.0)
                && storyAt(a1, "Pa", 2.0) && storyAt(a2, "Qa", 4.0) && uidCount(a1) == 1
                && uidCount(a2) == 1;
            std::fprintf(stderr, "[track-ops] G17e: V1 [%s] A1 [%s] A2 [%s] uid on A1=%d A2=%d "
                         "(expect P@0 Q@4, BGM@0 Pa@2, Qa@4, 1 / 1)\n", storyLayout(v1).c_str(),
                         storyLayout(a1).c_str(), storyLayout(a2).c_str(), uidCount(a1), uidCount(a2));
        }
        {
            // The same in a storyline drop: N's audio Na (A2) shares its uid with
            // Qa (A1). Qa, pushed into the BGM, may not evacuate onto A2 while Na
            // is out, so neither Na's fallback nor Qa ends up doubling the uid.
            Timeline t;
            ClipInfo qa = storyClip("Qa", 4.0, 2);
            qa.uid = QStringLiteral("track-ops-drop-shared-uid");
            ClipInfo na = storyClip("Na", 5.0, 3, 20.0);
            na.uid = qa.uid;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("P", 4.0, 1), storyClip("Q", 4.0, 2)},
                {storyClip("N", 5.0, 3, 20.0)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Pa", 4.0, 1), qa, storyClip("BGM", 6.0)}, {na}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            const bool accepted = storyClipDrop(t.trackAt(false, 1), 0, t.trackAt(false, 0), 2.0);
            int maxUidPerTrack = 0, audioClips = 0;
            QStringList layouts;
            for (int a = 0; a < t.audioTrackCount(); ++a) {
                const TimelineTrack *track = t.trackAt(true, a);
                QHash<QString, int> uids;
                for (const ClipInfo &c : track->clips())
                    maxUidPerTrack = qMax(maxUidPerTrack, ++uids[c.uid]);
                audioClips += track->clipCount();
                layouts.append(QString::fromStdString(storyLayout(track)));
            }
            const bool dropUidOk = accepted && storyAt(t.trackAt(false, 0), "N", 4.0)
                && maxUidPerTrack == 1 && audioClips == 4;
            std::fprintf(stderr, "[track-ops] G17e drop: accepted=%d V1 [%s] A [%s] maxUidPerTrack=%d "
                         "audioClips=%d (expect 1, N@4, 1, 4)\n", accepted,
                         storyLayout(t.trackAt(false, 0)).c_str(),
                         layouts.join(QStringLiteral(" | ")).toUtf8().constData(), maxUidPerTrack, audioClips);
            eOk = eOk && dropUidOk;
        }
        {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{{storyClip("X", 4.0, 1), storyClip("J", 4.0, 2)}};
            const QVector<QVector<ClipInfo>> audio{{storyClip("Xa", 3.0, 1), storyClip("Ja", 4.0, 2)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            t.trackAt(false, 0)->setSelectedClip(1);
            t.copySelectedClip();
            t.trackAt(false, 0)->setSelectedClip(0);
            const auto serial = t.undoManager()->saveSerial();
            t.pasteClip();
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            // The pasted pair carries a new link group; the pushed J pair keeps group 2.
            double pastedV = -1.0, pastedA = -1.0, movedJ = -1.0, movedJa = -1.0;
            int pastedVGroup = -1, pastedAGroup = -2;
            for (const TimelineTrack *track : {v1, a1}) {
                double cursor = 0.0;
                for (const ClipInfo &c : track->clips()) {
                    const double start = cursor + c.leadInSec;
                    cursor = start + c.effectiveDuration();
                    if (!c.displayName.startsWith(QLatin1Char('J'))) continue;
                    const bool audioTrack = track == a1;
                    if (c.linkGroup == 2) {
                        (audioTrack ? movedJa : movedJ) = start;
                    } else {
                        (audioTrack ? pastedA : pastedV) = start;
                        (audioTrack ? pastedAGroup : pastedVGroup) = c.linkGroup;
                    }
                }
            }
            cOk = v1->clipCount() == 3 && a1->clipCount() == 3 && pastedVGroup > 0
                && pastedVGroup == pastedAGroup && pastedVGroup != 2
                && std::abs(pastedV - 4.0) < 1e-6 && std::abs(pastedA - 3.0) < 1e-6
                && std::abs(movedJ - 8.0) < 1e-6 && std::abs(movedJa - 7.0) < 1e-6
                && storyAt(a1, "Xa", 0.0) && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G17c: V1 [%s] A1 [%s] pasted V@%.2f A@%.2f (expect 4 / 3), "
                         "pushed J@%.2f Ja@%.2f (expect 8 / 7)\n", storyLayout(v1).c_str(),
                         storyLayout(a1).c_str(), pastedV, pastedA, movedJ, movedJa);
        }
        for (const bool on : {true, false}) {
            // (f) Ba dragged from A1 to A2 at 4: its picture B leaves V1 for V2.
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("A", 4.0, 1), storyClip("B", 4.0, 2), storyClip("C", 4.0, 3)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Aa", 4.0, 1), storyClip("Ba", 4.0, 2), storyClip("Ca", 4.0, 3)}, {}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(on);
            storyBaseline(t);
            const auto serial = t.undoManager()->saveSerial();
            const bool accepted = storyClipDrop(t.trackAt(true, 0), 1, t.trackAt(true, 1), 4.0);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *v2 = t.videoTrackCount() > 1 ? t.trackAt(false, 1) : nullptr;
            const TimelineTrack *a1 = t.trackAt(true, 0);
            double maxLeadIn = 0.0;
            for (const ClipInfo &c : v1->clips()) maxLeadIn = qMax(maxLeadIn, c.leadInSec);
            const double cSec = on ? 4.0 : 8.0;
            const bool caseOk = accepted && v1->clipCount() == 2 && storyAt(v1, "A", 0.0)
                && storyAt(v1, "C", cSec) && (on ? maxLeadIn < 1e-6 : maxLeadIn > 1.0)
                && v2 && storyAt(v2, "B", 4.0) && storyAt(t.trackAt(true, 1), "Ba", 4.0)
                && storyAt(a1, "Aa", 0.0) && storyAt(a1, "Ca", cSec)
                && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G17f %s: accepted=%d V1 [%s] maxLeadIn=%.2f V2 [%s] A1 [%s] "
                         "A2 [%s] (expect %s)\n", on ? "on" : "off", accepted, storyLayout(v1).c_str(),
                         maxLeadIn, v2 ? storyLayout(v2).c_str() : "-", storyLayout(a1).c_str(),
                         storyLayout(t.trackAt(true, 1)).c_str(),
                         on ? "A@0 C@4, 0, B@4, Aa@0 Ca@4, Ba@4" : "A@0 C@8 (gap 4-8), 4, B@4, Aa@0 Ca@8, Ba@4");
            (on ? fOk : fOffOk) = caseOk;
        }
        for (const bool on : {true, false}) {
            // (f, onto A1) Ba dragged from A2 to A1 at 1 (inside A, which has no
            // audio): B goes into V1 at the insert point after A (4), C and its Ca
            // move +2, and Ba lands at B's start (OFF: B appended at 8, Ba at 1).
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("A", 4.0), storyClip("C", 4.0, 3)}, {storyClip("B", 2.0, 2, 20.0)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Ca", 4.0, 3, 4.0)}, {storyClip("Ba", 2.0, 2, 20.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(on);
            storyBaseline(t);
            const auto serial = t.undoManager()->saveSerial();
            const bool accepted = storyClipDrop(t.trackAt(true, 1), 0, t.trackAt(true, 0), 1.0);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            double maxLeadIn = 0.0;
            for (const ClipInfo &c : v1->clips()) maxLeadIn = qMax(maxLeadIn, c.leadInSec);
            const ClipInfo *ba = storyFind(a1, "Ba");
            const bool layoutOk = on
                ? storyAt(v1, "A", 0.0) && storyAt(v1, "B", 4.0) && storyAt(v1, "C", 6.0)
                    && maxLeadIn < 1e-6 && storyAt(a1, "Ba", 4.0) && storyAt(a1, "Ca", 6.0)
                : storyAt(v1, "A", 0.0) && storyAt(v1, "C", 4.0) && storyAt(v1, "B", 8.0)
                    && storyAt(a1, "Ba", 1.0) && storyAt(a1, "Ca", 4.0);
            const bool caseOk = accepted && layoutOk && v1->clipCount() == 3 && a1->clipCount() == 2
                && t.trackAt(false, 1)->clipCount() == 0 && t.trackAt(true, 1)->clipCount() == 0
                && ba && ba->anchorUid.isEmpty() && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G17f onto A1 %s: accepted=%d V1 [%s] maxLeadIn=%.2f A1 [%s] "
                         "Ba connected=%d (expect %s)\n", on ? "on" : "off", accepted,
                         storyLayout(v1).c_str(), maxLeadIn, storyLayout(a1).c_str(),
                         ba && !ba->anchorUid.isEmpty(),
                         on ? "A@0 B@4 C@6, 0, Ba@4 Ca@6, 0" : "A@0 C@4 B@8, Ba@1 Ca@4, 0");
            (on ? fA1Ok : fA1OffOk) = caseOk;
        }
        for (const bool on : {true, false}) {
            // (f, same track) Ca dragged +2 s on A1: ON its V1 picture C (the last
            // V1 clip) stays at leadIn 0 and nothing moves; OFF C and Ca move to 10.
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("A", 4.0, 1), storyClip("B", 4.0, 2), storyClip("C", 4.0, 3)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Aa", 4.0, 1), storyClip("Ba", 4.0, 2), storyClip("Ca", 4.0, 3)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(on);
            storyBaseline(t);
            const auto serial = t.undoManager()->saveSerial();
            storyDrag(t.trackAt(true, 0), 9.0, 11.0);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const ClipInfo *c = storyFind(v1, "C");
            const double cSec = on ? 8.0 : 10.0;
            const bool caseOk = c && std::abs(c->leadInSec - (cSec - 8.0)) < 1e-6
                && storyAt(v1, "C", cSec) && storyAt(a1, "Ca", cSec)
                && t.undoManager()->saveSerial() == serial + (on ? 0 : 1);
            std::fprintf(stderr, "[track-ops] G17f drag Ca %s: V1 [%s] C.leadIn=%.2f A1 [%s] serial +%d "
                         "(expect %s)\n", on ? "on" : "off", storyLayout(v1).c_str(),
                         c ? c->leadInSec : -1.0, storyLayout(a1).c_str(),
                         int(t.undoManager()->saveSerial() - serial),
                         on ? "C@8 leadIn 0, Ca@8, +0" : "C@10 leadIn 2, Ca@10, +1");
            (on ? fDragOk : fDragOffOk) = caseOk;
        }
        std::fprintf(stderr, "[track-ops] G17 items: a=%d b=%d bOffControl=%d bV1Only=%d c=%d d=%d e=%d "
                     "f=%d fOffControl=%d fOntoA1=%d fOntoA1OffControl=%d fDrag=%d fDragOffControl=%d\n",
                     aOk, bOk, bOffOk, bV1Ok, cOk, dOk, eOk, fOk, fOffOk, fA1Ok, fA1OffOk, fDragOk,
                     fDragOffOk);
        gate(17, aOk && bOk && bOffOk && bV1Ok && cOk && dOk && eOk && fOk && fOffOk && fA1Ok
                 && fA1OffOk && fDragOk && fDragOffOk);
    }
    // US-1220 G18 / G19: storyline mode ripples V1 only when the delete removes
    // a V1 clip. V1 holds a gap (B after 2 s, as a Lift or a pre-toggle edit
    // leaves it). G18 deletes only the V2 clip K, G19 only the unlinked A2 clip M:
    // every V1 start and every other audio start stays (B@5, Ba@5, N@5). The base
    // (07c705f) closed V1 on any delete, so B and Ba moved to 3. Control (the
    // same setup, the V1 clip A deleted): the V1 ripple still runs (B@0, Ba@0).
    for (const int gateNo : {18, 19}) {
        bool ok = true;
        for (const bool deleteV1 : {false, true}) {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{
                {storyClip("A", 3.0, 1), storyClip("B", 3.0, 2, 2.0)},
                {storyClip("K", 2.0, 0, 1.0)}};
            const QVector<QVector<ClipInfo>> audio{
                {storyClip("Aa", 3.0, 1), storyClip("Ba", 3.0, 2, 2.0)},
                {storyClip("M", 2.0), storyClip("N", 2.0, 0, 3.0)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            for (int v = 0; v < t.videoTrackCount(); ++v) t.trackAt(false, v)->setSelectedClip(-1);
            for (int a = 0; a < t.audioTrackCount(); ++a) t.trackAt(true, a)->setSelectedClip(-1);
            if (deleteV1)
                t.trackAt(false, 0)->setSelectedClip(0);
            else if (gateNo == 18)
                t.trackAt(false, 1)->setSelectedClip(0);
            else
                t.trackAt(true, 1)->setSelectedClip(0);
            const auto serial = t.undoManager()->saveSerial();
            t.deleteSelectedClip();
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *v2 = t.trackAt(false, 1);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const TimelineTrack *a2 = t.trackAt(true, 1);
            const bool caseOk = deleteV1
                ? v1->clipCount() == 1 && storyAt(v1, "B", 0.0) && storyAt(a1, "Ba", 0.0)
                    && storyStart(a1, "Aa") < 0 && storyAt(v2, "K", 1.0)
                    && storyAt(a2, "M", 0.0) && storyAt(a2, "N", 5.0)
                : storyAt(v1, "A", 0.0) && storyAt(v1, "B", 5.0) && v1->clipCount() == 2
                    && storyAt(a1, "Aa", 0.0) && storyAt(a1, "Ba", 5.0) && a1->clipCount() == 2
                    && (gateNo == 18
                            ? v2->clipCount() == 0 && storyAt(a2, "M", 0.0) && storyAt(a2, "N", 5.0)
                            : storyAt(v2, "K", 1.0) && a2->clipCount() == 1
                                && storyStart(a2, "M") < 0 && storyAt(a2, "N", 5.0));
            const bool serialOk = t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G%d %s: V1 [%s] V2 [%s] A1 [%s] A2 [%s] serial +%d (expect %s)\n",
                         gateNo, deleteV1 ? "control delete A" : gateNo == 18 ? "delete K (V2)" : "delete M (A2)",
                         storyLayout(v1).c_str(), storyLayout(v2).c_str(), storyLayout(a1).c_str(),
                         storyLayout(a2).c_str(), int(t.undoManager()->saveSerial() - serial),
                         deleteV1 ? "B@0, K@1, Ba@0, M@0 N@5, +1"
                                  : gateNo == 18 ? "A@0 B@5, -, Aa@0 Ba@5, M@0 N@5, +1"
                                                 : "A@0 B@5, K@1, Aa@0 Ba@5, N@5, +1");
            ok = ok && caseOk && serialOk;
        }
        gate(gateNo, ok);
    }
    // US-1220 G20: storyline paste never inserts into a locked audio track.
    // (a) A1 (holding the linked audio) locked, A2 unlocked but busy at the
    // landing time (Z covers 0-20): the pasted J audio goes to the end of A2
    // (20) and A1's clip list is byte-equal (ProjectFile JSON) to before. (b)
    // the only audio track locked: the paste changes nothing (V1, A1 and the
    // undo serial stay). The base (07c705f) fell back to A1 (m_audioTrack)
    // without a lock check in both cases.
    {
        const auto clipsJson = [](const TimelineTrack *track) {
            ProjectData data;
            data.audioTracks = {track->clips()};
            return ProjectFile::toJsonString(data);
        };
        bool aOk = false, bOk = false;
        for (const bool onlyLocked : {false, true}) {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{{storyClip("X", 4.0, 1), storyClip("J", 4.0, 2)}};
            QVector<QVector<ClipInfo>> audio{{storyClip("Xa", 4.0, 1), storyClip("Ja", 4.0, 2)}};
            if (!onlyLocked) audio.append({storyClip("Z", 20.0)});
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(true);
            storyBaseline(t);
            t.trackAt(false, 0)->setSelectedClip(1);
            t.copySelectedClip();
            t.trackAt(false, 0)->setSelectedClip(0);
            t.trackAt(true, 0)->setLocked(true);
            const QString a1Before = clipsJson(t.trackAt(true, 0));
            const QString v1Before = clipsJson(t.trackAt(false, 0));
            const auto serial = t.undoManager()->saveSerial();
            t.pasteClip();
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const TimelineTrack *a2 = onlyLocked ? nullptr : t.trackAt(true, 1);
            const bool a1Same = clipsJson(a1) == a1Before;
            const int serialDelta = int(t.undoManager()->saveSerial() - serial);
            if (onlyLocked) {
                bOk = a1Same && clipsJson(v1) == v1Before && t.audioTrackCount() == 1 && serialDelta == 0;
            } else {
                double pastedA = -1.0;
                double cursor = 0.0;
                for (const ClipInfo &c : a2->clips()) {
                    const double start = cursor + c.leadInSec;
                    cursor = start + c.effectiveDuration();
                    if (c.displayName == QLatin1String("Ja")) pastedA = start;
                }
                aOk = a1Same && v1->clipCount() == 3 && storyAt(v1, "X", 0.0) && a2->clipCount() == 2
                    && storyAt(a2, "Z", 0.0) && std::abs(pastedA - 20.0) < 1e-6 && serialDelta == 1;
            }
            std::fprintf(stderr, "[track-ops] G20%s: V1 [%s] A1 (locked) [%s] A2 [%s] A1 byte-equal=%d "
                         "serial +%d (expect %s)\n", onlyLocked ? "b only A1 (locked)" : "a A1 locked, A2 busy",
                         storyLayout(v1).c_str(), storyLayout(a1).c_str(), a2 ? storyLayout(a2).c_str() : "-",
                         a1Same, serialDelta,
                         onlyLocked ? "X@0 J@4 unchanged, Xa@0 Ja@4, -, 1, +0"
                                    : "X@0 J@4 J@8, Xa@0 Ja@4, Z@0 Ja@20, 1, +1");
        }
        gate(20, aOk && bOk);
    }
    // C-04-base G21: the non-storyline paste never writes into a locked V1/A1.
    // Cases: V1 locked (storyline off), A1 locked (storyline off), V1 locked
    // with storyline on (falls into the same path). The locked track's clips
    // (ProjectFile JSON) and the other track stay byte-equal; no undo state.
    {
        const auto clipsJson = [](const TimelineTrack *track) {
            ProjectData data;
            data.audioTracks = {track->clips()};
            return ProjectFile::toJsonString(data);
        };
        bool ok = true;
        struct Case { const char *name; bool lockV1; bool storyline; };
        for (const Case c : {Case{"V1 locked, storyline off", true, false},
                             Case{"A1 locked, storyline off", false, false},
                             Case{"V1 locked, storyline on", true, true}}) {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{{storyClip("X", 4.0, 1), storyClip("J", 4.0, 2)}};
            const QVector<QVector<ClipInfo>> audio{{storyClip("Xa", 4.0, 1), storyClip("Ja", 4.0, 2)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setStorylineMode(c.storyline);
            storyBaseline(t);
            t.trackAt(false, 0)->setSelectedClip(1);
            t.copySelectedClip();
            t.trackAt(false, 0)->setSelectedClip(0);
            t.trackAt(true, 0)->setSelectedClip(0);
            t.trackAt(false, 0)->setLocked(c.lockV1);
            t.trackAt(true, 0)->setLocked(!c.lockV1);
            const QString v1Before = clipsJson(t.trackAt(false, 0));
            const QString a1Before = clipsJson(t.trackAt(true, 0));
            const auto serial = t.undoManager()->saveSerial();
            t.pasteClip();
            const bool v1Same = clipsJson(t.trackAt(false, 0)) == v1Before;
            const bool a1Same = clipsJson(t.trackAt(true, 0)) == a1Before;
            const int serialDelta = int(t.undoManager()->saveSerial() - serial);
            const bool caseOk = v1Same && a1Same && serialDelta == 0;
            std::fprintf(stderr, "[track-ops] G21 %s: V1 [%s] A1 [%s] V1 byte-equal=%d A1 byte-equal=%d "
                         "serial +%d (expect unchanged, +0)\n", c.name, storyLayout(t.trackAt(false, 0)).c_str(),
                         storyLayout(t.trackAt(true, 0)).c_str(), v1Same, a1Same, serialDelta);
            ok = ok && caseOk;
        }
        gate(21, ok);
    }
    // US-1306 G22: importMedia honours track locks. Explicit index onto a locked V1 and
    // AppendToFirstTrack with a locked V1 are refused (same text as dropMediaOnTrack, nothing
    // changes, no undo point); the automatic ParallelTrack placement skips a locked empty V1/A1.
    const QString importPath = QDir::current().absoluteFilePath(QStringLiteral("test_assets/e2e_clip.mp4"));
    const QString lockedText = QStringLiteral("ロック中のトラックには配置できません");
    if (!QFileInfo::exists(importPath))
        std::fprintf(stderr, "[track-ops] G22-G24 fixture missing: %s\n", qPrintable(importPath));
    {
        const auto refuses = [&](ImportPlacement placement, int trackIndex, QString *line) {
            ImportPlacementPin pin(placement);
            Timeline t;
            const QVector<QVector<ClipInfo>> video{{storyClip("X", 4.0, 1)}};
            const QVector<QVector<ClipInfo>> audio{{storyClip("Xa", 4.0, 1)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            storyBaseline(t);
            t.trackAt(false, 0)->setLocked(true);
            const QString v1Before = importClipsJson(t.trackAt(false, 0));
            const QString a1Before = importClipsJson(t.trackAt(true, 0));
            const auto serial = t.undoManager()->saveSerial();
            QString err;
            const bool imported = t.importMedia(importPath, trackIndex, 10.0, nullptr, &err);
            const bool v1Same = importClipsJson(t.trackAt(false, 0)) == v1Before;
            const bool a1Same = importClipsJson(t.trackAt(true, 0)) == a1Before;
            const int serialDelta = int(t.undoManager()->saveSerial() - serial);
            *line = QStringLiteral("imported=%1 err=[%2] V1 same=%3 A1 same=%4 V=%5 A=%6 serial +%7")
                        .arg(imported).arg(err).arg(v1Same).arg(a1Same)
                        .arg(t.videoTrackCount()).arg(t.audioTrackCount()).arg(serialDelta);
            return !imported && err == lockedText && v1Same && a1Same
                && t.videoTrackCount() == 1 && t.audioTrackCount() == 1 && serialDelta == 0;
        };
        QString explicitLine, appendLine;
        const bool explicitOk = refuses(ImportPlacement::ParallelTrack, 0, &explicitLine);
        const bool appendOk = refuses(ImportPlacement::AppendToFirstTrack, -1, &appendLine);
        bool autoOk = false;
        QString autoLine;
        {
            ImportPlacementPin pin(ImportPlacement::ParallelTrack);
            Timeline t;
            const QVector<QVector<ClipInfo>> empty2{{}, {}};
            t.restoreFromProject(empty2, empty2, 0.0, -1.0, -1.0, 100);
            storyBaseline(t);
            t.trackAt(false, 0)->setLocked(true);
            t.trackAt(true, 0)->setLocked(true);
            QString err;
            Timeline::MediaImportResult res;
            const bool imported = t.importMedia(importPath, -1, -1.0, &res, &err);
            autoOk = imported && t.trackAt(false, 0)->clipCount() == 0 && t.trackAt(true, 0)->clipCount() == 0
                && t.trackAt(false, 1)->clipCount() == 1 && t.trackAt(true, 1)->clipCount() == 1
                && res.videoTrackIndex == 1 && res.audioTrackIndex == 1 && t.videoTrackCount() == 2;
            autoLine = QStringLiteral("imported=%1 V1=%2 V2=%3 A1=%4 A2=%5 resultV=%6 resultA=%7 (expect V1/A1 0, V2/A2 1)")
                           .arg(imported).arg(t.trackAt(false, 0)->clipCount()).arg(t.trackAt(false, 1)->clipCount())
                           .arg(t.trackAt(true, 0)->clipCount()).arg(t.trackAt(true, 1)->clipCount())
                           .arg(res.videoTrackIndex).arg(res.audioTrackIndex);
        }
        std::fprintf(stderr, "[track-ops] G22 explicit locked V1: %s\n[track-ops] G22 append locked V1: %s\n"
                     "[track-ops] G22 auto skips locked V1/A1: %s\n",
                     qPrintable(explicitLine), qPrintable(appendLine), qPrintable(autoLine));
        gate(22, explicitOk && appendOk && autoOk);
    }
    // US-1306 G23: a refused import (V2 overlap) leaves no partner track behind and no undo point.
    {
        ImportPlacementPin pin(ImportPlacement::ParallelTrack);
        Timeline t;
        // V2 holds a clip while only A1 exists, so a V2 import would need a new A2.
        const QVector<QVector<ClipInfo>> video{{storyClip("X", 4.0, 1)}, {storyClip("Y", 4.0)}};
        t.restoreFromProject(video, QVector<QVector<ClipInfo>>{{}}, 0.0, -1.0, -1.0, 100);
        storyBaseline(t);
        const int vBefore = t.videoTrackCount();
        const int aBefore = t.audioTrackCount();
        const auto serial = t.undoManager()->saveSerial();
        QString err;
        const bool imported = t.importMedia(importPath, 1, 1.0, nullptr, &err);
        const int serialDelta = int(t.undoManager()->saveSerial() - serial);
        std::fprintf(stderr, "[track-ops] G23 overlap import: imported=%d err=[%s] V %d->%d A %d->%d serial +%d "
                     "(expect 0, err set, A stays %d)\n", imported, qPrintable(err), vBefore, t.videoTrackCount(),
                     aBefore, t.audioTrackCount(), serialDelta, aBefore);
        gate(23, aBefore == 1 && !imported && err == QStringLiteral("指定位置にクリップを配置できません") && t.videoTrackCount() == vBefore
                     && t.audioTrackCount() == aBefore && serialDelta == 0);
    }
    // US-1306 G24: link groups come from allocateLinkGroup(), which skips numbers in use.
    // (1) importMedia beside an existing group 1 pair; (2) a separate fixture with groups {1, 2}
    // and a split that does not import first (m_nextLinkGroup stays 1 after setClips).
    {
        bool importOk = false;
        QString importLine;
        {
            ImportPlacementPin pin(ImportPlacement::ParallelTrack);
            Timeline t;
            const QVector<QVector<ClipInfo>> video{{storyClip("X", 4.0, 1)}};
            const QVector<QVector<ClipInfo>> audio{{storyClip("Xa", 4.0, 1)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            QString err;
            const bool imported = t.importMedia(importPath, 0, 10.0, nullptr, &err);
            const int vGroup = t.trackAt(false, 0)->clips().last().linkGroup;
            const int aGroup = t.trackAt(true, 0)->clips().last().linkGroup;
            importOk = imported && vGroup > 0 && vGroup == aGroup && vGroup != 1;
            importLine = QStringLiteral("imported=%1 new V group=%2 A group=%3 (expect equal, not 1)")
                             .arg(imported).arg(vGroup).arg(aGroup);
        }
        bool splitOk = false;
        QString splitLine;
        {
            Timeline t;
            const QVector<QVector<ClipInfo>> video{{storyClip("X", 4.0, 1), storyClip("Y", 4.0, 2)}};
            const QVector<QVector<ClipInfo>> audio{{storyClip("Xa", 4.0, 1), storyClip("Ya", 4.0, 2)}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            QString err;
            const bool split = t.splitClipByIndex(false, 0, 0, 2.0, &err);
            const QVector<ClipInfo> v = t.trackAt(false, 0)->clips();
            const QVector<ClipInfo> a = t.trackAt(true, 0)->clips();
            const int rightV = v.size() == 3 ? v[1].linkGroup : -1;
            const int rightA = a.size() == 3 ? a[1].linkGroup : -1;
            splitOk = split && v.size() == 3 && a.size() == 3 && rightV > 0 && rightV == rightA
                && rightV != 1 && rightV != 2 && v[0].linkGroup == 1 && v[2].linkGroup == 2;
            splitLine = QStringLiteral("split=%1 V clips=%2 right V group=%3 right A group=%4 (expect equal, not 1/2) err=[%5]")
                            .arg(split).arg(v.size()).arg(rightV).arg(rightA).arg(err);
        }
        std::fprintf(stderr, "[track-ops] G24 (1) import: %s\n[track-ops] G24 (2) split: %s\n",
                     qPrintable(importLine), qPrintable(splitLine));
        gate(24, importOk && splitOk);
    }
    // US-1305 G25: storyline mode, mouse trims of the linked V1 clip Y in a gap-free
    // V1 [X|Y|Z] (Y: 8 s source, in 2 / out 6 at 4 s; Ya on A1 under it, then the
    // unlinked BGM at 10 s; K on V2 connected to Z at 8 s). Each edge keeps Y's and
    // Ya's start and ripples V1: (1) left +d, (2) right +d, (3) left -d; (4) undo
    // once restores each; (5) OFF control: left +d moves Y by d and Z stays. The C3
    // variant puts the linked audio Xa / Ya / Za under all three V1 clips and an
    // unlinked BGM after Za (gap 2 s, at 14 s): Za follows Z and the BGM stays. The
    // unlinked variant (Y not linked, no audio) takes the direct trim path: ON left +d
    // keeps Y's start and Z follows, OFF left +d moves Y by d and Z stays.
    // HEAD: (1) moves Y.start by +d, (2) / (3) cannot move (next / own leadIn 0).
    {
        const double d = 1.0;
        auto makeStory = [](Timeline &t, bool on, bool allLinked) {
            ClipInfo y = storyClip("Y", 8.0, 2);
            y.inPoint = 2.0;
            y.outPoint = 6.0;
            ClipInfo ya = y;
            ya.displayName = QStringLiteral("Ya");
            ya.filePath = QStringLiteral("track-ops-story-Ya.mov");
            const QVector<QVector<ClipInfo>> video{
                {storyClip("X", 4.0, allLinked ? 1 : 0), y, storyClip("Z", 4.0, allLinked ? 3 : 0)},
                allLinked ? QVector<ClipInfo>{} : QVector<ClipInfo>{storyClip("K", 2.0, 0, 8.0)}};
            QVector<ClipInfo> a1;
            if (allLinked) {
                a1 = {storyClip("Xa", 4.0, 1), ya, storyClip("Za", 4.0, 3), storyClip("BGM", 3.0, 0, 2.0)};
            } else {
                ya.leadInSec = 4.0;
                a1 = {ya, storyClip("BGM", 3.0, 0, 2.0)};
            }
            t.restoreFromProject(video, QVector<QVector<ClipInfo>>{a1}, 0.0, -1.0, -1.0, 100);
            t.setSnapEnabled(false);
            for (auto *track : t.videoTracks()) track->setSnapEnabled(false);
            for (auto *track : t.audioTracks()) track->setSnapEnabled(false);
            t.setStorylineMode(on);
            QString err;
            bool connected = true;
            if (!allLinked)
                connected = t.connectClip(TrackKind::Video, 1, 0, t.trackAt(false, 0)->clips().at(2).uid, &err);
            storyBaseline(t);
            return connected;
        };
        auto layout = [](Timeline &t) {
            return "V1 [" + storyLayout(t.trackAt(false, 0)) + "] V2 [" + storyLayout(t.trackAt(false, 1))
                + "] A1 [" + storyLayout(t.trackAt(true, 0)) + "]";
        };
        bool mainOk = true;
        struct Step { const char *name; bool right; double delta; };
        const Step steps[] = {{"(1) left +d", false, d}, {"(2) right +d", true, d}, {"(3) left -d", false, -d}};
        for (const Step &step : steps) {
            Timeline t;
            const bool connected = makeStory(t, true, false);
            const std::string before = layout(t);
            const auto serial = t.undoManager()->saveSerial();
            const double moved = storyTrimEdge(t.trackAt(false, 0), 1, step.right, step.delta);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *v2 = t.trackAt(false, 1);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const double yLen = storyLength(v1, "Y");
            const double yaLen = storyLength(a1, "Ya");
            // Left +d shortens by d, right +d and left -d lengthen by d.
            const double lenDelta = step.right ? moved : -moved;
            const double zSec = 8.0 + lenDelta;
            const bool editOk = connected && std::abs(moved - step.delta) < 1e-6
                && storyAt(v1, "X", 0.0) && storyAt(v1, "Y", 4.0) && std::abs(yLen - (4.0 + lenDelta)) < 1e-6
                && storyAt(v1, "Z", zSec) && storyAt(a1, "Ya", 4.0) && std::abs(yaLen - yLen) < 1e-6
                && storyAt(a1, "BGM", 10.0) && storyAt(v2, "K", zSec)
                && t.undoManager()->saveSerial() == serial + 1;
            const std::string edited = layout(t);
            const int serialDelta = int(t.undoManager()->saveSerial() - serial);
            t.undo();
            const bool undoOk = layout(t) == before;
            std::fprintf(stderr, "[track-ops] G25 %s: connected=%d moved=%.3f %s Y.len=%.2f Ya.len=%.2f serial +%d "
                         "(expect Y@4 len %.2f, Z@%.2f, Ya@4 same len, BGM@10, K@%.2f, +1); undo %s (expect %s) -> %d/%d\n",
                         step.name, connected, moved, edited.c_str(), yLen, yaLen,
                         serialDelta, 4.0 + lenDelta, zSec, zSec,
                         layout(t).c_str(), before.c_str(), editOk, undoOk);
            mainOk = mainOk && editOk && undoOk;
        }
        bool offOk = false;
        {
            Timeline t;
            makeStory(t, false, false);
            const double moved = storyTrimEdge(t.trackAt(false, 0), 1, false, d);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            offOk = storyAt(v1, "Y", 4.0 + moved) && storyAt(v1, "Z", 8.0);
            std::fprintf(stderr, "[track-ops] G25 (5) OFF left +d: %s (expect Y@5, Z@8) -> %d\n",
                         layout(t).c_str(), offOk);
        }
        bool c3Ok = true;
        for (const Step &step : steps) {
            Timeline t;
            makeStory(t, true, true);
            const std::string before = layout(t);
            const auto serial = t.undoManager()->saveSerial();
            const double moved = storyTrimEdge(t.trackAt(false, 0), 1, step.right, step.delta);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            const double lenDelta = step.right ? moved : -moved;
            const double zSec = 8.0 + lenDelta;
            const bool editOk = std::abs(moved - step.delta) < 1e-6
                && storyAt(v1, "Y", 4.0) && storyAt(a1, "Ya", storyStart(v1, "Y"))
                && std::abs(storyLength(a1, "Ya") - (4.0 + lenDelta)) < 1e-6
                && storyAt(v1, "Z", zSec) && storyAt(a1, "Za", storyStart(v1, "Z"))
                && storyAt(a1, "Xa", 0.0) && a1->clipCount() == 4 && storyNoOverlap(a1)
                && storyAt(a1, "Za", zSec) && storyAt(a1, "BGM", 14.0)
                && t.undoManager()->saveSerial() == serial + 1;
            const std::string edited = layout(t);
            const double yaLen = storyLength(a1, "Ya");
            const double bgmSec = storyStart(a1, "BGM");
            const int serialDelta = int(t.undoManager()->saveSerial() - serial);
            t.undo();
            const bool undoOk = layout(t) == before;
            std::fprintf(stderr, "[track-ops] G25 C3 %s: %s Ya.len=%.2f BGM@%.2f serial +%d (expect Y@4 Ya@4 len %.2f, "
                         "Z@%.2f Za@%.2f, BGM@14, no A1 overlap, +1); undo %s -> %d/%d\n", step.name, edited.c_str(),
                         yaLen, bgmSec, serialDelta,
                         4.0 + lenDelta, zSec, zSec, layout(t).c_str(), editOk, undoOk);
            c3Ok = c3Ok && editOk && undoOk;
        }
        bool directOnOk = false, directOffOk = false;
        for (const bool on : {true, false}) {
            Timeline t;
            ClipInfo y = storyClip("Y", 8.0);
            y.inPoint = 2.0;
            y.outPoint = 6.0;
            t.restoreFromProject(QVector<QVector<ClipInfo>>{{storyClip("X", 4.0), y, storyClip("Z", 4.0)}},
                                 QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
            t.setSnapEnabled(false);
            for (auto *track : t.videoTracks()) track->setSnapEnabled(false);
            t.setStorylineMode(on);
            storyBaseline(t);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const std::string before = storyLayout(v1);
            const auto serial = t.undoManager()->saveSerial();
            const double moved = storyTrimEdge(t.trackAt(false, 0), 1, false, d);
            const std::string edited = storyLayout(v1);
            const double yLen = storyLength(v1, "Y");
            const int serialDelta = int(t.undoManager()->saveSerial() - serial);
            if (on) {
                const bool editOk = std::abs(moved - d) < 1e-6 && storyAt(v1, "Y", 4.0)
                    && std::abs(yLen - (4.0 - moved)) < 1e-6 && storyAt(v1, "Z", 8.0 - moved) && serialDelta == 1;
                t.undo();
                const bool undoOk = storyLayout(v1) == before;
                directOnOk = editOk && undoOk;
                std::fprintf(stderr, "[track-ops] G25 unlinked ON left +d: V1 [%s] Y.len=%.2f serial +%d (expect Y@4 "
                             "len %.2f, Z@%.2f, +1); undo V1 [%s] -> %d/%d\n", edited.c_str(), yLen, serialDelta,
                             4.0 - moved, 8.0 - moved, storyLayout(v1).c_str(), editOk, undoOk);
            } else {
                directOffOk = std::abs(moved - d) < 1e-6 && storyAt(v1, "Y", 4.0 + moved) && storyAt(v1, "Z", 8.0);
                std::fprintf(stderr, "[track-ops] G25 unlinked OFF left +d: V1 [%s] (expect Y@%.2f, Z@8) -> %d\n",
                             edited.c_str(), 4.0 + moved, directOffOk);
            }
        }
        std::fprintf(stderr, "[track-ops] G25 items: main=%d off=%d c3=%d directOn=%d directOff=%d\n",
                     mainOk, offOk, c3Ok, directOnOk, directOffOk);
        gate(25, mainOk && offOk && c3Ok && directOnOk && directOffOk);
    }
    // US-1305 G26: storyline mode nudge of the V1 clip Y (25 fps, 1f = 0.04 s) in a
    // gap-free V1 [X|Y|Z]: +1f moves Y and Z by 1f, X stays and a 1f gap opens before Y
    // that the reconcile (saveUndoState) leaves; one undo restores. -1f with Y's leadIn 0
    // is refused (state unchanged, one status). Y with its linked Ya (taken in by the
    // linked selection): +1f is accepted and the reconcile puts Ya at Y's start, the A1
    // BGM stays. Y with an unlinked V2 clip: refused, state unchanged, one status.
    // Y with its linked Ya on a locked A1: refused, state unchanged, one status.
    // HEAD: moveClipByIndex moves only Y, Z stays.
    {
        const double f = 1.0 / 25.0;
        auto makeNudge = [](Timeline &t, bool linked) {
            const QVector<QVector<ClipInfo>> video{
                {storyClip("X", 4.0), storyClip("Y", 4.0, linked ? 2 : 0), storyClip("Z", 4.0)},
                {storyClip("K", 2.0, 0, 12.0)}};
            const QVector<QVector<ClipInfo>> audio{
                linked ? QVector<ClipInfo>{storyClip("Ya", 4.0, 2, 4.0), storyClip("BGM", 3.0, 0, 2.0)}
                       : QVector<ClipInfo>{}};
            t.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
            t.setNudgeFrameRate(25.0);
            t.setStorylineMode(true);
            storyBaseline(t);
        };
        auto layout = [](Timeline &t) {
            return "V1 [" + storyLayout(t.trackAt(false, 0)) + "] V2 [" + storyLayout(t.trackAt(false, 1))
                + "] A1 [" + storyLayout(t.trackAt(true, 0)) + "]";
        };
        bool plusOk = false, minusOk = false, linkedOk = false, mixedOk = false, lockedOk = false;
        {
            Timeline t;
            makeNudge(t, false);
            const std::string before = layout(t);
            t.trackAt(false, 0)->setSelectedClip(1);
            auto serial = t.undoManager()->saveSerial();
            t.nudgeSelectedClips(1);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const ClipInfo *y = storyFind(v1, "Y");
            const double gap = y ? y->leadInSec : -1.0;
            plusOk = storyAt(v1, "X", 0.0) && storyAt(v1, "Y", 4.0 + f) && storyAt(v1, "Z", 8.0 + f)
                && std::abs(gap - f) < 1e-9 && t.undoManager()->saveSerial() == serial + 1;
            const std::string nudged = layout(t);
            const int nudgeSerialDelta = int(t.undoManager()->saveSerial() - serial);
            t.undo();
            const bool undoOk = layout(t) == before;
            std::fprintf(stderr, "[track-ops] G26 +1f: %s Y gap=%.4f serial +%d (expect X@0 Y@4.04 Z@8.04, gap 0.04 "
                         "after the reconcile, +1); undo %s -> %d/%d\n", nudged.c_str(), gap,
                         nudgeSerialDelta, layout(t).c_str(), plusOk, undoOk);
            plusOk = plusOk && undoOk;
            t.trackAt(false, 0)->setSelectedClip(1);
            QSignalSpy status(&t, &Timeline::statusMessageRequested);
            serial = t.undoManager()->saveSerial();
            t.nudgeSelectedClips(-1);
            minusOk = layout(t) == before && status.count() == 1 && t.undoManager()->saveSerial() == serial;
            std::fprintf(stderr, "[track-ops] G26 -1f at leadIn 0: %s status=%d serial +%d (expect unchanged, 1, +0) -> %d\n",
                         layout(t).c_str(), int(status.count()), int(t.undoManager()->saveSerial() - serial), minusOk);
        }
        {
            Timeline t;
            makeNudge(t, true);
            const std::string before = layout(t);
            t.trackAt(false, 0)->setSelectedClip(1);
            const bool yaSelected = t.trackAt(true, 0)->isClipSelected(0);
            QSignalSpy status(&t, &Timeline::statusMessageRequested);
            const auto serial = t.undoManager()->saveSerial();
            t.nudgeSelectedClips(1);
            const TimelineTrack *v1 = t.trackAt(false, 0);
            const TimelineTrack *a1 = t.trackAt(true, 0);
            linkedOk = yaSelected && status.count() == 0 && storyAt(v1, "Y", 4.0 + f) && storyAt(v1, "Z", 8.0 + f)
                && storyAt(a1, "Ya", storyStart(v1, "Y")) && storyAt(a1, "BGM", 10.0)
                && t.undoManager()->saveSerial() == serial + 1;
            std::fprintf(stderr, "[track-ops] G26 Y+Ya +1f: YaSelected=%d status=%d %s serial +%d (expect Y@4.04 "
                         "Ya@4.04 BGM@10, +1; before %s) -> %d\n", yaSelected, int(status.count()), layout(t).c_str(),
                         int(t.undoManager()->saveSerial() - serial), before.c_str(), linkedOk);
        }
        {
            Timeline t;
            makeNudge(t, false);
            const std::string before = layout(t);
            t.trackAt(false, 0)->setSelectedClip(1);
            t.trackAt(false, 1)->toggleClipSelection(0);
            const bool bothSelected = t.trackAt(false, 0)->isClipSelected(1) && t.trackAt(false, 1)->isClipSelected(0);
            QSignalSpy status(&t, &Timeline::statusMessageRequested);
            const auto serial = t.undoManager()->saveSerial();
            t.nudgeSelectedClips(1);
            mixedOk = bothSelected && layout(t) == before && status.count() == 1
                && t.undoManager()->saveSerial() == serial;
            std::fprintf(stderr, "[track-ops] G26 Y+V2 K +1f: bothSelected=%d %s status=%d serial +%d "
                         "(expect unchanged, 1, +0) -> %d\n", bothSelected, layout(t).c_str(), int(status.count()),
                         int(t.undoManager()->saveSerial() - serial), mixedOk);
        }
        {
            Timeline t;
            makeNudge(t, true);
            t.trackAt(true, 0)->setLocked(true);
            const std::string before = layout(t);
            t.trackAt(false, 0)->setSelectedClip(1);
            QSignalSpy status(&t, &Timeline::statusMessageRequested);
            const auto serial = t.undoManager()->saveSerial();
            t.nudgeSelectedClips(1);
            lockedOk = layout(t) == before && status.count() == 1 && t.undoManager()->saveSerial() == serial;
            std::fprintf(stderr, "[track-ops] G26 Y+Ya locked A1 +1f: %s status=%d serial +%d "
                         "(expect unchanged %s, 1, +0) -> %d\n", layout(t).c_str(), int(status.count()),
                         int(t.undoManager()->saveSerial() - serial), before.c_str(), lockedOk);
        }
        std::fprintf(stderr, "[track-ops] G26 items: plus=%d minus=%d linked=%d mixed=%d locked=%d\n",
                     plusOk, minusOk, linkedOk, mixedOk, lockedOk);
        gate(26, plusOk && minusOk && linkedOk && mixedOk && lockedOk);
    }
    // US-1305 G27: storyline mode OFF, W (applyTrimActive RippleOut +1) on X in
    // V1 [X|Y] and applyTrimLinked RippleOut +1 on X with its linked Xa (A1 [Xa|N])
    // move the downstream clip by exactly +1. HEAD: +2 (TrimOps also shifted the
    // next leadIn).
    {
        ClipInfo x = storyClip("X", 8.0, 1);
        x.outPoint = 4.0;
        ClipInfo xa = storyClip("Xa", 8.0, 1);
        xa.outPoint = 4.0;
        bool activeOk = false, linkedOk = false;
        {
            Timeline t;
            ClipInfo solo = x;
            solo.linkGroup = 0;
            t.restoreFromProject(QVector<QVector<ClipInfo>>{{solo, storyClip("Y", 4.0)}},
                                 QVector<QVector<ClipInfo>>{{}}, 0.0, -1.0, -1.0, 100);
            storyBaseline(t);
            t.trackAt(false, 0)->setSelectedClip(0);
            QString err;
            const bool ok = t.applyTrimActive(trimops::TrimType::RippleOut, 1.0, &err);
            activeOk = ok && storyAt(t.trackAt(false, 0), "Y", 5.0);
            std::fprintf(stderr, "[track-ops] G27 applyTrimActive: ok=%d err='%s' V1 [%s] (expect Y@5) -> %d\n", ok,
                         err.toUtf8().constData(), storyLayout(t.trackAt(false, 0)).c_str(), activeOk);
        }
        {
            Timeline t;
            t.restoreFromProject(QVector<QVector<ClipInfo>>{{x, storyClip("Y", 4.0)}},
                                 QVector<QVector<ClipInfo>>{{xa, storyClip("N", 4.0)}}, 0.0, -1.0, -1.0, 100);
            storyBaseline(t);
            QString err;
            const bool ok = t.applyTrimLinked(t.trackAt(false, 0), 0, trimops::TrimType::RippleOut, 1.0, &err);
            linkedOk = ok && storyAt(t.trackAt(false, 0), "Y", 5.0) && storyAt(t.trackAt(true, 0), "N", 5.0);
            std::fprintf(stderr, "[track-ops] G27 applyTrimLinked: ok=%d err='%s' V1 [%s] A1 [%s] (expect Y@5, N@5) -> %d\n",
                         ok, err.toUtf8().constData(), storyLayout(t.trackAt(false, 0)).c_str(),
                         storyLayout(t.trackAt(true, 0)).c_str(), linkedOk);
        }
        gate(27, activeOk && linkedOk);
    }
    // US-1313 G28: a cross-track drop moves every member of a link group. (a) V2+A2+A3
    // (A2 / A3 start at 5) dropped V2 -> V3: A2 -> A3, A3 -> A4 (created), both still
    // at 5, one link group, one undo point. HEAD: only A2 moves and lands after A3's
    // clip (appended at the end). (b) V1+A1 dropped V1 -> V2: A1 -> A2 (as HEAD).
    // (c) V2+V3+A2 with roto / matte / parent on V2 and V3, A2 dropped on A3 so V2 -> V3
    // and V3 -> V4: each carrier follows its own clip and one undo puts the keys back.
    // HEAD: V3's member stays; a per-member remapCarriersAfterCrossTrackMove after
    // all members left their tracks maps both entries onto one key.
    {
        auto linkGroupOf = [](const TimelineTrack *track, const QString &name) {
            const ClipInfo *c = track ? storyFind(track, name) : nullptr;
            return c ? c->linkGroup : -1;
        };
        bool aOk = false, bOk = false, cOk = false;
        {
            Timeline t;
            t.restoreFromProject(
                QVector<QVector<ClipInfo>>{{}, {storyClip("V", 4.0, 1, 5.0)}, {}},
                QVector<QVector<ClipInfo>>{{}, {storyClip("A2", 4.0, 1, 5.0)}, {storyClip("A3", 4.0, 1, 5.0)}},
                0.0, -1.0, -1.0, 100);
            storyBaseline(t);
            const auto serial = t.undoManager()->saveSerial();
            const bool accepted = storyClipDrop(t.trackAt(false, 1), 0, t.trackAt(false, 2), 5.0);
            const int audioCount = t.audioTrackCount();
            const std::string a3 = storyLayout(t.trackAt(true, 2));
            const std::string a4 = audioCount > 3 ? storyLayout(t.trackAt(true, 3)) : std::string("-");
            const int group = linkGroupOf(t.trackAt(false, 2), "V");
            const bool moved = accepted && audioCount == 4 && t.trackAt(false, 1)->clipCount() == 0
                && storyAt(t.trackAt(false, 2), "V", 5.0) && t.trackAt(true, 1)->clipCount() == 0
                && t.trackAt(true, 2)->clipCount() == 1 && storyAt(t.trackAt(true, 2), "A2", 5.0)
                && t.trackAt(true, 3)->clipCount() == 1 && storyAt(t.trackAt(true, 3), "A3", 5.0);
            const bool grouped = group > 0 && linkGroupOf(t.trackAt(true, 2), "A2") == group
                && audioCount > 3 && linkGroupOf(t.trackAt(true, 3), "A3") == group;
            const bool oneUndo = t.undoManager()->saveSerial() == serial + 1;
            t.undo();
            const bool undone = t.audioTrackCount() == 3 && storyAt(t.trackAt(false, 1), "V", 5.0)
                && storyAt(t.trackAt(true, 1), "A2", 5.0) && t.trackAt(true, 2)->clipCount() == 1
                && storyAt(t.trackAt(true, 2), "A3", 5.0);
            aOk = moved && grouped && oneUndo && undone;
            std::fprintf(stderr, "[track-ops] G28a: accepted=%d audioTracks=%d A3 [%s] A4 [%s] (expect A2@5.00 / "
                         "A3@5.00) group=%d grouped=%d oneUndo=%d undone=%d -> %d\n", accepted, audioCount,
                         a3.c_str(), a4.c_str(), group, grouped, oneUndo, undone, aOk);
        }
        {
            Timeline t;
            t.restoreFromProject(QVector<QVector<ClipInfo>>{{storyClip("P", 3.0, 2)}, {}},
                                 QVector<QVector<ClipInfo>>{{storyClip("Pa", 3.0, 2)}, {}}, 0.0, -1.0, -1.0, 100);
            storyBaseline(t);
            const bool accepted = storyClipDrop(t.trackAt(false, 0), 0, t.trackAt(false, 1), 3.0);
            bOk = accepted && t.audioTrackCount() == 2 && t.trackAt(true, 0)->clipCount() == 0
                && storyAt(t.trackAt(false, 1), "P", 3.0) && storyAt(t.trackAt(true, 1), "Pa", 3.0);
            std::fprintf(stderr, "[track-ops] G28b: accepted=%d audioTracks=%d A1 [%s] A2 [%s] (expect Pa@3.00 on A2)"
                         " -> %d\n", accepted, t.audioTrackCount(), storyLayout(t.trackAt(true, 0)).c_str(),
                         storyLayout(t.trackAt(true, 1)).c_str(), bOk);
        }
        {
            Timeline t;
            t.restoreFromProject(
                QVector<QVector<ClipInfo>>{{}, {storyClip("Va", 3.0, 3, 10.0)},
                                           {storyClip("Vb", 3.0, 3), storyClip("K", 2.0, 0, 2.0)}},
                QVector<QVector<ClipInfo>>{{}, {storyClip("Aa", 3.0, 3, 10.0)}, {}}, 0.0, -1.0, -1.0, 100);
            // Roto marker n = n path points: Va 1, Vb 2, K 3.
            auto roto = [](const QString &key, int marker) {
                RotoClipEntry e;
                e.clipId = key;
                e.path.points.resize(marker);
                return e;
            };
            t.setRotoEntries({{trackMatteClipKey(1, 0), roto(trackMatteClipKey(1, 0), 1)},
                              {trackMatteClipKey(2, 0), roto(trackMatteClipKey(2, 0), 2)},
                              {trackMatteClipKey(2, 1), roto(trackMatteClipKey(2, 1), 3)}});
            TimelineTrackMatteEntry matte;
            matte.matteType = TrackMatteType::AlphaMatte;
            matte.matteSourceClipId = trackMatteClipKey(1, 0);   // Vb takes Va as its matte
            t.setTrackMatteEntries({{trackMatteClipKey(2, 0), matte}});
            t.setClipParentEntries({{trackMatteClipKey(2, 0), trackMatteClipKey(1, 0)}});   // Vb -> Va
            storyBaseline(t);
            auto carriers = [&t]() {
                QStringList parts;
                const auto rotos = t.rotoEntries();
                for (int marker = 1; marker <= 3; ++marker) {
                    QString at = QStringLiteral("missing");
                    for (auto it = rotos.cbegin(); it != rotos.cend(); ++it)
                        if (it.value().path.points.size() == marker) at = it.key();
                    parts.append(QStringLiteral("roto%1@%2").arg(marker).arg(at));
                }
                parts.append(QStringLiteral("rotoCount=%1").arg(rotos.size()));
                const auto mattes = t.trackMatteEntries();
                for (auto it = mattes.cbegin(); it != mattes.cend(); ++it)
                    parts.append(QStringLiteral("matte %1<%2").arg(it.key(), it.value().matteSourceClipId));
                const auto parents = t.clipParentEntries();
                for (auto it = parents.cbegin(); it != parents.cend(); ++it)
                    parts.append(QStringLiteral("parent %1>%2").arg(it.key(), it.value()));
                return parts.join(QLatin1Char(' ')).toStdString();
            };
            const std::string carriersBefore = carriers();
            const auto serial = t.undoManager()->saveSerial();
            const bool accepted = storyClipDrop(t.trackAt(true, 1), 0, t.trackAt(true, 2), 10.0);
            const int videoCount = t.videoTrackCount();
            const bool moved = accepted && videoCount == 4 && t.trackAt(false, 1)->clipCount() == 0
                && t.trackAt(false, 2)->clipCount() == 2 && storyAt(t.trackAt(false, 2), "K", 5.0)
                && storyAt(t.trackAt(false, 2), "Va", 10.0) && t.trackAt(false, 3)->clipCount() == 1
                && storyAt(t.trackAt(false, 3), "Vb", 10.0) && storyAt(t.trackAt(true, 2), "Aa", 10.0);
            const std::string carriersAfter = carriers();
            const std::string v3 = storyLayout(t.trackAt(false, 2));
            const std::string v4 = videoCount > 3 ? storyLayout(t.trackAt(false, 3)) : std::string("-");
            // Va V2:0 -> V3:1, Vb V3:0 -> V4:0, K V3:1 -> V3:0.
            const std::string expectedAfter = "roto1@2:1 roto2@3:0 roto3@2:0 rotoCount=3 matte 3:0<2:1 parent 3:0>2:1";
            const bool oneUndo = t.undoManager()->saveSerial() == serial + 1;
            t.undo();
            const std::string carriersUndone = carriers();
            const bool undone = t.videoTrackCount() == 3 && storyAt(t.trackAt(false, 1), "Va", 10.0)
                && storyAt(t.trackAt(false, 2), "Vb", 0.0) && storyAt(t.trackAt(true, 1), "Aa", 10.0)
                && carriersUndone == carriersBefore;
            cOk = moved && carriersAfter == expectedAfter && oneUndo && undone;
            std::fprintf(stderr, "[track-ops] G28c: accepted=%d videoTracks=%d V3 [%s] V4 [%s] moved=%d\n"
                         "[track-ops] G28c: before  %s\n[track-ops] G28c: after   %s\n"
                         "[track-ops] G28c: expect  %s\n[track-ops] G28c: undone  %s oneUndo=%d undone=%d -> %d\n",
                         accepted, videoCount, v3.c_str(), v4.c_str(), moved,
                         carriersBefore.c_str(), carriersAfter.c_str(), expectedAfter.c_str(),
                         carriersUndone.c_str(), oneUndo, undone, cOk);
        }
        // (d) uid twin: V3 already holds T (same uid as the moving picture W, no roto)
        // before the drop point; W+Wa dropped A2 -> A3 brings W to V3:1 after T, and
        // W's roto must follow W, not land on its twin T at V3:0.
        bool dOk = false;
        {
            Timeline t;
            ClipInfo w = storyClip("W", 3.0, 4, 10.0);
            w.uid = QStringLiteral("track-ops-g28d-twin");
            ClipInfo twin = storyClip("T", 2.0);
            twin.uid = w.uid;
            t.restoreFromProject(QVector<QVector<ClipInfo>>{{}, {w}, {twin}},
                                 QVector<QVector<ClipInfo>>{{}, {storyClip("Wa", 3.0, 4, 10.0)}, {}}, 0.0, -1.0,
                                 -1.0, 100);
            RotoClipEntry e;
            e.clipId = trackMatteClipKey(1, 0);
            e.path.points.resize(1);
            t.setRotoEntries({{trackMatteClipKey(1, 0), e}});
            storyBaseline(t);
            const QString wUid = storyFind(t.trackAt(false, 1), "W") ? storyFind(t.trackAt(false, 1), "W")->uid : QString();
            const QString tUid = storyFind(t.trackAt(false, 2), "T") ? storyFind(t.trackAt(false, 2), "T")->uid : QString();
            const bool twins = !wUid.isEmpty() && wUid == tUid;
            auto rotoKeys = [&t]() { return QStringList(t.rotoEntries().keys()).join(QLatin1Char(',')).toStdString(); };
            const std::string keysBefore = rotoKeys();
            const bool accepted = storyClipDrop(t.trackAt(true, 1), 0, t.trackAt(true, 2), 10.0);
            const bool moved = accepted && t.trackAt(false, 1)->clipCount() == 0
                && t.trackAt(false, 2)->clipCount() == 2 && storyAt(t.trackAt(false, 2), "T", 0.0)
                && storyAt(t.trackAt(false, 2), "W", 10.0) && t.trackAt(false, 2)->clips().at(1).displayName == "W";
            const std::string keysAfter = rotoKeys();
            const std::string expectedKeys = trackMatteClipKey(2, 1).toStdString();
            dOk = twins && moved && keysAfter == expectedKeys;
            std::fprintf(stderr, "[track-ops] G28d: twins=%d (uid '%s' / '%s') accepted=%d V3 [%s] moved=%d roto before [%s]"
                         " after [%s] (expect W's new key [%s], twin T at %s) -> %d\n", twins,
                         wUid.toUtf8().constData(), tUid.toUtf8().constData(), accepted,
                         storyLayout(t.trackAt(false, 2)).c_str(), moved, keysBefore.c_str(), keysAfter.c_str(),
                         expectedKeys.c_str(), trackMatteClipKey(2, 0).toUtf8().constData(), dOk);
        }
        // (e) a locked track neither gives up nor takes a member: Ea (V2, locked) + Eb
        // (V3) + Ec (A2), Eb dropped V3 -> V4: Ea stays on V2 (same uid and start), no
        // clip lands on V2, Ec follows to A4, one undo point. Before the fix Ea moved to V3.
        bool eOk = false;
        {
            Timeline t;
            t.restoreFromProject(
                QVector<QVector<ClipInfo>>{{}, {storyClip("Ea", 3.0, 7, 6.0)}, {storyClip("Eb", 3.0, 7, 6.0)}, {}},
                QVector<QVector<ClipInfo>>{{}, {storyClip("Ec", 3.0, 7, 6.0)}}, 0.0, -1.0, -1.0, 100);
            t.trackAt(false, 1)->setLocked(true);
            storyBaseline(t);
            const QString eaUid = storyFind(t.trackAt(false, 1), "Ea") ? storyFind(t.trackAt(false, 1), "Ea")->uid : QString();
            const auto serial = t.undoManager()->saveSerial();
            const bool accepted = storyClipDrop(t.trackAt(false, 2), 0, t.trackAt(false, 3), 6.0);
            const ClipInfo *ea = storyFind(t.trackAt(false, 1), "Ea");
            const bool lockedKept = t.trackAt(false, 1)->clipCount() == 1 && ea && !eaUid.isEmpty()
                && ea->uid == eaUid && storyAt(t.trackAt(false, 1), "Ea", 6.0);
            const bool moved = accepted && t.videoTrackCount() == 4 && t.trackAt(false, 2)->clipCount() == 0
                && t.trackAt(false, 3)->clipCount() == 1 && storyAt(t.trackAt(false, 3), "Eb", 6.0)
                && t.audioTrackCount() >= 4 && t.trackAt(true, 1)->clipCount() == 0
                && t.trackAt(true, 3)->clipCount() == 1 && storyAt(t.trackAt(true, 3), "Ec", 6.0);
            const bool oneUndo = t.undoManager()->saveSerial() == serial + 1;
            eOk = lockedKept && moved && oneUndo;
            std::fprintf(stderr, "[track-ops] G28e: accepted=%d V2(locked) [%s] uid '%s' -> '%s' V3 [%s] V4 [%s] A2 [%s]"
                         " A4 [%s] (expect Ea@6.00 kept on V2, V3 empty, Eb@6.00 on V4, Ec@6.00 on A4)"
                         " lockedKept=%d moved=%d oneUndo=%d -> %d\n", accepted,
                         storyLayout(t.trackAt(false, 1)).c_str(), eaUid.toUtf8().constData(),
                         ea ? ea->uid.toUtf8().constData() : "-", storyLayout(t.trackAt(false, 2)).c_str(),
                         t.videoTrackCount() > 3 ? storyLayout(t.trackAt(false, 3)).c_str() : "-",
                         storyLayout(t.trackAt(true, 1)).c_str(),
                         t.audioTrackCount() > 3 ? storyLayout(t.trackAt(true, 3)).c_str() : "-",
                         lockedKept, moved, oneUndo, eOk);
        }
        // (f) the same lock rule for a later member of the other track type: Fv (V2) + Fa
        // (A2) + Fb (A5) at 5, Fv dropped V2 -> V3, so the anchor Fa goes to A3 and Fb maps
        // to A6. (f1) A5 locked: Fb stays on A5 (same uid and start). (f2) A6 exists and is
        // locked: Fb stays on A5 and A6 keeps its clip. Both keep the link group and add one
        // undo point. Before the fix Fb moved to A6 in both cases.
        bool fOk = true;
        for (int fCase = 1; fCase <= 2; ++fCase) {
            Timeline t;
            QVector<QVector<ClipInfo>> audio{{}, {storyClip("Fa", 3.0, 8, 5.0)}, {}, {}, {storyClip("Fb", 3.0, 8, 5.0)}};
            if (fCase == 2) audio.push_back(QVector<ClipInfo>{storyClip("Fl", 2.0, 0, 20.0)});
            t.restoreFromProject(QVector<QVector<ClipInfo>>{{}, {storyClip("Fv", 3.0, 8, 5.0)}, {}}, audio,
                                 0.0, -1.0, -1.0, 100);
            t.trackAt(true, fCase == 1 ? 4 : 5)->setLocked(true);
            storyBaseline(t);
            const QString fbUid = storyFind(t.trackAt(true, 4), "Fb") ? storyFind(t.trackAt(true, 4), "Fb")->uid : QString();
            const auto serial = t.undoManager()->saveSerial();
            const bool accepted = storyClipDrop(t.trackAt(false, 1), 0, t.trackAt(false, 2), 5.0);
            const int audioCount = t.audioTrackCount();
            const ClipInfo *fb = storyFind(t.trackAt(true, 4), "Fb");
            const bool lockedKept = t.trackAt(true, 4)->clipCount() == 1 && fb && !fbUid.isEmpty() && fb->uid == fbUid
                && storyAt(t.trackAt(true, 4), "Fb", 5.0);
            const bool a6Ok = fCase == 1 ? audioCount == 5
                                         : audioCount == 6 && t.trackAt(true, 5)->clipCount() == 1
                                               && storyAt(t.trackAt(true, 5), "Fl", 20.0);
            const bool moved = accepted && t.trackAt(false, 1)->clipCount() == 0 && storyAt(t.trackAt(false, 2), "Fv", 5.0)
                && t.trackAt(true, 1)->clipCount() == 0 && t.trackAt(true, 2)->clipCount() == 1
                && storyAt(t.trackAt(true, 2), "Fa", 5.0);
            const int group = linkGroupOf(t.trackAt(false, 2), "Fv");
            const bool grouped = group > 0 && linkGroupOf(t.trackAt(true, 2), "Fa") == group
                && linkGroupOf(t.trackAt(true, 4), "Fb") == group;
            const bool oneUndo = t.undoManager()->saveSerial() == serial + 1;
            const bool caseOk = lockedKept && a6Ok && moved && grouped && oneUndo;
            fOk = fOk && caseOk;
            std::fprintf(stderr, "[track-ops] G28f%d: accepted=%d audioTracks=%d A3 [%s] A5(%s) [%s] uid '%s' -> '%s'"
                         " A6(%s) [%s] (expect Fa@5.00 on A3, Fb@5.00 kept on A5, %s) lockedKept=%d a6Ok=%d"
                         " moved=%d group=%d grouped=%d oneUndo=%d -> %d\n", fCase, accepted, audioCount,
                         storyLayout(t.trackAt(true, 2)).c_str(), fCase == 1 ? "locked" : "free",
                         storyLayout(t.trackAt(true, 4)).c_str(), fbUid.toUtf8().constData(),
                         fb ? fb->uid.toUtf8().constData() : "-", fCase == 2 ? "locked" : "none",
                         audioCount > 5 ? storyLayout(t.trackAt(true, 5)).c_str() : "-",
                         fCase == 1 ? "no A6" : "Fl@20.00 alone on A6", lockedKept, a6Ok, moved, group,
                         grouped, oneUndo, caseOk);
        }
        gate(28, aOk && bOk && cOk && dOk && eOk && fOk);
    }
    // US-1313 G29: storyline mode ON, a group of V2's picture G and two audio clips
    // (A1 / A2 at 10) dropped on V1 at 4 (between P and Q) is a storyline insert: both
    // audio clips land at 4 on free audio tracks and V1 stays gap-free. HEAD: only
    // the first audio clip moves, the A2 one stays at 10.
    {
        Timeline t;
        t.restoreFromProject(
            QVector<QVector<ClipInfo>>{{storyClip("P", 4.0, 5), storyClip("Q", 4.0, 6)},
                                       {storyClip("G", 3.0, 9, 10.0)}},
            QVector<QVector<ClipInfo>>{{storyClip("Pa", 4.0, 5), storyClip("Qa", 4.0, 6), storyClip("Ga1", 3.0, 9, 2.0)},
                                       {storyClip("Ga2", 3.0, 9, 10.0)}},
            0.0, -1.0, -1.0, 100);
        t.setStorylineMode(true);
        storyBaseline(t);
        const auto serial = t.undoManager()->saveSerial();
        const bool accepted = storyClipDrop(t.trackAt(false, 1), 0, t.trackAt(false, 0), 4.0);
        bool gapFree = t.trackAt(false, 0)->clipCount() == 3;
        for (const ClipInfo &c : t.trackAt(false, 0)->clips()) gapFree = gapFree && std::abs(c.leadInSec) < 1e-6;
        const bool v1Ok = gapFree && storyAt(t.trackAt(false, 0), "P", 0.0) && storyAt(t.trackAt(false, 0), "G", 4.0)
            && storyAt(t.trackAt(false, 0), "Q", 7.0);
        double ga1 = -1.0, ga2 = -1.0;
        int ga1Track = -1, ga2Track = -1, ga1Count = 0, ga2Count = 0;
        std::string audio;
        for (int a = 0; a < t.audioTrackCount(); ++a) {
            const TimelineTrack *track = t.trackAt(true, a);
            audio += "A" + std::to_string(a + 1) + " [" + storyLayout(track) + "] ";
            for (const ClipInfo &c : track->clips()) {
                if (c.displayName == QStringLiteral("Ga1")) { ++ga1Count; ga1Track = a; ga1 = storyStart(track, "Ga1"); }
                if (c.displayName == QStringLiteral("Ga2")) { ++ga2Count; ga2Track = a; ga2 = storyStart(track, "Ga2"); }
            }
        }
        const bool audioOk = ga1Count == 1 && ga2Count == 1 && ga1Track != ga2Track
            && std::abs(ga1 - 4.0) < 1e-6 && std::abs(ga2 - 4.0) < 1e-6
            && storyAt(t.trackAt(true, 0), "Qa", 7.0);
        const bool oneUndo = t.undoManager()->saveSerial() == serial + 1;
        std::fprintf(stderr, "[track-ops] G29: accepted=%d V1 [%s] (expect P@0.00 G@4.00 Q@7.00) %s(expect Ga1 / Ga2 "
                     "@4.00 on two tracks, Qa@7.00) Ga1=A%d@%.2f Ga2=A%d@%.2f v1Ok=%d audioOk=%d oneUndo=%d\n",
                     accepted, storyLayout(t.trackAt(false, 0)).c_str(), audio.c_str(), ga1Track + 1, ga1,
                     ga2Track + 1, ga2, v1Ok, audioOk, oneUndo);
        gate(29, accepted && v1Ok && audioOk && oneUndo);
    }
    std::fprintf(stderr, "[track-ops] summary: %d PASS, %d FAIL\n", passed, failed);
    // The registry checks the declared "(N gates)" against this count.
    selftests::reportTally(passed, failed, 0);
    return failed;
}
