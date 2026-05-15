#include "DavinciResolveXmlExporter.h"

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTextStream>
#include <QUrl>
#include <QVector>
#include <QXmlStreamWriter>

namespace davinci::xml {

// ---------------------------------------------------------------------------
// buildXml
// ---------------------------------------------------------------------------

QString buildXml(const QVector<ClipEntry> &clips, const ExporterConfig &config)
{
    // Compute maxOutPoint for sequence duration
    int maxOutPoint = 0;
    for (const ClipEntry &c : clips) {
        if (c.outPoint > maxOutPoint)
            maxOutPoint = c.outPoint;
    }

    QString    xml;
    QXmlStreamWriter w(&xml);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(2);

    // <?xml version="1.0" encoding="UTF-8"?>
    w.writeStartDocument("1.0");

    // <!DOCTYPE xmeml>
    w.writeDTD("<!DOCTYPE xmeml>");

    // <xmeml version="4">
    w.writeStartElement("xmeml");
    w.writeAttribute("version", "4");

    // <sequence id="seq1">
    w.writeStartElement("sequence");
    w.writeAttribute("id", "seq1");

    w.writeTextElement("name", config.sequenceName);
    w.writeTextElement("duration", QString::number(maxOutPoint));

    // <rate>
    w.writeStartElement("rate");
    w.writeTextElement("timebase", QString::number(config.fps));
    w.writeTextElement("ntsc", "FALSE");
    w.writeEndElement(); // </rate>

    // <media>
    w.writeStartElement("media");
    w.writeStartElement("video");

    // <format>
    w.writeStartElement("format");
    w.writeStartElement("samplecharacteristics");
    w.writeTextElement("width",  QString::number(config.width));
    w.writeTextElement("height", QString::number(config.height));
    w.writeEndElement(); // </samplecharacteristics>
    w.writeEndElement(); // </format>

    // Group clips by trackIndex — emit one <track> per distinct track.
    // Collect track indices in insertion order.
    QVector<int> trackOrder;
    for (const ClipEntry &c : clips) {
        if (!trackOrder.contains(c.trackIndex))
            trackOrder.append(c.trackIndex);
    }

    int clipSerial = 0;
    for (int trackIdx : trackOrder) {
        w.writeStartElement("track");

        for (int i = 0; i < clips.size(); ++i) {
            const ClipEntry &c = clips[i];
            if (c.trackIndex != trackIdx)
                continue;

            QFileInfo fi(c.filePath);
            QString   baseName = fi.fileName();
            int       duration = c.outPoint - c.inPoint;
            QString   fileUrl  = QUrl::fromLocalFile(c.filePath).toString();

            w.writeStartElement("clipitem");
            w.writeAttribute("id", QString("clip%1").arg(clipSerial));

            w.writeTextElement("name", baseName);
            w.writeTextElement("duration", QString::number(duration));

            w.writeStartElement("rate");
            w.writeTextElement("timebase", QString::number(config.fps));
            w.writeEndElement(); // </rate>

            w.writeTextElement("in",  QString::number(c.inPoint));
            w.writeTextElement("out", QString::number(c.outPoint));

            w.writeStartElement("file");
            w.writeAttribute("id", QString("file%1").arg(clipSerial));
            w.writeTextElement("name",    baseName);
            w.writeTextElement("pathurl", fileUrl);
            w.writeEndElement(); // </file>

            w.writeEndElement(); // </clipitem>
            ++clipSerial;
        }

        w.writeEndElement(); // </track>
    }

    w.writeEndElement(); // </video>
    w.writeEndElement(); // </media>

    w.writeEndElement(); // </sequence>
    w.writeEndElement(); // </xmeml>
    w.writeEndDocument();

    return xml;
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

    QTextStream stream(&file);
    stream << buildXml(clips, config);
    return true;
}

} // namespace davinci::xml
