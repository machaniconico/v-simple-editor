#pragma once

#include <QColor>
#include <QImage>
#include <QSize>
#include <QtGlobal>

/// FractalNoise — deterministic 3D fractal noise generator.
///
/// Provides a namespace of free functions.  The third noise dimension is
/// "evolution" (time), so that varying it yields a smooth animation.
///
/// Usage:
///   FractalNoise::Params p;
///   p.kind   = FractalKind::FBm;
///   p.octaves = 6;
///   QImage img = FractalNoise::render(QSize(1280, 720), 0.5, p);
namespace FractalNoise
{

/// Fractal layering mode.
enum class FractalKind { FBm, Turbulence, Ridged };

/// Parameters controlling the fractal noise output.
struct Params {
    FractalKind kind      = FractalKind::FBm;
    int         octaves   = 5;
    double      lacunarity = 2.0;   // frequency multiplier per octave
    double      gain      = 0.5;    // amplitude multiplier per octave
    double      frequency = 4.0;    // base spatial frequency
    unsigned    seed      = 1337;

    // Color ramp (used only when grayscale == false in render)
    QColor lowColor  = Qt::black;
    QColor highColor = Qt::white;

    bool isDefault() const {
        return kind == FractalKind::FBm && octaves == 5
            && qFuzzyCompare(lacunarity, 2.0) && qFuzzyCompare(gain, 0.5)
            && qFuzzyCompare(frequency, 4.0) && seed == 1337;
    }
};

/// Return the raw (un-remapped) fractal noise value at normalised coords.
///
/// x, y are in [0, 1] across the image; evolution is the time axis.
/// The returned value range depends on `kind`:
///   FBm        — roughly [-1, 1]
///   Turbulence — [0, ~octaves]  (sum of absolute values)
///   Ridged     — [0, ~octaves]  (sum of (1-|n|)^2)
double sample(double x, double y, double evolution, const Params &p);

/// Render a fractal noise image.
///
/// When `grayscale` is true the output is Format_Grayscale8 (single 8-bit
/// channel).  When false the output is Format_ARGB32 and each pixel's
/// luminance is mapped through the lowColor → highColor ramp in params.
QImage render(const QSize &size, double evolution, const Params &p,
              bool grayscale = true);

} // namespace FractalNoise
