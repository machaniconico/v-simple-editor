#pragma once

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>

namespace extgen {

struct GeneratorSpec;
struct RenderOutcome;

namespace edge {

// Locate Windows system Microsoft Edge (or VEDITOR_GENERATOR_BROWSER override).
// Returns empty string if no compatible Chromium/Edge browser is found.
// Zero browser binary is bundled with v-simple-editor.
QString findBrowserExecutable();

// RFC6455 minimal WebSocket frame helpers (used by CDP client & unit-tested directly).
QByteArray buildClientTextFrame(const QByteArray &payload, quint32 maskKey = 0x12345678u);
bool parseServerFrame(const QByteArray &buffer,
                      QByteArray *payloadOut,
                      int *opcodeOut,
                      int *consumedBytesOut);

// Build the JS evaluation snippet sent via CDP Runtime.evaluate to advance an HTML/React page.
QString buildSetFrameExpression(int frameIndex, double fps, const QJsonObject &params);

// Render one frame of an HTML/React page via Edge headless + DevTools Protocol.
RenderOutcome renderFrameViaEdgeCdp(const GeneratorSpec &spec,
                                    int frameIndex,
                                    QSize size,
                                    int timeoutMs = 15000);

} // namespace edge
} // namespace extgen
