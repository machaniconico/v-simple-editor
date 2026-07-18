// SafeZone.cpp — SNS セーフゾーン/プラットフォーム UI ガイド純粋エンジン。
// display-local 適用専用。export / render / timeline 経路には一切関与しない。
#include "SafeZone.h"
#include <QPainter>
#include <cmath>

namespace safezone {

QVector<Guide> guides(const QSize& outSize, Platform p)
{
    if (p == Platform::None)
        return {};

    const double W = outSize.width();
    const double H = outSize.height();

    QVector<Guide> result;
    result.reserve(5);

    // TitleSafe: 中央 90%、各辺 5% マージン
    result.append({ QRectF(W * 0.05, H * 0.05, W * 0.90, H * 0.90),
                    GuideKind::TitleSafe,
                    QStringLiteral("タイトルセーフ") });

    // ActionSafe: 中央 80%、各辺 10% マージン
    result.append({ QRectF(W * 0.10, H * 0.10, W * 0.80, H * 0.80),
                    GuideKind::ActionSafe,
                    QStringLiteral("アクションセーフ") });

    // PlatformUi — プラットフォーム別
    switch (p) {
    case Platform::TikTok:
        result.append({ QRectF(W * 0.86, H * 0.45, W * 0.14, H * 0.45),
                        GuideKind::PlatformUi,
                        QStringLiteral("TikTok 右: アクション") });
        result.append({ QRectF(0, H * 0.80, W, H * 0.20),
                        GuideKind::PlatformUi,
                        QStringLiteral("TikTok 下部: キャプション") });
        result.append({ QRectF(0, 0, W, H * 0.06),
                        GuideKind::PlatformUi,
                        QStringLiteral("TikTok 上部") });
        break;
    case Platform::InstagramReels:
        result.append({ QRectF(W * 0.86, H * 0.40, W * 0.14, H * 0.45),
                        GuideKind::PlatformUi,
                        QStringLiteral("Reels 右: アクション") });
        result.append({ QRectF(0, H * 0.78, W, H * 0.22),
                        GuideKind::PlatformUi,
                        QStringLiteral("Reels 下部: キャプション") });
        result.append({ QRectF(0, 0, W, H * 0.10),
                        GuideKind::PlatformUi,
                        QStringLiteral("Reels 上部") });
        break;
    case Platform::YouTubeShorts:
        result.append({ QRectF(W * 0.86, H * 0.45, W * 0.14, H * 0.40),
                        GuideKind::PlatformUi,
                        QStringLiteral("Shorts 右: アクション") });
        result.append({ QRectF(0, H * 0.82, W, H * 0.18),
                        GuideKind::PlatformUi,
                        QStringLiteral("Shorts 下部: キャプション") });
        result.append({ QRectF(0, 0, W, H * 0.08),
                        GuideKind::PlatformUi,
                        QStringLiteral("Shorts 上部") });
        break;
    case Platform::Generic:
    default:
        // Generic は TitleSafe + ActionSafe のみ (PlatformUi なし)
        break;
    }

    return result;
}

QImage apply(const QImage& display, Platform p, double opacity)
{
    // Platform::None または null/空 → 元画像のコピーをそのまま返す (byte-identical)
    if (p == Platform::None || display.isNull() || display.size().isEmpty())
        return display.copy();

    QImage result = display.copy();
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int alphaFill = qRound(opacity * 255);

    const QVector<Guide> gs = guides(display.size(), p);
    for (const Guide& g : gs) {
        switch (g.kind) {
        case GuideKind::TitleSafe:
            // シアン細線の枠
            painter.setPen(QPen(QColor(0, 255, 255, 200), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(g.rect);
            break;
        case GuideKind::ActionSafe:
            // 黄細線の枠
            painter.setPen(QPen(QColor(255, 255, 0, 200), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(g.rect);
            break;
        case GuideKind::PlatformUi:
            // 赤の半透明塗り
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 0, 0, alphaFill));
            painter.drawRect(g.rect);
            break;
        }
    }

    painter.end();
    return result;
}

QString platformName(Platform p)
{
    switch (p) {
    case Platform::None:           return QStringLiteral("なし");
    case Platform::TikTok:         return QStringLiteral("TikTok");
    case Platform::InstagramReels: return QStringLiteral("Instagram Reels");
    case Platform::YouTubeShorts:  return QStringLiteral("YouTube Shorts");
    case Platform::Generic:        return QStringLiteral("汎用");
    }
    return QStringLiteral("不明");
}

} // namespace safezone
