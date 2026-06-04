#pragma once

#include <QVector>

#include "../DolbyVisionMetadata.h"
#include "ClipColor.h"

namespace dvtimeline {

struct ShotSpan {
    double startSec = 0.0;
    double endSec = 0.0;
    clipcolor::ColorMeta colorMeta;
};

dolbyvision::DolbyVisionMetadata buildFromTimeline(
    const QVector<ShotSpan>& spans,
    const dolbyvision::DolbyVisionMetadata& base);

} // namespace dvtimeline
