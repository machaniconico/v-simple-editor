#include "../TextManager.h"
#include "../TextOverlayBake.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRect>
#include <QtGlobal>

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

bool sameImageBytes(const QImage &a, const QImage &b)
{
    return a.size() == b.size()
        && a.format() == b.format()
        && a.bytesPerLine() == b.bytesPerLine()
        && a.sizeInBytes() == b.sizeInBytes()
        && (a.sizeInBytes() == 0
            || std::memcmp(a.constBits(), b.constBits(),
                           static_cast<std::size_t>(a.sizeInBytes())) == 0);
}

QRect alphaBounds(const QImage &image)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                const QRect pixelRect(x, y, 1, 1);
                bounds = bounds.isNull() ? pixelRect : bounds.united(pixelRect);
            }
        }
    }
    return bounds;
}

EnhancedTextOverlay makeSpacingOverlay()
{
    EnhancedTextOverlay ov;
    ov.text = QStringLiteral("TRACKING");
    ov.font = QFont(QStringLiteral("Arial"), 36, QFont::Bold);
    ov.color = QColor(255, 255, 255, 255);
    ov.backgroundColor = QColor(0, 0, 0, 0);
    ov.outlineColor = QColor(0, 0, 0, 0);
    ov.outlineWidth = 0;
    ov.gradientEnabled = false;
    ov.x = 0.5;
    ov.y = 0.5;
    ov.width = 0.0;
    ov.height = 0.0;
    ov.startTime = 0.0;
    ov.endTime = 0.0;
    ov.visible = true;
    return ov;
}

QImage legacyBakeDefaultSpacing(const QImage &source,
                                const QVector<EnhancedTextOverlay> &overlays,
                                double nowSec)
{
    if (source.isNull())
        return source;
    if (overlays.isEmpty())
        return source;

    QImage composed = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&composed);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int W = composed.width();
    const int H = composed.height();
    for (const auto &ov : overlays) {
        if (!ov.visible || ov.text.isEmpty()) continue;
        const double start = ov.startTime;
        const double end = (ov.endTime > 0.0) ? ov.endTime : 1e18;
        if (nowSec < start || nowSec >= end) continue;

        QFont font = ov.font;
        font.setPointSizeF(qMax(1.0, font.pointSizeF()));
        p.setFont(font);

        const QFontMetrics fm(font);
        int boxW = qMax(1, static_cast<int>(ov.width * W));
        int boxH = qMax(1, static_cast<int>(ov.height * H));
        if (ov.width <= 0.0 || ov.height <= 0.0) {
            const QSize textSize = fm.boundingRect(ov.text).size();
            boxW = textSize.width() + 16;
            boxH = textSize.height() + 8;
        }
        const int cx = static_cast<int>(ov.x * W);
        const int cy = static_cast<int>(ov.y * H);
        QRect box(cx - boxW / 2, cy - boxH / 2, boxW, boxH);

        if (ov.backgroundColor.alpha() > 0)
            p.fillRect(box, ov.backgroundColor);

        const QRect textRect =
            fm.boundingRect(box, Qt::AlignCenter | Qt::TextWordWrap, ov.text);
        QPainterPath path;
        path.addText(textRect.left(), textRect.top() + fm.ascent(), font, ov.text);

        if (ov.outlineWidth > 0 && ov.outlineColor.alpha() > 0) {
            QPen outline(ov.outlineColor);
            outline.setWidthF(ov.outlineWidth);
            outline.setJoinStyle(Qt::RoundJoin);
            outline.setCapStyle(Qt::RoundCap);
            p.strokePath(path, outline);
        }
        p.fillPath(path, ov.color);
    }
    p.end();
    return composed;
}

void printGate(const char *gate, bool ok, const char *reason,
               int &passed, int &failed)
{
    if (ok) {
        std::printf("[text-spacing] %s: PASS\n", gate);
        ++passed;
        return;
    }
    std::printf("[text-spacing] %s: FAIL - %s\n", gate, reason);
    ++failed;
}

} // namespace

int runTextSpacingSelftest()
{
    if (!QGuiApplication::instance()) {
        std::printf("[text-spacing] QApplication missing; register this selftest as needsQApplication\n");
        return 1;
    }

    int passed = 0;
    int failed = 0;

    QImage source(360, 160, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::transparent);

    const EnhancedTextOverlay base = makeSpacingOverlay();
    const QVector<EnhancedTextOverlay> baseList{base};

    {
        const QImage legacy = legacyBakeDefaultSpacing(source, baseList, 0.0);
        const QImage current =
            textbake::bakeOverlays(source, baseList, 0.0, -1, 1.0);
        printGate("G1 DEFAULT BYTE-IDENTICAL",
                  sameImageBytes(legacy, current),
                  "letterSpacing=0 and lineSpacing=0 changed the legacy bake",
                  passed, failed);
    }

    {
        EnhancedTextOverlay tracked = base;
        tracked.letterSpacing = 8.0;
        const QVector<EnhancedTextOverlay> trackedList{tracked};
        const QImage normal =
            textbake::bakeOverlays(source, baseList, 0.0, -1, 1.0);
        const QImage expanded =
            textbake::bakeOverlays(source, trackedList, 0.0, -1, 1.0);
        const QRect normalBounds = alphaBounds(normal);
        const QRect expandedBounds = alphaBounds(expanded);
        printGate("G2 TRACKING WIDTH",
                  normalBounds.isValid() && expandedBounds.isValid()
                      && expandedBounds.width() > normalBounds.width(),
                  "positive letterSpacing did not increase rendered glyph bounds",
                  passed, failed);
    }

    {
        EnhancedTextOverlay spaced = base;
        spaced.letterSpacing = 3.5;
        spaced.lineSpacing = 6.25;
        const QVector<EnhancedTextOverlay> spacedList{spaced};
        const QJsonArray json = TextManager::toJson(spacedList);
        const QJsonObject obj = json.at(0).toObject();
        const QVector<EnhancedTextOverlay> restored = TextManager::fromJson(json);
        const bool ok = obj.value(QStringLiteral("letterSpacing")).toDouble() == 3.5
            && obj.value(QStringLiteral("lineSpacing")).toDouble() == 6.25
            && restored.size() == 1
            && restored.first().letterSpacing == 3.5
            && restored.first().lineSpacing == 6.25;
        printGate("G3 JSON ROUNDTRIP",
                  ok,
                  "letterSpacing/lineSpacing were not saved and restored",
                  passed, failed);
    }

    std::printf("[text-spacing] summary: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
