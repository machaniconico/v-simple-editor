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
    // Video track index (0 = primary storyline / spine). Lane > 0 clips are
    // written as lane children: of the spine clip whose uid is anchorUid when
    // that anchor exists, else of a spine clip covering their start, else of a gap.
    int     lane            = 0;
    QString uid;        // lane 0 only: identity other entries may anchor to
    QString anchorUid;  // lane > 0 only: uid of the primary-storyline anchor
};

// Build the FCPXML document as a QString.
QString buildXml(const QVector<ClipEntry> &clips, const ExporterConfig &config);

// Write the FCPXML document to outPath. Returns true on success.
bool exportProject(const QString &outPath, const QVector<ClipEntry> &clips,
                   const ExporterConfig &config);

} // namespace fcpx::xml

class Timeline;

namespace fcpx {

// Collect the timeline's video clips as export entries (V1 = spine, V2+ = lane
// = track index, primary-storyline connections carry anchorUid).
QVector<xml::ClipEntry> buildExportClips(const Timeline &timeline);

} // namespace fcpx
