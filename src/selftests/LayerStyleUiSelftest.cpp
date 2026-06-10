#include "../LayerStyle.h"
#include "../Timeline.h"

#include <QApplication>
#include <QPointF>
#include <QVector>

#include <cmath>
#include <cstdio>

namespace {

bool near(double a, double b)
{
    return std::abs(a - b) <= 1e-9;
}

bool sameStyle(const LayerStyle &a, const LayerStyle &b)
{
    return a.dropShadowEnabled == b.dropShadowEnabled
        && a.shadowColor.rgba() == b.shadowColor.rgba()
        && near(a.shadowOffset.x(), b.shadowOffset.x())
        && near(a.shadowOffset.y(), b.shadowOffset.y())
        && near(a.shadowBlurRadius, b.shadowBlurRadius)
        && near(a.shadowOpacity, b.shadowOpacity)
        && a.strokeEnabled == b.strokeEnabled
        && a.strokeColor.rgba() == b.strokeColor.rgba()
        && near(a.strokeWidth, b.strokeWidth);
}

void printGate(const char *gate, bool ok, const char *reason,
               int &passed, int &failed)
{
    if (ok) {
        std::printf("[layer-style-ui] %s: PASS\n", gate);
        ++passed;
        return;
    }
    std::printf("[layer-style-ui] %s: FAIL - %s\n", gate, reason);
    ++failed;
}

void printSkip(const char *gate, const char *reason, int &skipped)
{
    std::printf("[layer-style-ui] %s: SKIP - %s\n", gate, reason);
    ++skipped;
}

ClipInfo basicClip()
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("layer-style-ui-test.mov");
    clip.displayName = QStringLiteral("Layer Style UI Test");
    clip.duration = 1.0;
    clip.outPoint = 1.0;
    return clip;
}

} // namespace

int runLayerStyleUiSelftest()
{
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    {
        printGate("G1 DEFAULT IDENTITY",
                  LayerStyle{}.isIdentity(),
                  "default-constructed LayerStyle must be identity",
                  passed, failed);
    }

    {
        LayerStyle style;
        style.dropShadowEnabled = true;
        style.shadowColor = QColor(12, 34, 56, 180);
        style.shadowOffset = QPointF(-7.5, 8.25);
        style.shadowBlurRadius = 11.0;
        style.shadowOpacity = 0.42;
        style.strokeEnabled = true;
        style.strokeColor = QColor(240, 10, 20, 200);
        style.strokeWidth = 3.5;

        const LayerStyle roundTrip = LayerStyle::fromJson(style.toJson());
        printGate("G2 JSON ROUNDTRIP",
                  sameStyle(roundTrip, style),
                  "shadow and stroke fields must survive toJson/fromJson",
                  passed, failed);
    }

    {
        LayerStyle shadowOnly;
        shadowOnly.dropShadowEnabled = true;
        LayerStyle strokeOnly;
        strokeOnly.strokeEnabled = true;
        printGate("G3 ENABLE FLAGS",
                  !shadowOnly.isIdentity() && !strokeOnly.isIdentity(),
                  "either enabled layer-style branch must make the style non-identity",
                  passed, failed);
    }

    {
        if (!qApp) {
            printSkip("G4 TIMELINE SET/GET",
                      "Timeline is a QWidget and cannot be constructed before QApplication",
                      skipped);
        } else {
            Timeline timeline;
            const auto &tracks = timeline.videoTracks();
            if (tracks.isEmpty() || !tracks.first()) {
                printSkip("G4 TIMELINE SET/GET",
                          "Timeline constructed without a V1 video track",
                          skipped);
            } else {
                tracks.first()->setClips(QVector<ClipInfo>{basicClip()});
                tracks.first()->setSelectedClip(0);

                LayerStyle style;
                style.dropShadowEnabled = true;
                style.shadowColor = QColor(20, 30, 40, 220);
                style.shadowOffset = QPointF(3.0, 4.0);
                style.shadowBlurRadius = 5.0;
                style.shadowOpacity = 0.5;
                style.strokeEnabled = true;
                style.strokeColor = QColor(250, 240, 230, 255);
                style.strokeWidth = 6.0;

                timeline.setClipLayerStyle(style);
                printGate("G4 TIMELINE SET/GET",
                          sameStyle(timeline.clipLayerStyle(), style),
                          "setClipLayerStyle must be readable through clipLayerStyle",
                          passed, failed);
            }
        }
    }

    {
        if (!qApp) {
            printSkip("G5 TRACK-AWARE SET/GET",
                      "Timeline is a QWidget and cannot be constructed before QApplication",
                      skipped);
        } else {
            Timeline timeline;
            timeline.addVideoTrack();
            const auto &tracks = timeline.videoTracks();
            if (tracks.size() < 2 || !tracks[0] || !tracks[1]) {
                printSkip("G5 TRACK-AWARE SET/GET",
                          "Timeline could not provide V1+V2 video tracks",
                          skipped);
            } else {
                tracks[0]->setClips(QVector<ClipInfo>{basicClip()});
                tracks[0]->setSelectedClip(0);
                tracks[1]->setClips(QVector<ClipInfo>{basicClip()});

                LayerStyle v2Style;
                v2Style.dropShadowEnabled = true;
                v2Style.shadowBlurRadius = 9.0;
                timeline.setClipLayerStyle(1, 0, v2Style);

                const bool v2Saved = sameStyle(timeline.clipLayerStyle(1, 0), v2Style);
                const bool v1Clean = timeline.clipLayerStyle(0, 0).isIdentity();
                // out-of-range writes must be silent no-ops
                timeline.setClipLayerStyle(5, 0, v2Style);
                timeline.setClipLayerStyle(1, 7, v2Style);
                const bool boundsOk = timeline.clipLayerStyle(5, 0).isIdentity()
                                   && timeline.clipLayerStyle(1, 7).isIdentity();
                printGate("G5 TRACK-AWARE SET/GET",
                          v2Saved && v1Clean && boundsOk,
                          "V2 style must land on V2 only, V1 stays identity, OOB is a no-op",
                          passed, failed);
            }
        }
    }

    std::printf("[layer-style-ui] summary passed=%d skipped=%d failed=%d\n",
                passed, skipped, failed);
    return failed == 0 ? 0 : 1;
}
