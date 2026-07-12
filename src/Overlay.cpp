#include "Overlay.h"
#include "BrushAnimation.h"
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QImage>
#include <QVector>

namespace {

// Separable box blur on the alpha channel. Mirrors the helper used in
// TextManager.cpp's enhanced path; kept local here so Overlay.cpp does
// not depend on TextManager's private renderer.
void boxBlurAlphaInPlace(QImage &img, int radius)
{
    if (radius <= 0 || img.isNull()) return;
    const int r = qMin(radius, 32);
    const int w = img.width();
    const int h = img.height();
    if (w == 0 || h == 0) return;
    if (img.format() != QImage::Format_ARGB32_Premultiplied)
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QVector<int> tmp(w * h);
    for (int y = 0; y < h; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        int sum = 0;
        for (int x = -r; x <= r; ++x)
            sum += qAlpha(row[qBound(0, x, w - 1)]);
        const int span = 2 * r + 1;
        for (int x = 0; x < w; ++x) {
            tmp[y * w + x] = sum / span;
            sum += qAlpha(row[qBound(0, x + r + 1, w - 1)])
                 - qAlpha(row[qBound(0, x - r,     w - 1)]);
        }
    }
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int y = -r; y <= r; ++y)
            sum += tmp[qBound(0, y, h - 1) * w + x];
        const int span = 2 * r + 1;
        for (int y = 0; y < h; ++y) {
            const int a = sum / span;
            QRgb *row = reinterpret_cast<QRgb*>(img.scanLine(y));
            row[x] = qRgba(0, 0, 0, a);
            sum += tmp[qBound(0, y + r + 1, h - 1) * w + x]
                 - tmp[qBound(0, y - r,     h - 1) * w + x];
        }
    }
}

QImage rasterTextAlpha(const QString &text, const QRect &rect,
                       const QFont &font, int alignFlags, int padPx)
{
    const int W = qMax(1, rect.width()  + padPx * 2);
    const int H = qMax(1, rect.height() + padPx * 2);
    QImage buf(W, H, QImage::Format_ARGB32_Premultiplied);
    buf.fill(Qt::transparent);
    QPainter p(&buf);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font);
    p.setPen(Qt::black);
    p.drawText(QRect(padPx, padPx, rect.width(), rect.height()), alignFlags, text);
    p.end();
    return buf;
}

double brushProgressForTime(const BrushOverlay &overlay, double currentTime)
{
    if (!overlay.visible || !overlay.animation) {
        return -1.0;
    }

    if (overlay.durationSec > 0.0) {
        const double localTime = currentTime - overlay.startTime;
        if (localTime <= 0.0) {
            return 0.0;
        }
        if (localTime >= overlay.durationSec) {
            return 1.0;
        }
        return localTime / overlay.durationSec;
    }

    return overlay.animation->progress();
}

} // namespace

void OverlayRenderer::renderTextOverlay(QImage &frame, const TextOverlay &overlay, double currentTime)
{
    if (!overlay.visible) return;
    if (currentTime < overlay.startTime) return;
    if (overlay.endTime > 0 && currentTime > overlay.endTime) return;

    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    painter.setFont(overlay.font);
    QFontMetrics fm(overlay.font);
    const int alignFlags = overlay.alignment | Qt::TextWordWrap;
    QRect textBounds = fm.boundingRect(QRect(0, 0, frame.width() - 40, 0),
        alignFlags, overlay.text);

    int px = static_cast<int>(overlay.x * frame.width()) - textBounds.width() / 2;
    int py = static_cast<int>(overlay.y * frame.height()) - textBounds.height() / 2;
    QRect drawRect(px, py, textBounds.width(), textBounds.height());

    // Background
    if (overlay.backgroundColor.alpha() > 0) {
        painter.fillRect(drawRect.adjusted(-8, -4, 8, 4), overlay.backgroundColor);
    }

    // Order: shadow -> glow -> outline -> fill. Each effect skips its
    // offscreen QImage entirely when the boolean flag is false so disabled
    // effects cost nothing beyond the branch test.
    if (overlay.shadow) {
        const double blur = qMax(0.0, overlay.shadowBlur);
        const int pad = qMax(8, static_cast<int>(blur * 2.0) + 4);
        QImage buf = rasterTextAlpha(overlay.text, drawRect, overlay.font, alignFlags, pad);
        boxBlurAlphaInPlace(buf, static_cast<int>(blur));
        {
            QPainter pp(&buf);
            pp.setCompositionMode(QPainter::CompositionMode_SourceIn);
            pp.fillRect(buf.rect(), overlay.shadowColor);
        }
        const double op = qBound(0.0, overlay.shadowOpacity, 1.0);
        const double prevOp = painter.opacity();
        painter.setOpacity(prevOp * op);
        painter.drawImage(drawRect.left() - pad + static_cast<int>(overlay.shadowOffsetX),
                          drawRect.top()  - pad + static_cast<int>(overlay.shadowOffsetY),
                          buf);
        painter.setOpacity(prevOp);
    }

    if (overlay.glow) {
        const double radius = qMax(0.0, overlay.glowRadius);
        const int pad = qMax(8, static_cast<int>(radius * 2.0) + 4);
        QImage buf = rasterTextAlpha(overlay.text, drawRect, overlay.font, alignFlags, pad);
        boxBlurAlphaInPlace(buf, static_cast<int>(radius));
        {
            QPainter pp(&buf);
            pp.setCompositionMode(QPainter::CompositionMode_SourceIn);
            pp.fillRect(buf.rect(), overlay.glowColor);
        }
        const double op = qBound(0.0, overlay.glowOpacity, 1.0);
        const double prevOp = painter.opacity();
        const QPainter::CompositionMode prevMode = painter.compositionMode();
        painter.setOpacity(prevOp * op);
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        painter.drawImage(drawRect.left() - pad, drawRect.top() - pad, buf);
        painter.setCompositionMode(prevMode);
        painter.setOpacity(prevOp);
    }

    // Outline
    if (overlay.outlineWidth > 0) {
        QPainterPath path;
        path.addText(px, py + fm.ascent(), overlay.font, overlay.text);
        painter.setPen(QPen(overlay.outlineColor, overlay.outlineWidth));
        painter.drawPath(path);
    }

    // Text
    painter.setPen(overlay.color);
    painter.drawText(drawRect, alignFlags, overlay.text);
}

