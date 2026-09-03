#include "../Timeline.h"
#include "../UndoManager.h"

#include <QApplication>

#include <cmath>
#include <cstdio>

namespace {

ClipInfo testClip(const QString &name)
{
    ClipInfo clip;
    clip.filePath = name;
    clip.displayName = name;
    clip.duration = 4.0;
    clip.outPoint = 4.0;
    return clip;
}

bool sameTransition(const Transition &left, const Transition &right)
{
    return left.type == right.type
        && std::abs(left.duration - right.duration) < 1e-9;
}

bool sameClipIdentity(const QVector<ClipInfo> &left,
                      const QVector<ClipInfo> &right)
{
    if (left.size() != right.size())
        return false;
    for (int i = 0; i < left.size(); ++i) {
        if (left[i].filePath != right[i].filePath
            || left[i].leadIn.type != right[i].leadIn.type
            || left[i].trailOut.type != right[i].trailOut.type
            || std::abs(left[i].leadIn.duration - right[i].leadIn.duration) >= 1e-9
            || std::abs(left[i].trailOut.duration - right[i].trailOut.duration) >= 1e-9) {
            return false;
        }
    }
    return true;
}

bool transitionsAreNone(const QVector<ClipInfo> &clips)
{
    for (const ClipInfo &clip : clips) {
        if (clip.leadIn.type != TransitionType::None
            || clip.trailOut.type != TransitionType::None) {
            return false;
        }
    }
    return true;
}

bool saveAudioBaseline(Timeline &timeline,
                       const QVector<ClipInfo> &clips)
{
    TimelineTrack *track = timeline.trackAt(true, 0);
    if (!track || !timeline.undoManager())
        return false;
    track->setClips(clips);
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(
        timeline.currentState(), QStringLiteral("audio-xfade selftest baseline"));
    return true;
}

void gate(int number, const char *description, bool ok, int &passed, int &failed)
{
    std::fprintf(stderr, "%s G%d %s\n", ok ? "PASS" : "FAIL", number,
                 description);
    ok ? ++passed : ++failed;
}

} // namespace

