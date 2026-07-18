#include "AnimatedExport.h"

#include <QImageWriter>
#include <QFile>
#include <QDataStream>
#include <QHash>
#include <QtGlobal>
#include <QtMath>

#include <algorithm>
#include <vector>
#include <limits>
#include <cstdint>

namespace animexport {

// ===========================================================================
// median-cut color quantization
// ===========================================================================

namespace {

struct RgbColor {
    quint8 r = 0;
    quint8 g = 0;
    quint8 b = 0;
};

struct ColorBox {
    int begin = 0;   // [begin, end) range into the working color vector
    int end   = 0;
};

// Longest axis of the box: 0=R, 1=G, 2=B
int longestAxis(const std::vector<RgbColor> &colors, const ColorBox &box)
{
    int rMin = 255, rMax = 0, gMin = 255, gMax = 0, bMin = 255, bMax = 0;
    for (int i = box.begin; i < box.end; ++i) {
        const RgbColor &c = colors[static_cast<size_t>(i)];
        rMin = qMin<int>(rMin, c.r);
        rMax = qMax<int>(rMax, c.r);
        gMin = qMin<int>(gMin, c.g);
        gMax = qMax<int>(gMax, c.g);
        bMin = qMin<int>(bMin, c.b);
        bMax = qMax<int>(bMax, c.b);
    }
    const int rRange = rMax - rMin;
    const int gRange = gMax - gMin;
    const int bRange = bMax - bMin;
    if (gRange >= rRange && gRange >= bRange)
        return 1;
    if (rRange >= gRange && rRange >= bRange)
        return 0;
    return 2;
}

QRgb boxAverage(const std::vector<RgbColor> &colors, const ColorBox &box)
{
    if (box.end <= box.begin)
        return qRgb(0, 0, 0);
    quint64 sr = 0, sg = 0, sb = 0;
    const quint64 n = static_cast<quint64>(box.end - box.begin);
    for (int i = box.begin; i < box.end; ++i) {
        const RgbColor &c = colors[static_cast<size_t>(i)];
        sr += c.r;
        sg += c.g;
        sb += c.b;
    }
    return qRgb(static_cast<int>(sr / n),
                static_cast<int>(sg / n),
                static_cast<int>(sb / n));
}

} // anonymous namespace

QVector<QRgb> medianCutPalette(const QImage &img, int maxColors)
{
    QVector<QRgb> palette;
    if (img.isNull() || maxColors <= 0)
        return palette;
    if (maxColors > 256)
        maxColors = 256;

    const QImage src = img.convertToFormat(QImage::Format_RGB32);
    const int w = src.width();
    const int h = src.height();
    if (w <= 0 || h <= 0)
        return palette;

    // Sample if huge: cap the working set so quantization stays fast.
    const qint64 total = static_cast<qint64>(w) * static_cast<qint64>(h);
    const qint64 kMaxSamples = 65536;
    int stride = 1;
    if (total > kMaxSamples)
        stride = static_cast<int>((total + kMaxSamples - 1) / kMaxSamples);

    std::vector<RgbColor> colors;
    colors.reserve(static_cast<size_t>(qMin<qint64>(total, kMaxSamples) + 1));

    qint64 idx = 0;
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < w; ++x, ++idx) {
            if ((idx % stride) != 0)
                continue;
            const QRgb p = line[x];
            RgbColor c;
            c.r = static_cast<quint8>(qRed(p));
            c.g = static_cast<quint8>(qGreen(p));
            c.b = static_cast<quint8>(qBlue(p));
            colors.push_back(c);
        }
    }

    if (colors.empty())
        return palette;

    // Recursive median-cut via an explicit box list (split until maxColors).
    std::vector<ColorBox> boxes;
    boxes.push_back(ColorBox{0, static_cast<int>(colors.size())});

    while (static_cast<int>(boxes.size()) < maxColors) {
        // Pick the box with the most colors that is still splittable.
        int target = -1;
        int bestCount = 1;
        for (size_t i = 0; i < boxes.size(); ++i) {
            const int count = boxes[i].end - boxes[i].begin;
            if (count > bestCount) {
                bestCount = count;
                target = static_cast<int>(i);
            }
        }
        if (target < 0)
            break; // every box has <=1 color; cannot split further

        ColorBox box = boxes[static_cast<size_t>(target)];
        const int axis = longestAxis(colors, box);

        std::sort(colors.begin() + box.begin, colors.begin() + box.end,
                  [axis](const RgbColor &a, const RgbColor &b) {
                      if (axis == 0) return a.r < b.r;
                      if (axis == 1) return a.g < b.g;
                      return a.b < b.b;
                  });

        const int mid = box.begin + (box.end - box.begin) / 2;
        ColorBox left{box.begin, mid};
        ColorBox right{mid, box.end};
        if (left.end <= left.begin || right.end <= right.begin)
            break; // degenerate split; stop

        boxes[static_cast<size_t>(target)] = left;
        boxes.push_back(right);
    }

    palette.reserve(static_cast<int>(boxes.size()));
    for (const ColorBox &box : boxes)
        palette.append(boxAverage(colors, box));

    if (palette.size() > maxColors)
        palette.resize(maxColors);
    return palette;
}

