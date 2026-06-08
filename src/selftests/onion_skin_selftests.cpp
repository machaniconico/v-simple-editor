// Onion skin headless selftest.
//
// QApplication 不要。onionskin::compose() の純粋 display-local 合成だけを検証する。

#include <cstring>
#include <cstddef>
#include <cstdio>

#include <QColor>
#include <QImage>
#include <QVector>

#include "../OnionSkin.h"

namespace {

QImage solid(int w, int h, const QColor &c, QImage::Format fmt = QImage::Format_ARGB32)
{
    QImage img(w, h, fmt);
    img.fill(c);
    return img;
}

bool byteIdentical(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()
        || a.format() != b.format()
        || a.bytesPerLine() != b.bytesPerLine()) {
        return false;
    }
    const int byteCount = a.bytesPerLine() * a.height();
    if (byteCount <= 0)
        return true;
    return std::memcmp(a.constBits(), b.constBits(),
                       static_cast<std::size_t>(byteCount)) == 0;
}

} // namespace

int runOnionSkinSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](const char *name, bool ok) {
        std::printf("[onion-skin] %s: %s\n", name, ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    };

    // G1: enabled=false → 入力と byte-identical。
    {
        const QImage current = solid(8, 8, QColor(40, 80, 120, 255));
        const QVector<QImage> before{solid(8, 8, QColor(255, 0, 0, 255))};
        onionskin::Config cfg;
        cfg.enabled = false;
        const QImage out = onionskin::compose(current, before, {}, cfg);
        check("G1 disabled is byte-identical", byteIdentical(current, out));
    }

    // G2: ghost が空なら no-op。
    {
        const QImage current = solid(8, 8, QColor(10, 20, 30, 255));
        onionskin::Config cfg;
        cfg.enabled = true;
        const QImage out = onionskin::compose(current, {}, {}, cfg);
        check("G2 empty ghost vectors are no-op", byteIdentical(current, out));
    }

    // G3: opacity=0 → 入力と byte-identical。
    {
        const QImage current = solid(8, 8, QColor(20, 40, 60, 255));
        const QVector<QImage> after{solid(8, 8, QColor(0, 0, 255, 255))};
        onionskin::Config cfg;
        cfg.enabled = true;
        cfg.opacity = 0.0;
        const QImage out = onionskin::compose(current, {}, after, cfg);
        check("G3 opacity zero is byte-identical", byteIdentical(current, out));
    }

    // G4: 通常合成はサイズとフォーマットを保持する。
    {
        const QImage current = solid(16, 12, QColor(32, 32, 32, 255));
        const QVector<QImage> before{solid(8, 6, QColor(255, 255, 255, 255))};
        const QVector<QImage> after{solid(16, 12, QColor(0, 0, 0, 255))};
        onionskin::Config cfg;
        cfg.enabled = true;
        cfg.opacity = 0.5;
        const QImage out = onionskin::compose(current, before, after, cfg);
        check("G4 blend preserves size and format",
              out.size() == current.size() && out.format() == current.format());
    }

    std::printf("[onion-skin] Result: %d/4 PASSED\n", passed);
    return failed == 0 ? 0 : 1;
}
