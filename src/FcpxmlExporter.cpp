#include "FcpxmlExporter.h"

#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QUrl>
#include <QSet>
#include <QtMath>

namespace fcpx::xml {

// ---------------------------------------------------------------------------
// Helper: format seconds as rational time string "numerator/fps s"
// ---------------------------------------------------------------------------

static QString fmtTime(double secs, int fps)
{
    return QString::number(qRound(secs * fps)) + "/" + QString::number(fps) + "s";
}

static QString xmlEsc(QString s)
{
    return s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
}

// ---------------------------------------------------------------------------
// buildXml
// ---------------------------------------------------------------------------

QString buildXml(const QVector<ClipEntry> &clips, const ExporterConfig &config)
{
    const int fps = config.fps;

    // Compute total timeline duration
    double totalDuration = 0.0;
    for (const ClipEntry &c : clips) {
        const double end = c.offset + c.duration;
        if (end > totalDuration)
            totalDuration = end;
    }

    // Build ordered unique-file list and id map in one pass (clips order preserved)
    QVector<const ClipEntry *> uniqueClips;
    QMap<QString, int> assetIdMap;  // filePath -> asset id (1-based)
    {
        int idx = 1;
        for (const ClipEntry &c : clips) {
            if (!assetIdMap.contains(c.filePath)) {
                assetIdMap.insert(c.filePath, idx);
                uniqueClips.append(&c);
                ++idx;
            }
        }
    }

    QString out;
    QTextStream s(&out);
    s.setEncoding(QStringConverter::Utf8);

    // XML declaration + DOCTYPE
    s << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s << "<!DOCTYPE fcpxml>\n";
    s << "<fcpxml version=\"1.10\">\n";

    // --- resources ---
    s << "  <resources>\n";

    // Format resource (r0)
    s << "    <format"
      << " id=\"r0\""
      << " name=\"FFVideoFormat1080p30\""
      << " frameDuration=\"" << config.frameDuration << "\""
      << " width=\""         << config.width         << "\""
      << " height=\""        << config.height        << "\""
      << "/>\n";

    // Asset resources — one per unique file, ids r1..rN
    for (const ClipEntry *c : uniqueClips) {
        const int assetIdx = assetIdMap.value(c->filePath);
        const QString clipName = c->name.isEmpty()
                               ? QFileInfo(c->filePath).baseName()
                               : c->name;
        const QString srcUrl = QUrl::fromLocalFile(c->filePath).toString();

        s << "    <asset"
          << " id=\"r"      << assetIdx          << "\""
          << " name=\""     << xmlEsc(clipName)  << "\""
          << " src=\""      << xmlEsc(srcUrl)    << "\""
          << " start=\"0s\""
          << " duration=\"" << fmtTime(c->duration, fps) << "\""
          << " hasVideo=\"1\""
          << " format=\"r0\""
          << "/>\n";
    }

    s << "  </resources>\n";

    // --- library / event / project / sequence / spine ---
    s << "  <library>\n";
    s << "    <event name=\"" << xmlEsc(config.projectName) << " Event\">\n";
    s << "      <project name=\"" << xmlEsc(config.projectName) << "\">\n";
    s << "        <sequence"
      << " format=\"r0\""
      << " duration=\""  << fmtTime(totalDuration, fps) << "\""
      << " tcStart=\"0s\""
      << " tcFormat=\"NDF\""
      << ">\n";
    s << "          <spine>\n";

    for (const ClipEntry &c : clips) {
        const int ref = assetIdMap.value(c.filePath, 1);
        const QString clipName = c.name.isEmpty()
                               ? QFileInfo(c.filePath).baseName()
                               : c.name;

        s << "            <asset-clip"
          << " ref=\"r"     << ref                           << "\""
          << " offset=\""   << fmtTime(c.offset, fps)        << "\""
          << " duration=\"" << fmtTime(c.duration, fps)      << "\""
          << " start=\""    << fmtTime(c.startInSource, fps) << "\""
          << " name=\""     << xmlEsc(clipName)               << "\""
          << "/>\n";
    }

    s << "          </spine>\n";
    s << "        </sequence>\n";
    s << "      </project>\n";
    s << "    </event>\n";
    s << "  </library>\n";
    s << "</fcpxml>\n";

    return out;
}

// ---------------------------------------------------------------------------
// exportProject
// ---------------------------------------------------------------------------

bool exportProject(const QString &outPath, const QVector<ClipEntry> &clips,
                   const ExporterConfig &config)
{
    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << buildXml(clips, config);
    return true;
}

} // namespace fcpx::xml
