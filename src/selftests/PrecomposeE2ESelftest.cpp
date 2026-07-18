#include "../MainWindow.h"
#include "../Timeline.h"
#include "../UndoManager.h"

#include <QApplication>
#include <QDebug>
#include <QString>
#include <QVector>

#include <cmath>

namespace {

constexpr double kEps = 1.0e-6;

struct TrackSnapshot {
    QVector<QVector<ClipInfo>> videoTracks;
    QVector<QVector<ClipInfo>> audioTracks;
};

ClipInfo makeClip(const QString &id, double duration, int linkGroup = 0)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("precompose-e2e://%1.mov").arg(id);
    clip.displayName = id;
    clip.duration = duration;
    clip.outPoint = duration;
    clip.speed = 1.0;
    clip.volume = 1.0;
    clip.opacity = 1.0;
    clip.linkGroup = linkGroup;
    return clip;
}

TimelineSequence makeMainSequence(const QVector<ClipInfo> &videoClips,
                                  const QVector<ClipInfo> &audioClips)
{
    TimelineSequence sequence;
    sequence.id = QStringLiteral("main");
    sequence.name = QStringLiteral("Main");
    sequence.videoTracks = {videoClips};
    sequence.audioTracks = {audioClips};
    return sequence;
}

TrackSnapshot snapshotTracks(const Timeline *timeline)
{
    TrackSnapshot snapshot;
    if (!timeline)
        return snapshot;

    snapshot.videoTracks.reserve(timeline->videoTracks().size());
    for (const TimelineTrack *track : timeline->videoTracks())
        snapshot.videoTracks.append(track ? track->clips() : QVector<ClipInfo>{});

    snapshot.audioTracks.reserve(timeline->audioTracks().size());
    for (const TimelineTrack *track : timeline->audioTracks())
        snapshot.audioTracks.append(track ? track->clips() : QVector<ClipInfo>{});

    return snapshot;
}

bool nearlyEqual(double a, double b)
{
    return std::fabs(a - b) <= kEps;
}

bool sameClipForThisGate(const ClipInfo &a, const ClipInfo &b, QString *detail)
{
    auto fail = [detail](const QString &field) {
        if (detail)
            *detail = QStringLiteral("clip field differs: %1").arg(field);
        return false;
    };

    if (a.filePath != b.filePath)
        return fail(QStringLiteral("filePath"));
    if (a.displayName != b.displayName)
        return fail(QStringLiteral("displayName"));
    if (a.sequenceRefId != b.sequenceRefId)
        return fail(QStringLiteral("sequenceRefId"));
    if (!nearlyEqual(a.duration, b.duration))
        return fail(QStringLiteral("duration"));
    if (!nearlyEqual(a.inPoint, b.inPoint))
        return fail(QStringLiteral("inPoint"));
    if (!nearlyEqual(a.outPoint, b.outPoint))
        return fail(QStringLiteral("outPoint"));
    if (!nearlyEqual(a.leadInSec, b.leadInSec))
        return fail(QStringLiteral("leadInSec"));
    if (!nearlyEqual(a.speed, b.speed))
        return fail(QStringLiteral("speed"));
    if (!nearlyEqual(a.volume, b.volume))
        return fail(QStringLiteral("volume"));
    if (!nearlyEqual(a.pan, b.pan))
        return fail(QStringLiteral("pan"));
    if (!nearlyEqual(a.opacity, b.opacity))
        return fail(QStringLiteral("opacity"));
    if (a.visible != b.visible)
        return fail(QStringLiteral("visible"));
    if (a.isAdjustment != b.isAdjustment)
        return fail(QStringLiteral("isAdjustment"));
    if (a.linkGroup != b.linkGroup)
        return fail(QStringLiteral("linkGroup"));
    return true;
}

