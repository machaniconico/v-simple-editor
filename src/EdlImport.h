#pragma once

#include "EdlExport.h"
#include <QStringList>

namespace edl {
// CMX3600 C/D events on V/A; other lines are reported, never silently applied.
// CMX3600 carries no numeric frame rate: callers supply the project rate.
struct ImportResult {
    EdlDocument document;
    QVector<QVector<ClipInfo>> videoTracks{QVector<ClipInfo>{}};
    QVector<QVector<ClipInfo>> audioTracks{QVector<ClipInfo>{}};
    QStringList warnings;
    QString error;
    int resolvedFiles = 0;
    int unresolvedFiles = 0;
};
ImportResult parseEdl(const QString &text, double frameRate);
void resolveClips(ImportResult &result, const QString &directory);
ImportResult readEdl(const QString &filePath, double frameRate);
} // namespace edl
