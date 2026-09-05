#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../VideoPlayer.h"
#include "../Overlay.h"

#include <QFileInfo>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
bool bitsEqual(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size() || a.format() != b.format())
        return false;
    const int bytes = (a.width() * a.depth() + 7) / 8;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y), bytes) != 0)
            return false;
    return true;
}
// Preview/export may store identical pixels in different QImage formats.
// Keep bitsEqual strict for G1's same-path byte identity check.
bool pixelsEqual(const QImage &a, const QImage &b)
{
    return bitsEqual(a.convertToFormat(QImage::Format_RGBA8888),
                     b.convertToFormat(QImage::Format_RGBA8888));
}
double mse(const QImage &left, const QImage &right)
{
    if (left.isNull() || right.isNull() || left.size() != right.size())
        return std::numeric_limits<double>::infinity();
    const QImage a = left.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = right.convertToFormat(QImage::Format_RGBA8888);
    double sum = 0.0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width() * 4; ++x) {
            const double d = int(a.constScanLine(y)[x]) - int(b.constScanLine(y)[x]);
            sum += d * d;
        }
    return sum / (a.width() * a.height() * 4.0);
}
QColor meanColor(const QImage &image, int left, int right)
{
    if (image.isNull()) return QColor();
    double r = 0, g = 0, b = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = left; x < right; ++x) {
            const QColor c = image.pixelColor(x, y);
            r += c.red(); g += c.green(); b += c.blue();
        }
    const double n = (right - left) * image.height();
    return QColor(qRound(r / n), qRound(g / n), qRound(b / n));
}
ClipInfo clip(const QString &path)
{
    ClipInfo c;
    c.filePath = path;
    c.duration = 4.0;
    c.inPoint = 1.0;
    c.outPoint = 3.0;
    return c;
}
void transitionPair(Timeline &timeline, ClipInfo a, ClipInfo b, TransitionType type,
                    TransitionAlignment alignment = TransitionAlignment::Center,
                    TransitionEasing easing = TransitionEasing::Linear)
{
    a.trailOut.type = b.leadIn.type = type;
    a.trailOut.duration = b.leadIn.duration = 1.0;
    a.trailOut.alignment = b.leadIn.alignment = alignment;
    a.trailOut.easing = b.leadIn.easing = easing;
    timeline.trackAt(false, 0)->setClips({a, b});
}
}

int runTransitionExportSelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int n, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", n);
        ok ? ++passed : ++failed;
    };
    QTemporaryDir temp;
    const QSize size(64, 48);
    QImage red(size, QImage::Format_RGBA8888), blue(size, QImage::Format_RGBA8888);
    red.fill(Qt::red); blue.fill(Qt::blue);
    const QString redPath = temp.filePath(QStringLiteral("red.png"));
    const QString bluePath = temp.filePath(QStringLiteral("blue.png"));
    const bool fixtures = temp.isValid() && red.save(redPath) && blue.save(bluePath)
        && QFileInfo::exists(QStringLiteral("test_assets/e2e_clip.mp4"));
    const ClipInfo a = clip(redPath), b = clip(bluePath);
    Timeline timeline;
    auto render = [&](double t) {
        return tlrender::renderFrameAt(&timeline, qRound64(t * 1000000.0), size);
    };

    // Real decoder + speed mapping + nested sequence, with no transition calls.
    ClipInfo fast = clip(QStringLiteral("test_assets/e2e_clip.mp4"));
    fast.inPoint = 0.0; fast.outPoint = 2.0; fast.speed = 2.0;
    TimelineSequence child;
    child.id = QStringLiteral("transition-export-child");
    ClipInfo childVideo = fast;
    childVideo.speed = 1.0;
    child.videoTracks = {{childVideo}};
    ClipInfo nested;
    nested.sequenceRefId = child.id;
    nested.filePath = timeline_nesting::sequenceClipFilePath(child.id);
    nested.duration = nested.outPoint = 2.0;
    TimelineSequence main;
    main.id = QStringLiteral("transition-export-main");
    main.videoTracks = {{fast, nested}};
    timeline.setSequences({main, child}, main.id);
    bool identity = fixtures;
    for (double t : {0.0, 0.25, 0.9, 1.0, 1.5, 2.8}) {
        tlrender::setTransitionStepsEnabledForTest(false);
        const QImage disabled = render(t);
        identity &= tlrender::transitionStepCallCountForTest() == 0;
        tlrender::setTransitionStepsEnabledForTest(true);
        identity &= bitsEqual(disabled, render(t));
        identity &= tlrender::transitionStepCallCountForTest() == 0;
    }
    gate(1, identity);

    timeline.trackAt(false, 0)->setClips({a, b});
    // Use the same still-image reader as export and preview harvesting.
    const QImage layerA = tlrender::readTransitionStillFrame(redPath);
    const QImage layerB = tlrender::readTransitionStillFrame(bluePath);
    const bool layersValid = fixtures && pixelsEqual(layerA, red) && pixelsEqual(layerB, blue);
    bool dissolve = layersValid;
    // All alignments use the same handle-borrowing helper; the cut is T=2.
    for (TransitionAlignment alignment : {TransitionAlignment::Center,
             TransitionAlignment::Start, TransitionAlignment::End}) {
        transitionPair(timeline, a, b, TransitionType::CrossDissolve, alignment);
        const auto ivs = timeline.videoOverlapIntervals();
        if (ivs.isEmpty() || ivs[0].size() != 2) { dissolve = false; continue; }
        const double expectedStart = alignment == TransitionAlignment::Start ? 2.0
            : alignment == TransitionAlignment::End ? 1.0 : 1.5;
        dissolve &= std::abs(ivs[0][1].timelineStart - expectedStart) < 1e-9;
        dissolve &= std::abs(ivs[0][0].timelineEnd - (expectedStart + 1.0)) < 1e-9;
        const double mid = expectedStart + 0.5;
        dissolve &= mse(render(mid), OverlayRenderer::applyTransition(
            layerA, layerB, TransitionType::CrossDissolve, 0.5)) < 1.0;
    }
    // A transition elsewhere must not change ClipInfo timing outside its
    // overlap, even when the active source is sped up or a nested sequence.
    timeline.trackAt(false, 0)->setClips({fast, childVideo, nested});
    const QVector<double> outsideTimes{0.1, 2.75, 3.5};
    QVector<QImage> outsideFrames;
    for (double t : outsideTimes) outsideFrames.append(render(t));
    ClipInfo fastTransition = fast, nextTransition = childVideo;
    fastTransition.trailOut.type = nextTransition.leadIn.type = TransitionType::CrossDissolve;
    fastTransition.trailOut.duration = nextTransition.leadIn.duration = 1.0;
    timeline.trackAt(false, 0)->setClips({fastTransition, nextTransition, nested});
    for (int i = 0; i < outsideTimes.size(); ++i)
        dissolve &= bitsEqual(outsideFrames[i], render(outsideTimes[i]));
    gate(2, dissolve);

    transitionPair(timeline, a, b, TransitionType::WipeLeft);
    const QImage wipe = render(2.0);
    const QColor left = meanColor(wipe, 0, size.width() / 2);
    const QColor right = meanColor(wipe, size.width() / 2, size.width());
    gate(3, !wipe.isNull() && left.blue() > 245 && left.red() < 8
        && right.red() > 245 && right.blue() < 8);

    transitionPair(timeline, a, b, TransitionType::CrossDissolve);
    const QImage linear = render(1.75);
    transitionPair(timeline, a, b, TransitionType::CrossDissolve,
        TransitionAlignment::Center, TransitionEasing::EaseIn);
    const QImage eased = render(1.75);
    gate(4, layersValid && !linear.isNull() && !eased.isNull()
        && mse(linear, OverlayRenderer::applyTransition(
            layerA, layerB, TransitionType::CrossDissolve, 0.25)) < 1.0
        && mse(eased, linear) > 1.0 && mse(eased, OverlayRenderer::applyTransition(
        layerA, layerB, TransitionType::CrossDissolve,
        applyEasing(0.25, TransitionEasing::EaseIn))) < 1.0);

    ClipInfo fade = a;
    fade.leadIn.type = TransitionType::FadeIn;
    fade.leadIn.duration = 0.5;
    timeline.trackAt(false, 0)->setClips({fade});
    const QImage first = render(1.0 / 30.0);
    const QColor mean = meanColor(first, 0, size.width());
    gate(5, layersValid && !first.isNull() && mean.red() > 8
        && mean.green() == 0 && mean.blue() == 0
        && (mean.red() + mean.green() + mean.blue()) / 3.0 < 8.0
        && pixelsEqual(render(0.6), layerA));

    // Inject only decoded pixels: sequence lookup, neighbour selection,
    // displayFrame, and m_currentFrameImage are the real preview path.
    transitionPair(timeline, a, b, TransitionType::CrossDissolve,
        TransitionAlignment::Center, TransitionEasing::EaseIn);
    const QVector<PlaybackEntry> sequence = timeline.computePlaybackSequence();
    bool previewParity = layersValid && sequence.size() == 2;
    if (previewParity) {
        tlrender::setTransitionStepsEnabledForTest(true);
        const QImage preview = VideoPlayer::displayFrameForTest(
            layerA, layerB, sequence, 0, 1750000);
        previewParity &= tlrender::transitionStepCallCountForTest() == 1;
        const QImage shared = tlrender::applyOverlapTransitionStep(
            layerA, layerB, timeline.videoOverlapIntervals()[0][0], 1.75);
        previewParity &= pixelsEqual(preview, shared) && pixelsEqual(render(1.75), shared);
        // Also traverse the real still-image harvest without pixel injection.
        tlrender::setTransitionStepsEnabledForTest(true);
        const QImage harvested = VideoPlayer::displayFrameForTest(
            layerA, QImage(), sequence, 0, 1750000);
        previewParity &= pixelsEqual(harvested, shared)
            && tlrender::transitionStepCallCountForTest() == 1;
    }
    gate(6, previewParity);
    tlrender::setTransitionStepsEnabledForTest(true);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
