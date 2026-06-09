// src/selftests/clip_parent_parity_selftest.cpp
// --selftest=clip-parent-parity

#include <QDebug>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>

#include <cmath>

#include "../ClipGeometry.h"
#include "../FrameDiff.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TrackMatteKey.h"

namespace {

constexpr int kGateCount = 5;

bool near(double a, double b, double eps = 1e-10)
{
    return std::fabs(a - b) <= eps;
}

bool sameTransform(const clipgeom::ClipTransform &a,
                   const clipgeom::ClipTransform &b,
                   double eps = 1e-10)
{
    return near(a.videoScale, b.videoScale, eps)
        && near(a.videoDx, b.videoDx, eps)
        && near(a.videoDy, b.videoDy, eps)
        && near(a.rotationDeg, b.rotationDeg, eps);
}

bool makeSolidClip(const QString &outPath, const QString &hexColor)
{
    QProcess ff;
    ff.start(QStringLiteral("ffmpeg"),
             { QStringLiteral("-hide_banner"),
               QStringLiteral("-loglevel"), QStringLiteral("error"),
               QStringLiteral("-y"),
               QStringLiteral("-f"), QStringLiteral("lavfi"),
               QStringLiteral("-i"),
               QStringLiteral("color=c=%1:s=64x64:r=1:d=1").arg(hexColor),
               QStringLiteral("-frames:v"), QStringLiteral("1"),
               QStringLiteral("-c:v"), QStringLiteral("libx264rgb"),
               QStringLiteral("-qp"), QStringLiteral("0"),
               QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"),
               outPath });
    if (!ff.waitForStarted(15000))
        return false;
    ff.waitForFinished(60000);
    return ff.exitStatus() == QProcess::NormalExit
        && ff.exitCode() == 0
        && QFile::exists(outPath);
}

ClipInfo makeClip(const QString &path, const QString &name,
                  const clipgeom::ClipTransform &t = {})
{
    ClipInfo clip;
    clip.filePath = path;
    clip.displayName = name;
    clip.duration = 1.0;
    clip.inPoint = 0.0;
    clip.outPoint = 1.0;
    clip.opacity = 1.0;
    clip.visible = true;
    clip.videoScale = t.videoScale;
    clip.videoDx = t.videoDx;
    clip.videoDy = t.videoDy;
    clip.rotation2DDegrees = t.rotationDeg;
    return clip;
}

ClipInfo makeNullClip(const QString &name, const clipgeom::ClipTransform &t = {})
{
    ClipInfo clip = makeClip(clipgeom::nullObjectFilePath(), name, t);
    clip.opacity = 0.0;
    clip.visible = false;
    return clip;
}

void ensureVideoTrack(Timeline &tl, int trackIdx)
{
    while (tl.videoTracks().size() <= trackIdx)
        tl.addVideoTrack();
}

void addClip(Timeline &tl, int trackIdx, const ClipInfo &clip)
{
    ensureVideoTrack(tl, trackIdx);
    TimelineTrack *track = tl.videoTracks().value(trackIdx, nullptr);
    if (track)
        track->addClip(clip);
}

double frameMse(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull())
        return -1.0;
    return framediff::mse(a, b);
}

quint64 alphaSum(const QImage &img)
{
    if (img.isNull())
        return 1;
    const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    quint64 sum = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar *row = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x)
            sum += row[x * 4 + 3];
    }
    return sum;
}

void check(int &passed, int &failed, const char *name, bool ok,
           const QString &detail = QString())
{
    if (ok) {
        ++passed;
        qInfo().noquote() << "[clip-parent-parity] PASS" << name;
    } else {
        ++failed;
        qWarning().noquote() << "[clip-parent-parity] FAIL" << name << detail;
    }
}

} // namespace

