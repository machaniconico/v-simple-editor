#pragma once

// OtioExport (US-2012): the active sequence as an OpenTimelineIO .otio JSON
// (Timeline.1 / Stack.1 / Track.1 / Clip.1 / Gap.1 / ExternalReference.1 /
// LinearTimeWarp.1 / Marker.2). Reads the Timeline only; no QWidget use.
//
// Limits: transitions, time-remap curves, nested sequences and non-active
// sequences are not written. A clip without media (sequence reference, null
// object, shape, adjustment layer, 3D text only) becomes a named Gap.1.

#include <QJsonObject>
#include <QString>

class Timeline;

namespace otio {

QJsonObject timelineToOtio(const Timeline *tl, double fps, const QString &name);

bool writeOtio(const Timeline *tl, double fps, const QString &name,
               const QString &path, QString *err);

} // namespace otio
