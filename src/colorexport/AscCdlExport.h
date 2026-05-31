#pragma once
// AscCdlExport: ASC CDL (Color Decision List) 書き出しの純粋エンジン。
//
// QObject / QWidget / QApplication を一切使わない純粋・決定的なロジックのみ。
// headless (QApplication 不要) でテスト可能。UI 層はこのエンジンを呼び出すだけにする。
//
// 前提・規約:
//   - QtCore のみ使用 (QString, QVector, QStringList, QXmlStreamWriter 等)。外部依存ゼロ。
//   - 生成は完全に決定的: 同じ入力からは常にバイト同一の文字列を返す
//     (乱数・時刻・ロケール依存の整形は使わない)。
//   - 数値整形は QString::number(v, 'f', 6) で小数 6 桁固定。
//     SOP のトリプルは半角スペース 1 個区切り (例: "1.000000 1.000000 1.000000")。
//
// 出力規格: ASC CDL v1.01 (xmlns="urn:ASC:CDL:v1.01")。
//   buildCc  … 単一 <ColorCorrection> ドキュメント (.cc)
//   buildCcc … <ColorCorrectionCollection> (.ccc)
//   buildCdl … <ColorDecisionList> (.cdl)

#include <QString>
#include <QStringList>
#include <QVector>

namespace asccdl {

// ASC CDL の 1 補正 (1 つの <ColorCorrection>)。
//   SOP (Slope/Offset/Power) は RGB 各チャンネル独立の 3 要素、
//   Saturation はスカラ 1 要素。
struct CdlCorrection {
    QString id;                     // <ColorCorrection id="...">。空なら id 属性を省略する。
    double  slope[3]  = {1, 1, 1};  // Slope  (乗数, 既定 1)。
    double  offset[3] = {0, 0, 0};  // Offset (加算, 既定 0)。
    double  power[3]  = {1, 1, 1};  // Power  (ガンマ, 既定 1)。
    double  saturation = 1.0;       // Saturation (彩度, 既定 1)。
};

// アプリのカラーホイール (Lift/Gamma/Gain + Saturation) を ASC CDL の SOP に変換する。
//
//   注意: これは厳密な数学的等価変換ではなく、相互運用のための「近似変換」である。
//   アプリ内部のグレーディングモデルと ASC CDL の SOP モデルは完全には一致しないため、
//   他ツールへ書き出した際の見えは目安として扱うこと。
//
// アプリのカラーホイールは UI 値 (lift/gain 既定 0, gamma 既定 1) を前提に、
// GLPreview::setLiftGammaGain() と fragment shader の実効値へ揃えてから SOP にする。
// ただし GLPreview の gain は power 適用後の乗算、ASC CDL の slope は power 適用前の
// 乗算なので、非恒等 gamma では厳密一致ではなく自然な SOP 近似として出力する:
//   slope[c]  = clamp(pow(2, 2 * (gain[c] + gainLuma)), 0, 16)
//   offset[c] = 0.5 * (lift[c] + liftLuma)
//   power[c]  = 1.0 / max(1e-3, gamma[c] * gammaLuma)
//   saturation = max(0.0, saturation)
CdlCorrection fromLgg(const double lift[3],  double liftLuma,
                      const double gamma[3], double gammaLuma,
                      const double gain[3],  double gainLuma,
                      double saturation, const QString& id);

// 単一 <ColorCorrection> ドキュメント (.cc 形式) を返す。
QString buildCc(const CdlCorrection& c);

// <ColorCorrectionCollection> (.ccc 形式) を返す。list の各要素を ColorCorrection として列挙する。
QString buildCcc(const QVector<CdlCorrection>& list);

// <ColorDecisionList> (.cdl 形式) を返す。
// 各補正を <ColorDecision><ColorCorrection>...</ColorCorrection></ColorDecision> で包む。
QString buildCdl(const QVector<CdlCorrection>& list);

} // namespace asccdl
