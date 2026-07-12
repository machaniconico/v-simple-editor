#include "AffinityPsdImporter.h"

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QPair>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

// ---------------------------------------------------------------------------
// PSD reference: https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
//
// File layout (network / big-endian byte order throughout):
//   1) File header        — signature '8BPS', version, channels, h, w, depth, mode
//   2) Color mode data    — uint32 length + bytes
//   3) Image resources    — uint32 length + bytes
//   4) Layer & mask info  — uint32 length, contains layer info + global mask
//   5) Image data         — composite, compression code + channel rows
//
// We only need (1), (2), (3) for sizing/skipping and (4) for actual layer data.
// ---------------------------------------------------------------------------

namespace affinity {
namespace psd {

namespace {

// -- helpers -----------------------------------------------------------------

bool readBytes(QIODevice& dev, void* dst, qint64 n) {
    return dev.read(static_cast<char*>(dst), n) == n;
}

bool skipBytes(QIODevice& dev, qint64 n) {
    if (n <= 0) return true;
    // QIODevice::skip is available in Qt6 but seek is universally safe here.
    return dev.seek(dev.pos() + n);
}

bool readU16(QIODevice& dev, quint16& out) {
    quint8 b[2];
    if (!readBytes(dev, b, 2)) return false;
    out = (quint16(b[0]) << 8) | quint16(b[1]);
    return true;
}

bool readU32(QIODevice& dev, quint32& out) {
    quint8 b[4];
    if (!readBytes(dev, b, 4)) return false;
    out = (quint32(b[0]) << 24) | (quint32(b[1]) << 16)
        | (quint32(b[2]) << 8)  |  quint32(b[3]);
    return true;
}

bool readS32(QIODevice& dev, qint32& out) {
    quint32 u = 0;
    if (!readU32(dev, u)) return false;
    out = static_cast<qint32>(u);
    return true;
}

bool readU8(QIODevice& dev, quint8& out) {
    return readBytes(dev, &out, 1);
}

// PSD Pascal string padded to multiple of `pad` bytes (incl. length byte).
bool readPascalString(QIODevice& dev, int pad, QString& out) {
    quint8 len = 0;
    if (!readU8(dev, len)) return false;
    QByteArray buf(len, '\0');
    if (len > 0 && !readBytes(dev, buf.data(), len)) return false;
    out = QString::fromLatin1(buf);
    int total = 1 + len;
    int rem = (pad - (total % pad)) % pad;
    return skipBytes(dev, rem);
}

// PSD Unicode string: uint32 char count + UTF-16BE chars (no terminator).
bool readUnicodeString(QIODevice& dev, QString& out) {
    quint32 count = 0;
    if (!readU32(dev, count)) return false;
    if (count == 0) { out.clear(); return true; }
    QByteArray buf(static_cast<int>(count) * 2, '\0');
    if (!readBytes(dev, buf.data(), buf.size())) return false;
    // Manually decode UTF-16BE → QString to avoid QStringDecoder return-type
    // ambiguity across Qt6 minor versions.
    out.clear();
    out.reserve(static_cast<int>(count));
    for (quint32 i = 0; i < count; ++i) {
        const quint8 hi = static_cast<quint8>(buf[int(i) * 2]);
        const quint8 lo = static_cast<quint8>(buf[int(i) * 2 + 1]);
        out.append(QChar(static_cast<ushort>((quint16(hi) << 8) | quint16(lo))));
    }
    // Strip trailing nulls if present (Photoshop sometimes pads).
    while (!out.isEmpty() && out.back() == QChar(u'\0')) out.chop(1);
    return true;
}

// -- PackBits / RLE decode (PSD compression code = 1) -----------------------
//
// For each row we get a stream of:
//   n in [0..127]   -> copy next n+1 literal bytes
//   n in [-127..-1] -> repeat next byte (-n + 1) times
//   n == -128       -> noop
QByteArray decodePackBitsRow(const quint8* src, qint64 srcLen, int dstLen) {
    QByteArray out;
    out.reserve(dstLen);
    qint64 i = 0;
    while (i < srcLen && out.size() < dstLen) {
        qint8 n = static_cast<qint8>(src[i++]);
        if (n >= 0) {
            int count = int(n) + 1;
            if (i + count > srcLen) break;
            out.append(reinterpret_cast<const char*>(src + i), count);
            i += count;
        } else if (n != -128) {
            int count = 1 - int(n);
            if (i >= srcLen) break;
            char b = static_cast<char>(src[i++]);
            out.append(count, b);
        }
        // n == -128 is a no-op
    }
    if (out.size() < dstLen) out.append(dstLen - out.size(), '\0');
    return out;
}

// Read a single channel image plane for a layer.
// `compression` is read from the first 2 bytes of the channel data block.
//
// Returns a byte array of width*height bytes (one byte per pixel) or empty on
// unsupported compression / read error.  ZIP variants (2,3) are warned about
// and an empty buffer is returned, so the caller keeps the layer with a black
// channel rather than aborting the whole document.
QByteArray readChannelPlane(QIODevice& dev, qint64 channelDataLen,
                            int width, int height) {
    if (width <= 0 || height <= 0 || channelDataLen < 2) {
        skipBytes(dev, channelDataLen);
        return {};
    }
    const qint64 planeBytes64 = qint64(width) * qint64(height);
    if (planeBytes64 <= 0 || planeBytes64 > std::numeric_limits<int>::max()) {
        skipBytes(dev, channelDataLen);
        return {};
    }
    quint16 comp = 0;
    if (!readU16(dev, comp)) return {};
    qint64 remaining = channelDataLen - 2;
    if (!dev.isSequential() && remaining > dev.size() - dev.pos()) {
        qWarning() << "[psd] Channel declares" << remaining
                   << "bytes but only" << (dev.size() - dev.pos())
                   << "remain; skipping channel.";
        skipBytes(dev, std::max<qint64>(0, dev.size() - dev.pos()));
        return {};
    }
    const int planeBytes = static_cast<int>(planeBytes64);
    QByteArray plane;

    if (comp == 0) {
        // Raw bytes
        const qint64 bytesToRead = std::min<qint64>(remaining, planeBytes);
        plane.resize(static_cast<int>(bytesToRead));
        if (bytesToRead > 0 && !readBytes(dev, plane.data(), bytesToRead)) return {};
        if (!skipBytes(dev, remaining - bytesToRead)) return {};
        if (plane.size() < planeBytes) plane.append(planeBytes - plane.size(), '\0');
    } else if (comp == 1) {
        // PackBits RLE — first 2*height bytes are per-row byte counts.
        QByteArray header(2 * height, '\0');
        if (remaining < header.size()) {
            skipBytes(dev, remaining);
            return {};
        }
        if (!readBytes(dev, header.data(), header.size())) return {};
        remaining -= header.size();
        QVector<quint16> rowLens(height, 0);
        for (int r = 0; r < height; ++r) {
            const quint8* p = reinterpret_cast<const quint8*>(header.constData()) + r * 2;
            rowLens[r] = (quint16(p[0]) << 8) | quint16(p[1]);
        }
        plane.reserve(planeBytes);
        for (int r = 0; r < height; ++r) {
            quint16 rl = rowLens[r];
            if (rl > remaining) {
                skipBytes(dev, remaining);
                if (plane.size() < planeBytes) plane.append(planeBytes - plane.size(), '\0');
                return plane;
            }
            QByteArray rowSrc(rl, '\0');
            if (rl > 0 && !readBytes(dev, rowSrc.data(), rl)) return plane;
            remaining -= rl;
            QByteArray rowDst = decodePackBitsRow(
                reinterpret_cast<const quint8*>(rowSrc.constData()), rl, width);
            plane.append(rowDst);
        }
    } else if (comp == 2 || comp == 3) {
        qWarning() << "[psd] ZIP-compressed channel (code" << comp
                   << ") is not supported; returning empty plane.";
        skipBytes(dev, remaining);
        return {};
    } else {
        qWarning() << "[psd] Unknown compression code" << comp << "; skipping channel.";
        skipBytes(dev, remaining);
        return {};
    }

    if (plane.size() != planeBytes) {
        plane.resize(planeBytes); // pad with zeros / clamp
    }
    return plane;
}

// Build a single-channel intermediate plane index map keyed by PSD channel id:
//    0 = R, 1 = G, 2 = B, -1 = transparency mask
QImage buildLayerImage(int width, int height,
                      const QHash<int, QByteArray>& planes) {
    if (width <= 0 || height <= 0) return QImage();

    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    const QByteArray& r = planes.value(0);
    const QByteArray& g = planes.value(1);
    const QByteArray& b = planes.value(2);
    const QByteArray& a = planes.value(-1);
    const bool hasA = !a.isEmpty();
    const int planeBytes = width * height;
    const bool rOk = r.size() == planeBytes;
    const bool gOk = g.size() == planeBytes;
    const bool bOk = b.size() == planeBytes;

    for (int y = 0; y < height; ++y) {
        QRgb* dst = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int idx = y * width + x;
            const quint8 rv = rOk ? quint8(r[idx]) : 0;
            const quint8 gv = gOk ? quint8(g[idx]) : 0;
            const quint8 bv = bOk ? quint8(b[idx]) : 0;
            const quint8 av = hasA && a.size() == planeBytes ? quint8(a[idx]) : 255;
            dst[x] = qRgba(rv, gv, bv, av);
        }
    }
    return img;
}

} // namespace (anonymous)

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

QString psdBlendToString(const QString& psdCode) {
    static const QHash<QString, QString> kMap = {
        {"norm", "Normal"},
        {"dark", "Darken"},
        {"mul ", "Multiply"},
        {"idiv", "Color Burn"},
        {"lbrn", "Linear Burn"},
        {"dkCl", "Darker Color"},
        {"lite", "Lighten"},
        {"scrn", "Screen"},
        {"div ", "Color Dodge"},
        {"lddg", "Linear Dodge"},
        {"lgCl", "Lighter Color"},
        {"over", "Overlay"},
        {"sLit", "Soft Light"},
        {"hLit", "Hard Light"},
        {"vLit", "Vivid Light"},
        {"lLit", "Linear Light"},
        {"pLit", "Pin Light"},
        {"hMix", "Hard Mix"},
        {"diff", "Difference"},
        {"smud", "Exclusion"},
        {"fsub", "Subtract"},
        {"fdiv", "Divide"},
        {"hue ", "Hue"},
        {"sat ", "Saturation"},
        {"colr", "Color"},
        {"lum ", "Luminosity"},
        {"pass", "Pass Through"},
        {"diss", "Dissolve"},
    };
    QString key = psdCode;
    // Codes are 4-char ascii padded with spaces — keep the trailing space
    // when looking up but display the trimmed version on miss.
    auto it = kMap.constFind(key);
    if (it != kMap.constEnd()) return it.value();

    QString trimmed = psdCode.trimmed();
    if (trimmed.isEmpty()) return QStringLiteral("Normal");
    trimmed[0] = trimmed[0].toUpper();
    return trimmed;
}

// ---------------------------------------------------------------------------
// loadPsd
// ---------------------------------------------------------------------------

PsdDocument loadPsd(const QString& path) {
    PsdDocument doc; // canvasSize defaults to (0,0), layers empty

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[psd] Cannot open" << path << ":" << file.errorString();
        return doc;
    }

