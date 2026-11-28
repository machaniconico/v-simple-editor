#include "SelftestGate.h"
#include "../LutImporter.h"

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QString>
#include <QTemporaryDir>

#include <cstdlib>
#include <string>

// US-2003: .cube sizes outside 2..256 (3D) / 2..65536 (1D) must load as invalid
// LUTs, and LutData::isValid() needs size >= 2. G1 / G7 are 2-point controls.
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

    // G7: 2-point 1D (W20-R1): valid, (0) -> first row, size 1 control invalid.
    // The (1) output follows the unchanged 1D path and is recorded, not judged.
    {
        const QString p = dir.filePath(QStringLiteral("g7.cube"));
        QFile f(p);
        bool wrote = f.open(QIODevice::WriteOnly);
        if (wrote) { f.write("LUT_1D_SIZE 2\n0.2 0.4 0.6\n0.8 0.9 1.0\n"); f.close(); }
        const LutData l = LutImporter::loadCubeFile(p);
        bool oneOk = false;
        const LutData one = LutImporter::loadCubeFile(writeCube(dir, "g7one.cube", "LUT_1D_SIZE 1", 1, &oneOk));
        const QColor lo = LutImporter::applyLut(pixel(0, 0, 0), l).pixelColor(0, 0);
        const QColor hi = LutImporter::applyLut(pixel(255, 255, 255), l).pixelColor(0, 0);
        // 0.2/0.4/0.6 -> 51/102/153
        const bool ok = wrote && l.isValid() && l.size == 2 && near(lo, 51, 102, 153)
            && oneOk && !one.isValid();
        tally.gate(7, ok, d(l) + " lo=" + std::to_string(lo.red()) + "," + std::to_string(lo.green())
                              + "," + std::to_string(lo.blue()) + " | size1: " + (oneOk ? d(one) : std::string("fixture write failed"))
                              + " | recorded only, hi=" + std::to_string(hi.red()) + ","
                              + std::to_string(hi.green()) + "," + std::to_string(hi.blue()));
    }

    return tally.finish("lut-import");
}
