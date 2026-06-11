// src/selftests/clip_parent_parity_selftest.cpp
// --selftest=clip-parent-parity

#include <QDebug>
#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QtGlobal>

#include <cmath>
#include <functional>

#include "../ClipGeometry.h"
#include "../FrameDiff.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TrackMatteKey.h"

namespace {

constexpr int kGateCount = 7;

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

QVector<clipgeom::ClipTransform> previewEffectiveTransforms(
    const QVector<clipgeom::ClipTransform> &raw,
    const QVector<int> &parentByIndex,
    QSize canvas)
{
    QVector<clipgeom::ClipTransform> effective = raw;
    QVector<int> state(raw.size(), 0);
    QVector<char> valid(raw.size(), 1);

    std::function<bool(int, int)> resolve = [&](int idx, int depth) -> bool {
        if (idx < 0 || idx >= raw.size())
            return false;
        if (state[idx] == 1) {
            valid[idx] = 0;
            effective[idx] = raw[idx];
            state[idx] = 2;
            return false;
        }
        if (state[idx] == 2)
            return valid[idx] != 0;
        if (depth > 8) {
            effective[idx] = raw[idx];
            return true;
        }

        state[idx] = 1;
        const int parentIdx = parentByIndex.value(idx, -1);
        if (parentIdx < 0 || parentIdx >= raw.size() || parentIdx == idx) {
            effective[idx] = raw[idx];
            valid[idx] = 1;
            state[idx] = 2;
            return true;
        }
        if (!resolve(parentIdx, depth + 1)) {
            effective[idx] = raw[idx];
            valid[idx] = 0;
            state[idx] = 2;
            return false;
        }

        effective[idx] = clipgeom::composeParented(raw[idx], effective[parentIdx], canvas);
        valid[idx] = 1;
        state[idx] = 2;
        return true;
    };

    for (int i = 0; i < raw.size(); ++i)
        resolve(i, 0);
    return effective;
}

clipgeom::ClipTransform previewLeafTransformForChain(
    const QVector<clipgeom::ClipTransform> &rootToLeaf,
    QSize canvas)
{
    QVector<clipgeom::ClipTransform> raw;
    QVector<int> parents;
    raw.reserve(rootToLeaf.size());
    parents.reserve(rootToLeaf.size());
    for (int i = rootToLeaf.size() - 1; i >= 0; --i) {
        raw.append(rootToLeaf[i]);
        parents.append(raw.size());
    }
    if (!parents.isEmpty())
        parents[parents.size() - 1] = -1;

    const QVector<clipgeom::ClipTransform> effective =
        previewEffectiveTransforms(raw, parents, canvas);
    return effective.value(0);
}

void populateDirectChainTimeline(Timeline &direct,
                                 const QString &redPath,
                                 const QVector<clipgeom::ClipTransform> &parentTransforms,
                                 const clipgeom::ClipTransform &leafTransform)
{
    addClip(direct, 0, makeNullClip(QStringLiteral("base")));
    for (int i = 0; i < parentTransforms.size(); ++i) {
        addClip(direct, i + 1,
                makeNullClip(QStringLiteral("parent-%1").arg(i), parentTransforms[i]));
    }
    addClip(direct, parentTransforms.size() + 1,
            makeClip(redPath, QStringLiteral("child"), leafTransform));
}

void populateParentedChainTimeline(Timeline &parented,
                                   const QString &redPath,
                                   const QVector<clipgeom::ClipTransform> &parentTransforms,
                                   const clipgeom::ClipTransform &leafTransform)
{
    populateDirectChainTimeline(parented, redPath, parentTransforms, leafTransform);
    const int childTrack = parentTransforms.size() + 1;
    parented.setClipParent(trackMatteClipKey(childTrack, 0),
                           trackMatteClipKey(childTrack - 1, 0));
    for (int track = childTrack - 1; track > 1; --track)
        parented.setClipParent(trackMatteClipKey(track, 0),
                               trackMatteClipKey(track - 1, 0));
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

    {
        const bool hadParentingEnv = qEnvironmentVariableIsSet("VEDITOR_PARENTING");
        const QByteArray oldParentingEnv = qgetenv("VEDITOR_PARENTING");
        qputenv("VEDITOR_PARENTING", QByteArray("0"));

        const clipgeom::ClipTransform parentT{1.0, 0.25, 0.0, 0.0};
        const clipgeom::ClipTransform childT{0.75, -0.1, 0.0, 0.0};
        Timeline parented;
        addClip(parented, 0, makeNullClip(QStringLiteral("base")));
        addClip(parented, 1, makeNullClip(QStringLiteral("parent"), parentT));
        addClip(parented, 2, makeClip(redPath, QStringLiteral("child"), childT));
        parented.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));

        Timeline unparented;
        addClip(unparented, 0, makeNullClip(QStringLiteral("base")));
        addClip(unparented, 1, makeNullClip(QStringLiteral("parent"), parentT));
        addClip(unparented, 2, makeClip(redPath, QStringLiteral("child"), childT));

        const QImage parentedFrame = tlrender::renderFrameAt(&parented, 0, canvas);
        const QImage unparentedFrame = tlrender::renderFrameAt(&unparented, 0, canvas);
        const double mse = frameMse(parentedFrame, unparentedFrame);

