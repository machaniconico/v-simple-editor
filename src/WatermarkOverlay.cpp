#include "WatermarkOverlay.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSet>
#include <QTransform>

namespace watermark {

namespace {

QString normalizedPathKey(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

QString nonCollidingOutputPath(const QDir &dir, const QFileInfo &sourceInfo,
                               const QSet<QString> &reservedPaths)
{
    const QString base = sourceInfo.completeBaseName();
    const QString suffix = sourceInfo.suffix();
    const QString extension = suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix;

    for (int attempt = 0; ; ++attempt) {
        QString fileName;
        if (attempt == 0) {
            fileName = sourceInfo.fileName();
        } else if (attempt == 1) {
            fileName = base + QStringLiteral("_watermarked") + extension;
        } else {
            fileName = base + QStringLiteral("_watermarked_") + QString::number(attempt) + extension;
        }

        const QString candidate = dir.filePath(fileName);
        if (!reservedPaths.contains(normalizedPathKey(candidate)) && !QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
}

} // namespace

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

    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setOpacity(cfg.opacity);

    if (cfg.position == Position::Tiled) {
        // Tiled: repeat the stamp across the whole frame in a grid
        const int spacingX = sw + qMax(sw / 2, cfg.marginPx);
        const int spacingY = sh + qMax(sh / 2, cfg.marginPx);

        for (int y = cfg.marginPx; y < fh; y += spacingY) {
            for (int x = cfg.marginPx; x < fw; x += spacingX) {
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
            dx = cfg.marginPx;
            dy = cfg.marginPx;
            break;
        case Position::TopRight:
            dx = fw - sw - cfg.marginPx;
            dy = cfg.marginPx;
            break;
        case Position::BottomLeft:
            dx = cfg.marginPx;
            dy = fh - sh - cfg.marginPx;
            break;
        case Position::BottomRight:
            dx = fw - sw - cfg.marginPx;
            dy = fh - sh - cfg.marginPx;
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
    QSet<QString> reservedPaths;

    for (const QString &path : inputPaths) {
        reservedPaths.insert(normalizedPathKey(path));
    }

    for (const QString &path : inputPaths) {
        QImage img(path);
        if (img.isNull()) {
            continue;
        }
        const QImage watermarked = applyWatermark(img, cfg);
        const QString outPath = nonCollidingOutputPath(dir, QFileInfo(path), reservedPaths);
        if (watermarked.save(outPath)) {
            reservedPaths.insert(normalizedPathKey(outPath));
            ++count;
        }
    }

    return count;
}

} // namespace watermark
