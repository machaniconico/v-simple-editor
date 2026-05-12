#include "NodeLibrary.h"

#include "FractalNoise.h"
#include "ParticleSystem.h"

#include <QImage>
#include <QPainter>
#include <QTransform>
#include <QtMath>

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace nodelib {

NodeRegistry& NodeRegistry::instance()
{
    static NodeRegistry inst;
    return inst;
}

void NodeRegistry::registerType(const NodeTypeDescriptor& descriptor)
{
    m_types.insert(descriptor.typeName, descriptor);
}

const NodeTypeDescriptor* NodeRegistry::descriptor(const QString& typeName) const
{
    auto it = m_types.constFind(typeName);
    if (it == m_types.constEnd()) {
        return nullptr;
    }
    return &(*it);
}

QStringList NodeRegistry::allTypeNames() const
{
    return m_types.keys();
}

static QImage makePlaceholderImage(QSize size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(128, 128, 128));
    img.setText("nodelib::ImageInput", "placeholder");
    return img;
}

static QImage evaluateSolidColor(const QVariantMap& params, QSize outputSize)
{
    if (outputSize.isEmpty()) {
        return QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
    }

    QColor color = params.value(QStringLiteral("color"), QColor(0, 0, 0)).value<QColor>();
    QImage img(outputSize, QImage::Format_ARGB32_Premultiplied);
    img.fill(color);
    return img;
}

static QImage evaluateGradient(const QVariantMap& params, QSize outputSize)
{
    if (outputSize.isEmpty()) {
        return QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
    }

    QColor color1 = params.value(QStringLiteral("color1"), QColor(0, 0, 0)).value<QColor>();
    QColor color2 = params.value(QStringLiteral("color2"), QColor(255, 255, 255)).value<QColor>();
    double angleDeg = params.value(QStringLiteral("angle"), 0.0).toDouble();

    double angleRad = angleDeg * M_PI / 180.0;
    double cx = outputSize.width() / 2.0;
    double cy = outputSize.height() / 2.0;
    double dx = std::cos(angleRad);
    double dy = std::sin(angleRad);

    QPointF start(cx - dx * cx, cy - dy * cy);
    QPointF end(cx + dx * cx, cy + dy * cy);

    QImage img(outputSize, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&img);
    QLinearGradient gradient(start, end);
    gradient.setColorAt(0.0, color1);
    gradient.setColorAt(1.0, color2);
    painter.fillRect(0, 0, outputSize.width(), outputSize.height(), gradient);
    painter.end();
    return img;
}

static QImage evaluateTransform(const GraphNode& node, const QVector<QVariant>& inputs, QSize outputSize)
{
    if (outputSize.isEmpty()) {
        return QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
    }

    QImage inputImage;
    if (!inputs.isEmpty() && inputs[0].canConvert<QImage>()) {
        inputImage = inputs[0].value<QImage>();
    }

    if (inputImage.isNull()) {
        return QImage(outputSize, QImage::Format_ARGB32_Premultiplied);
    }

    double scaleX = node.params.value(QStringLiteral("scaleX"), 1.0).toDouble();
    double scaleY = node.params.value(QStringLiteral("scaleY"), 1.0).toDouble();
    double rotationDeg = node.params.value(QStringLiteral("rotationDeg"), 0.0).toDouble();
    double translateX = node.params.value(QStringLiteral("translateX"), 0.0).toDouble();
    double translateY = node.params.value(QStringLiteral("translateY"), 0.0).toDouble();
    double opacity = node.params.value(QStringLiteral("opacity"), 1.0).toDouble();

    QTransform transform;
    transform.translate(outputSize.width() / 2.0 + translateX, outputSize.height() / 2.0 + translateY);
    transform.rotate(rotationDeg);
    transform.scale(scaleX, scaleY);
    transform.translate(-inputImage.width() / 2.0, -inputImage.height() / 2.0);

    QImage result(outputSize, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setTransform(transform);
    painter.setOpacity(opacity);
    painter.drawImage(0, 0, inputImage);
    painter.end();

    return result;
}

static int clampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double clampDbl(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static QImage evaluateBrightnessContrast(const GraphNode& node, const QVector<QVariant>& inputs, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);
    QImage inputImage;
    if (!inputs.isEmpty() && inputs[0].canConvert<QImage>()) inputImage = inputs[0].value<QImage>();
    if (inputImage.isNull()) return QImage(outputSize, QImage::Format_ARGB32_Premultiplied);

    QImage src = inputImage.convertToFormat(QImage::Format_ARGB32);
    QImage result(src.size(), QImage::Format_ARGB32);

    double brightness = node.params.value(QStringLiteral("brightness"), 0.0).toDouble();
    double contrast = node.params.value(QStringLiteral("contrast"), 0.0).toDouble();
    double factor = (259.0 * (contrast + 255.0)) / (255.0 * (259.0 - contrast));

    for (int y = 0; y < src.height(); ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            QRgb px = srcLine[x];
            auto adjust = [&](int c) -> int {
                double v = c + brightness * 2.55;
                v = factor * (v - 128.0) + 128.0;
                return clampInt(qRound(v), 0, 255);
            };
            dstLine[x] = qRgba(adjust(qRed(px)), adjust(qGreen(px)), adjust(qBlue(px)), qAlpha(px));
        }
    }
    return result;
}

