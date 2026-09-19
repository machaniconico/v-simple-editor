#include "../Timeline.h"
#include "../UndoManager.h"
#include "../ProjectFile.h"
#include "../TimelineFrameRenderer.h"
#include "../ClipGeometry.h"
#include "../TrackMatteKey.h"
#include "../RenderInPlace.h"
#include "../libavcore/Encode.h"
#include "../libavcore/AudioExtract.h"

#include <QColor>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <vector>

namespace clipenabletest {
void setBypass(bool value);
quint64 skipCount();
}

namespace {
bool writeVideo(const QString &path)
{
    libavcore::EncodeRequest request;
    request.width = request.height = 64;
    request.fps = request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoBitrateBits = 600000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(64, 64, QImage::Format_RGB888);
    frame.fill(QColor(230, 30, 20));
    for (int i = 0; i < 10; ++i)
        if (!encoder.pushFrame(frame, i)) return false;
    return !encoder.finalize().has_value();
}

void baseline(Timeline &timeline)
{
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("基準"));
}

double peak(const std::vector<double> &samples)
{
    double result = 0.0;
    for (double sample : samples) result = qMax(result, std::fabs(double(sample)));
    return result;
}
}

int runClipEnableSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    QTemporaryDir temporary;
    const QString video = temporary.filePath(QStringLiteral("solid.mp4"));
    const bool fixture = temporary.isValid() && writeVideo(video);
    ClipInfo clip;
    clip.filePath = video;
    clip.duration = clip.outPoint = 1.0;
    Timeline timeline;
    timeline.videoTracks().first()->setClips({clip});
    baseline(timeline);
    const auto render = [&]() {
        return tlrender::renderFrameAt(&timeline, 400000, QSize(64, 64));
    };
    const QImage enabled = render();
    const auto serial = timeline.undoManager()->saveSerial();
    const bool changed = timeline.setClipEnabled(TrackKind::Video, 0, 0, false);
    const QImage disabled = render();
    const bool skipped = clipenabletest::skipCount() > 0;
    clipenabletest::setBypass(true);
    const QImage bypassDisabled = render();
    clipenabletest::setBypass(false);
    gate(1, fixture && changed && skipped && bypassDisabled == enabled && !enabled.isNull() && !disabled.isNull()
        && enabled != disabled && disabled.pixelColor(32, 32).alpha() == 0
        && timeline.computePlaybackSequence().size() == 1);

    ProjectData project;
    project.videoTracks = timeline.allVideoTracks();
    project.audioTracks = timeline.allAudioTracks();
    const QString projectPath = temporary.filePath(QStringLiteral("disabled.vedit"));
    ProjectData loaded;
    bool persisted = ProjectFile::save(projectPath, project) && ProjectFile::load(projectPath, loaded);
    Timeline restored;
    if (persisted)
        restored.restoreFromProject(loaded.videoTracks, loaded.audioTracks, 0.0, -1.0, -1.0, 10);
    persisted = persisted && !restored.videoClips().isEmpty() && !restored.videoClips().first().visible;
    const bool oneSave = timeline.setClipEnabled(TrackKind::Video, 0, 0, false)
        && !timeline.setClipEnabled(TrackKind::Video, 0, 99, false)
        && timeline.undoManager()->saveSerial() == serial + 1;
    timeline.undo();
    gate(2, persisted && oneSave && timeline.videoClips().first().visible
        && timeline.undoManager()->saveSerial() == serial + 1 && render() == enabled);

    const QString wav = temporary.filePath(QStringLiteral("tone.wav"));
    QString error;
    std::vector<float> tone(48000 * 2);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = float(0.25 * std::sin(2.0 * 3.141592653589793 * 440.0 * (i / 2) / 48000.0));
    bool audioOk = libavcore::writeWavFloatInterleaved(wav, tone, 48000, 2, &error);
    ClipInfo audioClip = clip;
    audioClip.filePath = wav;
    timeline.audioTracks().first()->setClips({audioClip});
    baseline(timeline);
    const auto mix = [&](const QString &name, std::vector<double> &samples) {
        const QString output = temporary.filePath(name);
        int rate = 0;
        return !renderinplace::prepareAudioMix(&timeline, output, 1.0, &error).isEmpty()
            && libavcore::readPcm16WavToMono(output, samples, rate, &error)
            && rate == 48000 && !samples.empty();
    };
    std::vector<double> audible, silent;
    audioOk = audioOk && mix(QStringLiteral("enabled.wav"), audible);
    const auto audioSerial = timeline.undoManager()->saveSerial();
    audioOk = timeline.setClipEnabled(TrackKind::Audio, 0, 0, false) && audioOk;
    audioOk = audioOk && timeline.computeAudioPlaybackSequence().isEmpty()
        && mix(QStringLiteral("disabled.wav"), silent)
        && peak(audible) > 0.1 && peak(silent) < 0.0001
        && timeline.undoManager()->saveSerial() == audioSerial + 1;
    audioOk = audioOk && !ProjectFile::clipFromJson(ProjectFile::clipToJson(
        timeline.audioTracks().first()->clips().first())).visible;
    timeline.undo();
    gate(3, audioOk && timeline.audioTracks().first()->clips().first().visible
        && !timeline.computeAudioPlaybackSequence().isEmpty());

    ClipInfo nullClip = clip;
    nullClip.filePath = clipgeom::nullObjectFilePath();
    nullClip.visible = false;
    nullClip.opacity = 0.0;
    nullClip.videoDx = 0.25;
    timeline.addVideoTrack();
    timeline.videoTracks()[1]->setClips({nullClip});
    project.videoTracks = timeline.allVideoTracks();
    project.audioTracks = timeline.allAudioTracks();
    bool noVisible = true;
    for (const auto *track : timeline.videoTracks())
        for (const auto &item : track->clips())
            noVisible = noVisible && !QJsonDocument(ProjectFile::clipToJson(item)).toJson().contains("\"visible\"");
    const QString defaultPath = temporary.filePath(QStringLiteral("default.vedit"));
    noVisible = ProjectFile::save(defaultPath, project) && noVisible;
    QFile saved(defaultPath);
    noVisible = saved.open(QIODevice::ReadOnly) && noVisible;
    noVisible = noVisible && !saved.readAll().contains("\"visible\"");
    clipenabletest::setBypass(true);
    const QImage bypass = render();
    clipenabletest::setBypass(false);
    const QImage normal = render();
    gate(4, fixture && noVisible && !normal.isNull() && normal == bypass
        && clipenabletest::skipCount() == 0);

    timeline.setClipParent(trackMatteClipKey(0, 0), trackMatteClipKey(1, 0));
    const QImage parented = render();
    ClipInfo translated = clip;
    translated.videoDx = nullClip.videoDx;
    Timeline reference;
    reference.videoTracks().first()->setClips({translated});
    reference.addVideoTrack();
    reference.videoTracks()[1]->setClips({nullClip});
    const QImage expected = tlrender::renderFrameAt(&reference, 400000, QSize(64, 64));
    gate(5, fixture && !parented.isNull() && parented != normal && parented == expected
        && !timeline.setClipEnabled(TrackKind::Video, 1, 0, true));
    clipenabletest::setBypass(false);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
