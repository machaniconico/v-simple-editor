#pragma once

#include <QString>
#include <QSize>
#include <functional>

class Timeline;
class RenderQueue;
namespace renderinplace {
struct Options {
    QString codec = QStringLiteral("h264");
    double handlesSec = 0.0;
    QString outputDir;
    QString projectFilePath;
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
