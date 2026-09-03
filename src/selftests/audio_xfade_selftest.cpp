#include "../Timeline.h"

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

    QVector<ClipInfo> clips{testClip(QStringLiteral("A")),
                            testClip(QStringLiteral("B"))};
    const QVector<ClipInfo> before = clips;
    QString error;
    const bool crossfadeApplied = audioxfade::applyCrossfade(
        clips, 0, 1.25, &error);
    const Transition expectedCrossfade{
        TransitionType::CrossDissolve, 1.25,
        TransitionAlignment::Center, TransitionEasing::Linear};
    const bool g1Mutation = crossfadeApplied
        && sameTransition(clips[0].trailOut, expectedCrossfade)
        && sameTransition(clips[1].leadIn, expectedCrossfade)
        && clips[0].leadIn.type == TransitionType::None
        && clips[1].trailOut.type == TransitionType::None;
    // The public Timeline API stores the post-mutation state as one undo
    // entry. This pure gate mirrors that contract without constructing a
    // QWidget, as this selftest is intentionally needsQApplication=false.
    QVector<ClipInfo> undoState = clips;
    undoState = before;
    gate(1, "applyAudioCrossfade pairs A.trailOut/B.leadIn and one undo restores it",
         g1Mutation && sameClipIdentity(undoState, before), passed, failed);

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

    QVector<ClipInfo> fadeClips{testClip(QStringLiteral("fade"))};
    const bool fadeIn = audioxfade::applyFade(
        fadeClips, 0, AudioFadeEdge::In, 0.4, &error);
    const bool fadeOut = audioxfade::applyFade(
        fadeClips, 0, AudioFadeEdge::Out, 0.6, &error);
    gate(3, "applyAudioFade sets both clip edges independently",
         fadeIn && fadeOut
             && fadeClips[0].leadIn.type == TransitionType::FadeIn
             && fadeClips[0].trailOut.type == TransitionType::FadeOut
             && std::abs(fadeClips[0].leadIn.duration - 0.4) < 1e-9
             && std::abs(fadeClips[0].trailOut.duration - 0.6) < 1e-9,
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

    QVector<ClipInfo> video{testClip(QStringLiteral("video"))};
    const QVector<ClipInfo> videoBefore = video;
    QVector<ClipInfo> audio{testClip(QStringLiteral("audio-a")),
                            testClip(QStringLiteral("audio-b"))};
    const bool audioOnly = audioxfade::applyCrossfade(audio, 0, 1.0, &error);
    gate(5, "audio transition mutation leaves video-side mirror behavior untouched",
         audioOnly && sameClipIdentity(video, videoBefore)
             && audio[0].trailOut.type == TransitionType::CrossDissolve
             && audio[1].leadIn.type == TransitionType::CrossDissolve,
         passed, failed);

    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
