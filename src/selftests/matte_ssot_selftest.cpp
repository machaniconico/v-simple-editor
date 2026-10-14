// --selftest=matte-ssot: Timeline matte carrier synchronization, remap and undo,
// plus the absorption of undo-less edits, the bus routing undo carrier and the
// uid-keyed remap of paste clones.
#include "SelftestGate.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../TrackMatteKey.h"
#include "../libavcore/Encode.h"
#include "../MainWindow.h"
#include "../AudioBusPanel.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDataStream>
#include <QDialog>
#include <QFile>
#include <QFuture>
#include <QImage>
#include <QJsonArray>
#include <QMimeData>
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

// G11-G13: V1 {C} (linkGroup 0), V2 {S}, empty V3 and A1. With A1 empty,
// Timeline::pasteClip inserts the clone at V1 index 0, so a paste gives
// V1 {clone, C}: equal filePath, linkGroup and inPoint, distinct uids.
void seedClones(Timeline &tl, const QString &path, bool paste)
{
    const QVector<QVector<ClipInfo>> video{{clip(path, 0)}, {clip(path, 10)}, {}};
    const QVector<QVector<ClipInfo>> audio{{}};
    tl.clearSelection();
    tl.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    if (!paste) return;
    selectClip(tl, 0, 0);
    tl.copySelectedClip();
    tl.pasteClip();
}

// Matte (source V2:0), roto and parent (V2:0) on one clip key.
void attachCarriers(Timeline &tl, const QString &owner)
{
    tl.setTrackMatteEntries(matteAt(owner, trackMatteClipKey(1, 0)));
    tl.setRotoEntries({{owner, RotoClipEntry{}}});
    tl.setClipParentEntries({{owner, trackMatteClipKey(1, 0)}});
}

bool carriedTo(const Timeline &tl, const QString &owner)
{
    return containsOne(tl.trackMatteEntries(), owner, trackMatteClipKey(1, 0))
        && tl.rotoEntries().size() == 1 && tl.rotoEntries().contains(owner)
        && tl.clipParentEntries().size() == 1
        && tl.clipParentEntries().value(owner) == trackMatteClipKey(1, 0);
}

bool carriersEmpty(const Timeline &tl)
{
    return tl.trackMatteEntries().isEmpty() && tl.rotoEntries().isEmpty()
        && tl.clipParentEntries().isEmpty();
}

std::string showCarriers(const Timeline &tl)
{
    QStringList roto = tl.rotoEntries().keys();
    std::sort(roto.begin(), roto.end());
    QStringList parents;
    const auto parentEntries = tl.clipParentEntries();
    for (auto it = parentEntries.cbegin(); it != parentEntries.cend(); ++it)
        parents.append(it.key() + QStringLiteral("->") + it.value());
    std::sort(parents.begin(), parents.end());
    return "matte=" + show(tl.trackMatteEntries())
        + " roto={" + roto.join(QLatin1Char(',')).toStdString() + "}"
        + " parent={" + parents.join(QLatin1Char(',')).toStdString() + "}";
}

QString uidAt(const Timeline &tl, int track, int clipIndex)
{
    const auto &tracks = tl.videoTracks();
    if (track < 0 || track >= tracks.size() || !tracks[track]) return {};
    const auto clips = tracks[track]->clips();
    return clipIndex >= 0 && clipIndex < clips.size() ? clips[clipIndex].uid : QString();
}

