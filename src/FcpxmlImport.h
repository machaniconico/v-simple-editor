#pragma once
#include "Timeline.h"
#include <QStringList>

namespace fcpx::xml {
struct ImportResult {
    QString title;
    QVector<QVector<ClipInfo>> videoTracks{QVector<ClipInfo>{}};
    QVector<QVector<ClipInfo>> audioTracks{QVector<ClipInfo>{}};
    QStringList warnings;
    QString error;
    int resolvedFiles = 0;
    int unresolvedFiles = 0;
};
// Spine asset-clips, clip/sync-clip containers, and nested connected clips are
// flattened into video and audio tracks. Lane 0 takes priority; overlapping
// clips move to a higher track. An empty spine produces a warning.
ImportResult parseXml(const QString &xml, const QString &directory);
ImportResult readFcpxml(const QString &filePath);
} // namespace fcpx::xml
