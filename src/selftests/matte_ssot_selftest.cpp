// --selftest=matte-ssot: Timeline matte carrier synchronization, remap and undo.
#include "SelftestGate.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../TrackMatteKey.h"
#include "../libavcore/Encode.h"
#include "../MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDataStream>
#include <QDialog>
#include <QFile>
#include <QFuture>
#include <QImage>
#include <QJsonArray>
#include <QMessageBox>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QWaitCondition>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace {

using Entries = QHash<QString, TimelineTrackMatteEntry>;

bool containsOne(const Entries &entries, const QString &key, const QString &source,
                 TrackMatteType type = TrackMatteType::AlphaMatte)
{
    const auto it = entries.constFind(key);
    return entries.size() == 1 && it != entries.cend()
        && it->matteType == type && it->matteSourceClipId == source;
}

Entries matteAt(const QString &key, const QString &source)
{
    TimelineTrackMatteEntry entry;
    entry.matteType = TrackMatteType::AlphaMatte;
    entry.matteSourceClipId = source;
    return {{key, entry}};
}

bool writeSyntheticClip(const QString &path)
{
    libavcore::EncodeRequest request;
    request.width = 32;
    request.height = 32;
    request.fps = 10;
    request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoBitrateBits = 300000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(32, 32, QImage::Format_RGB888);
    frame.fill(QColor(64, 96, 128));
    for (int i = 0; i < 20; ++i)
        if (!encoder.pushFrame(frame, i)) return false;
    return !encoder.finalize().has_value() && QFile::exists(path);
}

ClipInfo clip(const QString &path, int id)
{
    ClipInfo c;
    c.filePath = path;
    c.displayName = QStringLiteral("matte-%1").arg(id);
    c.linkGroup = id;
    c.duration = 2.0;
    c.inPoint = 0.0;
    c.outPoint = 2.0;
    return c;
}

void seed(Timeline &tl, const QString &path)
{
    const QVector<QVector<ClipInfo>> video{
        {clip(path, 10)}, {clip(path, 1), clip(path, 2), clip(path, 3)}};
    const QVector<QVector<ClipInfo>> audio{{}};
    tl.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    tl.setTrackMatteEntries(matteAt(trackMatteClipKey(1, 1), trackMatteClipKey(1, 2)));
    RotoClipEntry roto;
    tl.setRotoEntries({{trackMatteClipKey(1, 1), roto}});
    tl.setClipParentEntries({{trackMatteClipKey(1, 1), trackMatteClipKey(0, 0)}});
}

bool remapped(const Timeline &tl, const QString &owner, const QString &source)
{
    return containsOne(tl.trackMatteEntries(), owner, source)
        && tl.rotoEntries().size() == 1 && tl.rotoEntries().contains(owner)
        && tl.clipParentEntries().value(owner) == trackMatteClipKey(0, 0);
}

std::string text(const QString &value)
{
    return value.toStdString();
}

std::string show(const Entries &entries)
{
    QStringList parts;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        parts.append(QStringLiteral("%1=%2/%3").arg(it.key()).arg(int(it->matteType))
                         .arg(it->matteSourceClipId));
    std::sort(parts.begin(), parts.end());
    return "{" + parts.join(QLatin1Char(',')).toStdString() + "}";
}

// 10 s of a 440 Hz tone (48 kHz mono PCM16) for convertAudioToKeyframes.
bool writeToneWav(const QString &path)
{
    constexpr quint32 rate = 48000;
    constexpr quint32 samples = rate * 10;
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + samples * 2);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << rate << quint32(rate * 2)
        << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(samples * 2);
    for (quint32 i = 0; i < samples; ++i)
        out << qint16(std::lround(16000.0 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / rate)));
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

// Runs a MainWindow call that opens modal dialogs. The first QDialog goes to
// `handle` (its result is the done() code); a QMessageBox (a failure path) is
// recorded and closed so it cannot block the selftest.
QStringList driveModal(const std::function<void()> &call,
                       const std::function<int(QDialog *)> &handle)
{
    QStringList messages;
    bool handled = false;
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (auto *box = qobject_cast<QMessageBox *>(dialog)) {
            messages.append(box->text());
            box->done(QMessageBox::Ok);
            return;
        }
        dialog->done(handled ? QDialog::Rejected : handle(dialog));
        handled = true;
    });
    timer.start();
    call();
    timer.stop();
    return messages;
}

