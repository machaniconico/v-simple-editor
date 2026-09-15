#pragma once

#include <QString>
#include <QSize>
#include <functional>

class Timeline;
class RenderQueue;
namespace renderinplace {
// Uses the same audio filter/mix implementation as normal export.
QString prepareAudioMix(Timeline *timeline, const QString &outputPath, QString *error);
struct Options {
    QString codec = QStringLiteral("h264");
    double handlesSec = 0.0;
    QString outputDir;
    QString projectFilePath;
    // Caller canvas size (e.g. control renders); baking always probes native media size.
    QSize outputSize = QSize(1920, 1080);
    double fps = 30.0;
    // UI installs the existing queue progress/cancel signal bridge before start.
    std::function<void(RenderQueue &)> connectProgress;
};
bool renderClipInPlace(Timeline &timeline, int trackIndex, int clipIndex,
                       const Options &options, QString *outPath = nullptr,
                       QString *error = nullptr);
bool decomposeRenderInPlace(Timeline &timeline, int trackIndex, int clipIndex);
}
