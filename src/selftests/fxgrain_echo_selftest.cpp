#include "../EffectParamSchema.h"
#include "../EffectPreset.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoEffect.h"

#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

bool imagesByteIdentical(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()
        || a.bytesPerLine() != b.bytesPerLine()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<std::size_t>(a.bytesPerLine())) != 0) {
            return false;
        }
    }
    return true;
}

QImage makePattern(int width, int height)
{
    QImage image(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixelColor(x, y,
                                QColor((x * 31 + y * 7) & 255,
                                       (x * 11 + y * 29) & 255,
                                       (x * 19 + y * 13) & 255,
                                       255));
        }
    }
    return image;
}

double redVariance(const QImage &image)
{
    if (image.isNull())
        return 0.0;
    const int sampleCount = image.width() * image.height();
    double sum = 0.0;
    double sumSquares = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double value = image.pixelColor(x, y).red();
            sum += value;
            sumSquares += value * value;
        }
    }
    const double mean = sum / sampleCount;
    return sumSquares / sampleCount - mean * mean;
}

bool schemaNamesEqual(VideoEffectType type, const QStringList &expected)
{
    const QVector<effectctrl::ParamDef> schema = effectctrl::paramSchemaFor(type);
    if (schema.size() != expected.size())
        return false;
    for (int i = 0; i < schema.size(); ++i) {
        if (schema[i].name != expected[i])
            return false;
    }
    return true;
}

bool containsType(VideoEffectType type)
{
    const QVector<VideoEffectType> types = VideoEffect::allTypes();
    return std::find(types.cbegin(), types.cend(), type) != types.cend();
}

void reportGate(const char *gate, const char *description, bool ok,
                int &passed, int &failed)
{
    std::cerr << (ok ? "PASS " : "FAIL ") << gate << ' ' << description << '\n';
    if (ok)
        ++passed;
    else
        ++failed;
}

} // namespace

