#include "../LayerStyle.h"
#include "../ProjectFile.h"

#include <QColor>
#include <QImage>
#include <QRect>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

void printGate(const char *gate, bool ok, const char *reason,
               int &passed, int &failed)
{
    if (ok) {
        std::printf("[layer-style] %s: PASS\n", gate);
        ++passed;
        return;
    }
    std::printf("[layer-style] %s: FAIL - %s\n", gate, reason);
    ++failed;
}

QImage makeTransparentTestImage()
{
    QImage img(8, 8, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    img.setPixelColor(1, 1, QColor(10, 20, 30, 64));
    img.setPixelColor(2, 3, QColor(200, 30, 40, 255));
    img.setPixelColor(6, 5, QColor(20, 220, 40, 128));
    return img;
}

QImage makeSquareImage(int size, const QRect &square, const QColor &color)
{
    QImage img(size, size, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    for (int y = square.top(); y <= square.bottom(); ++y) {
        for (int x = square.left(); x <= square.right(); ++x)
            img.setPixelColor(x, y, color);
    }
    return img;
}

bool sameBytes(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()
        || a.bytesPerLine() != b.bytesPerLine()
        || a.sizeInBytes() != b.sizeInBytes()) {
        return false;
    }
    return std::memcmp(a.constBits(), b.constBits(),
                       static_cast<std::size_t>(a.sizeInBytes())) == 0;
}

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

ProjectData projectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    return data;
}

ClipInfo basicClip()
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("layer-style-test.mov");
    clip.displayName = QStringLiteral("Layer Style Test");
    clip.duration = 1.0;
    clip.outPoint = 1.0;
    return clip;
}

} // namespace

int runLayerStyleSelftest()
{
    int passed = 0;
    int failed = 0;

    {
        const QImage img = makeTransparentTestImage();
        const QImage out = layerstyle::apply(img, LayerStyle{});
        printGate("G1 IDENTITY",
                  out.constBits() == img.constBits() && sameBytes(img, out),
                  "identity style must share pixels and stay byte-identical",
                  passed, failed);
    }

    {
        LayerStyle shadow;
        shadow.dropShadowEnabled = true;
        LayerStyle stroke;
        stroke.strokeEnabled = true;
        printGate("G2 isIdentity",
                  LayerStyle{}.isIdentity()
                      && !shadow.isIdentity()
                      && !stroke.isIdentity(),
                  "default must be identity; either enabled style must not",
                  passed, failed);
    }

    {
        const QImage img =
            makeSquareImage(20, QRect(4, 4, 4, 4), QColor(20, 80, 200, 255));
        LayerStyle s;
        s.dropShadowEnabled = true;
        s.shadowBlurRadius = 0.0;
        s.shadowOffset = QPointF(4.0, 4.0);
        const QImage out = layerstyle::apply(img, s);
        printGate("G3 DROP SHADOW",
                  out.pixelColor(10, 10).alpha() > 0
                      && out.pixelColor(5, 5) == img.pixelColor(5, 5),
                  "offset shadow should add alpha while preserving original square",
                  passed, failed);
    }

    {
        const QColor fill(30, 120, 220, 255);
        const QImage img = makeSquareImage(16, QRect(5, 5, 4, 4), fill);
        LayerStyle s;
        s.strokeEnabled = true;
        s.strokeColor = QColor(255, 0, 0, 255);
        s.strokeWidth = 2.0;
        const QImage out = layerstyle::apply(img, s);
        const QColor outside = out.pixelColor(4, 6);
        printGate("G4 STROKE",
                  outside.alpha() > 0
                      && outside.red() > 200
                      && outside.green() < 40
                      && outside.blue() < 40
                      && out.pixelColor(6, 6) == fill,
                  "outside stroke should be red and interior must stay unchanged",
                  passed, failed);
    }

    {
        LayerStyle s;
        s.dropShadowEnabled = true;
        s.shadowColor = QColor(12, 34, 56, 180);
        s.shadowOffset = QPointF(-3.0, 5.5);
        s.shadowBlurRadius = 6.0;
        s.shadowOpacity = 0.6;
        s.strokeEnabled = true;
        s.strokeColor = QColor(240, 10, 20, 200);
        s.strokeWidth = 3.5;

        const LayerStyle roundTrip = LayerStyle::fromJson(s.toJson());

        ClipInfo identityClip = basicClip();
        const QString identityJson =
            ProjectFile::toJsonString(projectWithClip(identityClip));

        ClipInfo styledClip = basicClip();
        styledClip.layerStyle = s;
        const QString styledJson =
            ProjectFile::toJsonString(projectWithClip(styledClip));
        ProjectData loaded;
        const bool loadedOk = ProjectFile::fromJsonString(styledJson, loaded);
        const bool loadedStyle =
            loadedOk && !loaded.videoTracks.isEmpty()
            && !loaded.videoTracks[0].isEmpty()
            && sameStyle(loaded.videoTracks[0][0].layerStyle, s);

        printGate("G5 serialization",
                  sameStyle(roundTrip, s)
                      && !identityJson.contains(QStringLiteral("\"layerStyle\""))
                      && styledJson.contains(QStringLiteral("\"layerStyle\""))
                      && loadedStyle,
                  "LayerStyle JSON must round-trip and identity clip JSON must omit layerStyle",
                  passed, failed);
    }

    std::printf("[layer-style] summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
