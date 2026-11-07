// US-2001: the project's dirty state (the id of the last saved undo entry plus
// edits outside undo) and the save / discard / cancel prompt before a project
// is replaced. The gates never call closeEvent (it writes the real QSettings
// and the real autosave directory); they observe maybeSaveChanges() and the
// prompt / save-as / failure-dialog / missing-media seams.
#include "../AudioMixer.h"
#include "../AudioMixerDock.h"
#include "../EffectClipboard.h"
#include "../EffectControlsPanel.h"
#include "../MainWindow.h"
#include "../MediaPaths.h"
#include "../MotionSectionWidget.h"
#include "../MultiCam.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"
#include "SelftestGate.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QRegularExpression>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <string>

namespace {

using Choice = MainWindow::UnsavedChangesChoice;

std::string b(bool v) { return v ? "1" : "0"; }

bool touchFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("media", 5) == 5;
}

ClipInfo makeClip(const QString &path)
{
    ClipInfo clip;
    clip.filePath = path;
    clip.duration = 1.0;
    clip.outPoint = 1.0;
    return clip;
}

int clipCount(const Timeline &timeline)
{
    int n = 0;
    for (const auto *tracks : {&timeline.videoTracks(), &timeline.audioTracks()})
        for (const TimelineTrack *track : *tracks)
            n += track ? track->clips().size() : 0;
    return n;
}

QPushButton *headerButton(Timeline &timeline, const QString &trackName, const QString &objectName)
{
    for (auto *label : timeline.findChildren<QLabel *>(QStringLiteral("timelineTrackName"))) {
        QWidget *header = label->parentWidget();
        if (label->property("defaultName").toString() != trackName || !header
            || !header->parentWidget() || !header->parentWidget()->layout()
            || header->parentWidget()->layout()->indexOf(header) < 0)
            continue;
        return header->findChild<QPushButton *>(objectName, Qt::FindDirectChildrenOnly);
    }
    return nullptr;
}

// US-2108: answers whatever the leg under test opens, so a dialog never blocks the run. A
// message box is answered Yes; any other modal dialog gets `prep` and is accepted; a popup
// menu has its first action triggered, or the action whose text is `actionText`.
struct DialogDriver {
    DialogDriver(QObject *context, std::function<void(QWidget *)> prepare = {}, QString actionText = {})
        : prep(std::move(prepare)), action(std::move(actionText))
    {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, context, [this] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                ++answered;
                QAction *target = menu->actions().isEmpty() ? nullptr : menu->actions().first();
                for (QAction *candidate : menu->actions())
                    if (!action.isEmpty() && candidate->text() == action) target = candidate;
                if (target) target->trigger();
                menu->close();
                return;
            }
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal) return;
            ++answered;
            if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                if (QAbstractButton *yes = box->button(QMessageBox::Yes)) yes->click();
                else box->reject();
                return;
            }
            if (prep) prep(modal);
            QMetaObject::invokeMethod(modal, "accept");
        });
        timer.start();
    }
    ~DialogDriver() { timer.stop(); }
    QTimer timer;
    std::function<void(QWidget *)> prep;
    QString action;
    int answered = 0;
};

} // namespace

int runUnsavedPromptSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir dir;
    if (!dir.isValid()) {
        for (int g = 1; g <= 12; ++g)
            tally.gate(g, false, "temporary directory unavailable");
        return tally.finish("unsaved-prompt");
    }
    const auto tmp = [&](const char *name) { return QDir(dir.path()).filePath(QString::fromLatin1(name)); };

    // ---- G7 (e): track flags, on a fresh MainWindow ----
    bool g7e = false;
    std::string g7eDetail;
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        MainWindow window;
        Timeline &timeline = *window.m_timeline;
        auto *dock = window.findChild<AudioMixerDock *>(QStringLiteral("AudioMixerDock"));
        QPushButton *mixerMute = nullptr;
        QPushButton *mixerSolo = nullptr;
        if (dock) {
            const bool found = QTest::qWaitFor([&]() {
                QCoreApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QWidget *root = dock->widget() ? dock->widget() : static_cast<QWidget *>(dock);
                mixerMute = root->findChild<QPushButton *>(QStringLiteral("mixerMute0"));
                mixerSolo = root->findChild<QPushButton *>(QStringLiteral("mixerSolo0"));
                return mixerMute && mixerSolo;
            }, 5000);
            (void)found;
        }
        QPushButton *lockV1 = headerButton(timeline, QStringLiteral("V1"), QStringLiteral("timelineTrackLockButton"));
        QPushButton *hideV1 = headerButton(timeline, QStringLiteral("V1"), QStringLiteral("timelineTrackHideButton"));
        QPushButton *muteA1 = headerButton(timeline, QStringLiteral("A1"), QStringLiteral("timelineTrackMuteButton"));
        QPushButton *soloA1 = headerButton(timeline, QStringLiteral("A1"), QStringLiteral("timelineTrackSoloButton"));
        const bool premise = timeline.videoTrackCount() >= 1 && timeline.audioTrackCount() >= 1
            && dock && mixerMute && mixerSolo && lockV1 && hideV1 && muteA1 && soloA1;
        g7eDetail = "premise v=" + std::to_string(timeline.videoTrackCount())
            + " a=" + std::to_string(timeline.audioTrackCount())
            + " dock=" + b(dock) + " mixerMute0=" + b(mixerMute) + " mixerSolo0=" + b(mixerSolo)
            + " lock=" + b(lockV1) + " hide=" + b(hideV1) + " mute=" + b(muteA1) + " solo=" + b(soloA1);
        g7e = premise;
        if (premise) {
            TimelineTrack *v1 = timeline.videoTracks()[0];
            TimelineTrack *a1 = timeline.audioTracks()[0];
            struct Leg {
                const char *name;
                std::function<void()> act;
                std::function<bool()> flag;
            };
            const QVector<Leg> legs = {
                {"headerLock", [&] { lockV1->click(); }, [&] { return v1->isLocked(); }},
                {"headerHide", [&] { hideV1->click(); }, [&] { return v1->isHidden(); }},
                {"headerMute", [&] { muteA1->click(); }, [&] { return a1->isMuted(); }},
                {"headerSolo", [&] { soloA1->click(); }, [&] { return a1->isSolo(); }},
                {"menuMute", [&] { QMetaObject::invokeMethod(&window, "toggleMute"); }, [&] { return a1->isMuted(); }},
                {"menuSolo", [&] { QMetaObject::invokeMethod(&window, "toggleSolo"); }, [&] { return a1->isSolo(); }},
                {"mixerMute", [&] { mixerMute->click(); }, [&] { return a1->isMuted(); }},
                {"mixerSolo", [&] { mixerSolo->click(); }, [&] { return a1->isSolo(); }},
            };
            // US-2106: each toggle is one named undo point; undo brings the flag and
            // the clean state back, redo flips it again.
            for (const Leg &leg : legs) {
                timeline.saveUndoState(QStringLiteral("pre"));
                window.markProjectClean();
                const bool before = leg.flag();
                const quint64 serial0 = timeline.undoManager()->saveSerial();
                QSignalSpy spy(&timeline, &Timeline::trackFlagsChanged);
                leg.act();
                const bool flipped = leg.flag() != before;
                const bool dirty = window.isWindowModified();
                const bool notClean = !timeline.undoManager()->isClean();
                const quint64 pushed = timeline.undoManager()->saveSerial() - serial0;
                const int emits = spy.count();
                timeline.undo();
                const bool restored = leg.flag() == before;
                const bool undoDirty = window.isWindowModified();
                timeline.redo();
                const bool reflipped = leg.flag() != before;
                const bool redoDirty = window.isWindowModified();
                const bool ok = flipped && dirty && notClean && pushed == 1 && emits == 1
                    && restored && !undoDirty && reflipped && redoDirty;
                g7eDetail += std::string(" | ") + leg.name + " flipped=" + b(flipped) + " dirty=" + b(dirty)
                    + " notClean=" + b(notClean) + " pushed=" + std::to_string(pushed)
                    + " emits=" + std::to_string(emits) + " undoRestored=" + b(restored)
                    + " afterUndo=" + b(undoDirty) + " (expect 0) redoFlipped=" + b(reflipped)
                    + " afterRedo=" + b(redoDirty);
                g7e = g7e && ok;
            }
        }
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    MainWindow window;
    Timeline &timeline = *window.m_timeline;
    UndoManager *undo = timeline.undoManager();
    Choice next = Choice::Cancel;
    const auto count = [&] { return window.unsavedChangesPromptCountForTest(); };

    // ---- G1: the constructed window is clean; an undo-recorded edit is dirty ----
    {
        const bool clean0 = undo->isClean();
        const bool mod0 = window.isWindowModified();
        window.setUnsavedChangesPromptForTest([&] { return next; });
        next = Choice::Cancel;
        const int c0 = count();
        const bool ask0 = window.maybeSaveChanges();
        const int c1 = count();
        timeline.addVideoTrack(true);
        const bool modAfterAdd = window.isWindowModified();
        window.markProjectClean();
        const bool ask1 = window.maybeSaveChanges();
        const int c2 = count();
        const bool ok = clean0 && !mod0 && ask0 && c1 == c0 && c0 == 0 && modAfterAdd && ask1 && c2 == 0;
        tally.gate(1, ok, "isClean=" + b(clean0) + " modified=" + b(mod0) + " ask=" + b(ask0)
            + " count=" + std::to_string(c1) + " afterAddTrack=" + b(modAfterAdd)
            + " (b) ask=" + b(ask1) + " count=" + std::to_string(c2));
    }

    // ---- G2: dirty + Cancel ----
    {
        window.markProjectDirty();
        next = Choice::Cancel;
        const int clips0 = clipCount(timeline);
        const int c0 = count();
        const bool r = window.maybeSaveChanges();
        const bool ok = !r && count() == c0 + 1 && window.isWindowModified() && clipCount(timeline) == clips0;
        tally.gate(2, ok, "ret=" + b(r) + " countDelta=" + std::to_string(count() - c0)
            + " modified=" + b(window.isWindowModified()) + " clips=" + std::to_string(clipCount(timeline))
            + "/" + std::to_string(clips0));
    }

    // ---- G3: dirty + Discard ----
    {
        next = Choice::Discard;
        const int c0 = count();
        const bool r = window.maybeSaveChanges();
        const bool ok = r && count() == c0 + 1 && window.isWindowModified();
        tally.gate(3, ok, "ret=" + b(r) + " countDelta=" + std::to_string(count() - c0)
            + " modified=" + b(window.isWindowModified()));
    }

    // ---- G4: dirty + Save to the current path ----
    const QString projectA = tmp("g4.veditor");
    {
        window.m_projectFilePath = projectA;
        next = Choice::Save;
        const int c0 = count();
        const bool r = window.maybeSaveChanges();
        const QFileInfo info(projectA);
        const bool ok = r && count() == c0 + 1 && info.exists() && info.size() > 0 && !window.isWindowModified();
        tally.gate(4, ok, "ret=" + b(r) + " exists=" + b(info.exists()) + " size=" + std::to_string(info.size())
            + " modified=" + b(window.isWindowModified()));
    }

    // ---- G5: a failed save and save-as ----
    const QString projectB = tmp("g5c.veditor");
    {
        window.setSaveFailureDialogSuppressedForTest(true);
        window.markProjectDirty();
        window.m_projectFilePath = QDir(dir.path()).filePath(QStringLiteral("missing-dir/sub/x.veditor"));
        next = Choice::Save;
        const int c0 = count();
        const bool ra = window.maybeSaveChanges();
        const int countA = count() - c0;
        const bool okA = !ra && countA == 1 && window.isWindowModified();

        window.m_projectFilePath.clear();
        int providerCalls = 0;
        QString provided;
        window.setSaveAsPathProviderForTest([&] { ++providerCalls; return provided; });
        const bool rb = window.maybeSaveChanges();
        const bool okB = !rb && providerCalls == 1 && window.isWindowModified();

        provided = projectB;
        const bool rc = window.maybeSaveChanges();
        const bool okC = rc && providerCalls == 2 && QFileInfo::exists(projectB)
            && window.m_projectFilePath == projectB && !window.isWindowModified();
        window.setSaveAsPathProviderForTest({});
        tally.gate(5, okA && okB && okC, "(a) ret=" + b(ra) + " countDelta=" + std::to_string(countA)
            + " (b) ret=" + b(rb) + " provider=" + std::to_string(providerCalls) + " modified=" + b(okB)
            + " (c) ret=" + b(rc) + " exists=" + b(QFileInfo::exists(projectB))
            + " path=" + b(window.m_projectFilePath == projectB));
    }

    // ---- G6: other paths, autosave restore, relink on load ----
    {
        std::string d;
        const QString mediaPresent = tmp("g6-present.mp4");
        const QString mediaMissing = tmp("g6-offline.mp4");
        bool ok = touchFile(mediaPresent);
        // A state that differs from projectA (which has no clips).
        timeline.videoTracks()[0]->addClip(makeClip(mediaPresent));
        timeline.saveUndoState(QStringLiteral("clip"));
        const int c0 = count();
        // (a) recent file + Cancel: nothing loaded.
        next = Choice::Cancel;
        const QString pathBefore = window.m_projectFilePath;
        window.openRecentFile(projectA);
        const bool okA = window.isWindowModified() && count() == c0 + 1 && clipCount(timeline) == 1
            && window.m_projectFilePath == pathBefore;
        d += "(a) count=" + std::to_string(count() - c0) + " clips=" + std::to_string(clipCount(timeline));
        // (b) MCP open: no prompt.
        QString err;
        const bool loadedB = window.openProjectFromPath(projectA, &err, false);
        const bool okB = loadedB && count() == c0 + 1 && clipCount(timeline) == 0
            && window.m_projectFilePath == projectA && !window.isWindowModified();
        d += " (b) loaded=" + b(loadedB) + " count=" + std::to_string(count() - c0)
            + " clips=" + std::to_string(clipCount(timeline)) + " modified=" + b(window.isWindowModified());
        // (c) recent file + Discard: loaded.
        timeline.videoTracks()[0]->addClip(makeClip(mediaPresent));
        timeline.saveUndoState(QStringLiteral("clip"));
        window.m_projectFilePath = projectB;
        next = Choice::Discard;
        window.openRecentFile(projectA);
        const bool okC = count() == c0 + 2 && clipCount(timeline) == 0 && window.m_projectFilePath == projectA
            && !window.isWindowModified();
        d += " (c) count=" + std::to_string(count() - c0) + " clips=" + std::to_string(clipCount(timeline))
            + " modified=" + b(window.isWindowModified());
        // (d) autosave restore stays dirty (explicitly, the undo side is clean).
        window.markProjectClean();
        QFile fileA(projectA);
        const QString json = fileA.open(QIODevice::ReadOnly) ? QString::fromUtf8(fileA.readAll()) : QString();
        fileA.close();
        const bool restored = window.restoreFromAutosaveJson(json);
        const bool modD = window.isWindowModified();
        const bool pathEmpty = window.m_projectFilePath.isEmpty();
        const bool cleanD = undo->isClean();
        next = Choice::Cancel;
        const bool askD = window.maybeSaveChanges();
        const bool okD = restored && modD && pathEmpty && cleanD && !askD;
        d += " (d) restored=" + b(restored) + " modified=" + b(modD) + " pathEmpty=" + b(pathEmpty)
            + " isClean=" + b(cleanD) + " ask=" + b(askD);
        // (e) unreadable autosave: nothing changes.
        const bool modE0 = window.isWindowModified();
        const bool cleanE0 = undo->isClean();
        const int tracksE0 = timeline.videoTrackCount() + timeline.audioTrackCount();
        const bool rEmpty = window.restoreFromAutosaveJson(QString());
        const bool rBrace = window.restoreFromAutosaveJson(QStringLiteral("{"));
        const bool okE = !rEmpty && !rBrace && window.isWindowModified() == modE0 && undo->isClean() == cleanE0
            && timeline.videoTrackCount() + timeline.audioTrackCount() == tracksE0;
        d += " (e) empty=" + b(rEmpty) + " brace=" + b(rBrace) + " unchanged=" + b(okE);

        // (f)-(i) relink on load.
        const QString relinkProject = tmp("g6-relink.veditor");
        int missingBefore = -1;
        {
            ProjectData data;
            data.videoTracks = ProjectTrackClips{QVector<ClipInfo>{makeClip(mediaMissing), makeClip(mediaPresent)}};
            ok = ok && ProjectFile::save(relinkProject, data);
            ProjectData reread;
            ok = ok && ProjectFile::load(relinkProject, reread);
            missingBefore = mediapaths::missingFiles(reread).size();
        }
        const auto missingNow = [&] {
            ProjectData data;
            window.populateProjectData(data);
            return int(mediapaths::missingFiles(data).size());
        };
        int resolverCalls = 0;
        // (f) a successful relink stays dirty, even after its undo.
        window.setMissingMediaResolverForTest([&](const QStringList &missing) {
            ++resolverCalls;
            return QHash<QString, QString>{{missing.value(0), mediaPresent}};
        });
        int cf = count();
        const bool loadedF = window.openProjectFromPath(relinkProject, &err, true);
        const int missingAfter = missingNow();
        const bool modF = window.isWindowModified();
        const bool cleanF = undo->isClean();
        const bool askF = window.maybeSaveChanges();
        const int countF = count() - cf;
        timeline.undo();
        const bool cleanFUndo = undo->isClean();
        const bool modFUndo = window.isWindowModified();
        window.markProjectClean();
        const bool modFCleared = window.isWindowModified();
        window.setMissingMediaResolverForTest({});
        const bool okF = missingBefore == 1 && loadedF && resolverCalls == 1 && missingAfter == 0 && modF
            && !cleanF && !askF && countF == 1 && cleanFUndo && modFUndo && !modFCleared;
        d += " (f) missingBefore=" + std::to_string(missingBefore) + " loaded=" + b(loadedF)
            + " resolver=" + std::to_string(resolverCalls) + " missingAfter=" + std::to_string(missingAfter)
            + " modified=" + b(modF) + " isClean=" + b(cleanF) + " ask=" + b(askF)
            + " countDelta=" + std::to_string(countF) + " undo:isClean=" + b(cleanFUndo)
            + " modified=" + b(modFUndo) + " markClean:modified=" + b(modFCleared);
        // (g) negative control 1: an empty mapping keeps the load clean.
        resolverCalls = 0;
        window.setMissingMediaResolverForTest([&](const QStringList &) {
            ++resolverCalls;
            return QHash<QString, QString>{};
        });
        const bool loadedG = window.openProjectFromPath(relinkProject, &err, true);
        const bool okG = loadedG && resolverCalls == 1 && !window.isWindowModified() && undo->isClean();
        d += " (g) loaded=" + b(loadedG) + " resolver=" + std::to_string(resolverCalls)
            + " modified=" + b(window.isWindowModified()) + " isClean=" + b(undo->isClean());
        window.setMissingMediaResolverForTest({});
        // (h) negative control 2: the MCP path never asks the resolver.
        resolverCalls = 0;
        window.setMissingMediaResolverForTest([&](const QStringList &missing) {
            ++resolverCalls;
            return QHash<QString, QString>{{missing.value(0), mediaPresent}};
        });
        window.markProjectDirty();
        const bool loadedH = window.openProjectFromPath(relinkProject, &err, false);
        const bool okH = loadedH && resolverCalls == 0 && !window.isWindowModified();
        d += " (h) loaded=" + b(loadedH) + " resolver=" + std::to_string(resolverCalls)
            + " modified=" + b(window.isWindowModified());
        window.setMissingMediaResolverForTest({});
        // (i) negative control 3: a failed relink keeps the load clean.
        resolverCalls = 0;
        const QString nowhere = tmp("g6-nowhere.mp4");
        window.setMissingMediaResolverForTest([&](const QStringList &missing) {
            ++resolverCalls;
            return QHash<QString, QString>{{missing.value(0), nowhere}};
        });
        const bool loadedI = window.openProjectFromPath(relinkProject, &err, true);
        const bool okI = loadedI && resolverCalls == 1 && !window.isWindowModified() && undo->isClean();
        d += " (i) loaded=" + b(loadedI) + " resolver=" + std::to_string(resolverCalls)
            + " modified=" + b(window.isWindowModified()) + " isClean=" + b(undo->isClean());
        window.setMissingMediaResolverForTest({});

        tally.gate(6, ok && okA && okB && okC && okD && okE && okF && okG && okH && okI, d);
    }

    // ---- G7 (a)-(d), (f): real edits are dirty and an undo back to the save is clean ----
    {
        std::string d;
        next = Choice::Cancel;
        // (a) structural: the undo restores adjustment layers inside restoreState.
        window.markProjectClean();
        timeline.addVideoTrack(true);
        // The top undo entry was captured from currentState() by saveUndoState.
        const bool premiseAdj = timeline.currentState().externalTrackState.contains(QStringLiteral("adjustments"));
        const bool modA = window.isWindowModified();
        int c0 = count();
        const bool askA = window.maybeSaveChanges();
        const int countA = count() - c0;
        timeline.undo();
        const bool modAUndo = window.isWindowModified();
        c0 = count();
        const bool askAUndo = window.maybeSaveChanges();
        const int countAUndo = count() - c0;
        timeline.redo();
        const bool modARedo = window.isWindowModified();
        const bool okA = premiseAdj && modA && !askA && countA == 1 && !modAUndo && askAUndo && countAUndo == 0
            && modARedo;
        d += "(a) premise adjustments=" + b(premiseAdj) + " add=" + b(modA) + " ask=" + b(askA)
            + " undo=" + b(modAUndo) + " askAfterUndo=" + b(askAUndo) + " redo=" + b(modARedo);
        // (a) remap leg: removing V1 remaps the V2 adjustment layer outside undo.
        window.markProjectClean();
        if (timeline.videoTrackCount() < 2)
            timeline.addVideoTrack(true);
        AdjustmentLayer layer;
        layer.trackIndex = 1;
        layer.timelineStartUs = 0;
        layer.timelineEndUs = 1000000;
        layer.name = QStringLiteral("adj");
        timeline.addAdjustmentLayer(layer);
        window.markProjectClean();
        const int adjBefore = timeline.adjustmentLayers().size();
        const bool removed = timeline.removeTrack(false, 0);
        const bool remapped = timeline.adjustmentLayers().size() == adjBefore && adjBefore > 0
            && timeline.adjustmentLayers().first().trackIndex == 0;
        const bool modR = window.isWindowModified();
        timeline.undo();
        const bool modRUndo = window.isWindowModified();
        timeline.redo();
        const bool modRRedo = window.isWindowModified();
        const bool okR = removed && remapped && modR && !modRUndo && modRRedo;
        d += " (a-remap) removed=" + b(removed) + " remapped=" + b(remapped) + " dirty=" + b(modR)
            + " undo=" + b(modRUndo) + " redo=" + b(modRRedo);
        // (b) non-structural clip edit.
        window.markProjectClean();
        timeline.videoTracks()[0]->addClip(makeClip(tmp("g6-present.mp4")));
        timeline.saveUndoState(QStringLiteral("clip"));
        const bool modB = window.isWindowModified();
        timeline.undo();
        const bool modBUndo = window.isWindowModified();
        timeline.redo();
        const bool modBRedo = window.isWindowModified();
        const bool okB = modB && !modBUndo && modBRedo;
        d += " (b) clip=" + b(modB) + " undo=" + b(modBUndo) + " redo=" + b(modBRedo);
        // (c) marker (no undo entry) stays dirty after an undo back to the save.
        window.markProjectClean();
        timeline.addMarker(1000000, QStringLiteral("m"));
        const bool modC = window.isWindowModified();
        timeline.saveUndoState(QStringLiteral("x"));
        timeline.undo();
        const bool cleanC = undo->isClean();
        const bool modCUndo = window.isWindowModified();
        const bool okC = modC && cleanC && modCUndo;
        d += " (c) marker=" + b(modC) + " undo:isClean=" + b(cleanC) + " modified=" + b(modCUndo);
        // (d) explicit dirty.
        window.markProjectClean();
        window.markProjectDirty();
        timeline.saveUndoState(QStringLiteral("x"));
        timeline.undo();
        const bool cleanD = undo->isClean();
        const bool modDUndo = window.isWindowModified();
        const bool okD = cleanD && modDUndo;
        d += " (d) undo:isClean=" + b(cleanD) + " modified=" + b(modDUndo);
        // (f) project settings dialog.
        window.markProjectClean();
        ProjectConfig fpsOnly = window.m_projectConfig;
        fpsOnly.fps = fpsOnly.fps == 24 ? 30 : 24;
        window.applyEditedProjectConfig(fpsOnly);
        const bool modF1 = window.isWindowModified();
        const bool cleanF1 = undo->isClean();
        timeline.saveUndoState(QStringLiteral("x"));
        timeline.undo();
        const bool cleanF1Undo = undo->isClean();
        const bool modF1Undo = window.isWindowModified();
        window.markProjectClean();
        const bool modF1Cleared = window.isWindowModified();
        const bool okF1 = modF1 && cleanF1 && cleanF1Undo && modF1Undo && !modF1Cleared;
        d += " (f) fps: dirty=" + b(modF1) + " isClean=" + b(cleanF1) + " undo:isClean=" + b(cleanF1Undo)
            + " modified=" + b(modF1Undo) + " markClean=" + b(modF1Cleared);

        window.markProjectClean();
        ProjectConfig sizeExplicit = window.m_projectConfig;
        sizeExplicit.width = sizeExplicit.width == 1280 ? 1920 : 1280;
        sizeExplicit.height = sizeExplicit.height == 720 ? 1080 : 720;
        sizeExplicit.explicitOutputResolution = true;
        window.applyEditedProjectConfig(sizeExplicit);
        const bool modF2 = window.isWindowModified();
        const bool cleanF2 = undo->isClean();
        timeline.undo();
        const bool modF2Undo = window.isWindowModified();
        timeline.redo();
        const bool modF2Redo = window.isWindowModified();
        const bool okF2 = modF2 && !cleanF2 && !modF2Undo && modF2Redo;
        d += " size+explicit: dirty=" + b(modF2) + " isClean=" + b(cleanF2) + " undo=" + b(modF2Undo)
            + " redo=" + b(modF2Redo);

        window.markProjectClean();
        ProjectConfig sizeImplicit = window.m_projectConfig;
        sizeImplicit.width = sizeImplicit.width == 1280 ? 1920 : 1280;
        sizeImplicit.height = sizeImplicit.height == 720 ? 1080 : 720;
        sizeImplicit.explicitOutputResolution = false;
        window.applyEditedProjectConfig(sizeImplicit);
        const bool modF3 = window.isWindowModified();
        const bool cleanF3 = undo->isClean();
        timeline.undo();
        const bool modF3Undo = window.isWindowModified();
        timeline.redo();
        const bool modF3Redo = window.isWindowModified();
        const bool okF3 = modF3 && !cleanF3 && !modF3Undo && modF3Redo;
        d += " size+implicit: dirty=" + b(modF3) + " isClean=" + b(cleanF3) + " undo=" + b(modF3Undo)
            + " redo=" + b(modF3Redo);

        // A size change combined with an fps or a name change: the undo puts the size
        // back but not the fps / name, so the project stays dirty after it.
        bool okF5 = true;
        for (int leg = 0; leg < 2; ++leg) {
            window.markProjectClean();
            ProjectConfig mixed = window.m_projectConfig;
            mixed.width = mixed.width == 1280 ? 1920 : 1280;
            mixed.height = mixed.height == 720 ? 1080 : 720;
            mixed.explicitOutputResolution = true;
            if (leg == 0)
                mixed.fps = mixed.fps == 24 ? 30 : 24;
            else
                mixed.name += QStringLiteral("-renamed");
            window.applyEditedProjectConfig(mixed);
            const bool modM = window.isWindowModified();
            timeline.undo();
            const bool cleanMUndo = undo->isClean();
            const bool modMUndo = window.isWindowModified();
            window.markProjectClean();
            const bool modMCleared = window.isWindowModified();
            okF5 = okF5 && modM && cleanMUndo && modMUndo && !modMCleared;
            d += std::string(leg == 0 ? " size+explicit+fps" : " size+explicit+name") + ": dirty=" + b(modM)
                + " undo:isClean=" + b(cleanMUndo) + " modified=" + b(modMUndo) + " markClean=" + b(modMCleared);
        }

        // An explicit-output-only change records no undo point: after a save, an edit
        // and its undo, the restored entry must carry the saved explicit flag, so a
        // clean window really matches the saved state.
        window.markProjectClean();
        ProjectConfig explicitOnly = window.m_projectConfig;
        explicitOnly.explicitOutputResolution = !explicitOnly.explicitOutputResolution;
        window.applyEditedProjectConfig(explicitOnly);
        const bool modF6 = window.isWindowModified();
        window.markProjectClean();
        const bool savedF6 = explicitOnly.explicitOutputResolution;
        const bool cleanF6Saved = undo->isClean() && !window.isWindowModified();
        timeline.saveUndoState(QStringLiteral("x"));
        timeline.undo();
        const bool cleanF6Undo = undo->isClean();
        const bool explicitF6 = window.m_projectConfig.explicitOutputResolution;
        const bool modF6Undo = window.isWindowModified();
        const bool okF6 = modF6 && cleanF6Saved && cleanF6Undo && explicitF6 == savedF6 && !modF6Undo;
        d += " explicit-only: dirty=" + b(modF6) + " saved=" + b(cleanF6Saved) + " undo:isClean=" + b(cleanF6Undo)
            + " explicit=" + b(explicitF6) + "/" + b(savedF6) + " modified=" + b(modF6Undo);

        window.markProjectClean();
        const ProjectConfig same = window.m_projectConfig;
        window.applyEditedProjectConfig(same);
        const bool modF4 = window.isWindowModified();
        c0 = count();
        const bool askF4 = window.maybeSaveChanges();
        const bool okF4 = !modF4 && askF4 && count() == c0 && undo->isClean();
        d += " same: dirty=" + b(modF4) + " ask=" + b(askF4) + " countDelta=" + std::to_string(count() - c0)
            + " isClean=" + b(undo->isClean());

        d += " (e) " + g7eDetail;
        tally.gate(7, okA && okR && okB && okC && okD && okF1 && okF2 && okF3 && okF4 && okF5 && okF6 && g7e, d);
    }

    // ---- G8: New Project resets the timeline, the path and the history ----
    {
        timeline.videoTracks()[0]->addClip(makeClip(tmp("g6-present.mp4")));
        timeline.saveUndoState(QStringLiteral("clip"));
        window.m_projectFilePath = projectA;
        timeline.addMarker(2000000, QStringLiteral("g8"));
        AdjustmentLayer g8Layer;
        g8Layer.trackIndex = 0;
        g8Layer.timelineStartUs = 0;
        g8Layer.timelineEndUs = 1000000;
        g8Layer.name = QStringLiteral("g8");
        timeline.addAdjustmentLayer(g8Layer);
        window.markProjectDirty();
        const int markersBefore = timeline.markers().size();
        const int adjBefore = timeline.adjustmentLayers().size();
        const int clipsBefore = clipCount(timeline);
        const int vBefore = timeline.videoTrackCount();
        const int aBefore = timeline.audioTrackCount();
        ProjectConfig config;
        config.name = QStringLiteral("g8");
        config.width = 1280;
        config.height = 720;
        config.fps = 24;
        window.resetProject(config);
        const bool ok = clipsBefore > 0 && markersBefore > 0 && adjBefore > 0 && clipCount(timeline) == 0
            && timeline.markers().isEmpty() && timeline.adjustmentLayers().isEmpty() && window.m_projectFilePath.isEmpty()
            && !window.isWindowModified() && !undo->canUndo() && window.m_projectConfig.width == 1280
            && window.m_projectConfig.height == 720 && window.m_projectConfig.fps == 24;
        tally.gate(8, ok, "clips=" + std::to_string(clipsBefore) + "->" + std::to_string(clipCount(timeline))
            + " pathEmpty=" + b(window.m_projectFilePath.isEmpty()) + " modified=" + b(window.isWindowModified())
            + " canUndo=" + b(undo->canUndo()) + " config=" + std::to_string(window.m_projectConfig.width) + "x"
            + std::to_string(window.m_projectConfig.height) + "@" + std::to_string(window.m_projectConfig.fps)
            + " tracks v " + std::to_string(vBefore) + "->" + std::to_string(timeline.videoTrackCount())
            + " a " + std::to_string(aBefore) + "->" + std::to_string(timeline.audioTrackCount())
            + " markers " + std::to_string(markersBefore) + "->" + std::to_string(timeline.markers().size())
            + " adjustments " + std::to_string(adjBefore) + "->" + std::to_string(timeline.adjustmentLayers().size()));
    }

    // ---- G9: the clean entry's boundaries ----
    {
        std::string d;
        next = Choice::Cancel;
        // (i) a full history.
        window.markProjectClean();
        for (int i = 0; i < 101; ++i)
            timeline.saveUndoState(QStringLiteral("fill"));
        const int indexFull = undo->currentIndex();
        window.markProjectClean();
        const bool m1 = window.isWindowModified();
        timeline.saveUndoState(QStringLiteral("x"));
        const bool m2 = window.isWindowModified();
        timeline.undo();
        const bool m3 = window.isWindowModified();
        const bool okI = indexFull == UndoManager::MAX_UNDO - 1 && !m1 && m2 && !m3;
        d += "(i) index=" + std::to_string(indexFull) + " clean=" + b(m1) + " save=" + b(m2) + " undo=" + b(m3);
        // (ii) a branch after undo.
        timeline.saveUndoState(QStringLiteral("x"));
        window.markProjectClean();
        timeline.undo();
        const bool b1 = window.isWindowModified();
        timeline.saveUndoState(QStringLiteral("y"));
        const bool b2 = window.isWindowModified();
        timeline.undo();
        const bool b3 = window.isWindowModified();
        timeline.redo();
        const bool b4 = window.isWindowModified();
        window.markProjectClean();
        const bool b5 = window.isWindowModified();
        const bool okII = b1 && b2 && b3 && b4 && !b5;
        d += " (ii) undo=" + b(b1) + " branch=" + b(b2) + " undo=" + b(b3) + " redo=" + b(b4) + " clean=" + b(b5);
        // (iii) an absorbed edit makes the clean entry unreachable.
        timeline.saveUndoState(QStringLiteral("pre"));
        window.markProjectClean();
        QHash<QString, TimelineTrackMatteEntry> mattes;
        mattes.insert(QStringLiteral("0:0"), TimelineTrackMatteEntry{TrackMatteType::AlphaMatte, QStringLiteral("1:0")});
        timeline.setTrackMatteEntries(mattes);
        timeline.absorbIntoCurrentUndoPoint();
        const bool a1 = window.isWindowModified();
        timeline.undo();
        const bool a2 = window.isWindowModified();
        window.markProjectClean();
        const bool a3 = window.isWindowModified();
        const bool okIII = a1 && a2 && !a3;
        d += " (iii) absorb=" + b(a1) + " undo=" + b(a2) + " clean=" + b(a3);
        // (iv) negative control: an undo back to the clean entry is clean.
        window.markProjectClean();
        timeline.saveUndoState(QStringLiteral("x"));
        timeline.undo();
        const bool n1 = window.isWindowModified();
        const bool okIV = !n1 && undo->isClean();
        d += " (iv) undo=" + b(n1) + " isClean=" + b(undo->isClean());
        // (v) history jump. Step back first so the three saves below do not hit
        // MAX_UNDO (a full history keeps the index fixed and jumpTo(c) is a no-op).
        for (int i = 0; i < 5; ++i)
            timeline.undo();
        window.markProjectClean();
        const int c = undo->currentIndex();
        for (int i = 0; i < 3; ++i)
            timeline.saveUndoState(QStringLiteral("j"));
        undo->jumpTo(c);
        const bool j1 = window.isWindowModified();
        const int c0 = count();
        const bool askJ = window.maybeSaveChanges();
        const bool countJ = count() == c0;
        undo->jumpTo(c + 2);
        const bool j2 = window.isWindowModified();
        const bool okV = !j1 && askJ && countJ && j2;
        d += " (v) c=" + std::to_string(c) + " jumpToClean=" + b(j1) + " ask=" + b(askJ) + " jumpAway=" + b(j2);
        tally.gate(9, okI && okII && okIII && okIV && okV, d);
    }

    // ---- G10 (US-2106): a sequence switch outside restore is unsaved; a restore-driven one is not ----
    {
        TimelineSequence seqA;
        seqA.id = QStringLiteral("seq-a");
        seqA.name = QStringLiteral("A");
        seqA.videoTracks = {{makeClip(tmp("g10-a.mp4"))}};
        seqA.audioTracks = {{}};
        TimelineSequence seqB = seqA;
        seqB.id = QStringLiteral("seq-b");
        seqB.name = QStringLiteral("B");
        seqB.videoTracks = {{makeClip(tmp("g10-b.mp4"))}};
        timeline.setSequences({seqA, seqB}, seqA.id);
        undo->clear();
        timeline.saveUndoState(QStringLiteral("Initial"));
        window.markProjectClean();
        const bool clean0 = !window.isWindowModified();
        // (a) the dock's direct call.
        const bool toB = timeline.setActiveSequence(seqB.id);
        const bool onB = timeline.activeSequenceId() == seqB.id;
        const bool switchDirty = window.isWindowModified();
        const bool okA = clean0 && toB && onB && switchDirty;
        std::string d = "(a) clean0=" + b(clean0) + " switched=" + b(toB) + " onB=" + b(onB)
            + " modified=" + b(switchDirty) + " (expect 1)";
        // (b) E1 on A, switch, E2 on B, clean at E2: undo lands on A, redo on B, both restore-driven.
        const bool toA = timeline.setActiveSequence(seqA.id);
        timeline.saveUndoState(QStringLiteral("E1"));
        const bool toB2 = timeline.setActiveSequence(seqB.id);
        timeline.saveUndoState(QStringLiteral("E2"));
        window.markProjectClean();
        timeline.undo();
        const bool undoOnA = timeline.activeSequenceId() == seqA.id;
        const bool undoDirty = window.isWindowModified();
        timeline.redo();
        const bool redoOnB = timeline.activeSequenceId() == seqB.id;
        const bool redoDirty = window.isWindowModified();
        const bool okB = toA && toB2 && undoOnA && undoDirty && redoOnB && !redoDirty && undo->isClean();
        d += " (b) toA=" + b(toA) + " toB=" + b(toB2) + " undoOnA=" + b(undoOnA) + " undoModified=" + b(undoDirty)
            + " redoOnB=" + b(redoOnB) + " redoModified=" + b(redoDirty) + " (expect 0) isClean=" + b(undo->isClean());
        tally.gate(10, okA && okB, d);
    }

    // ---- G11 (US-2106): a flag undo point across a track-structure change ----
    {
        timeline.undoManager()->clear();
        timeline.saveUndoState(QStringLiteral("Initial"));
        window.markProjectClean();
        TimelineTrack *a1 = timeline.audioTrackCount() > 0 ? timeline.audioTracks()[0] : nullptr;
        const bool premise = a1 && !a1->isMuted();
        const int aBase = timeline.audioTrackCount();
        if (a1) timeline.toggleMuteTrack(0);
        const bool muted = a1 && a1->isMuted();
        timeline.addAudioTrack(true);
        const int aAdded = timeline.audioTrackCount();
        const auto a1Muted = [&] {
            return timeline.audioTrackCount() > 0 && timeline.audioTracks()[0]->isMuted();
        };
        timeline.undo();
        const int aUndo1 = timeline.audioTrackCount();
        const bool mutedUndo1 = a1Muted();
        timeline.undo();
        const bool mutedUndo2 = a1Muted();
        timeline.redo();
        timeline.redo();
        const int aRedo = timeline.audioTrackCount();
        const bool mutedRedo = a1Muted();
        const bool ok = premise && muted && aAdded == aBase + 1 && aUndo1 == aBase && mutedUndo1
            && !mutedUndo2 && aRedo == aAdded && mutedRedo;
        tally.gate(11, ok, "premise=" + b(premise) + " muted=" + b(muted) + " tracks " + std::to_string(aBase)
            + "->" + std::to_string(aAdded) + " undo1 tracks=" + std::to_string(aUndo1) + " muted=" + b(mutedUndo1)
            + " undo2 muted=" + b(mutedUndo2) + " (expect 0) redo2 tracks=" + std::to_string(aRedo)
            + " muted=" + b(mutedRedo));
    }

    // ---- G12 (US-2108): a persisted setting that reached the file with neither an undo point nor an
    // explicit dirty is now unsaved; an undo-point row also undoes to clean and redoes its change ----
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        MainWindow w12;
        Timeline &tl = *w12.m_timeline;
        UndoManager *um = tl.undoManager();
        const QString clipPath = tmp("g12-clip.mp4");
        const QString stillPath = tmp("g12-still.png");
        const QString camPath = tmp("g12-cam.mp4");
        const bool filesOk = touchFile(clipPath) && touchFile(camPath)
            && ([&] { QImage img(64, 64, QImage::Format_RGB32); img.fill(Qt::gray); return img.save(stillPath); })();
        const auto v1Clip = [&]() -> ClipInfo {
            const auto &clips = tl.videoClips();
            return clips.isEmpty() ? ClipInfo{} : clips.first();
        };
        const auto setup = [&](const QString &path) {
            tl.videoTracks()[0]->setClips({makeClip(path)});
            tl.videoTracks()[0]->setSelectedClip(0);
            um->clear();
            tl.saveUndoState(QStringLiteral("Initial"));
            w12.markProjectClean();
            effectctrl::EffectClipboard::instance().capture({}, effectctrl::ClipMotion{});
        };
        // `changed` is true on the live state after the edit; `undone` after one undo; `redone` after the redo.
        struct Leg {
            const char *name;
            QString clip;
            std::function<void()> act;
            std::function<bool()> changed;
            std::function<bool()> undone;
            std::function<bool()> redone;
            std::function<void(QWidget *)> prep;
            const char *action = "";   // popup-menu action the driver triggers (default: the first)
            bool timed = false;        // the edit takes its undo point from a single-shot timer
            bool driven = false;       // a dialog or popup menu opens: the driver must have answered it
        };
        VideoEffect blur;
        blur.type = VideoEffectType::Blur;
        const auto clipboardMotion = [] {
            effectctrl::ClipMotion m;
            m.scale = 1.5;
            m.dx = 0.2;
            return m;
        };
        const auto scaleIs = [&](double v) { return std::abs(v1Clip().videoScale - v) < 1e-9; };
        const auto eqMixer = [&] { return w12.m_player->audioMixer(); };
        // Audio > Track EQ > A1: the submenu is rebuilt on aboutToShow; trigger the named action
        // (empty = the first preset).
        const auto triggerTrackEq = [&](const QString &text) {
            auto *menu = w12.findChild<QMenu *>(QStringLiteral("trackEqMenu"));
            if (!menu) return;
            Q_EMIT menu->aboutToShow();
            for (QAction *track : menu->actions()) {
                if (!track->menu() || track->menu()->actions().isEmpty()) continue;
                QAction *target = track->menu()->actions().first();
                for (QAction *candidate : track->menu()->actions())
                    if (!text.isEmpty() && candidate->text() == text) target = candidate;
                target->trigger();
                return;
            }
        };
        const auto enableEqClean = [&] {
            eqMixer()->setTrackEqEnabled(0, true);
            w12.markProjectClean();
        };
        MultiCamProject cam;
        cam.angles = {MultiCamAngle{1, camPath, 0, QStringLiteral("A")}};
        cam.switches = {MultiCamSwitch{0, 1}};
        const QVector<Leg> legs = {
            {"dragTransform", clipPath,
             [&] { Q_EMIT w12.m_player->videoSourceTransformChanged(0, 0, 1.5, 0.2, 0.1); },
             [&] { return scaleIs(1.5); }, [&] { return scaleIs(1.0); }, [&] { return scaleIs(1.5); },
             nullptr, "", true},
            {"effectControlsMotion", clipPath,
             [&] {
                 Q_EMIT tl.clipSelectedOnTrack(0, 0);
                 auto *motion = w12.m_effectControlsPanel->findChild<effectctrl::MotionSectionWidget *>();
                 if (!motion) return;
                 effectctrl::MotionState state = motion->currentMotion();
                 state.scale = 1.5;
                 Q_EMIT motion->motionChanged(state);
             },
             [&] { return scaleIs(1.5); }, [&] { return scaleIs(1.0); }, [&] { return scaleIs(1.5); },
             nullptr, "", true},
            {"pasteEffects", clipPath,
             [&] {
                 effectctrl::EffectClipboard::instance().capture({blur}, clipboardMotion());
                 w12.pasteEffects();
             },
             [&] { return scaleIs(1.5) && v1Clip().effects.size() == 1; },
             [&] { return scaleIs(1.0) && v1Clip().effects.isEmpty(); },
             [&] { return scaleIs(1.5) && v1Clip().effects.size() == 1; }, nullptr},
            {"pasteAttributes", clipPath,
             [&] {
                 effectctrl::EffectClipboard::instance().capture({blur}, clipboardMotion());
                 w12.pasteAttributes();
             },
             // Scale only: the effects checkbox is cleared, so no effects setter records the point.
             [&] { return scaleIs(1.5) && v1Clip().effects.isEmpty(); }, [&] { return scaleIs(1.0); },
             [&] { return scaleIs(1.5) && v1Clip().effects.isEmpty(); },
             [](QWidget *dialog) {
                 for (auto *check : dialog->findChildren<QCheckBox *>())
                     check->setChecked(check->text() == QStringLiteral("Scale"));
             }},
            {"speedAtempo", clipPath,
             [&] { w12.openSpeedRampDialog(); },
             [&] { return v1Clip().atempoEnabled; }, [&] { return !v1Clip().atempoEnabled; },
             [&] { return v1Clip().atempoEnabled; },
             [](QWidget *dialog) {
                 for (auto *check : dialog->findChildren<QCheckBox *>()) check->setChecked(true);
             }},
            {"stabilizer", stillPath,
             [&] { w12.runMotionStabilizer(); },
             [&] { return !v1Clip().stabilizerKeyframes.isEmpty(); },
             [&] { return v1Clip().stabilizerKeyframes.isEmpty(); },
             [&] { return !v1Clip().stabilizerKeyframes.isEmpty(); }, nullptr},
            {"multicamEdl", clipPath,
             [&] { w12.onMultiCamApplyToTimeline(cam); },
             [&] { return v1Clip().filePath == camPath; }, [&] { return v1Clip().filePath == clipPath; },
             [&] { return v1Clip().filePath == camPath; }, nullptr},
            {"eqAudioMenuPreset", clipPath, [&] { triggerTrackEq({}); },
             [&] { return eqMixer()->trackEqEnabled(0); }, nullptr, nullptr, nullptr},
            {"eqAudioMenuBypass", clipPath, [&] { enableEqClean(); triggerTrackEq(QStringLiteral("Bypass / Reset")); },
             [&] { return !eqMixer()->trackEqEnabled(0); }, nullptr, nullptr, nullptr},
            {"eqMeterMenuPreset", clipPath, [&] { w12.onMeterRequestEqPresetMenu(0, QPoint(0, 0)); },
             [&] { return eqMixer()->trackEqEnabled(0); }, nullptr, nullptr, nullptr, "", false, true},
            {"eqMeterMenuBypass", clipPath,
             [&] { enableEqClean(); w12.onMeterRequestEqPresetMenu(0, QPoint(0, 0)); },
             [&] { return !eqMixer()->trackEqEnabled(0); }, nullptr, nullptr, nullptr, "Bypass / Reset", false, true},
            {"masterCompressor", clipPath, [&] { w12.openMasterCompressor(); }, nullptr, nullptr, nullptr, nullptr, "", false, true},
            {"autoDuckDialog", clipPath, [&] { w12.onMeterRequestAutoDuckDialog(); }, nullptr, nullptr, nullptr, nullptr, "", false, true},
            {"loudnessSettings", clipPath, [&] { w12.openLoudnessSettings(); }, nullptr, nullptr, nullptr, nullptr, "", false, true},
        };
        bool ok = filesOk;
        std::string d = "files=" + b(filesOk);
        for (const Leg &leg : legs) {
            setup(leg.clip);
            const bool clean0 = !w12.isWindowModified();
            // openLoudnessSettings writes the real audio/normalizer* preferences: put them back.
            QSettings prefs(QStringLiteral("VSimpleEditor"), QStringLiteral("Preferences"));
            const QStringList prefKeys = {QStringLiteral("audio/normalizerAmount"),
                                          QStringLiteral("audio/normalizerUniformity")};
            QHash<QString, QVariant> savedPrefs;
            for (const QString &key : prefKeys)
                if (prefs.contains(key)) savedPrefs.insert(key, prefs.value(key));
            DialogDriver driver(&w12, leg.prep, QString::fromLatin1(leg.action));
            leg.act();
            QCoreApplication::processEvents();
            if (leg.timed) QTest::qWait(500); // past the 300 ms undo-point timer
            driver.timer.stop();
            for (const QString &key : prefKeys) {
                if (savedPrefs.contains(key)) prefs.setValue(key, savedPrefs.value(key));
                else prefs.remove(key);
            }
            prefs.sync();
            const bool dirty = w12.isWindowModified();
            const bool changed = !leg.changed || leg.changed();
            const bool answered = !leg.driven || driver.answered >= 1;
            bool legOk = clean0 && dirty && changed && answered;
            std::string detail = std::string(" | ") + leg.name + " clean0=" + b(clean0) + " dirty=" + b(dirty)
                + " changed=" + b(changed) + " answered=" + b(answered) + " dialogs=" + std::to_string(driver.answered);
            if (leg.undone) {
                // An undo-point row: not clean, undo lands back on the clean entry with the change gone,
                // redo brings the whole change back (a point taken before part of the edit loses it here).
                const bool notClean = !um->isClean();
                tl.undo();
                const bool undone = leg.undone();
                const bool afterUndo = w12.isWindowModified();
                tl.redo();
                const bool redone = leg.redone();
                legOk = legOk && notClean && undone && !afterUndo && redone;
                detail += " notClean=" + b(notClean) + " undone=" + b(undone) + " afterUndo=" + b(afterUndo)
                    + " (expect 0) redone=" + b(redone);
            }
            d += detail;
            ok = ok && legOk;
        }
        // The two timer-backed edits (legs 0 and 1: dragTransform, effectControlsMotion) with no wait:
        // the debounce is still pending when the next step runs.
        for (size_t i = 0; i < 2; ++i) {
            const Leg &leg = legs[i];
            // (a) a prompt-guarded action right after the edit sees an unsaved project and prompts once.
            setup(leg.clip);
            int prompts = 0;
            w12.setUnsavedChangesPromptForTest([&] { ++prompts; return MainWindow::UnsavedChangesChoice::Discard; });
            const int count0 = w12.unsavedChangesPromptCountForTest();
            leg.act();
            QCoreApplication::processEvents();
            const bool guarded = w12.maybeSaveChanges();
            const bool counted = w12.unsavedChangesPromptCountForTest() == count0 + 1 && prompts == 1;
            const bool modified = w12.isWindowModified();
            const int indexAfter = um->currentIndex();
            QTest::qWait(500);
            const bool single = um->currentIndex() == indexAfter; // the flush stopped the timer: no second point
            w12.setUnsavedChangesPromptForTest({});
            // (i) undo before the timer fires: the flush takes the point, undo goes back, redo restores the edit.
            setup(leg.clip);
            leg.act();
            QCoreApplication::processEvents();
            w12.undoAction();
            QTest::qWait(500);
            const bool undoneNow = leg.undone();
            const bool redoKept = um->canRedo();
            w12.redoAction();
            const bool redoRestores = leg.redone();
            // (iv) an undo that bypasses MainWindow (MCP calls Timeline::undo): the pending point is stale and dropped,
            // so the redo entry survives the timer.
            setup(leg.clip);
            tl.saveUndoState(QStringLiteral("pre"));
            leg.act();
            QCoreApplication::processEvents();
            tl.undo();
            QTest::qWait(500);
            const bool bypassRedo = um->canRedo() && um->currentIndex() == 0;
            // (iv-b) an edit, a bypass undo, then a second edit inside the same debounce: the burst is re-based,
            // so the second edit takes its point, stays unsaved and can be undone (motion leg only).
            bool rebaseOk = true;
            std::string rebaseDetail;
            if (i == 1) {
                setup(leg.clip);
                tl.saveUndoState(QStringLiteral("pre"));
                w12.markProjectClean();
                leg.act();
                QCoreApplication::processEvents();
                tl.undo();
                Q_EMIT tl.clipSelectedOnTrack(0, 0);
                auto *motion = w12.m_effectControlsPanel->findChild<effectctrl::MotionSectionWidget *>();
                if (motion) {
                    effectctrl::MotionState state = motion->currentMotion();
                    state.scale = 1.75;
                    Q_EMIT motion->motionChanged(state);
                }
                QCoreApplication::processEvents();
                QTest::qWait(500);
                const bool rbModified = w12.isWindowModified();
                const bool rbNotClean = !um->isClean();
                const bool rbValue = scaleIs(1.75);
                tl.undo();
                const bool rbUndone = !scaleIs(1.75);
                rebaseOk = rbModified && rbNotClean && rbValue && rbUndone;
                rebaseDetail = " rebaseAfterBypassUndo: modified=" + b(rbModified) + " notClean=" + b(rbNotClean)
                    + " value=" + b(rbValue) + " undone=" + b(rbUndone);
            }
            // (ii) save before the timer fires: the saved file has the edit and the project stays clean.
            setup(leg.clip);
            leg.act();
            QCoreApplication::processEvents();
            const QString savePath = tmp(i == 0 ? "g12-save-drag.veditor" : "g12-save-motion.veditor");
            const bool saved = w12.saveProjectToPath(savePath, nullptr);
            QTest::qWait(500);
            QFile saveFile(savePath);
            const bool fileHasEdit = saveFile.open(QIODevice::ReadOnly)
                && QString::fromUtf8(saveFile.readAll()).contains(
                    QRegularExpression(QStringLiteral("\"videoScale\"\\s*:\\s*1\\.5")));
            const bool cleanAfterSave = !w12.isWindowModified();
            // (iii) a new project before the timer fires: the old project's point must not land on it.
            setup(leg.clip);
            leg.act();
            QCoreApplication::processEvents();
            w12.setUnsavedChangesPromptForTest([] { return MainWindow::UnsavedChangesChoice::Discard; });
            DialogDriver newDriver(&w12);
            w12.newProject();
            newDriver.timer.stop();
            QTest::qWait(500);
            w12.setUnsavedChangesPromptForTest({});
            const bool resetClean = !w12.isWindowModified();
            const bool resetNoUndo = !um->canUndo();
            const bool legOk = guarded && counted && modified && single && undoneNow && redoKept && redoRestores && bypassRedo
                && rebaseOk && saved && fileHasEdit && cleanAfterSave && newDriver.answered >= 1 && resetClean && resetNoUndo;
            ok = ok && legOk;
            d += std::string(" | ") + leg.name + "-nowait guarded=" + b(guarded) + " prompts+1=" + b(counted)
                + " modified=" + b(modified) + " single=" + b(single) + " undoBeforeTimer: undone=" + b(undoneNow)
                + " canRedo=" + b(redoKept) + " redo=" + b(redoRestores) + " mcpUndoKeepsRedo=" + b(bypassRedo) + rebaseDetail
                + " saveBeforeTimer: saved=" + b(saved)
                + " fileHasEdit=" + b(fileHasEdit) + " clean=" + b(cleanAfterSave) + " newBeforeTimer: dialogs="
                + std::to_string(newDriver.answered) + " clean=" + b(resetClean) + " canUndo=" + b(!resetNoUndo)
                + " (expect 0)";
        }
        // Negative sub-leg of dragTransform: a second emission with the values already stored moves
        // nothing, so it must leave the project clean and push no undo point.
        {
            setup(clipPath);
            Q_EMIT w12.m_player->videoSourceTransformChanged(0, 0, 1.5, 0.2, 0.1);
            QCoreApplication::processEvents();
            QTest::qWait(500);
            w12.markProjectClean();
            const int idx0 = um->currentIndex();
            Q_EMIT w12.m_player->videoSourceTransformChanged(0, 0, 1.5, 0.2, 0.1);
            QCoreApplication::processEvents();
            QTest::qWait(500);
            const bool stayedClean = !w12.isWindowModified() && um->isClean() && um->currentIndex() == idx0;
            ok = ok && stayedClean;
            d += std::string(" | dragTransform-zeroMove stayedClean=") + b(stayedClean);
        }
        tally.gate(12, ok, d);
    }

    window.setUnsavedChangesPromptForTest({});
    window.setSaveFailureDialogSuppressedForTest(false);
    return tally.finish("unsaved-prompt");
}
