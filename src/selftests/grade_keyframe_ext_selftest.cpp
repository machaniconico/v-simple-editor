#include "../clipanim/ClipAnim.h"
#include "../Timeline.h"
#include "../ProjectFile.h"
#include "../TimelineFrameRenderer.h"
#include "../libavcore/Encode.h"
#include "SelftestGate.h"
#include <QColor>
#include <QImage>
#include <QSet>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

namespace {
QString projectJson(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = {{clip}};
    return ProjectFile::toJsonString(data);
}
void animate(ClipInfo &clip, const QString &name, double from, double to)
{
    KeyframeTrack track(name, from);
    track.addKeyframe(0.0, from);
    track.addKeyframe(2.0, to);
    clip.keyframes.addTrack(track);
}
bool near(double a, double b) { return std::abs(a - b) < 0.01; }
bool writeFrameClip(const QString &path)
{
    libavcore::EncodeRequest request;
    request.outputPath = path.toStdString();
    request.width = request.height = 64;
    request.fps = request.fpsNum = 10;
    request.fpsDen = 1;
    request.videoCodecName = "mpeg4";
    request.videoBitrateBits = 300000;
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;
    libavcore::FrameEncoder encoder;
    if (encoder.open(request)) return false;
    QImage frame(64, 64, QImage::Format_RGB888);
    // The decoded hue must fall inside the 60-degree qualifier at t=1s.
    frame.fill(QColor::fromHsv(60, 190, 210));
    for (int i = 0; i < 20; ++i)
        if (!encoder.pushFrame(frame, i)) return false;
    return !encoder.finalize().has_value();
}
}

