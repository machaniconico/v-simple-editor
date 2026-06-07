#pragma once
// SafeZone: SNS セーフゾーン/プラットフォーム UI ガイドのプレビュー表示専用オーバーレイ。
// 露出エイド (ExposureAids) と同じ「display-local 適用 = キャッシュ非汚染・export 非変更」
// パターンを採用する。Platform::None (既定) 時は apply() が入力と pixel 同一のコピーを返す
// ため、書き出し画に何も焼き込まれない。
#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

namespace safezone {

enum class Platform { None, TikTok, InstagramReels, YouTubeShorts, Generic };

enum class GuideKind { TitleSafe, ActionSafe, PlatformUi };

struct Guide {
    QRectF    rect;   // ピクセル座標 (outSize 基準)
    GuideKind kind;
    QString   label;  // 右: アクション 等 (任意)
};

// outSize に対するガイド矩形群を返す。
// TitleSafe  = 中央 90% (各辺 5% マージン)
// ActionSafe = 中央 80% (各辺 10% マージン)
// PlatformUi = プラットフォーム別に UI が被さる領域 (下部キャプション帯・右側アクション列・上部)
// Platform::None は空、Generic は TitleSafe+ActionSafe のみ。
QVector<Guide> guides(const QSize& outSize, Platform p);

// display のコピーに半透明ガイドを描いて返す (元画像は不変)。
// TitleSafe/ActionSafe = 細い枠線、PlatformUi = 薄い塗り。opacity で全体の濃さ調整。
// Platform::None または display が null/空なら元画像のコピーをそのまま返す。
QImage apply(const QImage& display, Platform p, double opacity = 0.35);

QString platformName(Platform p);

} // namespace safezone
