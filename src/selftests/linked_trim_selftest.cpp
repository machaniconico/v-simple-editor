// US-705 LINKED-TRIM: edge-drag trims (DragMode::TrimLeft / TrimRight) on a
// linkGroup member propagate to every member; Alt+drag trims only the
// grabbed clip. Drags go through real QMouseEvents on the TimelineTrack.
#include "../Timeline.h"
#include "../UndoManager.h"
#include "SelftestGate.h"

#include <QtTest/QTest>

#include <cmath>
#include <cstdio>

namespace {

ClipInfo makeClip(int group, double in = 1.0, double out = 5.0,
                  double duration = 10.0, double leadIn = 2.0, double speed = 1.0)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("/selftests/linked-trim.mp4");
    clip.duration = duration;
    clip.inPoint = in;
    clip.outPoint = out;
    clip.leadInSec = leadIn;
    clip.speed = speed;
    clip.linkGroup = group;
    return clip;
}

void setup(Timeline &timeline, const QVector<QVector<ClipInfo>> &video,
           const QVector<QVector<ClipInfo>> &audio)
{
    timeline.restoreFromProject(video, audio, 0.0, -1.0, -1.0, 100);
    timeline.setSnapEnabled(false);
    for (auto *t : timeline.videoTracks()) t->setSnapEnabled(false);
    for (auto *t : timeline.audioTracks()) t->setSnapEnabled(false);
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("基準"));
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

// Press on the clip's left / right trim handle, move by deltaSec, release.
void dragEdge(TimelineTrack *track, int clipIdx, bool rightEdge, double deltaSec,
              Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const double pps = track->pixelsPerSecond();
    const ClipInfo &clip = track->clips()[clipIdx];
    const int width = qMax(20, static_cast<int>(clip.effectiveDuration() * pps));
    const int x0 = track->clipStartX(clipIdx) + (rightEdge ? width - 2 : 2);
    const int y = track->rowHeight() / 2;
    const int x1 = x0 + static_cast<int>(std::lround(deltaSec * pps));
    QTest::mousePress(track, Qt::LeftButton, mods, QPoint(x0, y));
    QTest::mouseMove(track, QPoint(x1, y));
    QTest::mouseRelease(track, Qt::LeftButton, mods, QPoint(x1, y));
}

double clipStartSec(const TimelineTrack *track, int idx)
{
    double t = 0.0;
    const auto &clips = track->clips();
    for (int i = 0; i < idx && i < clips.size(); ++i)
        t += clips[i].leadInSec + clips[i].effectiveDuration();
    return t + clips[idx].leadInSec;
}

} // namespace

