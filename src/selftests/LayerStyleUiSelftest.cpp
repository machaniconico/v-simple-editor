#include "../LayerStyle.h"
#include "../Timeline.h"
#include "../ClipGeometry.h"
#include "../VideoPlayer.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QVector>

#include <cmath>
#include <cstring>
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

bool imagesByteEqual(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format())
        return false;
    if (a.format() != QImage::Format_ARGB32_Premultiplied)
        return false;

    const int rowBytes = a.width() * 4;
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<std::size_t>(rowBytes)) != 0) {
            return false;
        }
    }
    return true;
}

QImage g6BaseFrame(const QSize &size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            img.setPixelColor(x, y, QColor(24 + (x % 37),
                                           44 + (y % 31),
                                           76 + ((x + y) % 43),
                                           255));
        }
    }
    return img;
}

QImage g6OverlayFrame(const QSize &size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.fillRect(QRect(size.width() / 3,
                     size.height() / 3,
                     size.width() / 3,
                     size.height() / 3),
               QColor(232, 84, 32, 210));
    p.fillRect(QRect(size.width() / 2 - 5,
                     size.height() / 2 - 13,
                     10,
                     26),
               QColor(255, 240, 88, 180));
    p.end();

    return img;
}

QImage g6OracleComposite(const QImage &v1Frame,
                         const VideoPlayer::DecodedLayer &v2Layer,
                         const LayerStyle &style)
{
    QImage expected =
        v1Frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QSize canvas(expected.width(), expected.height());
    const clipgeom::ClipTransform t{v2Layer.videoScale,
                                    v2Layer.videoDx,
                                    v2Layer.videoDy,
                                    v2Layer.rotation2DDegrees};
    QImage placed = clipgeom::renderLayer(v2Layer.rgb, t, canvas,
                                          /*smooth=*/false);
    placed = layerstyle::apply(placed, style);

    QPainter p(&expected);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setOpacity(qBound(0.0, v2Layer.opacity, 1.0));
    p.drawImage(0, 0, placed);
    p.end();

    return expected;
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

    {
        if (!qApp) {
            printSkip("G6 PREVIEW/EXPORT PARITY",
                      "VideoPlayer is a QWidget and cannot be constructed before QApplication",
                      skipped);
        } else {
            const QSize canvas(96, 64);
            const QImage v1Frame = g6BaseFrame(canvas);
            const QImage v2Frame = g6OverlayFrame(canvas);

            LayerStyle style;
            style.dropShadowEnabled = true;
            style.shadowColor = QColor(0, 0, 0, 210);
            style.shadowOffset = QPointF(5.0, 3.0);
            style.shadowBlurRadius = 3.0;
            style.shadowOpacity = 0.70;
            style.strokeEnabled = true;
            style.strokeColor = QColor(32, 214, 255, 230);
            style.strokeWidth = 3.0;

            VideoPlayer::DecodedLayer v2Layer;
            v2Layer.rgb = v2Frame;
            v2Layer.opacity = 0.86;
            v2Layer.videoScale = 0.78;
            v2Layer.videoDx = 0.10;
            v2Layer.videoDy = -0.07;
            v2Layer.rotation2DDegrees = 0.0;
            v2Layer.sourceTrack = 1;
            v2Layer.sourceClipIndex = 0;
            v2Layer.sequenceIdx = 1;

            VideoPlayer player(nullptr);
            const QImage preview =
                player.composeMultiTrackFrameForTest(
                    v1Frame,
                    {v2Frame},
                    {v2Layer.opacity},
                    {v2Layer.videoScale},
                    {v2Layer.videoDx},
                    {v2Layer.videoDy},
                    {v2Layer.rotation2DDegrees},
                    {style});
            const QImage expected = g6OracleComposite(v1Frame, v2Layer, style);

            const QImage noStyle =
                player.composeMultiTrackFrameForTest(
                    v1Frame,
                    {v2Frame},
                    {v2Layer.opacity},
                    {v2Layer.videoScale},
                    {v2Layer.videoDx},
                    {v2Layer.videoDy},
                    {v2Layer.rotation2DDegrees});
            const QImage expectedNoStyle =
                g6OracleComposite(v1Frame, v2Layer, LayerStyle{});

            printGate("G6 PREVIEW/EXPORT PARITY",
                      imagesByteEqual(preview, expected)
                          && imagesByteEqual(noStyle, expectedNoStyle)
                          && !imagesByteEqual(preview, noStyle),
                      "preview compositor must match styled export output, keep no-style output byte-identical, and differ when style is enabled",
                      passed, failed);
        }
    }

    std::printf("[layer-style-ui] summary passed=%d skipped=%d failed=%d\n",
                passed, skipped, failed);
    return failed == 0 ? 0 : 1;
}
