// ClipColor: per-clip color metadata model implementation.
// Storage/plumbing only; no rendering consumer belongs in Stage1.

#include "ClipColor.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace clipcolor {
namespace {

QString primariesLabel(Primaries p)
{
    switch (p) {
        case Primaries::Rec709:    return QStringLiteral("Rec.709");
        case Primaries::Rec2020:   return QStringLiteral("Rec.2020");
        case Primaries::DisplayP3: return QStringLiteral("Display P3");
        case Primaries::ACEScg:    return QStringLiteral("ACEScg");
        default:                   return QStringLiteral("Rec.709");
    }
}

QString transferLabel(Transfer t)
{
    switch (t) {
        case Transfer::sRGB:   return QStringLiteral("sRGB");
        case Transfer::Rec709: return QStringLiteral("Rec.709");
        case Transfer::PQ:     return QStringLiteral("PQ");
        case Transfer::HLG:    return QStringLiteral("HLG");
        case Transfer::Linear: return QStringLiteral("Linear");
        default:               return QStringLiteral("sRGB");
    }
}

} // namespace

ColorMeta defaultSdr()
{
    return {};
}

ColorMeta fromCodecParams(int avColorPrimaries, int avColorTrc, int bitDepth,
                          bool hasHdrMetadata)
{
    constexpr int kAvcolPriBt709 = 1;       // AVCOL_PRI_BT709 = 1
    constexpr int kAvcolPriUnspecified = 2; // AVCOL_PRI_UNSPECIFIED = 2
    constexpr int kAvcolPriBt2020 = 9;      // AVCOL_PRI_BT2020 = 9
    constexpr int kAvcolPriSmpte432 = 12;   // AVCOL_PRI_SMPTE432 = 12 (Display P3)

    constexpr int kAvcolTrcBt709 = 1;         // AVCOL_TRC_BT709 = 1
    constexpr int kAvcolTrcUnspecified = 2;   // AVCOL_TRC_UNSPECIFIED = 2
    constexpr int kAvcolTrcIec61966_2_1 = 13; // AVCOL_TRC_IEC61966_2_1 = 13 (sRGB)
    constexpr int kAvcolTrcSmpte2084 = 16;    // AVCOL_TRC_SMPTE2084 = 16 (PQ)
    constexpr int kAvcolTrcAribStdB67 = 18;   // AVCOL_TRC_ARIB_STD_B67 = 18 (HLG)

    ColorMeta meta = defaultSdr();

    switch (avColorPrimaries) {
        case kAvcolPriBt709:
            meta.primaries = Primaries::Rec709;
            break;
        case kAvcolPriBt2020:
            meta.primaries = Primaries::Rec2020;
            break;
        case kAvcolPriSmpte432:
            meta.primaries = Primaries::DisplayP3;
            break;
        case kAvcolPriUnspecified:
        default:
            meta.primaries = Primaries::Rec709;
            break;
    }

    switch (avColorTrc) {
        case kAvcolTrcSmpte2084:
            meta.transfer = Transfer::PQ;
            meta.isHdr = true;
            break;
        case kAvcolTrcAribStdB67:
            meta.transfer = Transfer::HLG;
            meta.isHdr = true;
            break;
        case kAvcolTrcBt709:
        case kAvcolTrcIec61966_2_1:
            meta.transfer = Transfer::sRGB;
            meta.isHdr = false;
            break;
        case kAvcolTrcUnspecified:
        default:
            meta.transfer = hasHdrMetadata ? Transfer::PQ : Transfer::sRGB;
            meta.isHdr = hasHdrMetadata;
            break;
    }

    meta.bitDepth = bitDepth < 8 ? 8 : bitDepth;
    return meta;
}

bool ColorMeta::isDefault() const
{
    return *this == defaultSdr();
}

bool ColorMeta::operator==(const ColorMeta& o) const
{
    return primaries == o.primaries
        && transfer == o.transfer
        && bitDepth == o.bitDepth
        && isHdr == o.isHdr;
}

QString primariesToken(Primaries p)
{
    switch (p) {
        case Primaries::Rec709:    return QStringLiteral("rec709");
        case Primaries::Rec2020:   return QStringLiteral("rec2020");
        case Primaries::DisplayP3: return QStringLiteral("p3");
        case Primaries::ACEScg:    return QStringLiteral("acescg");
        default:                   return QStringLiteral("rec709");
    }
}

Primaries primariesFromToken(const QString& token, Primaries fallback)
{
    if (token == QStringLiteral("rec709"))
        return Primaries::Rec709;
    if (token == QStringLiteral("rec2020"))
        return Primaries::Rec2020;
    if (token == QStringLiteral("p3"))
        return Primaries::DisplayP3;
    if (token == QStringLiteral("acescg"))
        return Primaries::ACEScg;
    return fallback;
}

QString transferToken(Transfer t)
{
    switch (t) {
        case Transfer::sRGB:   return QStringLiteral("srgb");
        case Transfer::Rec709: return QStringLiteral("rec709");
        case Transfer::PQ:     return QStringLiteral("pq");
        case Transfer::HLG:    return QStringLiteral("hlg");
        case Transfer::Linear: return QStringLiteral("linear");
        default:               return QStringLiteral("srgb");
    }
}

Transfer transferFromToken(const QString& token, Transfer fallback)
{
    if (token == QStringLiteral("srgb"))
        return Transfer::sRGB;
    if (token == QStringLiteral("rec709"))
        return Transfer::Rec709;
    if (token == QStringLiteral("pq"))
        return Transfer::PQ;
    if (token == QStringLiteral("hlg"))
        return Transfer::HLG;
    if (token == QStringLiteral("linear"))
        return Transfer::Linear;
    return fallback;
}

QString describe(const ColorMeta& meta)
{
    return QStringLiteral("%1 / %2 / %3-bit %4")
        .arg(primariesLabel(meta.primaries),
             transferLabel(meta.transfer),
             QString::number(meta.bitDepth),
             meta.isHdr ? QStringLiteral("HDR") : QStringLiteral("SDR"));
}

QJsonObject toJson(const ColorMeta& meta)
{
    QJsonObject obj;
    obj[QStringLiteral("primaries")] = primariesToken(meta.primaries);
    obj[QStringLiteral("transfer")] = transferToken(meta.transfer);
    obj[QStringLiteral("bitDepth")] = meta.bitDepth;
    obj[QStringLiteral("isHdr")] = meta.isHdr;
    return obj;
}

ColorMeta fromJson(const QJsonObject& obj)
{
    ColorMeta meta = defaultSdr();
    if (obj.contains(QStringLiteral("primaries"))) {
        meta.primaries = primariesFromToken(
            obj.value(QStringLiteral("primaries")).toString(), meta.primaries);
    }
    if (obj.contains(QStringLiteral("transfer"))) {
        meta.transfer = transferFromToken(
            obj.value(QStringLiteral("transfer")).toString(), meta.transfer);
    }
    if (obj.contains(QStringLiteral("bitDepth")))
        meta.bitDepth = obj.value(QStringLiteral("bitDepth")).toInt(meta.bitDepth);
    if (obj.contains(QStringLiteral("isHdr")))
        meta.isHdr = obj.value(QStringLiteral("isHdr")).toBool(meta.isHdr);
    return meta;
}

} // namespace clipcolor
