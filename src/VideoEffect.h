#pragma once

#include <QImage>
#include <QColor>
#include <QVector>
#include <QString>

// --- Color Correction / Grading ---

struct ColorCorrection {
    double brightness = 0.0;    // -100 to 100
    double contrast = 0.0;      // -100 to 100
    double saturation = 0.0;    // -100 to 100
    double hue = 0.0;           // -180 to 180 degrees
    double temperature = 0.0;   // -100 (cool/blue) to 100 (warm/orange)
    double tint = 0.0;          // -100 (green) to 100 (magenta)
    double gamma = 1.0;         // 0.1 to 3.0
    double highlights = 0.0;    // -100 to 100
    double shadows = 0.0;       // -100 to 100
    double exposure = 0.0;      // -3.0 to 3.0

    // Lift/Gamma/Gain color wheels (DaVinci Resolve style)
    double liftR = 0.0, liftG = 0.0, liftB = 0.0;       // -1.0 to 1.0
    double gammaR = 0.0, gammaG = 0.0, gammaB = 0.0;     // -1.0 to 1.0
    double gainR = 0.0, gainG = 0.0, gainB = 0.0;        // -1.0 to 1.0

    bool isDefault() const {
        return brightness == 0.0 && contrast == 0.0 && saturation == 0.0
            && hue == 0.0 && temperature == 0.0 && tint == 0.0
            && gamma == 1.0 && highlights == 0.0 && shadows == 0.0
            && exposure == 0.0
            && liftR == 0.0 && liftG == 0.0 && liftB == 0.0
            && gammaR == 0.0 && gammaG == 0.0 && gammaB == 0.0
            && gainR == 0.0 && gainG == 0.0 && gainB == 0.0;
    }

    void reset() { *this = ColorCorrection{}; }
};

// --- Video Effects ---

enum class VideoEffectType {
    None,
    Blur,
    Sharpen,
    Mosaic,
    ChromaKey,
    Vignette,
    Sepia,
    Grayscale,
    Invert,
    Noise,
    DisplacementMap,
    FractalNoiseGen,
    GaussianBlur,
    DirectionalBlur,
    RadialBlur
};

struct VideoEffect {
    VideoEffectType type = VideoEffectType::None;
    bool enabled = true;

    // Params — meaning depends on type:
    //   Blur: p1=radius
    //   Sharpen: p1=amount(0-10)
    //   Mosaic: p1=blockSize(2-100)
    //   ChromaKey: p1=tolerance, p2=softness
    //   Vignette: p1=intensity(0-1), p2=radius(0-1)
    //   Sepia: p1=intensity(0-1)
    //   Noise: p1=amount(0-100)
    //   DisplacementMap: p1=hAmount(-200..200), p2=vAmount(-200..200), p3=mapMode(0=fractal,1=luminance)
    //   FractalNoiseGen: p1=scale/frequency, p2=octaves(1..8), p3=evolution(time offset)
    //   GaussianBlur: p1=radius/sigma
    //   DirectionalBlur: p1=angleDegrees, p2=lengthPx
    //   RadialBlur: p1=amount, p2=mode(0=spin,1=zoom)
    double param1 = 0.0;
    double param2 = 0.0;
    double param3 = 0.0;
    QColor keyColor = QColor(0, 255, 0); // ChromaKey only
    double startSec = -1.0; // clip-local seconds; -1 == no start limit
    double endSec = -1.0;   // clip-local seconds; -1 == no end limit

    static QString typeName(VideoEffectType t);
    static QVector<VideoEffectType> allTypes();

    static VideoEffect createBlur(double radius = 5.0);
    static VideoEffect createSharpen(double amount = 1.5);
    static VideoEffect createMosaic(double blockSize = 10.0);
    static VideoEffect createChromaKey(QColor color = QColor(0, 255, 0),
                                       double tolerance = 40.0, double softness = 10.0);
    static VideoEffect createVignette(double intensity = 0.5, double radius = 0.8);
    static VideoEffect createSepia(double intensity = 1.0);
    static VideoEffect createGrayscale();
    static VideoEffect createInvert();
    static VideoEffect createNoise(double amount = 20.0);
    static VideoEffect createDisplacementMap(double hAmount = 50.0, double vAmount = 0.0, int mapMode = 0);
    static VideoEffect createFractalNoise(double scale = 4.0, int octaves = 5, double evolution = 0.0);
    static VideoEffect createGaussianBlur(double radius = 5.0);
    static VideoEffect createDirectionalBlur(double angleDegrees = 0.0, double lengthPx = 20.0);
    static VideoEffect createRadialBlur(double amount = 10.0, int mode = 0);
};

// --- Processor ---

class VideoEffectProcessor
{
public:
    static QImage applyColorCorrection(const QImage &input, const ColorCorrection &cc);
    static QImage applyEffect(const QImage &input, const VideoEffect &effect);
    static QImage applyEffectStack(const QImage &input, const ColorCorrection &cc,
                                   const QVector<VideoEffect> &effects);
    static void adjustTemperatureTint(QImage &img, double temperature, double tint);

private:
    static void adjustBrightnessContrast(QImage &img, double brightness, double contrast);
    static void adjustSaturation(QImage &img, double saturation);
    static void adjustHue(QImage &img, double hue);
    static void adjustGamma(QImage &img, double gamma);
    static void adjustHighlightsShadows(QImage &img, double highlights, double shadows);
    static void adjustExposure(QImage &img, double exposure);

    static QImage applyBlur(const QImage &img, double radius);
    static QImage applySharpen(const QImage &img, double amount);
    static QImage applyMosaic(const QImage &img, double blockSize);
    static QImage applyChromaKey(const QImage &img, QColor keyColor,
                                  double tolerance, double softness);
    static QImage applyVignette(const QImage &img, double intensity, double radius);
    static QImage applySepia(const QImage &img, double intensity);
    static QImage applyGrayscale(const QImage &img);
    static QImage applyInvert(const QImage &img);
    static QImage applyNoise(const QImage &img, double amount);
    static QImage applyDisplacementMap(const QImage &img, double hAmt, double vAmt, int mode);
    static QImage applyFractalNoise(const QImage &img, double scale, int octaves, double evolution);
    static QImage applyGaussianBlur(const QImage &img, double radius);
    static QImage applyDirectionalBlur(const QImage &img, double angleDegrees, double lengthPx);
    static QImage applyRadialBlur(const QImage &img, double amount, int mode);
};