static void rgbToHsl(double r, double g, double b, double &h, double &s, double &l)
{
    double max = r, min = r;
    if (g > max) max = g; if (b > max) max = b;
    if (g < min) min = g; if (b < min) min = b;
    l = (max + min) / 2.0;
    if (max == min) { h = s = 0.0; return; }
    double d = max - min;
    s = (l > 0.5) ? d / (2.0 - max - min) : d / (max + min);
    if (max == r) h = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (max == g) h = (b - r) / d + 2.0;
    else h = (r - g) / d + 4.0;
    h /= 6.0;
}

static double hue2rgb(double p, double q, double t)
{
    if (t < 0) t += 1.0;
    if (t > 1) t -= 1.0;
    if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0/2.0) return q;
    if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
    return p;
}

static void hslToRgb(double h, double s, double l, double &r, double &g, double &b)
{
    if (s == 0.0) { r = g = b = l; return; }
    double q = (l < 0.5) ? l * (1.0 + s) : l + s - l * s;
    double p = 2.0 * l - q;
    r = hue2rgb(p, q, h + 1.0/3.0);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.0/3.0);
}

static QImage evaluateHueSaturation(const GraphNode& node, const QVector<QVariant>& inputs, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);
    QImage inputImage;
    if (!inputs.isEmpty() && inputs[0].canConvert<QImage>()) inputImage = inputs[0].value<QImage>();
    if (inputImage.isNull()) return QImage(outputSize, QImage::Format_ARGB32_Premultiplied);

    QImage src = inputImage.convertToFormat(QImage::Format_ARGB32);
    QImage result(src.size(), QImage::Format_ARGB32);

    double hueDeg = node.params.value(QStringLiteral("hueDeg"), 0.0).toDouble();
    double satShift = node.params.value(QStringLiteral("saturation"), 0.0).toDouble();
    double lightShift = node.params.value(QStringLiteral("lightness"), 0.0).toDouble();

    for (int y = 0; y < src.height(); ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            QRgb px = srcLine[x];
            double r = qRed(px) / 255.0, g = qGreen(px) / 255.0, b = qBlue(px) / 255.0;
            double h, s, l;
            rgbToHsl(r, g, b, h, s, l);
            h = fmod(h + hueDeg / 360.0 + 1.0, 1.0);
            if (h < 0) h += 1.0;
            s = clampDbl(s + satShift / 100.0, 0.0, 1.0);
            l = clampDbl(l + lightShift / 100.0, 0.0, 1.0);
            double nr, ng, nb;
            hslToRgb(h, s, l, nr, ng, nb);
            dstLine[x] = qRgba(clampInt(qRound(nr * 255), 0, 255),
                               clampInt(qRound(ng * 255), 0, 255),
                               clampInt(qRound(nb * 255), 0, 255),
                               qAlpha(px));
        }
    }
    return result;
}

