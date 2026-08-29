// --selftest=reverse-clip

#include "../AudioMixer.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kWidth = 48;
constexpr int kHeight = 32;
constexpr int kFps = 10;
constexpr int kFrameCount = 10;

bool near(double left, double right, double epsilon = 1e-9)
{
    return std::abs(left - right) <= epsilon;
}

QImage syntheticFrame(int frameIndex)
{
    QImage frame(kWidth, kHeight, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            frame.setPixelColor(
                x, y,
                QColor((x * 5 + frameIndex * 19) & 255,
                       (y * 7 + frameIndex * 31) & 255,
                       (x + y * 3 + frameIndex * 43) & 255));
        }
    }
    return frame;
}

bool writeSyntheticClip(const QString &path, QString *error)
{
    libavcore::EncodeRequest request;
    request.width = kWidth;
    request.height = kHeight;
    request.fps = kFps;
    request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 800000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (const auto openError = encoder.open(request)) {
        if (error)
            *error = QString::fromStdString(*openError);
        return false;
    }
    for (int i = 0; i < kFrameCount; ++i) {
        if (!encoder.pushFrame(syntheticFrame(i), i)) {
            if (error)
                *error = QStringLiteral("frame %1 encode failed").arg(i);
            return false;
        }
    }
    if (const auto finalizeError = encoder.finalize()) {
        if (error)
            *error = QString::fromStdString(*finalizeError);
        return false;
    }
    return true;
}

bool equalPixels(const QImage &left, const QImage &right)
{
    const QImage a = left.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = right.convertToFormat(QImage::Format_RGBA8888);
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return false;
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<std::size_t>(a.width() * 4)) != 0) {
            return false;
        }
    }
    return true;
}

QJsonObject projectClipAt(const QString &json, int clipIndex)
{
    const QJsonArray tracks = QJsonDocument::fromJson(json.toUtf8())
                                  .object()
                                  .value(QStringLiteral("videoTracks"))
                                  .toArray();
    if (tracks.isEmpty())
        return {};
    const QJsonArray clips = tracks.first().toArray();
    if (clipIndex < 0 || clipIndex >= clips.size())
        return {};
    return clips.at(clipIndex).toObject();
}

} // namespace

