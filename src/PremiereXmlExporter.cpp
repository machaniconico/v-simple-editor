#include "PremiereXmlExporter.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamWriter>
#include <cmath>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static bool isNtsc(double fps)
{
    return std::abs(fps - std::round(fps)) > 0.01;
}

static QString ntscString(double fps)
{
    return isNtsc(fps) ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
}

static int frameDuration(double durationSec, double fps)
{
    return static_cast<int>(std::round(durationSec * fps));
}

// Write <rate> element (timebase + ntsc)
static void writeRate(QXmlStreamWriter& w, double fps)
{
    w.writeStartElement(QStringLiteral("rate"));
    w.writeTextElement(QStringLiteral("timebase"), QString::number(static_cast<int>(std::round(fps))));
    w.writeTextElement(QStringLiteral("ntsc"), ntscString(fps));
    w.writeEndElement(); // rate
}

// Write <samplecharacteristics> for video format
static void writeVideoSampleChars(QXmlStreamWriter& w, int width, int height, double fps)
{
    w.writeStartElement(QStringLiteral("samplecharacteristics"));
    w.writeTextElement(QStringLiteral("width"), QString::number(width));
    w.writeTextElement(QStringLiteral("height"), QString::number(height));
    writeRate(w, fps);
    w.writeEndElement(); // samplecharacteristics
}

// Write <format> element inside <video>
static void writeVideoFormat(QXmlStreamWriter& w, int width, int height, double fps)
{
    w.writeStartElement(QStringLiteral("format"));
    writeVideoSampleChars(w, width, height, fps);
    w.writeEndElement(); // format
}

// Write <file id="file-N"> element.
// In a clipitem inside the sequence, this is the full definition.
// In audio clipitems, it is just a reference (<file id="file-N"/>).
static void writeFileElement(QXmlStreamWriter& w,
                              const QString& fileId,
                              const PremiereHighlight& h,
                              const PremiereVideoInfo& info,
                              int frames)
{
    w.writeStartElement(QStringLiteral("file"));
    w.writeAttribute(QStringLiteral("id"), fileId);

    // name = stem of clip path
    const QFileInfo fi(h.filePath);
    w.writeTextElement(QStringLiteral("name"), fi.completeBaseName());

    // pathurl as file:// URI
    w.writeTextElement(QStringLiteral("pathurl"), QUrl::fromLocalFile(h.filePath).toString());

    w.writeTextElement(QStringLiteral("duration"), QString::number(frames));
    writeRate(w, info.fps);

    // media samplecharacteristics
    w.writeStartElement(QStringLiteral("media"));

    w.writeStartElement(QStringLiteral("video"));
    w.writeStartElement(QStringLiteral("samplecharacteristics"));
    w.writeTextElement(QStringLiteral("width"), QString::number(info.width));
    w.writeTextElement(QStringLiteral("height"), QString::number(info.height));
    w.writeEndElement(); // samplecharacteristics
    w.writeEndElement(); // video

    w.writeStartElement(QStringLiteral("audio"));
    w.writeStartElement(QStringLiteral("samplecharacteristics"));
    w.writeTextElement(QStringLiteral("depth"), QStringLiteral("16"));
    w.writeTextElement(QStringLiteral("samplerate"), QStringLiteral("48000"));
    w.writeEndElement(); // samplecharacteristics
    w.writeEndElement(); // audio

    w.writeEndElement(); // media
    w.writeEndElement(); // file
}

