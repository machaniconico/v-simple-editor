#pragma once

#include <QImage>
#include <QPointF>
#include <QVector>

namespace feattrack {

QVector<QPointF> detectShiTomasi(const QImage& gray, int maxCorners = 500,
                               double qualityLevel = 0.01, double minDistance = 8.0);

struct Track {
    QPointF pt;
    float error = 0.0f; // Mean squared intensity residual (8-bit intensity units).
    bool alive = true;
};

// levels includes the full-resolution level. Dead input tracks are untouched.
// Rejected tracks retain their input position; error is infinity if no fit exists.
// Even window sizes are rounded up. Invalid images/parameters reject live tracks.
void trackPyramidalLK(const QImage& prevGray, const QImage& nextGray,
                      QVector<Track>& tracks, int levels = 3, int window = 15,
                      int maxIter = 20, double eps = 0.01,
                      bool forwardBackwardCheck = true,
                      double forwardBackwardThreshold = 1.0);

} // namespace feattrack
