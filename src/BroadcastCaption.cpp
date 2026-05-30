// BroadcastCaption.cpp — CC-1 純粋エンジン実装 (namespace broadcastcc)。
// CEA-608 ロウレベル + SCC エクスポート + CEA-708 基本パケット + JSON 永続化。

#include "BroadcastCaption.h"

#include <cmath>

#include <QJsonArray>
#include <QStringList>

namespace broadcastcc {

// ===========================================================================
// CEA-608 ロウレベル
// ===========================================================================

uint8_t oddParity(uint8_t b)
{
    // 下位 7bit のうち立っている bit を数え、合計が奇数になるよう MSB を立てる。
    uint8_t v = static_cast<uint8_t>(b & 0x7F);
    int ones = 0;
    for (int i = 0; i < 7; ++i) {
        if (v & (1u << i)) ++ones;
    }
    // 1 の数が偶数なら MSB を立てて奇数にする。奇数ならそのまま (MSB=0)。
    if ((ones & 1) == 0) {
        v = static_cast<uint8_t>(v | 0x80);
    }
    return v;
}

bool cea608CharToByte(QChar c, uint8_t& outByte)
{
    const ushort u = c.unicode();
    // CEA-608 基本文字テーブルは ASCII 0x20-0x7F とほぼ一致。範囲外は未対応。
    if (u < 0x20 || u > 0x7F) {
        return false;
    }
    outByte = static_cast<uint8_t>(u);  // 下位 7bit 値 (パリティ付与前)
    return true;
}

QVector<uint8_t> encodeText608(const QString& text)
{
    QVector<uint8_t> out;
    out.reserve(text.size() + 1);
    for (const QChar& ch : text) {
        uint8_t raw = 0;
        if (!cea608CharToByte(ch, raw)) {
            raw = 0x20;  // 未対応文字はスペースで代替
        }
        out.push_back(oddParity(raw));
    }
    // 608 はバイトペア境界で送出されるため、奇数長は null fill (0x00→parity 0x80) で詰める。
    if ((out.size() & 1) != 0) {
        out.push_back(oddParity(0x00));  // = 0x80
    }
    return out;
}

// ---------------------------------------------------------------------------
// 制御コード
// ---------------------------------------------------------------------------
namespace {

// hi/lo (パリティ付与前) を奇パリティ付与して 16bit ペアに合成する。
uint16_t makePair(uint8_t hi, uint8_t lo)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(oddParity(hi)) << 8)
                                 | oddParity(lo));
}

} // namespace

uint16_t cmdRCL() { return makePair(kCea608Ch1ControlHi, kCea608RclLo); }
uint16_t cmdEOC() { return makePair(kCea608Ch1ControlHi, kCea608EocLo); }
uint16_t cmdENM() { return makePair(kCea608Ch1ControlHi, kCea608EnmLo); }
uint16_t cmdEDM() { return makePair(kCea608Ch1ControlHi, kCea608EdmLo); }

uint16_t pac(int row, int col)
{
    // CEA-608 PAC: row 1..15 を 2 バイトに符号化する。
    //   hi バイト: 0x10 | (ch<<3) | rowGroupHigh。Ch1 の row グループ表に従う。
    //   lo バイト: 0x40 | (rowGroupLow<<5) | indent/underline。
    // row → (hi, baseLo) の標準対応表 (Channel 1, パリティ付与前)。
    // 各エントリは {hi, lo} で、lo はその row の base (indent 0, no underline)。
    static const uint8_t kPacRowBase[15][2] = {
        { 0x11, 0x40 }, // row 1
        { 0x11, 0x60 }, // row 2
        { 0x12, 0x40 }, // row 3
        { 0x12, 0x60 }, // row 4
        { 0x15, 0x40 }, // row 5
        { 0x15, 0x60 }, // row 6
        { 0x16, 0x40 }, // row 7
        { 0x16, 0x60 }, // row 8
        { 0x17, 0x40 }, // row 9
        { 0x17, 0x60 }, // row 10
        { 0x10, 0x40 }, // row 11
        { 0x13, 0x40 }, // row 12
        { 0x13, 0x60 }, // row 13
        { 0x14, 0x40 }, // row 14
        { 0x14, 0x60 }, // row 15
    };

    int r = row;
    if (r < 1) r = 1;
    if (r > 15) r = 15;
    uint8_t hi = kPacRowBase[r - 1][0];
    uint8_t lo = kPacRowBase[r - 1][1];

    // indent 列指定: col を 4 単位の indent に量子化 (0..28 → 0..7)。
    int c = col;
    if (c < 0) c = 0;
    if (c > 31) c = 31;
    const int indentStep = c / 4;  // 0..7
    if (indentStep > 0) {
        // indent 付き PAC: lo の下位ビットに (indent<<1) を載せる。
        // base lo (0x40/0x60) はその row の preamble。indent は 0x50/0x70 系へ。
        lo = static_cast<uint8_t>(lo | 0x10 | (static_cast<uint8_t>(indentStep) << 1));
    }

    return makePair(hi, lo);
}