static QImage evaluateColorBalance(const GraphNode& node, const QVector<QVariant>& inputs, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);
    QImage inputImage;
    if (!inputs.isEmpty() && inputs[0].canConvert<QImage>()) inputImage = inputs[0].value<QImage>();
    if (inputImage.isNull()) return QImage(outputSize, QImage::Format_ARGB32_Premultiplied);

    QImage src = inputImage.convertToFormat(QImage::Format_ARGB32);
    QImage result(src.size(), QImage::Format_ARGB32);

    QColor liftColor = node.params.value(QStringLiteral("liftColor"), QColor(0, 0, 0)).value<QColor>();
    QColor gammaColor = node.params.value(QStringLiteral("gammaColor"), QColor(128, 128, 128)).value<QColor>();
    QColor gainColor = node.params.value(QStringLiteral("gainColor"), QColor(255, 255, 255)).value<QColor>();

    double liftR = (liftColor.red() - 128) / 128.0;
    double liftG = (liftColor.green() - 128) / 128.0;
    double liftB = (liftColor.blue() - 128) / 128.0;
    double gammaR = gammaColor.red() / 128.0;
    double gammaG = gammaColor.green() / 128.0;
    double gammaB = gammaColor.blue() / 128.0;
    double gainR = gainColor.red() / 255.0;
    double gainG = gainColor.green() / 255.0;
    double gainB = gainColor.blue() / 255.0;

    auto apply = [&](int c, double l, double g, double gn) -> int {
        double v = c / 255.0;
        double shadow = 1.0 - v;
        v = v + l * shadow * shadow;
        v = std::pow(v, 1.0 / g);
        v = v * gn;
        return clampInt(qRound(v * 255), 0, 255);
    };

    for (int y = 0; y < src.height(); ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            QRgb px = srcLine[x];
            dstLine[x] = qRgba(apply(qRed(px), liftR, gammaR, gainR),
                               apply(qGreen(px), liftG, gammaG, gainG),
                               apply(qBlue(px), liftB, gammaB, gainB),
                               qAlpha(px));
        }
    }
    return result;
}

static QImage evaluateGaussianBlur(const GraphNode& node, const QVector<QVariant>& inputs, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);
    QImage inputImage;
    if (!inputs.isEmpty() && inputs[0].canConvert<QImage>()) inputImage = inputs[0].value<QImage>();
    if (inputImage.isNull()) return QImage(outputSize, QImage::Format_ARGB32_Premultiplied);

    int radius = node.params.value(QStringLiteral("radius"), 0.0).toInt();
    if (radius <= 0) return inputImage.convertToFormat(QImage::Format_ARGB32);

    QImage src = inputImage.convertToFormat(QImage::Format_ARGB32);
    int w = src.width(), h = src.height();
    int kernelSize = radius * 2 + 1;
    QVector<double> kernel(kernelSize);
    double sigma = radius / 2.5;
    if (sigma < 0.5) sigma = 0.5;
    double sum = 0.0;
    for (int i = 0; i < kernelSize; ++i) {
        double x = i - radius;
        kernel[i] = std::exp(-(x * x) / (2.0 * sigma * sigma));
        sum += kernel[i];
    }
    for (int i = 0; i < kernelSize; ++i) kernel[i] /= sum;

    QImage tmp(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int k = 0; k < kernelSize; ++k) {
                int sx = clampInt(x + k - radius, 0, w - 1);
                QRgb px = src.pixel(sx, y);
                double wgt = kernel[k];
                r += qRed(px) * wgt;
                g += qGreen(px) * wgt;
                b += qBlue(px) * wgt;
                a += qAlpha(px) * wgt;
            }
            tmp.setPixel(x, y, qRgba(clampInt(qRound(r),0,255), clampInt(qRound(g),0,255),
                                     clampInt(qRound(b),0,255), clampInt(qRound(a),0,255)));
        }
    }

    QImage result(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int k = 0; k < kernelSize; ++k) {
                int sy = clampInt(y + k - radius, 0, h - 1);
                QRgb px = tmp.pixel(x, sy);
                double wgt = kernel[k];
                r += qRed(px) * wgt;
                g += qGreen(px) * wgt;
                b += qBlue(px) * wgt;
                a += qAlpha(px) * wgt;
            }
            result.setPixel(x, y, qRgba(clampInt(qRound(r),0,255), clampInt(qRound(g),0,255),
                                        clampInt(qRound(b),0,255), clampInt(qRound(a),0,255)));
        }
    }
    return result;
}

