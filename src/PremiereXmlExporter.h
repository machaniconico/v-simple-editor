#pragma once
#include <QList>
#include <QString>

struct PremiereHighlight {
    QString filePath;     // clip mp4 path (absolute)
    QString title;        // sequence name
    double startSec = 0;  // highlight start (source video, for reference)
    double endSec = 0;    // highlight end
    double durationSec() const { return endSec - startSec; }
};

struct PremiereVideoInfo {
    int width = 1920;
    int height = 1080;
    double fps = 30.0;
};

class PremiereXmlExporter {
public:
    // 1 つの xmeml に N 個 sequence を embed (clip-extractor の combined mode)
    static bool generateCombinedXml(
        const QList<PremiereHighlight>& highlights,
        const PremiereVideoInfo& videoInfo,
        const QString& outputPath,
        const QString& projectName = QStringLiteral("v-simple-editor Project"));

    // 各 highlight ごとに個別 xmeml を出力 (clip-extractor の individual mode)
    // return: 出力された file path の list
    static QList<QString> generateIndividualXmls(
        const QList<PremiereHighlight>& highlights,
        const PremiereVideoInfo& videoInfo,
        const QString& outputDir);
};
