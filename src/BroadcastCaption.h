#pragma once

// BroadcastCaption — 北米放送納品向けの CEA-608 / CEA-708 クローズドキャプション +
// SCC (Scenarist Closed Caption) サイドカー生成を担う純粋エンジン (CC-1)。
//
// 既存の字幕系 (src/SubtitleIO.h の SRT/VTT、CaptionTrack の焼き込み、字幕翻訳) は
// "見える字幕" 止まりで、放送規格の CC データ (CEA-608 line-21 / CEA-708 DTVCC) を
// 持たなかった。放送局・配信プラットフォームへの納品では SCC サイドカーや 608/708
// バイトストリームが要求されるため、本エンジンでそのロウレベル生成を提供する。
//
// 提供範囲:
//   - CEA-608 ロウレベル: 奇パリティ付与、ASCII→608 基本文字バイト、テキストの
//     データバイト列化、PAC (Preamble Address Code) / RCL・EOC・ENM・EDM 等の制御コード。
//   - SCC エクスポート (Scenarist_SCC V1.0): timecode<TAB>hex-byte-pair 行の生成。
//   - CEA-708 (基本): DTVCC caption packet (packet header + service block + text) の構築。
//   - BroadcastCaptionDoc モデル + SCC エクスポート + JSON 永続化 round-trip。
//
// 設計方針:
//   - QObject / QWidget を一切持たない純粋関数・構造体 (namespace broadcastcc)。
//     QApplication 不要で headless 単体テスト可能 (--selftest=broadcast-cc,
//     needsQApplication=false)。前例: namespace dolbyvision (DolbyVisionMetadata.h)。
//   - Qt は QString / QVector / QJsonObject のみ。バイト列は QVector<uint8_t>。
//
// 出典:
//   - CEA-608-E: line-21 closed caption。7bit データ + 奇パリティ (MSB)。
//       基本文字テーブルは ASCII 0x20-0x7F とほぼ一致 (一部 special char を除く)。
//       制御コード (PAC / mid-row / コマンド) は 2 バイトペア。
//   - CEA-708-D: DTVCC。caption channel packet → service block → コマンド/テキスト。
//   - Scenarist Closed Caption V1.0 (.scc): "Scenarist_SCC V1.0" ヘッダ + 各行
//       "HH:MM:SS:FF<TAB>aaaa bbbb cccc ..." (16進ペア space 区切り)。29.97fps は ';' 区切り。

#include <cstdint>
#include <vector>

#include <QString>
#include <QVector>
#include <QJsonObject>

