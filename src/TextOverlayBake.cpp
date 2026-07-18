#include "TextOverlayBake.h"
#include "TextManager.h"   // EnhancedTextOverlay / GradientStop / PositionKeyframe

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QPen>
#include <QFontMetrics>
#include <QTextLayout>
#include <QTextOption>
#include <QThread>
#include <QtGlobal>
#include <QtMath>
#include <atomic>
#include <algorithm>
#include <cmath>

namespace {

struct BakeTextLine {
    QString text;
    qreal width = 0.0;
};

QFont bakeFontForOverlay(const EnhancedTextOverlay &ov, double fontScale)
{
    QFont font = ov.font;
    font.setPointSizeF(qMax(1.0, font.pointSizeF() * fontScale));
    if (ov.letterSpacing != 0.0)
        font.setLetterSpacing(QFont::AbsoluteSpacing,
                              ov.letterSpacing * fontScale);
    return font;
}

QVector<BakeTextLine> layoutBakeTextLines(const QString &text, const QFont &font,
                                          qreal maxWidth, bool wordWrap)
{
    QVector<BakeTextLine> lines;
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(wordWrap ? QTextOption::WordWrap : QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    const qreal lineWidth = qMax<qreal>(1.0, maxWidth);
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(lineWidth);
        QString lineText = text.mid(line.textStart(), line.textLength());
        lineText.remove(QChar::LineSeparator);
        lineText.remove(QLatin1Char('\n'));
        lineText.remove(QLatin1Char('\r'));
        lines.append({lineText, line.naturalTextWidth()});
    }
    layout.endLayout();
    if (lines.isEmpty() && !text.isEmpty()) {
        const QFontMetrics fm(font);
        lines.append({text, static_cast<qreal>(fm.horizontalAdvance(text))});
    }
    return lines;
}

QSize spacedBakeTextSize(const QString &text, const QFont &font, double lineSpacing)
{
    const QVector<BakeTextLine> lines =
        layoutBakeTextLines(text, font, 1'000'000.0, false);
    const QFontMetrics fm(font);
    qreal maxWidth = 0.0;
    for (const BakeTextLine &line : lines)
        maxWidth = qMax(maxWidth, line.width);
    const int lineCount = qMax(1, lines.size());
    const double lineAdvance = qMax(1.0, static_cast<double>(fm.height()) + lineSpacing);
    const double totalHeight =
        fm.height() + qMax(0, lineCount - 1) * lineAdvance;
    return QSize(static_cast<int>(std::ceil(maxWidth)),
                 static_cast<int>(std::ceil(qMax(1.0, totalHeight))));
}

QPainterPath spacedBakeTextPath(const QString &text, const QRect &box,
                                const QFont &font, double lineSpacing)
{
    QPainterPath path;
    const QVector<BakeTextLine> lines =
        layoutBakeTextLines(text, font, box.width(), true);
    if (lines.isEmpty())
        return path;

    const QFontMetrics fm(font);
    const double lineAdvance = qMax(1.0, static_cast<double>(fm.height()) + lineSpacing);
    const int lineCount = lines.size();
    const double totalHeight =
        fm.height() + qMax(0, lineCount - 1) * lineAdvance;
    double baselineY = box.top() + (box.height() - totalHeight) * 0.5 + fm.ascent();
    for (const BakeTextLine &line : lines) {
        if (!line.text.isEmpty()) {
            const double x = box.left() + (box.width() - line.width) * 0.5;
            path.addText(x, baselineY, font, line.text);
        }
        baselineY += lineAdvance;
    }
    return path;
}

} // namespace

