// MotionBlurP2Selftest.cpp
// T5 MOTION BLUR P2: per-clip activation gate and true-only persistence.
// Run via: --selftest=motion-blur-p2

#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../playback/motionblur_flag.h"

#include <QDebug>
#include <QString>
#include <QVector>

namespace {

ClipInfo makeClip(bool motionBlurEnabled = false)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("motion_blur_p2_selftest.mp4");
    clip.displayName = QStringLiteral("motion_blur_p2_selftest");
    clip.duration = 1.0;
    clip.outPoint = 1.0;
    clip.motionBlurEnabled = motionBlurEnabled;
    return clip;
}

void setTimelineClip(Timeline &timeline, const ClipInfo &clip)
{
    timeline.videoTracks().first()->setClips(QVector<ClipInfo>{clip});
}

ProjectData projectWithClip(const ClipInfo &clip)
{
    ProjectData data;
    data.videoTracks = QVector<QVector<ClipInfo>>{QVector<ClipInfo>{clip}};
    return data;
}

} // namespace

int runMotionBlurP2Selftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *desc, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[motion-blur-p2] PASS G%1 %2")
                                     .arg(gate)
                                     .arg(QString::fromLatin1(desc));
        } else {
            ++failed;
            qWarning().noquote()
                << QStringLiteral("[motion-blur-p2] FAIL G%1 %2%3")
                       .arg(gate)
                       .arg(QString::fromLatin1(desc))
                       .arg(detail.isEmpty()
                                ? QString()
                                : QStringLiteral(" - %1").arg(detail));
        }
    };

    qInfo().noquote() << "[motion-blur-p2] selftest start";

    // G1: env off + no flagged clips keeps the single-sample/default path.
    {
        Timeline timeline;
        setTimelineClip(timeline, makeClip(false));
        check(1, "activeForTimeline false when env off and no clip flagged",
              !motionblur::activeForTimeline(&timeline, false));
    }

    // G2: env on remains a global force, preserving existing motion-blur mode.
    {
        Timeline timeline;
        setTimelineClip(timeline, makeClip(false));
        check(2, "activeForTimeline true when env on",
              motionblur::activeForTimeline(&timeline, true));
    }

    // G3: per-clip opt-in is a second activation trigger when env is off.
    {
        Timeline timeline;
        setTimelineClip(timeline, makeClip(true));
        check(3, "activeForTimeline true when any clip is flagged",
              motionblur::activeForTimeline(&timeline, false));
    }

    // G4: true round-trips; false/default is omitted from project JSON.
    {
        const QString defaultJson = ProjectFile::toJsonString(projectWithClip(makeClip(false)));
        const bool defaultOmitted =
            !defaultJson.contains(QStringLiteral("\"motionBlurEnabled\""));

        ClipInfo enabledClip = makeClip(true);
        const QString enabledJson = ProjectFile::toJsonString(projectWithClip(enabledClip));
        ProjectData loaded;
        const bool loadedOk = ProjectFile::fromJsonString(enabledJson, loaded);
        const bool hasLoadedClip = loadedOk
            && !loaded.videoTracks.isEmpty()
            && !loaded.videoTracks[0].isEmpty();
        const bool roundTrips =
            hasLoadedClip && loaded.videoTracks[0][0].motionBlurEnabled;

        check(4, "ProjectFile true round-trip and false omission",
              defaultOmitted
                  && enabledJson.contains(QStringLiteral("\"motionBlurEnabled\""))
                  && roundTrips,
              QStringLiteral("defaultOmitted=%1 containsEnabled=%2 loadedOk=%3")
                  .arg(defaultOmitted)
                  .arg(enabledJson.contains(QStringLiteral("\"motionBlurEnabled\"")))
                  .arg(loadedOk));
    }

    // G5: the ClipInfo default is strict OFF.
    {
        const ClipInfo clip{};
        check(5, "ClipInfo default motionBlurEnabled is false",
              !clip.motionBlurEnabled);
    }

    qInfo().noquote() << QStringLiteral("[motion-blur-p2] summary passed=%1 failed=%2")
                             .arg(passed)
                             .arg(failed);
    return failed == 0 ? 0 : 1;
}
