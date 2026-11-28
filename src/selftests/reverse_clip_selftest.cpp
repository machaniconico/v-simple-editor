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
#include <cstdint>
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
    const QString reversedSpeed2AudioFilter =
        buildExportAudioMixEntryFilterChain(
            0, QStringLiteral("0"), QStringLiteral("4"), 0,
            QStringLiteral("1"), AudioChannelMode::Stereo, true, 2.0);
    const QString forwardSpeed2AudioFilter =
        buildExportAudioMixEntryFilterChain(
            0, QStringLiteral("0"), QStringLiteral("4"), 0,
            QStringLiteral("1"), AudioChannelMode::Stereo, false, 2.0);
    const QString reversedBelowHalfAudioFilter =
        buildExportAudioMixEntryFilterChain(
            0, QStringLiteral("0"), QStringLiteral("4"), 0,
            QStringLiteral("1"), AudioChannelMode::Stereo, true,
            0.4999999995);

    // This is the same staging primitive MixerIODevice uses to consume the
    // whole-clip reversed PCM buffer. Verify both identity-speed consumption
    // and the speed=2 path that previously advanced ringHead at only 1x.
    const std::int16_t reversedStereoInput[] = {
        4, 40, 3, 30, 2, 20, 1, 10
    };
    std::int16_t stagedSpeed1[8] = {};
    int consumedSpeed1 = 0;
    double phaseSpeed1 = -1.0;
    const int framesSpeed1 = stageInterleavedPcmFramesAtSpeed(
        reversedStereoInput, 4, 2, 1.0, 0.0,
        stagedSpeed1, 4, &consumedSpeed1, &phaseSpeed1);
    std::int16_t stagedSpeed2[8] = {};
    int consumedSpeed2 = 0;
    double phaseSpeed2 = -1.0;
    const int framesSpeed2 = stageInterleavedPcmFramesAtSpeed(
        reversedStereoInput, 4, 2, 2.0, 0.0,
        stagedSpeed2, 4, &consumedSpeed2, &phaseSpeed2);
    const bool previewSpeed1Ok = framesSpeed1 == 4
        && consumedSpeed1 == 4 && near(phaseSpeed1, 0.0)
        && stagedSpeed1[0] == 4 && stagedSpeed1[1] == 40
        && stagedSpeed1[2] == 3 && stagedSpeed1[3] == 30
        && stagedSpeed1[4] == 2 && stagedSpeed1[5] == 20
        && stagedSpeed1[6] == 1 && stagedSpeed1[7] == 10;
    const bool previewSpeed2Ok = framesSpeed2 == 2
        && consumedSpeed2 == 4 && near(phaseSpeed2, 0.0)
        && stagedSpeed2[0] == 4 && stagedSpeed2[1] == 40
        && stagedSpeed2[2] == 2 && stagedSpeed2[3] == 20;

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
    check(4, "AudioMixer の逆順 PCM を speed=1/2 で正しく消費する",
          reversedPcm == QVector<float>{4.0f, 3.0f, 2.0f, 1.0f}
              && reversedStereoPcm
                     == QVector<float>{3.0f, 4.0f, 1.0f, 2.0f}
              && previewSpeed1Ok && previewSpeed2Ok
              && !forwardAudioFilter.contains(QStringLiteral("areverse"))
              && !forwardSpeed2AudioFilter.contains(QStringLiteral("atempo"))
              && reversedAudioFilter.contains(
                     QStringLiteral("atrim=start=0:end=4,areverse,asetpts"))
              && reversedSpeed2AudioFilter.contains(
                     QStringLiteral(
                         "atrim=start=0:end=4,areverse,atempo=2,asetpts"))
              && reversedBelowHalfAudioFilter.contains(
                     QStringLiteral("areverse,atempo=0.5,atempo=0.999999999"))
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

        // The clip end is exclusive. Sample one microsecond before the final
        // frame's nominal PTS boundary so the forward decoder's at-or-after
        // policy selects that frame without seeking exactly to encoded EOF.
        const qint64 finalFrameTimelineUs = qRound64(
            (renderClip.duration - 1.0 / kFps) * 1'000'000.0) - 1;
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

    ClipInfo heldLeaf;
    heldLeaf.filePath = QStringLiteral("nested-hold-leaf.mp4");
    heldLeaf.displayName = QStringLiteral("nested hold leaf");
    heldLeaf.duration = 2.0;
    heldLeaf.outPoint = 2.0;
    heldLeaf.reversed = true;
    TimelineSequence heldLeafSequence;
    heldLeafSequence.id = QStringLiteral("nested-one-key-leaf");
    heldLeafSequence.name = heldLeafSequence.id;
    heldLeafSequence.videoTracks = {{heldLeaf}};
    heldLeafSequence.audioTracks = {{heldLeaf}};

    ClipInfo heldSequenceRef;
    heldSequenceRef.sequenceRefId = heldLeafSequence.id;
    heldSequenceRef.filePath = timeline_nesting::sequenceClipFilePath(
        heldLeafSequence.id);
    heldSequenceRef.displayName = QStringLiteral("nested one-key hold");
    heldSequenceRef.duration = 2.0;
    heldSequenceRef.outPoint = 2.0;
    heldSequenceRef.timeRemapCurve.addKey(0.0, 0.75);
    TimelineSequence heldMainSequence;
    heldMainSequence.id = QStringLiteral("nested-one-key-main");
    heldMainSequence.name = heldMainSequence.id;
    heldMainSequence.videoTracks = {{heldSequenceRef}};
    heldMainSequence.audioTracks = {{heldSequenceRef}};

    Timeline heldTimeline;
    heldTimeline.setSequences(
        QVector<TimelineSequence>{heldMainSequence, heldLeafSequence},
        heldMainSequence.id);
    const QVector<PlaybackEntry> heldVideoEntries =
        heldTimeline.computePlaybackSequence();
    const QVector<PlaybackEntry> heldAudioEntries =
        heldTimeline.computeAudioPlaybackSequence();
    const bool heldVideoIntervalOk = heldVideoEntries.size() == 1
        && heldVideoEntries.first().timelineEnd
               > heldVideoEntries.first().timelineStart;
    const bool heldAudioIntervalOk = heldAudioEntries.size() == 1
        && heldAudioEntries.first().timelineEnd
               > heldAudioEntries.first().timelineStart;
    check(6, "ネストした1キー time-remap の preview 区間を保持する",
          heldVideoIntervalOk && heldAudioIntervalOk);

    // G7: splitting a reversed uniform-speed clip keeps the source frame of
    // every local second on both halves (rule (69)). Reversed speed 2 over
    // source [2, 8] is 3 s long; the split at local 1.2 sits at source 5.6.
    // HEAD split it with the forward formula, so the left half started at
    // source 4.4 and the right half at source 8 (main check FAILs).
    {
        constexpr double kFrameSec = 1.0 / 30.0;
        constexpr double kSplitLocalSec = 1.2;
        ClipInfo splitSource;
        splitSource.filePath = QStringLiteral("reverse-split.mp4");
        splitSource.displayName = QStringLiteral("reverse split");
        splitSource.duration = 8.0;
        splitSource.inPoint = 2.0;
        splitSource.outPoint = 8.0;
        splitSource.speed = 2.0;
        splitSource.reversed = true;
        const double totalSec = splitSource.effectiveDuration();
        const double splitSourceSec = splitSource.outPoint
            - kSplitLocalSec * splitSource.speed;

        QStringList details;
        // Compares every sampled local second of the two halves with the
        // unsplit clip and prints timeline / local / source seconds.
        const auto contractHolds = [&](const char *label, const ClipInfo &left,
                                       const ClipInfo &right) {
            bool ok = near(left.effectiveDuration(), kSplitLocalSec, 1e-6)
                && near(right.effectiveDuration(), totalSec - kSplitLocalSec, 1e-6);
            std::fprintf(stderr,
                         "reverse-clip G7 %s: left in=%.6f out=%.6f effectiveDuration=%.6f "
                         "(expect %.6f) right in=%.6f out=%.6f effectiveDuration=%.6f "
                         "(expect %.6f)\n",
                         label, left.inPoint, left.outPoint, left.effectiveDuration(),
                         kSplitLocalSec, right.inPoint, right.outPoint,
                         right.effectiveDuration(), totalSec - kSplitLocalSec);
            const double rightSec = totalSec - kSplitLocalSec;
            const double leftLocals[] = {0.0, kSplitLocalSec / 2.0,
                                         kSplitLocalSec - kFrameSec};
            const double rightLocals[] = {0.0, rightSec / 2.0, rightSec - kFrameSec};
            for (int half = 0; half < 2; ++half) {
                const ClipInfo &piece = half == 0 ? left : right;
                const double *locals = half == 0 ? leftLocals : rightLocals;
                const double pieceStartSec = half == 0 ? 0.0 : kSplitLocalSec;
                for (int i = 0; i < 3; ++i) {
                    const double localSec = locals[i];
                    const double timelineSec = pieceStartSec + localSec;
                    const double sourceSec = piece.sourceSecondAtLocalTime(localSec);
                    const double expectedSourceSec =
                        splitSource.sourceSecondAtLocalTime(timelineSec);
                    const bool sampleOk = near(sourceSec, expectedSourceSec, 1e-6);
                    std::fprintf(stderr,
                                 "reverse-clip G7 %s: %s timelineSec=%.6f localSec=%.6f "
                                 "sourceSec=%.6f (expect %.6f) %s\n",
                                 label, half == 0 ? "left" : "right", timelineSec,
                                 localSec, sourceSec, expectedSourceSec,
                                 sampleOk ? "ok" : "MISMATCH");
                    ok = ok && sampleOk;
                }
            }
            if (!ok)
                details << QString::fromLatin1(label);
            return ok;
        };

        Timeline splitTimeline;
        TimelineTrack *splitTrack = splitTimeline.videoTracks()[0];
        splitTrack->setClips({splitSource});
        splitTrack->splitClipAt(0, kSplitLocalSec);
        const QVector<ClipInfo> halves = splitTrack->clips();
        const bool splitOk = halves.size() == 2
            && contractHolds("split", halves.value(0), halves.value(1));
        if (halves.size() != 2)
            details << QStringLiteral("clipCount=%1 (expect 2)").arg(halves.size());

        // Save -> load keeps the contract.
        bool reloadOk = false;
        if (halves.size() == 2) {
            ProjectData savedHalves;
            savedHalves.videoTracks = QVector<QVector<ClipInfo>>{halves};
            ProjectData loadedHalves;
            const bool loadOk = ProjectFile::fromJsonString(
                ProjectFile::toJsonString(savedHalves), loadedHalves);
            reloadOk = loadOk && loadedHalves.videoTracks.size() == 1
                && loadedHalves.videoTracks.first().size() == 2
                && contractHolds("reload", loadedHalves.videoTracks.first().at(0),
                                 loadedHalves.videoTracks.first().at(1));
            if (!reloadOk && !loadOk)
                details << QStringLiteral("reload parse failed");
        }

        // Forward uniform split keeps the forward formula: [in, in + L*speed]
        // and [in + L*speed, out].
        ClipInfo forwardSource = splitSource;
        forwardSource.reversed = false;
        Timeline forwardTimeline;
        TimelineTrack *forwardTrack = forwardTimeline.videoTracks()[0];
        forwardTrack->setClips({forwardSource});
        forwardTrack->splitClipAt(0, kSplitLocalSec);
        const QVector<ClipInfo> forwardHalves = forwardTrack->clips();
        const double forwardSplitSec = forwardSource.inPoint
            + kSplitLocalSec * forwardSource.speed;
        const bool forwardOk = forwardHalves.size() == 2
            && near(forwardHalves[0].inPoint, 2.0) && near(forwardHalves[0].outPoint, forwardSplitSec)
            && near(forwardHalves[1].inPoint, forwardSplitSec) && near(forwardHalves[1].outPoint, 8.0);
        std::fprintf(stderr,
                     "reverse-clip G7 forward: halves=%d left [%.6f, %.6f] right [%.6f, %.6f] "
                     "(expect [2, %.6f] [%.6f, 8]) %s\n",
                     int(forwardHalves.size()), forwardHalves.value(0).inPoint,
                     forwardHalves.value(0).outPoint, forwardHalves.value(1).inPoint,
                     forwardHalves.value(1).outPoint, forwardSplitSec, forwardSplitSec,
                     forwardOk ? "ok" : "MISMATCH");
        if (!forwardOk)
            details << QStringLiteral("forward");

        // Reversed + speed ramp (constant 2x over source [2, 8]) is cut by the
        // consumed source (US-1501): local 1.2 consumes 2.4 s, so the left
        // half keeps [5.6, 8] and the right half [2, 5.6] (was the forward
        // formula [2, 3.2] / [3.2, 8] before US-1501).
        ClipInfo rampedSource = splitSource;
        rampedSource.speed = 1.0;
        rampedSource.speedRamp.addKeyframe(0, 2.0);
        Timeline rampedTimeline;
        TimelineTrack *rampedTrack = rampedTimeline.videoTracks()[0];
        rampedTrack->setClips({rampedSource});
        rampedTrack->splitClipAt(0, kSplitLocalSec);
        const QVector<ClipInfo> rampedHalves = rampedTrack->clips();
        const double rampedSplitSec = 5.6;
        const bool rampedOk = !rampedSource.speedRamp.isIdentity()
            && rampedHalves.size() == 2
            && near(rampedHalves[0].inPoint, rampedSplitSec, 1e-6)
            && near(rampedHalves[0].outPoint, 8.0, 1e-6)
            && near(rampedHalves[1].inPoint, 2.0, 1e-6)
            && near(rampedHalves[1].outPoint, rampedSplitSec, 1e-6);
        std::fprintf(stderr,
                     "reverse-clip G7 reversed+ramp: halves=%d left [%.6f, %.6f] right "
                     "[%.6f, %.6f] (expect [%.6f, 8] [2, %.6f]) %s\n",
                     int(rampedHalves.size()), rampedHalves.value(0).inPoint,
                     rampedHalves.value(0).outPoint, rampedHalves.value(1).inPoint,
                     rampedHalves.value(1).outPoint, rampedSplitSec, rampedSplitSec,
                     rampedOk ? "ok" : "MISMATCH");
        if (!rampedOk)
            details << QStringLiteral("reversed+ramp");

        // Freeze frame on a reversed clip holds the source frame under the
        // playhead: the held right half maps local 0 to the split source.
        Timeline freezeTimeline;
        TimelineTrack *freezeTrack = freezeTimeline.videoTracks()[0];
        freezeTrack->setClips({splitSource});
        freezeTimeline.setPlayheadPosition(kSplitLocalSec);
        const bool frozen = freezeTimeline.freezeFrameAtPlayhead(freezeTrack, 0);
        const QVector<ClipInfo> frozenClips = freezeTrack->clips();
        const double heldSourceSec = frozenClips.size() == 2
            ? frozenClips[1].sourceSecondAtLocalTime(0.0)
            : -1.0;
        const bool freezeOk = frozen && frozenClips.size() == 2
            && frozenClips[1].hasTimeRemap()
            && near(heldSourceSec, splitSourceSec, 1e-6);
        std::fprintf(stderr,
                     "reverse-clip G7 freeze: applied=%d clips=%d timelineSec=%.6f localSec=0 "
                     "sourceSec=%.6f (expect %.6f) %s\n",
                     frozen, int(frozenClips.size()), kSplitLocalSec, heldSourceSec,
                     splitSourceSec, freezeOk ? "ok" : "MISMATCH");
        if (!freezeOk)
            details << QStringLiteral("freeze");

        check(7, "逆再生 (一様速度) の分割で両片の素材フレームを保つ",
              splitOk && reloadOk && forwardOk && rampedOk && freezeOk,
              details.join(QStringLiteral(", ")));
    }

    // ---- US-1501: splitting speed-ramped / time-remapped clips (G8-G10) ----
    // Contract (rule (69)): every local second of both halves maps to the
    // source second of the unsplit clip, and the halves keep the lengths
    // L / D - L. Each sample prints timeline / local / source seconds; a
    // clip starts at timeline 0, so timelineSec is the unsplit local second.
    constexpr double kContractTol = 2e-5;
    struct Piece {
        double startSec = 0.0;  // start in the unsplit clip's local seconds
        ClipInfo clip;
    };
    // Largest |piece source - unsplit source| over localSamples (unsplit
    // local seconds). Prints every sample when verbose, else the worst one.
    const auto contractError = [&](const char *label, const ClipInfo &source,
                                   const QVector<Piece> &pieces,
                                   const QVector<double> &localSamples, bool verbose) {
        double worst = 0.0;
        double worstTimelineSec = 0.0, worstLocalSec = 0.0, worstSourceSec = 0.0,
               worstExpectSec = 0.0;
        for (const double timelineSec : localSamples) {
            int pieceIndex = 0;
            while (pieceIndex + 1 < pieces.size()
                   && timelineSec >= pieces[pieceIndex + 1].startSec)
                ++pieceIndex;
            const Piece &piece = pieces[pieceIndex];
            const double localSec = timelineSec - piece.startSec;
            const double sourceSec = piece.clip.sourceSecondAtLocalTime(localSec);
            const double expectSec = source.sourceSecondAtLocalTime(timelineSec);
            const double error = std::abs(sourceSec - expectSec);
            if (verbose) {
                std::fprintf(stderr,
                             "reverse-clip %s: piece=%d timelineSec=%.6f localSec=%.6f "
                             "sourceSec=%.6f (expect %.6f) err=%.2e\n",
                             label, pieceIndex, timelineSec, localSec, sourceSec, expectSec,
                             error);
            }
            if (error >= worst) {
                worst = error;
                worstTimelineSec = timelineSec;
                worstLocalSec = localSec;
                worstSourceSec = sourceSec;
                worstExpectSec = expectSec;
            }
        }
        std::fprintf(stderr,
                     "reverse-clip %s: samples=%d maxErr=%.3e at timelineSec=%.6f "
                     "localSec=%.6f sourceSec=%.6f (expect %.6f)\n",
                     label, int(localSamples.size()), worst, worstTimelineSec, worstLocalSec,
                     worstSourceSec, worstExpectSec);
        return worst;
    };
    // Five samples per piece: 0, len/4, len/2, 3len/4, len - 1/30.
    const auto fiveSamples = [](const QVector<Piece> &pieces) {
        QVector<double> samples;
        for (const Piece &piece : pieces) {
            const double lenSec = piece.clip.effectiveDuration();
            for (const double f : {0.0, 0.25, 0.5, 0.75})
                samples.append(piece.startSec + f * lenSec);
            samples.append(piece.startSec + lenSec - 1.0 / 30.0);
        }
        return samples;
    };
    const auto describe = [](const char *label, const QVector<Piece> &pieces) {
        for (int i = 0; i < pieces.size(); ++i) {
            const ClipInfo &c = pieces[i].clip;
            std::fprintf(stderr,
                         "reverse-clip %s: piece=%d start=%.6f in=%.6f out=%.6f speed=%.6f "
                         "effectiveDuration=%.6f rampIdentity=%d rampKeys=%d remapKeys=%d\n",
                         label, i, pieces[i].startSec, c.inPoint, c.outPoint, c.speed,
                         c.effectiveDuration(), c.speedRamp.isIdentity() ? 1 : 0,
                         int(c.speedRamp.keyframes.size()), int(c.timeRemapCurve.keys.size()));
        }
    };
    // Splits `source` alone on a fresh track at localSec.
    const auto splitAlone = [](const ClipInfo &source, double localSec, bool audioTrack) {
        TimelineTrack track;
        track.setIsAudioTrack(audioTrack);
        track.setClips({source});
        track.splitClipAt(0, localSec);
        QVector<Piece> pieces;
        double startSec = 0.0;
        for (const ClipInfo &clip : track.clips()) {
            pieces.append(Piece{startSec, clip});
            startSec += clip.effectiveDuration();
        }
        return pieces;
    };
    const auto lengthsOk = [](const QVector<Piece> &pieces, double splitSec, double totalSec) {
        return pieces.size() == 2
            && near(pieces[0].clip.effectiveDuration(), splitSec, 1e-6)
            && near(pieces[1].clip.effectiveDuration(), totalSec - splitSec, 1e-6);
    };
    const auto insideSource = [](const ClipInfo &source, const QVector<Piece> &pieces) {
        const double outSec = source.outPoint > 0.0 ? source.outPoint : source.duration;
        bool ok = true;
        for (const Piece &piece : pieces) {
            ok = ok && source.inPoint <= piece.clip.inPoint + 1e-9
                && piece.clip.inPoint < piece.clip.outPoint
                && piece.clip.outPoint <= outSec + 1e-9;
        }
        return ok;
    };

    // Near-constant ramp 2 -> 2.000002 (keys at 0 and 1000 s) on source
    // [0, 2100], split at L = 400 on a video track. Both pieces must match the
    // unsplit mapping and carry a ramp or remap curve that is not uniform.
    const auto nearConstantRampOk = [&](const char *label, bool reversed) {
        ClipInfo source;
        source.filePath = QStringLiteral("near-constant-ramp.mp4");
        source.duration = 2100.0;
        source.inPoint = 0.0;
        source.outPoint = 2100.0;
        source.speed = 1.0;
        source.reversed = reversed;
        source.speedRamp.keyframes = {{0, 2.0}, {1'000'000'000, 2.000002}};
        constexpr double kSplitSec = 400.0;
        const QVector<Piece> pieces = splitAlone(source, kSplitSec, false);
        describe(label, pieces);
        const bool lenOk = lengthsOk(pieces, kSplitSec, source.effectiveDuration());
        const double err = pieces.size() == 2
            ? contractError(label, source, pieces, fiveSamples(pieces), true)
            : 1.0;
        bool curved = pieces.size() == 2;
        for (const Piece &piece : pieces) {
            curved = curved
                && (!piece.clip.speedRamp.isIdentity() || !piece.clip.timeRemapCurve.keys.isEmpty());
        }
        std::fprintf(stderr, "reverse-clip %s: lengths=%d maxErr=%.3e notUniform=%d\n", label,
                     lenOk, err, curved);
        return lenOk && err <= kContractTol && curved;
    };

    // G8: forward ramp 0.5 -> 2.0 (keys at 0 and 10 s) over source [0, 10],
    // split at L = 4. Source runs out (t* ~ 8.685), so the right half also
    // covers the clamp. HEAD copied the ramp from the clip origin onto the
    // right half (right half started at R(0) instead of R(4) -> FAIL).
    {
        ClipInfo source;
        source.filePath = QStringLiteral("ramp-split.mp4");
        source.duration = 10.0;
        source.inPoint = 0.0;
        source.outPoint = 10.0;
        source.speed = 1.0;
        source.speedRamp.keyframes = {{0, 0.5}, {10'000'000, 2.0}};
        constexpr double kSplitSec = 4.0;
        const double totalSec = source.effectiveDuration();
        const QVector<Piece> pieces = splitAlone(source, kSplitSec, false);
        describe("G8 forward-ramp", pieces);
        const bool lenOk = lengthsOk(pieces, kSplitSec, totalSec);
        const double err = pieces.size() == 2
            ? contractError("G8 forward-ramp", source, pieces, fiveSamples(pieces), true)
            : 1.0;
        // Export path (TimelineFrameRenderer sourceSecondForClipAtLocalTime,
        // reverse composition OFF, no remap) reads in + localSec * speed:
        // enumerate the new pieces next to the HEAD pieces ([0, 4] / [4, 10]
        // at speed 1).
        if (pieces.size() == 2) {
            for (int i = 0; i < 2; ++i) {
                const ClipInfo &c = pieces[i].clip;
                const double headIn = i == 0 ? 0.0 : kSplitSec;
                for (const double f : {0.0, 0.5, 0.99}) {
                    const double localSec = f * c.effectiveDuration();
                    std::fprintf(stderr,
                                 "reverse-clip G8 export: piece=%d localSec=%.6f new=%.6f "
                                 "(in'=%.6f speed'=%.6f) head=%.6f (in=%.6f speed=1) "
                                 "preview=%.6f\n",
                                 i, localSec, c.inPoint + localSec * c.speed, c.inPoint, c.speed,
                                 headIn + localSec, headIn, c.sourceSecondAtLocalTime(localSec));
                }
            }
        }
        const bool rampsKept = pieces.size() == 2 && !pieces[0].clip.speedRamp.isIdentity()
            && !pieces[1].clip.speedRamp.isIdentity()
            && pieces[0].clip.timeRemapCurve.keys.isEmpty()
            && pieces[1].clip.timeRemapCurve.keys.isEmpty();
        // Near-constant ramp 2 -> 2.000002 over tau [0, 1000 s], split at 400:
        // normalizing by speed' puts every sliced key within 1e-6 of 1, so a
        // sliced ramp would read as identity (uniform) and drop the curvature.
        const bool nearConstOk = nearConstantRampOk("G8 near-constant", false);
        check(8, "順方向の速度ランプを分割しても両片の素材フレームと長さを保つ",
              lenOk && err <= kContractTol && rampsKept && nearConstOk,
              QStringLiteral("lengths=%1 maxErr=%2 rampsKept=%3 nearConstant=%4")
                  .arg(lenOk).arg(err, 0, 'e', 3).arg(rampsKept).arg(nearConstOk));
    }

    // G9: non-monotone remap (0,0) (4,6) (8,3) over source [1, 9] (D = 8),
    // split at L = 6 inside the decreasing span. Right half consumes [3, 4.5]
    // -> [4, 5.5] at speed' = 1.5 / 2 = 0.75. Freeze at the same playhead
    // holds F = 5.5 (forward) / 4.5 (reversed); HEAD held the right half's
    // in point with key (0, 0) -> FAIL. (i) remap (0,0) (4,12) (8,4) leaves
    // [0, out - in] for t in (8/3, 16/3); split at L = 4 keeps both halves
    // inside [in, out] and on the contract.
    {
        ClipInfo source;
        source.filePath = QStringLiteral("remap-split.mp4");
        source.duration = 10.0;
        source.inPoint = 1.0;
        source.outPoint = 9.0;
        source.speed = 1.0;
        source.timeRemapCurve.blendMode = timeremap::FrameBlendMode::Blend;
        source.timeRemapCurve.sourceFps = 24.0;
        source.timeRemapCurve.addKey(0.0, 0.0);
        source.timeRemapCurve.addKey(4.0, 6.0);
        source.timeRemapCurve.addKey(8.0, 3.0);
        constexpr double kSplitSec = 6.0;
        const double totalSec = source.effectiveDuration();
        const QVector<Piece> pieces = splitAlone(source, kSplitSec, false);
        describe("G9 remap", pieces);
        const bool lenOk = lengthsOk(pieces, kSplitSec, totalSec);
        const double err = pieces.size() == 2
            ? contractError("G9 remap", source, pieces, fiveSamples(pieces), true)
            : 1.0;
        const double expectSpeed = (4.5 - 3.0) / (totalSec - kSplitSec);
        const bool speedOk = pieces.size() == 2 && near(pieces[1].clip.speed, expectSpeed, 1e-9)
            && near(pieces[1].clip.inPoint, 4.0, 1e-9) && near(pieces[1].clip.outPoint, 5.5, 1e-9)
            && pieces[1].clip.timeRemapCurve.blendMode == timeremap::FrameBlendMode::Blend
            && near(pieces[1].clip.timeRemapCurve.sourceFps, 24.0);
        std::fprintf(stderr,
                     "reverse-clip G9 remap: right speed=%.6f (expect (mx-mn)/(D-L)=%.6f) "
                     "in=%.6f out=%.6f (expect [4, 5.5]) %s\n",
                     pieces.value(1).clip.speed, expectSpeed, pieces.value(1).clip.inPoint,
                     pieces.value(1).clip.outPoint, speedOk ? "ok" : "MISMATCH");

        // Freeze frame at the split playhead, forward and reversed.
        bool freezeOk = true;
        for (const bool reversedClip : {false, true}) {
            ClipInfo freezeSource = source;
            freezeSource.reversed = reversedClip;
            const double heldSec = freezeSource.sourceSecondAtLocalTime(kSplitSec);
            Timeline freezeTimeline;
            TimelineTrack *freezeTrack = freezeTimeline.videoTracks()[0];
            freezeTrack->setClips({freezeSource});
            freezeTimeline.setPlayheadPosition(kSplitSec);
            const bool frozen = freezeTimeline.freezeFrameAtPlayhead(freezeTrack, 0);
            const QVector<ClipInfo> frozenClips = freezeTrack->clips();
            const ClipInfo hold = frozenClips.value(1);
            const double holdOut = hold.outPoint > 0.0 ? hold.outPoint : hold.duration;
            const double expectKey = reversedClip ? holdOut - heldSec : heldSec - hold.inPoint;
            const double keySrc = hold.timeRemapCurve.keys.isEmpty()
                ? -1.0 : hold.timeRemapCurve.keys.first().srcTime;
            bool ok = frozen && frozenClips.size() == 2 && hold.timeRemapCurve.keys.size() == 1
                && near(keySrc, expectKey, 1e-9);
            for (const double localSec : {0.0, 0.5, 1.5}) {
                const double sourceSec = hold.sourceSecondAtLocalTime(localSec);
                const bool sampleOk = near(sourceSec, heldSec, 1e-9);
                std::fprintf(stderr,
                             "reverse-clip G9 freeze %s: timelineSec=%.6f localSec=%.6f "
                             "sourceSec=%.6f (expect F=%.6f) key=(0, %.6f) (expect %.6f) %s\n",
                             reversedClip ? "reversed" : "forward", kSplitSec + localSec,
                             localSec, sourceSec, heldSec, keySrc, expectKey,
                             sampleOk ? "ok" : "MISMATCH");
                ok = ok && sampleOk;
            }
            freezeOk = freezeOk && ok;
        }

        // (i) remap above out - in.
        ClipInfo over = source;
        over.timeRemapCurve.keys.clear();
        over.timeRemapCurve.addKey(0.0, 0.0);
        over.timeRemapCurve.addKey(4.0, 12.0);
        over.timeRemapCurve.addKey(8.0, 4.0);
        const QVector<Piece> overPieces = splitAlone(over, 4.0, false);
        describe("G9 (i) remap>out", overPieces);
        const double overErr = overPieces.size() == 2
            ? contractError("G9 (i) remap>out", over, overPieces, fiveSamples(overPieces), true)
            : 1.0;
        const bool overOk = lengthsOk(overPieces, 4.0, over.effectiveDuration())
            && insideSource(over, overPieces) && overErr <= kContractTol;

        check(9, "非単調なタイムリマップの分割で素材フレーム・片の速度・フリーズの保持を保つ",
              lenOk && err <= kContractTol && speedOk && freezeOk && overOk,
              QStringLiteral("lengths=%1 maxErr=%2 speed=%3 freeze=%4 (i)=%5")
                  .arg(lenOk).arg(err, 0, 'e', 3).arg(speedOk).arg(freezeOk).arg(overOk));
    }

    // G10: reversed ramp / remap, held pieces, clamped tails, large speed
    // ratios through the Timeline edit paths, and audio-track pieces (h).
    {
        QStringList failures;
        const auto note = [&](const QString &item, bool ok) {
            std::fprintf(stderr, "reverse-clip G10 %s: %s\n", item.toUtf8().constData(),
                         ok ? "ok" : "MISMATCH");
            if (!ok)
                failures << item;
        };

        // (a) reversed + ramp (G8's clip reversed) and reversed + remap (G9's).
        // HEAD cut reversed ramped / remapped clips with the forward formula.
        {
            ClipInfo rampSource;
            rampSource.filePath = QStringLiteral("ramp-split.mp4");
            rampSource.duration = 10.0;
            rampSource.outPoint = 10.0;
            rampSource.speed = 1.0;
            rampSource.reversed = true;
            rampSource.speedRamp.keyframes = {{0, 0.5}, {10'000'000, 2.0}};
            const QVector<Piece> rampPieces = splitAlone(rampSource, 4.0, false);
            describe("G10 (a) reversed+ramp", rampPieces);
            const double rampErr = rampPieces.size() == 2
                ? contractError("G10 (a) reversed+ramp", rampSource, rampPieces,
                                fiveSamples(rampPieces), true)
                : 1.0;
            note(QStringLiteral("(a) reversed+ramp"),
                 lengthsOk(rampPieces, 4.0, rampSource.effectiveDuration())
                     && insideSource(rampSource, rampPieces) && rampErr <= kContractTol);
            note(QStringLiteral("(a) reversed near-constant ramp"),
                 nearConstantRampOk("G10 (a) reversed near-constant", true));

            ClipInfo remapSource;
            remapSource.filePath = QStringLiteral("remap-split.mp4");
            remapSource.duration = 10.0;
            remapSource.inPoint = 1.0;
            remapSource.outPoint = 9.0;
            remapSource.speed = 1.0;
            remapSource.reversed = true;
            remapSource.timeRemapCurve.addKey(0.0, 0.0);
            remapSource.timeRemapCurve.addKey(4.0, 6.0);
            remapSource.timeRemapCurve.addKey(8.0, 3.0);
            const QVector<Piece> remapPieces = splitAlone(remapSource, 6.0, false);
            describe("G10 (a) reversed+remap", remapPieces);
            const double remapErr = remapPieces.size() == 2
                ? contractError("G10 (a) reversed+remap", remapSource, remapPieces,
                                fiveSamples(remapPieces), true)
                : 1.0;
            note(QStringLiteral("(a) reversed+remap"),
                 lengthsOk(remapPieces, 6.0, remapSource.effectiveDuration())
                     && insideSource(remapSource, remapPieces) && remapErr <= kContractTol);
        }

        // (ii) reversed + held span / (iii) forward clamped tail: constant 2x
        // ramp over source [1, 7] (D = 6) runs out at t* = 3; split at L = 4.
        // The right half holds the last consumed frame: reversed -> source
        // in (1), forward -> source out (7, out' = out).
        {
            ClipInfo heldSource;
            heldSource.filePath = QStringLiteral("held-split.mp4");
            heldSource.duration = 8.0;
            heldSource.inPoint = 1.0;
            heldSource.outPoint = 7.0;
            heldSource.speed = 1.0;
            heldSource.speedRamp.keyframes = {{0, 2.0}};
            for (const bool reversedClip : {true, false}) {
                ClipInfo c = heldSource;
                c.reversed = reversedClip;
                const char *label = reversedClip ? "G10 (ii) reversed held" : "G10 (iii) forward tail";
                const QVector<Piece> pieces = splitAlone(c, 4.0, false);
                describe(label, pieces);
                const double err = pieces.size() == 2
                    ? contractError(label, c, pieces, fiveSamples(pieces), true)
                    : 1.0;
                const double heldSec = reversedClip ? 1.0 : 7.0;
                bool holdOk = pieces.size() == 2
                    && pieces[1].clip.timeRemapCurve.keys.size() == 1
                    && pieces[1].clip.speedRamp.isIdentity();
                for (const double localSec : {0.0, 1.0, 1.9}) {
                    const double sourceSec = pieces.value(1).clip.sourceSecondAtLocalTime(localSec);
                    std::fprintf(stderr,
                                 "reverse-clip %s: hold timelineSec=%.6f localSec=%.6f "
                                 "sourceSec=%.6f (expect %.6f)\n",
                                 label, 4.0 + localSec, localSec, sourceSec, heldSec);
                    holdOk = holdOk && near(sourceSec, heldSec, 1e-9);
                }
                const bool tailOk = reversedClip
                    || (pieces.size() == 2 && near(pieces[1].clip.outPoint, 7.0, 1e-12));
                note(QString::fromLatin1(label + 4),  // drop "G10 "
                     lengthsOk(pieces, 4.0, c.effectiveDuration()) && insideSource(c, pieces)
                         && err <= kContractTol && holdOk && tailOk);
            }
        }

        // (iv') large speed-ratio ramp with a clamp and a short (<= 0.1 s)
        // span on the last clip of V1 with a linked A1 clip (atempo ON):
        // keys 0.1 -> 10 within 0.05 s, 10 -> 0.5 by 0.15 s, 0.5 -> 10 by
        // 1 s over source [0, 6] (D = 6, runs out mid clip). Split through
        // splitClipByIndex at timeline 2.0 + 0.3 and through
        // overwriteClip3Point over [2.4, 2.9]. Samples: every midpoint of the
        // video pieces' curve keys, and t* +- h/2.
        {
            ClipInfo rampClip;
            rampClip.filePath = QStringLiteral("ratio-split.mp4");
            rampClip.duration = 6.0;
            rampClip.inPoint = 0.0;
            rampClip.outPoint = 6.0;
            rampClip.speed = 1.0;
            rampClip.linkGroup = 9;
            rampClip.atempoEnabled = true;
            rampClip.speedRamp.keyframes = {{0, 0.1}, {50'000, 10.0}, {150'000, 0.5},
                                            {1'000'000, 10.0}};
            ClipInfo prevClip;
            prevClip.filePath = QStringLiteral("prev.mp4");
            prevClip.duration = 2.0;
            prevClip.outPoint = 2.0;
            prevClip.speed = 1.0;
            prevClip.linkGroup = 7;
            constexpr double kClipStartSec = 2.0;
            constexpr double kSplitLocalSec = 0.3;

            // Video samples: curve-key midpoints of each piece plus t* +- h/2.
            const auto curveSamples = [&](const QVector<Piece> &pieces, double *tStarOut) {
                QVector<double> samples;
                double tStar = -1.0;
                double hBefore = 0.0, hAfter = 0.0;
                for (const Piece &piece : pieces) {
                    const auto &keys = piece.clip.timeRemapCurve.keys;
                    for (int k = 0; k + 1 < keys.size(); ++k) {
                        samples.append(piece.startSec
                                       + 0.5 * (keys[k].outTime + keys[k + 1].outTime));
                        // t*: where the source reaches the unsplit clip's out.
                        const double outSec = rampClip.outPoint;
                        if (tStar < 0.0
                            && piece.clip.inPoint + keys[k + 1].srcTime >= outSec - 1e-9
                            && piece.clip.inPoint + keys[k].srcTime < outSec - 1e-9) {
                            tStar = piece.startSec + keys[k + 1].outTime;
                            hBefore = keys[k + 1].outTime - keys[k].outTime;
                            hAfter = k + 2 < keys.size()
                                ? keys[k + 2].outTime - keys[k + 1].outTime
                                : hBefore;
                        }
                    }
                }
                if (tStar >= 0.0) {
                    samples.append(tStar - hBefore / 2.0);
                    samples.append(tStar + hAfter / 2.0);
                }
                *tStarOut = tStar;
                return samples;
            };
            const auto videoPiecesOk = [&](const char *label, const ClipInfo &source,
                                           const QVector<Piece> &pieces, bool expectTStar) {
                bool ok = insideSource(source, pieces);
                const double sourceOutSec = source.outPoint > 0.0 ? source.outPoint : source.duration;
                for (const Piece &piece : pieces) {
                    double epsilonFinal = 0.0;
                    source.speedRamp.approximateSourceCurve(
                        piece.startSec, piece.startSec + piece.clip.effectiveDuration(),
                        source.speed > 0.0 ? source.speed : 1.0, sourceOutSec - source.inPoint,
                        &epsilonFinal);
                    std::fprintf(stderr,
                                 "reverse-clip %s: piece start=%.6f curveKeys=%d (<= 4096) "
                                 "epsilonFinal=%.3e\n",
                                 label, piece.startSec, int(piece.clip.timeRemapCurve.keys.size()),
                                 epsilonFinal);
                    ok = ok && !piece.clip.timeRemapCurve.keys.isEmpty()
                        && piece.clip.timeRemapCurve.keys.size() <= 4096
                        && piece.clip.speedRamp.isIdentity();
                }
                double tStar = -1.0;
                const QVector<double> samples = curveSamples(pieces, &tStar);
                std::fprintf(stderr, "reverse-clip %s: t*=%.6f (unsplit local)\n", label, tStar);
                const double err = contractError(label, source, pieces, samples, false);
                return ok && (!expectTStar || tStar > 0.0) && err <= kContractTol;
            };

            // splitClipByIndex with the linked audio.
            Timeline timeline;
            TimelineTrack *v1 = timeline.videoTracks()[0];
            TimelineTrack *a1 = timeline.audioTracks()[0];
            v1->setClips({prevClip, rampClip});
            a1->setClips({prevClip, rampClip});
            QString err;
            const bool splitOk = timeline.splitClipByIndex(false, 0, 1,
                                                          kClipStartSec + kSplitLocalSec, &err);
            const QVector<ClipInfo> vClips = v1->clips();
            const QVector<ClipInfo> aClips = a1->clips();
            std::fprintf(stderr,
                         "reverse-clip G10 (iv') splitClipByIndex: ok=%d err=%s v1=%d a1=%d "
                         "prevGroup v=%d a=%d (expect 7) a1.isAudioTrack=%d\n",
                         splitOk, err.toUtf8().constData(), int(vClips.size()),
                         int(aClips.size()), vClips.value(0).linkGroup,
                         aClips.value(0).linkGroup, a1->isAudioTrack());
            bool splitItemOk = splitOk && vClips.size() == 3 && aClips.size() == 3
                && vClips[0].linkGroup == 7 && aClips[0].linkGroup == 7;
            if (vClips.size() == 3) {
                const QVector<Piece> pieces{{0.0, vClips[1]},
                                            {kSplitLocalSec, vClips[2]}};
                describe("G10 (iv') video", pieces);
                splitItemOk = splitItemOk && lengthsOk(pieces, kSplitLocalSec, rampClip.effectiveDuration())
                    && videoPiecesOk("G10 (iv') video", rampClip, pieces, true);
            }
            if (aClips.size() == 3) {
                // Rule (h): no remap; forward out-of-range / held pieces play
                // 1:1 from min(in + mn, out - len * speed).
                const QVector<Piece> pieces{{0.0, aClips[1]}, {kSplitLocalSec, aClips[2]}};
                describe("G10 (iv') audio", pieces);
                bool audioOk = insideSource(rampClip, pieces)
                    && lengthsOk(pieces, kSplitLocalSec, rampClip.effectiveDuration());
                for (const Piece &piece : pieces) {
                    const ClipInfo &c = piece.clip;
                    const double lenSec = c.effectiveDuration();
                    const double mn = rampClip.sourceSecondAtLocalTime(piece.startSec)
                        - rampClip.inPoint;
                    const double expectIn = qMin(rampClip.inPoint + mn, 6.0 - lenSec * 1.0);
                    const bool pieceOk = c.timeRemapCurve.keys.isEmpty()
                        && c.speedRamp.isIdentity() && near(c.speed, 1.0)
                        && near(c.inPoint, expectIn, 1e-6)
                        && near(c.outPoint, expectIn + lenSec, 1e-6);
                    std::fprintf(stderr,
                                 "reverse-clip G10 (iv') audio: piece start=%.6f in=%.6f "
                                 "(expect min(in+mn, out-len*speed)=%.6f) out=%.6f speed=%.6f "
                                 "remapKeys=%d %s\n",
                                 piece.startSec, c.inPoint, expectIn, c.outPoint, c.speed,
                                 int(c.timeRemapCurve.keys.size()), pieceOk ? "ok" : "MISMATCH");
                    audioOk = audioOk && pieceOk;
                }
                splitItemOk = splitItemOk && audioOk;
            }
            note(QStringLiteral("(iv') splitClipByIndex"), splitItemOk);

            // overwriteClip3Point over [kClipStartSec + 0.4, + 0.9] of V1.
            ClipInfo insert;
            insert.filePath = QStringLiteral("insert.mp4");
            insert.duration = 0.5;
            insert.outPoint = 0.5;
            insert.speed = 1.0;
            TimelineTrack overwriteTrack;
            overwriteTrack.setClips({prevClip, rampClip});
            overwriteTrack.overwriteClip3Point(kClipStartSec + 0.4, insert);
            const QVector<ClipInfo> oClips = overwriteTrack.clips();
            std::fprintf(stderr,
                         "reverse-clip G10 (iv') overwriteClip3Point: clips=%d (expect 4) "
                         "inserted=%s prevGroup=%d\n",
                         int(oClips.size()), oClips.value(2).filePath.toUtf8().constData(),
                         oClips.value(0).linkGroup);
            bool overwriteOk = oClips.size() == 4 && oClips[2].filePath == insert.filePath
                && oClips[0].linkGroup == 7
                && near(oClips[1].effectiveDuration(), 0.4, 1e-6)
                && near(oClips[3].effectiveDuration(), rampClip.effectiveDuration() - 0.9, 1e-6);
            if (oClips.size() == 4) {
                const QVector<Piece> head{{0.0, oClips[1]}};
                const QVector<Piece> tail{{0.9, oClips[3]}};
                describe("G10 (iv') overwrite head", head);
                describe("G10 (iv') overwrite tail", tail);
                overwriteOk = overwriteOk && videoPiecesOk("G10 (iv') overwrite head", rampClip, head, false);
                // Tail samples are taken on the tail alone (contractError
                // picks the only piece).
                bool tailKeysOk = !oClips[3].timeRemapCurve.keys.isEmpty()
                    && oClips[3].speedRamp.isIdentity()
                    && oClips[3].timeRemapCurve.keys.size() <= 4096;
                QVector<double> tailSamples;
                const auto &tailKeys = oClips[3].timeRemapCurve.keys;
                for (int k = 0; k + 1 < tailKeys.size(); ++k)
                    tailSamples.append(0.9 + 0.5 * (tailKeys[k].outTime + tailKeys[k + 1].outTime));
                const double tailErr = tailSamples.isEmpty()
                    ? 1.0
                    : contractError("G10 (iv') overwrite tail", rampClip, tail, tailSamples, false);
                overwriteOk = overwriteOk && tailKeysOk && tailErr <= kContractTol
                    && insideSource(rampClip, tail);
            }
            note(QStringLiteral("(iv') overwriteClip3Point"), overwriteOk);
        }

        // (iv'') long ramp: 0.1 -> 10 linear over tau [0, 150 s] on source
        // [0, 760] (consumes 757.5 s by 150 s). Split at L = 150: the left
        // piece needs 4309 spans at eps = 1e-5, so eps relaxes.
        {
            ClipInfo longClip;
            longClip.filePath = QStringLiteral("long-ramp.mp4");
            longClip.duration = 760.0;
            longClip.outPoint = 760.0;
            longClip.speed = 1.0;
            longClip.speedRamp.keyframes = {{0, 0.1}, {150'000'000, 10.0}};
            constexpr double kSplitSec = 150.0;
            const QVector<Piece> pieces = splitAlone(longClip, kSplitSec, false);
            describe("G10 (iv'') long", pieces);
            bool ok = lengthsOk(pieces, kSplitSec, longClip.effectiveDuration())
                && insideSource(longClip, pieces);
            for (const Piece &piece : pieces) {
                double epsilonFinal = 0.0;
                const auto points = longClip.speedRamp.approximateSourceCurve(
                    piece.startSec, piece.startSec + piece.clip.effectiveDuration(), 1.0, 760.0,
                    &epsilonFinal);
                const auto &keys = piece.clip.timeRemapCurve.keys;
                QVector<double> samples;
                for (int k = 0; k + 1 < keys.size(); ++k)
                    samples.append(piece.startSec + 0.5 * (keys[k].outTime + keys[k + 1].outTime));
                const double err = samples.isEmpty()
                    ? 1.0
                    : contractError("G10 (iv'') long", longClip, {piece}, samples, false);
                const bool capped = keys.size() <= 4096;
                std::fprintf(stderr,
                             "reverse-clip G10 (iv'') long: piece start=%.6f keys=%d (helper %d, "
                             "<= 4096 or all n = 1: %d) epsilonFinal=%.3e maxErr=%.3e "
                             "(<= epsilonFinal + 6e-6 = %.3e)\n",
                             piece.startSec, int(keys.size()), int(points.size()), capped,
                             epsilonFinal, err, epsilonFinal + 6e-6);
                ok = ok && keys.size() == points.size() && capped && piece.clip.speedRamp.isIdentity()
                    && err <= epsilonFinal + 6e-6;
            }
            note(QStringLiteral("(iv'') long ramp"), ok);
        }

        // (v) audio-track pieces (rule (h)) on a TimelineTrack with
        // setIsAudioTrack(true). P = preview mapping (AudioMixer): reversed
        // in' + clamp_pos(R(t * clamp_speed(speed'))), forward ramp + atempo
        // in' + R(t * speed'), forward identity in' + t. E = export: forward
        // atrim [in', out'] read 1:1 (in' + t), reversed at speed'.
        {
            const auto rampSource = [](const speedramp::SpeedRamp &ramp, double tauSec) {
                return ramp.isIdentity()
                    ? tauSec
                    : static_cast<double>(ramp.timelineToSourceUs(qRound64(tauSec * 1'000'000.0)))
                        / 1'000'000.0;
            };
            const auto preview = [&](const ClipInfo &c, double t) {
                if (c.reversed) {
                    const double clampedSpeed = qBound(0.1, c.speed, 10.0);
                    return c.inPoint
                        + qBound(0.0, rampSource(c.speedRamp, t * clampedSpeed),
                                 c.outPoint - c.inPoint);
                }
                if (!c.speedRamp.isIdentity() && c.atempoEnabled)
                    return c.inPoint + rampSource(c.speedRamp, t * c.speed);
                return c.inPoint + t;
            };
            ClipInfo k2;
            k2.filePath = QStringLiteral("audio-k2.wav");
            k2.duration = 10.0;
            k2.inPoint = 0.0;
            k2.outPoint = 10.0;
            k2.speed = 1.0;
            k2.atempoEnabled = true;
            k2.speedRamp.keyframes = {{0, 2.0}};  // t* = 5 s
            const auto noRemap = [](const QVector<Piece> &pieces) {
                bool ok = !pieces.isEmpty();
                for (const Piece &piece : pieces)
                    ok = ok && piece.clip.timeRemapCurve.keys.isEmpty();
                return ok;
            };
            // P of the piece against P of the unsplit clip (forward ramp +
            // atempo), at five samples of the piece.
            const auto previewMatches = [&](const char *label, const ClipInfo &source,
                                            const Piece &piece) {
                bool ok = true;
                const double lenSec = piece.clip.effectiveDuration();
                for (const double f : {0.0, 0.25, 0.5, 0.75, 0.99}) {
                    const double t = f * lenSec;
                    const double got = preview(piece.clip, t);
                    const double expect = preview(source, piece.startSec + t);
                    std::fprintf(stderr,
                                 "reverse-clip G10 %s: P timelineSec=%.6f localSec=%.6f "
                                 "sourceSec=%.6f (expect %.6f)\n",
                                 label, piece.startSec + t, t, got, expect);
                    ok = ok && near(got, expect, kContractTol);
                }
                return ok;
            };
            const auto oneToOne = [&](const char *label, const Piece &piece, double expectIn,
                                      double expectOut) {
                const ClipInfo &c = piece.clip;
                bool ok = c.speedRamp.isIdentity() && near(c.speed, 1.0)
                    && near(c.inPoint, expectIn, 1e-6) && near(c.outPoint, expectOut, 1e-6);
                for (const double t : {0.0, 0.5 * c.effectiveDuration()}) {
                    const double p = preview(c, t);
                    const double e = c.inPoint + t;
                    std::fprintf(stderr,
                                 "reverse-clip G10 %s: 1:1 localSec=%.6f P=%.6f E=%.6f "
                                 "(expect %.6f)\n",
                                 label, t, p, e, expectIn + t);
                    ok = ok && near(p, expectIn + t, 1e-6) && near(e, expectIn + t, 1e-6);
                }
                std::fprintf(stderr,
                             "reverse-clip G10 %s: [%.6f, %.6f] (expect [%.6f, %.6f]) speed=%.6f "
                             "rampIdentity=%d %s\n",
                             label, c.inPoint, c.outPoint, expectIn, expectOut, c.speed,
                             c.speedRamp.isIdentity(), ok ? "ok" : "MISMATCH");
                return ok;
            };

            // (v-a) L = 6 after t* (P for the left piece, P / E 1:1 for the
            // right held piece [out - 4 * speed, out]).
            {
                const QVector<Piece> pieces = splitAlone(k2, 6.0, true);
                describe("G10 (v-a)", pieces);
                const bool ok = lengthsOk(pieces, 6.0, 10.0) && insideSource(k2, pieces)
                    && noRemap(pieces)
                    && !pieces[0].clip.speedRamp.isIdentity()
                    && near(pieces[0].clip.inPoint, 0.0, 1e-6)
                    && near(pieces[0].clip.outPoint, 10.0, 1e-6)  // E: atrim [in, in + mx]
                    && previewMatches("(v-a) left", k2, pieces[0])
                    && oneToOne("(v-a) right", pieces[1], 10.0 - 4.0, 10.0);
                note(QStringLiteral("(v-a) audio held piece"), ok);
            }
            // (v-b) delta / len = 0.08 before t* (v' = 12.5): forward
            // out-of-range piece, 1:1 from min(in + mn, out - len * speed).
            {
                const double delta = 0.08 * 5.0 / 0.92;
                const double splitSec = 5.0 - delta;
                const double lenSec = 10.0 - splitSec;
                const QVector<Piece> pieces = splitAlone(k2, splitSec, true);
                describe("G10 (v-b)", pieces);
                // Left piece: v' = 1, so its sliced ramp is the identity and
                // it is a uniform speed' = 2 piece; E = atrim [in, in + mx].
                const bool ok = lengthsOk(pieces, splitSec, 10.0) && insideSource(k2, pieces)
                    && noRemap(pieces) && near(pieces[0].clip.inPoint, 0.0, 1e-6)
                    && near(pieces[0].clip.outPoint, 2.0 * splitSec, 1e-5)
                    && near(pieces[0].clip.speed, 2.0, 1e-5)
                    && oneToOne("(v-b) right", pieces[1], 10.0 - lenSec, 10.0);
                note(QStringLiteral("(v-b) audio out-of-range forward piece"), ok);
            }
            // (v-b2) reversed, delta / len = 0.04 (speed' = 0.08): mirrored
            // [out - mx, out - mn] = [0, 2 delta]; P clamps the speed to 0.1,
            // E plays speed' = 0.08.
            {
                ClipInfo rev = k2;
                rev.reversed = true;
                const double delta = 0.04 * 5.0 / 0.96;
                const double splitSec = 5.0 - delta;
                const double lenSec = 10.0 - splitSec;
                const QVector<Piece> pieces = splitAlone(rev, splitSec, true);
                describe("G10 (v-b2)", pieces);
                bool ok = lengthsOk(pieces, splitSec, 10.0) && insideSource(rev, pieces)
                    && noRemap(pieces);
                if (pieces.size() == 2) {
                    const ClipInfo &c = pieces[1].clip;
                    ok = ok && c.speedRamp.isIdentity() && near(c.inPoint, 0.0, 1e-6)
                        && near(c.outPoint, 2.0 * delta, 1e-5) && near(c.speed, 0.08, 1e-5);
                    const double t = 0.5 * lenSec;
                    const double p = preview(c, t);
                    const double e = c.inPoint + t * c.speed;
                    const double expectP = qMin(0.1 * t, 2.0 * delta);
                    const double expectE = 0.08 * t;
                    std::fprintf(stderr,
                                 "reverse-clip G10 (v-b2) right: [%.6f, %.6f] (expect [0, %.6f]) "
                                 "speed'=%.6f (expect 0.08) localSec=%.6f P=%.6f (clamp 0.1, "
                                 "expect %.6f) E=%.6f (speed', expect %.6f)\n",
                                 c.inPoint, c.outPoint, 2.0 * delta, c.speed, t, p, expectP, e,
                                 expectE);
                    ok = ok && near(p, expectP, 1e-5) && near(e, expectE, 1e-5);
                }
                note(QStringLiteral("(v-b2) audio reversed out-of-range piece"), ok);
            }
            // (v-b3) delta / len = 0.2 (v' = 5): the right piece keeps a ramp.
            {
                const double splitSec = 3.75;
                const QVector<Piece> pieces = splitAlone(k2, splitSec, true);
                describe("G10 (v-b3)", pieces);
                const bool ok = lengthsOk(pieces, splitSec, 10.0) && insideSource(k2, pieces)
                    && noRemap(pieces) && !pieces[1].clip.speedRamp.isIdentity()
                    && near(pieces[1].clip.speed, 0.4, 1e-9)
                    && near(pieces[1].clip.inPoint, 7.5, 1e-6)
                    && previewMatches("(v-b3) right", k2, pieces[1]);
                note(QStringLiteral("(v-b3) audio ramp kept at v' = 5"), ok);
            }
            // (v-c) 0.5 -> 2 ramp + atempo ON: both pieces keep a ramp, P
            // matches, E = atrim [in + mn, in + mx]. HEAD copied the ramp from
            // the clip origin onto the right piece (P mismatch -> FAIL).
            ClipInfo slow = k2;
            slow.speedRamp.keyframes = {{0, 0.5}, {10'000'000, 2.0}};
            {
                const QVector<Piece> pieces = splitAlone(slow, 4.0, true);
                describe("G10 (v-c)", pieces);
                const double mn = 0.5 * 4.0 + 0.5 * (1.5 / 10.0) * 16.0;
                const bool ok = lengthsOk(pieces, 4.0, 10.0) && insideSource(slow, pieces)
                    && noRemap(pieces) && !pieces[0].clip.speedRamp.isIdentity()
                    && !pieces[1].clip.speedRamp.isIdentity()
                    && near(pieces[1].clip.inPoint, mn, 1e-5)
                    && near(pieces[1].clip.outPoint, 10.0, 1e-6)
                    && previewMatches("(v-c) left", slow, pieces[0])
                    && previewMatches("(v-c) right", slow, pieces[1]);
                note(QStringLiteral("(v-c) audio ramp + atempo"), ok);
            }
            // (v-d) atempo OFF: uniform rule, ramp copied (same as HEAD).
            {
                ClipInfo off = slow;
                off.atempoEnabled = false;
                const QVector<Piece> pieces = splitAlone(off, 4.0, true);
                describe("G10 (v-d)", pieces);
                const bool ok = lengthsOk(pieces, 4.0, 10.0) && insideSource(off, pieces)
                    && noRemap(pieces)
                    && near(pieces[0].clip.outPoint, 4.0) && near(pieces[1].clip.inPoint, 4.0)
                    && pieces[0].clip.speedRamp == off.speedRamp
                    && pieces[1].clip.speedRamp == off.speedRamp;
                note(QStringLiteral("(v-d) audio atempo OFF uniform"), ok);
            }
            // (v-e) audio clip carrying a remap: the pieces drop it and take
            // the uniform rule (lengths L / D - L). HEAD copied the remap.
            {
                ClipInfo remapped = k2;
                remapped.speedRamp = speedramp::SpeedRamp::identity();
                remapped.timeRemapCurve.addKey(0.0, 0.0);
                remapped.timeRemapCurve.addKey(10.0, 4.0);
                const QVector<Piece> pieces = splitAlone(remapped, 4.0, true);
                describe("G10 (v-e)", pieces);
                const bool ok = lengthsOk(pieces, 4.0, 10.0) && insideSource(remapped, pieces)
                    && noRemap(pieces)
                    && near(pieces[0].clip.outPoint, 4.0) && near(pieces[1].clip.inPoint, 4.0);
                note(QStringLiteral("(v-e) audio remap dropped"), ok);
            }
        }

        check(10, "逆再生・停止区間・大きな速度比・音声片でも分割が素材フレームと長さを保つ",
              failures.isEmpty(), failures.join(QStringLiteral(", ")));
    }

    std::fprintf(stderr, "summary: %d PASS, %d FAIL, %d SKIP\n", passed, failed, 0);
    return failed;
}