// ===========================================================================
// GIF89a writer with full LZW encoder
// ===========================================================================

namespace {

int nearestPaletteIndex(const QVector<QRgb> &pal, QRgb c)
{
    int best = 0;
    qint64 bestDist = std::numeric_limits<qint64>::max();
    const int cr = qRed(c);
    const int cg = qGreen(c);
    const int cb = qBlue(c);
    for (int i = 0; i < pal.size(); ++i) {
        const QRgb p = pal[i];
        const qint64 dr = cr - qRed(p);
        const qint64 dg = cg - qGreen(p);
        const qint64 db = cb - qBlue(p);
        const qint64 d = dr * dr + dg * dg + db * db;
        if (d < bestDist) {
            bestDist = d;
            best = i;
            if (d == 0)
                break;
        }
    }
    return best;
}

// LZW bit packer that emits GIF-style 255-byte sub-blocks.
class GifBitWriter {
public:
    explicit GifBitWriter(QByteArray &out) : m_out(out) {}

    void writeBits(int code, int bits)
    {
        m_acc |= (static_cast<quint32>(code) << m_nbits);
        m_nbits += bits;
        while (m_nbits >= 8) {
            m_buffer.append(static_cast<char>(m_acc & 0xFF));
            m_acc >>= 8;
            m_nbits -= 8;
            if (m_buffer.size() == 255)
                flushSubBlock();
        }
    }

    void finish()
    {
        if (m_nbits > 0) {
            m_buffer.append(static_cast<char>(m_acc & 0xFF));
            m_acc = 0;
            m_nbits = 0;
        }
        flushSubBlock();
        m_out.append(static_cast<char>(0x00)); // block terminator
    }

private:
    void flushSubBlock()
    {
        if (m_buffer.isEmpty())
            return;
        m_out.append(static_cast<char>(m_buffer.size() & 0xFF));
        m_out.append(m_buffer);
        m_buffer.clear();
    }