// MainWindow::configureTrackMatte through its dialog (track-ops G11 steps).
bool configureMatte(MainWindow &window, TrackMatteType type, const QString &source,
                    QStringList *messages)
{
    bool configured = false;
    *messages += driveModal(
        [&] { QMetaObject::invokeMethod(&window, "configureTrackMatte", Qt::DirectConnection); },
        [&](QDialog *dialog) {
            auto *typeCombo = dialog->findChild<QComboBox *>(QStringLiteral("trackMatteTypeCombo"));
            auto *sourceCombo = dialog->findChild<QComboBox *>(QStringLiteral("trackMatteSourceCombo"));
            const int typeIndex = typeCombo ? typeCombo->findData(int(type)) : -1;
            const int sourceIndex = sourceCombo ? sourceCombo->findData(source) : -1;
            configured = typeIndex >= 0 && sourceIndex >= 0;
            if (!configured) return int(QDialog::Rejected);
            typeCombo->setCurrentIndex(typeIndex);
            sourceCombo->setCurrentIndex(sourceIndex);
            return int(QDialog::Accepted);
        });
    return configured;
}

// V1 {10, 11}, V2 {1, 2, 3} (2 s each), optional A1 tone; V2:1 carries a
// luma matte sourced from V1:1. One baseline undo entry.
void seedWindow(Timeline &tl, const QString &video, const QString &audio = {})
{
    const QVector<QVector<ClipInfo>> videoTracks{
        {clip(video, 10), clip(video, 11)}, {clip(video, 1), clip(video, 2), clip(video, 3)}};
    QVector<QVector<ClipInfo>> audioTracks{{}};
    if (!audio.isEmpty()) {
        ClipInfo tone = clip(audio, 50);
        tone.duration = 10.0;
        tone.outPoint = 10.0;
        audioTracks = {{tone}};
    }
    tl.clearSelection();
    tl.restoreFromProject(videoTracks, audioTracks, 0.0, -1.0, -1.0, 100);
    TimelineTrackMatteEntry luma;
    luma.matteType = TrackMatteType::LumaMatte;
    luma.matteSourceClipId = trackMatteClipKey(0, 1);
    tl.setTrackMatteEntries({{trackMatteClipKey(1, 1), luma}});
    tl.undoManager()->clear();
    tl.undoManager()->saveState(tl.currentState(), QStringLiteral("基準"));
}

void selectClip(Timeline &tl, int track, int clipIndex)
{
    QString error;
    tl.clearSelection();
    tl.selectClipByIndex(false, track, clipIndex, &error);
}

} // namespace

