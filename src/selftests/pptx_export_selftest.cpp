// PptxExport headless selftest.
//
// QApplication 不要・QtCore のみ。PptxExport::buildPptx() が返す store ZIP を
// 「自前のミニ ZIP パーサ」と「自前の CRC32」で独立に検証する (independent comparator)。
// PptxExport の内部関数には一切依存しない。
//
// ZIP パーサ規約:
//   - EOCD(0x06054b50) を末尾から探し、Central Directory を辿る。
//   - store(method=0) のみ対応 (PptxExport は無圧縮 store のみ生成する)。
//   - 各エントリの { ファイル名 -> (圧縮データ=格納データ, crc32, 格納サイズ) } を作る。

#include <QByteArray>
#include <QByteArrayList>
#include <QDebug>
#include <QString>
#include <QVector>
#include <QXmlStreamReader>

#include "../PptxExport.h"

namespace {

// ---- リトルエンディアン読み出しヘルパ -----------------------------------
quint16 readU16(const QByteArray& buf, int off)
{
    if (off < 0 || off + 2 > buf.size()) {
        return 0;
    }
    const quint8 b0 = static_cast<quint8>(buf.at(off));
    const quint8 b1 = static_cast<quint8>(buf.at(off + 1));
    return static_cast<quint16>(b0 | (b1 << 8));
}

quint32 readU32(const QByteArray& buf, int off)
{
    if (off < 0 || off + 4 > buf.size()) {
        return 0;
    }
    const quint8 b0 = static_cast<quint8>(buf.at(off));
    const quint8 b1 = static_cast<quint8>(buf.at(off + 1));
    const quint8 b2 = static_cast<quint8>(buf.at(off + 2));
    const quint8 b3 = static_cast<quint8>(buf.at(off + 3));
    return static_cast<quint32>(b0)
         | (static_cast<quint32>(b1) << 8)
         | (static_cast<quint32>(b2) << 16)
         | (static_cast<quint32>(b3) << 24);
}

// ---- 独立 CRC32 (標準多項式 0xEDB88320 のテーブル方式) ------------------
quint32 crc32Compute(const QByteArray& data)
{
    static quint32 table[256];
    static bool initialized = false;
    if (!initialized) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    for (int i = 0; i < data.size(); ++i) {
        const quint8 byte = static_cast<quint8>(data.at(i));
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---- ミニ ZIP パーサ -----------------------------------------------------
struct ZipEntry {
    QByteArray data;        // 格納データ (store なので非圧縮そのまま)。
    quint32    crc32 = 0;   // Central Directory に記録された crc32。
    quint32    storedSize = 0; // 圧縮(=非圧縮)サイズ。
    quint16    method = 0;  // 圧縮方式 (0=store)。
};

struct ZipArchive {
    QVector<QString>    order;    // 出現順 (Central Directory 順) のファイル名。
    QVector<ZipEntry>   entries;  // order と同インデックス。

    int indexOf(const QString& name) const {
        for (int i = 0; i < order.size(); ++i) {
            if (order.at(i) == name) {
                return i;
            }
        }
        return -1;
    }
    bool has(const QString& name) const { return indexOf(name) >= 0; }
    QByteArray dataOf(const QString& name) const {
        const int i = indexOf(name);
        return i >= 0 ? entries.at(i).data : QByteArray();
    }
};

// 末尾から EOCD(0x06054b50) を探す。
int findEocd(const QByteArray& buf)
{
    // EOCD は最低 22 バイト。コメント無し前提だが末尾から走査。
    const int minOff = qMax(0, buf.size() - (22 + 65535));
    for (int off = buf.size() - 22; off >= minOff; --off) {
        if (readU32(buf, off) == 0x06054b50u) {
            return off;
        }
    }
    return -1;
}

// store ZIP をパースして archive を返す。失敗時は ok=false。
bool parseZip(const QByteArray& buf, ZipArchive& out, QString& err)
{
    const int eocd = findEocd(buf);
    if (eocd < 0) {
        err = QStringLiteral("EOCD signature not found");
        return false;
    }
    const quint16 totalEntries = readU16(buf, eocd + 10);
    const quint32 cdOffset     = readU32(buf, eocd + 16);

    int p = static_cast<int>(cdOffset);
    for (quint16 e = 0; e < totalEntries; ++e) {
        if (readU32(buf, p) != 0x02014b50u) {
            err = QStringLiteral("Central Directory header signature mismatch at entry %1").arg(e);
            return false;
        }
        const quint16 method     = readU16(buf, p + 10);
        const quint32 crc        = readU32(buf, p + 16);
        const quint32 compSize   = readU32(buf, p + 20);
        const quint16 nameLen    = readU16(buf, p + 28);
        const quint16 extraLen   = readU16(buf, p + 30);
        const quint16 commentLen = readU16(buf, p + 32);
        const quint32 lhOffset   = readU32(buf, p + 42);
        const QString name = QString::fromUtf8(buf.mid(p + 46, nameLen));

        // Local File Header から実データオフセットを求める。
        if (readU32(buf, static_cast<int>(lhOffset)) != 0x04034b50u) {
            err = QStringLiteral("Local File Header signature mismatch for '%1'").arg(name);
            return false;
        }
        const quint16 lhNameLen  = readU16(buf, static_cast<int>(lhOffset) + 26);
        const quint16 lhExtraLen = readU16(buf, static_cast<int>(lhOffset) + 28);
        const int dataOff = static_cast<int>(lhOffset) + 30 + lhNameLen + lhExtraLen;

        if (method != 0) {
            err = QStringLiteral("entry '%1' uses non-store method %2").arg(name).arg(method);
            return false;
        }
        if (dataOff < 0 || dataOff + static_cast<int>(compSize) > buf.size()) {
            err = QStringLiteral("entry '%1' data out of bounds").arg(name);
            return false;
        }

        ZipEntry entry;
        entry.method     = method;
        entry.crc32      = crc;
        entry.storedSize = compSize;
        entry.data       = buf.mid(dataOff, static_cast<int>(compSize));
        out.order.append(name);
        out.entries.append(entry);

        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

} // namespace

int runPptxExportSelftest()
{
    qInfo().noquote() << "[pptx-export] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[pptx-export] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[pptx-export] FAIL" << name << ":" << msg; };

    // 共通デッキ: 3 スライド + title/bullets。
    pptxexport::Deck deck;
    deck.title  = QStringLiteral("My Deck");
    deck.author = QStringLiteral("Selftest Author");
    {
        pptxexport::Slide s0;
        s0.title   = QStringLiteral("Intro Slide");
        s0.bullets = { QStringLiteral("First bullet point"),
                       QStringLiteral("Second bullet point") };
        deck.slides.append(s0);

        pptxexport::Slide s1;
        s1.title   = QStringLiteral("Agenda Overview");
        s1.bullets = { QStringLiteral("Topic Alpha"),
                       QStringLiteral("Topic Beta"),
                       QStringLiteral("Topic Gamma") };
        deck.slides.append(s1);

        pptxexport::Slide s2;
        s2.title   = QStringLiteral("Conclusion Notes");
        s2.bullets = { QStringLiteral("Summary line one") };
        deck.slides.append(s2);
    }

    const QByteArray pptx = pptxexport::buildPptx(deck);

    // 共通パース (G2 以降で使う)。
    ZipArchive archive;
    QString parseErr;
    const bool parsed = parseZip(pptx, archive, parseErr);

    // G1: 非空 + 先頭4バイト "PK\x03\x04"
    {
        const bool magicOk = pptx.size() >= 4
                          && static_cast<quint8>(pptx.at(0)) == 0x50
                          && static_cast<quint8>(pptx.at(1)) == 0x4B
                          && static_cast<quint8>(pptx.at(2)) == 0x03
                          && static_cast<quint8>(pptx.at(3)) == 0x04;
        if (!pptx.isEmpty() && magicOk) {
            pass("G1 buildPptx non-empty and starts with PK\\x03\\x04");
        } else {
            fail("G1 magic", QStringLiteral("size=%1 magicOk=%2").arg(pptx.size()).arg(magicOk));
        }
    }

    // G2: 必須パート存在 + Content_Types が先頭エントリ
    {
        bool ok = parsed
               && archive.has(QStringLiteral("[Content_Types].xml"))
               && archive.has(QStringLiteral("ppt/presentation.xml"))
               && archive.has(QStringLiteral("ppt/slides/slide1.xml"));
        const bool firstOk = parsed && !archive.order.isEmpty()
                          && archive.order.first() == QStringLiteral("[Content_Types].xml");
        if (ok && firstOk) {
            pass("G2 required parts present, [Content_Types].xml is first entry");
        } else {
            fail("G2 parts", QStringLiteral("parsed=%1 err='%2' firstOk=%3 first='%4'")
                    .arg(parsed).arg(parseErr).arg(firstOk)
                    .arg(archive.order.isEmpty() ? QString() : archive.order.first()));
        }
    }

    // G3: slideK.xml が K=1..N まで全て存在し余分が無い
    {
        const int n = deck.slides.size();
        bool allPresent = parsed;
        for (int k = 1; allPresent && k <= n; ++k) {
            allPresent = archive.has(QStringLiteral("ppt/slides/slide%1.xml").arg(k));
        }
        // 余分な slide(N+1) が無い
        const bool noExtra = parsed
                          && !archive.has(QStringLiteral("ppt/slides/slide%1.xml").arg(n + 1));
        // 実 slide エントリ数 == N
        int slideCount = 0;
        if (parsed) {
            for (const QString& name : archive.order) {
                if (name.startsWith(QStringLiteral("ppt/slides/slide"))
                    && name.endsWith(QStringLiteral(".xml"))
                    && !name.contains(QStringLiteral("_rels"))) {
                    ++slideCount;
                }
            }
        }
        if (allPresent && noExtra && slideCount == n) {
            pass("G3 slide1..N present, no extra slide, count matches deck.slides.size()");
        } else {
            fail("G3 slide count", QStringLiteral("allPresent=%1 noExtra=%2 slideCount=%3 expected=%4")
                    .arg(allPresent).arg(noExtra).arg(slideCount).arg(n));
        }
    }

    // G4: 各 slide XML に対応 title が含まれる
    {
        bool ok = parsed;
        QString badName;
        for (int i = 0; ok && i < deck.slides.size(); ++i) {
            const QString xml = QString::fromUtf8(
                archive.dataOf(QStringLiteral("ppt/slides/slide%1.xml").arg(i + 1)));
            if (!xml.contains(deck.slides.at(i).title)) {
                ok = false;
                badName = deck.slides.at(i).title;
            }
        }
        if (ok) {
            pass("G4 each slide XML contains its title");
        } else {
            fail("G4 title", QStringLiteral("missing title in slide: '%1'").arg(badName));
        }
    }

    // G5: 各 bullet 行が対応 slide XML に含まれる
    {
        bool ok = parsed;
        QString badBullet;
        for (int i = 0; ok && i < deck.slides.size(); ++i) {
            const QString xml = QString::fromUtf8(
                archive.dataOf(QStringLiteral("ppt/slides/slide%1.xml").arg(i + 1)));
            for (const QString& b : deck.slides.at(i).bullets) {
                if (!xml.contains(b)) {
                    ok = false;
                    badBullet = b;
                    break;
                }
            }
        }
        if (ok) {
            pass("G5 each bullet line present in its slide XML");
        } else {
            fail("G5 bullets", QStringLiteral("missing bullet: '%1'").arg(badBullet));
        }
    }

    // G6: 全 XML パートが well-formed (QXmlStreamReader で読み切り hasError()==false)
    {
        QStringList xmlParts = {
            QStringLiteral("[Content_Types].xml"),
            QStringLiteral("ppt/presentation.xml")
        };
        for (int k = 1; k <= deck.slides.size(); ++k) {
            xmlParts << QStringLiteral("ppt/slides/slide%1.xml").arg(k);
        }
        bool ok = parsed;
        QString badPart;
        for (const QString& part : xmlParts) {
            const QByteArray bytes = archive.dataOf(part);
            QXmlStreamReader reader(bytes);
            while (!reader.atEnd()) {
                reader.readNext();
            }
            if (reader.hasError()) {
                ok = false;
                badPart = QStringLiteral("%1: %2").arg(part, reader.errorString());
                break;
            }
        }
        if (ok) {
            pass("G6 all XML parts are well-formed (no parse error)");
        } else {
            fail("G6 well-formed", badPart);
        }
    }

    // G7: 特殊文字エスケープ
    {
        pptxexport::Deck esc;
        esc.title = QStringLiteral("Escaped Deck");
        pptxexport::Slide s;
        s.title = QStringLiteral("A & B <tag> \"q\"");
        esc.slides.append(s);

        const QByteArray escPptx = pptxexport::buildPptx(esc);
        ZipArchive escArchive;
        QString escErr;
        const bool escParsed = parseZip(escPptx, escArchive, escErr);
        const QString xml = escParsed
            ? QString::fromUtf8(escArchive.dataOf(QStringLiteral("ppt/slides/slide1.xml")))
            : QString();

        // 生の '<tag>' が含まれない (エスケープ済) かつ &lt;tag&gt; / &amp; が含まれる。
        const bool noRaw   = escParsed && !xml.contains(QStringLiteral("<tag>"));
        const bool hasLt   = escParsed && xml.contains(QStringLiteral("&lt;tag&gt;"));
        const bool hasAmp  = escParsed && xml.contains(QStringLiteral("&amp;"));
        if (noRaw && hasLt && hasAmp) {
            pass("G7 special chars escaped (no raw <tag>, has &lt;tag&gt; and &amp;)");
        } else {
            fail("G7 escape", QStringLiteral("parsed=%1 noRaw=%2 hasLt=%3 hasAmp=%4")
                    .arg(escParsed).arg(noRaw).arg(hasLt).arg(hasAmp));
        }
    }

    // G8: 空 deck でも G1/G6 を満たす
    {
        pptxexport::Deck empty;
        empty.title = QStringLiteral("Empty Deck");
        const QByteArray emptyPptx = pptxexport::buildPptx(empty);

        const bool magicOk = emptyPptx.size() >= 4
                          && static_cast<quint8>(emptyPptx.at(0)) == 0x50
                          && static_cast<quint8>(emptyPptx.at(1)) == 0x4B
                          && static_cast<quint8>(emptyPptx.at(2)) == 0x03
                          && static_cast<quint8>(emptyPptx.at(3)) == 0x04;

        ZipArchive emptyArchive;
        QString emptyErr;
        const bool emptyParsed = parseZip(emptyPptx, emptyArchive, emptyErr);

        // 少なくとも 1 枚の slide が補われ、全 XML が well-formed。
        bool wellFormed = emptyParsed;
        QString badPart;
        if (emptyParsed) {
            QStringList parts = {
                QStringLiteral("[Content_Types].xml"),
                QStringLiteral("ppt/presentation.xml"),
                QStringLiteral("ppt/slides/slide1.xml")
            };
            for (const QString& part : parts) {
                if (!emptyArchive.has(part)) {
                    wellFormed = false;
                    badPart = QStringLiteral("missing %1").arg(part);
                    break;
                }
                QXmlStreamReader reader(emptyArchive.dataOf(part));
                while (!reader.atEnd()) {
                    reader.readNext();
                }
                if (reader.hasError()) {
                    wellFormed = false;
                    badPart = QStringLiteral("%1: %2").arg(part, reader.errorString());
                    break;
                }
            }
        }
        if (!emptyPptx.isEmpty() && magicOk && emptyParsed && wellFormed) {
            pass("G8 empty deck yields valid file (title slide + well-formed parts)");
        } else {
            fail("G8 empty deck", QStringLiteral("magicOk=%1 parsed=%2 wellFormed=%3 detail='%4' err='%5'")
                    .arg(magicOk).arg(emptyParsed).arg(wellFormed).arg(badPart).arg(emptyErr));
        }
    }

    // G9: 各 store エントリの crc32 を再計算し Central Directory の値と一致
    {
        bool ok = parsed;
        QString badName;
        for (int i = 0; ok && i < archive.entries.size(); ++i) {
            const ZipEntry& e = archive.entries.at(i);
            const quint32 recomputed = crc32Compute(e.data);
            const bool sizeOk = (e.storedSize == static_cast<quint32>(e.data.size()));
            if (recomputed != e.crc32 || !sizeOk) {
                ok = false;
                badName = QStringLiteral("%1 (cdCrc=%2 recomputed=%3 storedSize=%4 actual=%5)")
                              .arg(archive.order.at(i))
                              .arg(e.crc32, 0, 16)
                              .arg(recomputed, 0, 16)
                              .arg(e.storedSize)
                              .arg(e.data.size());
            }
        }
        if (ok && !archive.entries.isEmpty()) {
            pass("G9 recomputed crc32 matches Central Directory for every store entry");
        } else {
            fail("G9 crc32", badName.isEmpty() ? QStringLiteral("no entries / not parsed") : badName);
        }
    }

    // G10: presentation.xml の sldId 数 == slide 数 + 各 slideN.xml.rels が slideLayout 参照を持つ
    {
        const QString pres = parsed
            ? QString::fromUtf8(archive.dataOf(QStringLiteral("ppt/presentation.xml")))
            : QString();
        // <p:sldId ...> の出現数を数える。末尾スペース付きで照合し、
        // 包含要素 <p:sldIdLst> (属性なしで '>' が直後) を誤カウントしない。
        // 実 sldId 要素は id/r:id 属性を持つため必ず "<p:sldId " となる。
        int sldIdCount = 0;
        int from = 0;
        const QString needle = QStringLiteral("<p:sldId ");
        while (true) {
            const int idx = pres.indexOf(needle, from);
            if (idx < 0) {
                break;
            }
            ++sldIdCount;
            from = idx + needle.size();
        }
        const bool countOk = parsed && sldIdCount == deck.slides.size();

        // 各 slideN.xml.rels が slideLayout 参照を持つ。
        bool relsOk = parsed;
        QString badRels;
        for (int k = 1; relsOk && k <= deck.slides.size(); ++k) {
            const QString relsPath = QStringLiteral("ppt/slides/_rels/slide%1.xml.rels").arg(k);
            if (!archive.has(relsPath)) {
                relsOk = false;
                badRels = QStringLiteral("missing %1").arg(relsPath);
                break;
            }
            const QString rels = QString::fromUtf8(archive.dataOf(relsPath));
            if (!rels.contains(QStringLiteral("slideLayout"))) {
                relsOk = false;
                badRels = QStringLiteral("%1 has no slideLayout reference").arg(relsPath);
            }
        }
        if (countOk && relsOk) {
            pass("G10 sldId count matches slides; each slide .rels references a slideLayout");
        } else {
            fail("G10 presentation integrity", QStringLiteral("sldIdCount=%1 expected=%2 relsOk=%3 detail='%4'")
                    .arg(sldIdCount).arg(deck.slides.size()).arg(relsOk).arg(badRels));
        }
    }

    qInfo().noquote().nospace() << "[pptx-export] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
