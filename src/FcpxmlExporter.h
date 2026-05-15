#pragma once

#include <QObject>
#include <QString>
#include <QVector>

// ---------------------------------------------------------------------------
// FCPXML Exporter — Final Cut Pro X FCPXML format version 1.10
// ---------------------------------------------------------------------------

namespace fcpx::xml {

struct ExporterConfig {
    QString projectName   = "Untitled Project";
    int     fps           = 30;
    QString frameDuration = "1/30s";
    int     width         = 1920;
    int     height        = 1080;
};

struct ClipEntry {
    QString filePath;
    double  offset          = 0.0;  // timeline offset in seconds
    double  duration        = 0.0;  // clip duration in seconds
    double  startInSource   = 0.0;  // in-point in source file, in seconds
    QString name;
};

// Build the FCPXML document as a QString.
QString buildXml(const QVector<ClipEntry> &clips, const ExporterConfig &config);

// Write the FCPXML document to outPath. Returns true on success.
bool exportProject(const QString &outPath, const QVector<ClipEntry> &clips,
                   const ExporterConfig &config);

} // namespace fcpx::xml
