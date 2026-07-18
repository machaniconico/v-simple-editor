#pragma once

// AutoMatte — グリーンバック不要の自動マッティング / 背景除去を純粋アルゴリズムで
// 提供するエンジン (AM-1)。
//
// 既存の chromakey (src/ChromaKeyRefine.h) は HSV 距離による「指定キー色の除去」
// (グリーンバック前提) であり、被写体の前にクロマ色のスクリーンがある撮影を想定する。
// 本エンジンはそれとは別機能で、(a) クリーンプレート (背景のみフレーム) があれば
// 差分マットで、(b) 無ければ四隅の背景色シードによる単純セグメンテーションで、
// クロマスクリーン無しの素材からアルファマットを推定する。ML モデルは同梱せず、
// 外部依存も追加しない (決定的なアルゴリズム実装)。
//
// 提供範囲:
//   - differenceMatte: クリーンプレートとの色差分による二値マット生成。
//   - autoSegment:     四隅背景シード + 中央被写体プライアによるフォールバック推定。
//   - erodeMatte / dilateMatte: モルフォロジ収縮・膨張 (ノイズ除去・穴埋め)。
//   - featherMatte:    ボックスぼかしによるソフト境界化 (0..255 アルファ)。
//   - refineMatte:     erode → dilate → feather を MatteParams で一括適用。
//   - suppressSpill:   境界の背景色かぶり軽減。
//   - composite:       fg over bg を matte をアルファとして線形合成。
//   - applyMatteAsAlpha: RGBA の A チャンネルを matte で置換 (透過 PNG 出力用)。
//   - QImage 変換ヘルパ (UI 用): rgbaToQImage / qimageToRgba。
//
// 設計方針:
//   - QObject / QWidget を一切持たない純粋関数・構造体 (namespace automatte)。
//     QApplication 不要で headless 単体テスト可能 (--selftest=auto-matte,
//     needsQApplication=false)。前例: namespace broadcastcc (BroadcastCaption.h)。
//   - Qt は QImage / QString / QJsonObject のみ。
//   - ピクセルバッファは RGBA8 (std::vector<uint8_t>, 長さ w*h*4)、マットは
//     std::vector<uint8_t> (長さ w*h, 0=背景 / 255=前景。feather 後は 0..255 ソフト)。
//   - すべて純粋・決定的。境界 / 空入力 / サイズ不整合は安全に空 or 入力を返す。

#include <cstdint>
#include <vector>

#include <QImage>
#include <QString>
#include <QJsonObject>

namespace automatte {

// マット精緻化パラメータ。
//   threshold      : 色距離しきい値 (0..1 正規化)。これを超えると前景。
//   erode / dilate : モルフォロジ半径 (px、0 で無効)。
//   featherRadius  : フェザー (ソフト境界) 半径 (px、0 でハード)。
//   spillSuppress  : スピル抑制強度 (0..1、0 で無効)。
struct MatteParams {
    double threshold     = 0.25;
    int    erode         = 0;
    int    dilate        = 0;
    int    featherRadius = 2;
    double spillSuppress = 0.0;
};

// マット + 出力バッファをまとめて返すための任意の集約型。
struct AutoMatteResult {
    std::vector<uint8_t> matte;   // 長さ w*h
    std::vector<uint8_t> output;  // 長さ w*h*4 (RGBA8)
};

// 差分マット (クリーンプレートあり)。
// 各ピクセルの fg と plate の色距離 (正規化 RGB ユークリッド, 0..1) が threshold を
// 超えれば前景 255、以下なら背景 0。サイズ不整合 / 空入力では空を返す。
std::vector<uint8_t> differenceMatte(const std::vector<uint8_t>& fgRgba,
                                     const std::vector<uint8_t>& plateRgba,
                                     int w, int h, double threshold);

// シードベース セグメンテーション (プレート無しフォールバック)。
// 四隅の色を背景シードとし、各ピクセルの「最寄り背景シードとの色距離」が threshold を
// 超えれば前景とする。中央被写体プライア (中心に近いほど前景寄り) を弱く加味する。
// 完全 AI ではないが決定的に動く。空入力では空を返す。
std::vector<uint8_t> autoSegment(const std::vector<uint8_t>& rgba,
                                 int w, int h, double threshold);

// モルフォロジ収縮 (radius 矩形カーネル。前景画素を侵食しノイズを除去)。
std::vector<uint8_t> erodeMatte(const std::vector<uint8_t>& matte, int w, int h, int radius);

// モルフォロジ膨張 (radius 矩形カーネル。前景画素を拡張し穴を埋める)。
std::vector<uint8_t> dilateMatte(const std::vector<uint8_t>& matte, int w, int h, int radius);

// フェザー (ソフトアルファ化)。box-blur を 2 回適用し近似ガウシアンで 0..255 の
// ソフト境界を作る。radius<=0 なら入力をそのまま返す。
std::vector<uint8_t> featherMatte(const std::vector<uint8_t>& matte, int w, int h, int radius);

// refineMatte: erode → dilate → feather を MatteParams の順に適用したソフトマット。
std::vector<uint8_t> refineMatte(const std::vector<uint8_t>& matte, int w, int h,
                                 const MatteParams& params);

// スピル抑制。matte が中間値 (境界) の画素で、最大チャンネルが突出している場合に
// その突出分を他 2 チャンネル平均へ寄せて背景色かぶりを軽減する。amount は 0..1。
// 戻り値は補正後 RGBA (アルファは不変)。
std::vector<uint8_t> suppressSpill(const std::vector<uint8_t>& fgRgba,
                                   const std::vector<uint8_t>& matte,
                                   int w, int h, double amount);

// 合成。fg over bg を matte (0..255 をアルファ a=matte/255 とみなす) で線形合成。
// out = fg*a + bg*(1-a)。アルファは 255 (不透明) として返す。
std::vector<uint8_t> composite(const std::vector<uint8_t>& fgRgba,
                               const std::vector<uint8_t>& bgRgba,
                               const std::vector<uint8_t>& matte,
                               int w, int h);

// アルファ適用。RGBA の A チャンネルを matte の値で置換 (透過 PNG 出力用)。
// RGB は不変。
std::vector<uint8_t> applyMatteAsAlpha(const std::vector<uint8_t>& rgba,
                                       const std::vector<uint8_t>& matte,
                                       int w, int h);

// QImage 変換ヘルパ (UI 用)。
// rgbaToQImage: RGBA8 バッファ → Format_RGBA8888 の QImage (深いコピー)。
QImage rgbaToQImage(const std::vector<uint8_t>& rgba, int w, int h);
// qimageToRgba: 任意フォーマットの QImage → RGBA8 バッファ (Format_RGBA8888 に変換)。
std::vector<uint8_t> qimageToRgba(const QImage& image);

} // namespace automatte
