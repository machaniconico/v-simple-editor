#include "SelftestGate.h"
#include "../AffinityPsdImporter.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QTemporaryDir>

#include <cstdint>
#include <string>

#include <windows.h>
#include <psapi.h>

// US-2002: the PSD importer must survive crafted layer / luni inputs. Every
// hostile file carries one hostile layer plus one well-formed "good" layer so a
// gate can prove the loader both returned in time and kept reading.
namespace {

using affinity::psd::PsdDocument;
using affinity::psd::PsdLayer;

constexpr qint64 kLimitMs = 5000;

void put16(QByteArray &b, quint16 v)
{
    b.append(char(v >> 8));
    b.append(char(v & 0xff));
}

void put32(QByteArray &b, quint32 v)
{
    put16(b, quint16(v >> 16));
    put16(b, quint16(v & 0xffff));
}

struct Channel {
    qint16 id = 0;
    QByteArray data; // u16 compression code + payload
};

struct LayerSpec {
    QString name;
    qint32 top = 0, left = 0, bottom = 0, right = 0;
    quint8 flags = 0;
    bool hasLuni = false;
    quint32 luniCount = 0;
    int luniChars = 0; // UTF-16 code units actually written
    QString luniText;  // when non-empty, written instead of luniChars 'X's
    QList<Channel> channels;
};

QByteArray rawChannelData(int bytes, int base)
{
    QByteArray d;
    put16(d, 0);
    for (int i = 0; i < bytes; ++i) d.append(char((base + i) & 0xff));
    return d;
}

QByteArray rleChannelData(int junkBytes)
{
    QByteArray d;
    put16(d, 1);
    d.append(QByteArray(junkBytes, '\0'));
    return d;
}

QList<Channel> rgbRaw(int bytes, int r, int g, int b)
{
    return {{0, rawChannelData(bytes, r)}, {1, rawChannelData(bytes, g)}, {2, rawChannelData(bytes, b)}};
}

QList<Channel> rgbRle(int junkBytes)
{
    return {{0, rleChannelData(junkBytes)}, {1, rleChannelData(junkBytes)}, {2, rleChannelData(junkBytes)}};
}

LayerSpec goodLayer()
{
    LayerSpec l;
    l.name = QStringLiteral("good");
    l.top = 2; l.left = 1; l.bottom = 6; l.right = 5;
    l.channels = rgbRaw(16, 200, 100, 50);
    return l;
}

QByteArray layerRecord(const LayerSpec &l)
{
    QByteArray r;
    put32(r, quint32(l.top)); put32(r, quint32(l.left));
    put32(r, quint32(l.bottom)); put32(r, quint32(l.right));
    put16(r, quint16(l.channels.size()));
    for (const Channel &c : l.channels) {
        put16(r, quint16(c.id));
        put32(r, quint32(c.data.size()));
    }
    r.append("8BIM");
    r.append("norm");
    r.append(char(255)); // opacity
    r.append(char(0));   // clipping
    r.append(char(l.flags));
    r.append(char(0));   // filler

    QByteArray extra;
    put32(extra, 0); // mask data
    put32(extra, 0); // blending ranges
    const QByteArray nm = l.name.toLatin1();
    extra.append(char(nm.size()));
    extra.append(nm);
    while (extra.size() % 4 != 0) extra.append('\0'); // pad (len byte included) to 4
    if (l.hasLuni) {
        extra.append("8BIM");
        extra.append("luni");
        QByteArray body;
        put32(body, l.luniCount);
        if (!l.luniText.isEmpty())
            for (QChar c : l.luniText) put16(body, c.unicode());
        else
            for (int i = 0; i < l.luniChars; ++i) put16(body, quint16(u'X'));
        if (body.size() % 2) body.append('\0');
        put32(extra, quint32(body.size()));
        extra.append(body);
    }
    put32(r, quint32(extra.size()));
    r.append(extra);
    return r;
}

// Big-endian PSD: header, empty colour-mode / image-resource sections, one
// layer-and-mask section holding the layer records then the channel data.
QByteArray buildPsd(const QList<LayerSpec> &layers)
{
    QByteArray info;
    put16(info, quint16(layers.size()));
    for (const LayerSpec &l : layers) info.append(layerRecord(l));
    for (const LayerSpec &l : layers)
        for (const Channel &c : l.channels) info.append(c.data);
    if (info.size() % 2) info.append('\0');

    QByteArray f;
    f.append("8BPS");
    put16(f, 1);
    f.append(QByteArray(6, '\0'));
    put16(f, 3);   // channels
    put32(f, 8);   // height
    put32(f, 8);   // width
    put16(f, 8);   // depth
    put16(f, 3);   // RGB
    put32(f, 0);   // colour mode data
    put32(f, 0);   // image resources
    put32(f, quint32(info.size() + 4)); // layer & mask section
    put32(f, quint32(info.size()));     // layer info
    f.append(info);
    return f;
}

struct Loaded {
    PsdDocument doc;
    qint64 ms = 0;
};

Loaded load(const QTemporaryDir &dir, const char *file, const QList<LayerSpec> &layers)
{
    const QString path = dir.filePath(QString::fromLatin1(file));
    QFile f(path);
    Loaded out;
    if (!f.open(QIODevice::WriteOnly)) return out;
    f.write(buildPsd(layers));
    f.close();
    QElapsedTimer t;
    t.start();
    out.doc = affinity::psd::loadPsd(path);
    out.ms = t.elapsed();
    return out;
}

const PsdLayer *byName(const PsdDocument &d, const QString &name)
{
    for (const PsdLayer &l : d.layers)
        if (l.name == name) return &l;
    return nullptr;
}

// The hostile layer is the one that is not "good".
const PsdLayer *hostile(const PsdDocument &d)
{
    for (const PsdLayer &l : d.layers)
        if (l.name != QLatin1String("good")) return &l;
    return nullptr;
}

bool goodReadable(const PsdDocument &d)
{
    const PsdLayer *g = byName(d, QStringLiteral("good"));
    return g && !g->image.isNull() && g->image.size() == QSize(4, 4)
        && qRed(g->image.pixel(0, 0)) == 200;
}

// Hostile layer is absent, or keeps no image (geometry cases) / no name (luni
// cases, where the pixels are legitimately readable).
bool hostileHarmless(const PsdDocument &d, bool luniCase)
{
    const PsdLayer *h = hostile(d);
    if (!h) return true;
    return luniCase ? h->name.isEmpty() : h->image.isNull();
}

LayerSpec hostileBase(const QString &name = QString())
{
    LayerSpec l;
    l.name = name;
    l.top = 0; l.left = 0; l.bottom = 4; l.right = 4;
    l.channels = rgbRaw(16, 1, 2, 3);
    return l;
}

} // namespace

int runPsdImportSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir dir;
    if (!dir.isValid()) {
        tally.gate(1, false, "temporary directory unavailable");
        return tally.finish("psd-import");
    }

    // G1 control: well-formed two-layer file reads back exactly.
    {
        LayerSpec a = goodLayer();
        LayerSpec b = goodLayer();
        b.name = QStringLiteral("other");
        b.flags = 0x02; // hidden
        b.top = 0; b.left = 0; b.bottom = 4; b.right = 4;
        b.channels = rgbRaw(16, 10, 20, 30);
        const Loaded r = load(dir, "g1.psd", {a, b});
        const PsdLayer *ga = byName(r.doc, QStringLiteral("good"));
        const PsdLayer *gb = byName(r.doc, QStringLiteral("other"));
        const bool ok = r.doc.layers.size() == 2 && ga && gb
            && !ga->image.isNull() && ga->image.size() == QSize(4, 4)
            && ga->image.pixel(0, 0) == qRgba(200, 100, 50, 255)
            && ga->image.pixel(3, 3) == qRgba(215, 115, 65, 255)
            && ga->visibility && ga->boundsRect == QRect(1, 2, 4, 4)
            && !gb->image.isNull() && gb->image.size() == QSize(4, 4)
            && gb->image.pixel(0, 0) == qRgba(10, 20, 30, 255)
            && !gb->visibility && gb->boundsRect == QRect(0, 0, 4, 4);
        auto sz = [](const PsdLayer *l) {
            return l ? std::to_string(l->image.width()) + "x" + std::to_string(l->image.height()) : std::string("absent");
        };
        auto rect = [](const PsdLayer *l) {
            return l ? std::to_string(l->boundsRect.x()) + "," + std::to_string(l->boundsRect.y()) + " "
                         + std::to_string(l->boundsRect.width()) + "x" + std::to_string(l->boundsRect.height())
                     : std::string("absent");
        };
        tally.gate(1, ok, "two-layer control: layers=" + std::to_string(r.doc.layers.size())
                              + " ga=" + std::to_string(ga != nullptr) + " gb=" + std::to_string(gb != nullptr)
                              + " ga.img=" + sz(ga) + " gb.img=" + sz(gb)
                              + " ga(0,0)=" + (ga ? QString::number(ga->image.pixel(0, 0), 16).toStdString() : std::string("n/a"))
                              + " ga(3,3)=" + (ga ? QString::number(ga->image.pixel(3, 3), 16).toStdString() : std::string("n/a"))
                              + " ga.vis=" + (ga ? std::to_string(ga->visibility) : std::string("n/a"))
                              + " gb.vis=" + (gb ? std::to_string(gb->visibility) : std::string("n/a"))
                              + " ga.rect=" + rect(ga) + " gb.rect=" + rect(gb));
    }

    // G2: luni count 0x80000000 with two characters of payload.
    {
        LayerSpec h = hostileBase();
        h.hasLuni = true; h.luniCount = 0x80000000u; h.luniChars = 2;
        const Loaded r = load(dir, "g2.psd", {h, goodLayer()});
        const bool hostileOk = r.ms < kLimitMs && goodReadable(r.doc) && hostileHarmless(r.doc, true);

        // luni boundary: block length is exactly 4 + 2 * payload chars.
        qint64 msa = 0, msb = 0, msb2 = 0, msc = 0;
        auto luniName = [&](const char *file, quint32 count, const QString &text, bool &readable, qint64 &ms) {
            LayerSpec s = hostileBase();
            s.hasLuni = true; s.luniCount = count; s.luniText = text;
            const Loaded x = load(dir, file, {s, goodLayer()});
            ms = x.ms;
            readable = x.ms < kLimitMs && goodReadable(x.doc);
            const PsdLayer *hl = hostile(x.doc);
            return hl ? hl->name : QStringLiteral("<absent>");
        };
        bool ra = false, rb = false, rb2 = false, rc = false;
        const QString na = luniName("g2a.psd", 0, QString(), ra, msa);
        const QString nb = luniName("g2b.psd", 2, QStringLiteral("AB"), rb, msb);
        const QString nb2 = luniName("g2b2.psd", 1, QString(QChar(0x3042)), rb2, msb2);
        const QString nc = luniName("g2c.psd", 3, QStringLiteral("AB"), rc, msc); // one char short
        const bool ok = hostileOk
            && ra && na.isEmpty()
            && rb && nb == QLatin1String("AB")
            && rb2 && nb2 == QString(QChar(0x3042))
            && rc && nc.isEmpty();
        auto desc = [](const QString &n) {
            return "len=" + std::to_string(n.size()) + " '" + n.toStdString() + "'";
        };
        tally.gate(2, ok,
                   "luni 2^31 ms=" + std::to_string(r.ms) + " hostileOk=" + std::to_string(hostileOk)
                   + "; count0 ok=" + std::to_string(ra) + " ms=" + std::to_string(msa) + " " + desc(na)
                   + "; n2 ok=" + std::to_string(rb) + " ms=" + std::to_string(msb) + " " + desc(nb)
                   + "; U+3042 ok=" + std::to_string(rb2) + " ms=" + std::to_string(msb2)
                   + " len=" + std::to_string(nb2.size()) + " u=" + std::to_string(nb2.isEmpty() ? 0 : nb2[0].unicode())
                   + "; short(count3,2 chars) ok=" + std::to_string(rc) + " ms=" + std::to_string(msc) + " " + desc(nc));
    }

    // Shared detail formatters for the hostile-layer gates (G3-G5).
    auto nm = [](const Loaded &x) {
        const PsdLayer *h = hostile(x.doc);
        return h ? "'" + h->name.toStdString() + "'" : std::string("<absent>");
    };
    auto img = [](const Loaded &x) {
        const PsdLayer *h = hostile(x.doc);
        return h && !h->image.isNull()
            ? std::to_string(h->image.width()) + "x" + std::to_string(h->image.height())
            : std::string("null");
    };
    auto d = [&](const Loaded &x, bool luniCase) {
        return "ms=" + std::to_string(x.ms) + " good=" + std::to_string(goodReadable(x.doc))
            + " harmless=" + std::to_string(hostileHarmless(x.doc, luniCase))
            + " name=" + nm(x) + " img=" + img(x);
    };

    // G3: RLE layer of width 1 / height 2^30.
    {
        LayerSpec h = hostileBase();
        h.bottom = 1 << 30; h.right = 1;
        h.channels = rgbRle(8);
        const Loaded r = load(dir, "g3.psd", {h, goodLayer()});
        tally.gate(3, r.ms < kLimitMs && goodReadable(r.doc) && hostileHarmless(r.doc, false),
                   "rle 1x2^30 " + d(r, false) + " limit=" + std::to_string(kLimitMs));
    }

    // G4: 2^24 x 2^24 layer.
    {
        LayerSpec h = hostileBase();
        h.bottom = 1 << 24; h.right = 1 << 24;
        h.channels = rgbRle(8);
        const Loaded r = load(dir, "g4.psd", {h, goodLayer()});
        tally.gate(4, r.ms < kLimitMs && goodReadable(r.doc) && hostileHarmless(r.doc, false),
                   "2^24 square " + d(r, false) + " limit=" + std::to_string(kLimitMs));
    }

    // G5: luni 0x40000000, reversed bounds, and a qint32-overflowing span.
    {
        LayerSpec luni = hostileBase();
        luni.hasLuni = true; luni.luniCount = 0x40000000u; luni.luniChars = 2;
        LayerSpec neg = hostileBase();
        neg.top = 10; neg.bottom = 0;
        LayerSpec wide = hostileBase();
        wide.left = INT32_MIN; wide.right = INT32_MAX;
        wide.channels = rgbRle(8);
        const Loaded a = load(dir, "g5a.psd", {luni, goodLayer()});
        const Loaded b = load(dir, "g5b.psd", {neg, goodLayer()});
        const Loaded c = load(dir, "g5c.psd", {wide, goodLayer()});
        const bool ok = a.ms < kLimitMs && goodReadable(a.doc) && hostileHarmless(a.doc, true)
            && b.ms < kLimitMs && goodReadable(b.doc) && hostileHarmless(b.doc, false)
            && c.ms < kLimitMs && goodReadable(c.doc) && hostileHarmless(c.doc, false);
        tally.gate(5, ok, "g5a(luni 2^30) " + d(a, true) + "; g5b(negative height) " + d(b, false)
                              + "; g5c(span overflow) " + d(c, false));
    }

    // G6: 30000 x 1 reads, 30001 x 1 is skipped.
    {
        LayerSpec in = hostileBase(QStringLiteral("edge"));
        in.right = 30000; in.bottom = 1;
        in.channels = rgbRaw(30000, 7, 8, 9);
        LayerSpec over = hostileBase(QStringLiteral("over"));
        over.right = 30001; over.bottom = 1;
        over.channels = rgbRaw(30001, 7, 8, 9);
        const Loaded r = load(dir, "g6.psd", {in, over, goodLayer()});
        const PsdLayer *e = byName(r.doc, QStringLiteral("edge"));
        const PsdLayer *o = byName(r.doc, QStringLiteral("over"));
        const bool ok = r.ms < kLimitMs && goodReadable(r.doc)
            && e && !e->image.isNull() && e->image.size() == QSize(30000, 1)
            && o && o->image.isNull();
        auto wh = [](const PsdLayer *l) {
            return l && !l->image.isNull()
                ? std::to_string(l->image.width()) + "x" + std::to_string(l->image.height())
                : std::string("null");
        };
        tally.gate(6, ok, "30000x1 / 30001x1 ms=" + std::to_string(r.ms)
                              + " good=" + std::to_string(goodReadable(r.doc))
                              + " edge.present=" + std::to_string(e != nullptr) + " edge.img=" + wh(e)
                              + " over.present=" + std::to_string(o != nullptr) + " over.img=" + wh(o));
    }

    // G7: forged aLen / extraLen / lmiLen (0x40000000) on the second layer's
    // luni with the file cut right after the count. The block ends must be
    // clamped to the real file, so the count never drives a big allocation.
    {
        auto forged = [&](const char *file, quint32 count, qint64 &fileSize) {
            LayerSpec l1 = goodLayer();
            l1.name = QStringLiteral("Layer1");
            LayerSpec l2 = hostileBase(QStringLiteral("Layer2"));
            l2.hasLuni = true; l2.luniCount = count; l2.luniChars = 0;
            QByteArray rec2 = layerRecord(l2);
            const qint32 extraPos = qint32(rec2.size()) - 4 - 32; // extra = 32 B for this layer
            const qint32 aLenPos = qint32(rec2.size()) - 8;
            auto patch32 = [](QByteArray &b, qint32 at, quint32 v) {
                for (int i = 0; i < 4; ++i) b[at + i] = char(v >> (24 - 8 * i));
            };
            const bool shapeOk = extraPos >= 0 && quint8(rec2[extraPos + 3]) == 32;
            patch32(rec2, extraPos, 0x40000000u);
            patch32(rec2, aLenPos, 0x40000000u);
            QByteArray f;
            f.append("8BPS");
            put16(f, 1);
            f.append(QByteArray(6, '\0'));
            put16(f, 3); put32(f, 8); put32(f, 8); put16(f, 8); put16(f, 3);
            put32(f, 0); put32(f, 0);
            put32(f, 0x40000000u); // lmiLen
            put32(f, 0x3fffffffu); // layer info length
            put16(f, 2);
            f.append(layerRecord(l1));
            f.append(rec2);
            const QString path = dir.filePath(QString::fromLatin1(file));
            QFile out(path);
            Loaded r;
            if (!shapeOk || !out.open(QIODevice::WriteOnly)) return std::make_pair(r, false);
            if (out.write(f) != f.size()) { out.close(); return std::make_pair(r, false); }
            out.close();
            fileSize = QFileInfo(path).size();
            QElapsedTimer t;
            t.start();
            r.doc = affinity::psd::loadPsd(path);
            r.ms = t.elapsed();
            return std::make_pair(r, true);
        };
        auto namesOk = [](const PsdDocument &doc) {
            return doc.layers.size() == 2 && doc.layers[0].name == QLatin1String("Layer2")
                && doc.layers[1].name == QLatin1String("Layer1");
        };
        auto workingSetMB = []() -> quint64 {
            PROCESS_MEMORY_COUNTERS pmc{};
            if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
                return 0;
            return quint64(pmc.WorkingSetSize) / (1024 * 1024);
        };
        qint64 sizeI = 0, sizeII = 0;
        const quint64 wsBefore = workingSetMB();
        const auto i = forged("g7i.psd", 0x1fffffffu, sizeI);
        const quint64 wsAfter = workingSetMB();
        const qint64 allocI = affinity::psd::psdtest::lastLuniAllocatedBytes();
        const auto ii = forged("g7ii.psd", 60000u, sizeII);
        const qint64 allocII = affinity::psd::psdtest::lastLuniAllocatedBytes();
        const bool ok = i.second && ii.second
            && i.first.ms < 2000 && namesOk(i.first.doc) && allocI <= 131072
            && ii.first.ms < 2000 && namesOk(ii.first.doc) && allocII <= sizeII;
        tally.gate(7, ok, "i(count 0x1fffffff) shapeOk=" + std::to_string(i.second)
                              + " ms=" + std::to_string(i.first.ms) + " layers=" + std::to_string(i.first.doc.layers.size())
                              + " names=" + std::to_string(namesOk(i.first.doc)) + " alloc=" + std::to_string(allocI)
                              + " size=" + std::to_string(sizeI) + " cap=131072"
                              + " ws=" + std::to_string(wsBefore) + "->" + std::to_string(wsAfter) + "MB"
                              + "; ii(count 60000) shapeOk=" + std::to_string(ii.second)
                              + " ms=" + std::to_string(ii.first.ms) + " layers=" + std::to_string(ii.first.doc.layers.size())
                              + " names=" + std::to_string(namesOk(ii.first.doc)) + " alloc=" + std::to_string(allocII)
                              + " size=" + std::to_string(sizeII));
    }

    return tally.finish("psd-import");
}
