#pragma once

#include <QImage>

namespace stillcompare {

enum class Mode {
    WipeHorizontal,
    WipeVertical,
    SplitSideBySide
};

struct Config {
    bool enabled = false;
    Mode mode = Mode::WipeHorizontal;
    double position = 0.5;
    QImage still;
};

// Produces a display-only comparison image. The saved still is always fitted
// into the destination with preserved aspect ratio and black letterboxing.
QImage apply(const QImage &display, const QImage &still,
             Mode mode, double position);

} // namespace stillcompare