    // ---- 1. File header -------------------------------------------------
    char sig[4] = {};
    if (!readBytes(file, sig, 4) || std::memcmp(sig, "8BPS", 4) != 0) {
        qWarning() << "[psd] Not a PSD file (signature mismatch):" << path;
        return doc;
    }
    quint16 version = 0;
    if (!readU16(file, version) || version != 1) {
        qWarning() << "[psd] Unsupported PSD version" << version
                   << "(only v1 / .psd is supported, not v2 / .psb).";
        return doc;
    }
    if (!skipBytes(file, 6)) return doc; // 6 reserved bytes

    quint16 channels = 0;
    quint32 height = 0, width = 0;
    quint16 depth = 0, colorMode = 0;
    if (!readU16(file, channels) || !readU32(file, height) || !readU32(file, width)
        || !readU16(file, depth)  || !readU16(file, colorMode)) {
        qWarning() << "[psd] Truncated header in" << path;
        return doc;
    }
    if (depth != 8) {
        qWarning() << "[psd]" << depth << "-bit depth is unsupported (8-bit only).";
        // Still report canvas size so the caller can show "unsupported" UI.
    }

    doc.canvasSize = QSize(int(width), int(height));

    // ---- 2. Color mode data --------------------------------------------
    quint32 cmLen = 0;
    if (!readU32(file, cmLen) || !skipBytes(file, cmLen)) return doc;

