#include "AcesColor.h"

#include <algorithm>

namespace aces {

// ===========================================================================
// 行列プリミティブ
// ===========================================================================
Mat3 identity3()
{
    return Mat3{{ {1.0, 0.0, 0.0},
                  {0.0, 1.0, 0.0},
                  {0.0, 0.0, 1.0} }};
}

Mat3 multiply(const Mat3& a, const Mat3& b)
{
    Mat3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) sum += a[i][k] * b[k][j];
            r[i][j] = sum;
        }
    }
    return r;
}

Vec3 apply(const Mat3& m, const Vec3& v)
{
    Vec3 r{};
    for (int i = 0; i < 3; ++i) {
        r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    }
    return r;
}

Mat3 inverse(const Mat3& m)
{
    const double a = m[0][0], b = m[0][1], c = m[0][2];
    const double d = m[1][0], e = m[1][1], f = m[1][2];
    const double g = m[2][0], h = m[2][1], i = m[2][2];

    const double A =  (e * i - f * h);
    const double B = -(d * i - f * g);
    const double C =  (d * h - e * g);
    const double det = a * A + b * B + c * C;

    if (std::fabs(det) < 1e-12) {
        // 特異 (に近い) 行列は逆が定義できない。安全側として単位行列を返す。
        return identity3();
    }
    const double invDet = 1.0 / det;

    Mat3 r{};
    r[0][0] = A * invDet;
    r[0][1] = -(b * i - c * h) * invDet;
    r[0][2] =  (b * f - c * e) * invDet;
    r[1][0] = B * invDet;
    r[1][1] =  (a * i - c * g) * invDet;
    r[1][2] = -(a * f - c * d) * invDet;
    r[2][0] = C * invDet;
    r[2][1] = -(a * h - b * g) * invDet;
    r[2][2] =  (a * e - b * d) * invDet;
    return r;
}

// ===========================================================================
// 色空間ごとの RGB->XYZ 行列 (公開プライマリ + 白色点から導出, 8 桁精度)
//
// sRGB / Rec709 / LinearSRGB : ITU-R BT.709 primaries, D65
// Rec2020                     : ITU-R BT.2020 primaries, D65
// DisplayP3                   : DCI-P3 (SMPTE RP 431-2) primaries, D65
// ACEScg                      : Academy AP1 primaries, ACES D60
// ACES2065_1                  : Academy AP0 primaries, ACES D60
// ===========================================================================
namespace {

// ITU-R BT.709 / sRGB (D65) RGB->XYZ。
const Mat3 kRec709ToXyz = {{
    { 0.41239080, 0.35758434, 0.18048079 },
    { 0.21263901, 0.71516868, 0.07219232 },
    { 0.01933082, 0.11919478, 0.95053215 }
}};

// ITU-R BT.2020 (D65) RGB->XYZ。
const Mat3 kRec2020ToXyz = {{
    { 0.63695805, 0.14461690, 0.16888098 },
    { 0.26270021, 0.67799807, 0.05930172 },
    { 0.00000000, 0.02807269, 1.06098506 }
}};

// DCI-P3 / Display P3 (D65) RGB->XYZ。
const Mat3 kDisplayP3ToXyz = {{
    { 0.48657095, 0.26566769, 0.19821729 },
    { 0.22897456, 0.69173852, 0.07928691 },
    { 0.00000000, 0.04511338, 1.04394437 }
}};

// Academy AP1 (ACEScg, D60) RGB->XYZ。
const Mat3 kAcesCgToXyz = {{
    {  0.66245418, 0.13400421, 0.15618769 },
    {  0.27222872, 0.67408177, 0.05368952 },
    { -0.00557465, 0.00406073, 1.01033910 }
}};

// Academy AP0 (ACES2065-1, D60) RGB->XYZ。
const Mat3 kAces2065ToXyz = {{
    { 0.95255240, 0.00000000,  0.00009368 },
    { 0.34396645, 0.72816610, -0.07213255 },
    { 0.00000000, 0.00000000,  1.00882518 }
}};

// 白色点 (CIE XYZ, Y=1 正規化)。
//   D65: x=0.3127,   y=0.3290
//   D60: x=0.32168,  y=0.33767  (ACES 白)
const Vec3 kWhiteD65 = { 0.95045593, 1.00000000, 1.08905775 };
const Vec3 kWhiteD60 = { 0.95264608, 1.00000000, 1.00882518 };

bool isD60(ColorSpace cs)
{
    return cs == ColorSpace::ACEScg || cs == ColorSpace::ACES2065_1;
}

Vec3 whitePointOf(ColorSpace cs)
{
    return isD60(cs) ? kWhiteD60 : kWhiteD65;
}

} // namespace