static QImage evaluateBlend(const GraphNode& node, const QVector<QVariant>& inputs, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);

    QImage imgA, imgB;
    if (inputs.size() >= 1 && inputs[0].canConvert<QImage>()) imgA = inputs[0].value<QImage>();
    if (inputs.size() >= 2 && inputs[1].canConvert<QImage>()) imgB = inputs[1].value<QImage>();
    if (imgA.isNull() && imgB.isNull()) return QImage(outputSize, QImage::Format_ARGB32_Premultiplied);
    if (imgA.isNull()) imgA = QImage(outputSize, QImage::Format_ARGB32);
    if (imgB.isNull()) imgB = QImage(outputSize, QImage::Format_ARGB32);

    imgA = imgA.convertToFormat(QImage::Format_ARGB32);
    imgB = imgB.convertToFormat(QImage::Format_ARGB32);
    QSize sz = outputSize.isValid() ? outputSize : imgA.size();
    if (imgA.size() != sz) imgA = imgA.scaled(sz.width(), sz.height());
    if (imgB.size() != sz) imgB = imgB.scaled(sz.width(), sz.height());

    QString mode = node.params.value(QStringLiteral("mode"), QStringLiteral("over")).toString();
    double opacity = node.params.value(QStringLiteral("opacity"), 1.0).toDouble();

    QImage result(sz, QImage::Format_ARGB32);

    auto blendChannel = [&](int ca, int cb, const QString& m) -> int {
        double a = ca / 255.0, b = cb / 255.0;
        double r;
        if (m == QLatin1String("add")) {
            r = a + b;
        } else if (m == QLatin1String("multiply")) {
            r = a * b;
        } else if (m == QLatin1String("screen")) {
            r = 1.0 - (1.0 - a) * (1.0 - b);
        } else {
            r = a + b * (1.0 - a);
        }
        return clampInt(qRound(r * 255), 0, 255);
    };

    for (int y = 0; y < sz.height(); ++y) {
        const QRgb *lineA = reinterpret_cast<const QRgb*>(imgA.constScanLine(y));
        const QRgb *lineB = reinterpret_cast<const QRgb*>(imgB.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < sz.width(); ++x) {
            QRgb pa = lineA[x], pb = lineB[x];
            double aA = qAlpha(pa) / 255.0, aB = qAlpha(pb) / 255.0;
            double aBeff = aB * opacity;
            double aOut = aA + aBeff * (1.0 - aA);
            auto blendPx = [&](int ca, int cb) -> int {
                int blended = blendChannel(ca, cb, mode);
                double contribB = (mode == QLatin1String("over")) ? aBeff * (1.0 - aA) : aBeff;
                double total = aA + contribB;
                if (total < 1e-6) return ca;
                return clampInt(qRound((ca / 255.0 * aA + blended / 255.0 * contribB) / total * 255), 0, 255);
            };
            dstLine[x] = qRgba(blendPx(qRed(pa), qRed(pb)),
                               blendPx(qGreen(pa), qGreen(pb)),
                               blendPx(qBlue(pa), qBlue(pb)),
                               clampInt(qRound(aOut * 255), 0, 255));
        }
    }
    return result;
}

