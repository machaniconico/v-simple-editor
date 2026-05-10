#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QColor>

// Forward declarations to avoid circular includes
struct VideoEffect;
enum class VideoEffectType : int;

namespace effectctrl {

enum class ParamType { Float, Int, Bool, Color, Choice };

struct ParamDef {
    QString name;
    QString displayLabel;
    ParamType type;
    double minVal = 0.0;
    double maxVal = 1.0;
    double defaultVal = 0.0;
    QStringList choices;
};

QVector<ParamDef> paramSchemaFor(VideoEffectType type);

double paramValue(const VideoEffect &effect, const QString &paramName);
void setParamValue(VideoEffect &effect, const QString &paramName, double value);

QColor colorParamValue(const VideoEffect &effect, const QString &paramName);
void setColorParam(VideoEffect &effect, const QString &paramName, QColor color);

} // namespace effectctrl
