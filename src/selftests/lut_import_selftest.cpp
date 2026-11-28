#include "SelftestGate.h"
#include "../LutImporter.h"

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <cstdlib>
#include <string>

// US-2003: .cube sizes outside 2..256 (3D) / 2..65536 (1D) must load as invalid
// LUTs, and LutData::isValid() needs size >= 2. G1 / G7 are 2-point controls.
// US-2102: 1D top interval, DOMAIN_MIN / DOMAIN_MAX, toGpuGrid (G7 revised, G8-G12).
namespace {

// Reports success through *ok: every write returned the expected byte count and
// the file on disk is header + 1 + rows * 14 bytes. A missing or truncated
// fixture must not turn an "invalid LUT" gate into a vacuous PASS.
QString writeCube(const QTemporaryDir &dir, const char *name, const QByteArray &header,
                  int rows, bool *ok)
{
    *ok = false;
    const QString path = dir.filePath(QString::fromLatin1(name));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    const QByteArray row("0.5 0.25 0.75\n");
    qint64 expected = 0;
    bool wrote = f.write(header) == header.size();
    expected += header.size();
    wrote = wrote && f.write("\n") == 1;
    expected += 1;
    for (int i = 0; wrote && i < rows; ++i) {
        wrote = f.write(row) == row.size();
        expected += row.size();
    }
    f.close();
    *ok = wrote && QFileInfo(path).size() == expected;
    return *ok ? path : QString();
}

QImage pixel(int r, int g, int b)
{
    QImage img(1, 1, QImage::Format_RGB888);
    img.setPixelColor(0, 0, QColor(r, g, b));
    return img;
}

bool near(const QColor &c, int r, int g, int b)
{
    return std::abs(c.red() - r) <= 1 && std::abs(c.green() - g) <= 1
        && std::abs(c.blue() - b) <= 1;
}

// Writes text to dir/name and loads it; *ok reports that the whole file was written.
LutData loadText(const QTemporaryDir &dir, const char *name, const QByteArray &text, bool *ok)
{
    const QString path = dir.filePath(QString::fromLatin1(name));
    QFile f(path);
    *ok = f.open(QIODevice::WriteOnly) && f.write(text) == text.size();
    f.close();
    return *ok ? LutImporter::loadCubeFile(path) : LutData();
}

QByteArray g1Body()
{
    QByteArray body = "LUT_3D_SIZE 2\n";
    for (int i = 0; i < 8; ++i)
        body += QByteArray::number(i * 0.125) + " " + QByteArray::number(i * 0.1)
             + " " + QByteArray::number(1.0 - i * 0.125) + "\n";
    return body;
}

QImage gradient4x4()
{
    QImage img(4, 4, QImage::Format_RGB888);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            img.setPixelColor(x, y, QColor(x * 85, y * 85, (x + y) * 42));
    return img;
}

std::string rgb(const QColor &c)
{
    return std::to_string(c.red()) + "," + std::to_string(c.green()) + "," + std::to_string(c.blue());
}

std::string d(const LutData &l)
{
    return "size=" + std::to_string(l.size) + " rows=" + std::to_string(l.table.size())
        + " valid=" + std::to_string(l.isValid());
}

} // namespace

