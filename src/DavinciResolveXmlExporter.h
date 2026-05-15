#pragma once

#include <QObject>
#include <QString>
#include <QVector>

// ---------------------------------------------------------------------------
// DaVinci Resolve XML Exporter — Final Cut Pro 7 XML schema (xmeml v4)
// ---------------------------------------------------------------------------

namespace davinci::xml {

struct ExporterConfig {
    QString sequenceName = "Sequence 1";
    int     fps          = 30;
    int     width        = 1920;
    int     height       = 1080;
};

struct ClipEntry {
    QString filePath;
    int     inPoint    = 0;
    int     outPoint   = 0;
    int     trackIndex = 0;
    int     sourceIn   = 0;
};

// Build the xmeml XML document as a QString.
QString buildXml(const QVector<ClipEntry> &clips, const ExporterConfig &config);

// Write the XML document to outPath. Returns true on success.
bool exportProject(const QString &outPath, const QVector<ClipEntry> &clips,
                   const ExporterConfig &config);

} // namespace davinci::xml