int runMatteSsotSelftest()
{
    selftest::GateTally tally;

    // G1 catches a torn/invalid getter snapshot under concurrent publication.
    {
        Timeline tl;
        const Entries a = matteAt(QStringLiteral("1:1"), QStringLiteral("1:2"));
        const Entries b = matteAt(QStringLiteral("1:1"), QStringLiteral("0:0"));
        tl.setTrackMatteEntries(a);
        QThreadPool pool;
        pool.setMaxThreadCount(2);
        QMutex startMutex;
        QWaitCondition startCondition;
        int ready = 0;
        const auto waitForBothWorkers = [&] {
            QMutexLocker lock(&startMutex);
            if (++ready == 2) startCondition.wakeAll();
            else while (ready < 2) startCondition.wait(&startMutex);
        };
        int setterCount = 0;
        int getterCount = 0;
        int inconsistent = 0;
        auto writer = QtConcurrent::run(&pool, [&] {
            waitForBothWorkers();
            for (int i = 0; i < 100; ++i) {
                tl.setTrackMatteEntries(i % 2 ? a : b);
                ++setterCount;
            }
        });
        auto reader = QtConcurrent::run(&pool, [&] {
            waitForBothWorkers();
            for (int i = 0; i < 1000; ++i) {
                const Entries seen = tl.trackMatteEntries();
                if (!containsOne(seen, QStringLiteral("1:1"), QStringLiteral("1:2"))
                    && !containsOne(seen, QStringLiteral("1:1"), QStringLiteral("0:0")))
                    ++inconsistent;
                ++getterCount;
            }
        });
        writer.waitForFinished();
        reader.waitForFinished();
        const bool roundTrip = containsOne(tl.trackMatteEntries(),
                                           QStringLiteral("1:1"), QStringLiteral("1:2"));
        const bool invalidRejected = !containsOne(
            matteAt(QStringLiteral("1:1"), QStringLiteral("9:9")),
            QStringLiteral("1:1"), QStringLiteral("1:2"));
        tally.gate(1, setterCount == 100 && getterCount == 1000
                   && inconsistent == 0 && roundTrip && invalidRejected,
                   "setter=" + std::to_string(setterCount)
                   + " getter=" + std::to_string(getterCount)
                   + " inconsistent=" + std::to_string(inconsistent)
                   + " roundTrip=" + std::to_string(roundTrip)
                   + " invalidRejected=" + std::to_string(invalidRejected));
    }

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("matte-ssot.mp4"));
    const bool mediaReady = dir.isValid() && writeSyntheticClip(path);

    // G2 catches owner/source key drift after an edit before the matte clip.
    {
        bool deleted = false, split = false, external = false;
        QString deleteError, splitError;
        if (mediaReady) {
            Timeline tl;
            seed(tl, path);
            deleted = tl.deleteClipByIndex(false, 1, 0, false, &deleteError)
                && remapped(tl, trackMatteClipKey(1, 0), trackMatteClipKey(1, 1));
        }
        if (mediaReady) {
            Timeline tl;
            seed(tl, path);
            split = tl.splitClipByIndex(false, 1, 0, 1.0, &splitError)
                && remapped(tl, trackMatteClipKey(1, 2), trackMatteClipKey(1, 3));
        }
        if (mediaReady) {
            Timeline tl;
            seed(tl, path);
            const TrackClipSnapshot before = snapshotTrackClips(&tl);
            auto clips = tl.videoTracks().at(1)->clips();
            clips.removeAt(0);
            tl.videoTracks().at(1)->setClips(clips);
            tl.remapCarriersAfterExternalMutation(before);
            external = remapped(tl, trackMatteClipKey(1, 0), trackMatteClipKey(1, 1));
        }
        const std::string detail = "mediaReady=" + std::to_string(mediaReady)
            + " delete 1:1->1:0 source 1:2->1:1=" + std::to_string(deleted)
            + " split 1:1->1:2 source 1:2->1:3=" + std::to_string(split)
            + " external 1:1->1:0 source 1:2->1:1=" + std::to_string(external)
            + " deleteError=" + text(deleteError) + " splitError=" + text(splitError);
        tally.gate(2, mediaReady && deleted && split && external, detail);
    }

    // G3 catches missing TimelineState matte snapshots during undo and redo.
    {
        bool initial = false, removed = false, restored = false, redone = false;
        QString error;
        if (mediaReady) {
            Timeline tl;
            seed(tl, path);
            tl.saveUndoState(QStringLiteral("マットを設定"));
            initial = containsOne(tl.currentState().trackMatteEntries,
                                  trackMatteClipKey(1, 1), trackMatteClipKey(1, 2));
            removed = tl.deleteClipByIndex(false, 1, 1, false, &error)
                && tl.trackMatteEntries().isEmpty();
            tl.undo();
            restored = containsOne(tl.trackMatteEntries(),
                                   trackMatteClipKey(1, 1), trackMatteClipKey(1, 2));
            tl.redo();
            redone = tl.trackMatteEntries().isEmpty();
        }
        tally.gate(3, mediaReady && initial && removed && restored && redone,
                   "initial=" + std::to_string(initial)
                   + " removed=" + std::to_string(removed)
                   + " undo=" + std::to_string(restored)
                   + " redo=" + std::to_string(redone)
                   + " error=" + text(error));
    }

    // G4-G6 drive MainWindow, which keeps no matte copy: Timeline is the SSOT.
    MainWindow window;
    Timeline *wtl = window.findChild<Timeline *>(QStringLiteral("mainTimeline"));

    // G4 catches configureTrackMatte writing somewhere other than the Timeline
    // carrier (set and clear) or saving an undo state for the matte edit.
    {
        bool set = false, cleared = false, configuredSet = false, configuredClear = false;
        quint64 serialBefore = 0, serialSet = 0, serialClear = 0;
        int selTrack = -1, selClip = -1;
        Entries afterSet, afterClear;
        QStringList messages;
        if (mediaReady && wtl) {
            seedWindow(*wtl, path);
            wtl->setTrackMatteEntries({});
            wtl->restoreState(wtl->currentState());
            selectClip(*wtl, 1, 1);
            selTrack = window.selectedVideoTrackIndex();
            selClip = window.selectedVideoClipIndexTracked();
            serialBefore = wtl->undoManager()->saveSerial();
            configuredSet = configureMatte(window, TrackMatteType::LumaMatte,
                                           trackMatteClipKey(0, 0), &messages);
            afterSet = wtl->trackMatteEntries();
            serialSet = wtl->undoManager()->saveSerial();
            set = containsOne(afterSet, trackMatteClipKey(1, 1), trackMatteClipKey(0, 0),
                              TrackMatteType::LumaMatte);
            configuredClear = configureMatte(window, TrackMatteType::None, QString(), &messages);
            afterClear = wtl->trackMatteEntries();
            serialClear = wtl->undoManager()->saveSerial();
            cleared = afterClear.isEmpty();
        }
        tally.gate(4, mediaReady && wtl && selTrack == 1 && selClip == 1 && configuredSet
                   && set && configuredClear && cleared && messages.isEmpty()
                   && serialSet == serialBefore && serialClear == serialBefore,
                   "selected=" + std::to_string(selTrack) + ":" + std::to_string(selClip)
                   + " set=" + show(afterSet) + " clear=" + show(afterClear)
                   + " saveSerial=" + std::to_string(serialBefore) + "->"
                   + std::to_string(serialSet) + "->" + std::to_string(serialClear)
                   + " messages=" + text(messages.join(QLatin1Char('|'))));
    }

    // G5 catches a MainWindow edit site whose Timeline edit leaves the matte
    // keys behind (16-row table: Timeline-internal remap or the catch-all
    // remapCarriersAfterExternalMutation). Moving sites change a key; the
    // convertAudioToKeyframes setClips site is property-only (identity) and
    // corroborates the table row instead of detecting a regression.
    {
        const QString tone = dir.filePath(QStringLiteral("matte-ssot.wav"));
        const bool toneReady = mediaReady && writeToneWav(tone);
        struct Site {
            const char *name;
            std::function<void()> run;
            QString owner;
            QString source;
            bool audio;
        };
        const QString owner0 = trackMatteClipKey(1, 1);
        const QString source0 = trackMatteClipKey(0, 1);
        int keyframeTracksBefore = -1, keyframeTracksAfter = -1;
        quint64 convertSerial = 0;
        const QVector<Site> sites{
            {"deleteClip", [&] {
                 selectClip(*wtl, 1, 0);
                 wtl->videoTracks()[0]->toggleClipSelection(0);
                 window.deleteClip();
             }, trackMatteClipKey(1, 0), trackMatteClipKey(0, 0), false},
            {"moveClipHeadToPlayhead", [&] {
                 selectClip(*wtl, 1, 1);
                 wtl->setPlayheadPosition(10.0);
                 window.m_moveClipHeadAction->trigger();
             }, trackMatteClipKey(1, 2), source0, false},
            {"pasteClip", [&] {
                 selectClip(*wtl, 0, 0);
                 window.copyClip();
                 wtl->setPlayheadPosition(0.0);
                 window.pasteClip();
             }, owner0, trackMatteClipKey(0, 2), false},
            {"convertAudioToKeyframes", [&] {
                 keyframeTracksBefore = int(wtl->videoTracks()[1]->clips()[1].keyframes.tracks().size());
                 const quint64 serial = wtl->undoManager()->saveSerial();
                 const QStringList boxes = driveModal(
                     [&] { window.convertAudioToKeyframes(1, 1); },
                     [](QDialog *) { return int(QDialog::Accepted); });
                 convertSerial = wtl->undoManager()->saveSerial() - serial;
                 keyframeTracksAfter = boxes.isEmpty()
                     ? int(wtl->videoTracks()[1]->clips()[1].keyframes.tracks().size()) : -1;
             }, owner0, source0, true},
        };
        const bool ready = mediaReady && toneReady && wtl;
        bool ok = ready;
        std::string detail = "mediaReady=" + std::to_string(mediaReady)
            + " toneReady=" + std::to_string(toneReady);
        for (const Site &site : sites) {
            if (!ready) break;
            seedWindow(*wtl, path, site.audio ? tone : QString());
            site.run();
            const Entries after = wtl->trackMatteEntries();
            const bool moved = site.owner != owner0 || site.source != source0;
            bool siteOk = containsOne(after, site.owner, site.source, TrackMatteType::LumaMatte);
            if (site.audio)
                siteOk = siteOk && convertSerial == 1 && keyframeTracksAfter == keyframeTracksBefore + 1;
            std::cerr << "G5 site=" << site.name << " owner " << text(owner0) << "->"
                      << text(site.owner) << " source " << text(source0) << "->"
                      << text(site.source) << " moved=" << moved
                      << " got=" << show(after) << " ok=" << siteOk << '\n';
            detail += std::string(" ") + site.name + "=" + std::to_string(siteOk);
            ok = ok && siteOk;
        }
        detail += " convertKeyframeTracks=" + std::to_string(keyframeTracksBefore) + "->"
            + std::to_string(keyframeTracksAfter) + " convertSaves=" + std::to_string(convertSerial);
        tally.gate(5, ok, detail);
    }

    // G6 catches a structural undo that restores the TimelineState matte
    // snapshot (empty here: the matte edit saved no undo) instead of the
    // external "mattes" capture. Same flow as track-ops G11.
    {
        bool configured = false, captured = false, removed = false;
        bool undone = false, redone = false;
        int baselineMattes = -1, externalMattes = -1;
        QStringList messages;
        const auto hasMatte = [&] {
            return containsOne(wtl->trackMatteEntries(), trackMatteClipKey(1, 1),
                               trackMatteClipKey(0, 0), TrackMatteType::LumaMatte);
        };
        if (mediaReady && wtl) {
            seedWindow(*wtl, path);
            wtl->setTrackMatteEntries({});
            wtl->undoManager()->clear();
            wtl->undoManager()->saveState(wtl->currentState(), QStringLiteral("基準"));
            wtl->restoreState(wtl->currentState());
            baselineMattes = int(wtl->currentState().trackMatteEntries.size());
            selectClip(*wtl, 1, 1);
            const quint64 serial = wtl->undoManager()->saveSerial();
            configured = configureMatte(window, TrackMatteType::LumaMatte,
                                        trackMatteClipKey(0, 0), &messages)
                && hasMatte() && wtl->undoManager()->saveSerial() == serial;
            externalMattes = int(window.collectExternalTrackState()
                                     .value(QStringLiteral("mattes")).toArray().size());
            captured = externalMattes == 1;
            removed = wtl->removeTrack(false, 1) && wtl->trackMatteEntries().isEmpty();
            wtl->undo();
            undone = wtl->videoTrackCount() == 2 && hasMatte();
            wtl->redo();
            redone = wtl->videoTrackCount() == 1 && wtl->trackMatteEntries().isEmpty();
        }
        tally.gate(6, mediaReady && wtl && baselineMattes == 0 && configured && captured
                   && removed && undone && redone && messages.isEmpty(),
                   "baselineStateMattes=" + std::to_string(baselineMattes)
                   + " externalMattes=" + std::to_string(externalMattes)
                   + " configured=" + std::to_string(configured)
                   + " removeTrack=" + std::to_string(removed)
                   + " undo=" + std::to_string(undone)
                   + " redo=" + std::to_string(redone)
                   + " messages=" + text(messages.join(QLatin1Char('|'))));
    }
    return tally.finish("matte-ssot");
}
