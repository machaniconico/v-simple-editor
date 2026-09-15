#include "../clipanim/ClipAnim.h"
#include "../Timeline.h"
#include "../ProjectFile.h"
#include <QFile>
#include <QSet>
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
bool sourceContains(const QString &path, const QByteArray &text)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.readAll().contains(text);
}
}

int runGradeKeyframeExtSelftest()
{
    int passed = 0, failed = 0;
    const auto check = [&](int gate, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", gate);
        ok ? ++passed : ++failed;
    };
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
    check(2, near(clipanim::effectiveHslSecondaryAt(clip, 1.0).hueCenter, 60.0)
        && near(clipanim::effectiveHslSecondaryAt(clip, 0.0).hueCenter, 0.0)
        && near(clipanim::effectiveHslSecondaryAt(clip, 2.0).hueCenter, 120.0)
        && sourceContains(QStringLiteral("src/MainWindow.cpp"),
                          "clipanim::effectiveHslSecondaryAt(*active, localSec)")
        && sourceContains(QStringLiteral("src/TimelineFrameRenderer.cpp"),
                          "clipanim::effectiveHslSecondaryAt(clip, clipLocalSeconds)"));

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
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
