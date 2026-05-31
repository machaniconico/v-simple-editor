// AscCdlExport: ASC CDL (Color Decision List) 書き出しの純粋エンジン実装。
// 出力規格・マッピング規約は src/colorexport/AscCdlExport.h を参照。

#include "AscCdlExport.h"

#include <algorithm>
#include <cmath>

#include <QXmlStreamWriter>

namespace asccdl {

namespace {

constexpr char kCdlNamespace[] = "urn:ASC:CDL:v1.01";
constexpr double kLiftUniformScale = 0.5;
constexpr double kGammaDenominatorFloor = 1e-3;
constexpr double kGainExponentScale = 2.0;
constexpr double kGainShaderCap = 16.0;

// 数値を小数 6 桁固定で整形する (ロケール非依存・決定的)。
QString fmt(double v)
{
    return QString::number(v, 'f', 6);
}

// 3 要素トリプルを半角スペース 1 個区切りの文字列にする (例: "1.000000 1.000000 1.000000")。
QString triple(const double v[3])
{
    return fmt(v[0]) + QLatin1Char(' ') + fmt(v[1]) + QLatin1Char(' ') + fmt(v[2]);
}

double previewGainMultiplier(double gainRgb, double gainLuma)
{
    const double exponent = kGainExponentScale * (gainRgb + gainLuma);
    if (!std::isfinite(exponent))
        return exponent > 0.0 ? kGainShaderCap : 0.0;
    if (exponent >= 4.0)
        return kGainShaderCap;

    const double value = std::pow(2.0, exponent);
    if (!std::isfinite(value))
        return kGainShaderCap;
    return std::clamp(value, 0.0, kGainShaderCap);
}

double previewPower(double gammaRgb, double gammaLuma)
{
    const double denominator = gammaRgb * gammaLuma;
    if (!std::isfinite(denominator) || denominator < kGammaDenominatorFloor)
        return 1.0 / kGammaDenominatorFloor;
    return 1.0 / denominator;
}

// 1 つの <ColorCorrection> 要素を w に書き出す。start/end は呼び出し側では行わずここで完結する。
void writeColorCorrection(QXmlStreamWriter& w, const CdlCorrection& c)
{
    w.writeStartElement(QStringLiteral("ColorCorrection"));
    if (!c.id.isEmpty())
        w.writeAttribute(QStringLiteral("id"), c.id);

    w.writeStartElement(QStringLiteral("SOPNode"));
    w.writeTextElement(QStringLiteral("Slope"),  triple(c.slope));
    w.writeTextElement(QStringLiteral("Offset"), triple(c.offset));
    w.writeTextElement(QStringLiteral("Power"),  triple(c.power));
    w.writeEndElement(); // SOPNode

    w.writeStartElement(QStringLiteral("SatNode"));
    w.writeTextElement(QStringLiteral("Saturation"), fmt(c.saturation));
    w.writeEndElement(); // SatNode

    w.writeEndElement(); // ColorCorrection
}

// XML 宣言 + 自動整形ありで初期化した QXmlStreamWriter を生成するヘルパ。
struct XmlBuilder {
    QString          out;
    QXmlStreamWriter w;

    XmlBuilder() : w(&out) {
        w.setAutoFormatting(true);
        w.writeStartDocument(QStringLiteral("1.0"));
    }
    QString finish() {
        w.writeEndDocument();
        return out;
    }
};

} // namespace

CdlCorrection fromLgg(const double lift[3],  double liftLuma,
                      const double gamma[3], double gammaLuma,
                      const double gain[3],  double gainLuma,
                      double saturation, const QString& id)
{
    CdlCorrection c;
    c.id = id;
    for (int i = 0; i < 3; ++i) {
        c.slope[i]  = previewGainMultiplier(gain[i], gainLuma);
        c.offset[i] = kLiftUniformScale * (lift[i] + liftLuma);
        c.power[i]  = previewPower(gamma[i], gammaLuma);
    }
    c.saturation = std::max(0.0, saturation);
    return c;
}

QString buildCc(const CdlCorrection& c)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("ColorCorrection"));
    w.writeDefaultNamespace(QString::fromLatin1(kCdlNamespace));
    if (!c.id.isEmpty())
        w.writeAttribute(QStringLiteral("id"), c.id);

    w.writeStartElement(QStringLiteral("SOPNode"));
    w.writeTextElement(QStringLiteral("Slope"),  triple(c.slope));
    w.writeTextElement(QStringLiteral("Offset"), triple(c.offset));
    w.writeTextElement(QStringLiteral("Power"),  triple(c.power));
    w.writeEndElement(); // SOPNode

    w.writeStartElement(QStringLiteral("SatNode"));
    w.writeTextElement(QStringLiteral("Saturation"), fmt(c.saturation));
    w.writeEndElement(); // SatNode

    w.writeEndElement(); // ColorCorrection
    return b.finish();
}

QString buildCcc(const QVector<CdlCorrection>& list)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("ColorCorrectionCollection"));
    w.writeDefaultNamespace(QString::fromLatin1(kCdlNamespace));
    for (const CdlCorrection& c : list)
        writeColorCorrection(w, c);
    w.writeEndElement(); // ColorCorrectionCollection
    return b.finish();
}

QString buildCdl(const QVector<CdlCorrection>& list)
{
    XmlBuilder b;
    QXmlStreamWriter& w = b.w;
    w.writeStartElement(QStringLiteral("ColorDecisionList"));
    w.writeDefaultNamespace(QString::fromLatin1(kCdlNamespace));
    for (const CdlCorrection& c : list) {
        w.writeStartElement(QStringLiteral("ColorDecision"));
        writeColorCorrection(w, c);
        w.writeEndElement(); // ColorDecision
    }
    w.writeEndElement(); // ColorDecisionList
    return b.finish();
}

} // namespace asccdl
