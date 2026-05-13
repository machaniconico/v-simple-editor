#pragma once
#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>

namespace aimask {

enum class Engine {
    LumaThreshold,
    ColorRange,
    ExternalPlugin
};

struct MaskParams {
    Engine  engine         = Engine::LumaThreshold;
    double  lumaThreshold  = 0.5;
    QColor  colorTarget    = Qt::green;
    double  colorTolerance = 0.15;
    QString pluginId;
};

struct MaskResult {
    QImage  mask;
    bool    success = false;
    QString error;
};

MaskResult   generateMask(const QImage& source, const MaskParams& params);
QStringList  availableEngines();
QString      engineToString(Engine engine);
Engine       engineFromString(const QString& name);

} // namespace aimask
