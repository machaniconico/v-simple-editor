#include "../EffectLibraryModel.h"
#include "../EffectParamSchema.h"
#include "../EffectPreset.h"
#include "../ShapeLayer.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../UndoManager.h"
#include "../VideoEffect.h"
#include "../VideoPlayer.h"

#include <QJsonDocument>
#include <cstdio>
#include <cstring>

namespace {
bool identical(const QImage &a, const QImage &b)
{
    return !a.isNull() && a.size() == b.size() && a.format() == b.format()
        && a.sizeInBytes() == b.sizeInBytes()
        && std::memcmp(a.constBits(), b.constBits(), size_t(a.sizeInBytes())) == 0;
}

QImage pattern(QImage::Format format)
{
    // Eight pixels avoids undefined RGB888 row padding in involution checks.
    QImage image(8, 5, format);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(17 + x * 29, 11 + y * 47,
                                            9 + x * 13 + y * 17, 80 + x * 19));
    return image;
}
} // namespace

int runFlipSolidSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "[flip-solid] %s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    VideoEffect flip;
    flip.type = VideoEffectType::Flip;

    bool bypass = true;
    for (const auto format : {QImage::Format_RGB888, QImage::Format_RGBA8888}) {
        // Initialize every byte, including padding, for strict bypass identity.
        QImage input(7, 5, format);
        for (qsizetype i = 0; i < input.sizeInBytes(); ++i)
            input.bits()[i] = static_cast<uchar>((i * 37 + 19) & 255);
        VideoEffectProcessor::setFlipEnabledForTesting(false);
        bypass &= identical(input, VideoEffectProcessor::applyEffect(input, flip))
            && VideoEffectProcessor::flipInvocationCountForTesting() == 0;
        VideoEffectProcessor::setFlipEnabledForTesting(true);
        VideoEffect disabled = flip;
        disabled.enabled = false;
        bypass &= identical(input, VideoEffectProcessor::applyEffectStack(input, {}, {}))
            && identical(input, VideoEffectProcessor::applyEffectStack(input, {}, {disabled}))
            && VideoEffectProcessor::flipInvocationCountForTesting() == 0;
    }
    gate(1, bypass);

    bool mapping = true, involution = true;
    efxlib::EffectLibraryModel library;
    QString flipId;
    for (const auto &entry : library.entries()) {
        if (entry.displayName == QStringLiteral("反転 (Flip/Flop)")
            && entry.category == QStringLiteral("ディストーション"))
            flipId = entry.id;
    }
    mapping &= !flipId.isEmpty();
    const auto schema = effectctrl::paramSchemaFor(VideoEffectType::Flip);
    mapping &= schema.size() == 1 && schema.first().name == QStringLiteral("mode")
        && schema.first().type == effectctrl::ParamType::Int
        && schema.first().minVal == 0.0 && schema.first().maxVal == 2.0;
    for (int mode = 0; mode < 3; ++mode) {
        effectctrl::setParamValue(flip, QStringLiteral("mode"), mode);
        mapping &= flip.param1 == mode
            && effectctrl::paramValue(flip, QStringLiteral("mode")) == mode;
        for (const auto format : {QImage::Format_RGB888, QImage::Format_RGBA8888}) {
            const QImage input = pattern(format);
            const QImage output = VideoEffectProcessor::applyEffect(input, flip);
            mapping &= output.size() == input.size() && output.format() == input.format();
            if (output.size() == input.size()) {
                // Check every pixel, including all four asymmetric corners and alpha.
                for (int y = 0; y < input.height(); ++y)
                    for (int x = 0; x < input.width(); ++x)
                        mapping &= input.pixelColor(x, y) == output.pixelColor(
                            mode == 1 ? x : input.width() - 1 - x,
                            mode == 0 ? y : input.height() - 1 - y);
            }
            involution &= identical(input, VideoEffectProcessor::applyEffect(output, flip));

            ClipInfo clip;
            mapping &= library.setParameterOverride(flipId, QStringLiteral("mode"), mode)
                && library.applyToClip(flipId, clip);
            mapping &= clip.effects.size() == 1
                && videopreview::stackRequiresClipLocalCpu(clip.effects, true);
            VideoEffectProcessor::setFlipEnabledForTesting(true);
            const QImage preview = videopreview::prepareEchoClipForComposite(
                input, clip, 0.0, 0.0, {}, {});
            const int previewCalls = VideoEffectProcessor::flipInvocationCountForTesting();
            VideoEffectProcessor::setFlipEnabledForTesting(true);
            const QImage exported = tlrender::applyClipFxStackFromSource(input, clip, 0.0);
            mapping &= previewCalls == 1
                && VideoEffectProcessor::flipInvocationCountForTesting() == 1
                && identical(preview, exported)
                && identical(output.convertToFormat(QImage::Format_RGBA8888), exported);
            clip.effects.append(flip);
            const QImage rgba = input.convertToFormat(QImage::Format_RGBA8888);
            involution &= identical(rgba, tlrender::applyClipFxStackFromSource(input, clip, 0.0))
                && identical(rgba, videopreview::prepareEchoClipForComposite(
                    input, clip, 0.0, 0.0, {}, {}));
        }
    }
    gate(2, mapping);
    gate(3, involution);

    bool presets = true;
    for (int mode = 0; mode < 3; ++mode) {
        flip.param1 = mode;
        flip.enabled = mode != 1;
        EffectPreset preset;
        preset.name = QStringLiteral("Flip");
        preset.effects = {flip};
        const auto json = preset.toJson();
        const auto restored = EffectPreset::fromJson(
            QJsonDocument::fromJson(QJsonDocument(json).toJson()).object());
        presets &= restored.effects.size() == 1 && restored.name == preset.name
            && QJsonDocument(restored.toJson()).toJson() == QJsonDocument(json).toJson();
        QJsonObject byName = PresetLibrary::videoEffectToJson(flip);
        presets &= byName.value(QStringLiteral("type")).toString() == QStringLiteral("Flip");
        byName.remove(QStringLiteral("typeId"));
        const VideoEffect named = PresetLibrary::videoEffectFromJson(byName);
        presets &= named.type == VideoEffectType::Flip && named.param1 == mode
            && named.enabled == flip.enabled;
    }
    gate(4, presets);

    bool solid = true;
    for (const QColor color : {QColor(Qt::white), QColor(17, 91, 203)}) {
        Timeline timeline;
        const double start = color == QColor(Qt::white) ? 0.0 : 1.25;
        const double duration = color == QColor(Qt::white) ? 5.0 : 2.75;
        timeline.setPlayheadPosition(start);
        ShapeFill fill;
        fill.color = color;
        fill.enabled = true;
        ShapeStroke stroke;
        stroke.enabled = false;
        Shape shape = ShapeLayer::createRectangle(QSizeF(1920, 1080), fill, stroke);
        shape.position = QPointF(960, 540);
        ClipInfo clip;
        clip.displayName = QStringLiteral("平面 %1").arg(color.name(QColor::HexRgb).toUpper());
        clip.duration = duration;
        clip.inPoint = 0.0;
        clip.outPoint = duration;
        clip.shapes = {shape};
        const quint64 before = timeline.undoManager()->saveSerial();
        solid &= timeline.insertShapeClipAtPlayhead(clip);
        const auto clips = timeline.videoTracks().first()->clips();
        solid &= clips.size() == 1 && clips.first().leadInSec == start
            && clips.first().effectiveDuration() == duration
            && clips.first().displayName == clip.displayName
            && timeline.undoManager()->saveSerial() == before + 1;
        timeline.refreshPlaybackSequence();
        const QImage rendered = tlrender::renderFrameAt(
            &timeline, qRound64((start + 0.5) * 1000000), QSize(1920, 1080));
        solid &= !rendered.isNull() && rendered.size() == QSize(1920, 1080);
        for (int y = 0; y < rendered.height(); ++y)
            for (int x = 0; x < rendered.width(); ++x)
                solid &= rendered.pixelColor(x, y) == color;
        timeline.undo();
        for (const auto *track : timeline.videoTracks())
            solid &= track->clips().isEmpty();
        solid &= !timeline.undoManager()->canUndo();
    }
    gate(5, solid);
    VideoEffectProcessor::setFlipEnabledForTesting(true);
    std::fprintf(stderr, "[flip-solid] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