bool sameTracksForThisGate(const QVector<QVector<ClipInfo>> &actual,
                           const QVector<QVector<ClipInfo>> &expected,
                           const QString &axis,
                           QString *detail)
{
    if (actual.size() != expected.size()) {
        if (detail) {
            *detail = QStringLiteral("%1 track count is %2, expected %3")
                .arg(axis)
                .arg(actual.size())
                .arg(expected.size());
        }
        return false;
    }

    for (int trackIndex = 0; trackIndex < expected.size(); ++trackIndex) {
        if (actual[trackIndex].size() != expected[trackIndex].size()) {
            if (detail) {
                *detail = QStringLiteral("%1%2 clip count is %3, expected %4")
                    .arg(axis)
                    .arg(trackIndex + 1)
                    .arg(actual[trackIndex].size())
                    .arg(expected[trackIndex].size());
            }
            return false;
        }
        for (int clipIndex = 0; clipIndex < expected[trackIndex].size(); ++clipIndex) {
            QString clipDetail;
            if (!sameClipForThisGate(actual[trackIndex][clipIndex],
                                     expected[trackIndex][clipIndex],
                                     &clipDetail)) {
                if (detail) {
                    *detail = QStringLiteral("%1%2 clip %3: %4")
                        .arg(axis)
                        .arg(trackIndex + 1)
                        .arg(clipIndex)
                        .arg(clipDetail);
                }
                return false;
            }
        }
    }
    return true;
}

bool sameSnapshot(const Timeline *timeline,
                  const TrackSnapshot &expected,
                  QString *detail)
{
    const TrackSnapshot actual = snapshotTracks(timeline);
    return sameTracksForThisGate(actual.videoTracks, expected.videoTracks,
                                 QStringLiteral("V"), detail)
        && sameTracksForThisGate(actual.audioTracks, expected.audioTracks,
                                 QStringLiteral("A"), detail);
}

const TimelineSequence *findSequence(const QVector<TimelineSequence> &sequences,
                                     const QString &id)
{
    for (const TimelineSequence &sequence : sequences) {
        if (sequence.id == id)
            return &sequence;
    }
    return nullptr;
}

bool containsPrecomposeSequence(const QVector<TimelineSequence> &sequences)
{
    for (const TimelineSequence &sequence : sequences) {
        if (sequence.id.startsWith(QStringLiteral("precomp-")))
            return true;
    }
    return false;
}

void clearSelections(Timeline *timeline)
{
    if (!timeline)
        return;
    for (TimelineTrack *track : timeline->videoTracks()) {
        if (track)
            track->setSelectedClip(-1);
    }
    for (TimelineTrack *track : timeline->audioTracks()) {
        if (track)
            track->setSelectedClip(-1);
    }
}

bool assertStoreAndTracksRestored(Timeline *timeline,
                                  int baselineSequenceCount,
                                  const TrackSnapshot &baselineTracks,
                                  QString *detail)
{
    if (!timeline) {
        if (detail)
            *detail = QStringLiteral("timeline unavailable");
        return false;
    }

    const QVector<TimelineSequence> sequences = timeline->sequences();
    if (sequences.size() != baselineSequenceCount) {
        if (detail) {
            *detail = QStringLiteral("sequence count is %1, expected %2")
                .arg(sequences.size())
                .arg(baselineSequenceCount);
        }
        return false;
    }
    if (containsPrecomposeSequence(sequences)) {
        if (detail)
            *detail = QStringLiteral("precomp sequence remained in store");
        return false;
    }
    return sameSnapshot(timeline, baselineTracks, detail);
}

bool resetTimelineForPrecompose(MainWindow &window,
                                Timeline **timelineOut,
                                TrackSnapshot *baselineTracks,
                                int *baselineSequenceCount,
                                QString *detail)
{
    Timeline *timeline = window.findChild<Timeline *>();
    if (!timeline) {
        if (detail)
            *detail = QStringLiteral("MainWindow timeline child not found");
        return false;
    }

    const ClipInfo videoA = makeClip(QStringLiteral("video-a"), 1.0, 1);
    const ClipInfo videoB = makeClip(QStringLiteral("video-b"), 1.5, 2);
    const ClipInfo audioA = makeClip(QStringLiteral("audio-a"), 1.0, 1);
    const ClipInfo audioB = makeClip(QStringLiteral("audio-b"), 1.5, 2);

    timeline->setSequences(
        {makeMainSequence({videoA, videoB}, {audioA, audioB})},
        QStringLiteral("main"));
    clearSelections(timeline);

    if (!timeline->undoManager()) {
        if (detail)
            *detail = QStringLiteral("undo manager unavailable");
        return false;
    }
    timeline->undoManager()->clear();
    timeline->undoManager()->saveState(timeline->currentState(),
                                       QStringLiteral("precompose e2e baseline"));

    const QVector<TimelineSequence> baselineSequences = timeline->sequences();
    if (baselineSequences.size() != 1) {
        if (detail) {
            *detail = QStringLiteral("baseline sequence count is %1, expected 1")
                .arg(baselineSequences.size());
        }
        return false;
    }
    if (containsPrecomposeSequence(baselineSequences)) {
        if (detail)
            *detail = QStringLiteral("baseline unexpectedly contains precomp sequence");
        return false;
    }

    if (baselineTracks)
        *baselineTracks = snapshotTracks(timeline);
    if (baselineSequenceCount)
        *baselineSequenceCount = baselineSequences.size();
    if (timelineOut)
        *timelineOut = timeline;
    return true;
}

