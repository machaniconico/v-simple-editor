#pragma once
#include <QImage>
#include <QSize>
#include <QString>

namespace libavcore {
// Independent decoder per call; safe on worker threads. Never upscales.
QImage grabFrameAt(const QString &path, double seconds, QSize maxSize = QSize());
bool isStillImage(const QString &path);
}
