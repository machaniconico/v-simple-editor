#include "../Timeline.h"
#include "../UndoManager.h"

#include <QDebug>
#include <QString>
#include <QVector>

#include <cmath>

namespace {

constexpr double kEps = 1e-6;

ClipInfo makeClip(const QString &name, double durationSec, double leadInSec = 0.0)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("/selftests/") + name + QStringLiteral(".mov");
    clip.displayName = name;
    clip.duration = durationSec;
    clip.outPoint = durationSec;
    clip.leadInSec = leadInSec;
    return clip;
}

bool near(double a, double b)
{
    return std::fabs(a - b) <= kEps;
}

QVector<double> clipStarts(const QVector<ClipInfo> &clips)
{
    QVector<double> starts;
    double cursor = 0.0;
    for (const ClipInfo &clip : clips) {
        const double start = cursor + clip.leadInSec;
        starts.append(start);
        cursor = start + clip.effectiveDuration();
    }
    return starts;
}

bool sameClip(const ClipInfo &a, const ClipInfo &b)
{
    return a.filePath == b.filePath
        && a.displayName == b.displayName
        && near(a.duration, b.duration)
        && near(a.inPoint, b.inPoint)
        && near(a.outPoint, b.outPoint)
        && near(a.leadInSec, b.leadInSec)
        && near(a.speed, b.speed);
}

bool sameTracks(const QVector<QVector<ClipInfo>> &a,
                const QVector<QVector<ClipInfo>> &b)
{
    if (a.size() != b.size())
        return false;
    for (int track = 0; track < a.size(); ++track) {
        if (a[track].size() != b[track].size())
            return false;
        for (int clip = 0; clip < a[track].size(); ++clip) {
            if (!sameClip(a[track][clip], b[track][clip]))
                return false;
        }
    }
    return true;
}

bool sameState(const TimelineState &a, const TimelineState &b)
{
    return sameTracks(a.videoTracks, b.videoTracks)
        && sameTracks(a.audioTracks, b.audioTracks)
        && a.selectedClip == b.selectedClip
        && a.selectedVideoTrackIndex == b.selectedVideoTrackIndex
        && a.selectedVideoClipIndex == b.selectedVideoClipIndex
        && a.selectedAudioTrackIndex == b.selectedAudioTrackIndex
        && a.selectedAudioClipIndex == b.selectedAudioClipIndex
        && near(a.playheadPos, b.playheadPos)
        && a.projectWidth == b.projectWidth
        && a.projectHeight == b.projectHeight
        && a.projectExplicitOutput == b.projectExplicitOutput;
}

bool expect(bool condition, const char *gate, const QString &message,
            int &passed, int &failed)
{
    if (condition) {
        ++passed;
        qInfo().noquote() << "[ripple-delete] PASS" << gate;
        return true;
    }
    ++failed;
    qWarning().noquote() << "[ripple-delete] FAIL" << gate << ":" << message;
    return false;
}

void resetUndoBaseline(Timeline &timeline, const QString &description)
{
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), description);
}

void clearSelections(Timeline &timeline)
{
    for (auto *track : timeline.videoTracks()) {
        if (track)
            track->clearClipSelection();
    }
    for (auto *track : timeline.audioTracks()) {
        if (track)
            track->clearClipSelection();
    }
}

void ensureTwoVideoAndAudioTracks(Timeline &timeline)
{
    while (timeline.videoTracks().size() < 2)
        timeline.addVideoTrack();
    while (timeline.audioTracks().size() < 2)
        timeline.addAudioTrack();
}

void installRippleFixture(Timeline &timeline)
{
    ensureTwoVideoAndAudioTracks(timeline);

    timeline.videoTracks()[0]->setClips({
        makeClip(QStringLiteral("v1-a"), 2.0),
        makeClip(QStringLiteral("v1-b"), 2.0),
        makeClip(QStringLiteral("v1-c"), 2.0),
    });
    timeline.videoTracks()[1]->setClips({
        makeClip(QStringLiteral("v2-a"), 1.0),
        makeClip(QStringLiteral("v2-b"), 1.0, 1.0),
        makeClip(QStringLiteral("v2-c"), 2.0, 2.0),
    });
    timeline.audioTracks()[0]->setClips({
        makeClip(QStringLiteral("a1-a"), 2.0),
        makeClip(QStringLiteral("a1-b"), 2.0),
        makeClip(QStringLiteral("a1-c"), 2.0),
    });
    timeline.audioTracks()[1]->setClips({
        makeClip(QStringLiteral("a2-a"), 1.0),
        makeClip(QStringLiteral("a2-b"), 2.0, 3.0),
    });

    clearSelections(timeline);
    timeline.videoTracks()[0]->setSelectedClip(1);
}