bool selectFirstVideoAudioClip(Timeline *timeline, QString *detail)
{
    if (!timeline
        || timeline->videoTracks().isEmpty()
        || timeline->audioTracks().isEmpty()
        || !timeline->videoTracks().first()
        || !timeline->audioTracks().first()) {
        if (detail)
            *detail = QStringLiteral("timeline V1/A1 unavailable");
        return false;
    }

    TimelineTrack *videoTrack = timeline->videoTracks().first();
    TimelineTrack *audioTrack = timeline->audioTracks().first();

    videoTrack->setSelectedClip(0);
    if (!videoTrack->selectedClips().contains(0)
        || !audioTrack->selectedClips().contains(0)) {
        if (detail)
            *detail = QStringLiteral("linked V1/A1 selection was not applied");
        return false;
    }
    return true;
}

bool assertPrecomposeReplacedSelection(Timeline *timeline,
                                       const QString &sequenceId,
                                       int baselineSequenceCount,
                                       QString *detail)
{
    if (!timeline) {
        if (detail)
            *detail = QStringLiteral("timeline unavailable");
        return false;
    }

    const QVector<TimelineSequence> sequences = timeline->sequences();
    if (sequences.size() != baselineSequenceCount + 1) {
        if (detail) {
            *detail = QStringLiteral("sequence count is %1, expected %2")
                .arg(sequences.size())
                .arg(baselineSequenceCount + 1);
        }
        return false;
    }

    const TimelineSequence *nested = findSequence(sequences, sequenceId);
    if (!nested) {
        if (detail)
            *detail = QStringLiteral("%1 missing from sequence store").arg(sequenceId);
        return false;
    }

    const QVector<ClipInfo> parentVideo =
        timeline->videoTracks().isEmpty() || !timeline->videoTracks().first()
            ? QVector<ClipInfo>{}
            : timeline->videoTracks().first()->clips();
    const QVector<ClipInfo> parentAudio =
        timeline->audioTracks().isEmpty() || !timeline->audioTracks().first()
            ? QVector<ClipInfo>{}
            : timeline->audioTracks().first()->clips();

    auto parentHasExpectedReference = [&](const QVector<ClipInfo> &clips,
                                          const QString &axis) {
        if (clips.size() != 2) {
            if (detail) {
                *detail = QStringLiteral("%1 parent clip count is %2, expected 2")
                    .arg(axis)
                    .arg(clips.size());
            }
            return false;
        }
        if (!clips[0].isSequenceReference() || clips[0].sequenceRefId != sequenceId) {
            if (detail) {
                *detail = QStringLiteral("%1 parent first clip is not %2 reference")
                    .arg(axis, sequenceId);
            }
            return false;
        }
        if (!nearlyEqual(clips[0].duration, 1.0)
            || !nearlyEqual(clips[0].outPoint, 1.0)) {
            if (detail) {
                *detail = QStringLiteral("%1 parent reference duration mismatch")
                    .arg(axis);
            }
            return false;
        }
        if (clips[1].isSequenceReference()) {
            if (detail) {
                *detail = QStringLiteral("%1 parent tail clip was unexpectedly replaced")
                    .arg(axis);
            }
            return false;
        }
        return true;
    };

    if (!parentHasExpectedReference(parentVideo, QStringLiteral("V1")))
        return false;
    if (!parentHasExpectedReference(parentAudio, QStringLiteral("A1")))
        return false;

    if (nested->videoTracks.isEmpty()
        || nested->audioTracks.isEmpty()
        || nested->videoTracks.first().size() != 1
        || nested->audioTracks.first().size() != 1) {
        if (detail)
            *detail = QStringLiteral("nested sequence did not capture one V1/A1 clip");
        return false;
    }
    if (nested->videoTracks.first().first().filePath
            != QStringLiteral("precompose-e2e://video-a.mov")
        || nested->audioTracks.first().first().filePath
            != QStringLiteral("precompose-e2e://audio-a.mov")) {
        if (detail)
            *detail = QStringLiteral("nested sequence captured wrong source clips");
        return false;
    }
    return true;
}

