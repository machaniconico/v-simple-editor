#pragma once

// AcesColor — シーンリファード色管理 (ACES) の中核変換を自前行列で実装する純粋エンジン
// (AC-1)。OCIO ライブラリは導入せず、ACES/ITU の公開プライマリ・白色点から導出した
// RGB<->XYZ 行列・Bradford 色順応・転送関数・IDT/RRT/ODT パイプラインを std + Qt 最小限
// で提供する。
//
// 既存の LUT / カラーグレーディング / HDR グレーディングは「表示符号値」を直接いじる
// ディスプレイリファードな処理であり、線形シーンリファードな作業色 (ACEScg) を中継する
// 色管理基盤は存在しなかった (acesFilmic はトーンマップ曲線名のみ)。本エンジンはその
// 基盤を担う。
//
// 設計方針:
//   - QObject / QWidget を一切持たない純粋関数群 (namespace aces)。QApplication 不要で
//     headless 単体テスト可能 (--selftest=aces-color, needsQApplication=false)。
//   - 行列は double 3x3 (Mat3 = array<array<double,3>,3>, row-major m[row][col])。
//   - conversionMatrix() は「線形 RGB」前提。転送関数 (oetf/eotf) は含まない。
//   - 既存 MobileColorSpace.h (D65 sRGB<->P3 + Bradford) が前例。本ファイルはより広い色域
//     (Rec2020 / ACEScg(AP1) / ACES2065-1(AP0)) と D60<->D65 の Bradford 順応を扱う。
//
// 出典 (プライマリ chromaticity と白色点):
//   - sRGB / Rec.709 : ITU-R BT.709 primaries, white = D65 (x=0.3127, y=0.3290)
//   - Rec.2020       : ITU-R BT.2020 primaries, white = D65
//   - Display P3     : SMPTE RP 431-2 / DCI-P3 primaries, white = D65 (sRGB 転送)
//   - ACEScg (AP1)   : Academy AP1 primaries,  white = ACES D60 (x=0.32168, y=0.33767)
//   - ACES2065-1(AP0): Academy AP0 primaries,  white = ACES D60
//   各 RGB->XYZ 行列はこれらの chromaticity と白色点から導いた公開値 (8 桁精度)。

#include <array>
#include <cmath>

#include <QString>
#include <QJsonObject>
#include <QImage>