Mat3 rgbToXyz(ColorSpace cs)
{
    switch (cs) {
    case ColorSpace::sRGB:
    case ColorSpace::Rec709:
    case ColorSpace::LinearSRGB: return kRec709ToXyz;
    case ColorSpace::Rec2020:    return kRec2020ToXyz;
    case ColorSpace::DisplayP3:  return kDisplayP3ToXyz;
    case ColorSpace::ACEScg:     return kAcesCgToXyz;
    case ColorSpace::ACES2065_1: return kAces2065ToXyz;
    }
    return kRec709ToXyz;
}

Mat3 xyzToRgb(ColorSpace cs)
{
    return inverse(rgbToXyz(cs));
}

// ===========================================================================
// Bradford 色順応
// ===========================================================================
namespace {

// Bradford LMS 円錐応答行列 (CIECAM 系で標準的に使われる published 値)。
const Mat3 kBradford = {{
    {  0.8951000,  0.2664000, -0.1614000 },
    { -0.7502000,  1.7135000,  0.0367000 },
    {  0.0389000, -0.0685000,  1.0296000 }
}};

} // namespace

Mat3 bradfordAdaptation(const Vec3& srcWhiteXyz, const Vec3& dstWhiteXyz)
{
    const Mat3 invBradford = inverse(kBradford);

    // 各白色点を LMS 円錐応答に変換。
    const Vec3 srcLms = apply(kBradford, srcWhiteXyz);
    const Vec3 dstLms = apply(kBradford, dstWhiteXyz);

    // LMS 空間での対角ゲイン (dst/src)。
    Mat3 gain = {{
        { (srcLms[0] != 0.0 ? dstLms[0] / srcLms[0] : 1.0), 0.0, 0.0 },
        { 0.0, (srcLms[1] != 0.0 ? dstLms[1] / srcLms[1] : 1.0), 0.0 },
        { 0.0, 0.0, (srcLms[2] != 0.0 ? dstLms[2] / srcLms[2] : 1.0) }
    }};

    // M = Bradford^-1 * gain * Bradford。XYZ(src 光源) -> XYZ(dst 光源)。
    return multiply(invBradford, multiply(gain, kBradford));
}

// ===========================================================================
// 線形 RGB(from) -> RGB(to) 合成行列
// ===========================================================================
Mat3 conversionMatrix(ColorSpace from, ColorSpace to)
{
    if (from == to) return identity3();

    Mat3 m = rgbToXyz(from);  // from RGB -> XYZ(from 白色点)

    const Vec3 srcWhite = whitePointOf(from);
    const Vec3 dstWhite = whitePointOf(to);
    // 白色点が異なる (D60 vs D65) なら Bradford で順応する。
    const bool whiteDiffers =
        std::fabs(srcWhite[0] - dstWhite[0]) > 1e-9 ||
        std::fabs(srcWhite[1] - dstWhite[1]) > 1e-9 ||
        std::fabs(srcWhite[2] - dstWhite[2]) > 1e-9;
    if (whiteDiffers) {
        m = multiply(bradfordAdaptation(srcWhite, dstWhite), m);
    }

    m = multiply(xyzToRgb(to), m);  // XYZ(to 白色点) -> to RGB
    return m;
}

