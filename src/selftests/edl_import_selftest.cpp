#include "../EdlImport.h"
#include "../UndoManager.h"
#include "../libavcore/Encode.h"
#include "../libavcore/Probe.h"
#include "SelftestGate.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

namespace {
bool writeSyntheticClip(const QString &path, QString *error)
{
    libavcore::EncodeRequest request;
    request.width = 40;
    request.height = 24;
    request.fps = 10;
    request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoBitrateBits = 600000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (const auto openError = encoder.open(request)) {
        if (error) *error = QString::fromStdString(*openError);
        return false;
    }
    for (int i = 0; i < 30; ++i) {
        QImage frame(40, 24, QImage::Format_RGB888);
        frame.fill(qRgb((i * 7) & 255, 48, 96));
        if (!encoder.pushFrame(frame, i)) {
            if (error) *error = QStringLiteral("pushFrame failed at %1").arg(i);
            return false;
        }
    }
    if (const auto finalizeError = encoder.finalize()) {
        if (error) *error = QString::fromStdString(*finalizeError);
        return false;
    }
    return true;
}
} // namespace

int runEdlImportSelftest()
{
    selftest::GateTally tally;
    auto gate = [&](int n, bool ok) {
        tally.gate(n, ok);
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
        std::fprintf(stderr, "G1 fps=%.9f sourceEvents=%d parsedError=%s "
            "parsedClips=%d exportedEvents=%d roundtrip=%d\n", fps,
            int(original.events.size()), parsed.error.toUtf8().constData(),
            int(parsed.videoTracks[0].size()), int(exported.events.size()), int(roundtrip));
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
    Timeline dissolveTimeline;
    const bool dissolveCreated = dissolveTimeline.createSequence(transitions.document.title,
        transitions.videoTracks, transitions.audioTracks);
    const auto overlaps = dissolveTimeline.videoOverlapIntervals();
    const bool composed = dissolveCreated && !overlaps.isEmpty() && overlaps[0].size() == 2
        && overlaps[0][0].trailOutType == TransitionType::CrossDissolve
        && overlaps[0][1].leadInType == TransitionType::CrossDissolve
        && overlaps[0][0].trailOutDuration > 0.0 && overlaps[0][1].leadInDuration > 0.0
        && std::abs(overlaps[0][0].timelineEnd - 1.5) < 1e-8
        && std::abs(overlaps[0][1].timelineStart - 1.0) < 1e-8;
    std::fprintf(stderr, "G2 error=%s videoClips=%d created=%d composed=%d "
        "sourceOutSec=%.9f sourceDurationSec=%.9f transitionTimelineSec=%.9f "
        "alignment=%d audioClips=%d\n", transitions.error.toUtf8().constData(),
        int(transitions.videoTracks[0].size()), int(dissolveCreated), int(composed),
        transitions.videoTracks[0].isEmpty() ? -1.0 : transitions.videoTracks[0][0].outPoint,
        transitions.videoTracks[0].isEmpty() ? -1.0 : transitions.videoTracks[0][0].duration,
        transitions.videoTracks[0].size() < 2 ? -1.0 : transitions.videoTracks[0][1].leadIn.duration,
        transitions.videoTracks[0].isEmpty() ? -1 : int(transitions.videoTracks[0][0].trailOut.alignment),
        int(transitions.audioTracks[0].size()));
    gate(2, transitions.error.isEmpty() && transitions.videoTracks[0].size() == 2
        && composed && transitions.videoTracks[0][0].outPoint == 1.0
        && transitions.videoTracks[0][0].duration == 1.5
        && transitions.videoTracks[0][0].trailOut.alignment == TransitionAlignment::Start
        && transitions.videoTracks[0][1].leadIn.type == TransitionType::CrossDissolve
        && transitions.videoTracks[0][1].leadIn.duration == 0.5
        && transitions.videoTracks[0][1].inPoint == 2.0
        && transitions.audioTracks[0].size() == 1);

    auto missing = edl::parseEdl(QStringLiteral("001 UNKNOWN V C 00:00:00:00 00:00:01:00 00:00:02:00 00:00:03:00\nM2 unsupported\n"), 24.0);
    edl::resolveClips(missing, dir.path());
    // Use a real video fixture for resolution and the production relink API.
    const QString mediaPath = dir.filePath(QStringLiteral("found.mp4"));
    const bool fixture = QFile::copy(QDir::current().absoluteFilePath(QStringLiteral("test_assets/e2e_clip.mp4")), mediaPath);
    auto found = edl::parseEdl(QStringLiteral("001 LONGSCEN V C 00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"
        "M2 LONGSCEN 048.0 00:00:00:00\n"
        "* UNSUPPORTED COMMENT\n"
        "* FROM CLIP NAME: found.mp4\n"), 24.0);
    edl::resolveClips(found, dir.path());
    Timeline relink;
    bool repaired = fixture && missing.videoTracks[0].size() == 1 && relink.createSequence(missing.document.title, missing.videoTracks, missing.audioTracks);
    if (repaired) repaired = relink.relinkMediaPaths({{missing.videoTracks[0][0].filePath, mediaPath}});
    repaired = repaired && relink.currentState().videoTracks[0][0].filePath == mediaPath;
    qint64 ignored = 0;
    std::fprintf(stderr, "G3 fixture=%d repaired=%d foundResolved=%d foundUnresolved=%d "
        "foundWarnings=%d missingResolved=%d missingUnresolved=%d "
        "missingWarnings=%d\n", int(fixture), int(repaired), found.resolvedFiles,
        found.unresolvedFiles, int(found.warnings.size()), missing.resolvedFiles,
        missing.unresolvedFiles, int(missing.warnings.size()));
    gate(3, repaired && found.resolvedFiles == 1 && found.unresolvedFiles == 0
        && found.error.isEmpty() && found.warnings.size() == 2
        && found.videoTracks[0].size() == 1 && found.videoTracks[0][0].filePath == mediaPath
        && missing.error.isEmpty() && missing.unresolvedFiles == 1 && missing.resolvedFiles == 0
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
    std::fprintf(stderr, "G4 created=%d undo=%d beforeSequences=%d afterRedoSequences=%d "
        "saveSerialBefore=%llu saveSerialAfter=%llu indexBefore=%d indexAfter=%d\n",
        int(created), int(undo), int(count), int(timeline.sequenceList().size()),
        static_cast<unsigned long long>(serial),
        static_cast<unsigned long long>(timeline.undoManager()->saveSerial()), index,
        timeline.undoManager()->currentIndex());
    gate(4, undo);

    // G5 detects loss of the record gap between consecutive imported events.
    auto gap = edl::parseEdl(QStringLiteral(
        "001 A V C 00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"
        "002 B V C 00:00:00:00 00:00:01:00 00:00:02:00 00:00:03:00\n"), 24.0);
    edl::resolveClips(gap, dir.path());
    const bool gapClips = gap.error.isEmpty() && gap.videoTracks[0].size() == 2;
    const double gapTimelineSec = gapClips ? gap.videoTracks[0][1].leadInSec : -1.0;
    std::fprintf(stderr, "G5 timelineGapSec=%.9f clipCount=%d error=%s\n",
        gapTimelineSec, int(gap.videoTracks[0].size()), gap.error.toUtf8().constData());
    gate(5, gapClips && std::abs(gapTimelineSec - 1.0) < 1e-8);

    // G6 detects a source/record duration ratio being dropped during import.
    auto speed = edl::parseEdl(QStringLiteral(
        "001 A V C 00:00:00:00 00:00:02:00 00:00:00:00 00:00:01:00\n"), 24.0);
    edl::resolveClips(speed, dir.path());
    const bool speedClip = speed.error.isEmpty() && speed.videoTracks[0].size() == 1;
    const double speedValue = speedClip ? speed.videoTracks[0][0].speed : -1.0;
    const double speedLocalSec = speedClip ? speed.videoTracks[0][0].effectiveDuration() : -1.0;
    std::fprintf(stderr, "G6 sourceInSec=0 sourceOutSec=2 timelineSec=1 localSec=%.9f speed=%.9f\n",
        speedLocalSec, speedValue);
    gate(6, speedClip && std::abs(speedValue - 2.0) < 1e-8
        && std::abs(speedLocalSec - 1.0) < 1e-8);

    // G7 detects malformed timecodes being admitted as playable events.
    const auto badTime = edl::parseEdl(QStringLiteral(
        "001 A V C 00:00:99:00 00:00:02:00 00:00:00:00 00:00:01:00\n"), 24.0);
    std::fprintf(stderr, "G7 events=%d error=%s\n", int(badTime.document.events.size()),
        badTime.error.toUtf8().constData());
    gate(7, badTime.document.events.isEmpty() && !badTime.error.isEmpty());

    // G8 detects a matching file with an unreadable container being discarded.
    const QString unreadablePath = dir.filePath(QStringLiteral("CASE.MOV"));
    QFile unreadable(unreadablePath);
    const bool wroteUnreadable = unreadable.open(QIODevice::WriteOnly)
        && unreadable.write("not a media container") > 0;
    unreadable.close();
    auto unreadableResult = edl::parseEdl(QStringLiteral(
        "001 CASE V C 00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"), 24.0);
    edl::resolveClips(unreadableResult, dir.path());
    const bool hasUnreadableClip = unreadableResult.error.isEmpty()
        && unreadableResult.videoTracks[0].size() == 1;
    const double unreadableDurationSec = hasUnreadableClip
        ? unreadableResult.videoTracks[0][0].duration : -1.0;
    std::fprintf(stderr, "G8 wrote=%d resolved=%d durationSec=%.9f outSec=1\n",
        int(wroteUnreadable), unreadableResult.resolvedFiles, unreadableDurationSec);
    gate(8, wroteUnreadable && hasUnreadableClip && unreadableResult.resolvedFiles == 1
        && std::abs(unreadableDurationSec - 1.0) < 1e-8);

    // G9 detects audio and video record positions being combined into one track.
    auto separate = edl::parseEdl(QStringLiteral(
        "001 V1 V C 00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"
        "002 A1 A C 00:00:00:00 00:00:01:00 00:00:02:00 00:00:03:00\n"), 24.0);
    edl::resolveClips(separate, dir.path());
    const bool separateClips = separate.error.isEmpty()
        && separate.videoTracks[0].size() == 1 && separate.audioTracks[0].size() == 1;
    const double audioTimelineSec = separateClips ? separate.audioTracks[0][0].leadInSec : -1.0;
    std::fprintf(stderr, "G9 videoCount=%d audioCount=%d audioTimelineSec=%.9f\n",
        int(separate.videoTracks[0].size()), int(separate.audioTracks[0].size()), audioTimelineSec);
    gate(9, separateClips && std::abs(audioTimelineSec - 2.0) < 1e-8);

    const QString syntheticPath = dir.filePath(QStringLiteral("synthetic.mp4"));
    QString encodeError;
    const bool encoderAvailable = selftest::requireEncoder(QStringLiteral("mpeg4"));
    const bool syntheticReady = encoderAvailable && writeSyntheticClip(syntheticPath, &encodeError);
    const QString selectionEdl = QStringLiteral(
        "001 SYNTH V C 00:00:00:05 00:00:01:05 00:00:00:00 00:00:01:00\n"
        "* FROM CLIP NAME: synthetic.mp4\n");
    if (!encoderAvailable) {
        tally.skip(10, "mpeg4 encoder unavailable");
    } else {
        auto selected = edl::parseEdl(selectionEdl, 10.0);
        edl::resolveClips(selected, dir.path());
        const bool selectedClip = selected.error.isEmpty() && selected.videoTracks[0].size() == 1;
        const double sourceDurationSec = selectedClip ? selected.videoTracks[0][0].duration : -1.0;
        const double sourceInSec = selectedClip ? selected.videoTracks[0][0].inPoint : -1.0;
        const double sourceOutSec = selectedClip ? selected.videoTracks[0][0].outPoint : -1.0;
        const double localSec = selectedClip ? selected.videoTracks[0][0].effectiveDuration() : -1.0;
        std::fprintf(stderr, "G10 encoded=%d encodeError=%s durationSourceSec=%.9f "
            "inSourceSec=%.9f outSourceSec=%.9f localSec=%.9f timelineSec=0\n",
            int(syntheticReady), encodeError.toUtf8().constData(), sourceDurationSec,
            sourceInSec, sourceOutSec, localSec);
        gate(10, syntheticReady && selectedClip && selected.resolvedFiles == 1
            && std::abs(sourceDurationSec - 3.0) < 0.15
            && std::abs(sourceInSec - 0.5) < 1e-8
            && std::abs(sourceOutSec - 1.5) < 1e-8
            && std::abs(localSec - 1.0) < 1e-8);
    }

    auto offline = edl::parseEdl(QStringLiteral(
        "001 OFFLINE V C 00:00:00:05 00:00:01:05 00:00:00:00 00:00:01:00\n"), 10.0);
    edl::resolveClips(offline, dir.path());
    const bool offlineClip = offline.error.isEmpty() && offline.videoTracks[0].size() == 1;
    const double offlineDurationSec = offlineClip ? offline.videoTracks[0][0].duration : -1.0;
    const double offlineOutSec = offlineClip ? offline.videoTracks[0][0].outPoint : -1.0;
    std::fprintf(stderr, "G11 sourceDurationSec=%.9f sourceOutSec=%.9f unresolved=%d\n",
        offlineDurationSec, offlineOutSec, offline.unresolvedFiles);
    gate(11, offlineClip && offline.unresolvedFiles == 1
        && std::abs(offlineDurationSec - offlineOutSec) < 1e-8
        && std::abs(offlineOutSec - 1.5) < 1e-8);

    if (!encoderAvailable) {
        tally.skip(12, "mpeg4 encoder unavailable");
    } else {
        auto onlineDissolve = edl::parseEdl(QStringLiteral(
            "001 SYNTH V C 00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"
            "* FROM CLIP NAME: synthetic.mp4\n"
            "002 SYNTH V D 005 00:00:01:00 00:00:02:00 00:00:01:00 00:00:02:00\n"
            "* FROM CLIP NAME: synthetic.mp4\n"), 10.0);
        edl::resolveClips(onlineDissolve, dir.path());
        const bool pair = onlineDissolve.error.isEmpty() && onlineDissolve.videoTracks[0].size() == 2;
        const double sourceDurationSec = pair ? onlineDissolve.videoTracks[0][0].duration : -1.0;
        const double sourceOutSec = pair ? onlineDissolve.videoTracks[0][0].outPoint : -1.0;
        const double trailSourceSec = sourceDurationSec - sourceOutSec;
        const double transitionTimelineSec = pair ? onlineDissolve.videoTracks[0][0].trailOut.duration : -1.0;
        std::fprintf(stderr, "G12 encoded=%d durationSourceSec=%.9f outSourceSec=%.9f "
            "trailSourceSec=%.9f transitionTimelineSec=%.9f localSec=1\n",
            int(syntheticReady), sourceDurationSec, sourceOutSec, trailSourceSec, transitionTimelineSec);
        gate(12, syntheticReady && pair && onlineDissolve.resolvedFiles == 1
            && std::abs(sourceDurationSec - 3.0) < 0.15 && trailSourceSec > 1.0
            && std::abs(transitionTimelineSec - 0.5) < 1e-8
            && onlineDissolve.videoTracks[0][0].trailOut.alignment == TransitionAlignment::Start);
    }

    // G13 detects a probed file length shorter than the frame-quantised source out.
    if (!encoderAvailable) {
        tally.skip(13, "mpeg4 encoder unavailable");
    } else {
        const auto probedUs = libavcore::probeDurationMicroseconds(
            syntheticPath.toUtf8().toStdString());
        const double probedSourceSec = probedUs ? double(*probedUs) / 1000000.0 : -1.0;
        auto shortMedia = edl::parseEdl(QStringLiteral(
            "001 SYNTH V C 00:00:02:05 00:00:03:05 00:00:00:00 00:00:01:00\n"
            "* FROM CLIP NAME: synthetic.mp4\n"), 10.0);
        edl::resolveClips(shortMedia, dir.path());
        const bool shortMediaClip = shortMedia.error.isEmpty()
            && shortMedia.videoTracks[0].size() == 1;
        const double shortSourceDurationSec = shortMediaClip
            ? shortMedia.videoTracks[0][0].duration : -1.0;
        const double shortSourceInSec = shortMediaClip
            ? shortMedia.videoTracks[0][0].inPoint : -1.0;
        const double shortSourceOutSec = shortMediaClip
            ? shortMedia.videoTracks[0][0].outPoint : -1.0;
        const double shortLocalSec = shortMediaClip
            ? shortMedia.videoTracks[0][0].effectiveDuration() : -1.0;
        std::fprintf(stderr, "G13 encoded=%d encodeError=%s probedSourceSec=%.9f "
            "sourceDurationSec=%.9f inSourceSec=%.9f outSourceSec=%.9f "
            "localSec=%.9f timelineSec=0 resolved=%d\n",
            int(syntheticReady), encodeError.toUtf8().constData(), probedSourceSec,
            shortSourceDurationSec,
            shortSourceInSec, shortSourceOutSec, shortLocalSec, shortMedia.resolvedFiles);
        gate(13, syntheticReady && shortMediaClip && shortMedia.resolvedFiles == 1
            && probedSourceSec > 2.8 && probedSourceSec < shortSourceOutSec
            && std::abs(shortSourceDurationSec - 3.5) < 1e-8
            && std::abs(shortSourceInSec - 2.5) < 1e-8
            && std::abs(shortSourceOutSec - 3.5) < 1e-8
            && std::abs(shortLocalSec - 1.0) < 1e-8);
    }
    return tally.finish();
}
