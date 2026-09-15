#include "../ProjectDiff.h"
#include "../ProjectFile.h"

#include <cstdio>

int runProjectDiffSelftest()
{
    using namespace projdiff;
    int passed = 0, failed = 0;
    auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    auto count = [](const QVector<Change> &changes, Change::Type type) {
        int result = 0;
        for (const auto &change : changes) if (change.type == type) ++result;
        return result;
    };
    ClipInfo clip{};
    clip.filePath = QStringLiteral("source.mp4");
    clip.duration = 20.0;
    clip.outPoint = 5.0;
    ProjectData a;
    a.videoTracks = {{clip}};
    a.audioTracks = {{clip}};
    ProjectData b = a;
    b.config.name = QStringLiteral("ignored");
    b.playheadPos = 123.0;
    b.vfxState.glow.enabled = true;
    b.rotoClipEntries.append(RotoClipEntry{});
    b.planarTracks.append(planartrack::PlanarTrack{});
    b.trackFlags = {{"video", QJsonArray{QJsonObject{{"locked", false}, {"muted", false}}}}};
    ProjectData roundtrip;
    const bool loaded = ProjectFile::fromJsonString(ProjectFile::toJsonString(a), roundtrip);
    gate(1, diff(a, a).isEmpty() && diff(a, b).isEmpty()
         && loaded && diff(a, roundtrip).isEmpty());

    b = a;
    ClipInfo added = clip;
    added.filePath = QStringLiteral("added.mp4");
    b.videoTracks[0].append(added);
    b.audioTracks[0].clear();
    const auto additions = diff(a, b);
    gate(2, additions.size() == 2 && count(additions, Change::Added) == 1
         && count(additions, Change::Removed) == 1
         && additions[0].path == QStringLiteral("video[0].clips[1]")
         && additions[1].path == QStringLiteral("audio[0].clips[0]"));

    b = a;
    b.videoTracks[0][0].leadInSec = 2.0;
    const auto move = diff(a, b);
    b = a;
    b.videoTracks[0][0].inPoint = 1.0;
    b.videoTracks[0][0].duration = 19.0;
    const auto trim = diff(a, b);
    gate(3, move.size() == 1 && count(move, Change::Moved) == 1
         && move[0].path.endsWith(QStringLiteral(".start"))
         && trim.size() == 1 && count(trim, Change::Trimmed) == 1);

    b = a;
    VideoEffect effect;
    effect.type = VideoEffectType::Blur;
    effect.param1 = 0.25;
    b.videoTracks[0][0].effects.append(effect);
    const auto effectAdded = diff(a, b);
    ProjectData c = b;
    c.videoTracks[0][0].effects[0].param1 = 0.75;
    const auto effectEdited = diff(b, c);
    gate(4, effectAdded.size() == 1 && count(effectAdded, Change::EffectsChanged) == 1
         && effectEdited.size() == 1 && count(effectEdited, Change::EffectsChanged) == 1
         && effectEdited[0].path == QStringLiteral("video[0].clips[0].effects"));

    b = a;
    b.videoTracks[0][0].trailOut.type = TransitionType::FadeOut;
    const auto transitionAdded = diff(a, b);
    c = b;
    c.videoTracks[0][0].trailOut.duration += 0.2;
    const auto transitionEdited = diff(b, c);
    gate(5, transitionAdded.size() == 1 && count(transitionAdded, Change::TransitionChanged) == 1
         && transitionEdited.size() == 1 && count(transitionEdited, Change::TransitionChanged) == 1
         && transitionEdited[0].path == QStringLiteral("video[0].clips[0].trailOut"));

    b = a;
    ClipInfo duplicate = clip;
    duplicate.inPoint = 10.0;
    duplicate.outPoint = 15.0;
    b.videoTracks[0].append(duplicate);
    c = b;
    c.videoTracks[0] = {duplicate, clip};
    c.videoTracks[0][1].inPoint = 0.2;
    const auto duplicates = diff(b, c);
    bool correctTrim = false;
    for (const auto &change : duplicates)
        if (change.type == Change::Trimmed)
            correctTrim = change.path == QStringLiteral("video[0].clips[1].trim");
    c = b;
    c.videoTracks.append({c.videoTracks[0].takeLast()});
    const auto crossTrack = diff(b, c);
    gate(6, duplicates.size() == 3 && count(duplicates, Change::Moved) == 2
         && count(duplicates, Change::Trimmed) == 1 && correctTrim
         && crossTrack.size() == 1 && count(crossTrack, Change::Moved) == 1);

    b = a;
    b.videoTracks[0][0].effects.append(effect);
    c = b;
    auto &small = c.videoTracks[0][0];
    small.inPoint += 0.0001;
    small.duration += 0.0001;
    small.leadInSec += 0.0001;
    small.volume += 0.0001;
    small.pan += 0.0001;
    small.material.diffuseCoeff += 0.0001;
    small.colorCorrection.brightness += 0.0001;
    small.colorCorrection.liftR += 0.0001;
    small.hslSecondary.hueCenter += 0.0001;
    small.effects[0].param1 += 0.0001;
    small.effects[0].startSec += 0.0001;
    small.trailOut.softness += 0.0001;
    const bool epsilonIgnored = diff(b, c).isEmpty() && !diff(b, c, 0.0).isEmpty();
    c = b;
    c.videoTracks[0][0].volume += 0.125;
    const bool epsilonBoundary = diff(b, c, 0.125).isEmpty()
        && !diff(b, c, 0.124).isEmpty();
    c = b;
    c.videoTracks[0][0].material.diffuseCoeff = 0.9;
    const auto materialChanges = diff(b, c);
    const bool materialChanged = materialChanges.size() == 1
        && materialChanges[0].type == Change::PropertyChanged
        && materialChanges[0].path.endsWith(QStringLiteral(".layerMaterial"));
    c = b;
    c.audioTracks[0][0].volume = 0.5;
    c.trackFlags = {{"audio", QJsonArray{QJsonObject{{"muted", true}}}}};
    const auto properties = diff(b, c);
    gate(7, epsilonIgnored && epsilonBoundary && materialChanged && properties.size() == 2
         && count(properties, Change::PropertyChanged) == 1
         && count(properties, Change::TrackFlagChanged) == 1
         && properties[0].path == QStringLiteral("audio[0].clips[0].volume"));

    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
