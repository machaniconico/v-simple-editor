// US-200: run from the repository root in both baseline and HEAD builds.
// Compare separate VEDITOR_RENDER_FIXTURE_OUT directories with diff -r.
// Fixture (b) intentionally captures the US-201 transition defect correction.
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <cstdio>
#include <initializer_list>

namespace {
const QSize kSize(640, 360);
const QColor kSecondColor(32, 192, 64);

ClipInfo clip(const QString &path, double in, double out)
{
    ClipInfo c;
    c.filePath = path;
    c.displayName = QStringLiteral("render-fixture");
    c.duration = 5.0;
    c.inPoint = in;
    c.outPoint = out;
    // All optional features retain their OFF defaults (including hueSatWarp
    // and camera projection); only the legacy features below are enabled.
    return c;
}

QByteArray rgbaBytes(const QImage &image)
{
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QByteArray bytes;
    for (int y = 0; y < rgba.height(); ++y)
        bytes.append(reinterpret_cast<const char *>(rgba.constScanLine(y)), rgba.width() * 4);
    return bytes;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool written = file.write(bytes) == bytes.size();
    return file.flush() && written;
}

// QJsonObject serializes keys in sorted order. Include even default-valued
// fields so changes to the production playback descriptor remain observable.
QJsonObject audioEntry(const PlaybackEntry &e, int index)
{
    QJsonObject o;
#define FIELD(name) o.insert(QStringLiteral(#name), e.name)
#define ENUM_FIELD(name) o.insert(QStringLiteral(#name), static_cast<int>(e.name))
    FIELD(filePath); FIELD(clipIn); FIELD(clipOut);
    FIELD(timelineStart); FIELD(timelineEnd); FIELD(speed); FIELD(sourceTrack);
    FIELD(audioMuted); FIELD(videoScale); FIELD(videoDx); FIELD(videoDy);
    FIELD(rotation2DDegrees); FIELD(opacity); FIELD(isVfxFootage);
    ENUM_FIELD(blendMode); FIELD(vfxIntensity); FIELD(vfxBlackLevel);
    FIELD(fitContain); FIELD(fitCover); FIELD(volume); FIELD(pan);
    FIELD(sourceClipIndex); FIELD(matteTypeOrdinal); FIELD(matteSourceClipId);
    FIELD(parentClipId); ENUM_FIELD(leadInType); FIELD(leadInDuration);
    ENUM_FIELD(leadInEasing); ENUM_FIELD(trailOutType); FIELD(trailOutDuration);
    ENUM_FIELD(trailOutEasing);
#undef ENUM_FIELD
#undef FIELD
    o.insert(QStringLiteral("colorMeta"), clipcolor::toJson(e.colorMeta));
    o.insert(QStringLiteral("layerStyle"), e.layerStyle.toJson());
    QJsonArray envelope;
    for (const auto &p : e.volumeEnvelope)
        envelope.append(QJsonObject{{QStringLiteral("time"), p.time}, {QStringLiteral("gain"), p.gain}});
    o.insert(QStringLiteral("volumeEnvelope"), envelope);
    QJsonArray stabilizer;
    for (const auto &p : e.stabilizerKeyframes)
        stabilizer.append(QJsonObject{{QStringLiteral("timeUs"), p.timeUs},
            {QStringLiteral("dx"), p.dx}, {QStringLiteral("dy"), p.dy},
            {QStringLiteral("theta"), p.theta}, {QStringLiteral("scale"), p.scale}});
    o.insert(QStringLiteral("stabilizerKeyframes"), stabilizer);
    o.insert(QStringLiteral("filterChain"), buildExportAudioMixEntryFilterChain(
        index, QString::number(e.clipIn, 'f', 6), QString::number(e.clipOut, 'f', 6),
        qRound(e.timelineStart * 1000.0), QString::number(e.volume, 'f', 6),
        AudioChannelMode::Stereo, false, e.speed,
        e.leadInType, e.leadInDuration, e.trailOutType, e.trailOutDuration));
    return o;
}

struct Dump {
    bool ok = true;
    bool solidMatches = false;
    int frameCount = 0;
    QByteArray frames;
    QByteArray audio;
    QStringList files;
};

Dump generate(const QDir &out, const QString &media)
{
    Dump result;
    const auto saveImage = [&](const QString &name, const QImage &image) {
        const bool ok = !image.isNull() && image.save(out.filePath(name));
        if (!ok) std::fprintf(stderr, "fixture image write failed: %s\n", qPrintable(name));
        result.ok &= ok;
        result.files.append(name);
    };
    QImage first(kSize, QImage::Format_RGBA8888);
    first.fill(QColor(192, 32, 64));
    QImage second(kSize, QImage::Format_RGBA8888);
    second.fill(kSecondColor);
    saveImage(QStringLiteral("source_0.png"), first);
    saveImage(QStringLiteral("source_1.png"), second);
    // Absolute image paths are consumed only by the renderer, never serialized.
    const QString aPath = out.absoluteFilePath(QStringLiteral("source_0.png"));
    const QString bPath = out.absoluteFilePath(QStringLiteral("source_1.png"));
    const auto render = [&](Timeline &timeline, const QString &name,
                            std::initializer_list<qint64> times) {
        timeline.refreshPlaybackSequence();
        int index = 0;
        for (qint64 us : times) {
            const QImage frame = tlrender::renderFrameAt(&timeline, us, kSize);
            result.ok &= !frame.isNull() && frame.size() == kSize;
            const QByteArray bytes = rgbaBytes(frame);
            if (name == QStringLiteral("a") && index == 1)
                result.solidMatches = !frame.isNull() && bytes == rgbaBytes(second);
            result.frames.append(bytes);
            saveImage(QStringLiteral("frame_%1_%2.png").arg(name).arg(index++), frame);
            ++result.frameCount;
        }
    };
    {
        Timeline timeline;
        ClipInfo a = clip(aPath, 0.0, 1.5);
        ClipInfo b = clip(bPath, 0.0, 2.5);
        // PNG has only PTS 0. Use the existing freeze-frame mapping so the
        // legacy decoder does not seek past that single frame (no transition).
        a.timeRemapCurve.addKey(0.0, 0.0);
        b.timeRemapCurve.addKey(0.0, 0.0);
        timeline.videoTracks().first()->setClips({a, b});
        render(timeline, QStringLiteral("a"), {300000, 1700000, 3500000});
    }
    {
        Timeline timeline;
        // Cut at 1.5s, with source handles for a centered 1s dissolve.
        ClipInfo a = clip(aPath, 1.0, 2.5);
        ClipInfo b = clip(bPath, 1.0, 3.5);
        a.trailOut.type = b.leadIn.type = TransitionType::CrossDissolve;
        a.trailOut.duration = b.leadIn.duration = 1.0;
        a.trailOut.alignment = b.leadIn.alignment = TransitionAlignment::Center;
        timeline.videoTracks().first()->setClips({a, b});
        render(timeline, QStringLiteral("b"), {1500000, 200000});
    }
    {
        Timeline timeline;
        ClipInfo fast = clip(media, 0.0, 2.0);
        fast.speed = 2.0;
        ClipInfo reverse = clip(media, 0.0, 2.0);
        reverse.reversed = true;
        TimelineSequence child;
        child.id = child.name = QStringLiteral("fixture-child");
        child.videoTracks = {{clip(media, 0.0, 1.0), clip(media, 1.0, 2.0)}};
        ClipInfo nested = clip(timeline_nesting::sequenceClipFilePath(child.id), 0.0, 2.0);
        nested.sequenceRefId = child.id;
        TimelineSequence main;
        main.id = main.name = QStringLiteral("fixture-main");
        main.videoTracks = {{fast, reverse, nested}};
        timeline.setSequences({main, child}, main.id);
        render(timeline, QStringLiteral("c"), {500000, 2000000, 4000000});
    }
    {
        Timeline timeline;
        ClipInfo c = clip(media, 0.0, 2.0);
        c.colorCorrection.liftR = 0.05;
        c.colorCorrection.gammaG = 0.04;
        c.colorCorrection.gainB = 0.08;
        c.colorCorrection.logShadowR = 0.1;
        c.hslSecondary.enabled = true;
        c.hslSecondary.hueRange = 180.0;
        c.hslSecondary.satMin = c.hslSecondary.lumaMin = 0.0;
        c.hslSecondary.satMax = c.hslSecondary.lumaMax = 1.0;
        c.hslSecondary.liftG = 0.03;
        EnhancedTextOverlay text;
        text.text = QStringLiteral("Render fixture 200");
        text.font = QFont(QStringLiteral("Arial"), 24, QFont::Bold);
        text.startTime = 0.0;
        text.endTime = 2.0;
        c.textManager.addOverlay(text);
        timeline.videoTracks().first()->setClips({c});
        AdjustmentLayer adjustment;
        adjustment.id = 1;
        adjustment.name = QStringLiteral("fixture-grade");
        adjustment.timelineEndUs = 2000000;
        adjustment.gradingEnabled = true;
        adjustment.lift[0] = 5.0;
        adjustment.wbTempSlider = 8.0;
        timeline.setAdjustmentLayers({adjustment});
        render(timeline, QStringLiteral("d"), {1000000});
    }
    {
        Timeline timeline;
        ClipInfo c = clip(media, 0.0, 2.0);
        c.is3DLayer = true;
        c.layer3D.rotationX = 20.0;
        c.layer3D.positionZ = 40.0;
        timeline.videoTracks().first()->setClips({c});
        render(timeline, QStringLiteral("e"), {1000000});
    }
    {
        Timeline timeline;
        // Keep the path relative and identical across baseline/HEAD worktrees.
        const QString hum = QStringLiteral("test_assets/e2e_hum.wav");
        ClipInfo a = clip(hum, 0.0, 2.0);
        ClipInfo b = clip(hum, 0.5, 2.5);
        a.trailOut.type = TransitionType::FadeOut;
        a.trailOut.duration = 0.3;
        timeline.audioTracks().first()->setClips({a, b});
        const auto entries = timeline.computeAudioPlaybackSequence();
        result.ok &= entries.size() == 2;
        QJsonArray audio;
        for (int i = 0; i < entries.size(); ++i) audio.append(audioEntry(entries[i], i));
        result.audio = QJsonDocument(audio).toJson(QJsonDocument::Indented);
    }
    result.ok &= result.frameCount == 10;
    result.ok &= writeBytes(out.filePath(QStringLiteral("frames.bin")), result.frames);
    result.ok &= writeBytes(out.filePath(QStringLiteral("audio.json")), result.audio);
    result.files.append(QStringLiteral("frames.bin"));
    result.files.append(QStringLiteral("audio.json"));
    return result;
}

bool readBytes(const QString &path, QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    bytes = file.readAll();
    return file.error() == QFileDevice::NoError;
}
} // namespace