// Write a <sequence id="sequence-N"> element.
static void writeSequence(QXmlStreamWriter& w,
                           int seqIndex,
                           const QString& fileId,
                           const PremiereHighlight& h,
                           const PremiereVideoInfo& info)
{
    const int frames = frameDuration(h.durationSec(), info.fps);

    w.writeStartElement(QStringLiteral("sequence"));
    w.writeAttribute(QStringLiteral("id"), QStringLiteral("sequence-%1").arg(seqIndex));

    w.writeTextElement(QStringLiteral("name"), h.title);
    w.writeTextElement(QStringLiteral("duration"), QString::number(frames));

    writeRate(w, info.fps);

    // <timecode>
    w.writeStartElement(QStringLiteral("timecode"));
    {
        // timecode rate — NDF (ntsc=FALSE)
        w.writeStartElement(QStringLiteral("rate"));
        w.writeTextElement(QStringLiteral("timebase"), QString::number(static_cast<int>(std::round(info.fps))));
        w.writeTextElement(QStringLiteral("ntsc"), QStringLiteral("FALSE"));
        w.writeEndElement(); // rate
    }
    w.writeTextElement(QStringLiteral("string"), QStringLiteral("00:00:00:00"));
    w.writeTextElement(QStringLiteral("frame"), QStringLiteral("0"));
    w.writeTextElement(QStringLiteral("displayformat"), QStringLiteral("NDF"));
    w.writeEndElement(); // timecode

    // <media>
    w.writeStartElement(QStringLiteral("media"));

    // --- video ---
    w.writeStartElement(QStringLiteral("video"));
    writeVideoFormat(w, info.width, info.height, info.fps);

    w.writeStartElement(QStringLiteral("track"));
    {
        w.writeStartElement(QStringLiteral("clipitem"));
        w.writeAttribute(QStringLiteral("id"), QStringLiteral("clipitem-v-%1").arg(seqIndex));

        w.writeTextElement(QStringLiteral("name"), h.title);
        w.writeTextElement(QStringLiteral("start"), QStringLiteral("0"));
        w.writeTextElement(QStringLiteral("end"), QString::number(frames));
        w.writeTextElement(QStringLiteral("in"), QStringLiteral("0"));
        w.writeTextElement(QStringLiteral("out"), QString::number(frames));

        writeFileElement(w, fileId, h, info, frames);

        w.writeEndElement(); // clipitem
    }
    w.writeEndElement(); // track
    w.writeEndElement(); // video

    // --- audio (stereo: 2 tracks) ---
    w.writeStartElement(QStringLiteral("audio"));
    for (int ch = 1; ch <= 2; ++ch) {
        w.writeStartElement(QStringLiteral("track"));

        w.writeStartElement(QStringLiteral("clipitem"));
        w.writeAttribute(QStringLiteral("id"), QStringLiteral("clipitem-a%1-%2").arg(ch).arg(seqIndex));

        w.writeTextElement(QStringLiteral("name"), h.title);
        w.writeTextElement(QStringLiteral("start"), QStringLiteral("0"));
        w.writeTextElement(QStringLiteral("end"), QString::number(frames));
        w.writeTextElement(QStringLiteral("in"), QStringLiteral("0"));
        w.writeTextElement(QStringLiteral("out"), QString::number(frames));

        // file reference only (id attr, no body repeated)
        w.writeStartElement(QStringLiteral("file"));
        w.writeAttribute(QStringLiteral("id"), fileId);
        w.writeEndElement(); // file

        w.writeStartElement(QStringLiteral("sourcetrack"));
        w.writeTextElement(QStringLiteral("mediatype"), QStringLiteral("audio"));
        w.writeTextElement(QStringLiteral("trackindex"), QString::number(ch));
        w.writeEndElement(); // sourcetrack

        w.writeEndElement(); // clipitem
        w.writeEndElement(); // track
    }
    w.writeEndElement(); // audio

    w.writeEndElement(); // media
    w.writeEndElement(); // sequence
}