    // ---- 3. Image resources --------------------------------------------
    quint32 irLen = 0;
    if (!readU32(file, irLen) || !skipBytes(file, irLen)) return doc;

    // ---- 4. Layer & Mask information section ----------------------------
    quint32 lmiLen = 0;
    if (!readU32(file, lmiLen)) return doc;
    if (lmiLen == 0) return doc; // no layers (flattened)

    const qint64 lmiStart = file.pos();
    const qint64 lmiEnd   = lmiStart + lmiLen;

    quint32 layerInfoLen = 0;
    if (!readU32(file, layerInfoLen)) return doc;
    if (layerInfoLen == 0) return doc;

    qint16 rawCount = 0;
    {
        quint16 tmp = 0;
        if (!readU16(file, tmp)) return doc;
        rawCount = static_cast<qint16>(tmp);
    }
    const int layerCount = std::abs(int(rawCount));

    // ---- per-layer records ---------------------------------------------
    struct LayerRec {
        qint32 top = 0, left = 0, bottom = 0, right = 0;
        QVector<QPair<qint16, quint32>> channels; // (id, dataLen)
        QString blendMode = "norm";
        quint8 opacity = 255;
        quint8 flags = 0;
        QString name;
        int sectionType = 0; // 0 = normal, 1/2 = open folder, 3 = close folder
    };
    QVector<LayerRec> recs(layerCount);

