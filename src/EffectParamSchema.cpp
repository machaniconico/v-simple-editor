#include "EffectParamSchema.h"
#include "VideoEffect.h"

namespace effectctrl {

QVector<ParamDef> paramSchemaFor(VideoEffectType type)
{
    switch (type) {
    case VideoEffectType::Blur:
        return { { "radius", "Radius", ParamType::Float, 0.0, 50.0, 5.0 } };

    case VideoEffectType::Sharpen:
        return { { "amount", "Amount", ParamType::Float, 0.0, 10.0, 1.5 } };

    case VideoEffectType::Mosaic:
        return { { "blockSize", "Block Size", ParamType::Float, 2.0, 100.0, 10.0 } };

    case VideoEffectType::ChromaKey:
        return {
            { "tolerance", "Tolerance", ParamType::Float, 0.0, 100.0, 40.0 },
            { "softness", "Softness", ParamType::Float, 0.0, 50.0, 10.0 },
            { "color", "Key Color", ParamType::Color, 0.0, 0.0, 0.0 }
        };

    case VideoEffectType::Vignette:
        return {
            { "intensity", "Intensity", ParamType::Float, 0.0, 1.0, 0.5 },
            { "radius", "Radius", ParamType::Float, 0.0, 1.0, 0.8 }
        };

    case VideoEffectType::Sepia:
        return { { "intensity", "Intensity", ParamType::Float, 0.0, 1.0, 1.0 } };

    case VideoEffectType::Grayscale:
        return {};

    case VideoEffectType::Invert:
        return {};

    case VideoEffectType::Noise:
        return { { "amount", "Amount", ParamType::Float, 0.0, 100.0, 20.0 } };

    case VideoEffectType::GaussianBlur:
        return { { "radius", "Radius", ParamType::Float, 0.0, 50.0, 5.0 } };

    case VideoEffectType::DirectionalBlur:
        return {
            { "angle", "Angle", ParamType::Float, 0.0, 360.0, 0.0 },
            { "length", "Length", ParamType::Float, 0.0, 100.0, 20.0 }
        };

    case VideoEffectType::RadialBlur:
        return {
            { "amount", "Amount", ParamType::Float, 0.0, 50.0, 10.0 },
            { "mode", "Mode", ParamType::Int, 0.0, 1.0, 0.0 }
        };

    case VideoEffectType::Glow:
        return {
            { "threshold", "Threshold", ParamType::Int, 0.0, 255.0, 128.0 },
            { "radius", "Radius", ParamType::Float, 0.0, 50.0, 10.0 },
            { "intensity", "Intensity", ParamType::Float, 0.0, 3.0, 1.0 }
        };

    case VideoEffectType::FindEdges:
        return { { "intensity", "Intensity", ParamType::Float, 0.0, 2.0, 1.0 } };

    case VideoEffectType::Emboss:
        return {
            { "angle", "Angle", ParamType::Float, 0.0, 360.0, 45.0 },
            { "amount", "Amount", ParamType::Float, 0.0, 5.0, 2.0 }
        };

    case VideoEffectType::Posterize:
        return { { "levels", "Levels", ParamType::Int, 2.0, 32.0, 4.0 } };

    case VideoEffectType::Threshold:
        return { { "level", "Level", ParamType::Int, 0.0, 255.0, 128.0 } };

    case VideoEffectType::Solarize:
        return { { "threshold", "Threshold", ParamType::Int, 0.0, 255.0, 128.0 } };

    case VideoEffectType::Levels:
        return {
            { "inputBlack", "Input Black", ParamType::Int, 0.0, 255.0, 0.0 },
            { "inputWhite", "Input White", ParamType::Int, 0.0, 255.0, 255.0 },
            { "gamma", "Gamma", ParamType::Float, 0.1, 5.0, 1.0 }
        };

    case VideoEffectType::Tint:
        return {
            { "amount", "Amount", ParamType::Float, 0.0, 1.0, 1.0 },
            { "keyColor", "Key Color", ParamType::Color, 0.0, 0.0, 0.0 }
        };

    case VideoEffectType::BlackWhite:
        return {
            { "redWeight", "Red Weight", ParamType::Float, 0.0, 1.0, 0.299 },
            { "greenWeight", "Green Weight", ParamType::Float, 0.0, 1.0, 0.587 },
            { "blueWeight", "Blue Weight", ParamType::Float, 0.0, 1.0, 0.114 }
        };

    case VideoEffectType::Exposure:
        return { { "stops", "Stops", ParamType::Float, -5.0, 5.0, 0.0 } };

    case VideoEffectType::HueSaturation:
        return {
            { "hueDegrees", "Hue", ParamType::Float, -180.0, 180.0, 0.0 },
            { "saturation", "Saturation", ParamType::Float, -100.0, 100.0, 0.0 },
            { "lightness", "Lightness", ParamType::Float, -100.0, 100.0, 0.0 }
        };

    case VideoEffectType::RGBSplit:
        return {
            { "offsetX", "Offset X", ParamType::Float, -50.0, 50.0, 6.0 },
            { "offsetY", "Offset Y", ParamType::Float, -50.0, 50.0, 0.0 }
        };

    case VideoEffectType::WaveWarp:
        return {
            { "amplitude", "Amplitude", ParamType::Float, 0.0, 100.0, 12.0 },
            { "wavelength", "Wavelength", ParamType::Float, 1.0, 500.0, 80.0 },
            { "phase", "Phase", ParamType::Float, 0.0, 360.0, 0.0 }
        };

    case VideoEffectType::Ripple:
        return {
            { "amplitude", "Amplitude", ParamType::Float, 0.0, 100.0, 12.0 },
            { "wavelength", "Wavelength", ParamType::Float, 1.0, 500.0, 80.0 },
            { "phase", "Phase", ParamType::Float, 0.0, 360.0, 0.0 }
        };

    case VideoEffectType::GlitchVHS:
        return {
            { "intensity", "Intensity", ParamType::Float, 0.0, 1.0, 0.5 },
            { "blockHeight", "Block Height", ParamType::Int, 4.0, 64.0, 12.0 },
            { "seed", "Seed", ParamType::Int, 0.0, 1000.0, 1.0 }
        };

    case VideoEffectType::GradientRamp:
        return {
            { "type", "タイプ", ParamType::Int, 0.0, 1.0, 0.0 },
            { "angle", "角度", ParamType::Float, 0.0, 360.0, 0.0 },
            { "opacity", "不透明度", ParamType::Float, 0.0, 1.0, 1.0 },
            { "keyColor", "Key Color", ParamType::Color, 0.0, 0.0, 0.0 }
        };

    case VideoEffectType::Fill:
        return {
            { "opacity", "不透明度", ParamType::Float, 0.0, 1.0, 1.0 },
            { "keyColor", "Key Color", ParamType::Color, 0.0, 0.0, 0.0 }
        };

    case VideoEffectType::Bloom:
        return {
            { "threshold", "しきい値", ParamType::Int, 0.0, 255.0, 128.0 },
            { "radius", "半径", ParamType::Float, 0.0, 80.0, 20.0 },
            { "intensity", "強度", ParamType::Float, 0.0, 2.0, 1.0 }
        };

    case VideoEffectType::Scanlines:
        return {
            { "lineSpacing", "間隔", ParamType::Int, 2.0, 20.0, 4.0 },
            { "darkness", "暗さ", ParamType::Float, 0.0, 1.0, 0.5 },
            { "opacity", "不透明度", ParamType::Float, 0.0, 1.0, 1.0 }
        };

    case VideoEffectType::Halftone:
        return {
            { "dotSize", "ドットサイズ", ParamType::Int, 2.0, 30.0, 8.0 },
            { "angle", "角度", ParamType::Float, 0.0, 360.0, 45.0 }
        };

    case VideoEffectType::None:
    default:
        return {};
    }
}

} // namespace effectctrl