void OverlayRenderer::renderImageOverlay(QImage &frame, const ImageOverlay &overlay, double currentTime)
{
    if (!overlay.visible) return;
    if (currentTime < overlay.startTime) return;
    if (overlay.endTime > 0 && currentTime > overlay.endTime) return;

    QImage img(overlay.filePath);
    if (img.isNull()) return;

    QPainter painter(&frame);
    painter.setOpacity(overlay.opacity);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    if (!overlay.renderQuad.isEmpty() && overlay.renderQuad.size() == 4) {
        QPointF dst[4] = {
            overlay.renderQuad[0],
            overlay.renderQuad[1],
            overlay.renderQuad[2],
            overlay.renderQuad[3],
        };

        int srcW = img.width();
        int srcH = img.height();
        QPointF src[4] = {
            QPointF(0, 0),
            QPointF(srcW, 0),
            QPointF(srcW, srcH),
            QPointF(0, srcH),
        };

        QPolygonF srcPoly;
        srcPoly << src[0] << src[1] << src[2] << src[3];
        QPolygonF dstPoly;
        dstPoly << dst[0] << dst[1] << dst[2] << dst[3];
        QTransform quadTransform;
        const bool quadOk = QTransform::quadToQuad(srcPoly, dstPoly, quadTransform);
        if (quadOk && !quadTransform.isIdentity() && quadTransform.isInvertible()) {
            painter.setTransform(quadTransform);
            painter.drawImage(0, 0, img);
        } else {
            int x = static_cast<int>(overlay.rect.x() * frame.width());
            int y = static_cast<int>(overlay.rect.y() * frame.height());
            int w = static_cast<int>(overlay.rect.width() * frame.width());
            int h = static_cast<int>(overlay.rect.height() * frame.height());
            QImage scaled = img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            painter.drawImage(x, y, scaled);
        }
    } else {
        int x = static_cast<int>(overlay.rect.x() * frame.width());
        int y = static_cast<int>(overlay.rect.y() * frame.height());
        int w = static_cast<int>(overlay.rect.width() * frame.width());
        int h = static_cast<int>(overlay.rect.height() * frame.height());

        QImage scaled;
        if (overlay.keepAspectRatio)
            scaled = img.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        else
            scaled = img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        painter.drawImage(x, y, scaled);
    }
}

void OverlayRenderer::renderBrushOverlay(QImage &frame, const BrushOverlay &overlay, double currentTime)
{
    const double progress = brushProgressForTime(overlay, currentTime);
    if (progress < 0.0) {
        return;
    }
    renderBrushOverlay(frame, overlay.animation, progress);
}

void OverlayRenderer::renderBrushOverlay(QImage &frame, BrushAnimation *brushAnimation, double progress)
{
    if (frame.isNull() || !brushAnimation) {
        return;
    }

    const double clampedProgress = qBound(0.0, progress, 1.0);
    if (clampedProgress <= 0.0) {
        return;
    }

    const QImage brushFrame = brushAnimation->renderFrame(frame.size(), clampedProgress);
    if (brushFrame.isNull()) {
        return;
    }

    QPainter painter(&frame);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(0, 0, brushFrame);
}

void OverlayRenderer::renderPip(QImage &frame, const QImage &pipSource, const PipConfig &config)
{
    if (!config.visible || pipSource.isNull()) return;

    int x = static_cast<int>(config.rect.x() * frame.width());
    int y = static_cast<int>(config.rect.y() * frame.height());
    int w = static_cast<int>(config.rect.width() * frame.width());
    int h = static_cast<int>(config.rect.height() * frame.height());

    QImage scaled = pipSource.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QPainter painter(&frame);
    painter.setOpacity(config.opacity);

    // Border
    if (config.borderWidth > 0) {
        painter.setPen(QPen(config.borderColor, config.borderWidth));
        painter.drawRect(x - 1, y - 1, scaled.width() + 1, scaled.height() + 1);
    }

    painter.drawImage(x, y, scaled);
}