    for (int i = 0; i < layerCount; ++i) {
        LayerRec& L = recs[i];
        if (!readS32(file, L.top) || !readS32(file, L.left)
            || !readS32(file, L.bottom) || !readS32(file, L.right)) return doc;

        quint16 nChan = 0;
        if (!readU16(file, nChan)) return doc;
        L.channels.resize(nChan);
        for (int c = 0; c < nChan; ++c) {
            quint16 cid = 0;
            quint32 cLen = 0;
            if (!readU16(file, cid) || !readU32(file, cLen)) return doc;
            L.channels[c] = qMakePair(static_cast<qint16>(cid), cLen);
        }

        // Blend mode signature '8BIM' + 4-char key
        char bsig[4] = {};
        char bkey[4] = {};
        if (!readBytes(file, bsig, 4) || !readBytes(file, bkey, 4)) return doc;
        L.blendMode = QString::fromLatin1(bkey, 4);

        if (!readU8(file, L.opacity)) return doc;
        quint8 clipping = 0;
        if (!readU8(file, clipping)) return doc;
        if (!readU8(file, L.flags)) return doc;
        if (!skipBytes(file, 1)) return doc; // filler

        quint32 extraLen = 0;
        if (!readU32(file, extraLen)) return doc;
        const qint64 extraStart = file.pos();
        const qint64 extraEnd   = extraStart + extraLen;

        // Layer mask data
        quint32 maskLen = 0;
        if (!readU32(file, maskLen) || !skipBytes(file, maskLen)) return doc;
        // Layer blending ranges
        quint32 blendLen = 0;
        if (!readU32(file, blendLen) || !skipBytes(file, blendLen)) return doc;

        // Pascal-padded layer name (pad = 4)
        QString pascalName;
        if (!readPascalString(file, 4, pascalName)) return doc;
        L.name = pascalName;

        // Additional layer information blocks
        while (file.pos() + 12 <= extraEnd) {
            char asig[4] = {};
            char akey[4] = {};
            quint32 aLen = 0;
            if (!readBytes(file, asig, 4) || !readBytes(file, akey, 4)
                || !readU32(file, aLen)) break;
            const qint64 aStart = file.pos();
            const QByteArray k(akey, 4);

            if (k == QByteArray("luni")) {
                QString uni;
                if (readUnicodeString(file, uni) && !uni.isEmpty()) {
                    L.name = uni;
                }
            } else if (k == QByteArray("lsct") || k == QByteArray("lsdk")) {
                // Section divider: first uint32 = type
                //   0 = any other type
                //   1 = open folder
                //   2 = closed folder
                //   3 = bounding section divider (close)
                quint32 stype = 0;
                if (readU32(file, stype)) {
                    L.sectionType = int(stype);
                }
            }
            // Always advance to declared end of this sub-block (with padding)
            qint64 padded = aStart + ((aLen + 1) & ~quint32(1));
            if (padded > extraEnd) padded = extraEnd;
            file.seek(padded);
        }

        // Skip any leftover bytes in the extra section
        file.seek(extraEnd);
    }

