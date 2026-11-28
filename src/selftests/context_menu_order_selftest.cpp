#include "../ContextMenuOrder.h"
#include "../GLPreview.h"
#include "../Timeline.h"
#include "SelftestGate.h"

#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstdio>

namespace {

bool writeMinimalWavFile(const QString &path, double durationSec, int sampleRate = 48000)
{
    const int numSamples = qMax(1, static_cast<int>(std::round(durationSec * sampleRate)));
    const int channels = 1;
    const int bitsPerSample = 16;
    const int dataBytes = numSamples * channels * (bitsPerSample / 8);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;

    auto writeU32 = [&](quint32 v) {
        char b[4];
        qToLittleEndian(v, reinterpret_cast<uchar *>(b));
        f.write(b, 4);
    };
    auto writeU16 = [&](quint16 v) {
        char b[2];
        qToLittleEndian(v, reinterpret_cast<uchar *>(b));
        f.write(b, 2);
    };

    f.write("RIFF", 4);
    writeU32(static_cast<quint32>(36 + dataBytes));
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    writeU32(16);
    writeU16(1); // PCM
    writeU16(static_cast<quint16>(channels));
    writeU32(static_cast<quint32>(sampleRate));
    writeU32(static_cast<quint32>(sampleRate * channels * (bitsPerSample / 8)));
    writeU16(static_cast<quint16>(channels * (bitsPerSample / 8)));
    writeU16(static_cast<quint16>(bitsPerSample));
    f.write("data", 4);
    writeU32(static_cast<quint32>(dataBytes));

    QByteArray pcm(dataBytes, '\0');
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < numSamples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double v = 0.25 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t);
        samples[i] = static_cast<qint16>(v * 32767.0);
    }
    f.write(pcm);
    f.close();
    return true;
}

} // namespace

