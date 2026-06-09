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
    RadialBlur,
    Glow,
    FindEdges,
    Emboss,
    Posterize,
    Threshold,
    Solarize,
    Levels,
    Tint,
    BlackWhite,
    Exposure,
    HueSaturation,
    RGBSplit,
    WaveWarp,
    Ripple,
    GlitchVHS,
    GradientRamp,
    Fill,
    Bloom,
    Scanlines,
    Halftone,
    Curves,
    ChannelMixer,
    Vibrance,
    PhotoFilter,
    Tritone,
    BrightnessContrast
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
    //   Glow: p1=threshold, p2=radius, p3=intensity
    //   FindEdges: p1=intensity
    //   Emboss: p1=angleDegrees, p2=amount
    //   Posterize: p1=levels
    //   Threshold: p1=level
    //   Solarize: p1=threshold
    //   Levels: p1=inputBlack(0..255), p2=inputWhite(0..255), p3=gamma(0.1..5.0)
    //   Tint: p1=amount(0..1), keyColor=highlight tint
    //   BlackWhite: p1=redWeight, p2=greenWeight, p3=blueWeight
    //   Exposure: p1=stops(-5..5)
    //   HueSaturation: p1=hueDegrees(-180..180), p2=saturation(-100..100), p3=lightness(-100..100)
    //   RGBSplit: p1=offsetX(-50..50), p2=offsetY(-50..50)
    //   WaveWarp: p1=amplitude(0..100), p2=wavelength(1..500), p3=phase(0..360)
    //   Ripple: p1=amplitude(0..100), p2=wavelength(1..500), p3=phase(0..360)
    //   GlitchVHS: p1=intensity(0..1), p2=blockHeight(4..64), p3=seed(0..1000)
    //   GradientRamp: p1=type(0=linear,1=radial), p2=angleDegrees, p3=opacity(0..1), keyColor=start color
    //   Fill: p1=opacity(0..1), keyColor=fill color
    //   Bloom: p1=threshold, p2=radius, p3=intensity
    //   Scanlines: p1=lineSpacing(2..20), p2=darkness(0..1), p3=opacity(0..1)
    //   Halftone: p1=dotSize(2..30), p2=angleDegrees
    //   Curves: p1=shadows(-100..100), p2=highlights(-100..100), p3=midContrast(-100..100)
    //   ChannelMixer: p1=redFromRed%(0..200), p2=greenFromGreen%(0..200), p3=blueFromBlue%(0..200)
    //   Vibrance: p1=vibrance(-100..100)
    //   PhotoFilter: p1=density(0..100), keyColor=filter color
    //   Tritone: p1=blend(0..1), keyColor=shadow color
    //   BrightnessContrast: p1=brightness(-100..100), p2=contrast(-100..100)
    double param1 = 0.0;
    double param2 = 0.0;
    double param3 = 0.0;
    QColor keyColor = QColor(0, 255, 0); // ChromaKey / Tint / GradientRamp / Fill / PhotoFilter / Tritone
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
    static VideoEffect createGlow(double threshold = 128.0, double radius = 10.0, double intensity = 1.0);
    static VideoEffect createFindEdges(double intensity = 1.0);
    static VideoEffect createEmboss(double angleDegrees = 45.0, double amount = 2.0);
    static VideoEffect createPosterize(double levels = 4.0);
    static VideoEffect createThreshold(double level = 128.0);
    static VideoEffect createSolarize(double threshold = 128.0);
    static VideoEffect createLevels(double inputBlack = 0.0, double inputWhite = 255.0, double gamma = 1.0);
    static VideoEffect createTint(double amount = 1.0, QColor highlightTint = QColor(255, 255, 255));
    static VideoEffect createBlackWhite(double redWeight = 0.299, double greenWeight = 0.587, double blueWeight = 0.114);
    static VideoEffect createExposure(double stops = 0.0);
    static VideoEffect createHueSaturation(double hueDegrees = 0.0, double saturation = 0.0, double lightness = 0.0);
    static VideoEffect createRGBSplit(double offsetX = 6.0, double offsetY = 0.0);
    static VideoEffect createWaveWarp(double amplitude = 12.0, double wavelength = 80.0, double phaseDegrees = 0.0);
    static VideoEffect createRipple(double amplitude = 12.0, double wavelength = 80.0, double phaseDegrees = 0.0);
    static VideoEffect createGlitchVHS(double intensity = 0.5, double blockHeight = 12.0, double seed = 1.0);
    static VideoEffect createGradientRamp(int type = 0, double angleDegrees = 0.0, double opacity = 1.0, QColor startColor = QColor(255, 255, 255));
    static VideoEffect createFill(QColor fillColor = QColor(255, 255, 255), double opacity = 1.0);
    static VideoEffect createBloom(double threshold = 128.0, double radius = 20.0, double intensity = 1.0);
    static VideoEffect createScanlines(double lineSpacing = 4.0, double darkness = 0.5, double opacity = 1.0);
    static VideoEffect createHalftone(double dotSize = 8.0, double angleDegrees = 45.0);
    static VideoEffect createCurves(double shadows = 0.0, double highlights = 0.0, double midContrast = 0.0);
    static VideoEffect createChannelMixer(double redFromRed = 100.0, double greenFromGreen = 100.0, double blueFromBlue = 100.0);
    static VideoEffect createVibrance(double vibrance = 0.0);
    static VideoEffect createPhotoFilter(QColor filterColor = QColor(236, 138, 0), double density = 0.0);
    static VideoEffect createTritone(QColor shadowColor = QColor(0, 0, 0), double blend = 0.0);
    static VideoEffect createBrightnessContrast(double brightness = 0.0, double contrast = 0.0);
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
    static QImage applyGlow(const QImage &img, double threshold, double radius, double intensity);
    static QImage applyFindEdges(const QImage &img, double intensity);
    static QImage applyEmboss(const QImage &img, double angleDegrees, double amount);
    static QImage applyPosterize(const QImage &img, double levels);
    static QImage applyThreshold(const QImage &img, double level);
    static QImage applySolarize(const QImage &img, double threshold);
    static QImage applyLevels(const QImage &img, double inputBlack, double inputWhite, double gamma);
    static QImage applyTint(const QImage &img, double amount, QColor highlightTint);
    static QImage applyBlackWhite(const QImage &img, double redWeight, double greenWeight, double blueWeight);
    static QImage applyExposureEffect(const QImage &img, double stops);
    static QImage applyHueSaturation(const QImage &img, double hueDegrees, double saturation, double lightness);
    static QImage applyRGBSplit(const QImage &img, double offsetX, double offsetY);
    static QImage applyWaveWarp(const QImage &img, double amplitude, double wavelength, double phaseDegrees);
    static QImage applyRipple(const QImage &img, double amplitude, double wavelength, double phaseDegrees);
    static QImage applyGlitchVHS(const QImage &img, double intensity, double blockHeight, double seed);
    static QImage applyGradientRamp(const QImage &img, int type, double angleDegrees, double opacity, QColor startColor);
    static QImage applyFill(const QImage &img, QColor fillColor, double opacity);
    static QImage applyBloom(const QImage &img, double threshold, double radius, double intensity);
    static QImage applyScanlines(const QImage &img, double lineSpacing, double darkness, double opacity);
    static QImage applyHalftone(const QImage &img, double dotSize, double angleDegrees);
    static QImage applyCurves(const QImage &img, double shadows, double highlights, double midContrast);
    static QImage applyChannelMixer(const QImage &img, double redFromRed, double greenFromGreen, double blueFromBlue);
    static QImage applyVibrance(const QImage &img, double vibrance);
    static QImage applyPhotoFilter(const QImage &img, QColor filterColor, double density);
    static QImage applyTritone(const QImage &img, QColor shadowColor, double blend);
    static QImage applyBrightnessContrastEffect(const QImage &img, double brightness, double contrast);
};