int runReverseClipSelftest()
{
    int passed = 0;
    int failed = 0;
    const auto check = [&](int gate, const char *description, bool ok,
                           const QString &detail = QString()) {
        const QByteArray suffix = detail.isEmpty()
            ? QByteArray()
            : QByteArray(" — ") + detail.toUtf8();
        std::fprintf(stderr, "%s G%d %s%s\n", ok ? "PASS" : "FAIL",
                     gate, description, suffix.constData());
        ok ? ++passed : ++failed;
    };

    ClipInfo reversed;
    reversed.duration = 8.0;
    reversed.inPoint = 2.0;
    reversed.outPoint = 8.0;
    reversed.speed = 1.0;
    reversed.reversed = true;
    check(1, "逆再生の始端と終端を source out/in に写像する",
          near(reversed.sourceSecondAtLocalTime(0.0), 8.0)
              && near(reversed.sourceSecondAtLocalTime(6.0), 2.0));

    reversed.speed = 2.0;
    const double fast0 = reversed.sourceSecondAtLocalTime(0.0);
    const double fast1 = reversed.sourceSecondAtLocalTime(0.5);
    const double fast2 = reversed.sourceSecondAtLocalTime(1.0);
    const double fastEnd = reversed.sourceSecondAtLocalTime(3.0);
    ClipInfo foldedDescending = reversed;
    foldedDescending.speed = 1.0;
    foldedDescending.timeRemapCurve.addKey(0.0, 6.0);
    foldedDescending.timeRemapCurve.addKey(6.0, 0.0);
    const bool descendingRemapFoldedForward =
        near(foldedDescending.sourceSecondAtLocalTime(0.0), 2.0)
        && near(foldedDescending.sourceSecondAtLocalTime(1.0), 3.0)
        && !foldedDescending.sourceTimeRunsBackwardAtLocalTime(1.0);
    check(2, "speed=2 と降順 timeRemap を正しい向きで合成する",
          fast0 > fast1 && fast1 > fast2 && fast2 > fastEnd
              && near(fastEnd, 2.0) && descendingRemapFoldedForward);

    ClipInfo normal = reversed;
    normal.reversed = false;
    const bool legacyMapping = near(normal.sourceSecondAtLocalTime(0.0), 2.0)
        && near(normal.sourceSecondAtLocalTime(0.5), 3.0)
        && near(normal.sourceSecondAtLocalTime(2.5), 7.0);
    ClipInfo persistedReverse = normal;
    persistedReverse.reversed = true;
    ProjectData saved;
    saved.videoTracks = QVector<QVector<ClipInfo>>{
        QVector<ClipInfo>{normal, persistedReverse}
    };
    const QString json = ProjectFile::toJsonString(saved);
    ProjectData loaded;
    const bool loadedOk = ProjectFile::fromJsonString(json, loaded);
    const bool persistenceOk = !projectClipAt(json, 0).contains(
                                    QStringLiteral("reversed"))
        && projectClipAt(json, 1).value(QStringLiteral("reversed")).toBool()
        && loadedOk && loaded.videoTracks.size() == 1
        && loaded.videoTracks.first().size() == 2
        && !loaded.videoTracks.first().at(0).reversed
        && loaded.videoTracks.first().at(1).reversed;
    check(3, "非逆再生は既存写像と同一で既定値を保存しない",
          legacyMapping && persistenceOk);

    const QVector<float> reversedPcm = reversedPcmFrames(
        QVector<float>{1.0f, 2.0f, 3.0f, 4.0f});
    const QVector<float> reversedStereoPcm = reversedPcmFrames(
        QVector<float>{1.0f, 2.0f, 3.0f, 4.0f}, 2);
    const QString forwardAudioFilter = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("0"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, false);
    const QString reversedAudioFilter = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("0"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, true);

    ClipInfo nestedAudioA;
    nestedAudioA.filePath = QStringLiteral("nested-a.wav");
    nestedAudioA.displayName = QStringLiteral("nested A");
    nestedAudioA.duration = 2.0;
    nestedAudioA.outPoint = 2.0;
    nestedAudioA.reversed = true;
    ClipInfo nestedAudioB = nestedAudioA;
    nestedAudioB.filePath = QStringLiteral("nested-b.wav");
    nestedAudioB.displayName = QStringLiteral("nested B");
    nestedAudioB.reversed = false;
    TimelineSequence audioLeaf;
    audioLeaf.id = QStringLiteral("reverse-audio-leaf");
    audioLeaf.name = audioLeaf.id;
    audioLeaf.audioTracks = {{nestedAudioA, nestedAudioB}};
    ClipInfo audioSequenceRef;
    audioSequenceRef.sequenceRefId = audioLeaf.id;
    audioSequenceRef.filePath = timeline_nesting::sequenceClipFilePath(
        audioLeaf.id);
    audioSequenceRef.displayName = QStringLiteral("nested audio");
    audioSequenceRef.duration = 4.0;
    audioSequenceRef.outPoint = 4.0;
    TimelineSequence audioMain;
    audioMain.id = QStringLiteral("reverse-audio-main");
    audioMain.name = audioMain.id;
    audioMain.audioTracks = {{audioSequenceRef}};
    Timeline nestedAudioTimeline;
    nestedAudioTimeline.setSequences(
        QVector<TimelineSequence>{audioMain, audioLeaf}, audioMain.id);
    const QVector<PlaybackEntry> childReverseEntries =
        nestedAudioTimeline.computeAudioPlaybackSequence();
    const QVector<bool> childReverseFlags =
        audioReversedFlagsForPlaybackEntries(childReverseEntries);
    const bool childReverseOk = childReverseEntries.size() == 2
        && childReverseFlags.size() == 2
        && childReverseEntries[0].filePath == nestedAudioA.filePath
        && childReverseFlags[0]
        && childReverseEntries[1].filePath == nestedAudioB.filePath
        && !childReverseFlags[1];

    audioSequenceRef.reversed = true;
    audioMain.audioTracks = {{audioSequenceRef}};
    Timeline parentReverseAudioTimeline;
    parentReverseAudioTimeline.setSequences(
        QVector<TimelineSequence>{audioMain, audioLeaf}, audioMain.id);
    const QVector<PlaybackEntry> parentReverseEntries =
        parentReverseAudioTimeline.computeAudioPlaybackSequence();
    const QVector<bool> parentReverseFlags =
        audioReversedFlagsForPlaybackEntries(parentReverseEntries);
    const bool parentReverseOk = parentReverseEntries.size() == 2
        && parentReverseFlags.size() == 2
        && parentReverseEntries[0].filePath == nestedAudioB.filePath
        && parentReverseFlags[0]
        && parentReverseEntries[1].filePath == nestedAudioA.filePath
        && !parentReverseFlags[1];

    ClipInfo foldedParentAudioRef = audioSequenceRef;
    foldedParentAudioRef.timeRemapCurve.addKey(0.0, 4.0);
    foldedParentAudioRef.timeRemapCurve.addKey(4.0, 0.0);
    audioMain.audioTracks = {{foldedParentAudioRef}};
    Timeline foldedParentAudioTimeline;
    foldedParentAudioTimeline.setSequences(
        QVector<TimelineSequence>{audioMain, audioLeaf}, audioMain.id);
    const QVector<PlaybackEntry> foldedParentAudioEntries =
        foldedParentAudioTimeline.computeAudioPlaybackSequence();
    const QVector<bool> foldedParentAudioFlags =
        audioReversedFlagsForPlaybackEntries(foldedParentAudioEntries);
    const bool foldedParentAudioOk = foldedParentAudioEntries.size() == 2
        && foldedParentAudioFlags.size() == 2
        && foldedParentAudioEntries[0].filePath == nestedAudioA.filePath
        && near(foldedParentAudioEntries[0].timelineStart, 0.0)
        && foldedParentAudioFlags[0]
        && foldedParentAudioEntries[1].filePath == nestedAudioB.filePath
        && near(foldedParentAudioEntries[1].timelineStart, 2.0)
        && !foldedParentAudioFlags[1];

    ClipInfo remappedForwardParent = audioSequenceRef;
    remappedForwardParent.reversed = false;
    remappedForwardParent.timeRemapCurve.keys.clear();
    remappedForwardParent.timeRemapCurve.addKey(0.0, 4.0);
    remappedForwardParent.timeRemapCurve.addKey(4.0, 0.0);
    audioMain.audioTracks = {{remappedForwardParent}};
    Timeline remappedForwardParentTimeline;
    remappedForwardParentTimeline.setSequences(
        QVector<TimelineSequence>{audioMain, audioLeaf}, audioMain.id);
    const QVector<PlaybackEntry> remappedForwardEntries =
        remappedForwardParentTimeline.computeAudioPlaybackSequence();
    const QVector<bool> remappedForwardFlags =
        audioReversedFlagsForPlaybackEntries(remappedForwardEntries);
    const bool remappedForwardParentOk = remappedForwardEntries.size() == 2
        && remappedForwardFlags.size() == 2
        && remappedForwardEntries[0].filePath == nestedAudioB.filePath
        && near(remappedForwardEntries[0].timelineStart, 0.0)
        && remappedForwardFlags[0]
        && remappedForwardEntries[1].filePath == nestedAudioA.filePath
        && near(remappedForwardEntries[1].timelineStart, 2.0)
        && !remappedForwardFlags[1];

    ClipInfo rampedForwardParent = audioSequenceRef;
    rampedForwardParent.reversed = false;
    rampedForwardParent.timeRemapCurve.keys.clear();
    rampedForwardParent.speedRamp.addKeyframe(0, 2.0);
    audioMain.audioTracks = {{rampedForwardParent}};
    Timeline rampedForwardParentTimeline;
    rampedForwardParentTimeline.setSequences(
        QVector<TimelineSequence>{audioMain, audioLeaf}, audioMain.id);
    const QVector<PlaybackEntry> rampedForwardEntries =
        rampedForwardParentTimeline.computeAudioPlaybackSequence();
    const QVector<bool> rampedForwardFlags =
        audioReversedFlagsForPlaybackEntries(rampedForwardEntries);
    const bool rampedForwardParentOk = rampedForwardEntries.size() == 2
        && rampedForwardFlags.size() == 2
        && rampedForwardEntries[0].filePath == nestedAudioA.filePath
        && near(rampedForwardEntries[0].timelineStart, 0.0)
        && near(rampedForwardEntries[0].timelineEnd, 1.0)
        && rampedForwardFlags[0]
        && rampedForwardEntries[1].filePath == nestedAudioB.filePath
        && near(rampedForwardEntries[1].timelineStart, 1.0)
        && near(rampedForwardEntries[1].timelineEnd, 2.0)
        && !rampedForwardFlags[1];

    ClipInfo parallelReverse = nestedAudioA;
    parallelReverse.filePath = QStringLiteral("parallel-duplicate.wav");
    parallelReverse.volume = 0.75;
    ClipInfo parallelForward = parallelReverse;
    parallelForward.reversed = false;
    parallelForward.volume = 1.25;
    TimelineSequence parallelLeaf;
    parallelLeaf.id = QStringLiteral("reverse-audio-parallel-leaf");
    parallelLeaf.name = parallelLeaf.id;
    parallelLeaf.audioTracks = {{parallelReverse}, {parallelForward}};
    ClipInfo parallelSequenceRef;
    parallelSequenceRef.sequenceRefId = parallelLeaf.id;
    parallelSequenceRef.filePath = timeline_nesting::sequenceClipFilePath(
        parallelLeaf.id);
    parallelSequenceRef.displayName = QStringLiteral("parallel audio");
    parallelSequenceRef.duration = 2.0;
    parallelSequenceRef.outPoint = 2.0;
    TimelineSequence parallelMain;
    parallelMain.id = QStringLiteral("reverse-audio-parallel-main");
    parallelMain.name = parallelMain.id;
    parallelMain.audioTracks = {{parallelSequenceRef}};
    Timeline parallelAudioTimeline;
    parallelAudioTimeline.setSequences(
        QVector<TimelineSequence>{parallelMain, parallelLeaf},
        parallelMain.id);
    const QVector<PlaybackEntry> parallelEntries =
        parallelAudioTimeline.computeAudioPlaybackSequence();
    const QVector<bool> parallelFlags =
        audioReversedFlagsForPlaybackEntries(parallelEntries);
    bool parallelReverseOk = parallelEntries.size() == 2
        && parallelFlags.size() == 2;
    for (int i = 0; parallelReverseOk && i < parallelEntries.size(); ++i) {
        const bool expected = qFuzzyCompare(
            parallelEntries[i].volume + 1.0,
            parallelReverse.volume + 1.0);
        parallelReverseOk = parallelFlags[i] == expected;
    }
    check(4, "PCM バッファをフレーム順に反転する",
          reversedPcm == QVector<float>{4.0f, 3.0f, 2.0f, 1.0f}
              && reversedStereoPcm
                     == QVector<float>{3.0f, 4.0f, 1.0f, 2.0f}
              && !forwardAudioFilter.contains(QStringLiteral("areverse"))
              && reversedAudioFilter.contains(
                     QStringLiteral("atrim=start=0:end=4,areverse,asetpts"))
              && childReverseOk && parentReverseOk
              && foldedParentAudioOk && remappedForwardParentOk
              && rampedForwardParentOk && parallelReverseOk);

    bool rendererOk = QApplication::instance() != nullptr;
    QString rendererDetail;
    QTemporaryDir temporaryDirectory;
    rendererOk = rendererOk && temporaryDirectory.isValid();
    const QString mediaPath = temporaryDirectory.filePath(
        QStringLiteral("reverse-clip-gradient.mp4"));
    if (rendererOk)
        rendererOk = writeSyntheticClip(mediaPath, &rendererDetail);
    if (rendererOk) {
        ClipInfo renderClip;
        renderClip.filePath = mediaPath;
        renderClip.displayName = QStringLiteral("reverse selftest");
        renderClip.duration = static_cast<double>(kFrameCount) / kFps;
        renderClip.inPoint = 0.0;
        renderClip.outPoint = renderClip.duration;
        renderClip.speed = 1.0;

        Timeline forwardTimeline;
        forwardTimeline.videoTracks().first()->setClips({renderClip});
        forwardTimeline.refreshPlaybackSequence();

        renderClip.reversed = true;
        Timeline reverseTimeline;
        reverseTimeline.videoTracks().first()->setClips({renderClip});
        reverseTimeline.refreshPlaybackSequence();

        const qint64 finalFrameTimelineUs = qRound64(
            (renderClip.duration - 1.0 / kFps) * 1'000'000.0);
        const QImage forwardEnd = tlrender::renderFrameAt(
            &forwardTimeline, finalFrameTimelineUs, QSize(kWidth, kHeight));
        const QImage reverseStart = tlrender::renderFrameAt(
            &reverseTimeline, 0, QSize(kWidth, kHeight));
        const qint64 reverseSecondFrameUs = qRound64(
            (1.0 / kFps) * 1'000'000.0);
        const qint64 forwardPenultimateFrameUs = qRound64(
            (renderClip.duration - 2.0 / kFps) * 1'000'000.0);
        const QImage forwardPenultimate = tlrender::renderFrameAt(
            &forwardTimeline, forwardPenultimateFrameUs,
            QSize(kWidth, kHeight));
        const QImage reverseSecond = tlrender::renderFrameAt(
            &reverseTimeline, reverseSecondFrameUs,
            QSize(kWidth, kHeight));
        const QImage forwardSecond = tlrender::renderFrameAt(
            &forwardTimeline, reverseSecondFrameUs,
            QSize(kWidth, kHeight));

        ClipInfo foldedRemapRenderClip = renderClip;
        foldedRemapRenderClip.timeRemapCurve.addKey(
            0.0, foldedRemapRenderClip.duration);
        foldedRemapRenderClip.timeRemapCurve.addKey(
            foldedRemapRenderClip.duration, 0.0);
        Timeline foldedRemapTimeline;
        foldedRemapTimeline.videoTracks().first()->setClips(
            {foldedRemapRenderClip});
        foldedRemapTimeline.refreshPlaybackSequence();
        const QImage foldedRemapSecond = tlrender::renderFrameAt(
            &foldedRemapTimeline, reverseSecondFrameUs,
            QSize(kWidth, kHeight));

        TimelineSequence leafSequence;
        leafSequence.id = QStringLiteral("reverse-leaf");
        leafSequence.name = QStringLiteral("reverse-leaf");
        ClipInfo nestedLeafClip = renderClip;
        nestedLeafClip.reversed = false;
        leafSequence.videoTracks = {{nestedLeafClip}};
        ClipInfo reversedSequenceRef;
        reversedSequenceRef.sequenceRefId = leafSequence.id;
        reversedSequenceRef.filePath = timeline_nesting::sequenceClipFilePath(
            leafSequence.id);
        reversedSequenceRef.displayName = QStringLiteral("reverse sequence");
        reversedSequenceRef.duration = renderClip.duration;
        reversedSequenceRef.outPoint = renderClip.duration;
        reversedSequenceRef.reversed = true;
        TimelineSequence mainSequence;
        mainSequence.id = QStringLiteral("reverse-main");
        mainSequence.name = QStringLiteral("reverse-main");
        mainSequence.videoTracks = {{reversedSequenceRef}};
        Timeline nestedReverseTimeline;
        nestedReverseTimeline.setSequences(
            QVector<TimelineSequence>{mainSequence, leafSequence},
            mainSequence.id);
        const QVector<PlaybackEntry> nestedReverseEntries =
            nestedReverseTimeline.computePlaybackSequence();
        const bool nestedReverseBindingOk = nestedReverseEntries.size() == 1
            && videoReversedForPlaybackEntry(nestedReverseEntries.first());
        const QImage nestedReverseStart = tlrender::renderFrameAt(
            &nestedReverseTimeline, 0, QSize(kWidth, kHeight));
        const QImage nestedReverseSecond = tlrender::renderFrameAt(
            &nestedReverseTimeline, reverseSecondFrameUs,
            QSize(kWidth, kHeight));

        TimelineSequence rampedLeafSequence = leafSequence;
        rampedLeafSequence.id = QStringLiteral("reverse-ramped-leaf");
        rampedLeafSequence.name = rampedLeafSequence.id;
        rampedLeafSequence.videoTracks[0][0].reversed = true;
        ClipInfo rampedSequenceRef = reversedSequenceRef;
        rampedSequenceRef.sequenceRefId = rampedLeafSequence.id;
        rampedSequenceRef.filePath = timeline_nesting::sequenceClipFilePath(
            rampedLeafSequence.id);
        rampedSequenceRef.reversed = false;
        rampedSequenceRef.speedRamp.addKeyframe(0, 2.0);
        TimelineSequence rampedMainSequence;
        rampedMainSequence.id = QStringLiteral("reverse-ramped-main");
        rampedMainSequence.name = rampedMainSequence.id;
        rampedMainSequence.videoTracks = {{rampedSequenceRef}};
        Timeline rampedNestedTimeline;
        rampedNestedTimeline.setSequences(
            QVector<TimelineSequence>{rampedMainSequence,
                                      rampedLeafSequence},
            rampedMainSequence.id);
        const QImage rampedNestedStart = tlrender::renderFrameAt(
            &rampedNestedTimeline, 0, QSize(kWidth, kHeight));
        const QImage rampedNestedSecond = tlrender::renderFrameAt(
            &rampedNestedTimeline, reverseSecondFrameUs,
            QSize(kWidth, kHeight));
        const QImage forwardAtSevenTenths = tlrender::renderFrameAt(
            &forwardTimeline, 700'000, QSize(kWidth, kHeight));

        ClipInfo foldedSequenceRef = reversedSequenceRef;
        foldedSequenceRef.timeRemapCurve.addKey(
            0.0, foldedSequenceRef.duration);
        foldedSequenceRef.timeRemapCurve.addKey(
            foldedSequenceRef.duration, 0.0);
        mainSequence.videoTracks = {{foldedSequenceRef}};
        Timeline foldedParentVideoTimeline;
        foldedParentVideoTimeline.setSequences(
            QVector<TimelineSequence>{mainSequence, leafSequence},
            mainSequence.id);
        const QVector<PlaybackEntry> foldedParentVideoEntries =
            foldedParentVideoTimeline.computePlaybackSequence();
        const QImage foldedParentVideoStart = tlrender::renderFrameAt(
            &foldedParentVideoTimeline, 0, QSize(kWidth, kHeight));
        const QImage forwardStart = tlrender::renderFrameAt(
            &forwardTimeline, 0, QSize(kWidth, kHeight));
        const QImage foldedParentVideoSecond = tlrender::renderFrameAt(
            &foldedParentVideoTimeline, reverseSecondFrameUs,
            QSize(kWidth, kHeight));
        rendererOk = !forwardEnd.isNull() && !reverseStart.isNull()
            && !forwardPenultimate.isNull() && !reverseSecond.isNull()
            && !forwardSecond.isNull() && !foldedRemapSecond.isNull()
            && !nestedReverseStart.isNull()
            && !nestedReverseSecond.isNull()
            && !rampedNestedStart.isNull()
            && !rampedNestedSecond.isNull()
            && !forwardAtSevenTenths.isNull()
            && !foldedParentVideoStart.isNull()
            && !foldedParentVideoSecond.isNull()
            && nestedReverseBindingOk
            && foldedParentVideoEntries.size() == 1
            && !videoReversedForPlaybackEntry(
                foldedParentVideoEntries.first())
            && equalPixels(forwardEnd, reverseStart)
            && equalPixels(forwardPenultimate, reverseSecond)
            && equalPixels(forwardSecond, foldedRemapSecond)
            && equalPixels(forwardEnd, nestedReverseStart)
            && equalPixels(forwardPenultimate, nestedReverseSecond)
            && equalPixels(forwardEnd, rampedNestedStart)
            && equalPixels(forwardAtSevenTenths, rampedNestedSecond)
            && equalPixels(forwardStart, foldedParentVideoStart)
            && equalPixels(forwardSecond, foldedParentVideoSecond);
        if (!rendererOk && rendererDetail.isEmpty()) {
            rendererDetail = QStringLiteral(
                "forwardEndNull=%1 reverseStartNull=%2 "
                "forwardPenultimateNull=%3 reverseSecondNull=%4 "
                "forwardSecondNull=%5 foldedRemapSecondNull=%6 "
                "nestedStartNull=%7 nestedSecondNull=%8 "
                "rampedNestedStartNull=%9 rampedNestedSecondNull=%10 "
                "forwardSevenNull=%11 foldedParentStartNull=%12 "
                "foldedParentSecondNull=%13")
                .arg(forwardEnd.isNull()).arg(reverseStart.isNull())
                .arg(forwardPenultimate.isNull()).arg(reverseSecond.isNull())
                .arg(forwardSecond.isNull()).arg(foldedRemapSecond.isNull())
                .arg(nestedReverseStart.isNull())
                .arg(nestedReverseSecond.isNull())
                .arg(rampedNestedStart.isNull())
                .arg(rampedNestedSecond.isNull())
                .arg(forwardAtSevenTenths.isNull())
                .arg(foldedParentVideoStart.isNull())
                .arg(foldedParentVideoSecond.isNull());
        }
    }
    check(5, "TimelineFrameRenderer の逆再生始端が通常再生の終端フレームと一致する",
          rendererOk, rendererDetail);

    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
