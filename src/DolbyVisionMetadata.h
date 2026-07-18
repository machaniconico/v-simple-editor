#pragma once

// DolbyVisionMetadata — Dolby Vision の動的メタデータモデル + Dolby Vision XML 生成を
// 担う純粋エンジン (DV-1)。既存の HDR10 / HLG は表示転送・マスタリングディスプレイ
// (ST.2086) の静的メタデータまでだが、Dolby Vision はショット単位の動的メタデータ
// (Level1 輝度 / Level2 trim / Level5 アクティブエリア / Level6 CLL-FALL) を持つ。
//
// フル DV エンコード (RPU 埋め込み) は Dolby のライセンスを要するため、本エンジンは
//   - PQ (SMPTE ST.2084) 輝度⇔符号値変換
//   - Level1/2/5/6 のメタデータ構造体モデル
//   - Dolby Vision XML (CMv4.0 風 DolbyLabsMDF) の生成
// までを提供する。生成した XML は外部 RPU 生成ツール (dovi_tool 等) の入力になり得る。
//
// 設計方針:
//   - QObject / QWidget を一切持たない純粋関数・構造体 (namespace dolbyvision)。
//     QApplication 不要で headless 単体テスト可能 (--selftest=dolby-vision,
//     needsQApplication=false)。前例: namespace aces (AcesColor.h)。
//   - PQ 変換は double。XML 出力時のみ 0..4095 (12bit) 整数へ量子化する。
//   - Qt は QString / QVector / QJsonObject のみ使用 (永続化と XML 整形に必要な範囲)。
//
// 出典:
//   - PQ EOTF/逆EOTF : SMPTE ST.2084 (Dolby PQ)。10000 nits 基準, 0..1 の正規化符号値。
//       m1 = 2610/16384, m2 = (2523/4096)*128, c1 = 3424/4096,
//       c2 = (2413/4096)*32, c3 = (2392/4096)*32
//   - メタデータ階層 : Dolby Vision CMv4.0 (Level1/2/5/6) + ST.2086 / CTA-861 (CLL/FALL)

#include <vector>
#include <cmath>

#include <QString>
#include <QVector>
#include <QJsonObject>

namespace dolbyvision {

// ---------------------------------------------------------------------------
// PQ (SMPTE ST.2084) 定数。10000 nits をピークとした正規化輝度に対する符号値曲線。
// ---------------------------------------------------------------------------
constexpr double kPqM1 = 2610.0 / 16384.0;          // = 0.1593017578125
constexpr double kPqM2 = (2523.0 / 4096.0) * 128.0; // = 78.84375
constexpr double kPqC1 = 3424.0 / 4096.0;           // = 0.8359375
constexpr double kPqC2 = (2413.0 / 4096.0) * 32.0;  // = 18.8515625
constexpr double kPqC3 = (2392.0 / 4096.0) * 32.0;  // = 18.6875
constexpr double kPqPeakNits = 10000.0;             // PQ 基準ピーク輝度

// ---------------------------------------------------------------------------
// PQ 変換 (スカラ)
//   nitsToPq : 絶対輝度 [nits] -> PQ 符号値 [0..1]。
//   pqToNits : PQ 符号値 [0..1] -> 絶対輝度 [nits]。
//   nitsToPq(pqToNits(x)) ≈ x の round-trip。nitsToPq(0)=0, nitsToPq(10000)=1。
// ---------------------------------------------------------------------------
double nitsToPq(double nits);
double pqToNits(double pq);

// nitsToPq を 0..4095 (12bit) 整数に量子化 (Dolby Vision XML の Level1 出力用)。
int nitsToPq12bit(double nits);

// ---------------------------------------------------------------------------
// メタデータ階層 (Dolby Vision CMv4.0)
// ---------------------------------------------------------------------------

// Level1: per-shot 輝度統計 (XML には PQ 12bit 整数で出力)。
struct L1Metadata {
    double minNits = 0.0;   // ショット最小輝度
    double avgNits = 0.0;   // ショット平均輝度
    double maxNits = 0.0;   // ショット最大輝度
};

// Level2: ターゲットディスプレイ別 trim。targetNits = そのトリムが想定する
// ターゲットピーク輝度 (例 100 = SDR 100nit, 600, 1000...)。
struct L2Trim {
    int targetNits = 100;
    double slope = 0.0;          // ゲイン (lift/gain 系)
    double offset = 0.0;         // オフセット (lift)
    double power = 0.0;          // ガンマ (gamma)
    double chromaWeight = 0.0;   // 彩度ウェイト
    double saturationGain = 0.0; // サチュレーションゲイン
    double toneDetail = 0.0;     // トーンディテール
};

// Level5: アクティブエリア (レターボックス・ピラーボックスの余白オフセット, px)。
struct L5ActiveArea {
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
};

// Level6: ST.2086 マスタリングディスプレイ + CTA-861 CLL/FALL (静的, クリップ全体)。
struct L6Metadata {
    int maxCll = 0;               // MaxCLL (Content Light Level), nits
    int maxFall = 0;              // MaxFALL (Frame-Average Light Level), nits
    int masteringMaxNits = 1000;  // マスタリングディスプレイ最大輝度
    int masteringMinNits = 0;     // マスタリングディスプレイ最小輝度 (×0.0001 nit 単位で出力)
};

// per-shot 動的メタデータ。1 ショット = [startSec, endSec)。
struct DvShot {
    double startSec = 0.0;
    double endSec = 0.0;
    L1Metadata l1;
    QVector<L2Trim> trims;  // 0 個以上のターゲット別 trim
    L5ActiveArea l5;
};

// Dolby Vision メタデータ全体。profile 5 (single-layer IPTPQc2) または
// 81 (profile 8.1, HDR10 互換ベース) を主に想定する。
struct DolbyVisionMetadata {
    int profile = 81;   // 5 or 81 (8.1)
    int level = 6;      // DV level
    QString title;      // 作品/シーケンス名
    L6Metadata l6;
    QVector<DvShot> shots;
};

// ---------------------------------------------------------------------------
// XML 生成 / 検証 / 永続化
// ---------------------------------------------------------------------------

// Dolby Vision XML (CMv4.0 風) を整形済み文字列で返す。
//   構造: <DolbyLabsMDF> ... <Outputs><Output><Track><Shots>
//           <Shot><Record inFrame="..." outFrame="..."/>
//             <PluginNode><DVDynamicData>
//               <Level1 minPQ="..." maxPQ="..." midPQ="..."/>   (PQ 12bit 整数)
//               <Level2 level="2" targetNits="..." .../>  (trim ごとに繰り返し)
//               <Level5 level="5" activeArea*Offset="..."/>
//             </DVDynamicData></PluginNode>
//           </Shot> ...
//         <Level6><MaxCLL/><MaxFALL/><MasteringDisplay .../></Level6>
// frameRate は Record の inFrame / outFrame を frame 単位に換算するのに使う (既定 24)。
QString toDolbyVisionXml(const DolbyVisionMetadata& meta, double frameRate = 24.0);

// メタデータの整合性検証。profile が 5/81、level>=1、各ショットの
// L1 min<=avg<=max、startSec<=endSec、ショット時間が非負であること等を確認。
// 失敗時は errorOut (nullable) に最初の不整合理由を日本語で書く。
bool validate(const DolbyVisionMetadata& meta, QString* errorOut);

// DolbyVisionMetadata <-> QJsonObject (プロジェクト保存用, 全フィールド round-trip)。
QJsonObject toJson(const DolbyVisionMetadata& meta);
DolbyVisionMetadata fromJson(const QJsonObject& obj);

} // namespace dolbyvision
