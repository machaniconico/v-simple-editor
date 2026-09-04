#include <QVector>

#include <cmath>
#include <iostream>

#include "../MusicRemix.h"
#include "../Timeline.h"
#include "../UndoManager.h"

int runMusicRemixSelftest()
{
    int passed = 0;
    int failed = 0;
    const auto pass = [&](const char *gate) {
        ++passed;
        std::cerr << "PASS " << gate << '\n';
    };
    const auto fail = [&](const char *gate, const QString &reason) {
        ++failed;
        std::cerr << "FAIL " << gate << " " << reason.toStdString() << '\n';
    };

    QVector<double> beats;
    for (int i = 0; i < 60; ++i)
        beats.append(static_cast<double>(i));

    const remix::Plan shortened = remix::planRemix(beats, 60.0, 40.0);
    const remix::Plan shortenedAgain = remix::planRemix(beats, 60.0, 40.0);
    bool deterministic = shortened.segments.size() == shortenedAgain.segments.size();
    for (int i = 0; deterministic && i < shortened.segments.size(); ++i) {
        deterministic = qAbs(shortened.segments[i].srcStart
                              - shortenedAgain.segments[i].srcStart) <= 1.0e-12
            && qAbs(shortened.segments[i].srcEnd
                    - shortenedAgain.segments[i].srcEnd) <= 1.0e-12;
    }
    const bool g1 = shortened.valid
        && deterministic
        && qAbs(shortened.resultDuration - 40.0) <= 1.0;
    g1 ? pass("G1")
       : fail("G1", QStringLiteral("shortening result=%1 segments=%2")
                          .arg(shortened.resultDuration)
                          .arg(shortened.segments.size()));

    const remix::Plan extended = remix::planRemix(beats, 60.0, 80.0);
    const bool g2 = extended.valid
        && extended.resultDuration >= 60.0
        && qAbs(extended.resultDuration - 80.0) <= 1.0
        && extended.segments.size() > shortened.segments.size();
    g2 ? pass("G2")
       : fail("G2", QStringLiteral("extension result=%1 segments=%2")
                          .arg(extended.resultDuration)
                          .arg(extended.segments.size()));

    const remix::Plan heldEdges = remix::planRemix(beats, 60.0, 42.0);
    const bool g3 = heldEdges.valid
        && !heldEdges.segments.isEmpty()
        && qAbs(heldEdges.segments.first().srcStart) <= 1.0e-9
        && qAbs(heldEdges.segments.last().srcEnd - 60.0) <= 1.0e-9;
    g3 ? pass("G3")
       : fail("G3", QStringLiteral("edges were not retained"));

    const remix::Plan tooFewBeats = remix::planRemix({12.0}, 60.0, 40.0);
    const bool g4 = !tooFewBeats.valid && tooFewBeats.segments.isEmpty()
        && !tooFewBeats.error.isEmpty();
    g4 ? pass("G4")
       : fail("G4", QStringLiteral("beat-count guard did not fail"));

    Timeline timeline;
    TimelineTrack *audioTrack = timeline.trackAt(true, 0);
    ClipInfo original;
    original.filePath = QStringLiteral("synthetic-remix.wav");
    original.displayName = QStringLiteral("synthetic-remix.wav");
    original.duration = 10.0;
    original.outPoint = 10.0;
    ClipInfo following = original;
    following.filePath = QStringLiteral("following.wav");
    following.displayName = QStringLiteral("following.wav");
    following.duration = 4.0;
    following.outPoint = 4.0;
    following.leadInSec = 1.0;
    if (audioTrack) {
        audioTrack->setClips({original, following});
        timeline.clearSelection();
        timeline.undoManager()->clear();
        timeline.undoManager()->saveState(timeline.currentState(),
                                          QStringLiteral("music remix baseline"));

        remix::Plan fixedPlan;
        fixedPlan.segments = {{0.0, 2.0}, {4.0, 10.0}};
        fixedPlan.resultDuration = 8.0;
        fixedPlan.crossfadeSec = 0.05;
        QString error;
        const bool applied = timeline.applyMusicRemix(0, 0, fixedPlan,
                                                      false, &error);
        const QVector<ClipInfo> &after = audioTrack->clips();
        const bool replaced = applied && after.size() == 3
            && qAbs(after[0].inPoint - 0.0) <= 1.0e-9
            && qAbs(after[0].outPoint - 2.0) <= 1.0e-9
            && qAbs(after[1].inPoint - 4.0) <= 1.0e-9
            && qAbs(after[1].outPoint - 10.0) <= 1.0e-9
            && after[0].trailOut.type == TransitionType::CrossDissolve
            && after[1].leadIn.type == TransitionType::CrossDissolve
            && qAbs(after[2].leadInSec - 3.0) <= 1.0e-9;
        timeline.undo();
        const QVector<ClipInfo> &restored = audioTrack->clips();
        const bool undoRestored = restored.size() == 2
            && restored[0].filePath == original.filePath
            && qAbs(restored[0].inPoint - original.inPoint) <= 1.0e-9
            && qAbs(restored[0].outPoint - original.outPoint) <= 1.0e-9
            && restored[1].filePath == following.filePath
            && qAbs(restored[1].leadInSec - following.leadInSec) <= 1.0e-9
            && !timeline.canUndo();
        const bool g5 = replaced && undoRestored;
        g5 ? pass("G5")
           : fail("G5", error.isEmpty()
                              ? QStringLiteral("replacement or one-step undo diverged")
                              : error);
    } else {
        fail("G5", QStringLiteral("A1 track was not available"));
    }

    QVector<double> interiorBeats;
    for (int i = 1; i < 60; ++i)
        interiorBeats.append(static_cast<double>(i));
    const remix::Plan unreachableShort =
        remix::planRemix(interiorBeats, 60.0, 0.05);
    const bool shortRejected = !unreachableShort.valid
        && unreachableShort.segments.isEmpty()
        && !unreachableShort.error.isEmpty();

    const remix::Plan unreachableLong =
        remix::planRemix({0.0, 30.0}, 60.0, 120.0);
    const bool longRejected = !unreachableLong.valid
        && unreachableLong.segments.isEmpty()
        && !unreachableLong.error.isEmpty();
    const bool g6 = shortRejected && longRejected;
    g6 ? pass("G6")
       : fail("G6", QStringLiteral("unreachable target accepted: short=%1 long=%2")
                          .arg(shortRejected ? QStringLiteral("rejected")
                                             : QStringLiteral("accepted"))
                          .arg(longRejected ? QStringLiteral("rejected")
                                            : QStringLiteral("accepted")));

    std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    return failed;
}