static QImage evaluateApplyMask(const GraphNode& node, const QVector<QVariant>& inputs, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);

    QImage img, mask;
    if (inputs.size() >= 1 && inputs[0].canConvert<QImage>()) img = inputs[0].value<QImage>();
    if (inputs.size() >= 2 && inputs[1].canConvert<QImage>()) mask = inputs[1].value<QImage>();
    if (img.isNull()) return QImage(outputSize, QImage::Format_ARGB32_Premultiplied);
    if (mask.isNull()) return img.convertToFormat(QImage::Format_ARGB32);

    img = img.convertToFormat(QImage::Format_ARGB32);
    mask = mask.convertToFormat(QImage::Format_ARGB32);
    QSize sz = img.size();
    if (mask.size() != sz) mask = mask.scaled(sz.width(), sz.height());

    QImage result(sz, QImage::Format_ARGB32);
    for (int y = 0; y < sz.height(); ++y) {
        const QRgb *imgLine = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        const QRgb *maskLine = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < sz.width(); ++x) {
            QRgb pi = imgLine[x], pm = maskLine[x];
            double lum = (0.299 * qRed(pm) + 0.587 * qGreen(pm) + 0.114 * qBlue(pm)) / 255.0;
            int newAlpha = clampInt(qRound(qAlpha(pi) * lum), 0, 255);
            dstLine[x] = qRgba(qRed(pi), qGreen(pi), qBlue(pi), newAlpha);
        }
    }
    return result;
}

static QImage evaluateInvert(const GraphNode& node, const QVector<QVariant>& inputs, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);
    QImage inputImage;
    if (!inputs.isEmpty() && inputs[0].canConvert<QImage>()) inputImage = inputs[0].value<QImage>();
    if (inputImage.isNull()) return QImage(outputSize, QImage::Format_ARGB32_Premultiplied);

    QImage src = inputImage.convertToFormat(QImage::Format_ARGB32);
    QImage result(src.size(), QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            QRgb px = srcLine[x];
            dstLine[x] = qRgba(255 - qRed(px), 255 - qGreen(px), 255 - qBlue(px), qAlpha(px));
        }
    }
    return result;
}

static QImage evaluateParticleEmitter(const GraphNode& node, double time, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);

    ParticleEmitterConfig cfg;

    int typeIdx = node.params.value(QStringLiteral("type"), 0).toInt();
    cfg.type = static_cast<ParticleType>(clampInt(typeIdx, 0, static_cast<int>(ParticleType::Custom)));
    cfg.emitRate = clampDbl(node.params.value(QStringLiteral("emitRate"), 50.0).toDouble(), 0.0, 10000.0);
    cfg.gravity.setX(node.params.value(QStringLiteral("gravityX"), 0.0).toDouble());
    cfg.gravity.setY(node.params.value(QStringLiteral("gravityY"), 0.0).toDouble());
    cfg.turbulenceAmount = clampDbl(node.params.value(QStringLiteral("turbulenceAmount"), 0.0).toDouble(), 0.0, 5000.0);
    cfg.lifeMin = clampDbl(node.params.value(QStringLiteral("lifeMin"), 2.0).toDouble(), 0.01, 60.0);
    cfg.lifeMax = clampDbl(node.params.value(QStringLiteral("lifeMax"), 4.0).toDouble(), cfg.lifeMin, 60.0);
    cfg.sizeMin = clampDbl(node.params.value(QStringLiteral("sizeMin"), 2.0).toDouble(), 0.1, 500.0);
    cfg.sizeMax = clampDbl(node.params.value(QStringLiteral("sizeMax"), 6.0).toDouble(), cfg.sizeMin, 500.0);
    cfg.startColor = node.params.value(QStringLiteral("startColor"), QColor(255, 255, 255)).value<QColor>();
    cfg.endColor = node.params.value(QStringLiteral("endColor"), QColor(255, 255, 255)).value<QColor>();

    ParticleSystem system;
    system.setConfig(cfg);
    system.reset();

    double simTime = qMin(time, 10.0);
    const double dt = 1.0 / 30.0;
    double elapsed = 0.0;
    while (elapsed < simTime) {
        double step = qMin(dt, simTime - elapsed);
        system.update(step);
        elapsed += step;
    }

    return system.renderFrame(outputSize, time);
}