// ===========================================================================
// SCC エクスポート
// ===========================================================================

QString timecodeFromSec(double sec, double frameRate)
{
    if (sec < 0.0) sec = 0.0;
    if (frameRate <= 0.0) frameRate = 30.0;

    // ドロップフレーム表記の判定 (29.97 / 59.94 系)。
    const bool dropFrame = (std::fabs(frameRate - 29.97) < 0.05)
        || (std::fabs(frameRate - 59.94) < 0.05);

    // 表示 fps は整数 fps ベースへ丸め、cue のフレーム位置は floor(sec * fps) で求める。
    const int fpsInt = static_cast<int>(std::lround(frameRate));
    const int safeFps = fpsInt > 0 ? fpsInt : 30;

    const long long totalFrames = static_cast<long long>(std::floor(sec * frameRate));

    long long frames = totalFrames % safeFps;
    long long totalSeconds = totalFrames / safeFps;
    long long ss = totalSeconds % 60;
    long long totalMinutes = totalSeconds / 60;
    long long mm = totalMinutes % 60;
    long long hh = totalMinutes / 60;

    const QChar sep = dropFrame ? QLatin1Char(';') : QLatin1Char(':');
    return QStringLiteral("%1:%2:%3%4%5")
        .arg(hh, 2, 10, QLatin1Char('0'))
        .arg(mm, 2, 10, QLatin1Char('0'))
        .arg(ss, 2, 10, QLatin1Char('0'))
        .arg(sep)
        .arg(frames, 2, 10, QLatin1Char('0'));
}

namespace {

// 16bit ペアを大文字 4 桁 hex 文字列に整形する ("C1E0" 等)。
QString pairToHex(uint16_t pair)
{
    return QStringLiteral("%1").arg(pair, 4, 16, QLatin1Char('0')).toUpper();
}

} // namespace

QString toScc(const QVector<CaptionCue>& cues, double frameRate)
{
    QString out = QStringLiteral("Scenarist_SCC V1.0\n\n");

    for (const CaptionCue& cue : cues) {
        QVector<uint16_t> pairs;
        // pop-on キャプションの標準シーケンス: ENM, RCL, PAC, text..., EOC。
        pairs.push_back(cmdENM());
        pairs.push_back(cmdRCL());
        pairs.push_back(pac(cue.row, cue.col));

        const QVector<uint8_t> bytes = encodeText608(cue.text);
        for (int i = 0; i + 1 < bytes.size(); i += 2) {
            pairs.push_back(static_cast<uint16_t>((static_cast<uint16_t>(bytes[i]) << 8)
                                                  | bytes[i + 1]));
        }
        pairs.push_back(cmdEOC());

        QStringList hexParts;
        hexParts.reserve(pairs.size());
        for (uint16_t p : pairs) {
            hexParts << pairToHex(p);
        }

        out += timecodeFromSec(cue.startSec, frameRate);
        out += QLatin1Char('\t');
        out += hexParts.join(QLatin1Char(' '));
        out += QStringLiteral("\n\n");
    }

    return out;
}

// ===========================================================================
// CEA-708 (基本) DTVCC caption packet
// ===========================================================================

