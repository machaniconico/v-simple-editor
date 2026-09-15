#include "../EffectParamSchema.h"
#include "../EffectPreset.h"
#include "../ShaderEffect.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoPlayer.h"

#include <QJsonDocument>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
constexpr int W = 320, H = 240;
constexpr double Fps = 30.0, Velocity = 40.0;

bool identical(const QImage &a, const QImage &b)
{
    if (a.isNull() || a.size() != b.size() || a.format() != b.format()
        || a.bytesPerLine() != b.bytesPerLine())
        return false;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        static_cast<std::size_t>(a.bytesPerLine())) != 0)
            return false;
    return true;
}

QVector<QImage> frames()
{
    // Fixed-seed textured scene under a horizontal camera pan. Texture and
    // the four-pixel white line share the same 40 px/frame motion and the
    // injected rate_in=1 rolling scan. No random runtime/global RNG state.
    QImage texture(1024, H, QImage::Format_RGBA8888);
    quint32 seed = 0x3052026u;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < texture.width(); ++x) {
            seed = seed * 1664525u + 1013904223u;
            const int gray = 24 + int((seed >> 24) & 127);
            texture.setPixelColor(x, y, QColor(gray, gray, gray));
        }
    }
    QVector<QImage> result;
    for (int frame = 0; frame < 8; ++frame) {
        QImage image(W, H, QImage::Format_RGBA8888);
        for (int y = 0; y < H; ++y) {
            const int shift = qRound(Velocity * frame + Velocity * (double(y) / H - 0.5));
            for (int x = 0; x < W; ++x) {
                const int sceneX = x - shift;
                image.setPixelColor(x, y, sceneX >= 12 && sceneX < 16
                    ? QColor(Qt::white)
                    : texture.pixelColor((sceneX + 1024) % 1024, y));
            }
        }
        result.append(image);
    }
    return result;
}

double lineCenter(const QImage &image, int y)
{
    double sum = 0.0, weight = 0.0;
    for (int x = 0; x < image.width(); ++x) {
        const double w = qMax(0, image.pixelColor(x, y).red() - 200);
        sum += x * w;
        weight += w;
    }
    return weight > 0.0 ? sum / weight : std::numeric_limits<double>::quiet_NaN();
}

double residual(const QImage &image)
{
    if (image.isNull())
        return std::numeric_limits<double>::infinity();
    return std::abs(lineCenter(image, H - 1) - lineCenter(image, 0));
}
} // namespace