static QImage evaluateFractalNoise(const GraphNode& node, double time, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);

    FractalNoise::Params p;
    int kindIdx = node.params.value(QStringLiteral("kind"), 0).toInt();
    p.kind = static_cast<FractalNoise::FractalKind>(clampInt(kindIdx, 0, 2));
    p.octaves = clampInt(node.params.value(QStringLiteral("octaves"), 5).toInt(), 1, 16);
    p.lacunarity = clampDbl(node.params.value(QStringLiteral("lacunarity"), 2.0).toDouble(), 0.5, 8.0);
    p.gain = clampDbl(node.params.value(QStringLiteral("gain"), 0.5).toDouble(), 0.0, 2.0);
    p.frequency = clampDbl(node.params.value(QStringLiteral("frequency"), 4.0).toDouble(), 0.1, 100.0);
    p.seed = static_cast<unsigned>(node.params.value(QStringLiteral("seed"), 1337).toInt());

    return FractalNoise::render(outputSize, time, p, false);
}

static QImage evaluateDisplace(const GraphNode& node, const QVector<QVariant>& inputs, QSize outputSize)
{
    if (outputSize.isEmpty()) outputSize = QSize(1, 1);

    QImage img;
    if (inputs.size() >= 1 && inputs[0].canConvert<QImage>()) img = inputs[0].value<QImage>();
    if (img.isNull()) return QImage(outputSize, QImage::Format_ARGB32_Premultiplied);

    QImage mapImg;
    if (inputs.size() >= 2 && inputs[1].canConvert<QImage>()) mapImg = inputs[1].value<QImage>();
    if (mapImg.isNull()) {
        mapImg = QImage(outputSize, QImage::Format_ARGB32_Premultiplied);
        mapImg.fill(QColor(128, 128, 128));
    }

    img = img.convertToFormat(QImage::Format_ARGB32);
    mapImg = mapImg.convertToFormat(QImage::Format_ARGB32);
    QSize sz = img.size();
    if (mapImg.size() != sz) mapImg = mapImg.scaled(sz.width(), sz.height());

    double hAmt = node.params.value(QStringLiteral("hAmount"), 0.0).toDouble();
    double vAmt = node.params.value(QStringLiteral("vAmount"), 0.0).toDouble();

    if (hAmt == 0.0 && vAmt == 0.0) return img;

    QImage result(sz, QImage::Format_ARGB32);
    int w = sz.width(), h = sz.height();

    for (int y = 0; y < h; ++y) {
        const QRgb *imgLine = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        const QRgb *mapLine = reinterpret_cast<const QRgb*>(mapImg.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            QRgb mp = mapLine[x];
            double lum = (0.2126 * qRed(mp) + 0.587 * qGreen(mp) + 0.114 * qBlue(mp)) / 255.0;
            double m = lum * 2.0 - 1.0;

            int sx = qBound(0, static_cast<int>(std::round(x + m * hAmt)), w - 1);
            int sy = qBound(0, static_cast<int>(std::round(y + m * vAmt)), h - 1);

            dstLine[x] = reinterpret_cast<const QRgb*>(img.constScanLine(sy))[sx];
        }
    }
    return result;
}

