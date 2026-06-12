#include "../GraphEditorPanel.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../clipanim/ClipAnim.h"

#include <QApplication>
#include <QComboBox>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QVector>
#include <QWidget>

#include <cmath>

namespace {

constexpr double kEpsilon = 1e-9;
constexpr int kGraphLeft = 118;
constexpr int kGraphTop = 28;
constexpr int kLaneHeight = 78;

bool nearlyEqual(double a, double b)
{
    return std::fabs(a - b) <= kEpsilon;
}

void pass(const char *gate, int &passed)
{
    ++passed;
    qInfo().noquote() << "[graph-editor] PASS" << gate;
}

void fail(const char *gate, const QString &message, int &failed)
{
    ++failed;
    qWarning().noquote() << "[graph-editor] FAIL" << gate << ":" << message;
}

bool expect(bool condition,
            const char *gate,
            const QString &message,
            int &passed,
            int &failed)
{
    if (condition) {
        pass(gate, passed);
        return true;
    }
    fail(gate, message, failed);
    return false;
}

bool sameKeyframe(const KeyframePoint &a, const KeyframePoint &b)
{
    return nearlyEqual(a.time, b.time)
        && nearlyEqual(a.value, b.value)
        && a.interpolation == b.interpolation
        && nearlyEqual(a.bezX1, b.bezX1)
        && nearlyEqual(a.bezY1, b.bezY1)
        && nearlyEqual(a.bezX2, b.bezX2)
        && nearlyEqual(a.bezY2, b.bezY2)
        && a.hasSpatialTangent == b.hasSpatialTangent
        && nearlyEqual(a.spatialOutX, b.spatialOutX)
        && nearlyEqual(a.spatialOutY, b.spatialOutY)
        && nearlyEqual(a.spatialInX, b.spatialInX)
        && nearlyEqual(a.spatialInY, b.spatialInY);
}

bool sameTrack(const KeyframeTrack *a, const KeyframeTrack *b)
{
    if (!a || !b)
        return false;
    if (a->propertyName() != b->propertyName()
        || !nearlyEqual(a->defaultValue(), b->defaultValue())
        || a->keyframes().size() != b->keyframes().size()) {
        return false;
    }
    for (int i = 0; i < a->keyframes().size(); ++i) {
        if (!sameKeyframe(a->keyframes()[i], b->keyframes()[i]))
            return false;
    }
    return true;
}

KeyframeTrack makeScaleTrack(KeyframePoint::Interpolation firstInterpolation)
{
    KeyframeTrack track(QStringLiteral("motion.scale"), 1.0);
    track.addKeyframe(0.0, 1.0, firstInterpolation);
    track.addKeyframe(2.0, 1.8, KeyframePoint::Linear);
    track.addKeyframe(4.0, 2.2, KeyframePoint::Linear);
    return track;
}

KeyframeTrack makeOpacityTrack()
{
    KeyframeTrack track(QStringLiteral("motion.opacity"), 1.0);
    track.addKeyframe(0.0, 1.0, KeyframePoint::Hold);
    track.addKeyframe(1.0, 0.4, KeyframePoint::Linear);
    track.addKeyframe(3.5, 0.9, KeyframePoint::EaseInOut);
    return track;
}

KeyframeManager makeGraphModel()
{
    KeyframeManager manager;
    manager.addTrack(makeScaleTrack(KeyframePoint::Bezier));
    manager.addTrack(makeOpacityTrack());
    manager.setLoopOutMode(QStringLiteral("motion.opacity"), LoopMode::PingPong);
    return manager;
}

ClipInfo makeClip(const KeyframeManager &manager)
{
    ClipInfo clip;
    clip.filePath = QStringLiteral("graph-editor-selftest.mov");
    clip.displayName = QStringLiteral("graph-editor-selftest");
    clip.duration = 4.0;
    clip.outPoint = 4.0;
    clip.videoScale = 1.0;
    clip.opacity = 1.0;
    clip.keyframes = manager;
    return clip;
}

const ClipInfo *firstVideoClip(const Timeline &timeline)
{
    if (timeline.videoTracks().isEmpty() || !timeline.videoTracks()[0])
        return nullptr;
    const auto &clips = timeline.videoTracks()[0]->clips();
    if (clips.isEmpty())
        return nullptr;
    return &clips[0];
}

QWidget *findCurveWidget(GraphEditorPanel &panel)
{
    const auto widgets = panel.findChildren<QWidget *>();
    for (QWidget *widget : widgets) {
        if (!widget)
            continue;
        if (qobject_cast<QScrollArea *>(widget))
            continue;
        if (widget->minimumWidth() == 420)
            return widget;
    }
    return nullptr;
}

QComboBox *findPresetCombo(GraphEditorPanel &panel)
{
    const auto combos = panel.findChildren<QComboBox *>();
    for (QComboBox *combo : combos) {
        if (!combo)
            continue;
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemText(i) == QStringLiteral("Ease Out"))
                return combo;
        }
    }
    return nullptr;
}

QPushButton *findApplyButton(GraphEditorPanel &panel)
{
    const auto buttons = panel.findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button && button->text() == QStringLiteral("Apply"))
            return button;
    }
    return nullptr;
}

QPoint keyframeCenter(const QWidget *curveWidget)
{
    const int x = kGraphLeft;
    const int y = kGraphTop + kLaneHeight - 1;
    Q_UNUSED(curveWidget);
    return QPoint(x, y);
}

