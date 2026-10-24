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

        // Reversed + speed ramp keeps the forward formula (not handled yet).
        ClipInfo rampedSource = splitSource;
        rampedSource.speed = 1.0;
        rampedSource.speedRamp.addKeyframe(0, 2.0);
        Timeline rampedTimeline;
        TimelineTrack *rampedTrack = rampedTimeline.videoTracks()[0];
        rampedTrack->setClips({rampedSource});
        rampedTrack->splitClipAt(0, kSplitLocalSec);
        const QVector<ClipInfo> rampedHalves = rampedTrack->clips();
        const double rampedSplitSec = rampedSource.inPoint
            + kSplitLocalSec * rampedSource.speed;
        const bool rampedOk = !rampedSource.speedRamp.isIdentity()
            && rampedHalves.size() == 2
            && near(rampedHalves[0].inPoint, 2.0) && near(rampedHalves[0].outPoint, rampedSplitSec)
            && near(rampedHalves[1].inPoint, rampedSplitSec) && near(rampedHalves[1].outPoint, 8.0);
        std::fprintf(stderr,
                     "reverse-clip G7 reversed+ramp: halves=%d left [%.6f, %.6f] right "
                     "[%.6f, %.6f] (expect [2, %.6f] [%.6f, 8]) %s\n",
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

    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