QVariant evaluateBuiltinNode(const GraphNode& node,
                             double time,
                             const QVector<QVariant>& inputs,
                             QSize outputSize)
{
    if (node.typeName == QLatin1String("Output")) {
        if (!inputs.isEmpty()) {
            return inputs[0];
        }
        return QVariant();
    }

    if (node.typeName == QLatin1String("SolidColor")) {
        return QVariant::fromValue(evaluateSolidColor(node.params, outputSize));
    }

    if (node.typeName == QLatin1String("Gradient")) {
        return QVariant::fromValue(evaluateGradient(node.params, outputSize));
    }

    if (node.typeName == QLatin1String("ImageInput")) {
        return QVariant::fromValue(makePlaceholderImage(outputSize));
    }

    if (node.typeName == QLatin1String("Transform")) {
        return QVariant::fromValue(evaluateTransform(node, inputs, outputSize));
    }

    if (node.typeName == QLatin1String("BrightnessContrast")) {
        return QVariant::fromValue(evaluateBrightnessContrast(node, inputs, outputSize));
    }

    if (node.typeName == QLatin1String("HueSaturation")) {
        return QVariant::fromValue(evaluateHueSaturation(node, inputs, outputSize));
    }

    if (node.typeName == QLatin1String("ColorBalance")) {
        return QVariant::fromValue(evaluateColorBalance(node, inputs, outputSize));
    }

    if (node.typeName == QLatin1String("GaussianBlur")) {
        return QVariant::fromValue(evaluateGaussianBlur(node, inputs, outputSize));
    }

    if (node.typeName == QLatin1String("Blend")) {
        return QVariant::fromValue(evaluateBlend(node, inputs, outputSize));
    }

    if (node.typeName == QLatin1String("ApplyMask")) {
        return QVariant::fromValue(evaluateApplyMask(node, inputs, outputSize));
    }

    if (node.typeName == QLatin1String("Invert")) {
        return QVariant::fromValue(evaluateInvert(node, inputs, outputSize));
    }

    if (node.typeName == QLatin1String("ParticleEmitter")) {
        return QVariant::fromValue(evaluateParticleEmitter(node, time, outputSize));
    }

    if (node.typeName == QLatin1String("FractalNoise")) {
        return QVariant::fromValue(evaluateFractalNoise(node, time, outputSize));
    }

    if (node.typeName == QLatin1String("Displace")) {
        return QVariant::fromValue(evaluateDisplace(node, inputs, outputSize));
    }

    return QVariant();
}

static NodePort makeInputPort(const QString& name, NodeSocketType type)
{
    NodePort p;
    p.name = name;
    p.type = type;
    p.isInput = true;
    return p;
}

static NodePort makeOutputPort(const QString& name, NodeSocketType type)
{
    NodePort p;
    p.name = name;
    p.type = type;
    p.isInput = false;
    return p;
}