bool selectFirstScaleKeyframe(QWidget *curveWidget)
{
    if (!curveWidget)
        return false;
    const QPoint center = keyframeCenter(curveWidget);
    QMouseEvent press(QEvent::MouseButtonPress, center,
                      curveWidget->mapToGlobal(center),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(curveWidget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, center,
                        curveWidget->mapToGlobal(center),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(curveWidget, &release);
    QApplication::processEvents();
    return press.isAccepted() || release.isAccepted();
}

bool applyEaseOutThroughPanel(GraphEditorPanel &panel)
{
    QWidget *curveWidget = findCurveWidget(panel);
    QComboBox *presetCombo = findPresetCombo(panel);
    QPushButton *applyButton = findApplyButton(panel);
    if (!curveWidget || !presetCombo || !applyButton)
        return false;

    curveWidget->resize(qMax(curveWidget->width(), 720),
                        qMax(curveWidget->height(), 220));
    QApplication::processEvents();
    if (!selectFirstScaleKeyframe(curveWidget))
        return false;

    const int easeOutIndex = presetCombo->findText(QStringLiteral("Ease Out"));
    if (easeOutIndex < 0)
        return false;
    presetCombo->setCurrentIndex(easeOutIndex);
    applyButton->click();
    QApplication::processEvents();
    return true;
}

} // namespace

int runGraphEditorSelftest()
{
    qInfo().noquote() << "[graph-editor] selftest start";
    int passed = 0;
    int failed = 0;

    const KeyframeManager originalModel = makeGraphModel();
    KeyframeManager roundTrippedModel;
    roundTrippedModel.fromJson(originalModel.toJson());
    const bool roundtripOk =
        sameTrack(originalModel.track(QStringLiteral("motion.scale")),
                  roundTrippedModel.track(QStringLiteral("motion.scale")))
        && sameTrack(originalModel.track(QStringLiteral("motion.opacity")),
                     roundTrippedModel.track(QStringLiteral("motion.opacity")))
        && roundTrippedModel.loopOutMode(QStringLiteral("motion.opacity"))
               == LoopMode::PingPong
        && QJsonDocument(originalModel.toJson()).toJson(QJsonDocument::Compact)
               == QJsonDocument(roundTrippedModel.toJson()).toJson(QJsonDocument::Compact);
    expect(roundtripOk,
           "G1 track edit roundtrip preserves graph model",
           QStringLiteral("numeric tracks or loop mode changed after JSON roundtrip"),
           passed, failed);

    Timeline timeline;
    timeline.videoTracks()[0]->addClip(makeClip(originalModel));
    timeline.videoTracks()[0]->setSelectedClip(0);
    if (timeline.undoManager())
        timeline.undoManager()->saveState(timeline.currentState(),
                                          QStringLiteral("Graph editor baseline"));

    GraphEditorPanel panel;
    panel.resize(900, 360);
    panel.setTimeline(&timeline);
    panel.setSelectedClip(0, 0);
    panel.show();
    QApplication::processEvents();

    const UndoManager *undo = timeline.undoManager();
    const int undoIndexBeforePanelEdit = undo ? undo->currentIndex() : -1;
    const bool panelEditApplied = applyEaseOutThroughPanel(panel);
    const ClipInfo *editedClip = firstVideoClip(timeline);
    const KeyframeTrack *editedScale =
        editedClip ? editedClip->keyframes.track(QStringLiteral("motion.scale")) : nullptr;
    const bool panelEditOk =
        panelEditApplied
        && editedScale
        && !editedScale->keyframes().isEmpty()
        && editedScale->keyframes().first().interpolation == KeyframePoint::EaseOut
        && undo
        && undo->currentIndex() == undoIndexBeforePanelEdit + 1;
    expect(panelEditOk,
           "G2 panel edit commits one graph undo step",
           QStringLiteral("panel edit failed, easing did not change, or undo index did not advance once"),
           passed, failed);

    bool evaluationParityOk = false;
    if (editedClip && editedScale) {
        evaluationParityOk = true;
        const QVector<double> sampleTimes{0.0, 0.25, 1.0, 1.5, 2.75, 4.0};
        for (double time : sampleTimes) {
            const double graphValue =
                editedClip->keyframes.valueAt(QStringLiteral("motion.scale"), time,
                                              editedClip->videoScale);
            const double renderValue =
                clipanim::effectiveTransformAt(*editedClip, time).videoScale;
            if (!nearlyEqual(graphValue, renderValue)) {
                evaluationParityOk = false;
                break;
            }
        }
    }
    expect(evaluationParityOk,
           "G3 panel-edited curve matches ClipAnim render evaluation",
           QStringLiteral("GraphEditor model valueAt diverged from clipanim::effectiveTransformAt"),
           passed, failed);

    if (timeline.canUndo())
        timeline.undo();
    QApplication::processEvents();

    const ClipInfo *restoredClip = firstVideoClip(timeline);
    const KeyframeTrack *restoredScale =
        restoredClip ? restoredClip->keyframes.track(QStringLiteral("motion.scale")) : nullptr;
    const KeyframeTrack *restoredOpacity =
        restoredClip ? restoredClip->keyframes.track(QStringLiteral("motion.opacity")) : nullptr;
    const bool undoRestoredOk =
        restoredClip
        && sameTrack(originalModel.track(QStringLiteral("motion.scale")), restoredScale)
        && sameTrack(originalModel.track(QStringLiteral("motion.opacity")), restoredOpacity)
        && restoredClip->keyframes.loopOutMode(QStringLiteral("motion.opacity"))
               == LoopMode::PingPong
        && undo
        && undo->currentIndex() == undoIndexBeforePanelEdit;
    expect(undoRestoredOk,
           "G4 undo restores pre-panel graph model",
           QStringLiteral("undo did not restore the original graph keyframe model"),
           passed, failed);

    qInfo().noquote()
        << QStringLiteral("[graph-editor] summary: %1 passed, %2 failed")
               .arg(passed)
               .arg(failed);
    return failed == 0 ? 0 : 1;
}