void installGapFixture(Timeline &timeline)
{
    ensureTwoVideoAndAudioTracks(timeline);

    timeline.videoTracks()[0]->setClips({
        makeClip(QStringLiteral("gap-v1-a"), 2.0),
        makeClip(QStringLiteral("gap-v1-b"), 2.0, 2.0),
    });
    timeline.videoTracks()[1]->setClips({
        makeClip(QStringLiteral("gap-v2-a"), 1.0),
        makeClip(QStringLiteral("gap-v2-b"), 1.0, 3.0),
    });
    timeline.audioTracks()[0]->setClips({
        makeClip(QStringLiteral("gap-a1-a"), 2.0),
        makeClip(QStringLiteral("gap-a1-b"), 2.0, 2.0),
    });
    timeline.audioTracks()[1]->setClips({
        makeClip(QStringLiteral("gap-a2-a"), 1.0),
        makeClip(QStringLiteral("gap-a2-b"), 1.0, 3.0),
    });

    clearSelections(timeline);
}

} // namespace

int runRippleDeleteSelftest()
{
    qInfo().noquote() << "[ripple-delete] selftest start";
    int passed = 0;
    int failed = 0;

    {
        Timeline timeline;
        installRippleFixture(timeline);
        const TimelineState before = timeline.currentState();
        resetUndoBaseline(timeline, QStringLiteral("ripple baseline"));

        timeline.rippleDeleteSelectedClip();

        const auto v1 = timeline.videoTracks()[0]->clips();
        const auto v2 = timeline.videoTracks()[1]->clips();
        const auto a1 = timeline.audioTracks()[0]->clips();
        const auto a2 = timeline.audioTracks()[1]->clips();
        const auto v1Starts = clipStarts(v1);
        const auto v2Starts = clipStarts(v2);
        const auto a1Starts = clipStarts(a1);
        const auto a2Starts = clipStarts(a2);

        const bool rippled = v1.size() == 2
            && v1[0].displayName == QStringLiteral("v1-a")
            && v1[1].displayName == QStringLiteral("v1-c")
            && near(v1Starts[0], 0.0) && near(v1Starts[1], 2.0)
            && v2.size() == 2
            && v2[0].displayName == QStringLiteral("v2-a")
            && v2[1].displayName == QStringLiteral("v2-c")
            && near(v2Starts[0], 0.0) && near(v2Starts[1], 3.0)
            && a1.size() == 2
            && a1[0].displayName == QStringLiteral("a1-a")
            && a1[1].displayName == QStringLiteral("a1-c")
            && near(a1Starts[0], 0.0) && near(a1Starts[1], 2.0)
            && a2.size() == 2
            && a2[0].displayName == QStringLiteral("a2-a")
            && a2[1].displayName == QStringLiteral("a2-b")
            && near(a2Starts[0], 0.0) && near(a2Starts[1], 2.0);
        expect(rippled, "G1 selected range all-track ripple",
               QStringLiteral("selected clip range did not ripple-delete every track"),
               passed, failed);

        timeline.undo();
        expect(sameState(before, timeline.currentState()) && !timeline.canUndo(),
               "G2 ripple undo one step restores",
               QStringLiteral("single undo did not restore the pre-ripple state"),
               passed, failed);
    }

    {
        Timeline timeline;
        installGapFixture(timeline);
        const TimelineState before = timeline.currentState();
        resetUndoBaseline(timeline, QStringLiteral("gap baseline"));

        const bool closed = timeline.closeGapAt(timeline.videoTracks()[0], 3.0);
        const auto v1Starts = clipStarts(timeline.videoTracks()[0]->clips());
        const auto v2Starts = clipStarts(timeline.videoTracks()[1]->clips());
        const auto a1Starts = clipStarts(timeline.audioTracks()[0]->clips());
        const auto a2Starts = clipStarts(timeline.audioTracks()[1]->clips());

        const bool gapClosed = closed
            && near(v1Starts[0], 0.0) && near(v1Starts[1], 2.0)
            && near(v2Starts[0], 0.0) && near(v2Starts[1], 2.0)
            && near(a1Starts[0], 0.0) && near(a1Starts[1], 2.0)
            && near(a2Starts[0], 0.0) && near(a2Starts[1], 2.0);
        expect(gapClosed, "G3 gap close all-track ripple",
               QStringLiteral("gap close did not move downstream clips on every track"),
               passed, failed);

        timeline.undo();
        expect(sameState(before, timeline.currentState()) && !timeline.canUndo(),
               "G4 gap undo one step restores",
               QStringLiteral("single undo did not restore the pre-gap-close state"),
               passed, failed);
    }

    {
        Timeline timeline;
        installRippleFixture(timeline);
        clearSelections(timeline);
        const TimelineState before = timeline.currentState();
        resetUndoBaseline(timeline, QStringLiteral("no selection baseline"));
        const int undoIndex = timeline.undoManager()->currentIndex();

        timeline.rippleDeleteSelectedClip();

        expect(!timeline.hasAnySelection()
                   && timeline.undoManager()->currentIndex() == undoIndex
                   && sameState(before, timeline.currentState()),
               "G5 no selection no-op",
               QStringLiteral("ripple delete changed state or pushed undo without selection"),
               passed, failed);
    }

    qInfo().noquote() << "[ripple-delete] summary:"
                      << passed << "PASS," << failed << "FAIL";
    return failed == 0 ? 0 : 1;
}
