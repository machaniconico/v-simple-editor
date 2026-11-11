#pragma once

#include "../GLPreview.h"

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QOpenGLContext>

namespace selftest {

// True when `gl` has a valid QOpenGLContext and its framebuffer can be grabbed
// (offscreen platforms report a GL-accelerated preview but cannot). Probes up to
// 8 times, letting the widget finish its first paint in between.
inline bool glContextUsable(GLPreview *gl)
{
    if (!gl)
        return false;
    for (int i = 0; i < 8; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        // The first grab creates the context, so grab before reading context().
        const QImage probe = gl->grabFramebuffer();
        if (gl->context() && gl->context()->isValid() && !probe.isNull())
            return true;
    }
    return false;
}

} // namespace selftest