// ===========================================================================
// 転送関数
// ===========================================================================
namespace {

// IEC 61966-2-1 sRGB カーブ (Display P3 / Rec.709 / Rec.2020 も同等近似で共有)。
double srgbOetf(double linear)
{
    if (linear <= 0.0031308) return 12.92 * linear;
    return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

double srgbEotf(double encoded)
{
    if (encoded <= 0.04045) return encoded / 12.92;
    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

bool isLinearSpace(ColorSpace cs)
{
    return cs == ColorSpace::LinearSRGB
        || cs == ColorSpace::ACEScg
        || cs == ColorSpace::ACES2065_1;
}

} // namespace

double oetf(ColorSpace cs, double linear)
{
    if (isLinearSpace(cs)) return linear;  // 線形空間は恒等
    return srgbOetf(linear);
}

double eotf(ColorSpace cs, double encoded)
{
    if (isLinearSpace(cs)) return encoded;  // 線形空間は恒等
    return srgbEotf(encoded);
}

// ===========================================================================
// IDT / RRT+ODT / process
// ===========================================================================
namespace {

// ACES filmic トーンマップ近似 (Narkowicz の RRT+ODT フィット, ハイライト圧縮)。
// 入力線形 -> 出力線形 (display referred 近似)。
double acesFilmicTonemap(double x)
{
    if (x < 0.0) x = 0.0;
    const double a = 2.51;
    const double b = 0.03;
    const double c = 2.43;
    const double d = 0.59;
    const double e = 0.14;
    const double num = x * (a * x + b);
    const double den = x * (c * x + d) + e;
    if (den <= 0.0) return 0.0;
    return num / den;
}

} // namespace

Vec3 idt(ColorSpace inputSpace, const Vec3& encodedInput)
{
    // 1) 符号値 -> 線形化。
    const Vec3 lin = {
        eotf(inputSpace, encodedInput[0]),
        eotf(inputSpace, encodedInput[1]),
        eotf(inputSpace, encodedInput[2])
    };
    // 2) 線形 RGB(input) -> 線形 RGB(ACEScg, 作業色)。
    const Mat3 toWorking = conversionMatrix(inputSpace, ColorSpace::ACEScg);
    return apply(toWorking, lin);
}

Vec3 rrtOdt(const Vec3& acescgLinear, ColorSpace outputSpace)
{
    // 1) トーンマップ (ハイライト圧縮) を ACEScg 線形に適用。
    const Vec3 tm = {
        acesFilmicTonemap(acescgLinear[0]),
        acesFilmicTonemap(acescgLinear[1]),
        acesFilmicTonemap(acescgLinear[2])
    };
    // 2) ACEScg 線形 -> 出力色のプライマリ (線形)。
    const Mat3 toOutput = conversionMatrix(ColorSpace::ACEScg, outputSpace);
    const Vec3 outLin = apply(toOutput, tm);
    // 3) oetf で符号化。
    return {
        oetf(outputSpace, outLin[0]),
        oetf(outputSpace, outLin[1]),
        oetf(outputSpace, outLin[2])
    };
}

Vec3 process(const AcesPipeline& p, const Vec3& encodedInput)
{
    if (!p.enabled) return encodedInput;  // identity
    // working は ACEScg 前提 (idt が ACEScg を返す)。
    const Vec3 working = idt(p.input, encodedInput);
    return rrtOdt(working, p.output);
}

// ===========================================================================
// JSON / 名前
// ===========================================================================
QString colorSpaceName(ColorSpace cs)
{
    switch (cs) {
    case ColorSpace::sRGB:       return QStringLiteral("sRGB");
    case ColorSpace::Rec709:     return QStringLiteral("Rec709");
    case ColorSpace::Rec2020:    return QStringLiteral("Rec2020");
    case ColorSpace::DisplayP3:  return QStringLiteral("DisplayP3");
    case ColorSpace::ACEScg:     return QStringLiteral("ACEScg");
    case ColorSpace::ACES2065_1: return QStringLiteral("ACES2065-1");
    case ColorSpace::LinearSRGB: return QStringLiteral("LinearSRGB");
    }
    return QStringLiteral("sRGB");
}

ColorSpace colorSpaceFromName(const QString& name)
{
    if (name == QStringLiteral("Rec709"))     return ColorSpace::Rec709;
    if (name == QStringLiteral("Rec2020"))    return ColorSpace::Rec2020;
    if (name == QStringLiteral("DisplayP3"))  return ColorSpace::DisplayP3;
    if (name == QStringLiteral("ACEScg"))     return ColorSpace::ACEScg;
    if (name == QStringLiteral("ACES2065-1")) return ColorSpace::ACES2065_1;
    if (name == QStringLiteral("LinearSRGB")) return ColorSpace::LinearSRGB;
    return ColorSpace::sRGB;
}

QJsonObject pipelineToJson(const AcesPipeline& p)
{
    QJsonObject o;
    o[QStringLiteral("enabled")] = p.enabled;
    o[QStringLiteral("input")]   = colorSpaceName(p.input);
    o[QStringLiteral("working")] = colorSpaceName(p.working);
    o[QStringLiteral("output")]  = colorSpaceName(p.output);
    return o;
}

AcesPipeline pipelineFromJson(const QJsonObject& obj)
{
    AcesPipeline p;
    p.enabled = obj.value(QStringLiteral("enabled")).toBool(false);
    p.input   = colorSpaceFromName(obj.value(QStringLiteral("input")).toString(QStringLiteral("sRGB")));
    p.working = colorSpaceFromName(obj.value(QStringLiteral("working")).toString(QStringLiteral("ACEScg")));
    p.output  = colorSpaceFromName(obj.value(QStringLiteral("output")).toString(QStringLiteral("Rec709")));
    return p;
}

} // namespace aces
