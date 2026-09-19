#include "../VideoEffect.h"
#include "../EffectPreset.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoPlayer.h"
#include "../libavcore/Encode.h"
#include "../libavcore/FrameGrab.h"

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QSet>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace {
QImage source(const QSize &size)
{
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(QColor(30, 70, 110));
    return image;
}
VideoEffect maskedEffect(double feather = 0.0)
{
    VideoEffect effect;
    effect.type = VideoEffectType::Invert;
    Mask mask;
    mask.rect = QRectF(0.25, 0.25, 0.5, 0.5);
    mask.feather.amount = feather;
    effect.mask.addMask(mask);
    return effect;
}
bool same(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) return false;
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage y = b.convertToFormat(QImage::Format_RGBA8888);
    for (int row = 0; row < x.height(); ++row)
        if (std::memcmp(x.constScanLine(row), y.constScanLine(row), x.width() * 4)) return false;
    return true;
}
bool writeClip(const QString &path)
{
    libavcore::EncodeRequest request;
    request.width = 64;
    request.height = 36;
    request.fps = 10;
    request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoBitrateBits = 600000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    for (int i = 0; i < 10; ++i)
        if (!encoder.pushFrame(source(QSize(64, 36)).convertToFormat(QImage::Format_RGB888), i)) return false;
    return !encoder.finalize().has_value();
}
}

int runEffectMaskSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    const QImage input = source(QSize(640, 360));
    VideoEffect plain;
    plain.type = VideoEffectType::Invert;
    VideoEffectProcessor::setEffectMaskDisabledForTesting(true);
    const QImage baseline = VideoEffectProcessor::applyEffect(input, plain);
    VideoEffectProcessor::setEffectMaskDisabledForTesting(false);
    const QImage ordinary = VideoEffectProcessor::applyEffect(input, plain);
    gate(1, same(baseline, ordinary) && plain.mask.isDefault()
         && VideoEffectProcessor::effectMaskInvocationCountForTesting() == 0);

    const VideoEffect effect = maskedEffect();
    const QImage masked = VideoEffectProcessor::applyEffect(input, effect);
    gate(2, !effect.mask.isDefault() && masked.pixelColor(320, 180) == baseline.pixelColor(320, 180)
         && masked.pixelColor(30, 180) == input.pixelColor(30, 180)
         && VideoEffectProcessor::effectMaskInvocationCountForTesting() == 1);

    const QImage feathered = VideoEffectProcessor::applyEffect(input, maskedEffect(0.05));
    QSet<int> levels;
    bool monotonic = true;
    int previous = 0;
    for (int x = 130; x < 195; ++x) {
        const int value = feathered.pixelColor(x, 180).red();
        levels.insert(value);
        monotonic &= value >= previous;
        previous = value;
    }
    gate(3, monotonic && levels.size() > 8);

    ClipInfo clip;
    clip.filePath = QStringLiteral("effect-mask.mp4");
    clip.duration = clip.outPoint = 1.0;
    clip.effects = {effect, plain};
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{{clip}};
    ProjectData restored;
    const QString json = ProjectFile::toJsonString(data);
    QTemporaryDir temporary;
    const QString project = temporary.filePath(QStringLiteral("mask.veditor"));
    ProjectData fileRestored;
    auto restoredMasks = [&](const ProjectData &d) {
        return d.videoTracks.size() == 1 && d.videoTracks.first().size() == 1
            && d.videoTracks.first().first().effects.size() == 2
            && d.videoTracks.first().first().effects.first().mask.toJson() == effect.mask.toJson()
            && d.videoTracks.first().first().effects.last().mask.isDefault();
    };
    const bool roundtrip = ProjectFile::fromJsonString(json, restored) && restoredMasks(restored)
        && ProjectFile::save(project, data) && ProjectFile::load(project, fileRestored)
        && restoredMasks(fileRestored);
    clip.effects = {plain};
    data.videoTracks = QVector<QVector<ClipInfo>>{{clip}};
    gate(4, roundtrip && !ProjectFile::toJsonString(data).contains(QStringLiteral("\"mask\"")));

    const QString video = temporary.filePath(QStringLiteral("source.mp4"));
    bool parity = writeClip(video);
    if (parity) {
        clip.filePath = video;
        clip.effects = {effect};
        Timeline timeline;
        timeline.restoreFromProject(QVector<QVector<ClipInfo>>{{clip}},
            QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
        const QImage decoded = libavcore::grabFrameAt(video, 0.2);
        const QImage preview = videopreview::prepareEchoClipForComposite(
            decoded, clip, 0.2, 0.2, {}, {});
        const QImage rendered = tlrender::renderFrameAt(&timeline, 200000, QSize(64, 36));
        parity = same(preview, rendered)
            && videopreview::stackRequiresClipLocalCpu(clip.effects, true);
        // The default render path must not enter the new branch either.
        clip.effects = {plain};
        timeline.videoTracks().first()->setClips({clip});
        timeline.refreshPlaybackSequence();
        VideoEffectProcessor::setEffectMaskDisabledForTesting(true);
        const QImage disabled = tlrender::renderFrameAt(&timeline, 200000, QSize(64, 36));
        VideoEffectProcessor::setEffectMaskDisabledForTesting(false);
        parity &= same(disabled, tlrender::renderFrameAt(&timeline, 200000, QSize(64, 36)))
            && VideoEffectProcessor::effectMaskInvocationCountForTesting() == 0;
    }
    gate(5, parity);

    const QImage large = VideoEffectProcessor::applyEffect(source(QSize(1920, 1080)), effect);
    bool scaled = true;
    for (const QPoint &p : {QPoint(50, 180), QPoint(200, 180), QPoint(320, 180), QPoint(550, 180)})
        scaled &= masked.pixelColor(p) == large.pixelColor(p * 3);
    const QImage largeFeather = VideoEffectProcessor::applyEffect(source(QSize(1920, 1080)), maskedEffect(0.05));
    for (int x = 140; x < 181; x += 5)
        scaled &= std::abs(feathered.pixelColor(x, 180).red()
                          - largeFeather.pixelColor(x * 3 + 1, 541).red()) <= 6;
    const QJsonObject preset = PresetLibrary::videoEffectToJson(maskedEffect(0.05));
    const VideoEffect reloaded = PresetLibrary::videoEffectFromJson(preset);
    EffectPreset stackPreset;
    stackPreset.name = QStringLiteral("マスク往復");
    stackPreset.effects = {maskedEffect(0.05), plain};
    const EffectPreset restoredPreset = EffectPreset::fromJson(stackPreset.toJson());
    const bool presetStack = restoredPreset.effects.size() == 2
        && restoredPreset.effects.first().mask.toJson() == maskedEffect(0.05).mask.toJson()
        && restoredPreset.effects.last().mask.isDefault();
    gate(6, scaled && presetStack && reloaded.mask.toJson() == maskedEffect(0.05).mask.toJson()
        && same(feathered, VideoEffectProcessor::applyEffect(input, reloaded))
        && !PresetLibrary::videoEffectToJson(plain).contains(QStringLiteral("mask")));
    VideoEffectProcessor::setEffectMaskDisabledForTesting(false);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