namespace broadcastcc {

// ---------------------------------------------------------------------------
// 字幕 cue (1 行のテキスト + 表示時刻 + 画面上の行/列)。
//   text は 1 行を想定 (改行を含む場合は呼び出し側で複数 cue に分割するか、
//   内部で行ごとに PAC を発行する)。row は 1..15 (line-21 の表示行)、col は 0..31。
// ---------------------------------------------------------------------------
struct CaptionCue {
    double startSec = 0.0;
    double endSec = 0.0;
    QString text;
    int row = 15;  // 1..15 (画面下端寄りが 15)
    int col = 0;   // 0..31 (indent)
};

// ---------------------------------------------------------------------------
// CEA-608 ロウレベル
// ---------------------------------------------------------------------------

// 下位 7bit に対し最上位 bit (MSB) を奇パリティになるよう設定して返す。
// CEA-608 は odd parity (1 の総数が奇数になるよう MSB を立てる)。
// 例: 0x41 ('A', 下位7bitの1の数=2=偶数) -> MSB=1 -> 0xC1。
uint8_t oddParity(uint8_t b);

// ASCII の標準文字 (0x20-0x7F) を CEA-608 基本文字バイト (パリティ付与前, 下位7bit) に
// 変換する。未対応文字 (制御文字や 0x80 以上) は false を返し outByte は変更しない。
// 注: 608 基本文字テーブルは ASCII とほぼ一致するため、本実装は 0x20-0x7F を
// そのまま下位7bit値として採用する (special/extended char は範囲外とする)。
bool cea608CharToByte(QChar c, uint8_t& outByte);

// 文字列を CEA-608 データバイト列 (パリティ付与済み) に変換する。
// 各文字を cea608CharToByte で 7bit 値に直し oddParity を被せる。
// 未対応文字はスペース (0x20) で代替する。
// 奇数長になった場合は末尾に oddParity(0x00) (= 0x80, null fill 相当) を 1 個足して
// 偶数長 (バイトペア境界) に揃える。
QVector<uint8_t> encodeText608(const QString& text);

// ---------------------------------------------------------------------------
// CEA-608 制御コード (Channel 1, パリティ付与済みの 16bit ペア [hi<<8 | lo])。
//   RCL: Resume Caption Loading (pop-on バッファ書き込み開始)
//   EOC: End Of Caption (バッファを表示メモリへ swap)
//   ENM: Erase Non-displayed Memory (バッファ消去)
//   EDM: Erase Displayed Memory (表示消去)
//   EOC/EDM/ENM/RCL はいずれも 0x14,0x2x 系の Ch1 コマンド。
// ---------------------------------------------------------------------------
constexpr uint8_t kCea608Ch1ControlHi = 0x14;  // パリティ付与前の hi バイト (Ch1)
constexpr uint8_t kCea608RclLo = 0x20;  // Resume Caption Loading
constexpr uint8_t kCea608EdmLo = 0x2C;  // Erase Displayed Memory
constexpr uint8_t kCea608EncLo = 0x2D;  // Carriage Return / (一部 ENC) — 参考
constexpr uint8_t kCea608EnmLo = 0x2E;  // Erase Non-displayed Memory
constexpr uint8_t kCea608EocLo = 0x2F;  // End Of Caption (swap)

// 上記コマンドをパリティ付与済み 16bit ペアで返す。
uint16_t cmdRCL();  // Resume Caption Loading
uint16_t cmdEOC();  // End Of Caption
uint16_t cmdENM();  // Erase Non-displayed Memory
uint16_t cmdEDM();  // Erase Displayed Memory

// PAC (Preamble Address Code): カーソルを (row, col) へ移動する 2 バイトコマンド。
// row 1..15、col 0..31 (4 の倍数 indent 単位)。戻り値はパリティ付与済み 16bit ペア。
uint16_t pac(int row, int col);

// ---------------------------------------------------------------------------
// SCC エクスポート (Scenarist Closed Caption V1.0)
// ---------------------------------------------------------------------------

// 秒を SMPTE timecode "HH:MM:SS:FF" (ノンドロップ) または "HH:MM:SS;FF"
// (29.97/59.94fps ドロップフレーム表記) に変換する。FF は floor(sec * frameRate)
// から求めたフレーム番号 (0..round(frameRate)-1)。
// frameRate が 29.97 / 59.94 近傍のときは ';' 区切りを用いる。
QString timecodeFromSec(double sec, double frameRate);

// CaptionCue 列を Scenarist SCC V1.0 文字列に変換する。
// 出力: "Scenarist_SCC V1.0\n\n" + 各 cue ごとに
//   "<timecode><TAB><hex pair> <hex pair> ...\n\n"
// コマンドシーケンスは ENM, RCL, PAC(row,col), text bytes..., EOC を 16進ペア
// (大文字 2 桁 × 2 = 4 桁 / ペア, space 区切り) で並べる。
QString toScc(const QVector<CaptionCue>& cues, double frameRate);

// ---------------------------------------------------------------------------
// CEA-708 (基本)
// ---------------------------------------------------------------------------

// DTVCC caption packet を構築する (完全実装ではなく packet 構造を持たせる範囲)。
// 構造: [packet header (sequence/packet-size)] [service block header (service# + block-size)]
//        [text: 1 バイト/文字 (ASCII)]。
// serviceNumber は 1..6 (standard service)。戻り値は packet バイト列。
QVector<uint8_t> buildDtvccCaptionPacket(const QString& text, int serviceNumber = 1);

// ---------------------------------------------------------------------------
// ドキュメントモデル + エクスポート + 永続化
// ---------------------------------------------------------------------------

struct BroadcastCaptionDoc {
    QString standard = QStringLiteral("CEA-608");  // "CEA-608" / "CEA-708"
    int channel = 1;          // CEA-608 caption channel (CC1=1 ... CC4=4)
    double frameRate = 29.97; // SCC timecode 換算用フレームレート
    QVector<CaptionCue> cues;
};

// BroadcastCaptionDoc を SCC 文字列にエクスポート (toScc ラッパー)。
QString exportScc(const BroadcastCaptionDoc& doc);

// BroadcastCaptionDoc <-> QJsonObject (プロジェクト保存用, 全フィールド round-trip)。
QJsonObject toJson(const BroadcastCaptionDoc& doc);
BroadcastCaptionDoc fromJson(const QJsonObject& obj);

// 既存字幕 (時刻付きテキスト cue) から BroadcastCaptionDoc を組むヘルパ。
// standard / channel / frameRate は既定値。各 cue は row=15, col=0 で配置する。
BroadcastCaptionDoc fromCues(const QVector<CaptionCue>& cues,
                             const QString& standard = QStringLiteral("CEA-608"),
                             int channel = 1,
                             double frameRate = 29.97);

} // namespace broadcastcc