QImage OverlayRenderer::applyTransition(const QImage &from, const QImage &to,
    TransitionType type, double progress)
{
    if (type == TransitionType::None) return (progress < 0.5) ? from : to;

    // NOTE: easing must be applied by the caller (compose path) so the
    // Transition.easing field on the boundary clip can drive the curve.
    // VideoPlayer::handlePlaybackTick / harvestOverlayLayer compute the
    // raw timeline progress and pass an already-eased value here.

    int w = qMax(from.width(), to.width());
    int h = qMax(from.height(), to.height());
    QImage result(w, h, QImage::Format_RGB888);
    result.fill(Qt::black);
    QPainter painter(&result);

    QImage fromScaled = from.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage toScaled = to.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    switch (type) {
    case TransitionType::FadeIn:
        painter.drawImage(0, 0, fromScaled);
        painter.setOpacity(progress);
        painter.drawImage(0, 0, toScaled);
        break;

    case TransitionType::FadeOut:
        painter.drawImage(0, 0, toScaled);
        painter.setOpacity(1.0 - progress);
        painter.drawImage(0, 0, fromScaled);
        break;

    case TransitionType::CrossDissolve:
        painter.setOpacity(1.0 - progress);
        painter.drawImage(0, 0, fromScaled);
        painter.setOpacity(progress);
        painter.drawImage(0, 0, toScaled);
        break;

    case TransitionType::WipeLeft: {
        int boundary = static_cast<int>(w * progress);
        painter.drawImage(0, 0, fromScaled);
        painter.setClipRect(0, 0, boundary, h);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::WipeRight: {
        int boundary = static_cast<int>(w * (1.0 - progress));
        painter.drawImage(0, 0, fromScaled);
        painter.setClipRect(boundary, 0, w - boundary, h);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::WipeUp: {
        int boundary = static_cast<int>(h * progress);
        painter.drawImage(0, 0, fromScaled);
        painter.setClipRect(0, 0, w, boundary);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::WipeDown: {
        int boundary = static_cast<int>(h * (1.0 - progress));
        painter.drawImage(0, 0, fromScaled);
        painter.setClipRect(0, boundary, w, h - boundary);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::SlideLeft: {
        int offset = static_cast<int>(w * progress);
        painter.drawImage(-offset, 0, fromScaled);
        painter.drawImage(w - offset, 0, toScaled);
        break;
    }

    case TransitionType::SlideRight: {
        int offset = static_cast<int>(w * progress);
        painter.drawImage(offset, 0, fromScaled);
        painter.drawImage(-w + offset, 0, toScaled);
        break;
    }

    case TransitionType::SlideUp: {
        int offset = static_cast<int>(h * progress);
        painter.drawImage(0, -offset, fromScaled);
        painter.drawImage(0, h - offset, toScaled);
        break;
    }

    case TransitionType::SlideDown: {
        int offset = static_cast<int>(h * progress);
        painter.drawImage(0, offset, fromScaled);
        painter.drawImage(0, -h + offset, toScaled);
        break;
    }

    case TransitionType::DipToBlack:
    case TransitionType::DipToWhite: {
        // First half: A fades to black/white. Second half: black/white fades
        // to B. Symmetric — at progress=0.5 the frame is solid black/white.
        const QColor mid = (type == TransitionType::DipToBlack)
                           ? QColor(0, 0, 0) : QColor(255, 255, 255);
        painter.fillRect(0, 0, w, h, mid);
        if (progress < 0.5) {
            painter.setOpacity(1.0 - progress * 2.0);
            painter.drawImage(0, 0, fromScaled);
        } else {
            painter.setOpacity((progress - 0.5) * 2.0);
            painter.drawImage(0, 0, toScaled);
        }
        break;
    }

    case TransitionType::IrisRound: {
        // Circular iris expanding from the centre — B revealed inside the
        // growing disc.
        painter.drawImage(0, 0, fromScaled);
        const int cx = w / 2;
        const int cy = h / 2;
        const double maxR = std::sqrt(double(cx) * cx + double(cy) * cy) + 2.0;
        const double r = maxR * progress;
        QPainterPath path;
        path.addEllipse(QPointF(cx, cy), r, r);
        painter.setClipPath(path);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::IrisBox: {
        // Rectangular iris expanding from the centre.
        painter.drawImage(0, 0, fromScaled);
        const int rectW = static_cast<int>(w * progress);
        const int rectH = static_cast<int>(h * progress);
        const int rx = (w - rectW) / 2;
        const int ry = (h - rectH) / 2;
        painter.setClipRect(rx, ry, rectW, rectH);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::ClockWipe: {
        // Radial sweep clockwise from 12 o'clock. Skip the pie-path entirely
        // at progress >= 1 so the closeSubpath()'s radial line back to the
        // center doesn't leave a 1-pixel seam at the final frame.
        if (progress >= 1.0) {
            painter.drawImage(0, 0, toScaled);
            break;
        }
        painter.drawImage(0, 0, fromScaled);
        const int cx = w / 2;
        const int cy = h / 2;
        const double maxR = std::sqrt(double(cx) * cx + double(cy) * cy) + 2.0;
        QPainterPath path;
        path.moveTo(cx, cy);
        // Qt arc angles: 0° at 3 o'clock, positive = counter-clockwise.
        // Start at 90° (12 o'clock) and sweep negative for clockwise.
        path.arcTo(cx - maxR, cy - maxR, maxR * 2, maxR * 2,
                   90.0, -360.0 * progress);
        path.closeSubpath();
        painter.setClipPath(path);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::BarnDoorHorizontal: {
        // Two doors opening horizontally from the centre, revealing B
        // through the widening gap.
        painter.drawImage(0, 0, fromScaled);
        const int strip = static_cast<int>((w / 2) * progress);
        const int cx = w / 2;
        painter.setClipRect(cx - strip, 0, strip * 2, h);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    case TransitionType::BarnDoorVertical: {
        painter.drawImage(0, 0, fromScaled);
        const int strip = static_cast<int>((h / 2) * progress);
        const int cy = h / 2;
        painter.setClipRect(0, cy - strip, w, strip * 2);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    // Push family: A is shoved off-screen as B comes in from the same
    // direction. Distinct from Slide — Slide overlays B on top of a
    // stationary A, while Push translates BOTH images together so the
    // edge between them is a hard boundary travelling across the frame.
    // Premiere Pro / FCP X both ship Push as a separate primitive.
    case TransitionType::PushLeft: {
        // B enters from the right, pushing A off to the left.
        const int offset = static_cast<int>(w * progress);
        painter.drawImage(-offset, 0, fromScaled);
        painter.drawImage(w - offset, 0, toScaled);
        break;
    }

    case TransitionType::PushRight: {
        // B enters from the left, pushing A off to the right.
        const int offset = static_cast<int>(w * progress);
        painter.drawImage(offset, 0, fromScaled);
        painter.drawImage(offset - w, 0, toScaled);
        break;
    }

    case TransitionType::PushUp: {
        // B enters from the bottom, pushing A off the top.
        const int offset = static_cast<int>(h * progress);
        painter.drawImage(0, -offset, fromScaled);
        painter.drawImage(0, h - offset, toScaled);
        break;
    }

    case TransitionType::PushDown: {
        // B enters from the top, pushing A off the bottom.
        const int offset = static_cast<int>(h * progress);
        painter.drawImage(0, offset, fromScaled);
        painter.drawImage(0, offset - h, toScaled);
        break;
    }

    // Film Dissolve: gamma-corrected CrossDissolve. Standard sRGB blending
    // (linear opacity in display-encoded space) makes mid-dissolve frames
    // perceptually darker than either source — the well-known "dissolve
    // muddiness". Linearising via gamma 2.2 before the lerp and re-encoding
    // afterwards produces the smooth perceptual brightness used in Premiere
    // / DaVinci's Film Dissolve. We do this per-pixel on a software image,
    // so it's heavier than CrossDissolve but only fires during the
    // transition window.
    case TransitionType::FilmDissolve: {
        const double a = qBound(0.0, 1.0 - progress, 1.0);
        const double b = 1.0 - a;
        // Pre-build a small lookup table to avoid std::pow per pixel.
        static int lin2srgb[1001];
        static uint8_t srgb2lin_built = 0;
        static double srgb2lin[256];
        if (!srgb2lin_built) {
            for (int i = 0; i < 256; ++i)
                srgb2lin[i] = std::pow(i / 255.0, 2.2);
            for (int i = 0; i <= 1000; ++i)
                lin2srgb[i] = qBound(0, static_cast<int>(
                    std::pow(i / 1000.0, 1.0 / 2.2) * 255.0 + 0.5), 255);
            srgb2lin_built = 1;
        }
        QImage from888 = fromScaled.format() == QImage::Format_RGB888
            ? fromScaled : fromScaled.convertToFormat(QImage::Format_RGB888);
        QImage to888 = toScaled.format() == QImage::Format_RGB888
            ? toScaled : toScaled.convertToFormat(QImage::Format_RGB888);
        const int W = qMin(from888.width(), to888.width());
        const int H = qMin(from888.height(), to888.height());
        for (int y = 0; y < H; ++y) {
            const uint8_t *fr = from888.constScanLine(y);
            const uint8_t *tr = to888.constScanLine(y);
            uint8_t *dr = result.scanLine(y);
            for (int x = 0; x < W; ++x) {
                for (int ch = 0; ch < 3; ++ch) {
                    const double lin = a * srgb2lin[fr[x * 3 + ch]]
                                     + b * srgb2lin[tr[x * 3 + ch]];
                    dr[x * 3 + ch] = static_cast<uint8_t>(
                        lin2srgb[qBound(0, static_cast<int>(lin * 1000.0 + 0.5), 1000)]);
                }
            }
        }
        // Painter is now stale — we wrote directly to result. Discard the
        // painter so its destruction doesn't blank what we just drew.
        painter.end();
        return result;
    }

    // Camera Shake: cross-dissolve with frame jitter peaking at p=0.5.
    // Uses pseudo-random offsets per progress phase so the shake animates
    // (different jitter each frame). Used for action cuts, impact moments,
    // music-video bridges. Software-only — pure translate of the layers.
    case TransitionType::CameraShake: {
        const double intensity = qMin(1.0, 4.0 * progress * (1.0 - progress));
        const int maxShake = static_cast<int>(intensity * qMax(w, h) * 0.04);
        // Phase-driven hash so offset changes per frame but stays
        // deterministic for cache purposes.
        const uint32_t phase = static_cast<uint32_t>(progress * 4096.0);
        auto jitter = [&](uint32_t seed) -> QPoint {
            uint32_t hX = seed * 73856093u ^ phase * 19349663u;
            uint32_t hY = seed * 83492791u ^ (phase + 17u) * 22229u;
            hX ^= hX >> 13; hX *= 0x5bd1e995u; hX ^= hX >> 15;
            hY ^= hY >> 13; hY *= 0x9e3779b1u; hY ^= hY >> 15;
            const int dx = static_cast<int>((static_cast<int>(hX & 0xff) - 128) / 128.0 * maxShake);
            const int dy = static_cast<int>((static_cast<int>(hY & 0xff) - 128) / 128.0 * maxShake);
            return QPoint(dx, dy);
        };
        const QPoint aOff = jitter(1u);
        const QPoint bOff = jitter(2u);
        // Black background covers any exposed edge from the shake.
        painter.fillRect(0, 0, w, h, Qt::black);
        painter.setOpacity(1.0 - progress);
        painter.drawImage(aOff.x(), aOff.y(), fromScaled);
        painter.setOpacity(progress);
        painter.drawImage(bOff.x(), bOff.y(), toScaled);
        painter.setOpacity(1.0);
        break;
    }

    // Color Channel Shift: clean RGB plane separation crossfade. Distinct
    // from Glitch (which has scanline displacement noise). Three colour
    // planes drift apart symmetrically around the centre, then re-converge
    // on B. Looks like a chromatic-aberration crossfade — popular in
    // tech / sci-fi edits.
    case TransitionType::ColorChannelShift: {
        const double bias = qMin(1.0, 4.0 * progress * (1.0 - progress));
        const int maxShift = static_cast<int>(bias * qMax(w, h) * 0.05);
        // Source: A in first half, B in second half, with an opacity
        // crossfade at the boundary so the channel-shifted A and B don't
        // pop.
        const double srcMix = progress;
        QImage from888 = fromScaled.format() == QImage::Format_RGB888
            ? fromScaled : fromScaled.convertToFormat(QImage::Format_RGB888);
        QImage to888 = toScaled.format() == QImage::Format_RGB888
            ? toScaled : toScaled.convertToFormat(QImage::Format_RGB888);
        const int W = qMin(from888.width(), to888.width());
        const int H = qMin(from888.height(), to888.height());
        for (int y = 0; y < H; ++y) {
            const uint8_t *fr = from888.constScanLine(y);
            const uint8_t *tr = to888.constScanLine(y);
            uint8_t *dr = result.scanLine(y);
            for (int x = 0; x < W; ++x) {
                // Red shifted right, Blue shifted left, Green stays. We
                // sample from A and B at the channel-displaced positions
                // and blend by srcMix.
                const int xr = qBound(0, x + maxShift, W - 1);
                const int xg = x;
                const int xb = qBound(0, x - maxShift, W - 1);
                const int rA = fr[xr * 3 + 0], gA = fr[xg * 3 + 1], bA = fr[xb * 3 + 2];
                const int rB = tr[xr * 3 + 0], gB = tr[xg * 3 + 1], bB = tr[xb * 3 + 2];
                dr[x * 3 + 0] = static_cast<uint8_t>(rA * (1.0 - srcMix) + rB * srcMix);
                dr[x * 3 + 1] = static_cast<uint8_t>(gA * (1.0 - srcMix) + gB * srcMix);
                dr[x * 3 + 2] = static_cast<uint8_t>(bA * (1.0 - srcMix) + bB * srcMix);
            }
        }
        painter.end();
        return result;
    }

    // Pixelate: A's effective resolution drops as p→0.5 (massive mosaic),
    // then B's resolution rises as p→1.0. The pixel-size animation creates
    // a clean "mosaic-out, mosaic-in" handoff. Cheap implementation: scale
    // down to a tiny image with FastTransformation, then scale back up
    // (also Fast — that's how we get the chunky blocks).
    case TransitionType::Pixelate: {
        // Mosaic block size in source pixels: 1 (sharp) at p=0 and p=1,
        // peaks at progress=0.5. Use the smaller image dimension to keep
        // tile count consistent across aspect ratios.
        const double bias = qMin(1.0, 4.0 * progress * (1.0 - progress));
        const int minDim = qMin(w, h);
        const int blockSize = qMax(1, static_cast<int>(bias * minDim / 8.0));
        const QImage &srcImg = (progress < 0.5) ? fromScaled : toScaled;
        const double crossOpacity = (progress < 0.5)
            ? 1.0 : (progress - 0.5) * 2.0;
        if (blockSize <= 1) {
            // No pixelation — just cross-dissolve as a degenerate fallback.
            painter.setOpacity(1.0 - progress);
            painter.drawImage(0, 0, fromScaled);
            painter.setOpacity(progress);
            painter.drawImage(0, 0, toScaled);
            break;
        }
        const int smallW = qMax(1, w / blockSize);
        const int smallH = qMax(1, h / blockSize);
        QImage tiny = srcImg.scaled(smallW, smallH,
            Qt::IgnoreAspectRatio, Qt::FastTransformation);
        QImage chunky = tiny.scaled(w, h,
            Qt::IgnoreAspectRatio, Qt::FastTransformation);
        if (progress < 0.5) {
            // Cross-fade chunky-A onto sharp-A so first half stays mostly
            // recognisable until p≈0.5.
            painter.drawImage(0, 0, fromScaled);
            painter.setOpacity(progress * 2.0);
            painter.drawImage(0, 0, chunky);
        } else {
            // Second half: chunky-B fades to sharp-B.
            painter.drawImage(0, 0, chunky);
            painter.setOpacity(crossOpacity);
            painter.drawImage(0, 0, toScaled);
        }
        break;
    }

    // Blur Dissolve: defocus dissolve. A blurs progressively up to p=0.5,
    // then B unblurs from p=0.5 onward. We use a separable box blur via
    // QImage::scaled down-and-up trick — cheap-but-readable defocus.
    // Real Gaussian would need a convolution kernel; the down/up trick is
    // visually similar and ~50× faster on CPU.
    case TransitionType::BlurDissolve: {
        const double bias = qMin(1.0, 4.0 * progress * (1.0 - progress));
        // Defocus radius scaled to image size; 0..1/12 of the smaller dim
        const int minDim = qMin(w, h);
        const int blurR = qMax(0, static_cast<int>(bias * minDim / 12.0));
        const int divisor = qMax(1, blurR);
        const int smallW = qMax(1, w / divisor);
        const int smallH = qMax(1, h / divisor);
        // Pre-scaled bitmaps with smooth interp give the soft-blur look.
        auto softify = [&](const QImage &src) {
            if (blurR <= 0) return src;
            QImage tiny = src.scaled(smallW, smallH,
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            return tiny.scaled(w, h,
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        };
        QImage aSoft = softify(fromScaled);
        QImage bSoft = softify(toScaled);
        if (progress < 0.5) {
            painter.drawImage(0, 0, fromScaled);
            painter.setOpacity(progress * 2.0);
            painter.drawImage(0, 0, aSoft);
        } else {
            // Cross-fade soft-A→soft-B around the midpoint, then sharp-B.
            painter.drawImage(0, 0, aSoft);
            const double midFade = (progress - 0.5) * 2.0;
            painter.setOpacity(midFade);
            painter.drawImage(0, 0, bSoft);
            // Sharp-B revealed in the final quarter.
            if (progress > 0.75) {
                const double sharp = (progress - 0.75) * 4.0;
                painter.setOpacity(sharp);
                painter.drawImage(0, 0, toScaled);
            }
        }
        break;
    }

    // Lens Flare: photographic anamorphic-style flare. Central bright star
    // with cross spokes + 3 secondary dots offset along the diagonal axis
    // (mimicking the typical lens-element ghost chain). Intensity peaks at
    // progress=0.5 over a CrossDissolve. Software rendering only — pixel
    // ops are radial-distance brightness lookups so it scales linearly with
    // resolution.
    case TransitionType::LensFlare: {
        // Base CrossDissolve underneath.
        painter.setOpacity(1.0 - progress);
        painter.drawImage(0, 0, fromScaled);
        painter.setOpacity(progress);
        painter.drawImage(0, 0, toScaled);
        const double intensity = qMin(1.0, 4.0 * progress * (1.0 - progress));
        if (intensity <= 0.001) break;
        painter.setOpacity(1.0);
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        // Hotspot moves diagonally over the transition for cinematic feel.
        const QPointF hot(w * (0.30 + 0.40 * progress),
                          h * (0.35 + 0.20 * progress));
        // Main core: bright white -> warm yellow radial gradient.
        const double coreR = qMax(w, h) * 0.18;
        QRadialGradient core(hot, coreR);
        core.setColorAt(0.00, QColor(255, 250, 220, static_cast<int>(255 * intensity)));
        core.setColorAt(0.20, QColor(255, 230, 160, static_cast<int>(220 * intensity)));
        core.setColorAt(0.55, QColor(255, 180, 90,  static_cast<int>(110 * intensity)));
        core.setColorAt(1.00, QColor(255, 140, 60, 0));
        painter.setBrush(core);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(hot, coreR, coreR);
        // Cross spokes — long horizontal + vertical bright streaks. Use a
        // very-thin gradient via QPainter pen with a long line — cheap.
        const double spokeLen = qMax(w, h) * 0.5;
        QPen spokePen;
        spokePen.setWidth(2);
        spokePen.setColor(QColor(255, 230, 180, static_cast<int>(180 * intensity)));
        painter.setPen(spokePen);
        painter.drawLine(QPointF(hot.x() - spokeLen, hot.y()),
                         QPointF(hot.x() + spokeLen, hot.y()));
        painter.drawLine(QPointF(hot.x(), hot.y() - spokeLen),
                         QPointF(hot.x(), hot.y() + spokeLen));
        // Secondary ghost dots along the diagonal from hotspot to image
        // centre — typical real-lens ghost reflection chain.
        const QPointF imgCenter(w / 2.0, h / 2.0);
        const QPointF axis = imgCenter - hot;
        const QColor ghostColors[3] = {
            QColor(255, 200, 120, static_cast<int>(140 * intensity)),
            QColor(180, 220, 255, static_cast<int>(110 * intensity)),
            QColor(255, 160, 200, static_cast<int>( 90 * intensity))
        };
        const double ghostFractions[3] = { 0.6, 1.2, 1.8 };
        const double ghostRadii[3]    = { coreR * 0.35, coreR * 0.5, coreR * 0.25 };
        painter.setPen(Qt::NoPen);
        for (int i = 0; i < 3; ++i) {
            const QPointF gp = hot + axis * ghostFractions[i];
            QRadialGradient g(gp, ghostRadii[i]);
            g.setColorAt(0.0, ghostColors[i]);
            g.setColorAt(1.0, QColor(ghostColors[i].red(), ghostColors[i].green(),
                                     ghostColors[i].blue(), 0));
            painter.setBrush(g);
            painter.drawEllipse(gp, ghostRadii[i], ghostRadii[i]);
        }
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        break;
    }

    // Film Burn: orange/red flare burning inward from a random edge with
    // a white-hot core. Used in 70s film aesthetics, Tarantino-style
    // chapter cuts, vintage / retro edits. Three-stage gradient (white →
    // orange → red → transparent) with intensity peaking at progress=0.5.
    case TransitionType::FilmBurn: {
        // Base: B fades in below the burn so when the burn clears, B is
        // already there. Flip the dissolve so we see A under the early
        // part of the burn (matches the actual physics of the film
        // chemistry — the leading edge of the burn obliterates A first).
        painter.setOpacity(1.0 - progress);
        painter.drawImage(0, 0, fromScaled);
        painter.setOpacity(progress);
        painter.drawImage(0, 0, toScaled);
        const double intensity = qMin(1.0, 4.0 * progress * (1.0 - progress));
        if (intensity <= 0.001) break;
        painter.setOpacity(1.0);
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        // Burn moves diagonally across the frame — start lower-left, end
        // upper-right. Gradient is conical-ish: bright white core trailing
        // an orange / red plume.
        const double t = progress;
        const QPointF burnFront(w * (-0.2 + 1.4 * t), h * (1.2 - 1.4 * t));
        // Core white-hot: small tight radial.
        const double coreR = qMax(w, h) * 0.10;
        QRadialGradient core(burnFront, coreR);
        core.setColorAt(0.00, QColor(255, 250, 240, static_cast<int>(255 * intensity)));
        core.setColorAt(0.40, QColor(255, 220, 160, static_cast<int>(220 * intensity)));
        core.setColorAt(1.00, QColor(255, 180, 80, 0));
        painter.setBrush(core);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(burnFront, coreR, coreR);
        // Outer plume: large orange / red haze trailing the front.
        const QPointF trail = burnFront - QPointF(w * 0.15, -h * 0.15);
        const double plumeR = qMax(w, h) * 0.45;
        QRadialGradient plume(trail, plumeR);
        plume.setColorAt(0.00, QColor(255, 160, 60,  static_cast<int>(180 * intensity)));
        plume.setColorAt(0.35, QColor(220, 80,  40,  static_cast<int>(140 * intensity)));
        plume.setColorAt(0.70, QColor(150, 30,  20,  static_cast<int>( 60 * intensity)));
        plume.setColorAt(1.00, QColor( 80, 20,  10, 0));
        painter.setBrush(plume);
        painter.drawEllipse(trail, plumeR, plumeR);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        break;
    }

    // Light Leak: warm radial gradient burst layered over a CrossDissolve.
    // The gradient peaks at progress=0.5 and fades at the edges, simulating
    // film light leak / lens flash. Uses a left-to-right traveling hotspot
    // (mimicking the way analog leaks sweep across the gate). Common in
    // wedding / vlog edits as a "warm transition".
    case TransitionType::LightLeak: {
        // Base layer: cross dissolve so A→B continues underneath the leak.
        painter.setOpacity(1.0 - progress);
        painter.drawImage(0, 0, fromScaled);
        painter.setOpacity(progress);
        painter.drawImage(0, 0, toScaled);
        // Leak: radial gradient with warm color, intensity = triangle wave
        // peaking at progress=0.5. Hotspot travels horizontally so the
        // viewer sees the leak "sweep" across the frame.
        const double intensity = qMin(1.0, 4.0 * progress * (1.0 - progress));
        const double cxNorm = qBound(0.15, progress, 0.85);
        const QPointF center(w * cxNorm, h * 0.45);
        QRadialGradient grad(center, qMax(w, h) * 0.6);
        grad.setColorAt(0.00, QColor(255, 230, 170, static_cast<int>(255 * intensity)));
        grad.setColorAt(0.30, QColor(255, 180, 100, static_cast<int>(220 * intensity)));
        grad.setColorAt(0.65, QColor(255, 130, 70, static_cast<int>(110 * intensity)));
        grad.setColorAt(1.00, QColor(255, 100, 50, 0));
        painter.setOpacity(1.0);
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        painter.fillRect(0, 0, w, h, grad);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        break;
    }

    // Flip Horizontal: faked 3D Y-axis rotation via horizontal scale
    // animation. progress 0→0.5 collapses A horizontally to a vertical
    // line; progress 0.5→1.0 unfolds B from the line. No real 3D — but
    // perspective is implied by the symmetric scale + opacity step.
    case TransitionType::FlipHorizontal: {
        painter.fillRect(0, 0, w, h, Qt::black);
        if (progress < 0.5) {
            // First half: A scales horizontally 1.0 → 0.0
            const double sx = 1.0 - 2.0 * progress;
            painter.save();
            painter.translate(w / 2.0, 0);
            painter.scale(sx, 1.0);
            painter.translate(-w / 2.0, 0);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(0, 0, fromScaled);
            painter.restore();
        } else {
            // Second half: B scales horizontally 0.0 → 1.0 (faked back-face
            // unflip — flip the image mirrored to suggest the back of the
            // sheet was facing us at progress=0.5).
            const double sx = 2.0 * progress - 1.0;
            painter.save();
            painter.translate(w / 2.0, 0);
            painter.scale(sx, 1.0);
            painter.translate(-w / 2.0, 0);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(0, 0, toScaled);
            painter.restore();
        }
        break;
    }

    case TransitionType::FlipVertical: {
        painter.fillRect(0, 0, w, h, Qt::black);
        if (progress < 0.5) {
            const double sy = 1.0 - 2.0 * progress;
            painter.save();
            painter.translate(0, h / 2.0);
            painter.scale(1.0, sy);
            painter.translate(0, -h / 2.0);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(0, 0, fromScaled);
            painter.restore();
        } else {
            const double sy = 2.0 * progress - 1.0;
            painter.save();
            painter.translate(0, h / 2.0);
            painter.scale(1.0, sy);
            painter.translate(0, -h / 2.0);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(0, 0, toScaled);
            painter.restore();
        }
        break;
    }

    // Whip Pan: A and B both translate in the same direction at high speed
    // with a fake motion-blur trail (4 stacked semi-transparent shifted
    // copies). The motion peaks at progress=0.5 — A pans out, B pans in,
    // intersecting at the centre. Real motion blur would need GL or a
    // separable Gaussian; the stacked-copy trick is much cheaper and
    // gives the perceptual whip-pan smear that vlog edits use.
    case TransitionType::WhipPanLeft:
    case TransitionType::WhipPanRight: {
        const double sign = (type == TransitionType::WhipPanLeft) ? -1.0 : +1.0;
        // Pan amount uses an ease-in/out so the centre of the transition is
        // where the speed peaks (the smear is widest). Linear progress would
        // make the "blur" look constant.
        const double t = progress;
        const double sShape = (t < 0.5) ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
        // A travels 0 → -2w (off-screen), B travels +2w → 0 (off-screen → in).
        // Sign flips for WhipPanRight.
        const int aShift = static_cast<int>(sign * (-2.0) * w * sShape);
        const int bShift = static_cast<int>(sign * 2.0 * w * (1.0 - sShape));
        painter.fillRect(0, 0, w, h, Qt::black);
        // Motion-blur trail: 4 shifted copies with falling opacity. The
        // direction of trailing is opposite to motion (A drags its tail
        // behind, B pushes its head forward).
        const int trailSteps = 4;
        const double blurStrength = qBound(0.0, 1.0 - qAbs(2.0 * t - 1.0), 1.0); // peaks at t=0.5
        const int aTrail = static_cast<int>(sign * (-w * 0.15) * blurStrength);
        const int bTrail = static_cast<int>(sign * (w * 0.15) * blurStrength);
        for (int i = trailSteps; i >= 0; --i) {
            const double ratio = static_cast<double>(i) / trailSteps;
            const double opacity = (1.0 - ratio * 0.6) * (1.0 - t);
            painter.setOpacity(opacity / (trailSteps + 1));
            painter.drawImage(aShift + static_cast<int>(aTrail * ratio), 0, fromScaled);
        }
        for (int i = trailSteps; i >= 0; --i) {
            const double ratio = static_cast<double>(i) / trailSteps;
            const double opacity = (1.0 - ratio * 0.6) * t;
            painter.setOpacity(opacity / (trailSteps + 1));
            painter.drawImage(bShift + static_cast<int>(bTrail * ratio), 0, toScaled);
        }
        // Sharp final layer — the lead frame at full opacity.
        painter.setOpacity(1.0 - t);
        painter.drawImage(aShift, 0, fromScaled);
        painter.setOpacity(t);
        painter.drawImage(bShift, 0, toScaled);
        painter.setOpacity(1.0);
        break;
    }

    // Glitch: RGB channel separation + horizontal scanline displacement.
    // A → glitched mid-frame → B. Peak distortion at progress=0.5 where
    // the channels split widely apart and scanlines tear horizontally.
    // No shader needed — it's a per-row pixel copy with three colour-
    // channel offsets and a noise displacement.
    case TransitionType::Glitch: {
        const double t = progress;
        // Distortion intensity: triangle peaking at 0.5 → peaks during the
        // middle of the transition, tapers at the edges so A and B remain
        // recognisable around the boundaries.
        const double intensity = qMin(1.0, 4.0 * t * (1.0 - t)); // 0..1, peaks at t=0.5
        // Choose source: first half mostly A, second half mostly B.
        const QImage &srcImg = (t < 0.5) ? fromScaled : toScaled;
        QImage src888 = srcImg.format() == QImage::Format_RGB888
            ? srcImg : srcImg.convertToFormat(QImage::Format_RGB888);
        const int W = src888.width();
        const int H = src888.height();
        const int chShiftR = static_cast<int>(intensity * 24);  // red shifts right
        const int chShiftB = static_cast<int>(intensity * -24); // blue shifts left
        // Random horizontal displacement per scanline using same hash trick
        // as Dither — deterministic per-row, varies per-frame via a phase
        // derived from progress so the glitch animates frame-to-frame.
        const uint32_t phase = static_cast<uint32_t>(t * 1024.0);
        const double maxRowShift = intensity * 40.0;
        for (int y = 0; y < H; ++y) {
            uint32_t hsh = static_cast<uint32_t>(y) * 73856093u
                         ^ phase * 19349663u;
            hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
            const int rowShift = static_cast<int>(
                ((static_cast<int>(hsh & 0xff) - 128) / 128.0) * maxRowShift);
            const uint8_t *sr = src888.constScanLine(y);
            uint8_t *dr = result.scanLine(y);
            for (int x = 0; x < W; ++x) {
                // Red from x + rowShift + chShiftR
                const int xr = qBound(0, x + rowShift + chShiftR, W - 1);
                const int xg = qBound(0, x + rowShift,            W - 1);
                const int xb = qBound(0, x + rowShift + chShiftB, W - 1);
                dr[x * 3 + 0] = sr[xr * 3 + 0];
                dr[x * 3 + 1] = sr[xg * 3 + 1];
                dr[x * 3 + 2] = sr[xb * 3 + 2];
            }
        }
        painter.end();
        return result;
    }

    // Dither Dissolve: per-pixel binary mask. Each pixel flips from A to B
    // independently when its (deterministic) pseudo-random threshold crosses
    // progress. Distinctive crunchy texture vs the smooth CrossDissolve.
    // Threshold table is built once and reused across frames so the same
    // pixel always switches at the same progress — no flicker.
    case TransitionType::DitherDissolve: {
        static constexpr int kDitherSize = 64; // tile to keep RAM small
        static uint8_t threshold[kDitherSize * kDitherSize];
        static bool ditherBuilt = false;
        if (!ditherBuilt) {
            // Hash-based pseudo-random — bit-mixed so neighbouring pixels
            // get uncorrelated thresholds (critical for the dither look,
            // otherwise you get visible bands). Range 0..255.
            for (int y = 0; y < kDitherSize; ++y) {
                for (int x = 0; x < kDitherSize; ++x) {
                    uint32_t h = static_cast<uint32_t>(x) * 73856093u
                               ^ static_cast<uint32_t>(y) * 19349663u;
                    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
                    threshold[y * kDitherSize + x] = static_cast<uint8_t>(h & 0xff);
                }
            }
            ditherBuilt = true;
        }
        QImage from888 = fromScaled.format() == QImage::Format_RGB888
            ? fromScaled : fromScaled.convertToFormat(QImage::Format_RGB888);
        QImage to888 = toScaled.format() == QImage::Format_RGB888
            ? toScaled : toScaled.convertToFormat(QImage::Format_RGB888);
        const int W = qMin(from888.width(), to888.width());
        const int H = qMin(from888.height(), to888.height());
        const uint8_t cutoff = static_cast<uint8_t>(qBound(0.0, progress, 1.0) * 255.0);
        for (int y = 0; y < H; ++y) {
            const uint8_t *fr = from888.constScanLine(y);
            const uint8_t *tr = to888.constScanLine(y);
            uint8_t *dr = result.scanLine(y);
            const uint8_t *thRow = threshold + (y % kDitherSize) * kDitherSize;
            for (int x = 0; x < W; ++x) {
                const bool useB = thRow[x % kDitherSize] < cutoff;
                const uint8_t *src = useB ? tr : fr;
                dr[x * 3 + 0] = src[x * 3 + 0];
                dr[x * 3 + 1] = src[x * 3 + 1];
                dr[x * 3 + 2] = src[x * 3 + 2];
            }
        }
        painter.end();
        return result;
    }

    // Iris Round (Close): inverse of IrisRound — A is shown inside a
    // SHRINKING disc with B filling everywhere outside. At p=0 the disc
    // covers everything (all A). At p=1 the disc is gone (all B).
    case TransitionType::IrisRoundClose: {
        painter.drawImage(0, 0, toScaled);
        const int cx = w / 2;
        const int cy = h / 2;
        const double maxR = std::sqrt(double(cx) * cx + double(cy) * cy) + 2.0;
        const double r = maxR * (1.0 - progress);
        QPainterPath path;
        path.addEllipse(QPointF(cx, cy), r, r);
        painter.setClipPath(path);
        painter.drawImage(0, 0, fromScaled);
        break;
    }

    // Iris Box (Close): inverse of IrisBox — rectangular shrinking centre
    // window of A over a B background.
    case TransitionType::IrisBoxClose: {
        painter.drawImage(0, 0, toScaled);
        const int rectW = static_cast<int>(w * (1.0 - progress));
        const int rectH = static_cast<int>(h * (1.0 - progress));
        const int rx = (w - rectW) / 2;
        const int ry = (h - rectH) / 2;
        if (rectW > 0 && rectH > 0) {
            painter.setClipRect(rx, ry, rectW, rectH);
            painter.drawImage(0, 0, fromScaled);
        }
        break;
    }

    // Barn Door H (Close): two doors closing horizontally toward the
    // centre, hiding A as B fills the centre. Inverse of BarnDoorHorizontal.
    case TransitionType::BarnDoorHClose: {
        painter.drawImage(0, 0, toScaled);
        const int gap = static_cast<int>((w / 2) * (1.0 - progress));
        const int cx = w / 2;
        if (gap > 0) {
            painter.setClipRect(cx - gap, 0, gap * 2, h);
            painter.drawImage(0, 0, fromScaled);
        }
        break;
    }

    // Barn Door V (Close): vertical close.
    case TransitionType::BarnDoorVClose: {
        painter.drawImage(0, 0, toScaled);
        const int gap = static_cast<int>((h / 2) * (1.0 - progress));
        const int cy = h / 2;
        if (gap > 0) {
            painter.setClipRect(0, cy - gap, w, gap * 2);
            painter.drawImage(0, 0, fromScaled);
        }
        break;
    }

    // Clock Wipe (CCW): radial sweep counter-clockwise from 12 o'clock.
    // Mirror of ClockWipe — sign of arc sweep is positive (CCW in Qt).
    case TransitionType::ClockWipeCCW: {
        if (progress >= 1.0) {
            painter.drawImage(0, 0, toScaled);
            break;
        }
        painter.drawImage(0, 0, fromScaled);
        const int cx = w / 2;
        const int cy = h / 2;
        const double maxR = std::sqrt(double(cx) * cx + double(cy) * cy) + 2.0;
        QPainterPath path;
        path.moveTo(cx, cy);
        // Counter-clockwise from 12 o'clock = positive sweep.
        path.arcTo(cx - maxR, cy - maxR, maxR * 2, maxR * 2,
                   90.0, +360.0 * progress);
        path.closeSubpath();
        painter.setClipPath(path);
        painter.drawImage(0, 0, toScaled);
        break;
    }

    // Spin: A rotates and shrinks out while B rotates from a small size and
    // grows in. Common in mobile vlogs / motion graphics. Direction (CW vs
    // CCW) is the sign of the rotation. We use 360° total rotation so each
    // image makes a full turn during the transition.
    case TransitionType::SpinCW:
    case TransitionType::SpinCCW: {
        const double sign = (type == TransitionType::SpinCW) ? +1.0 : -1.0;
        // A: scale 1.0 → 0.0, rotate 0° → 360°, opacity 1.0 → 0.0
        // B: scale 0.0 → 1.0, rotate -360° → 0°, opacity 0.0 → 1.0
        const double aScale = qMax(0.0, 1.0 - progress);
        const double bScale = progress;
        const double aAngle = sign * 360.0 * progress;
        const double bAngle = sign * (-360.0 + 360.0 * progress);

        painter.fillRect(0, 0, w, h, Qt::black);

        auto drawSpun = [&](const QImage &img, double scale,
                            double angle, double opacity) {
            if (scale <= 0.001 || opacity <= 0.001) return;
            painter.save();
            painter.translate(w / 2.0, h / 2.0);
            painter.rotate(angle);
            painter.scale(scale, scale);
            painter.translate(-w / 2.0, -h / 2.0);
            painter.setOpacity(opacity);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(0, 0, img);
            painter.restore();
        };
        drawSpun(fromScaled, aScale, aAngle, 1.0 - progress);
        drawSpun(toScaled,   bScale, bAngle, progress);
        painter.setOpacity(1.0);
        break;
    }

    // Cross Zoom: A scales up while fading, B starts oversized and shrinks
    // into place. Mimics the "punch in" zoom-blur dissolve — popular in
    // music videos, vlogs, action edits. We don't apply a true Gaussian
    // blur (too expensive in QPainter); instead the scale animation +
    // SmoothTransformation interpolation yields the perceptual zoom feel.
    case TransitionType::CrossZoom: {
        // A: scale 1.0 -> ~2.0 over the transition, opacity 1.0 -> 0.0
        // B: scale ~2.0 -> 1.0 over the transition, opacity 0.0 -> 1.0
        const double aScale = 1.0 + progress;          // 1.0 .. 2.0
        const double bScale = 2.0 - progress;          // 2.0 .. 1.0
        const int aw = static_cast<int>(w * aScale);
        const int ah = static_cast<int>(h * aScale);
        const int bw = static_cast<int>(w * bScale);
        const int bh = static_cast<int>(h * bScale);
        if (aScale > 1.0) {
            QImage aZoom = fromScaled.scaled(aw, ah,
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            painter.setOpacity(1.0 - progress);
            painter.drawImage((w - aw) / 2, (h - ah) / 2, aZoom);
        } else {
            painter.setOpacity(1.0 - progress);
            painter.drawImage(0, 0, fromScaled);
        }
        if (bScale > 1.0) {
            QImage bZoom = toScaled.scaled(bw, bh,
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            painter.setOpacity(progress);
            painter.drawImage((w - bw) / 2, (h - bh) / 2, bZoom);
        } else {
            painter.setOpacity(progress);
            painter.drawImage(0, 0, toScaled);
        }
        painter.setOpacity(1.0);
        break;
    }

    default:
        painter.drawImage(0, 0, (progress < 0.5) ? fromScaled : toScaled);
        break;
    }

    return result;
}
