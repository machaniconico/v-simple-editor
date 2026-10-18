#pragma once
#include "Timeline.h"
#include <QStringList>

namespace fcpx::xml {
struct ImportResult {
    QString title;
    QVector<QVector<ClipInfo>> videoTracks{QVector<ClipInfo>{}};
    QVector<QVector<ClipInfo>> audioTracks{QVector<ClipInfo>{}};
    // Compound clips (resources/media), inner sequences first. Each id is the
    // media id; ref-clips become ClipInfo::sequenceRefId to it.
    QVector<TimelineSequence> sequences;
    QStringList warnings;
    QString error;
    int resolvedFiles = 0;
    int unresolvedFiles = 0;
};
// Spine asset-clips, clip/sync-clip containers, and nested connected clips are
// flattened into video and audio tracks. Lane 0 takes priority; overlapping
// clips move to a higher track. An empty spine produces a warning.
// ref-clips read their media sequence (nesting depth 8); cycles are skipped.
ImportResult parseXml(const QString &xml, const QString &directory);
ImportResult readFcpxml(const QString &filePath);
// Adds result.sequences (ids made unique by Timeline::addSequence, references
// rewritten in result) and then the imported sequence via createSequence.
// warnings receives result.warnings and any apply failure. On failure the
// added nested sequences are removed again and false is returned.
bool applyImport(Timeline &timeline, ImportResult &result, QStringList *warnings);
} // namespace fcpx::xml