        if (hadParentingEnv)
            qputenv("VEDITOR_PARENTING", oldParentingEnv);
        else
            qunsetenv("VEDITOR_PARENTING");

        check(passed, failed,
              "G6 VEDITOR_PARENTING=0 disables preview/export parenting",
              mse == 0.0, QStringLiteral("mse=%1").arg(mse, 0, 'g', 17));
    }

    {
        auto chainMse = [&](int parentCount) {
            QVector<clipgeom::ClipTransform> parents;
            parents.reserve(parentCount);
            for (int i = 0; i < parentCount; ++i)
                parents.append(clipgeom::ClipTransform{1.0, 0.01, 0.0, 0.0});

            const clipgeom::ClipTransform childT{0.7, -0.05, 0.0, 0.0};
            QVector<clipgeom::ClipTransform> rootToLeaf = parents;
            rootToLeaf.append(childT);
            const clipgeom::ClipTransform previewEffective =
                previewLeafTransformForChain(rootToLeaf, canvas);

            Timeline parented;
            populateParentedChainTimeline(parented, redPath, parents, childT);
            Timeline direct;
            populateDirectChainTimeline(direct, redPath, parents, previewEffective);
            return frameMse(tlrender::renderFrameAt(&parented, 0, canvas),
                            tlrender::renderFrameAt(&direct, 0, canvas));
        };

        auto sharedAncestorMse = [&]() {
            Timeline parented;
            Timeline direct;
            addClip(parented, 0, makeNullClip(QStringLiteral("base")));
            addClip(direct, 0, makeNullClip(QStringLiteral("base")));

            const clipgeom::ClipTransform rootT{1.0, 0.18, 0.0, 0.0};
            const clipgeom::ClipTransform sharedT{1.0, 0.04, 0.0, 0.0};
            const clipgeom::ClipTransform childT{0.7, -0.05, 0.0, 0.0};
            addClip(parented, 1, makeNullClip(QStringLiteral("root"), rootT));
            addClip(direct, 1, makeNullClip(QStringLiteral("root"), rootT));
            addClip(parented, 2, makeNullClip(QStringLiteral("shared"), sharedT));
            addClip(direct, 2, makeNullClip(QStringLiteral("shared"), sharedT));
            addClip(parented, 3, makeClip(redPath, QStringLiteral("child"), childT));

            QVector<clipgeom::ClipTransform> previewRaw;
            QVector<int> previewParents;
            for (int track = 11; track >= 4; --track) {
                const clipgeom::ClipTransform t{1.0, 0.005, 0.0, 0.0};
                addClip(parented, track,
                        makeNullClip(QStringLiteral("bridge-%1").arg(track), t));
                addClip(direct, track,
                        makeNullClip(QStringLiteral("bridge-%1").arg(track), t));
                previewRaw.append(t);
                previewParents.append(previewRaw.size());
            }
            const int lastBridgePreviewIdx = previewRaw.size() - 1;

            parented.setClipParent(trackMatteClipKey(2, 0), trackMatteClipKey(1, 0));
            parented.setClipParent(trackMatteClipKey(3, 0), trackMatteClipKey(11, 0));
            for (int track = 11; track > 4; --track)
                parented.setClipParent(trackMatteClipKey(track, 0),
                                       trackMatteClipKey(track - 1, 0));
            parented.setClipParent(trackMatteClipKey(4, 0), trackMatteClipKey(2, 0));

            previewRaw.append(childT);
            previewParents.append(0);
            previewRaw.append(sharedT);
            previewParents.append(previewRaw.size());
            previewRaw.append(rootT);
            previewParents.append(-1);
            const int childPreviewIdx = previewRaw.size() - 3;
            previewParents[childPreviewIdx] = 0;
            const int sharedPreviewIdx = previewRaw.size() - 2;
            previewParents[sharedPreviewIdx] = previewRaw.size() - 1;
            previewParents[lastBridgePreviewIdx] = sharedPreviewIdx;

            const QVector<clipgeom::ClipTransform> previewEffective =
                previewEffectiveTransforms(previewRaw, previewParents, canvas);
            addClip(direct, 3,
                    makeClip(redPath, QStringLiteral("child"),
                             previewEffective.value(childPreviewIdx)));

            return frameMse(tlrender::renderFrameAt(&parented, 0, canvas),
                            tlrender::renderFrameAt(&direct, 0, canvas));
        };

        const double depth8Mse = chainMse(8);
        const double depth9Mse = chainMse(9);
        const double sharedMse = sharedAncestorMse();
        check(passed, failed,
              "G7 depth-discipline preview/export parity",
              depth8Mse == 0.0 && depth9Mse == 0.0 && sharedMse == 0.0,
              QStringLiteral("depth8=%1 depth9=%2 shared=%3")
                  .arg(depth8Mse, 0, 'g', 17)
                  .arg(depth9Mse, 0, 'g', 17)
                  .arg(sharedMse, 0, 'g', 17));
    }

    qInfo().noquote().nospace()
        << "[clip-parent-parity] selftest end, passed=" << passed
        << " failed=" << failed;
    return (passed == kGateCount && failed == 0) ? 0 : 1;
}
