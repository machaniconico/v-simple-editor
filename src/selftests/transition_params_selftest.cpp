#include "../Overlay.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../ProjectFile.h"
#include <QJsonDocument>
#include <cstdio>
#include <cstring>

namespace {
bool bitsEqual(const QImage &a, const QImage &b)
{
    if (a.isNull() || a.size() != b.size() || a.format() != b.format()) return false;
    const int bytes = (a.width() * a.depth() + 7) / 8;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y), bytes) != 0) return false;
    return true;
}
bool intermediate(const QImage &image, int x, int y)
{
    const int value = image.pixelColor(x, y).red();
    return value >= 51 && value <= 204;
}
}

int runTransitionParamsSelftest()
{
    int passed = 0, failed = 0;
    const auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    QImage from(200, 100, QImage::Format_RGB888), to(from.size(), from.format());
    from.fill(Qt::black);
    to.fill(Qt::white);
    Transition t;
    t.type = TransitionType::WipeLeft;
    OverlayRenderer::setEdgeParamsEnabledForTest(true);
    const QImage hard = OverlayRenderer::applyTransition(from, to, t, 0.5);
    const bool noBranch = OverlayRenderer::edgeParamsCallCountForTest() == 0;
    OverlayRenderer::setEdgeParamsEnabledForTest(false);
    const QImage disabled = OverlayRenderer::applyTransition(from, to, t, 0.5);
    // Legacy WipeLeft reveals the LEFT side. Preserve its established direction.
    gate(1, noBranch && OverlayRenderer::edgeParamsCallCountForTest() == 0
         && bitsEqual(hard, disabled) && hard.pixelColor(99, 50) == QColor(Qt::white)
         && hard.pixelColor(100, 50) == QColor(Qt::black));
    OverlayRenderer::setEdgeParamsEnabledForTest(true);
    t.softness = 0.5;
    const QImage soft = OverlayRenderer::applyTransition(from, to, t, 0.5);
    gate(2, !t.hasDefaultEdgeParams() && intermediate(soft, 100, 50)
         && soft.pixelColor(94, 50) == QColor(Qt::white)
         && soft.pixelColor(105, 50) == QColor(Qt::black)
         && OverlayRenderer::edgeParamsCallCountForTest() == 1);
    t.softness = 0.0;
    t.borderWidth = 10.0;
    t.borderColor = Qt::red;
    const QImage border = OverlayRenderer::applyTransition(from, to, t, 0.5);
    int redPixels = 0;
    for (int x = 0; x < border.width(); ++x)
        if (border.pixelColor(x, 50) == QColor(Qt::red)) ++redPixels;
    gate(3, !t.hasDefaultEdgeParams() && redPixels >= 8 && redPixels <= 12
         && border.pixelColor(99, 50) == QColor(Qt::red)
         && border.pixelColor(100, 50) == QColor(Qt::red));
    Transition cross;
    cross.type = TransitionType::CrossDissolve;
    const QImage ordinary = OverlayRenderer::applyTransition(from, to, cross, 0.5);
    cross.softness = 0.5;
    OverlayRenderer::setEdgeParamsEnabledForTest(true);
    gate(4, bitsEqual(ordinary, OverlayRenderer::applyTransition(from, to, cross, 0.5))
         && OverlayRenderer::edgeParamsCallCountForTest() == 0);

    ProjectData data;
    ClipInfo clip;
    clip.filePath = QStringLiteral("test_assets/transition-params.png");
    clip.duration = clip.outPoint = 2.0;
    clip.trailOut = t;
    clip.trailOut.softness = 0.5;
    clip.leadIn.type = TransitionType::WipeLeft;
    data.videoTracks.append(QVector<ClipInfo>{clip});
    const QString json = ProjectFile::toJsonString(data);
    const auto saved = QJsonDocument::fromJson(json.toUtf8()).object()
        .value("videoTracks").toArray().at(0).toArray().at(0).toObject();
    const auto defaults = saved.value("leadIn").toObject();
    const auto edges = saved.value("trailOut").toObject();
    ProjectData loaded;
    bool roundTrip = ProjectFile::fromJsonString(json, loaded)
        && loaded.videoTracks.size() == 1 && loaded.videoTracks.at(0).size() == 1;
    if (roundTrip) {
        const Transition &restored = loaded.videoTracks.at(0).at(0).trailOut;
        roundTrip = restored.softness == 0.5 && restored.borderWidth == 10.0
            && restored.borderColor == QColor(Qt::red);
    }
    Transition colorOnly;
    colorOnly.borderColor = Qt::red;
    Transition widthOnly;
    widthOnly.borderWidth = 1.0;
    gate(5, roundTrip && Transition{}.hasDefaultEdgeParams()
         && !colorOnly.hasDefaultEdgeParams() && !widthOnly.hasDefaultEdgeParams()
         && defaults.contains("type") && !defaults.contains("softness")
         && !defaults.contains("borderWidth") && !defaults.contains("borderColor")
         && edges.value("softness").toDouble() == 0.5);

    bool barn = true;
    for (const auto type : {TransitionType::BarnDoorHorizontal, TransitionType::BarnDoorVertical,
                           TransitionType::BarnDoorHClose, TransitionType::BarnDoorVClose}) {
        Transition door;
        door.type = type;
        door.softness = 0.5;
        const QImage frame = OverlayRenderer::applyTransition(from, to, door, 0.5);
        const bool horizontal = type == TransitionType::BarnDoorHorizontal || type == TransitionType::BarnDoorHClose;
        barn = barn && (horizontal ? intermediate(frame, 50, 50) && intermediate(frame, 150, 50)
                                   : intermediate(frame, 100, 25) && intermediate(frame, 100, 75));
    }
    gate(6, barn);

    bool shared = true;
    Timeline timeline;
    tlrender::setTransitionStepsEnabledForTest(true);
    for (const auto type : {TransitionType::WipeLeft, TransitionType::WipeRight,
                           TransitionType::WipeUp, TransitionType::WipeDown,
                           TransitionType::BarnDoorHorizontal, TransitionType::BarnDoorVertical,
                           TransitionType::BarnDoorHClose, TransitionType::BarnDoorVClose}) {
        t.type = type;
        t.duration = 1.0;
        t.softness = 0.5;
        ClipInfo first, second;
        first.filePath = QStringLiteral("test_assets/transition-params-from.png");
        second.filePath = QStringLiteral("test_assets/transition-params-to.png");
        first.duration = second.duration = 4.0;
        first.inPoint = second.inPoint = 1.0;
        first.outPoint = second.outPoint = 3.0;
        first.trailOut = second.leadIn = t;
        // Exercise both top-level discovery and nested sequence expansion.
        for (bool nested : {false, true}) {
            TimelineSequence main, child;
            main.id = QStringLiteral("transition-params-main");
            child.id = QStringLiteral("transition-params-child");
            child.videoTracks = {{first, second}};
            ClipInfo parent;
            parent.sequenceRefId = child.id;
            parent.filePath = timeline_nesting::sequenceClipFilePath(child.id);
            parent.duration = parent.outPoint = 4.0;
            if (nested)
                main.videoTracks = {{parent}};
            else
                main.videoTracks = {{first, second}};
            timeline.setSequences({main, child}, main.id);
            const auto intervals = timeline.videoOverlapIntervals();
            const auto entries = timeline.computePlaybackSequence();
            if (intervals.isEmpty() || intervals[0].size() != 2 || entries.size() != 2) {
                shared = false;
                continue;
            }
            const auto &interval = intervals[0][0];
            const auto &entry = entries[0];
            shared &= interval.softness == t.softness && entry.softness == t.softness
                && interval.borderWidth == t.borderWidth && entry.borderWidth == t.borderWidth
                && interval.borderColor == t.borderColor && entry.borderColor == t.borderColor;
            // Differing aspect ratios exercise the legacy top-left black canvas.
            QImage tall(50, 100, QImage::Format_RGB888);
            tall.fill(Qt::white);
            OverlayRenderer::setEdgeParamsEnabledForTest(true);
            const QImage expected = OverlayRenderer::applyTransition(from, tall, t, 0.5);
            const QImage exported = tlrender::applyOverlapTransitionStep(
                from, tall, interval, interval.timelineEnd - t.duration * 0.5);
            const QImage previewed = tlrender::applyOverlapTransitionStep(
                from, tall, entry, entry.timelineEnd - t.duration * 0.5);
            shared &= bitsEqual(exported, expected) && bitsEqual(previewed, expected)
                && OverlayRenderer::edgeParamsCallCountForTest() == 3;
        }
    }
    gate(7, shared && tlrender::transitionStepCallCountForTest() == 32);
    OverlayRenderer::setEdgeParamsEnabledForTest(true);
    tlrender::setTransitionStepsEnabledForTest(true);
    std::fprintf(stderr, "summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
