#include "../EffectLibraryModel.h"
#include "../LayerCompositor.h"
#include "../VfxGenerators.h"
#include "SelftestRegistry.h"

#include <QImage>
#include <QSet>
#include <QUrl>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

const QSize kCanvasSize(320, 180);

bool sameImage(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()
        || a.sizeInBytes() != b.sizeInBytes()) {
        return false;
    }
    if (a.sizeInBytes() <= 0)
        return true;
    return std::memcmp(a.constBits(), b.constBits(),
                       static_cast<size_t>(a.sizeInBytes())) == 0;
}

bool hasVisiblePixel(const QImage &image)
{
    if (image.isNull())
        return false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0)
                return true;
        }
    }
    return false;
}

bool firstVisiblePixel(const QImage &image, int *xOut, int *yOut)
{
    if (!xOut || !yOut)
        return false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                *xOut = x;
                *yOut = y;
                return true;
            }
        }
    }
    return false;
}

bool isPremultipliedAndBounded(const QImage &image)
{
    if (image.isNull() || image.format() != QImage::Format_ARGB32_Premultiplied)
        return false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            const int alpha = qAlpha(pixel);
            if (alpha < 0 || alpha > 255
                || qRed(pixel) > alpha
                || qGreen(pixel) > alpha
                || qBlue(pixel) > alpha) {
                return false;
            }
        }
    }
    return true;
}

double distance(const QPointF &a, const QPointF &b)
{
    const double dx = a.x() - b.x();
    const double dy = a.y() - b.y();
    return std::sqrt(dx * dx + dy * dy);
}

bool allLibraryVfxEntries(const efxlib::EffectLibraryModel &model)
{
    const QVector<VfxGeneratorType> types = VfxGenerators::allTypes();
    if (static_cast<int>(types.size()) != 7)
        return false;
    for (const VfxGeneratorType type : types) {
        const QString id = QStringLiteral("vfx:%1").arg(
            QString::fromLatin1(QUrl::toPercentEncoding(VfxGenerators::typeName(type))));
        efxlib::LibraryEntry entry;
        if (!model.entryById(id, &entry)
            || entry.kind != efxlib::SourceKind::VfxGenerator) {
            return false;
        }
    }
    return true;
}

} // namespace

int runVfxGeneratorsSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        const QByteArray detailUtf8 = detail.toUtf8();
        std::printf("[vfx-generators] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL", gate, name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : detailUtf8.constData());
        if (ok)
            ++passed;
        else
            ++failed;
    };

    const QVector<VfxGeneratorType> types = VfxGenerators::allTypes();

    // G1: every generator draws visible output with its own default params.
    bool allNonEmpty = static_cast<int>(types.size()) == 7;
    for (const VfxGeneratorType type : types)
        allNonEmpty = allNonEmpty
            && hasVisiblePixel(VfxGenerators::renderDefault(type, kCanvasSize, 0.0));
    check(1, "all seven defaults render visible RGBA", allNonEmpty);

    // G2: the same params, seed, and relative time produce byte-identical
    // output. This specifically catches hidden global/random device state.
    bool deterministic = true;
    for (const VfxGeneratorType type : types) {
        const VfxGeneratorParameters params = VfxGenerators::defaultParameters(type);
        const double sampleTime = qMin(0.37,
            VfxGenerators::durationSeconds(type, params) * 0.35);
        const QImage a = VfxGenerators::render(type, kCanvasSize, params, sampleTime);
        const QImage b = VfxGenerators::render(type, kCanvasSize, params, sampleTime);
        deterministic = deterministic && sameImage(a, b);
    }
    check(2, "same seed and time are byte deterministic", deterministic);

    // G3: changing the seed changes the two generators whose geometry is
    // explicitly stochastic.
    ExplosionParameters explosionA;
    ExplosionParameters explosionB = explosionA;
    explosionB.seed += 1u;
    LightningParameters lightningA;
    LightningParameters lightningB = lightningA;
    lightningB.seed += 1u;
    const bool seedDiffers = !sameImage(
        VfxGenerators::renderExplosion(kCanvasSize, 0.42, explosionA),
        VfxGenerators::renderExplosion(kCanvasSize, 0.42, explosionB))
        && !sameImage(
            VfxGenerators::renderLightning(kCanvasSize, 0.42, lightningA),
            VfxGenerators::renderLightning(kCanvasSize, 0.42, lightningB));
    check(3, "seed changes Explosion and Lightning", seedDiffers);

    // G4: generators are driven by relative seconds, not a frozen still.
    bool evolves = true;
    for (const VfxGeneratorType type : types) {
        const VfxGeneratorParameters params = VfxGenerators::defaultParameters(type);
        evolves = evolves && !sameImage(
            VfxGenerators::render(type, kCanvasSize, params, 0.0),
            VfxGenerators::render(type, kCanvasSize, params, 0.5));
    }
    check(4, "relative time changes every generator", evolves);

    // G5: midpoint-displaced Lightning still starts and ends at the requested
    // coordinates. Geometry is in pixels, matching the render canvas.
    const LightningGeometry lightningGeometry =
        VfxGenerators::lightningGeometry(kCanvasSize, lightningA);
    const QPointF expectedStart(lightningA.start.x() * kCanvasSize.width(),
                                lightningA.start.y() * kCanvasSize.height());
    const QPointF expectedEnd(lightningA.end.x() * kCanvasSize.width(),
                              lightningA.end.y() * kCanvasSize.height());
    const bool connected = !lightningGeometry.mainPath.isEmpty()
        && distance(lightningGeometry.mainPath.first(), expectedStart) < 1e-9
        && distance(lightningGeometry.mainPath.last(), expectedEnd) < 1e-9;
    check(5, "Lightning main path connects endpoints", connected);

    // G6: branch probability is an actual control, not a cosmetic parameter.
    LightningParameters noBranches = lightningA;
    noBranches.branchProbability = 0.0;
    LightningParameters manyBranches = lightningA;
    manyBranches.branchProbability = 1.0;
    const bool branching =
        VfxGenerators::lightningGeometry(kCanvasSize, noBranches).branches.isEmpty()
        && !VfxGenerators::lightningGeometry(kCanvasSize, manyBranches).branches.isEmpty();
    check(6, "Lightning branch probability controls branches", branching);

    // G7: ShockWave radius is monotone in seconds.
    ShockWaveParameters shock;
    const double shockRadius0 = VfxGenerators::shockWaveRadius(shock, 0.0);
    const double shockRadius1 = VfxGenerators::shockWaveRadius(shock, 0.5);
    const double shockRadius2 = VfxGenerators::shockWaveRadius(shock, 1.0);
    check(7, "ShockWave radius grows monotonically",
          shockRadius0 <= shockRadius1 && shockRadius1 <= shockRadius2
              && shockRadius2 > shockRadius0);

    // G8: ring angles use their own speeds, including direction.
    MagicCircleParameters circle;
    circle.rotationSpeeds = QVector<double>{0.5, -0.9, 0.15};
    const MagicCircleGeometry circle0 =
        VfxGenerators::magicCircleGeometry(kCanvasSize, circle, 0.0);
    const MagicCircleGeometry circle1 =
        VfxGenerators::magicCircleGeometry(kCanvasSize, circle, 0.5);
    bool rotatesDifferently = static_cast<int>(circle0.rings.size()) == circle.ringCount
        && static_cast<int>(circle1.rings.size()) == circle.ringCount;
    for (int i = 0; rotatesDifferently && i < circle.ringCount; ++i) {
        const double observedDelta = circle1.rings[i].angleRadians
            - circle0.rings[i].angleRadians;
        const double expectedDelta = circle.rotationSpeeds[i] * 0.5;
        rotatesDifferently = std::abs(observedDelta - expectedDelta) < 1e-9;
    }
    rotatesDifferently = rotatesDifferently
        && std::abs(circle.rotationSpeeds[0] - circle.rotationSpeeds[1]) > 1e-9
        && std::abs(circle.rotationSpeeds[1] - circle.rotationSpeeds[2]) > 1e-9;
    check(8, "MagicCircle rings use distinct rotation speeds", rotatesDifferently);

    // G9: MuzzleFlash observes its declared frame-rate-based lifetime.
    MuzzleFlashParameters muzzle;
    muzzle.durationFrames = 2;
    muzzle.frameRate = 24.0;
    const bool shortLived = hasVisiblePixel(
        VfxGenerators::renderMuzzleFlash(kCanvasSize, 0.0, muzzle))
        && !hasVisiblePixel(VfxGenerators::renderMuzzleFlash(
            kCanvasSize, muzzle.lifetimeSeconds() + 0.001, muzzle));
    check(9, "MuzzleFlash becomes transparent after duration", shortLived);

    // G10: all output stays premultiplied and bounded for the compositor.
    bool alphaValid = true;
    for (const VfxGeneratorType type : types) {
        const VfxGeneratorParameters params = VfxGenerators::defaultParameters(type);
        const double sampleTime = qMin(0.37,
            VfxGenerators::durationSeconds(type, params) * 0.35);
        alphaValid = alphaValid && isPremultipliedAndBounded(
            VfxGenerators::render(type, kCanvasSize, params, sampleTime));
    }
    check(10, "RGBA alpha is bounded and premultiplied", alphaValid);

    // G11: 24fps and 60fps sampling of the same relative second is identical.
    bool fpsIndependent = true;
    for (const VfxGeneratorType type : types) {
        VfxGeneratorParameters params = VfxGenerators::defaultParameters(type);
        double targetSeconds = 0.5;
        if (type == VfxGeneratorType::MuzzleFlash) {
            auto &muzzle = std::get<MuzzleFlashParameters>(params);
            muzzle.durationFrames = 3;
            muzzle.frameRate = 24.0;
            targetSeconds = 1.0 / 12.0;
        }
        const int frameAt24 = qRound(targetSeconds * 24.0);
        const int frameAt60 = qRound(targetSeconds * 60.0);
        const QImage at24 = VfxGenerators::render(type, kCanvasSize, params,
            static_cast<double>(frameAt24) / 24.0);
        const QImage at60 = VfxGenerators::render(type, kCanvasSize, params,
            static_cast<double>(frameAt60) / 60.0);
        fpsIndependent = fpsIndependent && sameImage(at24, at60);
    }
    check(11, "same relative seconds are fps independent", fpsIndependent);

    // G12: the existing KeyframeManager controls a generator scalar in
    // seconds, proving the keyframe seam is live rather than decorative.
    ExplosionParameters keyed;
    KeyframeTrack scaleTrack(QStringLiteral("scale"), keyed.scale);
    scaleTrack.addKeyframe(0.0, 0.08);
    scaleTrack.addKeyframe(1.0, 0.62);
    keyed.keyframes.addTrack(scaleTrack);
    check(12, "KeyframeManager animates generator parameters",
          !sameImage(VfxGenerators::renderExplosion(kCanvasSize, 0.0, keyed),
                     VfxGenerators::renderExplosion(kCanvasSize, 1.0, keyed)));

    // G13: each generator has a live KeyframeManager seam, not just a field
    // that is stored and ignored. Every comparison is at one identical time.
    bool allKeyframesLive = true;
    {
        ExplosionParameters p;
        ExplosionParameters keyedP = p;
        KeyframeTrack track(QStringLiteral("scale"), p.scale);
        track.addKeyframe(0.0, 0.08);
        track.addKeyframe(0.3, 0.62);
        keyedP.keyframes.addTrack(track);
        allKeyframesLive = allKeyframesLive
            && !sameImage(VfxGenerators::renderExplosion(kCanvasSize, 0.3, p),
                          VfxGenerators::renderExplosion(kCanvasSize, 0.3, keyedP));
    }
    {
        LightningParameters p;
        LightningParameters keyedP = p;
        KeyframeTrack track(QStringLiteral("coreWidth"), p.coreWidth);
        track.addKeyframe(0.0, 1.0);
        track.addKeyframe(0.3, 10.0);
        keyedP.keyframes.addTrack(track);
        allKeyframesLive = allKeyframesLive
            && !sameImage(VfxGenerators::renderLightning(kCanvasSize, 0.3, p),
                          VfxGenerators::renderLightning(kCanvasSize, 0.3, keyedP));
    }
    {
        ShockWaveParameters p;
        ShockWaveParameters keyedP = p;
        KeyframeTrack track(QStringLiteral("speed"), p.speed);
        track.addKeyframe(0.0, 30.0);
        track.addKeyframe(0.3, 700.0);
        keyedP.keyframes.addTrack(track);
        allKeyframesLive = allKeyframesLive
            && !sameImage(VfxGenerators::renderShockWave(kCanvasSize, 0.3, p),
                          VfxGenerators::renderShockWave(kCanvasSize, 0.3, keyedP));
    }
    {
        EnergyBeamParameters p;
        EnergyBeamParameters keyedP = p;
        KeyframeTrack track(QStringLiteral("flowSpeed"), p.flowSpeed);
        track.addKeyframe(0.0, 0.2);
        track.addKeyframe(0.3, 9.0);
        keyedP.keyframes.addTrack(track);
        allKeyframesLive = allKeyframesLive
            && !sameImage(VfxGenerators::renderEnergyBeam(kCanvasSize, 0.3, p),
                          VfxGenerators::renderEnergyBeam(kCanvasSize, 0.3, keyedP));
    }
    {
        MagicCircleParameters p;
        MagicCircleParameters keyedP = p;
        KeyframeTrack track(QStringLiteral("ringSpeed.0"), p.rotationSpeeds[0]);
        track.addKeyframe(0.0, 0.1);
        track.addKeyframe(0.3, 3.0);
        keyedP.keyframes.addTrack(track);
        allKeyframesLive = allKeyframesLive
            && !sameImage(VfxGenerators::renderMagicCircle(kCanvasSize, 0.3, p),
                          VfxGenerators::renderMagicCircle(kCanvasSize, 0.3, keyedP));
    }
    {
        MuzzleFlashParameters p;
        MuzzleFlashParameters keyedP = p;
        KeyframeTrack track(QStringLiteral("scale"), p.scale);
        track.addKeyframe(0.0, 0.08);
        track.addKeyframe(0.02, 0.48);
        keyedP.keyframes.addTrack(track);
        allKeyframesLive = allKeyframesLive
            && !sameImage(VfxGenerators::renderMuzzleFlash(kCanvasSize, 0.02, p),
                          VfxGenerators::renderMuzzleFlash(kCanvasSize, 0.02, keyedP));
    }
    {
        EnergyShieldParameters p;
        EnergyShieldParameters keyedP = p;
        KeyframeTrack track(QStringLiteral("edgeSharpness"), p.edgeSharpness);
        track.addKeyframe(0.0, 0.8);
        track.addKeyframe(0.3, 10.0);
        keyedP.keyframes.addTrack(track);
        allKeyframesLive = allKeyframesLive
            && !sameImage(VfxGenerators::renderEnergyShield(kCanvasSize, 0.3, p),
                          VfxGenerators::renderEnergyShield(kCanvasSize, 0.3, keyedP));
    }
    check(13, "Keyframes affect all seven generators", allKeyframesLive);

    // G14: registry names are unique and cover exactly the seven public types.
    QSet<QString> names;
    bool registryOk = static_cast<int>(types.size()) == 7;
    for (const VfxGeneratorType type : types) {
        const QString name = VfxGenerators::typeName(type);
        registryOk = registryOk && !name.isEmpty() && !names.contains(name);
        names.insert(name);
    }
    check(14, "generator registry has seven unique names", registryOk);

    // G15: a generated overlay is a first-class input to the existing Screen
    // blend path, not a special-case opaque image.
    const QImage base(kCanvasSize, QImage::Format_ARGB32_Premultiplied);
    QImage screenBase = base;
    screenBase.fill(QColor(8, 10, 14, 255));
    const QImage overlay = VfxGenerators::renderDefault(
        VfxGeneratorType::EnergyBeam, kCanvasSize, 0.25);
    const QImage composited = LayerCompositor::blendImages(
        screenBase, overlay, BlendMode::Screen, 1.0);
    const QImage expectedBase = screenBase.convertToFormat(QImage::Format_ARGB32);
    const QImage expectedTop = overlay.convertToFormat(QImage::Format_ARGB32);
    int visibleX = -1;
    int visibleY = -1;
    const bool hasOverlayPixel = firstVisiblePixel(expectedTop, &visibleX, &visibleY);
    const QRgb expectedPixel = hasOverlayPixel
        ? LayerCompositor::blendPixel(expectedBase.pixel(visibleX, visibleY),
                                      expectedTop.pixel(visibleX, visibleY),
                                      BlendMode::Screen, 1.0)
        : qRgba(0, 0, 0, 0);
    const bool screenPixelMatches = hasOverlayPixel
        && !composited.isNull() && composited.size() == kCanvasSize
        && composited.pixel(visibleX, visibleY) == expectedPixel
        && composited.pixel(visibleX, visibleY) != expectedBase.pixel(visibleX, visibleY);
    check(15, "RGBA generator overlays use Screen blend mode", screenPixelMatches);

    // G16: VFX-A's dynamic effect library walks the generator registry.
    efxlib::EffectLibraryModel library;
    check(16, "EffectLibraryModel registers all generators",
          allLibraryVfxEntries(library));

    // G17: ShockWave must displace the source image on the ring, not merely
    // return a separately drawable circle. Pixels well outside the ring must
    // remain byte-identical so the operation is safe on transparent footage.
    QImage source(kCanvasSize, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixel(x, y, qRgba((x * 13 + y * 3) % 256,
                                        (x * 5 + y * 17) % 256,
                                        (x * 29 + y * 7) % 256, 255));
        }
    }
    ShockWaveParameters displacementParameters;
    displacementParameters.initialRadius = 30.0;
    displacementParameters.speed = 0.0;
    displacementParameters.ringWidth = 12.0;
    displacementParameters.distortionStrength = 36.0;
    displacementParameters.decay = 0.0;
    QImage displaced = VfxGenerators::applyShockWave(
        source, 0.0, displacementParameters);
    int changedPixels = 0;
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            if (source.pixel(x, y) != displaced.pixel(x, y))
                ++changedPixels;
        }
    }
    const bool shockDisplacesSource = !displaced.isNull()
        && changedPixels > 0
        && source.pixel(0, 0) == displaced.pixel(0, 0);
    check(17, "ShockWave applies localized radial UV displacement",
          shockDisplacesSource);

    // G18: every registered generator is executable through the library's
    // public image-application route, including the source-image ShockWave
    // special case.
    const QImage librarySource = efxlib::EffectLibraryModel::testPattern(
        kCanvasSize);
    bool libraryAppliesVfx = true;
    for (const VfxGeneratorType type : types) {
        const QString id = QStringLiteral("vfx:%1").arg(
            QString::fromLatin1(QUrl::toPercentEncoding(
                VfxGenerators::typeName(type))));
        QImage applied;
        libraryAppliesVfx = libraryAppliesVfx
            && library.applyToImage(id, librarySource, &applied)
            && !applied.isNull()
            && !sameImage(applied, librarySource);
    }
    check(18, "registered generators apply through library image route",
          libraryAppliesVfx);

    std::printf("[vfx-generators] RESULT passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