void registerBuiltinNodes()
{
    auto& reg = NodeRegistry::instance();

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("Output");
        d.displayName = QStringLiteral("Output");
        d.category = QStringLiteral("General");
        d.inputs = {makeInputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.outputs = {};
        d.defaultParams = {};
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("SolidColor");
        d.displayName = QStringLiteral("Solid Color");
        d.category = QStringLiteral("Generator");
        d.inputs = {};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {{QStringLiteral("color"), QColor(0, 0, 0)}};
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("Gradient");
        d.displayName = QStringLiteral("Gradient");
        d.category = QStringLiteral("Generator");
        d.inputs = {};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("color1"), QColor(0, 0, 0)},
            {QStringLiteral("color2"), QColor(255, 255, 255)},
            {QStringLiteral("angle"), 0.0},
        };
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("ImageInput");
        d.displayName = QStringLiteral("Image Input");
        d.category = QStringLiteral("Input");
        d.inputs = {};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {{QStringLiteral("clipId"), QString()}};
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("Transform");
        d.displayName = QStringLiteral("Transform");
        d.category = QStringLiteral("Transform");
        d.inputs = {makeInputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("scaleX"), 1.0},
            {QStringLiteral("scaleY"), 1.0},
            {QStringLiteral("rotationDeg"), 0.0},
            {QStringLiteral("translateX"), 0.0},
            {QStringLiteral("translateY"), 0.0},
            {QStringLiteral("opacity"), 1.0},
        };
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("BrightnessContrast");
        d.displayName = QStringLiteral("Brightness / Contrast");
        d.category = QStringLiteral("Color");
        d.inputs = {makeInputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("brightness"), 0.0},
            {QStringLiteral("contrast"), 0.0},
        };
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("HueSaturation");
        d.displayName = QStringLiteral("Hue / Saturation");
        d.category = QStringLiteral("Color");
        d.inputs = {makeInputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("hueDeg"), 0.0},
            {QStringLiteral("saturation"), 0.0},
            {QStringLiteral("lightness"), 0.0},
        };
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("ColorBalance");
        d.displayName = QStringLiteral("Color Balance");
        d.category = QStringLiteral("Color");
        d.inputs = {makeInputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("liftColor"), QColor(0, 0, 0)},
            {QStringLiteral("gammaColor"), QColor(128, 128, 128)},
            {QStringLiteral("gainColor"), QColor(255, 255, 255)},
        };
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("GaussianBlur");
        d.displayName = QStringLiteral("Gaussian Blur");
        d.category = QStringLiteral("Filter");
        d.inputs = {makeInputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("radius"), 0.0},
        };
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("Blend");
        d.displayName = QStringLiteral("Blend");
        d.category = QStringLiteral("Composite");
        d.inputs = {
            makeInputPort(QStringLiteral("A"), NodeSocketType::Image),
            makeInputPort(QStringLiteral("B"), NodeSocketType::Image),
        };
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("mode"), QStringLiteral("over")},
            {QStringLiteral("opacity"), 1.0},
        };
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("ApplyMask");
        d.displayName = QStringLiteral("Apply Mask");
        d.category = QStringLiteral("Composite");
        d.inputs = {
            makeInputPort(QStringLiteral("Image"), NodeSocketType::Image),
            makeInputPort(QStringLiteral("Mask"), NodeSocketType::Image),
        };
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {};
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("Invert");
        d.displayName = QStringLiteral("Invert");
        d.category = QStringLiteral("Color");
        d.inputs = {makeInputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {};
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("ParticleEmitter");
        d.displayName = QStringLiteral("Particle Emitter");
        d.category = QStringLiteral("VFX");
        d.inputs = {};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("type"), 0},
            {QStringLiteral("emitRate"), 50.0},
            {QStringLiteral("gravityX"), 0.0},
            {QStringLiteral("gravityY"), 0.0},
            {QStringLiteral("turbulenceAmount"), 0.0},
            {QStringLiteral("lifeMin"), 2.0},
            {QStringLiteral("lifeMax"), 4.0},
            {QStringLiteral("sizeMin"), 2.0},
            {QStringLiteral("sizeMax"), 6.0},
            {QStringLiteral("startColor"), QColor(255, 255, 255)},
            {QStringLiteral("endColor"), QColor(255, 255, 255)},
        };
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("FractalNoise");
        d.displayName = QStringLiteral("Fractal Noise");
        d.category = QStringLiteral("VFX");
        d.inputs = {};
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("kind"), 0},
            {QStringLiteral("octaves"), 5},
            {QStringLiteral("lacunarity"), 2.0},
            {QStringLiteral("gain"), 0.5},
            {QStringLiteral("frequency"), 4.0},
            {QStringLiteral("seed"), 1337},
        };
        reg.registerType(d);
    }

    {
        NodeTypeDescriptor d;
        d.typeName = QStringLiteral("Displace");
        d.displayName = QStringLiteral("Displace");
        d.category = QStringLiteral("VFX");
        d.inputs = {
            makeInputPort(QStringLiteral("Image"), NodeSocketType::Image),
            makeInputPort(QStringLiteral("Map"), NodeSocketType::Image),
        };
        d.outputs = {makeOutputPort(QStringLiteral("Image"), NodeSocketType::Image)};
        d.defaultParams = {
            {QStringLiteral("hAmount"), 0.0},
            {QStringLiteral("vAmount"), 0.0},
        };
        reg.registerType(d);
    }
}

} // namespace nodelib
