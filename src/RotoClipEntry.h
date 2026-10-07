#pragma once

#include "Rotoscope.h"
#include <QImage>
#include <QString>
#include <QVector>

struct RotoClipEntry {
    QString clipId;
    RotoPath path;
    QVector<RotoKeyframe> keyframes;
    QImage brushMask;
};