int runFxGrainEchoSelftest()
{
    int passed = 0;
    int failed = 0;
    const QImage pattern = makePattern(48, 32);

    const VideoEffect grain = VideoEffect::createFilmGrain(0.45, 2, 0.3, true);
    const QImage deterministicA = VideoEffectProcessor::applyEffect(pattern, grain);
    const QImage deterministicB = VideoEffectProcessor::applyEffect(pattern, grain);
    reportGate("G1", "FilmGrain deterministic output",
               imagesByteIdentical(deterministicA, deterministicB), passed, failed);

    const QImage grainOff = VideoEffectProcessor::applyEffect(
        pattern, VideoEffect::createFilmGrain(0.0, 4, 1.0, true));
    reportGate("G2", "FilmGrain amount=0 byte identity",
               imagesByteIdentical(pattern, grainOff), passed, failed);

    QImage flat(64, 64, QImage::Format_RGBA8888);
    flat.fill(QColor(128, 128, 128, 255));
    const double lowVariance = redVariance(VideoEffectProcessor::applyEffect(
        flat, VideoEffect::createFilmGrain(0.2, 1, 0.0, false)));
    const double mediumVariance = redVariance(VideoEffectProcessor::applyEffect(
        flat, VideoEffect::createFilmGrain(0.5, 1, 0.0, false)));
    const double highVariance = redVariance(VideoEffectProcessor::applyEffect(
        flat, VideoEffect::createFilmGrain(0.8, 1, 0.0, false)));
    reportGate("G3", "FilmGrain variance increases with amount",
               lowVariance < mediumVariance && mediumVariance < highVariance,
               passed, failed);

    const bool metadataPresent =
        containsType(VideoEffectType::FilmGrain)
        && containsType(VideoEffectType::Echo)
        && VideoEffect::typeName(VideoEffectType::FilmGrain) != QStringLiteral("Unknown")
        && VideoEffect::typeName(VideoEffectType::Echo) != QStringLiteral("Unknown")
        && schemaNamesEqual(VideoEffectType::FilmGrain,
                            { "amount", "size", "colorAmount", "seedPerFrame" })
        && schemaNamesEqual(VideoEffectType::Echo,
                            { "delaySec", "count", "decay", "blend" });
    reportGate("G4", "typeName/allTypes/schema contain FilmGrain and Echo",
               metadataPresent, passed, failed);

    const QImage echoNoOp = VideoEffectProcessor::applyEffect(
        pattern, VideoEffect::createEcho());
    reportGate("G5", "Echo single-frame applyEffect is a no-op",
               imagesByteIdentical(pattern, echoNoOp), passed, failed);

    const QImage emptyCompose = tlrender::composeEcho(
        pattern, QVector<QImage>(), 0.5, 2);
    reportGate("G6", "composeEcho count=0 byte identity",
               imagesByteIdentical(pattern, emptyCompose), passed, failed);

    QImage brighter(pattern.size(), QImage::Format_RGBA8888);
    brighter.fill(QColor(240, 220, 200, 255));
    const QImage zeroDecay = tlrender::composeEcho(
        pattern, QVector<QImage>{brighter}, 0.0, 2);
    reportGate("G7", "composeEcho decay=0 byte identity",
               imagesByteIdentical(pattern, zeroDecay), passed, failed);

    const QImage lightened = tlrender::composeEcho(
        pattern, QVector<QImage>{brighter}, 0.75, 2);
    bool lightenNeverDarkens = lightened.size() == pattern.size();
    for (int y = 0; lightenNeverDarkens && y < pattern.height(); ++y) {
        for (int x = 0; x < pattern.width(); ++x) {
            const QColor basePixel = pattern.pixelColor(x, y);
            const QColor outPixel = lightened.pixelColor(x, y);
            if (outPixel.red() < basePixel.red()
                || outPixel.green() < basePixel.green()
                || outPixel.blue() < basePixel.blue()) {
                lightenNeverDarkens = false;
                break;
            }
        }
    }
    reportGate("G8", "Lighten output channels are not below base",
               lightenNeverDarkens, passed, failed);

    EffectPreset preset;
    preset.name = QStringLiteral("fxgrain-echo-roundtrip");
    preset.effects = {
        VideoEffect::createFilmGrain(),
        VideoEffect::createEcho()
    };
    QJsonObject presetJson = preset.toJson();
    QJsonArray serializedEffects = presetJson.value("effects").toArray();
    const bool serializedTypeNames = serializedEffects.size() == 2
        && serializedEffects.at(0).toObject().value("type").toString()
            == QStringLiteral("FilmGrain")
        && serializedEffects.at(1).toObject().value("type").toString()
            == QStringLiteral("Echo");
    for (int i = 0; i < serializedEffects.size(); ++i) {
        QJsonObject effectJson = serializedEffects.at(i).toObject();
        effectJson.remove("typeId");
        effectJson.remove("typeName");
        serializedEffects[i] = effectJson;
    }
    presetJson["effects"] = serializedEffects;
    const EffectPreset restoredPreset = EffectPreset::fromJson(presetJson);
    const bool presetStringRoundTrip = serializedTypeNames
        && restoredPreset.effects.size() == 2
        && restoredPreset.effects.at(0).type == VideoEffectType::FilmGrain
        && restoredPreset.effects.at(1).type == VideoEffectType::Echo;
    reportGate("G9", "EffectPreset string round-trip preserves FilmGrain and Echo",
               presetStringRoundTrip, passed, failed);

    const QVector<effectctrl::ParamDef> echoSchema =
        effectctrl::paramSchemaFor(VideoEffectType::Echo);
    const auto blendDef = std::find_if(
        echoSchema.cbegin(), echoSchema.cend(),
        [](const effectctrl::ParamDef &def) { return def.name == "blend"; });
    bool blendModesValid = blendDef != echoSchema.cend()
        && blendDef->type == effectctrl::ParamType::Int
        && blendDef->minVal == 0.0
        && blendDef->maxVal == 3.0
        && blendDef->defaultVal == 2.0;

    QImage blendBase(1, 1, QImage::Format_RGBA8888);
    blendBase.fill(QColor(40, 80, 120, 255));
    QImage blendEcho(1, 1, QImage::Format_RGBA8888);
    blendEcho.fill(QColor(200, 100, 60, 255));
    const QVector<QColor> expectedBlendPixels = {
        QColor(240, 180, 180, 255),
        QColor(209, 149, 152, 255),
        QColor(200, 100, 120, 255),
        QColor(200, 100, 60, 255)
    };
    for (int blend = 0; blend <= 3 && blendModesValid; ++blend) {
        VideoEffect blendEffect = VideoEffect::createEcho(0.1, 1, 1.0, blend);
        effectctrl::setParamValue(blendEffect, QStringLiteral("blend"), blend);
        ClipInfo blendClip;
        blendClip.inPoint = 0.0;
        blendClip.effects = { blendEffect };
        const tlrender::EchoFrameProvider provider =
            [&blendEcho](double, double) { return blendEcho; };
        const QImage composed = tlrender::applyClipFxPackWithEcho(
            blendBase, blendClip, 1.0, 1.0, provider);
        blendModesValid = effectctrl::paramValue(
            blendEffect, QStringLiteral("blend")) == blend
            && composed.pixelColor(0, 0) == expectedBlendPixels.at(blend);
    }
    reportGate("G10", "Echo blend values 0..3 match schema and composition",
               blendModesValid, passed, failed);

    std::cerr << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    return failed;
}
