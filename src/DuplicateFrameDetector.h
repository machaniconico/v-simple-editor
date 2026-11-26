#pragma once

#include <QColor>
#include <QString>
#include <QVector>

class Timeline;

namespace dupframes {

struct ClipRef {
    bool isAudio = false;
    int trackIdx = 0;
    int clipIdx = 0;
    QString clipUid;
    QString displayName;
    double timelineStart = 0.0;
    double timelineEnd = 0.0;
    double sourceIn = 0.0;
    double sourceOut = 0.0;
};

struct DuplicateOverlap {
    QString sourcePath;
    int groupId = 0;       // 1-based group color id per distinct duplicated source file
    QColor groupColor;
    double overlapSourceIn = 0.0;
    double overlapSourceOut = 0.0;
    ClipRef first;
    ClipRef second;
};

struct AnalysisResult {
    QVector<DuplicateOverlap> overlaps;
    int affectedClipCount = 0;
    int distinctSources = 0;
};

// Analyze all visible/audible clips in Timeline for overlapping [inPoint, outPoint]
// ranges referencing the same media file (Premiere Pro / Avid / Resolve Duplicate Frame Detection).
// Ignores synthetic URIs (veditor://...) and empty paths.
AnalysisResult analyzeTimeline(const Timeline *timeline,
                               bool includeVideo = true,
                               bool includeAudio = false,
                               double minOverlapSec = 0.01);

// Deterministic color palette for duplicate frame groups (groupId >= 1).
QColor colorForGroup(int groupId);

} // namespace dupframes