    QByteArray &m_out;
    QByteArray  m_buffer;
    quint32     m_acc   = 0;
    int         m_nbits = 0;
};

// Standard GIF LZW: variable code width, clear/EOI codes, dictionary reset.
void lzwCompress(const QVector<quint8> &indices, int minCodeSize, QByteArray &out)
{
    const int clearCode = 1 << minCodeSize;
    const int eoiCode   = clearCode + 1;

    out.append(static_cast<char>(minCodeSize & 0xFF));

    GifBitWriter bw(out);

    QHash<quint32, int> dict;
    auto resetDict = [&]() {
        dict.clear();
        // single-symbol entries are implicit; next free code starts after EOI
    };

    int codeSize  = minCodeSize + 1;
    int nextCode  = eoiCode + 1;
    resetDict();

    bw.writeBits(clearCode, codeSize);

    if (indices.isEmpty()) {
        bw.writeBits(eoiCode, codeSize);
        bw.finish();
        return;
    }

    int current = indices[0];

    for (int i = 1; i < indices.size(); ++i) {
        const int k = indices[i];
        // Key combines the running code with the next symbol.
        const quint32 key = (static_cast<quint32>(current) << 8) |
                            static_cast<quint32>(k & 0xFF);
        auto it = dict.constFind(key);
        if (it != dict.constEnd()) {
            current = it.value();
        } else {
            bw.writeBits(current, codeSize);
            if (nextCode < 4096) {
                dict.insert(key, nextCode);
                ++nextCode;
                if (nextCode > (1 << codeSize) && codeSize < 12)
                    ++codeSize;
            } else {
                // Dictionary full: emit clear and restart.
                bw.writeBits(clearCode, codeSize);
                resetDict();
                codeSize = minCodeSize + 1;
                nextCode = eoiCode + 1;
            }
            current = k;
        }
    }

    bw.writeBits(current, codeSize);
    bw.writeBits(eoiCode, codeSize);
    bw.finish();
}

int powerOfTwoCeil(int n)
{
    int p = 2;
    while (p < n && p < 256)
        p <<= 1;
    return p;
}

bool writeGif(const QVector<QImage> &frames, const QString &outPath,
              const ExportConfig &cfg)
{
    // Scale frames first (preserve aspect to cfg.width).
    QVector<QImage> scaled;
    scaled.reserve(frames.size());
    for (const QImage &f : frames) {
        if (f.isNull())
            continue;
        QImage s = f;
        if (cfg.width > 0 && f.width() != cfg.width) {
            s = f.scaledToWidth(cfg.width, Qt::SmoothTransformation);
        }
        scaled.append(s.convertToFormat(QImage::Format_RGB32));
    }
    if (scaled.isEmpty())
        return false;

    const int canvasW = scaled.first().width();
    const int canvasH = scaled.first().height();
    if (canvasW <= 0 || canvasH <= 0)
        return false;

    // Global color table from the first frame.
    QVector<QRgb> palette = medianCutPalette(scaled.first(), 256);
    if (palette.isEmpty())
        palette.append(qRgb(0, 0, 0));

    const int gctSize    = powerOfTwoCeil(palette.size());
    int colorResolution  = 0;
    {
        int p = gctSize;
        while (p > 1) { p >>= 1; ++colorResolution; }
    }
    const int gctBits = qMax(1, colorResolution); // log2(gctSize)
    const int minCodeSize = qMax(2, gctBits);

    QByteArray out;

    // --- Header ---
    out.append("GIF89a", 6);

    // --- Logical Screen Descriptor ---
    auto appendU16 = [&](int v) {
        out.append(static_cast<char>(v & 0xFF));
        out.append(static_cast<char>((v >> 8) & 0xFF));
    };
    appendU16(canvasW);
    appendU16(canvasH);
    // packed: global color table flag (1) | color resolution (3) |
    //         sort flag (0) | size of GCT (3)
    const quint8 packed = static_cast<quint8>(
        0x80 | ((gctBits - 1) << 4) | (gctBits - 1));
    out.append(static_cast<char>(packed));
    out.append(static_cast<char>(0x00)); // background color index
    out.append(static_cast<char>(0x00)); // pixel aspect ratio

    // --- Global Color Table (padded to power-of-two) ---
    for (int i = 0; i < gctSize; ++i) {
        QRgb c = (i < palette.size()) ? palette[i] : qRgb(0, 0, 0);
        out.append(static_cast<char>(qRed(c)));
        out.append(static_cast<char>(qGreen(c)));
        out.append(static_cast<char>(qBlue(c)));
    }

    // --- Netscape 2.0 looping extension ---
    {
        const int loopCount = qMax(0, cfg.loop); // 0 = forever
        out.append(static_cast<char>(0x21)); // extension introducer
        out.append(static_cast<char>(0xFF)); // application extension label
        out.append(static_cast<char>(0x0B)); // block size
        out.append("NETSCAPE2.0", 11);
        out.append(static_cast<char>(0x03)); // sub-block size
        out.append(static_cast<char>(0x01)); // sub-block id
        appendU16(loopCount);
        out.append(static_cast<char>(0x00)); // block terminator
    }

    const int fps = qMax(1, cfg.fps);
    const int delayCs = qMax(1, 100 / fps); // centiseconds per frame

    // --- Per-frame ---
    for (const QImage &frame : scaled) {
        QImage fr = frame;
        if (fr.width() != canvasW || fr.height() != canvasH)
            fr = fr.scaled(canvasW, canvasH, Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation);

        // Graphic Control Extension (delay).
        out.append(static_cast<char>(0x21)); // extension introducer
        out.append(static_cast<char>(0xF9)); // graphic control label
        out.append(static_cast<char>(0x04)); // block size
        out.append(static_cast<char>(0x00)); // packed (no transparency)
        appendU16(delayCs);
        out.append(static_cast<char>(0x00)); // transparent color index
        out.append(static_cast<char>(0x00)); // block terminator

        // Image Descriptor.
        out.append(static_cast<char>(0x2C)); // image separator
        appendU16(0); // left
        appendU16(0); // top
        appendU16(canvasW);
        appendU16(canvasH);
        out.append(static_cast<char>(0x00)); // no local color table

        // Map pixels to nearest palette index.
        QVector<quint8> indices;
        indices.reserve(canvasW * canvasH);
        for (int y = 0; y < canvasH; ++y) {
            const QRgb *line =
                reinterpret_cast<const QRgb *>(fr.constScanLine(y));
            for (int x = 0; x < canvasW; ++x)
                indices.append(static_cast<quint8>(
                    nearestPaletteIndex(palette, line[x])));
        }

        lzwCompress(indices, minCodeSize, out);
    }

    // --- Trailer ---
    out.append(static_cast<char>(0x3B));

    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const qint64 written = file.write(out);
    file.close();
    return written == out.size();
}

bool writeWebp(const QVector<QImage> &frames, const QString &outPath,
               const ExportConfig &cfg)
{
    QVector<QImage> scaled;
    scaled.reserve(frames.size());
    for (const QImage &f : frames) {
        if (f.isNull())
            continue;
        if (cfg.width > 0 && f.width() != cfg.width)
            scaled.append(f.scaledToWidth(cfg.width, Qt::SmoothTransformation));
        else
            scaled.append(f);
    }
    if (scaled.isEmpty())
        return false;

    QImageWriter writer(outPath, "webp");
    if (!writer.canWrite()) {
        qWarning("animexport: WebP write not supported by this Qt build "
                 "(install the Qt imageformats plugin)");
        return false;
    }

    writer.setQuality(qBound(0, cfg.quality, 100));

    // QImageWriter in this Qt build has no per-writer animation query API.
    // Probe multi-frame support empirically: if the format/plugin accepts a
    // first write and at least one subsequent frame, treat it as animated;
    // otherwise fall back to writing only the first frame.
    if (scaled.size() > 1) {
        writer.setFormat("webp");
        bool ok = writer.write(scaled.first());
        if (ok) {
            bool allOk = true;
            for (int i = 1; i < scaled.size(); ++i) {
                if (!writer.write(scaled[i])) {
                    allOk = false;
                    break;
                }
            }
            if (allOk)
                return true;
            qWarning("animexport: multi-frame WebP not supported by this "
                     "Qt imageformats plugin; writing first frame only");
        } else {
            qWarning("animexport: animated WebP needs the Qt imageformats "
                     "plugin; writing first frame only");
        }
    }

    // Single frame fallback (still a success if the file is written).
    QImageWriter single(outPath, "webp");
    single.setQuality(qBound(0, cfg.quality, 100));
    return single.write(scaled.first());
}

} // anonymous namespace

// ===========================================================================
// public entry point
// ===========================================================================

bool exportFrames(const QVector<QImage> &frames,
                  const QString &outPath,
                  const ExportConfig &cfg)
{
    if (frames.isEmpty() || outPath.isEmpty())
        return false;

    bool anyValid = false;
    for (const QImage &f : frames) {
        if (!f.isNull()) {
            anyValid = true;
            break;
        }
    }
    if (!anyValid)
        return false;

    switch (cfg.format) {
    case Format::Gif:
        return writeGif(frames, outPath, cfg);
    case Format::WebP:
        return writeWebp(frames, outPath, cfg);
    }
    return false;
}

} // namespace animexport