int runAudioXfadeSelftest()
{
    int passed = 0;
    int failed = 0;

    Timeline timeline;
    TimelineTrack *audioTrack = timeline.trackAt(true, 0);
    const QVector<ClipInfo> before{testClip(QStringLiteral("A")),
                                   testClip(QStringLiteral("B"))};
    const Transition expectedCrossfade{
        TransitionType::CrossDissolve, 1.25,
        TransitionAlignment::Center, TransitionEasing::Linear};
    QString error;
    const bool baselineReady = saveAudioBaseline(timeline, before);
    const bool crossfadeApplied = baselineReady
        && timeline.applyAudioCrossfade(0, 0, 1.25, &error);
    const QVector<ClipInfo> crossfadeState = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool crossfadeMutation = crossfadeApplied
        && crossfadeState.size() == 2
        && sameTransition(crossfadeState[0].trailOut, expectedCrossfade)
        && sameTransition(crossfadeState[1].leadIn, expectedCrossfade)
        && crossfadeState[0].leadIn.type == TransitionType::None
        && crossfadeState[1].trailOut.type == TransitionType::None
        && timeline.canUndo();
    if (crossfadeApplied)
        timeline.undo();
    const QVector<ClipInfo> crossfadeRestored = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool crossfadeUndo = crossfadeApplied
        && sameClipIdentity(crossfadeRestored, before)
        && transitionsAreNone(crossfadeRestored)
        && !timeline.canUndo();
    gate(1, "applyAudioCrossfade pairs A.trailOut/B.leadIn and one undo restores it",
         crossfadeMutation && crossfadeUndo, passed, failed);

    const QString legacy = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("1"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, false, 1.0);
    const QString fadeOff = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("1"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, false, 1.0,
        TransitionType::None, 0.0, TransitionType::None, 0.0);
    const QString fadeOn = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("1"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, false, 1.0,
        TransitionType::FadeIn, 0.75, TransitionType::FadeOut, 0.5);
    const QString reversedFade = buildExportAudioMixEntryFilterChain(
        0, QStringLiteral("1"), QStringLiteral("4"), 0,
        QStringLiteral("1"), AudioChannelMode::Stereo, true, 2.0,
        TransitionType::FadeIn, 0.75);
    const int reversePos = reversedFade.indexOf(QStringLiteral("areverse"));
    const int atempoPos = reversedFade.indexOf(QStringLiteral("atempo="));
    const int afadePos = reversedFade.indexOf(QStringLiteral("afade="));
    const bool g2 = legacy == fadeOff
        && fadeOn.contains(QStringLiteral("afade=t=in"))
        && fadeOn.contains(QStringLiteral("afade=t=out"))
        && fadeOn.contains(QStringLiteral("curve=qsin"))
        && reversePos >= 0 && atempoPos > reversePos && afadePos > atempoPos;
    gate(2, "export keeps the no-fade chain byte-identical and emits qsin afade",
         g2, passed, failed);

    const QVector<ClipInfo> fadeBefore{testClip(QStringLiteral("fade"))};
    const bool fadeInBaseline = saveAudioBaseline(timeline, fadeBefore);
    const bool fadeIn = fadeInBaseline
        && timeline.applyAudioFade(0, 0, AudioFadeEdge::In, 0.4, &error);
    const QVector<ClipInfo> fadeInState = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool fadeInMutation = fadeIn
        && fadeInState.size() == 1
        && fadeInState[0].leadIn.type == TransitionType::FadeIn
        && std::abs(fadeInState[0].leadIn.duration - 0.4) < 1e-9
        && fadeInState[0].trailOut.type == TransitionType::None
        && timeline.canUndo();
    if (fadeIn)
        timeline.undo();
    const QVector<ClipInfo> fadeInRestored = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool fadeInUndo = fadeIn
        && sameClipIdentity(fadeInRestored, fadeBefore)
        && transitionsAreNone(fadeInRestored)
        && !timeline.canUndo();

    const bool fadeOutBaseline = saveAudioBaseline(timeline, fadeBefore);
    const bool fadeOut = fadeOutBaseline
        && timeline.applyAudioFade(0, 0, AudioFadeEdge::Out, 0.6, &error);
    const QVector<ClipInfo> fadeOutState = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool fadeOutMutation = fadeOut
        && fadeOutState.size() == 1
        && fadeOutState[0].trailOut.type == TransitionType::FadeOut
        && std::abs(fadeOutState[0].trailOut.duration - 0.6) < 1e-9
        && fadeOutState[0].leadIn.type == TransitionType::None
        && timeline.canUndo();
    if (fadeOut)
        timeline.undo();
    const QVector<ClipInfo> fadeOutRestored = audioTrack
        ? audioTrack->clips() : QVector<ClipInfo>{};
    const bool fadeOutUndo = fadeOut
        && sameClipIdentity(fadeOutRestored, fadeBefore)
        && transitionsAreNone(fadeOutRestored)
        && !timeline.canUndo();
    gate(3, "applyAudioFade sets each requested edge and one undo restores it",
         fadeInMutation && fadeInUndo && fadeOutMutation && fadeOutUndo,
         passed, failed);

    QVector<ClipInfo> nonAdjacent{testClip(QStringLiteral("A")),
                                  testClip(QStringLiteral("B"))};
    error.clear();
    const bool nonAdjacentRejected = !audioxfade::applyCrossfade(
        nonAdjacent, 1, 1.0, &error) && !error.isEmpty();
    gate(4, "non-adjacent audio crossfade is rejected without mutation",
         nonAdjacentRejected
             && nonAdjacent[0].trailOut.type == TransitionType::None
             && nonAdjacent[1].leadIn.type == TransitionType::None,
         passed, failed);

    Timeline mirrorTimeline;
    TimelineTrack *videoTrack = mirrorTimeline.trackAt(false, 0);
    TimelineTrack *mirrorAudioTrack = mirrorTimeline.trackAt(true, 0);
    const QVector<ClipInfo> videoBefore{testClip(QStringLiteral("video-a")),
                                        testClip(QStringLiteral("video-b"))};
    const QVector<ClipInfo> audioBefore{testClip(QStringLiteral("audio-a")),
                                        testClip(QStringLiteral("audio-b"))};
    if (videoTrack)
        videoTrack->setClips(videoBefore);
    if (mirrorAudioTrack)
        mirrorAudioTrack->setClips(audioBefore);
    if (videoTrack)
        videoTrack->setSelectedClip(0);
    const Transition mirrorTransition{
        TransitionType::CrossDissolve, 1.0,
        TransitionAlignment::Center, TransitionEasing::Linear};
    if (videoTrack && mirrorAudioTrack)
        mirrorTimeline.applyTransitionToSelected(mirrorTransition);
    const QVector<ClipInfo> mirroredAudio = mirrorAudioTrack
        ? mirrorAudioTrack->clips() : QVector<ClipInfo>{};
    gate(5, "applyTransitionToSelected mirrors CrossDissolve to the A1 cut",
         mirroredAudio.size() == 2
             && sameTransition(mirroredAudio[0].trailOut, mirrorTransition)
             && sameTransition(mirroredAudio[1].leadIn, mirrorTransition)
             && mirroredAudio[0].leadIn.type == TransitionType::None
             && mirroredAudio[1].trailOut.type == TransitionType::None,
         passed, failed);

    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