namespace aces {

// ---------------------------------------------------------------------------
// 基本型 (row-major 3x3 行列 / 3 要素ベクトル)
// ---------------------------------------------------------------------------
using Mat3 = std::array<std::array<double, 3>, 3>;
using Vec3 = std::array<double, 3>;

// 行列積 out = a * b。
Mat3 multiply(const Mat3& a, const Mat3& b);
// 行列 m をベクトル v に適用 (m * v)。
Vec3 apply(const Mat3& m, const Vec3& v);
// 3x3 行列の逆行列 (余因子展開)。特異に近い場合は単位行列を返す。
Mat3 inverse(const Mat3& m);
// 単位行列。
Mat3 identity3();

// ---------------------------------------------------------------------------
// 色空間
//   sRGB / Rec709 / Rec2020 / DisplayP3 : 転送関数付きの表示色 (符号値は非線形)
//   LinearSRGB                          : sRGB プライマリの線形 (転送=恒等)
//   ACEScg (AP1) / ACES2065_1 (AP0)     : ACES の作業/交換色 (線形, 白色点=D60)
// ---------------------------------------------------------------------------
enum class ColorSpace {
    sRGB,        // ITU-R BT.709 primaries, D65, sRGB 転送
    Rec709,      // ITU-R BT.709 primaries, D65, BT.709 転送 (sRGB と同等扱い)
    Rec2020,     // ITU-R BT.2020 primaries, D65, BT.709 同等カーブ
    DisplayP3,   // DCI-P3 primaries, D65, sRGB 同等カーブ
    ACEScg,      // Academy AP1 primaries, D60, 線形
    ACES2065_1,  // Academy AP0 primaries, D60, 線形
    LinearSRGB   // ITU-R BT.709 primaries, D65, 線形
};

// 指定色空間の 線形 RGB -> CIE XYZ 行列 (出典は上記コメント参照)。
Mat3 rgbToXyz(ColorSpace cs);
// rgbToXyz の逆 (XYZ -> 線形 RGB)。
Mat3 xyzToRgb(ColorSpace cs);

// Bradford 色順応行列。src 白色点 (XYZ) の光源下の色を dst 白色点の光源下へ順応する。
// D60<->D65 などに使う。src==dst なら単位に近い。
Mat3 bradfordAdaptation(const Vec3& srcWhiteXyz, const Vec3& dstWhiteXyz);

// 線形 RGB(from) -> XYZ -> (白色点が違えば Bradford 順応) -> 線形 RGB(to) の合成行列。
// 転送関数は含まない (両端とも線形前提)。from==to は単位に近い。
Mat3 conversionMatrix(ColorSpace from, ColorSpace to);

// ---------------------------------------------------------------------------
// 転送関数 (スカラ, 正規化 [0..1] 想定だがクランプはしない)
//   oetf: 線形 -> 符号値 (エンコード)。eotf: 符号値 -> 線形 (デコード)。
//   sRGB / DisplayP3 : IEC 61966-2-1 sRGB カーブ
//   Rec709 / Rec2020 : BT.709 OETF / その逆 (本実装では sRGB と同等カーブで近似)
//   LinearSRGB / ACEScg / ACES2065_1 : 線形 (恒等)
// ---------------------------------------------------------------------------
double oetf(ColorSpace cs, double linear);
double eotf(ColorSpace cs, double encoded);

// ---------------------------------------------------------------------------
// シーンリファード パイプライン (IDT / RRT+ODT)
// ---------------------------------------------------------------------------

// IDT: 入力表示符号値 -> eotf で線形化 -> ACEScg(作業色) へ変換した Vec3 を返す。
Vec3 idt(ColorSpace inputSpace, const Vec3& encodedInput);

// RRT+ODT: ACEScg 線形 -> トーンマップ (acesFilmic 近似でハイライト圧縮) -> 出力色の
// プライマリへ変換 -> oetf で符号化した Vec3 を返す。
Vec3 rrtOdt(const Vec3& acescgLinear, ColorSpace outputSpace);

// ACES パイプライン設定。enabled=false なら process() は入力をそのまま返す (identity)。
struct AcesPipeline {
    bool enabled = false;
    ColorSpace input = ColorSpace::sRGB;
    ColorSpace working = ColorSpace::ACEScg;  // 現状 ACEScg 固定前提
    ColorSpace output = ColorSpace::Rec709;
};

// enabled=false なら encodedInput をそのまま返す。true なら idt -> rrtOdt を適用する。
Vec3 process(const AcesPipeline& p, const Vec3& encodedInput);

// 画像レベルの ACES 適用。
//   pipeline.enabled=false なら src をそのまま返す (identity, 変換なし)。
//   enabled=true なら src を RGBA8888 に正規化し、各ピクセルの RGB を [0..1] に正規化 ->
//   process(pipeline, {r,g,b}) -> [0..255] にクランプ戻し、で出力 QImage を作る。アルファは保持。
//   process() は 3ch 結合変換 (per-channel な 1D LUT に分解できない) のため per-pixel 適用だが、
//   同一入力色のキャッシュ (QHash<QRgb,QRgb>) で実画像の色数に対し再計算を避ける。
//   null / empty な src はそのまま返す。
QImage applyPipelineToImage(const QImage& src, const AcesPipeline& pipeline);

// AcesPipeline <-> QJsonObject (プロジェクト保存用)。
QJsonObject pipelineToJson(const AcesPipeline& p);
AcesPipeline pipelineFromJson(const QJsonObject& obj);

// 色空間 <-> 文字列 (UI / JSON 用の安定名)。未知名は sRGB。
QString colorSpaceName(ColorSpace cs);
ColorSpace colorSpaceFromName(const QString& name);

} // namespace aces
