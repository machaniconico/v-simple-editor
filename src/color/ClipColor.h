#pragma once
// ClipColor: per-clip color metadata model for HDR multi-layer plumbing.
//
// Pure QtCore-compatible value model. Rendering code must not consume this in
// Stage1; it is storage + pipeline plumbing only.

class QJsonObject;
class QString;

namespace clipcolor {

enum class Primaries { Rec709 = 0, Rec2020 = 1, DisplayP3 = 2, ACEScg = 3 };
enum class Transfer  { sRGB = 0, Rec709 = 1, PQ = 2, HLG = 3, Linear = 4 };

struct ColorMeta {
    Primaries primaries = Primaries::Rec709;
    Transfer  transfer  = Transfer::sRGB;
    int       bitDepth  = 8;
    bool      isHdr     = false;

    bool isDefault() const;
    bool operator==(const ColorMeta& o) const;
    bool operator!=(const ColorMeta& o) const { return !(*this == o); }
};

ColorMeta defaultSdr();
ColorMeta fromCodecParams(int avColorPrimaries, int avColorTrc, int bitDepth);

QString   primariesToken(Primaries p);
Primaries primariesFromToken(const QString& token,
                             Primaries fallback = Primaries::Rec709);
QString   transferToken(Transfer t);
Transfer  transferFromToken(const QString& token,
                            Transfer fallback = Transfer::sRGB);
QString   describe(const ColorMeta& meta);

QJsonObject toJson(const ColorMeta& meta);
ColorMeta   fromJson(const QJsonObject& obj);

} // namespace clipcolor