int runLutImportSelftest()
{
    selftest::GateTally tally;
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "lut-import: temp dir unavailable\n";
        tally.gate(1, false, "temp dir");
        return tally.finish("lut-import");
    }

    // G1: 2-point 3D control. R changes fastest; (0,0,0) -> first row, (1,1,1) -> last.
    {
        QByteArray body = "LUT_3D_SIZE 2\n";
        for (int i = 0; i < 8; ++i)
            body += QByteArray::number(i * 0.125) + " " + QByteArray::number(i * 0.1)
                 + " " + QByteArray::number(1.0 - i * 0.125) + "\n";
        const QString p = dir.filePath(QStringLiteral("g1.cube"));
        QFile f(p);
        bool wrote = f.open(QIODevice::WriteOnly);
        if (wrote) { f.write(body); f.close(); }
        const LutData l = LutImporter::loadCubeFile(p);
        const QColor lo = LutImporter::applyLut(pixel(0, 0, 0), l).pixelColor(0, 0);
        const QColor hi = LutImporter::applyLut(pixel(255, 255, 255), l).pixelColor(0, 0);
        // first row (0, 0, 1) -> (0,0,255); last row (0.875, 0.7, 0.125) -> (223,178,31)
        const bool ok = wrote && l.isValid() && l.size == 2
            && near(lo, 0, 0, 255) && near(hi, 223, 178, 31);
        tally.gate(1, ok, d(l) + " lo=" + std::to_string(lo.red()) + "," + std::to_string(lo.green())
                              + "," + std::to_string(lo.blue()) + " hi=" + std::to_string(hi.red())
                              + "," + std::to_string(hi.green()) + "," + std::to_string(hi.blue()));
    }

    // G2: 3D size 1 with one row.
    {
        bool ok = false;
        const LutData l = LutImporter::loadCubeFile(writeCube(dir, "g2.cube", "LUT_3D_SIZE 1", 1, &ok));
        tally.gate(2, ok && !l.isValid(), ok ? d(l) : "fixture write failed");
    }

    // G3: 1D size 1 with one row.
    {
        bool ok = false;
        const LutData l = LutImporter::loadCubeFile(writeCube(dir, "g3.cube", "LUT_1D_SIZE 1", 1, &ok));
        tally.gate(3, ok && !l.isValid(), ok ? d(l) : "fixture write failed");
    }

    // G4: size 0, negative, non-numeric.
    {
        bool oka = false, okb = false, okc = false;
        const LutData a = LutImporter::loadCubeFile(writeCube(dir, "g4a.cube", "LUT_3D_SIZE 0", 1, &oka));
        const LutData b = LutImporter::loadCubeFile(writeCube(dir, "g4b.cube", "LUT_3D_SIZE -3", 1, &okb));
        const LutData c = LutImporter::loadCubeFile(writeCube(dir, "g4c.cube", "LUT_3D_SIZE abc", 1, &okc));
        const bool ok = oka && okb && okc;
        tally.gate(4, ok && !a.isValid() && !b.isValid() && !c.isValid(),
                   std::string(ok ? "" : "fixture write failed; ") + "0: " + d(a) + " -3: " + d(b)
                   + " abc: " + d(c));
    }

    // G5: 65537^3 mod 2^32 == 196609, so a 32-bit row count would match.
    {
        bool ok = false;
        const LutData l = LutImporter::loadCubeFile(
            writeCube(dir, "g5.cube", "LUT_3D_SIZE 65537", 196609, &ok));
        tally.gate(5, ok && !l.isValid(), ok ? d(l) : "fixture write failed");
    }

    // G6: upper bounds. 256 passes the size check and then fails on row count.
    {
        bool oka = false, okb = false, okc = false;
        const LutData a = LutImporter::loadCubeFile(writeCube(dir, "g6a.cube", "LUT_3D_SIZE 257", 1, &oka));
        const LutData b = LutImporter::loadCubeFile(writeCube(dir, "g6b.cube", "LUT_1D_SIZE 65537", 1, &okb));
        const LutData c = LutImporter::loadCubeFile(writeCube(dir, "g6c.cube", "LUT_3D_SIZE 256", 1, &okc));
        const bool ok = oka && okb && okc;
        tally.gate(6, ok && !a.isValid() && !b.isValid() && !c.isValid(),
                   std::string(ok ? "" : "fixture write failed; ") + "3D257: " + d(a) + " 1D65537: "
                   + d(b) + " 3D256: " + d(c));
    }

    // G7: 2-point 1D: valid, lo -> first row, hi -> last row (US-2102: the top interval is
    // interpolated, so white reaches the last row), size 1 control invalid.
    {
        const QString p = dir.filePath(QStringLiteral("g7.cube"));
        QFile f(p);
        bool wrote = f.open(QIODevice::WriteOnly);
        if (wrote) { f.write("LUT_1D_SIZE 2\n0.2 0.4 0.6\n0.8 0.9 1.0\n"); f.close(); }
        const LutData l = LutImporter::loadCubeFile(p);
        bool oneOk = false;
        const LutData one = LutImporter::loadCubeFile(writeCube(dir, "g7one.cube", "LUT_1D_SIZE 1", 1, &oneOk));
        const QColor lo = LutImporter::applyLut(pixel(0, 0, 0), l).pixelColor(0, 0);
        const QColor mid = LutImporter::applyLut(pixel(128, 128, 128), l).pixelColor(0, 0);
        const QColor hi = LutImporter::applyLut(pixel(255, 255, 255), l).pixelColor(0, 0);
        // 0.2/0.4/0.6 -> 51/102/153; 0.8/0.9/1.0 -> 204/229 (0.9f * 255 = 229.5 truncates)/255
        const bool ok = wrote && l.isValid() && l.size == 2 && near(lo, 51, 102, 153)
            && near(mid, 127, 166, 204) && near(hi, 204, 229, 255)
            && oneOk && !one.isValid();
        tally.gate(7, ok, d(l) + " lo=" + rgb(lo) + " mid=" + rgb(mid) + " hi=" + rgb(hi)
                              + " | size1: " + (oneOk ? d(one) : std::string("fixture write failed")));
    }

    // G8: 3-point 1D identity: white reaches the last row, 191 stays 191.
    {
        bool wok = false;
        const LutData l = loadText(dir, "g8.cube", "LUT_1D_SIZE 3\n0 0 0\n0.5 0.5 0.5\n1 1 1\n", &wok);
        const QColor w = LutImporter::applyLut(pixel(255, 255, 255), l).pixelColor(0, 0);
        const QColor m = LutImporter::applyLut(pixel(191, 191, 191), l).pixelColor(0, 0);
        tally.gate(8, wok && l.isValid() && near(w, 255, 255, 255) && near(m, 191, 191, 191),
                   (wok ? d(l) : std::string("fixture write failed")) + " white=" + rgb(w) + " 191=" + rgb(m));
    }

    // G9: an explicit default domain changes nothing against the G1 3D LUT.
    {
        bool aok = false, bok = false;
        const LutData plain = loadText(dir, "g9a.cube", g1Body(), &aok);
        const LutData dom = loadText(dir, "g9b.cube",
            "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n" + g1Body(), &bok);
        const QImage img = gradient4x4();
        const QImage a = LutImporter::applyLut(img, plain);
        const QImage b = LutImporter::applyLut(img, dom);
        const bool same = a.sizeInBytes() > 0 && a.sizeInBytes() == b.sizeInBytes()
            && a.format() == b.format() && a != img && a == b;
        tally.gate(9, aok && bok && plain.isValid() && dom.isValid() && same,
                   std::string("plain: ") + d(plain) + " domain: " + d(dom) + " same=" + std::to_string(same));
    }

    // G10: domain 0..2 normalises the input; DOMAIN_MIN after the table gives the same result.
    {
        bool aok = false, bok = false;
        const LutData a = loadText(dir, "g10a.cube",
            "LUT_1D_SIZE 2\nDOMAIN_MAX 2 2 2\n0 0 0\n1 1 1\n", &aok);
        const LutData b = loadText(dir, "g10b.cube",
            "LUT_1D_SIZE 2\n0 0 0\n1 1 1\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n", &bok);
        const QColor aw = LutImporter::applyLut(pixel(255, 255, 255), a).pixelColor(0, 0);
        const QColor ak = LutImporter::applyLut(pixel(0, 0, 0), a).pixelColor(0, 0);
        const QColor bw = LutImporter::applyLut(pixel(255, 255, 255), b).pixelColor(0, 0);
        const QColor bk = LutImporter::applyLut(pixel(0, 0, 0), b).pixelColor(0, 0);
        const bool ok = aok && bok && a.isValid() && b.isValid()
            && near(aw, 127, 127, 127) && (aw.red() == 127 || aw.red() == 128)
            && near(ak, 0, 0, 0) && near(bw, 127, 127, 127) && near(bk, 0, 0, 0) && aw == bw && ak == bk;
        tally.gate(10, ok, "a white=" + rgb(aw) + " black=" + rgb(ak) + " b white=" + rgb(bw)
                              + " black=" + rgb(bk));
    }

    // G11: invalid domains reject the LUT; valid controls (incl. trailing comment) load.
    {
        const QByteArray two = "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n";
        bool o[6] = {};
        const LutData eq = loadText(dir, "g11a.cube", two + "DOMAIN_MAX 0 0 0\n", &o[0]);
        const LutData inv = loadText(dir, "g11b.cube", two + "DOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n", &o[1]);
        const LutData two2 = loadText(dir, "g11c.cube", two + "DOMAIN_MIN 0 0\n", &o[2]);
        const LutData nan = loadText(dir, "g11d.cube", two + "DOMAIN_MAX nan nan nan\n", &o[3]);
        bool cok = false, dok = false, eok = false;
        const LutData ctl = loadText(dir, "g11e.cube", "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n" + g1Body(), &cok);
        const LutData cmt = loadText(dir, "g11f.cube",
            "DOMAIN_MIN 0 0 0 # lo\nDOMAIN_MAX 1 1 1 # comment\n" + g1Body(), &dok);
        const LutData plain = loadText(dir, "g11g.cube", g1Body(), &eok);
        const QImage img = gradient4x4();
        const QImage ci = LutImporter::applyLut(img, cmt);
        const bool same = ci == LutImporter::applyLut(img, plain) && ci != img;
        const bool wrote = o[0] && o[1] && o[2] && o[3] && cok && dok && eok;
        tally.gate(11, wrote && !eq.isValid() && !inv.isValid() && !two2.isValid() && !nan.isValid()
                              && ctl.isValid() && cmt.isValid() && same,
                   std::string(wrote ? "" : "fixture write failed; ") + "max=min: " + d(eq) + " inverted: " + d(inv)
                   + " 2 comps: " + d(two2) + " nan: " + d(nan) + " ctl: " + d(ctl)
                   + " comment: " + d(cmt) + " same=" + std::to_string(same));
    }

    // G12: GPU grid (no GL needed).
    {
        // (a) 2-point 1D -> 2^3 grid whose corners match applyLut; metadata is carried over.
        bool aok = false;
        LutData l = loadText(dir, "g12a.cube", "LUT_1D_SIZE 2\n0.2 0.4 0.6\n0.8 0.9 1.0\n", &aok);
        const LutData g = LutImporter::toGpuGrid(l);
        bool cornersOk = g.table.size() == 8;
        std::string cornerDetail;
        for (int i = 0; cornersOk && i < 8; ++i) {
            const int r = (i & 1) * 255, gg = ((i >> 1) & 1) * 255, b = ((i >> 2) & 1) * 255;
            const QColor c = LutImporter::applyLut(pixel(r, gg, b), l).pixelColor(0, 0);
            const QVector3D v = g.table[i];
            cornersOk = std::abs(v.x() * 255.0f - c.red()) <= 1.0f && std::abs(v.y() * 255.0f - c.green()) <= 1.0f
                     && std::abs(v.z() * 255.0f - c.blue()) <= 1.0f;
            cornerDetail += " c" + std::to_string(i) + "=" + rgb(c);
        }
        l.intensity = 0.5;
        l.name = QStringLiteral("n");
        l.filePath = QStringLiteral("p.cube");
        const LutData gm = LutImporter::toGpuGrid(l);
        const bool metaOk = gm.intensity == 0.5 && gm.name == l.name && gm.filePath == l.filePath
            && !gm.isBuiltIn() && gm.type == LutType::Cube3D;
        const bool okA = aok && g.type == LutType::Cube3D && g.size == 2 && g.table.size() == 8
            && cornersOk && metaOk;

        // (b) 65-point identity -> 65^3, grid point (i,j,k) == (i,j,k)/64; (b') 1024-point v^3 -> 65^3.
        QByteArray id65 = "LUT_1D_SIZE 65\n";
        for (int i = 0; i < 65; ++i) {
            const QByteArray v = QByteArray::number(i / 64.0, 'g', 17);
            id65 += v + " " + v + " " + v + "\n";
        }
        bool bok = false, b2ok = false;
        const LutData id = loadText(dir, "g12b.cube", id65, &bok);
        const LutData idg = LutImporter::toGpuGrid(id);
        bool idOk = idg.size == 65 && idg.table.size() == 65 * 65 * 65;
        for (int k = 0; idOk && k < 65; ++k)
            for (int j = 0; idOk && j < 65; ++j)
                for (int i = 0; idOk && i < 65; ++i) {
                    const QVector3D v = idg.table[i + j * 65 + k * 65 * 65];
                    idOk = std::abs(v.x() - i / 64.0f) <= 1e-6f && std::abs(v.y() - j / 64.0f) <= 1e-6f
                        && std::abs(v.z() - k / 64.0f) <= 1e-6f;
                }
        QByteArray cube1k = "LUT_1D_SIZE 1024\n";
        for (int i = 0; i < 1024; ++i) {
            const double t = i / 1023.0;
            const QByteArray v = QByteArray::number(t * t * t, 'g', 17);
            cube1k += v + " " + v + " " + v + "\n";
        }
        const LutData big = loadText(dir, "g12b2.cube", cube1k, &b2ok);
        const LutData bigg = LutImporter::toGpuGrid(big);
        bool bigOk = big.isValid() && bigg.size == 65 && bigg.table.size() == 65 * 65 * 65;
        for (int k = 0; bigOk && k < 65; ++k)
            for (int j = 0; bigOk && j < 65; ++j)
                for (int i = 0; bigOk && i < 65; ++i) {
                    const QVector3D e = LutImporter::sample(big, QVector3D(i / 64.0f, j / 64.0f, k / 64.0f));
                    const QVector3D v = bigg.table[i + j * 65 + k * 65 * 65];
                    bigOk = std::abs(v.x() - e.x()) <= 1e-6f && std::abs(v.y() - e.y()) <= 1e-6f
                         && std::abs(v.z() - e.z()) <= 1e-6f;
                }
        // independent anchor: grid point (32,32,32) is t = 0.5 -> about 0.125 (1D table step 1/1023)
        const bool anchor = bigg.table.size() == 65 * 65 * 65
            && std::abs(bigg.table[32 + 32 * 65 + 32 * 65 * 65].x() - 0.125f) < 1e-3f;

        // (c) 3D LUT with DOMAIN_MAX 2 -> default-domain 3D grid; white matches the original.
        bool cok = false;
        const LutData dom = loadText(dir, "g12c.cube", "DOMAIN_MAX 2 2 2\n" + g1Body(), &cok);
        const LutData domg = LutImporter::toGpuGrid(dom);
        const QColor w0 = LutImporter::applyLut(pixel(255, 255, 255), dom).pixelColor(0, 0);
        const QColor w1 = LutImporter::applyLut(pixel(255, 255, 255), domg).pixelColor(0, 0);
        bool okC = cok && dom.isValid() && !dom.hasDefaultDomain() && domg.type == LutType::Cube3D
            && domg.size == 65 && domg.hasDefaultDomain() && near(w1, w0.red(), w0.green(), w0.blue());
        // Narrow (0..0.5, row v^2) and shifted (0.1..0.6, row v^2 / 2) 3-point domains: the
        // grid tracks the original on the diagonal and an off-axis line within 2/255.
        std::string domDetail;
        const char *domHeads[2] = {"DOMAIN_MIN 0 0 0\nDOMAIN_MAX 0.5 0.5 0.5\n",
                                   "DOMAIN_MIN 0.1 0.1 0.1\nDOMAIN_MAX 0.6 0.6 0.6\n"};
        const char *domNames[2] = {"g12c2.cube", "g12c3.cube"};
        for (int d = 0; d < 2; ++d) {
            const double scale = d == 0 ? 1.0 : 0.5;
            QByteArray text = QByteArray("LUT_3D_SIZE 3\n") + domHeads[d];
            for (int b = 0; b < 3; ++b)
                for (int gg = 0; gg < 3; ++gg)
                    for (int r = 0; r < 3; ++r)
                        text += QByteArray::number(scale * r * r / 4.0) + " "
                              + QByteArray::number(scale * gg * gg / 4.0) + " "
                              + QByteArray::number(scale * b * b / 4.0) + "\n";
            bool lok = false;
            const LutData orig = loadText(dir, domNames[d], text, &lok);
            const LutData grid = LutImporter::toGpuGrid(orig);
            const bool valid = lok && orig.isValid() && grid.isValid();
            float maxErr = valid ? 0.0f : 1.0f;
            for (int k = 0; valid && k <= 255; ++k) {
                const float t = k / 255.0f;
                const QVector3D xs[2] = {QVector3D(t, t, t), QVector3D(t, 0.25f, 0.75f)};
                for (const QVector3D &x : xs) {
                    const QVector3D e = LutImporter::sample(grid, x) - LutImporter::sample(orig, x);
                    maxErr = qMax(maxErr, qMax(std::abs(e.x()), qMax(std::abs(e.y()), std::abs(e.z()))));
                }
            }
            okC = okC && valid && orig.size == 3 && !orig.hasDefaultDomain()
                && grid.hasDefaultDomain() && maxErr <= 2.0f / 255.0f;
            domDetail += " dom" + std::to_string(d) + "_err255=" + std::to_string(maxErr * 255.0f);
        }
        // Logged only, not part of the gate (Wave 22 carryover C-02-residual): a domain narrower
        // than one 1/64 grid step (0.1..0.101, row 0 / 0.25 / 1) cannot be tracked by a uniform grid.
        {
            const double row[3] = {0.0, 0.25, 1.0};
            QByteArray text("LUT_3D_SIZE 3\nDOMAIN_MIN 0.1 0.1 0.1\nDOMAIN_MAX 0.101 0.101 0.101\n");
            for (int b = 0; b < 3; ++b)
                for (int gg = 0; gg < 3; ++gg)
                    for (int r = 0; r < 3; ++r)
                        text += QByteArray::number(row[r]) + " " + QByteArray::number(row[gg]) + " "
                              + QByteArray::number(row[b]) + "\n";
            bool rok = false;
            const LutData orig = loadText(dir, "g12c4.cube", text, &rok);
            const LutData grid = LutImporter::toGpuGrid(orig);
            if (rok && orig.isValid() && grid.isValid()) {
                const float t = 26.0f / 255.0f;
                const float ov = LutImporter::sample(orig, QVector3D(t, t, t)).x();
                const float gv = LutImporter::sample(grid, QVector3D(t, t, t)).x();
                std::cerr << "[INFO] lut-import G12 (c) narrow-domain residual (logged, not a pass): loaded="
                          << rok << " DOMAIN 0.1..0.101 x=26/255 orig=" << ov << " grid=" << gv
                          << " err255=" << std::abs(gv - ov) * 255.0f << "\n";
            } else {
                std::cerr << "[INFO] lut-import G12 (c) narrow-domain residual: fixture not loaded\n";
            }
        }

        // (d) default-domain 3D LUT is returned unchanged.
        bool dok = false;
        const LutData plain = loadText(dir, "g12d.cube", g1Body(), &dok);
        const LutData pg = LutImporter::toGpuGrid(plain);
        const bool okD = dok && plain.isValid() && pg.size == plain.size && pg.table == plain.table
            && pg.type == plain.type && pg.intensity == plain.intensity;

        tally.gate(12, okA && bok && idOk && b2ok && bigOk && anchor && okC && okD,
                   "a=" + std::to_string(okA) + cornerDetail + " b=" + std::to_string(bok && idOk)
                   + " b'=" + std::to_string(b2ok && bigOk) + " anchor=" + std::to_string(anchor)
                   + " c=" + std::to_string(okC) + " w0=" + rgb(w0) + " w1=" + rgb(w1) + domDetail
                   + " d=" + std::to_string(okD));
    }

    return tally.finish("lut-import");
}
