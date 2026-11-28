#include "SelftestRegistry.h"
#include "../ProjectFile.h"
#include "../SequenceListDock.h"
#include "../SequenceSettingsDialog.h"
#include "../Timeline.h"
#include "../UndoManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QListWidget>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>

int runSequenceSettingsSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](const char *gate, bool ok, const QString &detail) {
        if (ok) {
            ++passed;
            std::cout << "PASS " << gate << " " << detail.toStdString() << "\n";
        } else {
            ++failed;
            std::cerr << "FAIL " << gate << " " << detail.toStdString() << "\n";
        }
    };

    // G1: Default sequence inherits project config (hasCustomSettings == false).
    {
        Timeline tl;
        tl.setProjectOutputConfig(1920, 1080, true, 30.0);
        int w = 0, h = 0;
        double fps = 0.0;
        tl.activeSequenceEffectiveSettings(w, h, fps);
        const auto list = tl.sequenceList();
        const bool ok = !list.isEmpty() && !list.first().hasCustomSettings()
            && w == 1920 && h == 1080 && std::abs(fps - 30.0) < 1e-6;
        check("G1", ok, QStringLiteral("inherit=%1x%2@%3 custom=%4")
                            .arg(w).arg(h).arg(fps).arg(!list.isEmpty() && list.first().hasCustomSettings()));
    }

    // G2: setSequenceSettings sets per-sequence W/H/FPS independently; odd dims rounded to even,
    // out-of-range clamped; main stays inherited while seq2 gets 1080x1920 @ 60fps.
    {
        Timeline tl;
        tl.setProjectOutputConfig(1920, 1080, true, 30.0);
        tl.createSequence(QStringLiteral("縦型ショート"));
        const QString seq2Id = tl.activeSequenceId();
        QSignalSpy spy(&tl, &Timeline::sequenceSettingsChanged);
        const auto serialBefore = tl.undoManager()->saveSerial();
        const bool setOk = tl.setSequenceSettings(seq2Id, 1081, 1921, 60.0); // odd -> 1080x1920
        int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        double fps1 = 0.0, fps2 = 0.0;
        tl.effectiveSequenceSettings(QStringLiteral("main"), w1, h1, fps1);
        tl.effectiveSequenceSettings(seq2Id, w2, h2, fps2);
        const bool ok = setOk && spy.count() == 1
            && tl.undoManager()->saveSerial() == serialBefore + 1
            && w1 == 1920 && h1 == 1080 && std::abs(fps1 - 30.0) < 1e-6
            && w2 == 1080 && h2 == 1920 && std::abs(fps2 - 60.0) < 1e-6;
        check("G2", ok, QStringLiteral("main=%1x%2@%3 seq2=%4x%5@%6 spy=%7")
                            .arg(w1).arg(h1).arg(fps1).arg(w2).arg(h2).arg(fps2).arg(spy.count()));
    }

    // G3: createSequenceWithSettings creates 4K 24fps sequence in 1 Undo step; Undo & Redo work.
    {
        Timeline tl;
        tl.setProjectOutputConfig(1920, 1080, true, 30.0);
        const auto serial0 = tl.undoManager()->saveSerial();
        const bool created = tl.createSequenceWithSettings(QStringLiteral("4K Cinema"), 3840, 2160, 24.0);
        const QString seqId = tl.activeSequenceId();
        int w = 0, h = 0;
        double fps = 0.0;
        tl.activeSequenceEffectiveSettings(w, h, fps);
        const bool afterCreate = created && tl.undoManager()->saveSerial() == serial0 + 1
            && w == 3840 && h == 2160 && std::abs(fps - 24.0) < 1e-6;

        tl.undo();
        int wUndo = 0, hUndo = 0;
        double fpsUndo = 0.0;
        tl.activeSequenceEffectiveSettings(wUndo, hUndo, fpsUndo);
        const bool afterUndo = tl.sequenceList().size() == 1
            && wUndo == 1920 && hUndo == 1080 && std::abs(fpsUndo - 30.0) < 1e-6;

        tl.redo();
        int wRedo = 0, hRedo = 0;
        double fpsRedo = 0.0;
        tl.activeSequenceEffectiveSettings(wRedo, hRedo, fpsRedo);
        const bool afterRedo = tl.sequenceList().size() == 2 && tl.activeSequenceId() == seqId
            && wRedo == 3840 && hRedo == 2160 && std::abs(fpsRedo - 24.0) < 1e-6;

        check("G3", afterCreate && afterUndo && afterRedo,
              QStringLiteral("create=%1 undo=%2 redo=%3").arg(afterCreate).arg(afterUndo).arg(afterRedo));
    }

    // G4: Switching active sequence updates activeSequenceEffectiveSettings cleanly across 3 sequences.
    {
        Timeline tl;
        tl.setProjectOutputConfig(1920, 1080, true, 30.0);
        tl.createSequenceWithSettings(QStringLiteral("Vertical"), 1080, 1920, 60.0);
        const QString vertId = tl.activeSequenceId();
        tl.createSequenceWithSettings(QStringLiteral("Square"), 1080, 1080, 25.0);
        const QString sqId = tl.activeSequenceId();

        int w = 0, h = 0;
        double fps = 0.0;
        tl.setActiveSequence(QStringLiteral("main"));
        tl.activeSequenceEffectiveSettings(w, h, fps);
        const bool mainOk = (w == 1920 && h == 1080 && std::abs(fps - 30.0) < 1e-6);

        tl.setActiveSequence(vertId);
        tl.activeSequenceEffectiveSettings(w, h, fps);
        const bool vertOk = (w == 1080 && h == 1920 && std::abs(fps - 60.0) < 1e-6);

        tl.setActiveSequence(sqId);
        tl.activeSequenceEffectiveSettings(w, h, fps);
        const bool sqOk = (w == 1080 && h == 1080 && std::abs(fps - 25.0) < 1e-6);

        check("G4", mainOk && vertOk && sqOk,
              QStringLiteral("main=%1 vert=%2 sq=%3").arg(mainOk).arg(vertOk).arg(sqOk));
    }

    // G5: Resetting a sequence to project default (0,0,0) restores inheritance; Undo restores custom.
    {
        Timeline tl;
        tl.setProjectOutputConfig(1920, 1080, true, 30.0);
        tl.createSequenceWithSettings(QStringLiteral("Custom"), 1280, 720, 50.0);
        const QString id = tl.activeSequenceId();
        const bool cleared = tl.setSequenceSettings(id, 0, 0, 0.0);
        int w = 0, h = 0;
        double fps = 0.0;
        tl.effectiveSequenceSettings(id, w, h, fps);
        const bool inherited = cleared && w == 1920 && h == 1080 && std::abs(fps - 30.0) < 1e-6;

        tl.undo();
        tl.effectiveSequenceSettings(id, w, h, fps);
        const bool undoRestored = (w == 1280 && h == 720 && std::abs(fps - 50.0) < 1e-6);
        check("G5", inherited && undoRestored,
              QStringLiteral("inherited=%1 undoRestored=%2").arg(inherited).arg(undoRestored));
    }

    // G6: ProjectFile JSON round-trip preserves per-sequence width/height/fps & stays sparse when unset.
    {
        Timeline tl;
        tl.setProjectOutputConfig(1920, 1080, true, 30.0);
        tl.createSequenceWithSettings(QStringLiteral("Short"), 1080, 1920, 60.0);
        const QString shortId = tl.activeSequenceId();

        ProjectData data;
        data.config.width = 1920;
        data.config.height = 1080;
        data.config.fps = 30;
        data.videoTracks = tl.allVideoTracks();
        data.audioTracks = tl.allAudioTracks();
        const auto entries = tl.clipParentEntries();
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            ClipParentEntry e;
            e.clipId = it.key();
            e.parentClipId = it.value();
            data.clipParentEntries.append(e);
        }

        const QString jsonStr = ProjectFile::toJsonString(data);
        const QJsonObject root = QJsonDocument::fromJson(jsonStr.toUtf8()).object();
        const QJsonArray seqArr = root.value(QStringLiteral("sequences")).toArray();
        bool mainSparse = false;
        bool shortHasFields = false;
        for (const QJsonValue &v : seqArr) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("id")).toString() == QStringLiteral("main"))
                mainSparse = !o.contains(QStringLiteral("width")) && !o.contains(QStringLiteral("fps"));
            if (o.value(QStringLiteral("id")).toString() == shortId)
                shortHasFields = (o.value(QStringLiteral("width")).toInt() == 1080
                                  && o.value(QStringLiteral("height")).toInt() == 1920
                                  && std::abs(o.value(QStringLiteral("fps")).toDouble() - 60.0) < 1e-6);
        }

        ProjectData loaded;
        const bool loadOk = ProjectFile::fromJsonString(jsonStr, loaded);
        Timeline restored;
        restored.setProjectOutputConfig(loaded.config.width, loaded.config.height, true, loaded.config.fps);
        restored.restoreFromProject(loaded.videoTracks, loaded.audioTracks, 0.0, -1.0, -1.0, 100);
        QHash<QString, QString> loadedMap;
        for (const auto &e : loaded.clipParentEntries)
            loadedMap.insert(e.clipId, e.parentClipId);
        restored.setClipParentEntries(loadedMap);

        int w = 0, h = 0;
        double fps = 0.0;
        restored.effectiveSequenceSettings(shortId, w, h, fps);
        const bool rtOk = loadOk && w == 1080 && h == 1920 && std::abs(fps - 60.0) < 1e-6;
        check("G6", mainSparse && shortHasFields && rtOk,
              QStringLiteral("mainSparse=%1 shortFields=%2 rt=%3").arg(mainSparse).arg(shortHasFields).arg(rtOk));
    }

    // G7: SequenceListDock displays custom resolution/FPS badge while keeping default item text untouched.
    {
        Timeline tl;
        tl.setProjectOutputConfig(1920, 1080, true, 30.0);
        SequenceListDock dock(&tl);
        auto *list = dock.findChild<QListWidget *>(QStringLiteral("sequenceList"));
        tl.createSequenceWithSettings(QStringLiteral("Reels"), 1080, 1920, 60.0);
        const bool dockOk = list && list->count() == 2
            && list->item(0)->text() == QStringLiteral("メインシーケンス")
            && list->item(1)->text().contains(QStringLiteral("1080x1920"))
            && list->item(1)->text().contains(QStringLiteral("60fps"));

        ProjectConfig init;
        init.name = QStringLiteral("Reels");
        init.width = 1080;
        init.height = 1920;
        init.fps = 60;
        SequenceSettingsDialog dlg(init);
        const bool dlgOk = (dlg.config().width == 1080 && dlg.config().height == 1920 && dlg.config().fps == 60);
        check("G7", dockOk && dlgOk, QStringLiteral("dock=%1 dlg=%2").arg(dockOk).arg(dlgOk));
    }

    const int declared = 7;
    std::cout << "sequence-settings summary: " << passed << " PASS, " << failed << " FAIL, 0 SKIP\n";
    std::cout << "gate-count: declared " << declared << ", ran " << (passed + failed) << "\n";
    return failed == 0 ? 0 : 1;
}
