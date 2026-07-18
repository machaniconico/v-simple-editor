#include <QDebug>
#include <QString>
#include <QFile>
#include <QTemporaryDir>

#include "../PremiereXmlExporter.h"

int runPremiereXmlSelftest()
{
    qInfo().noquote() << "[premiere-xml] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[premiere-xml] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[premiere-xml] FAIL" << name << ":" << msg; };

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        fail("setup QTemporaryDir", QStringLiteral("failed to create temporary directory"));
        qInfo().noquote().nospace() << "[premiere-xml] selftest end, passed=" << passed << " failed=" << failed;
        return 1;
    }

    QList<PremiereHighlight> highlights;
    highlights.append({tempDir.path() + QStringLiteral("/source-a.mp4"), QStringLiteral("Opening Clip"), 0.0, 4.0});
    highlights.append({tempDir.path() + QStringLiteral("/source-b.mp4"), QStringLiteral("Middle Clip"), 12.5, 18.0});
    highlights.append({tempDir.path() + QStringLiteral("/source-c.mp4"), QStringLiteral("Closing Clip"), 32.0, 40.0});

    PremiereVideoInfo videoInfo;
    videoInfo.width = 1920;
    videoInfo.height = 1080;
    videoInfo.fps = 30.0;

    const QString combinedPath = tempDir.path() + QStringLiteral("/combined.xml");
    const bool combinedOk = PremiereXmlExporter::generateCombinedXml(
        highlights, videoInfo, combinedPath, QStringLiteral("Premiere XML Selftest"));

    // G1: combined XML writes successfully and creates the output file
    if (combinedOk && QFile::exists(combinedPath)) {
        pass("G1 generateCombinedXml writes combined XML file");
    } else {
        fail("G1 generateCombinedXml", QStringLiteral("ok=%1 exists=%2")
            .arg(combinedOk ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(QFile::exists(combinedPath) ? QStringLiteral("true") : QStringLiteral("false")));
    }

    // G2: individual XML writes one file per highlight
    const QList<QString> individualPaths = PremiereXmlExporter::generateIndividualXmls(
        highlights, videoInfo, tempDir.path());
    bool allIndividualFilesExist = individualPaths.size() == highlights.size();
    for (const QString& path : individualPaths) {
        if (!QFile::exists(path)) {
            allIndividualFilesExist = false;
            break;
        }
    }
    if (allIndividualFilesExist) {
        pass("G2 generateIndividualXmls writes one XML per highlight");
    } else {
        fail("G2 generateIndividualXmls", QStringLiteral("expected %1 paths, got %2")
            .arg(highlights.size())
            .arg(individualPaths.size()));
    }

    QFile combinedFile(combinedPath);
    QString combinedXml;
    if (combinedFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        combinedXml = QString::fromUtf8(combinedFile.readAll());
        combinedFile.close();
    }

    // G3: combined XML contains one sequence per highlight
    const int sequenceCount = combinedXml.count(QStringLiteral("<sequence"));
    if (sequenceCount == highlights.size()) {
        pass("G3 combined XML contains one sequence per highlight");
    } else {
        fail("G3 combined XML sequence count", QStringLiteral("expected %1, got %2")
            .arg(highlights.size())
            .arg(sequenceCount));
    }

    // G4: combined XML contains the Premiere-compatible xmeml DOCTYPE
    if (combinedXml.contains(QStringLiteral("<!DOCTYPE xmeml>"))) {
        pass("G4 combined XML contains xmeml DOCTYPE");
    } else {
        fail("G4 combined XML DOCTYPE", QStringLiteral("missing <!DOCTYPE xmeml>"));
    }

    qInfo().noquote().nospace() << "[premiere-xml] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