// The body below is the EXACT text-baking code that used to live inline in
// VideoPlayer::composeFrameWithOverlays (the authoritative preview baker).
// It was hoisted verbatim so the same code serves both the preview (which
// delegates here after computing fontScale from its widget) and the SSOT
// renderer's worker-thread export path (which calls here directly with
// fontScale = 1.0 — the value the headless path already produced because
// m_glPreview was null). NOTHING about the glyph/keyframe/outline/gradient
// math changed: fonts, keyframed position, outline stroke, linear/radial
// multi-stop gradient and the QFontMetrics layout-rect placement are
// identical, so S6 export-vs-preview text parity holds by construction.
namespace textbake {

// Test-only observability (see header). std::atomic so the worker-thread
// selftest can read it from the GUI thread after the export finished.
// The sentinel-gate is a runtime env check (not a static-init bool) so the
// argv-switch dispatch path can qputenv("VEDITOR_TEXTEXPORT_SELFTEST", "1")
// after program startup and still have bakeOverlays observe it.
static std::atomic<QThread *> g_lastBakeThread{nullptr};

QThread *lastBakeThreadForTest()
{
    return g_lastBakeThread.load(std::memory_order_acquire);
}

void resetLastBakeThreadForTest()
{
    g_lastBakeThread.store(nullptr, std::memory_order_release);
}

QImage bakeOverlays(const QImage &source,
                    const QVector<EnhancedTextOverlay> &overlays,
                    double nowSec,
                    int hiddenIdx,
                    double fontScale)
{
    if (source.isNull())
        return source;
    if (overlays.isEmpty())
        return source;

    // Record the thread we actually baked text on (test-only). Runtime
    // env check so the argv-switch dispatch path (which qputenvs the var
    // after program startup) is honored. This is GENUINE evidence:
    // it observes the real production code path, so the worker-thread text-
    // export selftest can prove renderFrameAt's text stage ran OFF the GUI
    // thread — exactly where the old VideoPlayer-QWidget seam was UB.
    if (!qEnvironmentVariableIsEmpty("VEDITOR_TEXTEXPORT_SELFTEST"))
        g_lastBakeThread.store(QThread::currentThread(),
                               std::memory_order_release);

    QImage composed = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&composed);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int W = composed.width();
    const int H = composed.height();
    if (!(fontScale > 0.0))
        fontScale = 1.0;
    for (int i = 0; i < overlays.size(); ++i) {
        if (i == hiddenIdx) continue;
        const auto &ov = overlays[i];
        if (!ov.visible || ov.text.isEmpty()) continue;
        const double start = ov.startTime;
        const double end   = (ov.endTime > 0.0) ? ov.endTime : 1e18;
        if (nowSec < start || nowSec >= end) continue;

        QFont font = bakeFontForOverlay(ov, fontScale);
        p.setFont(font);

        const QFontMetrics fm(font);
        int boxW = qMax(1, static_cast<int>(ov.width  * W));
        int boxH = qMax(1, static_cast<int>(ov.height * H));
        if (ov.width <= 0.0 || ov.height <= 0.0) {
            const QSize textSize = (ov.lineSpacing == 0.0)
                ? fm.boundingRect(ov.text).size()
                : spacedBakeTextSize(ov.text, font, ov.lineSpacing * fontScale);
            boxW = textSize.width() + 16;
            boxH = textSize.height() + 8;
        }
        double ovX = ov.x;
        double ovY = ov.y;
        const double ovRelTime = nowSec - ov.startTime;
        if (!ov.positionKeyframes.isEmpty()) {
            const auto &kfs = ov.positionKeyframes;
            if (ovRelTime <= kfs.first().time) {
                ovX = kfs.first().cx;
                ovY = kfs.first().cy;
            } else if (ovRelTime >= kfs.last().time) {
                ovX = kfs.last().cx;
                ovY = kfs.last().cy;
            } else {
                for (int k = 0; k < kfs.size() - 1; ++k) {
                    if (ovRelTime >= kfs[k].time && ovRelTime <= kfs[k + 1].time) {
                        double t = (ovRelTime - kfs[k].time) / (kfs[k + 1].time - kfs[k].time);
                        ovX = kfs[k].cx + (kfs[k + 1].cx - kfs[k].cx) * t;
                        ovY = kfs[k].cy + (kfs[k + 1].cy - kfs[k].cy) * t;
                        break;
                    }
                }
            }
        }
        const int cx = static_cast<int>(ovX * W);
        const int cy = static_cast<int>(ovY * H);
        QRect box(cx - boxW / 2, cy - boxH / 2, boxW, boxH);

        if (ov.backgroundColor.alpha() > 0)
            p.fillRect(box, ov.backgroundColor);

        // Match QPainter::drawText(rect, AlignCenter|TextWordWrap, text) so
        // the committed render lands at the exact same position as the inline
        // text tool (widget-space drawText uses fm.boundingRect(rect,flags)
        // internally; horizontalAdvance + fm.height() drifts by 1-2 px on
        // certain glyph runs because horizontalAdvance includes right-side
        // bearing while the visual glyph rect doesn't).
        QPainterPath path;
        if (ov.lineSpacing == 0.0) {
            const QRect textRect = fm.boundingRect(
                box, Qt::AlignCenter | Qt::TextWordWrap, ov.text);
            const int baselineX = textRect.left();
            const int baselineY = textRect.top() + fm.ascent();
            path.addText(baselineX, baselineY, font, ov.text);
        } else {
            path = spacedBakeTextPath(ov.text, box, font,
                                      ov.lineSpacing * fontScale);
        }

        if (ov.outlineWidth > 0 && ov.outlineColor.alpha() > 0) {
            QPen outline(ov.outlineColor);
            outline.setWidthF(ov.outlineWidth * fontScale);
            outline.setJoinStyle(Qt::RoundJoin);
            outline.setCapStyle(Qt::RoundCap);
            p.strokePath(path, outline);
        }

        if (ov.gradientEnabled) {
            const QRectF bb = path.boundingRect();
            const QPointF center = bb.center();

            // Build the effective stop list. Prefer multi-stop gradientStops;
            // fall back to the legacy 2-stop (+ midpoint) on empty.
            QVector<GradientStop> stops;
            if (ov.gradientStops.size() >= 2) {
                stops = ov.gradientStops;
            } else {
                const QColor a = ov.gradientReverse ? ov.gradientEnd   : ov.gradientStart;
                const QColor b = ov.gradientReverse ? ov.gradientStart : ov.gradientEnd;
                GradientStop s0, s1;
                s0.position = 0.0; s0.color = a; s0.opacity = a.alphaF();
                s1.position = 1.0; s1.color = b; s1.opacity = b.alphaF();
                stops = { s0, s1 };
                const double midT = qBound(0.01, ov.gradientMidpoint / 100.0, 0.99);
                GradientStop sm;
                sm.position = midT;
                sm.color = QColor(
                    (a.red()   + b.red())   / 2,
                    (a.green() + b.green()) / 2,
                    (a.blue()  + b.blue())  / 2,
                    (a.alpha() + b.alpha()) / 2);
                sm.opacity = 0.5 * (a.alphaF() + b.alphaF());
                stops.insert(1, sm);
            }
            if (ov.gradientReverse && !ov.gradientStops.isEmpty()) {
                for (auto &s : stops) s.position = 1.0 - s.position;
                std::sort(stops.begin(), stops.end(),
                          [](const GradientStop &a, const GradientStop &b){ return a.position < b.position; });
            }
            auto applyStops = [&](QGradient &g) {
                for (const auto &s : stops) {
                    QColor c = s.color;
                    c.setAlphaF(qBound(0.0, s.opacity, 1.0));
                    g.setColorAt(qBound(0.0, s.position, 1.0), c);
                }
            };

            if (ov.gradientType == 1) {
                // Radial: center the gradient on the text bbox, radius = half diagonal
                const double r = 0.5 * std::hypot(bb.width(), bb.height());
                QRadialGradient grad(center, r);
                applyStops(grad);
                p.fillPath(path, grad);
            } else {
                // Linear: project bbox onto angle so colors hit bbox edges exactly
                const double rad = qDegreesToRadians(ov.gradientAngle);
                const double dx = std::cos(rad);
                const double dy = std::sin(rad);
                const double halfSpan = 0.5 * (std::abs(bb.width() * dx) + std::abs(bb.height() * dy));
                const QPointF offset(dx * halfSpan, dy * halfSpan);
                QLinearGradient grad(center - offset, center + offset);
                applyStops(grad);
                p.fillPath(path, grad);
            }
        } else {
            p.fillPath(path, ov.color);
        }
    }
    p.end();
    return composed;
}

} // namespace textbake