int runClipParentParitySelftest()
{
    qInfo().noquote() << "[clip-parent-parity] selftest start";

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        qCritical() << "[clip-parent-parity] could not create temp dir";
        return 1;
    }
    const QString redPath = tmpDir.filePath(QStringLiteral("red.mp4"));
    const QString bluePath = tmpDir.filePath(QStringLiteral("blue.mp4"));
    if (!makeSolidClip(redPath, QStringLiteral("red"))
        || !makeSolidClip(bluePath, QStringLiteral("blue"))) {
        qCritical() << "[clip-parent-parity] ffmpeg clip generation failed";
        return 1;
    }

    const QSize canvas(64, 64);
    int passed = 0;
    int failed = 0;

    {
        Timeline baseline;
        addClip(baseline, 0, makeClip(redPath, QStringLiteral("red")));
        Timeline candidate;
        addClip(candidate, 0, makeClip(redPath, QStringLiteral("red")));
        candidate.setClipParentEntries({});

        const QImage a = tlrender::renderFrameAt(&baseline, 0, canvas);
        const QImage b = tlrender::renderFrameAt(&candidate, 0, canvas);
        const double mse = frameMse(a, b);
        check(passed, failed, "G1 no parent link renderFrameAt MSE=0",
              mse == 0.0, QStringLiteral("mse=%1").arg(mse, 0, 'g', 17));
    }

    {
        const QSize handCanvas(200, 100);
        const clipgeom::ClipTransform parent{1.5, 0.1, 0.2, 90.0};
        const clipgeom::ClipTransform child{2.0, 0.2, -0.1, 30.0};
        const clipgeom::ClipTransform expected{3.0, 0.175, 0.8, 120.0};
        const clipgeom::ClipTransform actual =
            clipgeom::composeParented(child, parent, handCanvas);
        check(passed, failed, "G2 composeParented hand reference",
              sameTransform(actual, expected),
              QStringLiteral("scale=%1 dx=%2 dy=%3 rot=%4")
                  .arg(actual.videoScale, 0, 'g', 17)
                  .arg(actual.videoDx, 0, 'g', 17)
                  .arg(actual.videoDy, 0, 'g', 17)
                  .arg(actual.rotationDeg, 0, 'g', 17));
    }

    {
        const clipgeom::ClipTransform parentT{1.0, 0.2, -0.1, 0.0};
        const clipgeom::ClipTransform childT{0.75, 0.0, 0.0, 0.0};
        const clipgeom::ClipTransform effective =
            clipgeom::composeParented(childT, parentT, canvas);

        Timeline parented;
        addClip(parented, 0, makeNullClip(QStringLiteral("base")));
        addClip(parented, 1, makeNullClip(QStringLiteral("parent"), parentT));
        addClip(parented, 2, makeClip(redPath, QStringLiteral("child"), childT));
        parented.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));

        Timeline direct;
        addClip(direct, 0, makeNullClip(QStringLiteral("base")));
        addClip(direct, 1, makeNullClip(QStringLiteral("parent"), parentT));
        addClip(direct, 2, makeClip(redPath, QStringLiteral("child"), effective));

        const double mse = frameMse(tlrender::renderFrameAt(&parented, 0, canvas),
                                    tlrender::renderFrameAt(&direct, 0, canvas));
        check(passed, failed,
              "G3 parented render equals direct pre-composed transform",
              mse == 0.0, QStringLiteral("mse=%1").arg(mse, 0, 'g', 17));
    }

    {
        const clipgeom::ClipTransform aT{0.8, 0.15, 0.0, 0.0};
        const clipgeom::ClipTransform bT{0.8, -0.15, 0.0, 0.0};
        Timeline raw;
        addClip(raw, 0, makeNullClip(QStringLiteral("base")));
        addClip(raw, 1, makeClip(redPath, QStringLiteral("A"), aT));
        addClip(raw, 2, makeClip(bluePath, QStringLiteral("B"), bT));

        Timeline cyclic;
        addClip(cyclic, 0, makeNullClip(QStringLiteral("base")));
        addClip(cyclic, 1, makeClip(redPath, QStringLiteral("A"), aT));
        addClip(cyclic, 2, makeClip(bluePath, QStringLiteral("B"), bT));
        QHash<QString, QString> entries;
        entries.insert(trackMatteClipKey(1, 0), trackMatteClipKey(2, 0));
        entries.insert(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
        cyclic.setClipParentEntries(entries);

        Timeline selfParent;
        addClip(selfParent, 0, makeNullClip(QStringLiteral("base")));
        addClip(selfParent, 1, makeClip(redPath, QStringLiteral("A"), aT));
        selfParent.setClipParent(trackMatteClipKey(1, 0), trackMatteClipKey(1, 0));

        const double cycleMse = frameMse(tlrender::renderFrameAt(&cyclic, 0, canvas),
                                         tlrender::renderFrameAt(&raw, 0, canvas));
        const QImage selfFrame = tlrender::renderFrameAt(&selfParent, 0, canvas);
        check(passed, failed,
              "G4 cycle/self-parent safe fallback",
              cycleMse == 0.0 && !selfFrame.isNull(),
              QStringLiteral("cycleMse=%1 selfNull=%2")
                  .arg(cycleMse, 0, 'g', 17)
                  .arg(selfFrame.isNull()));
    }

    {
        const clipgeom::ClipTransform parentT{1.0, -0.2, 0.15, 0.0};
        const clipgeom::ClipTransform childT{0.5, 0.1, 0.0, 0.0};
        const clipgeom::ClipTransform effective =
            clipgeom::composeParented(childT, parentT, canvas);

        Timeline nullOnly;
        addClip(nullOnly, 0, makeNullClip(QStringLiteral("null-only"), parentT));
        const QImage nullFrame = tlrender::renderFrameAt(&nullOnly, 0, canvas);

        Timeline parented;
        addClip(parented, 0, makeNullClip(QStringLiteral("base")));
        addClip(parented, 1, makeNullClip(QStringLiteral("null-parent"), parentT));
        addClip(parented, 2, makeClip(redPath, QStringLiteral("child"), childT));
        parented.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));

        Timeline direct;
        addClip(direct, 0, makeNullClip(QStringLiteral("base")));
        addClip(direct, 1, makeNullClip(QStringLiteral("null-parent"), parentT));
        addClip(direct, 2, makeClip(redPath, QStringLiteral("child"), effective));

        const double mse = frameMse(tlrender::renderFrameAt(&parented, 0, canvas),
                                    tlrender::renderFrameAt(&direct, 0, canvas));
        check(passed, failed,
              "G5 null object renders zero pixels and propagates transform",
              alphaSum(nullFrame) == 0 && mse == 0.0,
              QStringLiteral("alphaSum=%1 mse=%2")
                  .arg(alphaSum(nullFrame))
                  .arg(mse, 0, 'g', 17));
    }

    qInfo().noquote().nospace()
        << "[clip-parent-parity] selftest end, passed=" << passed
        << " failed=" << failed;
    return (passed == kGateCount && failed == 0) ? 0 : 1;
}
