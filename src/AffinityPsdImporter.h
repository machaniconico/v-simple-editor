#pragma once

// ---------------------------------------------------------------------------
// Sprint 16 / US-AFF-1 — Affinity Photo PSD layer-preserving importer
//
// namespace affinity::psd  — minimal PSD (Photoshop file format) reader that
//                            keeps individual layers, blend modes, opacity,
//                            visibility, bounds and group-folder depth so the
//                            v-simple-editor compositor can reconstruct an
//                            Affinity Photo / Photoshop document layer stack.
//
// Scope:
//   - 8-bit RGB / RGBA only
//   - Simple raster layers (no smart objects / adjustment layers / FX)
//   - Group folder depth via additional layer info 'lsct'
//   - Compression: raw (0) and PackBits RLE (1) only; ZIP (2/3) -> warn + empty
//
// All failures emit qWarning() and return a "best effort" PsdDocument; no
// exceptions are thrown.  Pure C++17 + Qt6, no external libpsd dependency.
// ---------------------------------------------------------------------------

#include <QImage>
#include <QList>
#include <QRect>
#include <QSize>
#include <QString>

namespace affinity {
namespace psd {

// Per-layer record extracted from the PSD layer & mask information section.
struct PsdLayer {
    QString name;                 // Unicode layer name (falls back to Pascal name)
    QString blendMode = "norm";   // 4-char PSD code: norm, mul, scrn, over, ...
    int opacity = 255;            // 0..255 (PSD layer opacity byte)
    bool visibility = true;       // !(flags & 0x02)
    QRect boundsRect;             // top/left/bottom/right rectangle in canvas px
    QImage image;                 // Composited layer pixels (Format_ARGB32)
    int groupDepth = 0;           // 0 = root; +1 per open folder, -1 per close
};

// Top-level document returned by loadPsd().
struct PsdDocument {
    QSize canvasSize = QSize(0, 0);
    QList<PsdLayer> layers;

    bool isValid() const { return canvasSize.isValid() && canvasSize.width() > 0; }
};

// Convert a 4-char PSD blend-mode code ("norm", "mul ", "scrn", ...) to a
// Qt-friendly display string ("Normal", "Multiply", "Screen", ...).  Unknown
// codes are returned trimmed and capitalised so callers never see an empty
// string.
QString psdBlendToString(const QString& psdCode);

// Parse a .psd file from disk.  Returns an empty (canvasSize=(0,0), no layers)
// PsdDocument when the signature is not "8BPS" or version != 1.  Never throws.
PsdDocument loadPsd(const QString& path);

// Selftest seam: bytes the most recent unicode layer name (luni) read actually
// allocated; 0 when that read rejected its count.  Not for production callers.
namespace psdtest {
qint64 lastLuniAllocatedBytes();
}

} // namespace psd
} // namespace affinity