int runRenderFixtureSelftest()
{
    int failed = 0;
    int passed = 0;
    const auto gate = [&](int n, bool ok, const char *reason) {
        std::fprintf(stderr, "%s G%d %s\n", ok ? "PASS" : "FAIL", n, reason);
        if (ok) ++passed; else ++failed;
    };
    const auto summary = [&]() {
        std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
        return failed;
    };
    const QString path = qEnvironmentVariable("VEDITOR_RENDER_FIXTURE_OUT");
    const QString media = qEnvironmentVariable("VEDITOR_RENDER_FIXTURE_MEDIA",
                                               QStringLiteral("test_assets/e2e_clip.mp4"));
    if (path.isEmpty()) {
        gate(1, false, "VEDITOR_RENDER_FIXTURE_OUT is required");
        return summary();
    }
    if (!qobject_cast<QApplication *>(QApplication::instance()) || !QDir().mkpath(path)
        || !QFile::exists(media) || !QFile::exists(QStringLiteral("test_assets/e2e_hum.wav"))) {
        gate(1, false, "requires QApplication, writable output directory, video and test_assets/e2e_hum.wav; run from repository root");
        return summary();
    }
    const QDir out(path);
    const Dump first = generate(out, media);
    QByteArray frames, audio;
    bool firstOk = first.ok;
    for (const QString &name : first.files) {
        QByteArray bytes;
        const bool ok = readBytes(out.filePath(name), bytes);
        firstOk &= ok && !bytes.isEmpty();
        if (ok) std::fprintf(stderr, "HASH %s %s\n", qPrintable(name),
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().constData());
    }
    firstOk &= readBytes(out.filePath(QStringLiteral("frames.bin")), frames);
    firstOk &= readBytes(out.filePath(QStringLiteral("audio.json")), audio);
    gate(1, firstOk, "all fixtures (a)-(f), ten non-null frames and all files generated");
    // Rebuild every Timeline and regenerate all files, without temporary/random
    // names leaking into the dump. Read back the actual files for G2.
    const Dump second = generate(out, media);
    QByteArray framesAgain, audioAgain;
    const bool framesRead = readBytes(out.filePath(QStringLiteral("frames.bin")), framesAgain);
    const bool audioRead = readBytes(out.filePath(QStringLiteral("audio.json")), audioAgain);
    gate(2, firstOk && second.ok && framesRead && audioRead
        && frames == framesAgain && audio == audioAgain, "second full generation is bit-identical");
    gate(3, first.solidMatches && second.solidMatches, "fixture (a) at 1.7s equals every RGBA pixel of source_1.png");
    return summary();
}
