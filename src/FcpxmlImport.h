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
// Only direct spine asset-clips on at most two distinct lanes are imported.
ImportResult parseXml(const QString &xml, const QString &directory);
ImportResult readFcpxml(const QString &filePath);
} // namespace fcpx::xml
