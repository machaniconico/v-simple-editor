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