// Write DOCTYPE header + xmeml body into an open QFile.
// Returns false if the writer entered an error state.
static bool writeXmemlToFile(QFile& file,
                              const QList<PremiereHighlight>& highlights,
                              const PremiereVideoInfo& videoInfo,
                              const QString& projectName,
                              bool individualMode,         // true → no bin wrapper
                              int seqIndexOffset = 1)      // 1-based start
{
    // Write DOCTYPE preamble manually (QXmlStreamWriter cannot emit <!DOCTYPE ...>)
    static const char kDoctype[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE xmeml>\n";
    if (file.write(kDoctype) == -1)
        return false;

    QXmlStreamWriter w(&file);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(2);

    // Do NOT call writeStartDocument() — DOCTYPE was already written manually.

    w.writeStartElement(QStringLiteral("xmeml"));
    w.writeAttribute(QStringLiteral("version"), QStringLiteral("4"));

    w.writeStartElement(QStringLiteral("project"));
    w.writeTextElement(QStringLiteral("name"), projectName);

    w.writeStartElement(QStringLiteral("children"));

    if (!individualMode) {
        // Combined mode: add a bin for media files
        w.writeStartElement(QStringLiteral("bin"));
        w.writeTextElement(QStringLiteral("name"), QStringLiteral("Media"));
        w.writeStartElement(QStringLiteral("children"));

        for (int i = 0; i < highlights.size(); ++i) {
            const PremiereHighlight& h = highlights[i];
            const int seqIdx = seqIndexOffset + i;
            const QString fileId = QStringLiteral("file-%1").arg(seqIdx);
            const int frames = frameDuration(h.durationSec(), videoInfo.fps);

            // masterclip in bin
            w.writeStartElement(QStringLiteral("clip"));
            w.writeAttribute(QStringLiteral("id"), QStringLiteral("masterclip-%1").arg(seqIdx));
            w.writeTextElement(QStringLiteral("name"), h.title);
            writeFileElement(w, fileId, h, videoInfo, frames);
            w.writeEndElement(); // clip
        }

        w.writeEndElement(); // children (bin)
        w.writeEndElement(); // bin
    }

    // Sequences
    for (int i = 0; i < highlights.size(); ++i) {
        const int seqIdx = seqIndexOffset + i;
        const QString fileId = QStringLiteral("file-%1").arg(seqIdx);
        writeSequence(w, seqIdx, fileId, highlights[i], videoInfo);
    }

    w.writeEndElement(); // children (project)
    w.writeEndElement(); // project
    w.writeEndElement(); // xmeml

    return !w.hasError();
}

// ---------------------------------------------------------------------------
// Public static methods
// ---------------------------------------------------------------------------

bool PremiereXmlExporter::generateCombinedXml(
    const QList<PremiereHighlight>& highlights,
    const PremiereVideoInfo& videoInfo,
    const QString& outputPath,
    const QString& projectName)
{
    if (highlights.isEmpty())
        return false;

    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }

    const bool ok = writeXmemlToFile(file, highlights, videoInfo, projectName,
                                     /*individualMode=*/false, /*seqIndexOffset=*/1);
    file.close();

    if (!ok) {
        QFile::remove(outputPath); // cleanup partial write
        return false;
    }
    return true;
}

QList<QString> PremiereXmlExporter::generateIndividualXmls(
    const QList<PremiereHighlight>& highlights,
    const PremiereVideoInfo& videoInfo,
    const QString& outputDir)
{
    QList<QString> result;

    for (int i = 0; i < highlights.size(); ++i) {
        const PremiereHighlight& h = highlights[i];

        // File name: use title, sanitised, with a 1-based zero-padded index
        // prefix so highlights sharing the same title never overwrite each other.
        QString safeName = h.title;
        safeName.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
        if (safeName.isEmpty())
            safeName = QStringLiteral("clip");

        const QString prefix = QStringLiteral("%1-").arg(i + 1, 3, 10, QLatin1Char('0'));
        const QString outputPath =
            outputDir + QStringLiteral("/") + prefix + safeName + QStringLiteral(".xml");

        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            continue;

        QList<PremiereHighlight> single;
        single.append(h);

        const bool ok = writeXmemlToFile(file, single, videoInfo, h.title,
                                          /*individualMode=*/true, /*seqIndexOffset=*/1);
        file.close();

        if (!ok) {
            QFile::remove(outputPath);
            continue;
        }
        result.append(outputPath);
    }

    return result;
}