#ifdef VEDITOR_EFFECT_PARAM_SCHEMA_SELFTEST
#include <QDebug>

namespace effectctrl::tests {

static bool runSelfTest()
{
    bool allPassed = true;

    auto testRoundTrip = [&](VideoEffectType type, const QString &paramName, double value, double expected) {
        VideoEffect effect;
        effect.type = type;
        setParamValue(effect, paramName, value);
        double readBack = paramValue(effect, paramName);
        if (std::abs(readBack - expected) > 1e-9) {
            qWarning() << "FAIL:" << VideoEffect::typeName(type) << paramName
                       << "expected" << expected << "got" << readBack;
            allPassed = false;
        }
    };

    auto testColorRoundTrip = [&](const QString &paramName, const QColor &value) {
        VideoEffect effect;
        effect.type = VideoEffectType::ChromaKey;
        setColorParam(effect, paramName, value);
        QColor readBack = colorParamValue(effect, paramName);
        if (readBack != value) {
            qWarning() << "FAIL: ChromaKey color expected" << value << "got" << readBack;
            allPassed = false;
        }
    };

    // Blur
    testRoundTrip(VideoEffectType::Blur, "radius", 15.0, 15.0);

    // Sharpen
    testRoundTrip(VideoEffectType::Sharpen, "amount", 5.0, 5.0);

    // Mosaic
    testRoundTrip(VideoEffectType::Mosaic, "blockSize", 25.0, 25.0);

    // ChromaKey
    testRoundTrip(VideoEffectType::ChromaKey, "tolerance", 55.0, 55.0);
    testRoundTrip(VideoEffectType::ChromaKey, "softness", 20.0, 20.0);
    testColorRoundTrip("color", QColor(255, 0, 0));

    // Vignette
    testRoundTrip(VideoEffectType::Vignette, "intensity", 0.75, 0.75);
    testRoundTrip(VideoEffectType::Vignette, "radius", 0.6, 0.6);

    // Sepia
    testRoundTrip(VideoEffectType::Sepia, "intensity", 0.5, 0.5);

    // Noise
    testRoundTrip(VideoEffectType::Noise, "amount", 50.0, 50.0);

    // Grayscale / Invert — no params, schema should be empty
    if (!paramSchemaFor(VideoEffectType::Grayscale).isEmpty()) {
        qWarning() << "FAIL: Grayscale schema should be empty";
        allPassed = false;
    }
    if (!paramSchemaFor(VideoEffectType::Invert).isEmpty()) {
        qWarning() << "FAIL: Invert schema should be empty";
        allPassed = false;
    }

    // None — empty schema
    if (!paramSchemaFor(VideoEffectType::None).isEmpty()) {
        qWarning() << "FAIL: None schema should be empty";
        allPassed = false;
    }

    // Verify all 9 effect types have non-empty or correctly empty schemas
    auto types = VideoEffect::allTypes();
    for (auto t : types) {
        auto schema = paramSchemaFor(t);
        bool hasColor = false;
        for (const auto &d : schema) {
            if (d.type == ParamType::Color) hasColor = true;
        }
        if (t == VideoEffectType::ChromaKey && !hasColor) {
            qWarning() << "FAIL: ChromaKey schema missing color param";
            allPassed = false;
        }
    }

    // Test default values match factory defaults
    {
        VideoEffect blur = VideoEffect::createBlur();
        if (std::abs(paramValue(blur, "radius") - 5.0) > 1e-9) {
            qWarning() << "FAIL: Blur default radius mismatch";
            allPassed = false;
        }
    }
    {
        VideoEffect chroma = VideoEffect::createChromaKey();
        if (std::abs(paramValue(chroma, "tolerance") - 40.0) > 1e-9) {
            qWarning() << "FAIL: ChromaKey default tolerance mismatch";
            allPassed = false;
        }
        if (colorParamValue(chroma, "color") != QColor(0, 255, 0)) {
            qWarning() << "FAIL: ChromaKey default color mismatch";
            allPassed = false;
        }
    }

    if (allPassed) {
        qDebug() << "EffectParamSchema self-test: ALL PASSED";
    } else {
        qWarning() << "EffectParamSchema self-test: SOME FAILED";
    }

    return allPassed;
}

struct SelfTestRunner {
    SelfTestRunner() { runSelfTest(); }
};
static SelfTestRunner _selfTestInstance;

} // namespace effectctrl::tests
#endif
