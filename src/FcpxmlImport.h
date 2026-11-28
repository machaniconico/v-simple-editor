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
// Spine asset-clips and gap-anchored or nested connected asset-clips are
// imported on up to two video lanes and two audio lanes, with lane 0 taking
// priority. A spine without asset-clips produces an empty result and warning.
ImportResult parseXml(const QString &xml, const QString &directory);
ImportResult readFcpxml(const QString &filePath);
} // namespace fcpx::xml