int runContextMenuOrderSelftest()
{
    selftest::GateTally tally;
    auto rec = [&](int n, bool ok, const QString &detail) {
        tally.gate(n, ok, detail.toStdString());
    };
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        std::fprintf(stderr, "context-menu-order: failed to create temp dir\n");
        return 1;
    }

    // G1: Default action catalogs for all 4 targets have unique non-empty IDs and labels.
    {
        bool ok = true;
        int totalDefs = 0;
        for (ctxmenu::Target t : ctxmenu::allTargets()) {
            const auto defs = ctxmenu::defaultActionsFor(t);
            if (defs.isEmpty() || ctxmenu::targetKey(t).isEmpty() || ctxmenu::targetDisplayName(t).isEmpty())
                ok = false;
            QSet<QString> seen;
            for (const auto &d : defs) {
                if (d.id.isEmpty() || d.label.isEmpty() || seen.contains(d.id))
                    ok = false;
                seen.insert(d.id);
            }
            totalDefs += defs.size();
        }
        rec(1, ok, QStringLiteral("targets=%1 totalDefs=%2")
                       .arg(ctxmenu::allTargets().size()).arg(totalDefs));
    }

    // G2: resolveOrder handles custom order, deduplication, unknown IDs, new IDs appended,
    // hidden preservation, and all-hidden safety fallback.
    {
        const auto defs = ctxmenu::defaultActionsFor(ctxmenu::Target::PreviewText);
        const QString firstId = defs[0].id;
        const QString thirdId = defs[2].id;
        const QString lastId = defs.last().id;

        const QStringList savedOrder = {
            QStringLiteral("unknown.legacy_id"),
            lastId,
            thirdId,
            lastId, // duplicate
        };
        const QSet<QString> savedHidden = { firstId, QStringLiteral("unknown.hidden") };
        const auto resolved = ctxmenu::resolveOrder(defs, savedOrder, savedHidden);

        bool ok = (resolved.size() == defs.size())
            && (resolved[0].id == lastId)
            && (resolved[1].id == thirdId)
            && (resolved[2].id == firstId)
            && (!resolved[2].visible)
            && resolved[0].visible;

        // Safety fallback: hiding every single item restores all visible.
        QSet<QString> allHidden;
        for (const auto &d : defs)
            allHidden.insert(d.id);
        const auto allHiddenResolved = ctxmenu::resolveOrder(defs, {}, allHidden);
        bool allVisibleFallback = (allHiddenResolved.size() == defs.size());
        for (const auto &it : allHiddenResolved) {
            if (!it.visible)
                allVisibleFallback = false;
        }
        ok = ok && allVisibleFallback;
        rec(2, ok, QStringLiteral("first=%1 second=%2 fallback=%3")
                       .arg(resolved[0].id, resolved[1].id)
                       .arg(allVisibleFallback ? 1 : 0));
    }

    // G3: QSettings round-trip (saveLayout -> loadLayout -> resetLayout) isolated in temp INI.
    {
        const QString iniPath = tmpDir.filePath(QStringLiteral("prefs.ini"));
        QSettings settings(iniPath, QSettings::IniFormat);
        const auto defs = ctxmenu::defaultActionsFor(ctxmenu::Target::TimelineAudioClip);
        QStringList customOrder;
        customOrder << defs[5].id << defs[0].id; // bring audio.normalize to top!
        QSet<QString> hidden = { defs[1].id };
        ctxmenu::saveLayout(ctxmenu::Target::TimelineAudioClip, customOrder, hidden, &settings);
        settings.sync();

        const auto loaded = ctxmenu::loadLayout(ctxmenu::Target::TimelineAudioClip, &settings);
        bool ok = (loaded.size() == defs.size())
            && (loaded[0].id == defs[5].id)
            && (loaded[1].id == defs[0].id);
        bool hiddenOk = false;
        for (const auto &it : loaded) {
            if (it.id == defs[1].id)
                hiddenOk = !it.visible;
        }
        ok = ok && hiddenOk;

        ctxmenu::resetLayout(ctxmenu::Target::TimelineAudioClip, &settings);
        const auto resetLoaded = ctxmenu::loadLayout(ctxmenu::Target::TimelineAudioClip, &settings);
        ok = ok && (resetLoaded.size() == defs.size())
            && (resetLoaded[0].id == defs[0].id)
            && resetLoaded[1].visible;
        rec(3, ok, QStringLiteral("customTop=%1 resetTop=%2")
                       .arg(loaded[0].id, resetLoaded[0].id));
    }

    // G4: ContextMenuCustomizeDialog moves rows, toggles visibility, switches targets, resets & commits.
    {
        const QString iniPath = tmpDir.filePath(QStringLiteral("dialog_prefs.ini"));
        QSettings settings(iniPath, QSettings::IniFormat);

        ctxmenu::ContextMenuCustomizeDialog dlg(ctxmenu::Target::PreviewVideoClip, nullptr, &settings);
        const auto initialIds = dlg.currentOrderIds();
        // Select row 2 and move it up twice to index 0
        auto *list = dlg.findChild<QListWidget *>(QStringLiteral("ctxMenuItemList"));
        auto *combo = dlg.findChild<QComboBox *>(QStringLiteral("ctxMenuTargetCombo"));
        bool ok = (list != nullptr && combo != nullptr && initialIds.size() >= 4);
        if (ok) {
            list->setCurrentRow(2);
            dlg.moveSelectedRow(-1);
            dlg.moveSelectedRow(-1);
            list->item(3)->setCheckState(Qt::Unchecked);
            const auto movedIds = dlg.currentOrderIds();
            ok = ok && (movedIds[0] == initialIds[2]);

            // Switch to PreviewText, move row 1 to 0, switch back and commit
            const int textIdx = combo->findData(static_cast<int>(ctxmenu::Target::PreviewText));
            combo->setCurrentIndex(textIdx);
            const auto textInit = dlg.currentOrderIds();
            list->setCurrentRow(1);
            dlg.moveSelectedRow(-1);
            dlg.commitDrafts();
            settings.sync();

            const auto savedVideo = ctxmenu::loadLayout(ctxmenu::Target::PreviewVideoClip, &settings);
            const auto savedText = ctxmenu::loadLayout(ctxmenu::Target::PreviewText, &settings);
            ok = ok && (savedVideo[0].id == initialIds[2]) && (!savedVideo[3].visible)
                && (savedText[0].id == textInit[1]);

            // Reset current target (PreviewText) and commit
            dlg.resetCurrentTargetToDefault();
            dlg.commitDrafts();
            settings.sync();
            const auto resetText = ctxmenu::loadLayout(ctxmenu::Target::PreviewText, &settings);
            ok = ok && (resetText[0].id == textInit[0]);
        }
        rec(4, ok, QStringLiteral("dialogOk=%1").arg(ok ? 1 : 0));
    }

    // G5: hitTestPreview priority & geometry (text > PiP > V1 > V2 > background, rotation, active time).
    {
        Timeline tl;
        tl.addVideoTrack(false); // V2
        ClipInfo v1;
        v1.filePath = QStringLiteral("v1.mp4");
        v1.displayName = QStringLiteral("V1 Clip");
        v1.duration = 10.0;
        v1.outPoint = 10.0;
        v1.videoScale = 1.0;
        tl.videoTracks()[0]->addClip(v1);

        ClipInfo v2Pip;
        v2Pip.filePath = QStringLiteral("v2.mp4");
        v2Pip.displayName = QStringLiteral("V2 PiP");
        v2Pip.duration = 10.0;
        v2Pip.outPoint = 10.0;
        v2Pip.videoScale = 0.25;
        v2Pip.videoDx = -0.3; // centered at x=0.2, y=0.2
        v2Pip.videoDy = -0.3;
        tl.videoTracks()[1]->addClip(v2Pip);

        EnhancedTextOverlay ovActive;
        ovActive.text = QStringLiteral("Hello");
        ovActive.x = 0.5;
        ovActive.y = 0.5;
        ovActive.width = 0.2;
        ovActive.height = 0.1;
        ovActive.startTime = 1.0;
        ovActive.endTime = 5.0;
        tl.addTextOverlayToFirstVideoClip(ovActive);

        EnhancedTextOverlay ovInactive;
        ovInactive.text = QStringLiteral("Later");
        ovInactive.x = 0.8;
        ovInactive.y = 0.8;
        ovInactive.width = 0.2;
        ovInactive.height = 0.1;
        ovInactive.startTime = 6.0;
        ovInactive.endTime = 9.0;
        tl.addTextOverlayToFirstVideoClip(ovInactive);

        const QRectF lb(100.0, 50.0, 1000.0, 500.0);
        // Hit center (0.5, 0.5) at t=2.0 -> PreviewText (idx 0)
        const auto hText = ctxmenu::hitTestPreview(&tl, 2.0, lb, QPointF(600.0, 300.0));
        // Hit PiP center (0.2, 0.2) at t=2.0 -> PreviewVideoClip (V2 track=1, clip=0)
        const auto hPip = ctxmenu::hitTestPreview(&tl, 2.0, lb, QPointF(300.0, 150.0));
        // Hit (0.8, 0.8) at t=2.0 -> inactive text ignored -> PreviewVideoClip (V1 track=0, clip=0)
        const auto hV1 = ctxmenu::hitTestPreview(&tl, 2.0, lb, QPointF(900.0, 450.0));
        // Hit (0.8, 0.8) at t=7.0 -> active text (idx 1)
        const auto hText2 = ctxmenu::hitTestPreview(&tl, 7.0, lb, QPointF(900.0, 450.0));
        // Hit outside letterbox -> PreviewBackground
        const auto hBgOut = ctxmenu::hitTestPreview(&tl, 2.0, lb, QPointF(20.0, 20.0));
        // Hit past clip end (t=12.0) -> PreviewBackground
        const auto hBgGap = ctxmenu::hitTestPreview(&tl, 12.0, lb, QPointF(600.0, 300.0));

        const bool ok = (hText.target == ctxmenu::Target::PreviewText && hText.overlayIndex == 0)
            && (hPip.target == ctxmenu::Target::PreviewVideoClip && hPip.videoTrackIndex == 1 && hPip.videoClipIndex == 0)
            && (hV1.target == ctxmenu::Target::PreviewVideoClip && hV1.videoTrackIndex == 0 && hV1.videoClipIndex == 0)
            && (hText2.target == ctxmenu::Target::PreviewText && hText2.overlayIndex == 1)
            && (hBgOut.target == ctxmenu::Target::PreviewBackground)
            && (hBgGap.target == ctxmenu::Target::PreviewBackground);
        rec(5, ok, QStringLiteral("text=%1 pip=%2:%3 v1=%4:%5 bg=%6/%7")
                       .arg(hText.overlayIndex)
                       .arg(hPip.videoTrackIndex).arg(hPip.videoClipIndex)
                       .arg(hV1.videoTrackIndex).arg(hV1.videoClipIndex)
                       .arg(static_cast<int>(hBgOut.target))
                       .arg(static_cast<int>(hBgGap.target)));
    }

    // G6: Text editing helpers (color, font size, center, duplicate, remove) + undo round-trip.
    {
        Timeline tl;
        ClipInfo v1;
        v1.filePath = QStringLiteral("v1.mp4");
        v1.duration = 10.0;
        v1.outPoint = 10.0;
        tl.videoTracks()[0]->addClip(v1);

        EnhancedTextOverlay ov;
        ov.text = QStringLiteral("Sample");
        ov.x = 0.25;
        ov.y = 0.30;
        ov.width = 0.3;
        ov.height = 0.1;
        ov.color = Qt::white;
        ov.font.setPointSize(32);
        ov.startTime = 1.0;
        ov.endTime = 4.0;
        tl.addTextOverlayToFirstVideoClip(ov);

        bool ok = ctxmenu::updateTextOverlayColor(&tl, 0, QColor(255, 64, 32));
        ok = ok && (tl.timelineTextOverlays()[0].color == QColor(255, 64, 32));

        ok = ok && ctxmenu::updateTextOverlayFontSize(&tl, 0, 56);
        ok = ok && (tl.timelineTextOverlays()[0].font.pointSize() == 56);

        ok = ok && ctxmenu::centerTextOverlay(&tl, 0);
        ok = ok && (std::abs(tl.timelineTextOverlays()[0].x - 0.5) < 1e-6)
            && (std::abs(tl.timelineTextOverlays()[0].y - 0.5) < 1e-6);

        const int dupIdx = ctxmenu::duplicateTextOverlay(&tl, 0);
        ok = ok && (dupIdx == 1) && (tl.timelineTextOverlays().size() == 2);

        ok = ok && ctxmenu::removeTextOverlay(&tl, 1);
        ok = ok && (tl.timelineTextOverlays().size() == 1);

        // Undo remove restores duplicated overlay
        tl.undo();
        ok = ok && (tl.timelineTextOverlays().size() == 2);
        // Undo duplicate returns to 1 overlay
        tl.undo();
        ok = ok && (tl.timelineTextOverlays().size() == 1);
        rec(6, ok, QStringLiteral("helpersUndoOk=%1").arg(ok ? 1 : 0));
    }

    // G7: insertAudioClipAtTime aligns SFX to text startTime, avoids overlap, and supports undo.
    {
        const QString sfxPath = tmpDir.filePath(QStringLiteral("sfx_pop.wav"));
        bool ok = writeMinimalWavFile(sfxPath, 0.5);
        Timeline tl;
        ok = ok && ctxmenu::insertAudioClipAtTime(&tl, sfxPath, 2.5, 2);
        ok = ok && (tl.audioTracks().size() >= 2)
            && (tl.audioTracks()[1]->clips().size() == 1)
            && (std::abs(tl.audioTracks()[1]->clips()[0].leadInSec - 2.5) < 1e-4);

        // Second SFX at overlapping time 2.6 should land on A3 (track index 2) without clobbering A2.
        ok = ok && ctxmenu::insertAudioClipAtTime(&tl, sfxPath, 2.6, 2);
        ok = ok && (tl.audioTracks().size() >= 3)
            && (tl.audioTracks()[2]->clips().size() == 1)
            && (std::abs(tl.audioTracks()[2]->clips()[0].leadInSec - 2.6) < 1e-4);

        tl.undo();
        const int a3CountAfterUndo = tl.audioTracks().size() >= 3 ? tl.audioTracks()[2]->clips().size() : 0;
        ok = ok && (a3CountAfterUndo == 0);
        rec(7, ok, QStringLiteral("sfxOk=%1").arg(ok ? 1 : 0));
    }

    // G8: GLPreview::beginInlineEditOverlay enters inline edit mode with prefilled text and commits.
    {
        GLPreview preview;
        preview.resize(640, 360);
        preview.setTextToolActive(true);
        int committedIdx = -1;
        QString committedText;
        QObject::connect(&preview, &GLPreview::textOverlayEditCommitted,
                         [&](int idx, const QString &text) {
            committedIdx = idx;
            committedText = text;
        });
        const bool started = preview.beginInlineEditOverlay(0, QRectF(0.5, 0.5, 0.3, 0.12), QStringLiteral("Initial"));
        bool ok = started
            && preview.isTextToolEditing()
            && preview.currentTextToolEditingIndex() == 0
            && preview.currentTextToolInputText() == QStringLiteral("Initial");
        preview.commitCurrentTextToolEdit();
        ok = ok && !preview.isTextToolEditing()
            && committedIdx == 0
            && committedText == QStringLiteral("Initial");
        rec(8, ok, QStringLiteral("started=%1 committed=%2:%3")
                       .arg(started ? 1 : 0).arg(committedIdx).arg(committedText));
    }

    return tally.finish("context-menu-order");
}