int runGradeKeyframeExtSelftest()
{
    selftest::GateTally tally;
    const auto check = [&](int gate, bool ok) { tally.gate(gate, ok); };
    ClipInfo clip;
    clip.duration = clip.outPoint = 2.0;
    clip.colorCorrection.brightness = 7.0;
    clip.colorCorrection.logMidG = 0.2;
    clip.colorCorrection.hueSatWarp.hueShiftDeg[2][11] = 11.0f;
    clip.hslSecondary.enabled = true;
    clip.hslSecondary.liftR = 0.1;
    const QString before = projectJson(clip);
    clipanim::resetExtendedGradeCallCountForTest();
    clipanim::setExtendedGradeDisabledForTest(true);
    ClipInfo bypass = clip;
    bypass.hslSecondary = clipanim::effectiveHslSecondaryAt(clip, 1.0);
    bypass.colorCorrection = clipanim::effectiveColorCorrectionAt(clip, 1.0);
    clipanim::setExtendedGradeDisabledForTest(false);
    ClipInfo evaluated = clip;
    evaluated.hslSecondary = clipanim::effectiveHslSecondaryAt(clip, 1.0);
    evaluated.colorCorrection = clipanim::effectiveColorCorrectionAt(clip, 1.0);
    check(1, projectJson(evaluated) == before && projectJson(bypass) == before
        && projectJson(clip) == before && clip.keyframes.tracks().isEmpty()
        && clipanim::extendedGradeCallCountForTest() == 0);

    animate(clip, QStringLiteral("grade.hsl.hueCenter"), 0.0, 120.0);
    // Compare the actual timeline renderer at the same timestamp with the
    // animated value and static references. The former source-text check did
    // not verify that the evaluated grade reached rendered pixels.
    QTemporaryDir renderTemp;
    const QString source = renderTemp.filePath("grade-source.mp4");
    const bool sourceReady = renderTemp.isValid() && writeFrameClip(source);
    ClipInfo renderClip;
    renderClip.filePath = source;
    renderClip.duration = renderClip.outPoint = 2.0;
    renderClip.hslSecondary.enabled = true;
    renderClip.hslSecondary.hueCenter = 0.0;
    renderClip.hslSecondary.hueRange = 20.0;
    renderClip.hslSecondary.softness = 0.0;
    renderClip.hslSecondary.satMin = 0.0;
    renderClip.hslSecondary.satMax = 1.0;
    renderClip.hslSecondary.lumaMin = 0.0;
    renderClip.hslSecondary.lumaMax = 1.0;
    renderClip.hslSecondary.liftG = 0.3;
    animate(renderClip, QStringLiteral("grade.hsl.hueCenter"), 0.0, 120.0);
    Timeline renderTimeline;
    renderTimeline.setProjectOutputConfig(64, 64, true);
    const auto renderAtOneSecond = [&](const ClipInfo &candidate) {
        renderTimeline.restoreFromProject(QVector<QVector<ClipInfo>>{{candidate}},
            QVector<QVector<ClipInfo>>{}, 0.0, -1.0, -1.0, 100);
        renderTimeline.refreshPlaybackSequence();
        return tlrender::renderFrameAt(&renderTimeline, 1000000, QSize(64, 64), 100000.0);
    };
    const QImage animatedFrame = sourceReady ? renderAtOneSecond(renderClip) : QImage();
    renderClip.keyframes = KeyframeManager{};
    renderClip.hslSecondary.hueCenter = 60.0;
    const QImage static60 = sourceReady ? renderAtOneSecond(renderClip) : QImage();
    renderClip.hslSecondary.hueCenter = 0.0;
    const QImage static0 = sourceReady ? renderAtOneSecond(renderClip) : QImage();
    renderClip.hslSecondary.enabled = false;
    const QImage unaffected = sourceReady ? renderAtOneSecond(renderClip) : QImage();
    const bool renderMatch = !animatedFrame.isNull() && animatedFrame == static60
        && static60 != unaffected && static0 == unaffected;
    const double h0 = clipanim::effectiveHslSecondaryAt(clip, 0.0).hueCenter;
    const double h1 = clipanim::effectiveHslSecondaryAt(clip, 1.0).hueCenter;
    const double h2 = clipanim::effectiveHslSecondaryAt(clip, 2.0).hueCenter;
    std::fprintf(stderr, "grade-keyframe-ext G2 sourceReady=%d framePresent=%d "
        "matchStatic60=%d static60DiffersUnaffected=%d static0EqualsUnaffected=%d "
        "h0=%g h1=%g h2=%g\n",
        sourceReady, !animatedFrame.isNull(), animatedFrame == static60,
        static60 != unaffected, static0 == unaffected, h0, h1, h2);
    check(2, near(h0, 0.0) && near(h1, 60.0) && near(h2, 120.0) && renderMatch);

    clip.colorCorrection.hueSatWarp = HueSatWarp{};
    animate(clip, QStringLiteral("grade.hueSatWarp.shift.0.0"), 0.0, 30.0);
    const auto warp = clipanim::effectiveColorCorrectionAt(clip, 1.0).hueSatWarp;
    bool otherDefault = true;
    for (int r = 0; r < HueSatWarp::kSatRings; ++r)
        for (int h = 0; h < HueSatWarp::kHueNodes; ++h) {
            if (r != 0 || h != 0) otherDefault &= warp.hueShiftDeg[r][h] == 0.0f;
            otherDefault &= warp.satScale[r][h] == 1.0f;
        }
    check(3, near(warp.hueShiftDeg[0][0], 15.0) && otherDefault);

    QSet<QString> names;
    bool valid = clipanim::warpGradeTracks().size() == 72
        && clipanim::hslGradeTracks().size() == 16;
    for (const auto &field : clipanim::hslGradeTracks()) {
        valid &= !names.contains(field.name);
        names.insert(field.name);
        animate(clip, field.name, 0.0, 2.0);
    }
    for (const auto &field : clipanim::warpGradeTracks()) {
        valid &= !names.contains(field.name) && field.ring >= 0
            && field.ring < HueSatWarp::kSatRings && field.hue >= 0
            && field.hue < HueSatWarp::kHueNodes;
        valid &= field.name == QStringLiteral("grade.hueSatWarp.%1.%2.%3")
            .arg(field.shift ? QStringLiteral("shift") : QStringLiteral("scale"))
            .arg(field.ring).arg(field.hue);
        names.insert(field.name);
        animate(clip, field.name, 0.0, 2.0);
    }
    for (bool log : {false, true})
        for (const auto &field : clipanim::sectionGradeTracks(log)) {
            valid &= !names.contains(field.name);
            names.insert(field.name);
            animate(clip, field.name, 0.0, 2.0);
        }
    const auto hsl = clipanim::effectiveHslSecondaryAt(clip, 1.0);
    const auto cc = clipanim::effectiveColorCorrectionAt(clip, 1.0);
    for (const auto &field : clipanim::hslGradeTracks())
        valid &= near(hsl.*(field.member), 1.0);
    for (const auto &field : clipanim::warpGradeTracks())
        valid &= near(field.shift ? cc.hueSatWarp.hueShiftDeg[field.ring][field.hue]
                                  : cc.hueSatWarp.satScale[field.ring][field.hue], 1.0);
    for (bool log : {false, true})
        for (const auto &field : clipanim::sectionGradeTracks(log))
            valid &= near(cc.*(field.member), 1.0);
    check(4, valid && names.size() == 72 + 16 + 18);

    ProjectData loaded;
    const bool restored = ProjectFile::fromJsonString(projectJson(clip), loaded);
    bool roundtrip = restored && !loaded.videoTracks.isEmpty()
        && !loaded.videoTracks.first().isEmpty();
    if (roundtrip) {
        const auto &copy = loaded.videoTracks.first().first();
        roundtrip &= projectJson(copy) == projectJson(clip);
        for (const auto &name : names) {
            const auto *track = copy.keyframes.track(name);
            roundtrip &= track && track->count() == 2
                && near(copy.keyframes.valueAt(name, 1.0, -1.0), 1.0);
        }
        roundtrip &= near(clipanim::effectiveHslSecondaryAt(copy, 1.0).hueCenter, 1.0)
            && near(clipanim::effectiveColorCorrectionAt(copy, 1.0).hueSatWarp.hueShiftDeg[0][0], 1.0);
    }
    check(5, roundtrip);
    return tally.finish("grade-keyframe-ext");
}