    // ---- per-layer channel data ----------------------------------------
    QVector<QHash<int, QByteArray>> layerPlanes(layerCount);
    for (int i = 0; i < layerCount; ++i) {
        const LayerRec& L = recs[i];
        const int w = std::max(0, int(L.right  - L.left));
        const int h = std::max(0, int(L.bottom - L.top));
        for (const auto& ch : L.channels) {
            const qint16 cid = ch.first;
            const quint32 cLen = ch.second;
            QByteArray plane = readChannelPlane(file, cLen, w, h);
            if (!plane.isEmpty()) {
                layerPlanes[i].insert(int(cid), plane);
            }
        }
    }

    // Snap to end of layer & mask info even if we did not consume global mask
    if (file.pos() < lmiEnd) file.seek(lmiEnd);

    // ---- Build PsdLayer list with group depth ---------------------------
    // PSD stores layers bottom-up; we emit them top-down (presentation order).
    // Walking top-down:
    //   - open-folder marker (lsct type 1 or 2) → enter a group, depth += 1
    //   - close-folder marker (lsct type 3)     → leave a group, depth -= 1
    // The group header itself sits at the child depth (depth AFTER increment),
    // because Affinity / Photoshop treat the open-folder record as the group's
    // visible header inside the parent.
    doc.layers.reserve(layerCount);
    int depthCounter = 0;
    for (int i = layerCount - 1; i >= 0; --i) {
        const LayerRec& L = recs[i];
        const int w = std::max(0, int(L.right  - L.left));
        const int h = std::max(0, int(L.bottom - L.top));

        if (L.sectionType == 1 || L.sectionType == 2) {
            depthCounter += 1;
        }

        PsdLayer out;
        out.name = L.name;
        out.blendMode = L.blendMode;
        out.opacity = int(L.opacity);
        out.visibility = ((L.flags & 0x02) == 0); // bit 1 set => hidden
        out.boundsRect = QRect(int(L.left), int(L.top), w, h);
        out.groupDepth = depthCounter;

        if (w > 0 && h > 0) {
            out.image = buildLayerImage(w, h, layerPlanes[i]);
        }
        doc.layers.append(out);

        if (L.sectionType == 3) {
            depthCounter = std::max(0, depthCounter - 1);
        }
    }

    return doc;
}

} // namespace psd
} // namespace affinity