int runRollingShutterSelftest()
{
    int pass = 0, fail = 0;
    auto gate = [&](int number, bool ok) {
        std::cerr << (ok ? "PASS G" : "FAIL G") << number << '\n';
        ok ? ++pass : ++fail;
    };
    Timeline timeline;
    ClipInfo clip;
    clip.duration = 8.0 / Fps;
    clip.outPoint = clip.duration;
    clip.effects = {VideoEffect::createRollingShutterRepair(1.0)};
    timeline.videoTracks().first()->setClips({clip});
    clip = timeline.videoTracks().first()->clips().first();
    const QVector<QImage> input = frames();
    QVector<double> requested;
    const tlrender::EchoFrameProvider provider = [&](double src, double local) {
        requested.append(src);
        const int index = qRound(src * Fps);
        return index >= 0 && index < input.size()
            ? tlrender::prepareClipSourceForEcho(input[index], clip, local) : QImage();
    };
    auto render = [&](int index) {
        return tlrender::applyClipFxStackWithEchoFromSource(
            input[index], clip, index / Fps, index / Fps, provider);
    };
    tlrender::setRollingShutterDisabledForTesting(false);
    tlrender::resetRollingShutterInvocationCountForTesting();
    const QVector<VideoEffect> savedEffects = clip.effects;
    clip.effects.clear();
    const bool defaultBypass = !tlrender::hasActiveRollingShutter(clip, 3.0 / Fps)
        && identical(render(3), input[3]);
    clip.effects = savedEffects;
    clip.effects[0].param1 = 0.0;
    const QImage zeroRate = render(3);
    bool bypass = !tlrender::hasActiveRollingShutter(clip, 3.0 / Fps)
        && identical(zeroRate, input[3]);
    clip.effects[0].param1 = 1.0;
    clip.effects[0].param3 = 0.0;
    bypass &= identical(render(3), input[3]);
    clip.effects[0].param3 = 1.0;
    clip.effects[0].enabled = false;
    bypass &= !tlrender::hasActiveRollingShutter(clip, 3.0 / Fps)
        && identical(render(3), input[3]);
    clip.effects[0].enabled = true;
    tlrender::setRollingShutterDisabledForTesting(true);
    bypass &= !tlrender::hasActiveRollingShutter(clip, 3.0 / Fps)
        && identical(render(3), input[3]);
    tlrender::setRollingShutterDisabledForTesting(false);
    gate(1, defaultBypass && bypass && requested.isEmpty()
        && tlrender::rollingShutterInvocationCountForTesting() == 0);

    const QImage repaired = render(3);
    const QImage preview = videopreview::prepareEchoClipForComposite(
        input[3], clip, 3.0 / Fps, 3.0 / Fps, provider, {});
    std::cerr << "central residual: " << residual(repaired) << " px\n";
    gate(2, tlrender::hasActiveRollingShutter(clip, 3.0 / Fps)
        && residual(input[3]) >= 39.0 && residual(repaired) <= Velocity * 0.2
        && identical(repaired, preview)
        && tlrender::rollingShutterInvocationCountForTesting() == 2);

    clip.effects[0].param2 = 1.0;
    const QImage reversed = render(3);
    std::cerr << "reverse residual: " << residual(reversed) << " px\n";
    gate(3, residual(reversed) >= Velocity);
    clip.effects[0].param2 = 0.0;

    requested.clear();
    const QImage last = render(7);
    std::cerr << "last residual: " << residual(last) << " px\n";
    gate(4, !last.isNull() && residual(last) <= Velocity * 0.3
        && requested.size() == 1 && std::abs(requested.first() - 6.0 / Fps) < 1e-9);
    gate(5, identical(repaired, render(3)) && identical(last, render(7)));

    effectctrl::setParamValue(clip.effects[0], "rate", 0.75);
    effectctrl::setParamValue(clip.effects[0], "direction", 1.0);
    effectctrl::setParamValue(clip.effects[0], "strength", 0.625);
    const VideoEffect decoded = PresetLibrary::videoEffectFromJson(
        QJsonDocument::fromJson(QJsonDocument(
            PresetLibrary::videoEffectToJson(clip.effects[0])).toJson()).object());
    const EffectPreset preset = EffectPreset::fromClipStack(
        QStringLiteral("ローリングシャッター補正"), clip, false);
    const EffectPreset restored = EffectPreset::fromJson(
        QJsonDocument::fromJson(QJsonDocument(preset.toJson()).toJson()).object());
    ClipInfo destination;
    destination.duration = clip.duration;
    restored.applyToClipStack(destination);
    const VideoEffect defaults = VideoEffect::createRollingShutterRepair();
    gate(6, decoded.type == VideoEffectType::RollingShutterRepair
        && decoded.param1 == 0.75 && decoded.param2 == 1.0 && decoded.param3 == 0.625
        && destination.effects.size() == 1
        && PresetLibrary::videoEffectToJson(destination.effects.first())
            == PresetLibrary::videoEffectToJson(clip.effects.first())
        && effectctrl::paramValue(decoded, "rate") == 0.75
        && effectctrl::paramValue(decoded, "direction") == 1.0
        && effectctrl::paramValue(decoded, "strength") == 0.625
        && defaults.param1 == 0.5 && defaults.param2 == 0.0 && defaults.param3 == 1.0
        && ShaderEffectLibrary::instance().findByName(
            QStringLiteral("RollingShutterRepair")).name.isEmpty());
    std::cerr << "summary: " << pass << " PASS, " << fail << " FAIL\n";
    return fail;
}