bool runSinglePrecomposeScenario(MainWindow &window, QString *detail)
{
    Timeline *timeline = nullptr;
    TrackSnapshot baselineTracks;
    int baselineSequenceCount = 0;
    if (!resetTimelineForPrecompose(window, &timeline, &baselineTracks,
                                    &baselineSequenceCount, detail)
        || !selectFirstVideoAudioClip(timeline, detail)) {
        return false;
    }

    const MainWindow::PrecomposeResult result =
        window.precomposeSelectionWithName(QStringLiteral("Comp 1"));
    if (!result.success) {
        if (detail)
            *detail = QStringLiteral("precompose failed: %1").arg(result.failureReason);
        return false;
    }
    if (!assertPrecomposeReplacedSelection(timeline, result.sequenceId,
                                           baselineSequenceCount, detail)) {
        return false;
    }

    timeline->undo();
    return assertStoreAndTracksRestored(timeline, baselineSequenceCount,
                                        baselineTracks, detail);
}

bool runDoublePrecomposeScenario(MainWindow &window, QString *detail)
{
    Timeline *timeline = nullptr;
    TrackSnapshot baselineTracks;
    int baselineSequenceCount = 0;
    if (!resetTimelineForPrecompose(window, &timeline, &baselineTracks,
                                    &baselineSequenceCount, detail)
        || !selectFirstVideoAudioClip(timeline, detail)) {
        return false;
    }

    const MainWindow::PrecomposeResult first =
        window.precomposeSelectionWithName(QStringLiteral("Comp 1"));
    if (!first.success) {
        if (detail)
            *detail = QStringLiteral("first precompose failed: %1").arg(first.failureReason);
        return false;
    }

    const MainWindow::PrecomposeResult second =
        window.precomposeSelectionWithName(QStringLiteral("Comp 2"));
    if (!second.success) {
        if (detail)
            *detail = QStringLiteral("second precompose failed: %1").arg(second.failureReason);
        return false;
    }
    if (first.sequenceId == second.sequenceId) {
        if (detail)
            *detail = QStringLiteral("second precompose reused %1").arg(first.sequenceId);
        return false;
    }
    const QVector<TimelineSequence> afterSecond = timeline->sequences();
    if (afterSecond.size() != baselineSequenceCount + 2
        || !findSequence(afterSecond, first.sequenceId)
        || !findSequence(afterSecond, second.sequenceId)) {
        if (detail)
            *detail = QStringLiteral("two precompose sequences not present before undo");
        return false;
    }

    timeline->undo();
    timeline->undo();
    return assertStoreAndTracksRestored(timeline, baselineSequenceCount,
                                        baselineTracks, detail);
}

} // namespace

int runPrecomposeE2ESelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[precompose-e2e] PASS G%1 %2")
                .arg(gate)
                .arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[precompose-e2e] FAIL G%1 %2%3")
                .arg(gate)
                .arg(QString::fromLatin1(name))
                .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
        }
    };

    check(1, "QApplication is available", QApplication::instance() != nullptr);
    if (failed)
        return failed;

    MainWindow window;
    QString detail;
    check(2, "MainWindow timeline is available",
          window.findChild<Timeline *>() != nullptr);
    if (failed)
        return failed;

    detail.clear();
    check(3, "single precompose replaces selection and undo restores store/tracks",
          runSinglePrecomposeScenario(window, &detail),
          detail);

    detail.clear();
    check(4, "double precompose two undos restore store/tracks",
          runDoublePrecomposeScenario(window, &detail),
          detail);

    qInfo().noquote() << QStringLiteral("[precompose-e2e] summary: %1 PASS, %2 FAIL")
        .arg(passed)
        .arg(failed);
    return failed == 0 ? 0 : failed;
}