QVector<uint8_t> buildDtvccCaptionPacket(const QString& text, int serviceNumber)
{
    int svc = serviceNumber;
    if (svc < 1) svc = 1;
    if (svc > 6) svc = 6;  // standard service 1..6 のみ扱う

    // --- service block ---
    // service block header: 上位 3bit = service number, 下位 5bit = block size。
    QVector<uint8_t> serviceData;
    for (const QChar& ch : text) {
        const ushort u = ch.unicode();
        // CEA-708 の G0 集合は ASCII 互換 (0x20-0x7F)。範囲外はスペースに代替。
        serviceData.push_back((u >= 0x20 && u <= 0x7F) ? static_cast<uint8_t>(u)
                                                       : static_cast<uint8_t>(0x20));
    }
    int blockSize = serviceData.size();
    if (blockSize > 31) blockSize = 31;  // 5bit に収まる範囲へクランプ

    const uint8_t serviceBlockHeader =
        static_cast<uint8_t>((static_cast<uint8_t>(svc) << 5)
                             | static_cast<uint8_t>(blockSize & 0x1F));

    // --- caption channel packet ---
    // packet header: 上位 2bit = sequence number, 下位 6bit = packet size code。
    // packet_size_code: 実バイト数を 2 で割った値 (0 は 128 バイトを表す特殊値)。
    QVector<uint8_t> packet;
    // service block (header + data) のバイト数。
    const int serviceBlockBytes = 1 + blockSize;
    // packet_size_code は packet header 自身を含む総バイト数を 2 で割った値。
    int totalPacketBytes = 1 + serviceBlockBytes;
    if ((totalPacketBytes & 1) != 0) {
        ++totalPacketBytes;  // null padding 1 バイト分
    }
    int packetSizeCode = totalPacketBytes / 2;
    if (packetSizeCode > 0x3F) packetSizeCode = 0x3F;

    const uint8_t sequenceNumber = 0;  // 単発パケットは sequence 0
    const uint8_t packetHeader =
        static_cast<uint8_t>((static_cast<uint8_t>(sequenceNumber & 0x03) << 6)
                             | static_cast<uint8_t>(packetSizeCode & 0x3F));

    packet.push_back(packetHeader);
    packet.push_back(serviceBlockHeader);
    for (int i = 0; i < blockSize; ++i) {
        packet.push_back(serviceData[i]);
    }
    // 2 バイト境界へ padding。
    if ((packet.size() & 1) != 0) {
        packet.push_back(0x00);
    }
    return packet;
}

// ===========================================================================
// ドキュメントモデル + エクスポート + 永続化
// ===========================================================================

QString exportScc(const BroadcastCaptionDoc& doc)
{
    return toScc(doc.cues, doc.frameRate);
}

QJsonObject toJson(const BroadcastCaptionDoc& doc)
{
    QJsonObject obj;
    obj[QStringLiteral("standard")] = doc.standard;
    obj[QStringLiteral("channel")] = doc.channel;
    obj[QStringLiteral("frameRate")] = doc.frameRate;

    QJsonArray cuesArr;
    for (const CaptionCue& cue : doc.cues) {
        QJsonObject c;
        c[QStringLiteral("startSec")] = cue.startSec;
        c[QStringLiteral("endSec")] = cue.endSec;
        c[QStringLiteral("text")] = cue.text;
        c[QStringLiteral("row")] = cue.row;
        c[QStringLiteral("col")] = cue.col;
        cuesArr.append(c);
    }
    obj[QStringLiteral("cues")] = cuesArr;
    return obj;
}

BroadcastCaptionDoc fromJson(const QJsonObject& obj)
{
    BroadcastCaptionDoc doc;
    if (obj.contains(QStringLiteral("standard")))
        doc.standard = obj.value(QStringLiteral("standard")).toString(doc.standard);
    if (obj.contains(QStringLiteral("channel")))
        doc.channel = obj.value(QStringLiteral("channel")).toInt(doc.channel);
    if (obj.contains(QStringLiteral("frameRate")))
        doc.frameRate = obj.value(QStringLiteral("frameRate")).toDouble(doc.frameRate);

    const QJsonArray cuesArr = obj.value(QStringLiteral("cues")).toArray();
    for (const QJsonValue& v : cuesArr) {
        const QJsonObject c = v.toObject();
        CaptionCue cue;
        cue.startSec = c.value(QStringLiteral("startSec")).toDouble(0.0);
        cue.endSec = c.value(QStringLiteral("endSec")).toDouble(0.0);
        cue.text = c.value(QStringLiteral("text")).toString();
        cue.row = c.value(QStringLiteral("row")).toInt(15);
        cue.col = c.value(QStringLiteral("col")).toInt(0);
        doc.cues.push_back(cue);
    }
    return doc;
}

BroadcastCaptionDoc fromCues(const QVector<CaptionCue>& cues,
                             const QString& standard,
                             int channel,
                             double frameRate)
{
    BroadcastCaptionDoc doc;
    doc.standard = standard;
    doc.channel = channel;
    doc.frameRate = frameRate;
    doc.cues = cues;
    return doc;
}

} // namespace broadcastcc
