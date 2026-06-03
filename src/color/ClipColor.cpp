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