// The two V1 clips after a paste are clones apart from their uid.
bool clonePair(const Timeline &tl)
{
    const auto clips = tl.videoTracks().value(0) ? tl.videoTracks()[0]->clips()
                                                 : QVector<ClipInfo>{};
    return clips.size() == 2 && clips[0].filePath == clips[1].filePath
        && clips[0].linkGroup == 0 && clips[1].linkGroup == 0
        && clips[0].inPoint == clips[1].inPoint
        && !clips[0].uid.isEmpty() && !clips[1].uid.isEmpty()
        && clips[0].uid != clips[1].uid;
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

    // G6 catches a structural undo that loses the matte edit, which saved no
    // undo point (the baseline is absorbed; the external "mattes" capture also
    // carries it). Same flow as track-ops G11.
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

    // G7-G9 share one flow: an empty-matte baseline, then the dialog on V2:1.
    const auto serialNow = [&] { return wtl->undoManager()->saveSerial(); };
    const auto hasMatte00 = [&] {
        return containsOne(wtl->trackMatteEntries(), trackMatteClipKey(1, 1),
                           trackMatteClipKey(0, 0), TrackMatteType::LumaMatte);
    };
    const auto emptyBaseline = [&] {
        seedWindow(*wtl, path);
        wtl->setTrackMatteEntries({});
        wtl->undoManager()->clear();
        wtl->undoManager()->saveState(wtl->currentState(), QStringLiteral("基準"));
        wtl->restoreState(wtl->currentState());
    };
    // V2:2 is the last clip, so a nudge to the right only grows its lead-in.
    const auto lastLeadIn = [&] {
        const auto clips = wtl->videoTracks()[1]->clips();
        return clips.size() == 3 ? clips[2].leadInSec : -1.0;
    };
    int historySignals = 0;
    QMetaObject::Connection historyCounter;
    if (wtl)
        historyCounter = QObject::connect(wtl->undoManager(), &UndoManager::historyChanged,
                                          wtl, [&historySignals] { ++historySignals; });

    // G7 catches a matte edit lost by undoing the next ordinary edit: the
    // dialog saves no undo point, so the pre-matte snapshot came back (HEAD
    // FAIL). The edit is absorbed into the current undo point instead.
    {
        bool configured = false, undone = false;
        quint64 serialBefore = 0, serialConfigured = 0, serialNudged = 0;
        int absorbHistory = -1;
        double startBefore = -1.0, startNudged = -1.0, startUndone = -1.0;
        QStringList messages;
        if (mediaReady && wtl) {
            emptyBaseline();
            selectClip(*wtl, 1, 1);
            serialBefore = serialNow();
            const int history = historySignals;
            configured = configureMatte(window, TrackMatteType::LumaMatte,
                                        trackMatteClipKey(0, 0), &messages) && hasMatte00();
            absorbHistory = historySignals - history;
            serialConfigured = serialNow();
            startBefore = lastLeadIn();
            selectClip(*wtl, 1, 2);
            wtl->nudgeSelectedClips(5);
            serialNudged = serialNow();
            startNudged = lastLeadIn();
            wtl->undo();
            startUndone = lastLeadIn();
            undone = hasMatte00();
        }
        tally.gate(7, mediaReady && wtl && configured && serialConfigured == serialBefore
                   && absorbHistory == 0 && serialNudged == serialBefore + 1
                   && startNudged > startBefore && startUndone == startBefore && undone
                   && messages.isEmpty(),
                   "configured=" + std::to_string(configured)
                   + " saveSerial=" + std::to_string(serialBefore) + "->"
                   + std::to_string(serialConfigured) + "->" + std::to_string(serialNudged)
                   + " absorbHistorySignals=" + std::to_string(absorbHistory)
                   + " V2:2 leadIn=" + std::to_string(startBefore) + "->"
                   + std::to_string(startNudged) + "->" + std::to_string(startUndone)
                   + " undoKeepsMatte=" + std::to_string(undone)
                   + " got=" + (wtl ? show(wtl->trackMatteEntries()) : std::string("-"))
                   + " messages=" + text(messages.join(QLatin1Char('|'))));
    }

    // G8 (a) is the G6 flow with the absorbed matte and addVideoTrack: the
    // structural undo must keep it (detects an absorb that breaks structural
    // undo). (b) catches importMedia creating a track without the external
    // capture: undoing the import restored the stale baseline matte (HEAD FAIL).
    {
        bool configured = false, added = false, undoneA = false;
        bool imported = false, undoneB = false;
        int tracksA = -1, tracksB = -1, tracksUndoneB = -1;
        quint64 serialA = 0, serialAddA = 0;
        QString importError;
        Entries gotB;
        QStringList messages;
        if (mediaReady && wtl) {
            emptyBaseline();
            selectClip(*wtl, 1, 1);
            const quint64 serial = serialNow();
            configured = configureMatte(window, TrackMatteType::LumaMatte,
                                        trackMatteClipKey(0, 0), &messages) && hasMatte00();
            serialA = serialNow() - serial;
            wtl->addVideoTrack();
            tracksA = wtl->videoTrackCount();
            serialAddA = serialNow() - serial;
            added = tracksA == 3 && hasMatte00();
            wtl->undo();
            undoneA = wtl->videoTrackCount() == 2 && hasMatte00();

            seedWindow(*wtl, path);   // baseline matte V2:1 <- V1:1
            wtl->setTrackMatteEntries(matteAt(trackMatteClipKey(1, 1), trackMatteClipKey(0, 0)));
            imported = wtl->importMedia(path, 2, 0.0, nullptr, &importError,
                                        Timeline::ImportMediaKind::VideoOnly);
            tracksB = wtl->videoTrackCount();
            wtl->undo();
            tracksUndoneB = wtl->videoTrackCount();
            gotB = wtl->trackMatteEntries();
            undoneB = tracksUndoneB == 2
                && containsOne(gotB, trackMatteClipKey(1, 1), trackMatteClipKey(0, 0));
        }
        tally.gate(8, mediaReady && wtl && configured && serialA == 0 && serialAddA == 1
                   && added && undoneA && imported && tracksB == 3 && undoneB
                   && messages.isEmpty(),
                   "a: configured=" + std::to_string(configured)
                   + " saves=" + std::to_string(serialA) + "/" + std::to_string(serialAddA)
                   + " tracks=" + std::to_string(tracksA)
                   + " undoKeepsMatte=" + std::to_string(undoneA)
                   + " b: imported=" + std::to_string(imported)
                   + " tracks=" + std::to_string(tracksB) + "->" + std::to_string(tracksUndoneB)
                   + " undoKeepsLiveMatte=" + std::to_string(undoneB) + " got=" + show(gotB)
                   + " importError=" + text(importError)
                   + " messages=" + text(messages.join(QLatin1Char('|'))));
    }

    // G9 catches a matte edit after an undo that leaves the redo entry alive:
    // redo then restored the pre-matte snapshot (HEAD FAIL). The absorption
    // drops the redo stack, signals historyChanged once and keeps saveSerial.
    // Two no-op controls keep redo: (i) clearing V2:1, which has no matte, and
    // (ii) re-applying the identical matte after nudge -> undo. An absorb that
    // runs without a change drops redo and signals historyChanged.
    {
        bool configured = false, redoBefore = false, redoAfter = true, kept = false;
        bool clearedNoop = false, clearRedo = false;
        bool sameBefore = false, sameNoop = false, sameRedo = false;
        quint64 serialUndone = 0, serialConfigured = 0;
        int absorbHistory = -1, clearHistory = -1, sameHistory = -1;
        QStringList messages;
        if (mediaReady && wtl) {
            emptyBaseline();
            selectClip(*wtl, 1, 2);
            wtl->nudgeSelectedClips(5);
            wtl->undo();
            redoBefore = wtl->canRedo();
            serialUndone = serialNow();
            selectClip(*wtl, 1, 1);
            int history = historySignals;
            clearedNoop = configureMatte(window, TrackMatteType::None, QString(), &messages)
                && wtl->trackMatteEntries().isEmpty();
            clearHistory = historySignals - history;
            clearRedo = wtl->canRedo();
            history = historySignals;
            configured = configureMatte(window, TrackMatteType::LumaMatte,
                                        trackMatteClipKey(0, 0), &messages) && hasMatte00();
            absorbHistory = historySignals - history;
            serialConfigured = serialNow();
            redoAfter = wtl->canRedo();
            wtl->redo();
            kept = hasMatte00();

            selectClip(*wtl, 1, 2);
            wtl->nudgeSelectedClips(5);
            wtl->undo();
            sameBefore = wtl->canRedo() && hasMatte00();
            selectClip(*wtl, 1, 1);
            history = historySignals;
            sameNoop = configureMatte(window, TrackMatteType::LumaMatte,
                                      trackMatteClipKey(0, 0), &messages) && hasMatte00();
            sameHistory = historySignals - history;
            sameRedo = wtl->canRedo();
        }
        tally.gate(9, mediaReady && wtl && redoBefore && configured && !redoAfter && kept
                   && absorbHistory == 1 && serialConfigured == serialUndone
                   && clearedNoop && clearRedo && clearHistory == 0
                   && sameBefore && sameNoop && sameRedo && sameHistory == 0
                   && messages.isEmpty(),
                   "canRedo=" + std::to_string(redoBefore) + "->" + std::to_string(redoAfter)
                   + " configured=" + std::to_string(configured)
                   + " absorbHistorySignals=" + std::to_string(absorbHistory)
                   + " saveSerial=" + std::to_string(serialUndone) + "->"
                   + std::to_string(serialConfigured)
                   + " matteAfterRedo=" + std::to_string(kept)
                   + " noopClear: configured=" + std::to_string(clearedNoop)
                   + " canRedo=" + std::to_string(clearRedo)
                   + " historySignals=" + std::to_string(clearHistory)
                   + " noopSame: redoAndMatteBefore=" + std::to_string(sameBefore)
                   + " configured=" + std::to_string(sameNoop)
                   + " canRedo=" + std::to_string(sameRedo)
                   + " historySignals=" + std::to_string(sameHistory)
                   + " messages=" + text(messages.join(QLatin1Char('|'))));
    }
    QObject::disconnect(historyCounter);

    // G10 catches bus routing missing from TimelineState: undo / redo left the
    // routing untouched (HEAD has no carrier). Track 0 goes to bus 1, then bus 2,
    // one undo point each. A bus panel edit is then absorbed into the bus-2
    // point (redo brings it back) without a new undo point.
    {
        auto &routing = window.m_audioBusRouting;
        int busUndo = -2, busRedo = -2, busPanelUndo = -2, busPanelRedo = -2;
        bool assigned = false, muteUndo = true, muteRedo = false;
        quint64 serialPanel = 0, serialPanelAfter = 1;
        const auto bus2Muted = [&] {
            const auto *bus = routing.bus(2);
            return bus && bus->mute;
        };
        if (wtl && window.m_audioBusPanel) {
            routing.fromJson(QJsonObject{});
            routing.addBus(QStringLiteral("バス 0"));
            routing.addBus(QStringLiteral("バス 1"));
            routing.addBus(QStringLiteral("バス 2"));
            window.onAudioBusRoutingChanged();
            wtl->undoManager()->clear();
            wtl->undoManager()->saveState(wtl->currentState(), QStringLiteral("基準"));
            assigned = routing.assignTrackToBus(0, 1);
            wtl->saveUndoState(QStringLiteral("トラックのバスを変更"));
            assigned = routing.assignTrackToBus(0, 2) && assigned;
            wtl->saveUndoState(QStringLiteral("トラックのバスを変更"));
            wtl->undo();
            busUndo = routing.trackBus(0);
            wtl->redo();
            busRedo = routing.trackBus(0);
            routing.setBusMute(2, true);
            serialPanel = serialNow();
            emit window.m_audioBusPanel->routingChanged();
            serialPanelAfter = serialNow();
            wtl->undo();
            busPanelUndo = routing.trackBus(0);
            muteUndo = bus2Muted();
            wtl->redo();
            busPanelRedo = routing.trackBus(0);
            muteRedo = bus2Muted();
        }
        tally.gate(10, wtl && assigned && busUndo == 1 && busRedo == 2
                   && serialPanelAfter == serialPanel && busPanelUndo == 1 && !muteUndo
                   && busPanelRedo == 2 && muteRedo,
                   "assigned=" + std::to_string(assigned)
                   + " trackBus(0) undo=" + std::to_string(busUndo)
                   + " redo=" + std::to_string(busRedo)
                   + " panel saveSerial=" + std::to_string(serialPanel) + "->"
                   + std::to_string(serialPanelAfter)
                   + " panel undo bus=" + std::to_string(busPanelUndo)
                   + " mute=" + std::to_string(muteUndo)
                   + " redo bus=" + std::to_string(busPanelRedo)
                   + " mute=" + std::to_string(muteRedo));
    }

    // G11 catches the carrier remap binding a paste clone to its original:
    // V1 {clone, C} with matte, roto and parent on C (V1:1); deleting the
    // clone must move all three to V1:0. Without the uid, C's old key binds
    // to nothing (the clone's old key took the one survivor) and all three
    // are pruned (HEAD FAIL).
    {
        bool pasted = false, deleted = false, survivor = false, carried = false;
        QString uidC, error;
        std::string got = "-";
        if (mediaReady) {
            Timeline tl;
            seedClones(tl, path, true);
            pasted = clonePair(tl);
            uidC = uidAt(tl, 0, 1);
            attachCarriers(tl, trackMatteClipKey(0, 1));
            deleted = tl.deleteClipByIndex(false, 0, 0, false, &error);
            survivor = tl.videoTracks()[0]->clipCount() == 1 && uidAt(tl, 0, 0) == uidC;
            carried = carriedTo(tl, trackMatteClipKey(0, 0));
            got = showCarriers(tl);
        }
        tally.gate(11, mediaReady && pasted && deleted && survivor && carried,
                   "mediaReady=" + std::to_string(mediaReady)
                   + " clonePair=" + std::to_string(pasted)
                   + " delete V1:0=" + std::to_string(deleted)
                   + " survivorIsC=" + std::to_string(survivor)
                   + " want matte/roto/parent 0:1->0:0 got " + got
                   + " error=" + text(error));
    }

    // G12 (a) catches the paste remap leaving the carriers on the clone: C
    // (V1:0) owns matte, roto and parent, and the paste inserts its clone at
    // V1:0, so all three must follow C to V1:1. Without the uid C's old key
    // binds to the clone at V1:0 (HEAD FAIL). (b) drops C onto V3 across
    // tracks; the keys follow to V3:0 through the explicit cross-track map,
    // which needs no uid, so (b) corroborates and does not detect the uid.
    {
        bool pastedA = false, followedA = false, pastedB = false, droppedB = false;
        bool followedB = false;
        QString uidA, uidB;
        std::string gotA = "-", gotB = "-";
        if (mediaReady) {
            Timeline tl;
            seedClones(tl, path, false);
            uidA = uidAt(tl, 0, 0);
            attachCarriers(tl, trackMatteClipKey(0, 0));
            selectClip(tl, 0, 0);
            tl.copySelectedClip();
            tl.pasteClip();
            pastedA = clonePair(tl) && uidAt(tl, 0, 1) == uidA;
            followedA = carriedTo(tl, trackMatteClipKey(0, 1));
            gotA = showCarriers(tl);
        }
        if (mediaReady) {
            Timeline tl;
            seedClones(tl, path, true);
            pastedB = clonePair(tl);
            uidB = uidAt(tl, 0, 1);
            attachCarriers(tl, trackMatteClipKey(0, 1));
            QMimeData mime;
            mime.setData(QStringLiteral("application/x-vse-clip"), QByteArray("1:0"));
            TimelineTrack *v1 = tl.videoTracks()[0];
            TimelineTrack *v3 = tl.videoTracks().value(2);
            droppedB = v3 && v3->acceptClipDrop(&mime, QPoint(0, 4), v1)
                && v1->clipCount() == 1 && v3->clipCount() == 1 && uidAt(tl, 2, 0) == uidB;
            followedB = carriedTo(tl, trackMatteClipKey(2, 0));
            gotB = showCarriers(tl);
        }
        tally.gate(12, mediaReady && pastedA && followedA && pastedB && droppedB && followedB,
                   "a: paste clone before C: pasted=" + std::to_string(pastedA)
                   + " want 0:0->0:1 got " + gotA
                   + " b: cross-track drop of C V1:1->V3:0: clonePair=" + std::to_string(pastedB)
                   + " dropped=" + std::to_string(droppedB)
                   + " want 0:1->2:0 got " + gotB);
    }

    // G13 is the negative control for the uid-less fallback. (i) sameAs: two
    // uids decide alone, one empty uid falls back to filePath / linkGroup /
    // inPoint. (ii) A snapshot with its uids cleared (a pre-uid key) still
    // remaps distinct clips (the G2 external delete). (iii) The G11 clones
    // with that uid-less snapshot keep the three-field ambiguity: the
    // carriers are pruned, while (iv) the same edit with the uid snapshot
    // moves them to V1:0, so G11 depends on the uid.
    {
        const auto key = [](const QString &file, int link, double in, const QString &uid) {
            ClipKeyId id;
            id.filePath = file;
            id.linkGroup = link;
            id.inPoint = in;
            id.uid = uid;
            return id;
        };
        const QString a = QStringLiteral("a.mp4"), b = QStringLiteral("b.mp4");
        const QString u1 = QStringLiteral("u1"), u2 = QStringLiteral("u2");
        const bool clonesApart = !key(a, 0, 0.0, u1).sameAs(key(a, 0, 0.0, u2));
        const bool uidWins = key(a, 0, 0.0, u1).sameAs(key(b, 5, 3.0, u1));
        const bool fallbackSame = key(a, 0, 0.0, QString()).sameAs(key(a, 0, 0.0, u2))
            && key(a, 0, 0.0, u1).sameAs(key(a, 0, 0.0, QString()));
        const bool fallbackDiffers = !key(a, 0, 0.0, QString()).sameAs(key(a, 0, 1.0, u2))
            && !key(a, 0, 0.0, QString()).sameAs(key(b, 0, 0.0, QString()));
        const bool sameAsOk = clonesApart && uidWins && fallbackSame && fallbackDiffers;

        const auto clearUids = [](TrackClipSnapshot snap) {
            for (auto &track : snap)
                for (auto &id : track) id.uid.clear();
            return snap;
        };
        const auto removeFirst = [](Timeline &tl, int track) {
            auto clips = tl.videoTracks().at(track)->clips();
            clips.removeAt(0);
            tl.videoTracks().at(track)->setClips(clips);
        };
        bool distinct = false, uidless = false, withUid = false, paired = false;
        int uidlessKeys = -1;
        std::string gotUidless = "-", gotWithUid = "-";
        if (mediaReady) {
            Timeline tl;
            seed(tl, path);
            const TrackClipSnapshot before = clearUids(snapshotTrackClips(&tl));
            removeFirst(tl, 1);
            tl.remapCarriersAfterExternalMutation(before);
            distinct = remapped(tl, trackMatteClipKey(1, 0), trackMatteClipKey(1, 1));
        }
        if (mediaReady) {
            Timeline tl;
            seedClones(tl, path, true);
            paired = clonePair(tl);
            attachCarriers(tl, trackMatteClipKey(0, 1));
            const TrackClipSnapshot before = clearUids(snapshotTrackClips(&tl));
            uidlessKeys = 0;
            for (const auto &track : before)
                for (const auto &id : track) uidlessKeys += id.uid.isEmpty() ? 0 : 1;
            removeFirst(tl, 0);
            tl.remapCarriersAfterExternalMutation(before);
            uidless = carriersEmpty(tl);
            gotUidless = showCarriers(tl);
        }
        if (mediaReady) {
            Timeline tl;
            seedClones(tl, path, true);
            paired = paired && clonePair(tl);
            attachCarriers(tl, trackMatteClipKey(0, 1));
            const TrackClipSnapshot before = snapshotTrackClips(&tl);
            removeFirst(tl, 0);
            tl.remapCarriersAfterExternalMutation(before);
            withUid = carriedTo(tl, trackMatteClipKey(0, 0));
            gotWithUid = showCarriers(tl);
        }
        tally.gate(13, mediaReady && sameAsOk && distinct && paired && uidlessKeys == 0
                   && uidless && withUid,
                   "sameAs: clonesApart=" + std::to_string(clonesApart)
                   + " uidWins=" + std::to_string(uidWins)
                   + " fallbackSame=" + std::to_string(fallbackSame)
                   + " fallbackDiffers=" + std::to_string(fallbackDiffers)
                   + " uidless external delete 1:1->1:0 source 1:2->1:1="
                   + std::to_string(distinct)
                   + " clonePair=" + std::to_string(paired)
                   + " uidless snapshot uids=" + std::to_string(uidlessKeys)
                   + " uidless clones want pruned got " + gotUidless
                   + " uid clones want 0:1->0:0 got " + gotWithUid);
    }
    return tally.finish("matte-ssot");
}
