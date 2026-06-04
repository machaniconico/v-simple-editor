#include "DvTimelineBuilder.h"

#include "../DolbyVisionMetadata.h"
#include "ClipColor.h"

namespace dvtimeline {

dolbyvision::DolbyVisionMetadata buildFromTimeline(
    const QVector<ShotSpan>& spans,
    const dolbyvision::DolbyVisionMetadata& base)
{
    dolbyvision::DolbyVisionMetadata result = base;
    result.shots.clear();
    result.shots.reserve(spans.size());

    for (int i = 0; i < spans.size(); ++i) {
        dolbyvision::DvShot shot;
        if (i < base.shots.size()) {
            shot.l1 = base.shots[i].l1;
            shot.trims = base.shots[i].trims;
            shot.l5 = base.shots[i].l5;
        } else {
            shot.l1.minNits = base.l6.masteringMinNits;
            shot.l1.maxNits = base.l6.masteringMaxNits;
            shot.l1.avgNits = (shot.l1.minNits + shot.l1.maxNits) / 2.0;
        }

        shot.startSec = spans[i].startSec;
        shot.endSec = spans[i].endSec;
        result.shots.append(shot);
    }

    return result;
}

} // namespace dvtimeline
