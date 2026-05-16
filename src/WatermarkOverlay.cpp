#include "WatermarkOverlay.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QTransform>

namespace watermark {

// ---------------------------------------------------------------------------
// buildWatermarkPixmap
// Build the watermark stamp at the desired logical size.
// ---------------------------------------------------------------------------
static QPixmap buildWatermarkPixmap(const WmConfig &cfg, int frameWidth, int frameHeight)
{
    if (cfg.mode == Mode::Image) {
        if (cfg.imagePath.isEmpty()) {
            return QPixmap();
        }
        QPixmap src(cfg.imagePath);
        if (src.isNull()) {
            return QPixmap();
        }
        const int targetW = qMax(1, static_cast<int>(frameWidth * cfg.scale));
        return src.scaledToWidth(targetW, Qt::SmoothTransformation);
    }

    // Text mode — measure the string and render it into a pixmap
    const int pointSize = qMax(6, static_cast<int>(frameHeight * cfg.scale));
    QFont font;
    font.setPointSize(pointSize);
    font.setBold(false);

    const QFontMetrics fm(font);
    const QRect textBounds = fm.boundingRect(cfg.text);
    const int pw = textBounds.width()  + 4;
    const int ph = textBounds.height() + 4;

    QPixmap pix(pw, ph);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setFont(font);
    p.setPen(cfg.textColor);
    p.drawText(pix.rect(), Qt::AlignCenter, cfg.text);
    p.end();

    return pix;
}

// ---------------------------------------------------------------------------
// applyWatermark
// ---------------------------------------------------------------------------
QImage applyWatermark(const QImage &frame, const WmConfig &cfg)
{
    if (frame.isNull()) {
        return frame;
    }

    QImage result = frame.convertToFormat(QImage::Format_ARGB32);

    const int fw = result.width();
    const int fh = result.height();

    const QPixmap stamp = buildWatermarkPixmap(cfg, fw, fh);
    if (stamp.isNull()) {
        return result;
    }

    const int sw = stamp.width();
    const int sh = stamp.height();

    // Clamp: negative marginPx pushes corner positions off-canvas and zeroes the tiled step.
    const int margin = qMax(0, cfg.marginPx);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setOpacity(cfg.opacity);

    if (cfg.position == Position::Tiled) {
        // Tiled: repeat the stamp across the whole frame in a grid
        const int spacingX = qMax(1, sw + qMax(sw / 2, margin));
        const int spacingY = qMax(1, sh + qMax(sh / 2, margin));

        for (int y = margin; y < fh; y += spacingY) {
            for (int x = margin; x < fw; x += spacingX) {
                if (cfg.rotationDeg != 0.0) {
                    painter.save();
                    painter.translate(x + sw / 2.0, y + sh / 2.0);
                    painter.rotate(cfg.rotationDeg);
                    painter.drawPixmap(-sw / 2, -sh / 2, stamp);
                    painter.restore();
                } else {
                    painter.drawPixmap(x, y, stamp);
                }
            }
        }
    } else {
        // Compute top-left corner for the stamp
        int dx = 0;
        int dy = 0;

        switch (cfg.position) {
        case Position::TopLeft:
            dx = margin;
            dy = margin;
            break;
        case Position::TopRight:
            dx = fw - sw - margin;
            dy = margin;
            break;
        case Position::BottomLeft:
            dx = margin;
            dy = fh - sh - margin;
            break;
        case Position::BottomRight:
            dx = fw - sw - margin;
            dy = fh - sh - margin;
            break;
        case Position::Center:
            dx = (fw - sw) / 2;
            dy = (fh - sh) / 2;
            break;
        case Position::Tiled:
            break; // handled above
        }

        if (cfg.rotationDeg != 0.0) {
            painter.save();
            painter.translate(dx + sw / 2.0, dy + sh / 2.0);
            painter.rotate(cfg.rotationDeg);
            painter.drawPixmap(-sw / 2, -sh / 2, stamp);
            painter.restore();
        } else {
            painter.drawPixmap(dx, dy, stamp);
        }
    }

    painter.end();
    return result;
}

// ---------------------------------------------------------------------------
// batchApply
// ---------------------------------------------------------------------------
int batchApply(const QStringList &inputPaths, const QString &outDir, const WmConfig &cfg)
{
    int count = 0;
    const QDir dir(outDir);

    for (const QString &path : inputPaths) {
        QImage img(path);
        if (img.isNull()) {
            continue;
        }
        const QImage watermarked = applyWatermark(img, cfg);
        const QString baseName   = QFileInfo(path).fileName();
        const QString outPath    = dir.filePath(baseName);
        if (watermarked.save(outPath)) {
            ++count;
        }
    }

    return count;
}

} // namespace watermark
