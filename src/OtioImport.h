#pragma once

#include "MarkerData.h"
#include "Timeline.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace otio {

struct ImportedTrack {
    QString name;
    bool isAudio = false;
    bool hidden = false;
    QVector<ClipInfo> clips;
};

struct ImportResult {
    bool ok = false;
    QString name;
    double fps = 30.0;
    QVector<ImportedTrack> videoTracks; // index 0 = V1 (frontmost)
    QVector<ImportedTrack> audioTracks;
    QVector<Marker> markers;
    QStringList warnings;
    QString error;
};

// Parse an OpenTimelineIO JSON object (Timeline.1 / Stack.1 / Track.1 / Clip.1..2 /
// Gap.1 / Transition.1 / ExternalReference.1 / LinearTimeWarp.1 / Marker.2).
ImportResult parseOtioObject(const QJsonObject &root);

// Read and parse a .otio file from disk.
ImportResult readOtioFile(const QString &path);

// Apply parsed OTIO tracks and markers to Timeline (creates a clean undo state).
bool applyOtioToTimeline(Timeline *timeline, const ImportResult &parsed, QString *errorOut = nullptr);

} // namespace otio