int runLinkedTrimSelftest()
{
    selftest::GateTally tally;
    Timeline timeline;
    char buf[512];

    // G1: AV pair right edge -0.5 s -> both outPoints 5.0 -> 4.5.
    setup(timeline, {{makeClip(5)}}, {{makeClip(5)}});
    {
        auto *v1 = timeline.videoTracks()[0];
        auto *a1 = timeline.audioTracks()[0];
        dragEdge(v1, 0, true, -0.5);
        const double vo = v1->clips()[0].outPoint, ao = a1->clips()[0].outPoint;
        std::fprintf(stderr, "[linked-trim] G1 V1.out=%.6f A1.out=%.6f (expect 4.5)\n", vo, ao);
        tally.gate(1, near(vo, 4.5) && near(ao, 4.5));
    }

    // G2: AV pair left edge +0.5 s -> inPoint and leadInSec both +0.5.
    setup(timeline, {{makeClip(5)}}, {{makeClip(5)}});
    {
        auto *v1 = timeline.videoTracks()[0];
        auto *a1 = timeline.audioTracks()[0];
        dragEdge(v1, 0, false, 0.5);
        const ClipInfo &v = v1->clips()[0];
        const ClipInfo &a = a1->clips()[0];
        std::fprintf(stderr, "[linked-trim] G2 V1.in=%.6f V1.lead=%.6f A1.in=%.6f A1.lead=%.6f"
                     " (expect in 1.5, lead 2.5)\n", v.inPoint, v.leadInSec, a.inPoint, a.leadInSec);
        tally.gate(2, near(v.inPoint, 1.5) && near(v.leadInSec, 2.5)
                   && near(a.inPoint, 1.5) && near(a.leadInSec, 2.5));
    }

    // G3: group_clips (setClipGrouping) of V1 / V2 / A1, right edge of V2.
    const QVector<Timeline::GroupClipTarget> targets{{false, 0, 0}, {false, 1, 0}, {true, 0, 0}};
    QString error;
    setup(timeline, {{makeClip(0)}, {makeClip(0)}}, {{makeClip(0)}, {}});
    {
        const bool grouped = timeline.setClipGrouping(targets, true, &error);
        auto *v1 = timeline.videoTracks()[0];
        auto *v2 = timeline.videoTracks()[1];
        auto *a1 = timeline.audioTracks()[0];
        const int group = v1->clips()[0].linkGroup;
        dragEdge(v2, 0, true, -0.5);
        const double o1 = v1->clips()[0].outPoint, o2 = v2->clips()[0].outPoint,
                     o3 = a1->clips()[0].outPoint;
        std::fprintf(stderr, "[linked-trim] G3 grouped=%d group=%d V1.out=%.6f V2.out=%.6f"
                     " A1.out=%.6f (expect 4.5)\n", grouped ? 1 : 0, group, o1, o2, o3);
        tally.gate(3, grouped && group > 0 && near(o1, 4.5) && near(o2, 4.5) && near(o3, 4.5));
    }

    // G4: Alt + right edge -> only the grabbed clip changes.
    setup(timeline, {{makeClip(5)}}, {{makeClip(5)}});
    {
        auto *v1 = timeline.videoTracks()[0];
        auto *a1 = timeline.audioTracks()[0];
        dragEdge(v1, 0, true, -0.5, Qt::AltModifier);
        const ClipInfo &v = v1->clips()[0];
        const ClipInfo &a = a1->clips()[0];
        std::fprintf(stderr, "[linked-trim] G4 V1.out=%.6f (expect 4.5) A1.in=%.6f A1.out=%.6f"
                     " A1.lead=%.6f (expect 1 / 5 / 2)\n", v.outPoint, a.inPoint, a.outPoint,
                     a.leadInSec);
        tally.gate(4, near(v.outPoint, 4.5) && near(a.inPoint, 1.0) && near(a.outPoint, 5.0)
                   && near(a.leadInSec, 2.0));
    }

    // G5: A1 at speed 2.0 has 1.0 s of source (0.5 s timeline) left; V1 has
    // 5 s. Dragging V1's right edge +2 s stops both at +0.5 s timeline.
    setup(timeline, {{makeClip(5)}}, {{makeClip(5, 2.0, 10.0, 11.0, 2.0, 2.0)}});
    {
        auto *v1 = timeline.videoTracks()[0];
        auto *a1 = timeline.audioTracks()[0];
        dragEdge(v1, 0, true, 2.0);
        const ClipInfo &v = v1->clips()[0];
        const ClipInfo &a = a1->clips()[0];
        const double vLen = v.effectiveDuration(), aLen = a.effectiveDuration();
        std::fprintf(stderr, "[linked-trim] G5 V1.out=%.6f A1.out=%.6f A1.duration=%.6f"
                     " V1.len=%.6f A1.len=%.6f diff=%.9f (expect lens 4.5, A1.out 11)\n",
                     v.outPoint, a.outPoint, a.duration, vLen, aLen, vLen - aLen);
        tally.gate(5, near(vLen, aLen) && near(vLen, 4.5) && a.outPoint <= a.duration + 1e-9
                   && near(a.outPoint, 11.0));
    }

    // G6: A1's next clip starts 0.3 s after the pair ends. Extending V1's
    // right edge by +1 s stops both at +0.3 s; the neighbour stays put.
    setup(timeline, {{makeClip(5)}}, {{makeClip(5), makeClip(0, 0.0, 3.0, 10.0, 0.3)}});
    {
        auto *v1 = timeline.videoTracks()[0];
        auto *a1 = timeline.audioTracks()[0];
        const double nStart0 = clipStartSec(a1, 1);
        const double nDur0 = a1->clips()[1].effectiveDuration();
        dragEdge(v1, 0, true, 1.0);
        const double vo = v1->clips()[0].outPoint, ao = a1->clips()[0].outPoint;
        const double nStart = clipStartSec(a1, 1);
        const double nDur = a1->clips()[1].effectiveDuration();
        int overlaps = 0;
        for (int i = 0; i < a1->clips().size(); ++i) {
            for (int j = i + 1; j < a1->clips().size(); ++j) {
                const double si = clipStartSec(a1, i), ei = si + a1->clips()[i].effectiveDuration();
                const double sj = clipStartSec(a1, j), ej = sj + a1->clips()[j].effectiveDuration();
                if (si < ej - 1e-9 && sj < ei - 1e-9) ++overlaps;
            }
            std::fprintf(stderr, "[linked-trim] G6 A1[%d] start=%.6f end=%.6f\n", i,
                         clipStartSec(a1, i), clipStartSec(a1, i) + a1->clips()[i].effectiveDuration());
        }
        std::fprintf(stderr, "[linked-trim] G6 V1.out=%.6f A1.out=%.6f (expect 5.3)"
                     " next.start %.6f->%.6f next.dur %.6f->%.6f overlaps=%d\n",
                     vo, ao, nStart0, nStart, nDur0, nDur, overlaps);
        tally.gate(6, near(vo, 5.3) && near(ao, 5.3) && near(nStart, nStart0)
                   && near(nDur, nDur0) && overlaps == 0);
    }

    // G7: one drag on a 3-clip group = one undo entry; one undo restores all.
    setup(timeline, {{makeClip(0)}, {makeClip(0)}}, {{makeClip(0)}, {}});
    {
        timeline.setClipGrouping(targets, true, &error);
        timeline.undoManager()->clear();
        timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("基準"));
        const int base = timeline.undoManager()->currentIndex();
        dragEdge(timeline.videoTracks()[0], 0, true, -0.5);
        const int afterDrag = timeline.undoManager()->currentIndex();
        const double trimmed = timeline.audioTracks()[0]->clips()[0].outPoint;
        timeline.undo();
        const double o1 = timeline.videoTracks()[0]->clips()[0].outPoint;
        const double o2 = timeline.videoTracks()[1]->clips()[0].outPoint;
        const double o3 = timeline.audioTracks()[0]->clips()[0].outPoint;
        std::fprintf(stderr, "[linked-trim] G7 undo index %d->%d (expect +1) A1.out after drag=%.6f"
                     " after undo V1=%.6f V2=%.6f A1=%.6f (expect 5)\n",
                     base, afterDrag, trimmed, o1, o2, o3);
        tally.gate(7, afterDrag == base + 1 && near(trimmed, 4.5)
                   && near(o1, 5.0) && near(o2, 5.0) && near(o3, 5.0));
    }

    // G8: linkGroup == 0 -> single-clip trim as before.
    setup(timeline, {{makeClip(0), makeClip(0, 0.0, 3.0, 10.0, 0.3)}}, {{makeClip(0)}});
    {
        auto *v1 = timeline.videoTracks()[0];
        auto *a1 = timeline.audioTracks()[0];
        dragEdge(v1, 0, true, -0.5);
        const ClipInfo &v = v1->clips()[0];
        const ClipInfo &n = v1->clips()[1];
        const ClipInfo &a = a1->clips()[0];
        std::fprintf(stderr, "[linked-trim] G8 V1[0].out=%.6f (expect 4.5) V1[1] in/out/lead="
                     "%.6f/%.6f/%.6f (expect 0/3/0.3) A1 in/out/lead=%.6f/%.6f/%.6f (expect 1/5/2)\n",
                     v.outPoint, n.inPoint, n.outPoint, n.leadInSec, a.inPoint, a.outPoint, a.leadInSec);
        std::snprintf(buf, sizeof(buf), "V1[0].out=%.6f", v.outPoint);
        tally.gate(8, near(v.outPoint, 4.5) && near(n.inPoint, 0.0) && near(n.outPoint, 3.0)
                   && near(n.leadInSec, 0.3) && near(a.inPoint, 1.0) && near(a.outPoint, 5.0)
                   && near(a.leadInSec, 2.0), buf);
    }

    // G9: A1 locked -> a linked trim on V1 is rejected at press time (the
    // convention of the other linked ops): both clips unchanged, no undo
    // entry. Alt (solo) on the unlocked V1 still trims V1 alone.
    setup(timeline, {{makeClip(5)}}, {{makeClip(5)}});
    {
        auto *v1 = timeline.videoTracks()[0];
        auto *a1 = timeline.audioTracks()[0];
        a1->setLocked(true);
        const int base = timeline.undoManager()->currentIndex();
        dragEdge(v1, 0, true, -0.5);
        const double vo = v1->clips()[0].outPoint, ao = a1->clips()[0].outPoint;
        const int afterLinked = timeline.undoManager()->currentIndex();
        dragEdge(v1, 0, true, -0.5, Qt::AltModifier);
        const double voAlt = v1->clips()[0].outPoint, aoAlt = a1->clips()[0].outPoint;
        a1->setLocked(false);
        std::fprintf(stderr, "[linked-trim] G9 locked A1: V1.out=%.6f A1.out=%.6f (expect 5)"
                     " undo index %d->%d (expect +0); Alt solo: V1.out=%.6f (expect 4.5)"
                     " A1.out=%.6f (expect 5)\n", vo, ao, base, afterLinked, voAlt, aoAlt);
        tally.gate(9, near(vo, 5.0) && near(ao, 5.0) && afterLinked == base
                   && near(voAlt, 4.5) && near(aoAlt, 5.0));
    }

    return tally.finish("[linked-trim]");
}
