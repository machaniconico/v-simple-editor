#include <QByteArray>
#include <QDebug>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../BroadcastCaption.h"

namespace {

// 8bit のうち立っている bit の数を数える (popcount)。
int popcount8(uint8_t b)
{
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        if ((b >> i) & 0x1) {
            ++n;
        }
    }
    return n;
}

// 16bit ペアを大文字 4 桁 hex に整形する (toScc 出力との照合用)。
QString pairHex(uint16_t v)
{
    return QStringLiteral("%1").arg(v, 4, 16, QLatin1Char('0')).toUpper();
}

} // namespace

int runBroadcastCaptionSelftest()
{
    using namespace broadcastcc;

    qInfo().noquote() << "[broadcast-cc] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[broadcast-cc] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[broadcast-cc] FAIL" << name << ":" << msg; };

    // -----------------------------------------------------------------------
    // G1: oddParity — 出力の 8bit popcount が常に奇数 (CEA-608 odd parity)、
    //     下位 7bit は保存される。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        for (int v = 0; v <= 0x7F; ++v) {
            const uint8_t in = static_cast<uint8_t>(v);
            const uint8_t out = oddParity(in);
            // 下位 7bit が保存されていること
            if ((out & 0x7F) != (in & 0x7F)) {
                ok = false;
                detail = QStringLiteral("low7 not preserved for 0x%1 -> 0x%2")
                             .arg(in, 2, 16, QLatin1Char('0')).arg(out, 2, 16, QLatin1Char('0'));
                break;
            }
            // 8bit popcount が奇数であること
            if ((popcount8(out) % 2) != 1) {
                ok = false;
                detail = QStringLiteral("popcount not odd for 0x%1 -> 0x%2 (count=%3)")
                             .arg(in, 2, 16, QLatin1Char('0')).arg(out, 2, 16, QLatin1Char('0'))
                             .arg(popcount8(out));
                break;
            }
        }
        // 既知値: 'A' (0x41, 下位7bit=2個=偶数) -> 0xC1、0x00 -> 0x80。
        if (ok && oddParity(0x41) != 0xC1) {
            ok = false;
            detail = QStringLiteral("oddParity(0x41)=0x%1 expected 0xC1")
                         .arg(oddParity(0x41), 2, 16, QLatin1Char('0'));
        }
        if (ok && oddParity(0x00) != 0x80) {
            ok = false;
            detail = QStringLiteral("oddParity(0x00)=0x%1 expected 0x80")
                         .arg(oddParity(0x00), 2, 16, QLatin1Char('0'));
        }
        if (ok) {
            pass("G1 oddParity makes 8bit popcount odd and preserves low7");
        } else {
            fail("G1 oddParity", detail);
        }
    }

    // -----------------------------------------------------------------------
    // G2: cea608CharToByte — 既知 ASCII が正しい 7bit 値に、未対応文字は false。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        uint8_t b = 0xFF;
        if (ok && (!cea608CharToByte(QChar('A'), b) || b != 0x41)) {
            ok = false;
            detail = QStringLiteral("'A' -> %1 (byte=0x%2)")
                         .arg(cea608CharToByte(QChar('A'), b)).arg(b, 2, 16, QLatin1Char('0'));
        }
        b = 0xFF;
        if (ok && (!cea608CharToByte(QChar(' '), b) || b != 0x20)) {
            ok = false;
            detail = QStringLiteral("space byte=0x%1").arg(b, 2, 16, QLatin1Char('0'));
        }
        b = 0xFF;
        if (ok && (!cea608CharToByte(QChar('~'), b) || b != 0x7E)) {  // 0x7E は 0x20-0x7F 範囲内
            ok = false;
            detail = QStringLiteral("'~' byte=0x%1").arg(b, 2, 16, QLatin1Char('0'));
        }
        // 未対応: 制御文字 (0x0A) と 0x80 以上
        b = 0x55;
        if (ok && cea608CharToByte(QChar(static_cast<ushort>(0x0A)), b)) {
            ok = false;
            detail = QStringLiteral("0x0A unexpectedly accepted");
        }
        if (ok && b != 0x55) {  // false のとき outByte 不変
            ok = false;
            detail = QStringLiteral("outByte mutated on reject (0x%1)").arg(b, 2, 16, QLatin1Char('0'));
        }
        if (ok && cea608CharToByte(QChar(static_cast<ushort>(0x00A9)), b)) {  // © は範囲外
            ok = false;
            detail = QStringLiteral("0x00A9 unexpectedly accepted");
        }
        if (ok) {
            pass("G2 cea608CharToByte maps ASCII and rejects unsupported chars");
        } else {
            fail("G2 cea608CharToByte", detail);
        }
    }

    // -----------------------------------------------------------------------
    // G3: encodeText608 — 偶数長 (バイトペア境界)、各バイトが奇パリティ。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        // 奇数長入力 ("ABC" = 3 文字) -> パディングで偶数長
        const QVector<uint8_t> odd = encodeText608(QStringLiteral("ABC"));
        if ((odd.size() % 2) != 0) {
            ok = false;
            detail = QStringLiteral("odd-length input not padded to even (size=%1)").arg(odd.size());
        }
        if (ok && odd.size() < 3) {
            ok = false;
            detail = QStringLiteral("size too small (%1)").arg(odd.size());
        }
        // 偶数長入力 ("AB")
        const QVector<uint8_t> even = encodeText608(QStringLiteral("AB"));
        if (ok && (even.size() % 2) != 0) {
            ok = false;
            detail = QStringLiteral("even input not even (size=%1)").arg(even.size());
        }
        // 全バイトが奇パリティ
        if (ok) {
            for (uint8_t byte : odd) {
                if ((popcount8(byte) % 2) != 1) {
                    ok = false;
                    detail = QStringLiteral("non-odd-parity byte 0x%1").arg(byte, 2, 16, QLatin1Char('0'));
                    break;
                }
            }
        }
        // 'A','B' が含まれること (oddParity('A')=0xC1)
        if (ok && !odd.contains(oddParity(0x41))) {
            ok = false;
            detail = QStringLiteral("encoded 'A' (0xC1) not present");
        }
        if (ok) {
            pass("G3 encodeText608 pads to even length with odd parity bytes");
        } else {
            fail("G3 encodeText608", detail);
        }
    }

    // -----------------------------------------------------------------------
    // G4: pac(row,col) — row/col を変えると異なる 2 バイト、パリティ付き。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        const uint16_t p1 = pac(15, 0);
        const uint16_t p2 = pac(1, 0);
        const uint16_t p3 = pac(15, 4);
        if (p1 == p2) {
            ok = false;
            detail = QStringLiteral("row change produced same pac (0x%1)").arg(p1, 4, 16, QLatin1Char('0'));
        }
        if (ok && p1 == p3) {
            ok = false;
            detail = QStringLiteral("col change produced same pac (0x%1)").arg(p1, 4, 16, QLatin1Char('0'));
        }
        // hi/lo バイトともパリティ付き (8bit popcount 奇数)
        if (ok) {
            const uint8_t hi = static_cast<uint8_t>((p1 >> 8) & 0xFF);
            const uint8_t lo = static_cast<uint8_t>(p1 & 0xFF);
            if ((popcount8(hi) % 2) != 1 || (popcount8(lo) % 2) != 1) {
                ok = false;
                detail = QStringLiteral("pac bytes not odd parity hi=0x%1 lo=0x%2")
                             .arg(hi, 2, 16, QLatin1Char('0')).arg(lo, 2, 16, QLatin1Char('0'));
            }
        }
        if (ok) {
            pass("G4 pac varies with row/col and yields odd-parity bytes");
        } else {
            fail("G4 pac", detail);
        }
    }

    // -----------------------------------------------------------------------
    // G5: timecodeFromSec — ノンドロップは ':'、29.97/59.94 系は ';'。
    //     現実装は SMPTE の分・10分境界ドロップ番号スキップまでは行わず、
    //     floor(sec * fps) のフレーム番号を整数 fps 表示へ分解する。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        const QString nonDrop = timecodeFromSec(3661.0, 25.0);
        if (nonDrop != QStringLiteral("01:01:01:00")) {
            ok = false;
            detail = QStringLiteral("3661sec@25 -> '%1' expected 01:01:01:00").arg(nonDrop);
        }

        const QString dropFrame = timecodeFromSec(1.0, 29.97);
        if (ok && dropFrame != QStringLiteral("00:00:00;29")) {
            ok = false;
            detail = QStringLiteral("1sec@29.97 -> '%1' expected 00:00:00;29").arg(dropFrame);
        }

        if (ok) {
            pass("G5 timecodeFromSec uses non-drop/drop-frame separators with floor frames");
        } else {
            fail("G5 timecodeFromSec", detail);
        }
    }

    // -----------------------------------------------------------------------
    // G6: toScc — "Scenarist_SCC V1.0" ヘッダ、各 cue 行に timecode と hex ペア。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        QVector<CaptionCue> cues;
        cues.append(CaptionCue{ 1.0, 3.0, QStringLiteral("HELLO"), 15, 0 });
        const QString scc = toScc(cues, 29.97);
        if (!scc.contains(QStringLiteral("Scenarist_SCC V1.0"))) {
            ok = false;
            detail = QStringLiteral("missing header");
        }
        // timecode (1.0 秒 @ 29.97 ドロップフレーム) を含む
        if (ok) {
            const QString tc = timecodeFromSec(1.0, 29.97);
            if (!scc.contains(tc)) {
                ok = false;
                detail = QStringLiteral("missing timecode '%1'").arg(tc);
            }
        }
        // EOC ペア (シーケンス末尾) を含む
        if (ok && !scc.contains(pairHex(cmdEOC()))) {
            ok = false;
            detail = QStringLiteral("missing EOC pair '%1'").arg(pairHex(cmdEOC()));
        }
        if (ok) {
            pass("G6 toScc emits header, timecode and hex pairs");
        } else {
            fail("G6 toScc", detail);
        }
    }

    // -----------------------------------------------------------------------
    // G7: toScc — cue を変えると出力が変わる、hex は大文字 4 桁ペア。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        QVector<CaptionCue> a;
        a.append(CaptionCue{ 1.0, 3.0, QStringLiteral("HELLO"), 15, 0 });
        QVector<CaptionCue> b;
        b.append(CaptionCue{ 1.0, 3.0, QStringLiteral("WORLD"), 15, 0 });
        const QString sccA = toScc(a, 29.97);
        const QString sccB = toScc(b, 29.97);
        if (sccA == sccB) {
            ok = false;
            detail = QStringLiteral("different cues produced identical scc");
        }
        // hex ペアが大文字であること (RCL コマンドペアで確認)
        if (ok) {
            const QString rclHex = pairHex(cmdRCL());
            if (!sccA.contains(rclHex)) {
                ok = false;
                detail = QStringLiteral("uppercase RCL pair '%1' not found").arg(rclHex);
            }
            // 小文字版が存在しないこと (純 hex の小文字混入チェック)
            if (ok && rclHex != rclHex.toLower() && sccA.contains(rclHex.toLower())) {
                ok = false;
                detail = QStringLiteral("lowercase hex '%1' present").arg(rclHex.toLower());
            }
        }
        if (ok) {
            pass("G7 toScc varies per cue and emits uppercase hex pairs");
        } else {
            fail("G7 toScc varies", detail);
        }
    }

    // -----------------------------------------------------------------------
    // G8: buildDtvccCaptionPacket — 非空、packet/サービスブロック構造の整合。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        const QVector<uint8_t> pkt = buildDtvccCaptionPacket(QStringLiteral("HI"), 1);
        if (pkt.isEmpty()) {
            ok = false;
            detail = QStringLiteral("packet empty");
        }
        // 最低: packet header(1) + service block header(1) + text(>=1)
        if (ok && pkt.size() < 3) {
            ok = false;
            detail = QStringLiteral("packet too small (size=%1)").arg(pkt.size());
        }
        // 2 バイト境界 (DTVCC packet は偶数長へ padding)
        if (ok && (pkt.size() % 2) != 0) {
            ok = false;
            detail = QStringLiteral("packet not 2-byte aligned (size=%1)").arg(pkt.size());
        }
        // service block header: 上位 3bit に service# (=1) が載る
        if (ok) {
            const uint8_t svcHeader = pkt.at(1);
            const int svcNum = (svcHeader >> 5) & 0x07;
            if (svcNum != 1) {
                ok = false;
                detail = QStringLiteral("service# in block header=%1 expected 1 (header=0x%2)")
                             .arg(svcNum).arg(svcHeader, 2, 16, QLatin1Char('0'));
            }
        }
        // 長いテキストの packet は短いテキストより大きい
        if (ok) {
            const QVector<uint8_t> longPkt = buildDtvccCaptionPacket(QStringLiteral("HELLO WORLD"), 1);
            if (longPkt.size() <= pkt.size()) {
                ok = false;
                detail = QStringLiteral("long packet (%1) not larger than short (%2)")
                             .arg(longPkt.size()).arg(pkt.size());
            }
            if (ok) {
                const int sizeCode = longPkt.at(0) & 0x3F;
                const int expectedSizeCode = longPkt.size() / 2;
                if (sizeCode != expectedSizeCode) {
                    ok = false;
                    detail = QStringLiteral("packet_size_code=%1 expected %2 for packet size %3")
                                 .arg(sizeCode).arg(expectedSizeCode).arg(longPkt.size());
                }
            }
        }
        if (ok) {
            pass("G8 buildDtvccCaptionPacket builds aligned packet with service block");
        } else {
            fail("G8 buildDtvccCaptionPacket", detail);
        }
    }

    // -----------------------------------------------------------------------
    // G9: exportScc が toScc と整合、複数 cue が列挙される。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        BroadcastCaptionDoc doc;
        doc.frameRate = 29.97;
        doc.cues.append(CaptionCue{ 1.0, 3.0, QStringLiteral("FIRST"), 15, 0 });
        doc.cues.append(CaptionCue{ 4.0, 6.0, QStringLiteral("SECOND"), 14, 0 });
        const QString viaExport = exportScc(doc);
        const QString viaToScc = toScc(doc.cues, doc.frameRate);
        if (viaExport != viaToScc) {
            ok = false;
            detail = QStringLiteral("exportScc != toScc(doc.cues, frameRate)");
        }
        // 2 つの cue の timecode が両方含まれる
        if (ok) {
            const QString tc1 = timecodeFromSec(1.0, 29.97);
            const QString tc2 = timecodeFromSec(4.0, 29.97);
            if (!viaExport.contains(tc1) || !viaExport.contains(tc2)) {
                ok = false;
                detail = QStringLiteral("missing cue timecode tc1='%1' tc2='%2'").arg(tc1, tc2);
            }
        }
        if (ok) {
            pass("G9 exportScc matches toScc and enumerates multiple cues");
        } else {
            fail("G9 exportScc", detail);
        }
    }

    // -----------------------------------------------------------------------
    // G10: toJson / fromJson round-trip (standard/channel/frameRate/cues)。
    // -----------------------------------------------------------------------
    {
        bool ok = true;
        QString detail;
        BroadcastCaptionDoc doc;
        doc.standard = QStringLiteral("CEA-708");
        doc.channel = 3;
        doc.frameRate = 59.94;
        doc.cues.append(CaptionCue{ 2.5, 5.0, QStringLiteral("ROUND TRIP"), 12, 8 });
        doc.cues.append(CaptionCue{ 7.0, 9.0, QStringLiteral("SECOND CUE"), 10, 0 });

        const QJsonObject json = toJson(doc);
        const BroadcastCaptionDoc back = fromJson(json);

        if (back.standard != doc.standard) {
            ok = false;
            detail = QStringLiteral("standard '%1' != '%2'").arg(back.standard, doc.standard);
        }
        if (ok && back.channel != doc.channel) {
            ok = false;
            detail = QStringLiteral("channel %1 != %2").arg(back.channel).arg(doc.channel);
        }
        if (ok && qAbs(back.frameRate - doc.frameRate) > 1e-6) {
            ok = false;
            detail = QStringLiteral("frameRate %1 != %2").arg(back.frameRate).arg(doc.frameRate);
        }
        if (ok && back.cues.size() != doc.cues.size()) {
            ok = false;
            detail = QStringLiteral("cues count %1 != %2").arg(back.cues.size()).arg(doc.cues.size());
        }
        if (ok && !back.cues.isEmpty()) {
            const CaptionCue& c0 = back.cues.at(0);
            if (c0.text != QStringLiteral("ROUND TRIP")
                || qAbs(c0.startSec - 2.5) > 1e-6
                || qAbs(c0.endSec - 5.0) > 1e-6
                || c0.row != 12
                || c0.col != 8) {
                ok = false;
                detail = QStringLiteral("cue[0] mismatch text='%1' start=%2 end=%3 row=%4 col=%5")
                             .arg(c0.text).arg(c0.startSec).arg(c0.endSec).arg(c0.row).arg(c0.col);
            }
        }
        if (ok) {
            pass("G10 toJson/fromJson round-trips all fields");
        } else {
            fail("G10 toJson/fromJson", detail);
        }
    }

    qInfo().noquote().nospace() << "[broadcast-cc] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
