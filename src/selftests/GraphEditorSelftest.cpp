#include "../GraphEditorPanel.h"
#include "../ProjectFile.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../clipanim/ClipAnim.h"

#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <cmath>
#include <functional>

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

KeyframeManager makeIndependentSpatialLoopModel()
{
    KeyframeTrack xTrack(QStringLiteral("motion.position.x"), 0.0);
    xTrack.addKeyframe(0.0, 0.0, KeyframePoint::Linear,
                       0.0, 0.0, 1.0, 1.0,
                       true, 0.25, 0.40, 0.0, 0.0);
    xTrack.addKeyframe(1.0, 1.0, KeyframePoint::Linear,
                       0.0, 0.0, 1.0, 1.0,
                       true, 0.0, 0.0, -0.25, -0.40);

    KeyframeTrack yTrack(QStringLiteral("motion.position.y"), 0.0);
    yTrack.addKeyframe(0.0, 0.0, KeyframePoint::Linear);
    yTrack.addKeyframe(1.0, 1.0, KeyframePoint::Linear);

    KeyframeManager manager;
    manager.addTrack(xTrack);
    manager.addTrack(yTrack);
    manager.setLoopOutMode(QStringLiteral("motion.position.x"), LoopMode::Cycle);
    manager.setLoopOutMode(QStringLiteral("motion.position.y"), LoopMode::None);
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

QListWidget *findTrackList(GraphEditorPanel &panel)
{
    return panel.findChild<QListWidget *>(QStringLiteral("GraphEditorTrackList"));
}

QComboBox *findLoopOutCombo(GraphEditorPanel &panel)
{
    return panel.findChild<QComboBox *>(QStringLiteral("GraphEditorLoopOutCombo"));
}

QJsonObject numericTrackJson(const KeyframeManager &manager, const QString &propertyName)
{
    const QJsonArray tracks = manager.toJson().value(QStringLiteral("tracks")).toArray();
    for (const QJsonValue &value : tracks) {
        const QJsonObject track = value.toObject();
        if (track.value(QStringLiteral("property")).toString() == propertyName)
            return track;
    }
    return {};
}

bool runAndWaitForSequenceChange(Timeline &timeline,
                                 const std::function<void()> &action)
{
    bool received = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    const QMetaObject::Connection sequenceConnection = QObject::connect(
        &timeline, &Timeline::sequenceChanged, &loop, [&]() {
            received = true;
            loop.quit();
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeout.start(1000);
    action();
    if (!received)
        loop.exec();
    QObject::disconnect(sequenceConnection);
    return received;
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

    ProjectData projectData;
    projectData.videoTracks = {{makeClip(originalModel)}};
    ProjectData loadedProjectData;
    const bool projectRoundtripOk = ProjectFile::fromJsonString(
        ProjectFile::toJsonString(projectData), loadedProjectData)
        && loadedProjectData.videoTracks.size() == 1
        && loadedProjectData.videoTracks.first().size() == 1
        && loadedProjectData.videoTracks.first().first().keyframes.loopOutMode(
               QStringLiteral("motion.opacity")) == LoopMode::PingPong
        && sameTrack(
            originalModel.track(QStringLiteral("motion.opacity")),
            loadedProjectData.videoTracks.first().first().keyframes.track(
                QStringLiteral("motion.opacity")));
    expect(projectRoundtripOk,
           "G1b ProjectFile roundtrip preserves Loop Out",
           QStringLiteral("saving and reopening a project dropped the selected track loopOut mode"),
           passed, failed);

    KeyframeManager emptyTrackLoopModel;
    emptyTrackLoopModel.addTrack(
        KeyframeTrack(QStringLiteral("motion.rotation"), 0.0));
    emptyTrackLoopModel.setLoopOutMode(
        QStringLiteral("motion.rotation"), LoopMode::Cycle);
    ProjectData emptyTrackProject;
    emptyTrackProject.videoTracks = {{makeClip(emptyTrackLoopModel)}};
    ProjectData loadedEmptyTrackProject;
    const bool emptyTrackProjectRoundtripOk = ProjectFile::fromJsonString(
        ProjectFile::toJsonString(emptyTrackProject), loadedEmptyTrackProject)
        && loadedEmptyTrackProject.videoTracks.size() == 1
        && loadedEmptyTrackProject.videoTracks.first().size() == 1
        && loadedEmptyTrackProject.videoTracks.first().first().keyframes.hasTrack(
            QStringLiteral("motion.rotation"))
        && loadedEmptyTrackProject.videoTracks.first().first().keyframes.track(
            QStringLiteral("motion.rotation"))->count() == 0
        && loadedEmptyTrackProject.videoTracks.first().first().keyframes.loopOutMode(
            QStringLiteral("motion.rotation")) == LoopMode::Cycle;
    expect(emptyTrackProjectRoundtripOk,
           "G1c ProjectFile preserves Loop Out on an empty existing track",
           QStringLiteral("a zero-keyframe track lost its Loop Out metadata on save/reopen"),
           passed, failed);

    ClipInfo independentSpatialClip = makeClip(makeIndependentSpatialLoopModel());
    const clipgeom::ClipTransform independentSpatial =
        clipanim::effectiveTransformAt(independentSpatialClip, 2.25);
    const bool independentSpatialLoopOk =
        independentSpatial.videoDx > 0.0
        && independentSpatial.videoDx < 1.0
        && nearlyEqual(independentSpatial.videoDy, 1.0);
    expect(independentSpatialLoopOk,
           "G1d spatial Position X/Y use independent Loop Out clocks",
           QStringLiteral("X Cycle incorrectly remapped the Y track despite Y=None"),
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
    QObject::connect(&timeline, &Timeline::sequenceChanged,
                     &panel, &GraphEditorPanel::refreshFromTimeline);
    QObject::connect(&timeline, &Timeline::clipSelectedOnTrack,
                     &panel, &GraphEditorPanel::setSelectedClip);
    QObject::connect(timeline.undoManager(), &UndoManager::stateChanged,
                     &panel, &GraphEditorPanel::refreshFromTimeline);
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

    QListWidget *trackList = findTrackList(panel);
    QComboBox *loopOutCombo = findLoopOutCombo(panel);
    const QStringList expectedLoopOutLabels{
        QStringLiteral("None"),
        QStringLiteral("Cycle"),
        QStringLiteral("PingPong"),
        QStringLiteral("Continue"),
    };
    QStringList actualLoopOutLabels;
    if (loopOutCombo) {
        for (int i = 0; i < loopOutCombo->count(); ++i)
            actualLoopOutLabels.append(loopOutCombo->itemText(i));
    }
    const bool loopOutUiOk =
        trackList
        && trackList->accessibleName() == QStringLiteral("Keyframe tracks")
        && loopOutCombo
        && loopOutCombo->accessibleName()
               == QStringLiteral("Loop Out mode for selected keyframe track")
        && actualLoopOutLabels == expectedLoopOutLabels
        && loopOutCombo->isEnabled()
        && trackList->currentRow() == 0
        && static_cast<LoopMode>(loopOutCombo->currentData().toInt()) == LoopMode::None;
    expect(loopOutUiOk,
           "G5 Loop Out control exposes stable accessible four-mode UI",
           QStringLiteral("named controls, accessible labels, mode order, or initial track state mismatch"),
           passed, failed);

    if (trackList)
        trackList->setCurrentRow(1);
    QApplication::processEvents();
    const bool selectionSyncOk =
        trackList
        && trackList->currentRow() == 1
        && loopOutCombo
        && static_cast<LoopMode>(loopOutCombo->currentData().toInt())
               == LoopMode::PingPong;
    expect(selectionSyncOk,
           "G6 track selection synchronizes Loop Out mode",
           QStringLiteral("selecting the opacity track did not show its PingPong mode"),
           passed, failed);

    const int undoIndexBeforeLoopOutEdits = undo ? undo->currentIndex() : -1;
    const QVector<LoopMode> uiModeSequence{
        LoopMode::Cycle,
        LoopMode::PingPong,
        LoopMode::Continue,
        LoopMode::None,
    };
    bool uiModeSequenceOk = loopOutCombo && trackList && undo;
    int appliedModeCount = 0;
    for (LoopMode mode : uiModeSequence) {
        const int modeIndex = loopOutCombo
            ? loopOutCombo->findData(static_cast<int>(mode))
            : -1;
        if (modeIndex < 0) {
            uiModeSequenceOk = false;
            continue;
        }

        loopOutCombo->setCurrentIndex(modeIndex);
        QApplication::processEvents();
        ++appliedModeCount;

        const ClipInfo *modeEditedClip = firstVideoClip(timeline);
        KeyframeManager roundTrippedMode;
        if (modeEditedClip)
            roundTrippedMode.fromJson(modeEditedClip->keyframes.toJson());
        uiModeSequenceOk = uiModeSequenceOk
            && modeEditedClip
            && sameTrack(originalModel.track(QStringLiteral("motion.scale")),
                         modeEditedClip->keyframes.track(QStringLiteral("motion.scale")))
            && sameTrack(originalModel.track(QStringLiteral("motion.opacity")),
                         modeEditedClip->keyframes.track(QStringLiteral("motion.opacity")))
            && modeEditedClip->keyframes.loopOutMode(QStringLiteral("motion.scale"))
                   == LoopMode::None
            && modeEditedClip->keyframes.loopOutMode(QStringLiteral("motion.opacity")) == mode
            && roundTrippedMode.loopOutMode(QStringLiteral("motion.scale")) == LoopMode::None
            && roundTrippedMode.loopOutMode(QStringLiteral("motion.opacity")) == mode
            && trackList->currentRow() == 1
            && static_cast<LoopMode>(loopOutCombo->currentData().toInt()) == mode
            && undo->currentIndex() == undoIndexBeforeLoopOutEdits + appliedModeCount;
    }
    expect(uiModeSequenceOk && appliedModeCount == uiModeSequence.size(),
           "G7 UI applies Cycle, PingPong, Continue, and None per track",
           QStringLiteral("mode sequence changed another track, missed JSON persistence, or skipped an undo step"),
           passed, failed);

    const ClipInfo *noneClip = firstVideoClip(timeline);
    const QJsonObject opacityTrackJson = noneClip
        ? numericTrackJson(noneClip->keyframes, QStringLiteral("motion.opacity"))
        : QJsonObject{};
    const bool noneJsonOk =
        noneClip
        && noneClip->keyframes.loopOutMode(QStringLiteral("motion.opacity")) == LoopMode::None
        && !opacityTrackJson.isEmpty()
        && !opacityTrackJson.contains(QStringLiteral("loopOut"));
    expect(noneJsonOk,
           "G8 None removes Loop Out from JSON",
           QStringLiteral("None left a stale loopOut field in the selected track JSON"),
           passed, failed);

    const int undoIndexBeforeRefresh = undo ? undo->currentIndex() : -1;
    // This explicit refresh belongs only to the read-only refresh gate.
    // Undo/redo below must update the panel through the production signals.
    panel.refreshFromTimeline();
    QApplication::processEvents();
    const bool refreshIsReadOnlyOk =
        undo
        && undo->currentIndex() == undoIndexBeforeRefresh
        && trackList
        && trackList->currentRow() == 1
        && loopOutCombo
        && static_cast<LoopMode>(loopOutCombo->currentData().toInt()) == LoopMode::None;
    expect(refreshIsReadOnlyOk,
           "G9 refresh synchronizes without adding Undo",
           QStringLiteral("refresh changed Undo history or lost the selected track mode"),
           passed, failed);

    const int undoIndexBeforeAutomaticUndo = undo ? undo->currentIndex() : -1;
    const bool undoSequenceReceived = runAndWaitForSequenceChange(timeline, [&]() {
        if (timeline.canUndo())
            timeline.undo();
    });
    const ClipInfo *loopUndoClip = firstVideoClip(timeline);
    const bool loopOutUndoOk =
        undoSequenceReceived
        && loopUndoClip
        && loopUndoClip->keyframes.loopOutMode(QStringLiteral("motion.opacity"))
               == LoopMode::Continue
        && undo
        && undo->currentIndex() == undoIndexBeforeAutomaticUndo - 1
        && trackList
        && trackList->currentRow() == 1
        && loopOutCombo
        && static_cast<LoopMode>(loopOutCombo->currentData().toInt())
               == LoopMode::Continue;
    expect(loopOutUndoOk,
           "G10 production signals synchronize Loop Out Undo",
           QStringLiteral("undo did not automatically restore the model, selected row, combo, and history index"),
           passed, failed);

    const bool redoSequenceReceived = runAndWaitForSequenceChange(timeline, [&]() {
        if (timeline.canRedo())
            timeline.redo();
    });
    const ClipInfo *loopRedoClip = firstVideoClip(timeline);
    const bool loopOutRedoOk =
        redoSequenceReceived
        && loopRedoClip
        && loopRedoClip->keyframes.loopOutMode(QStringLiteral("motion.opacity"))
               == LoopMode::None
        && undo
        && undo->currentIndex() == undoIndexBeforeAutomaticUndo
        && trackList
        && trackList->currentRow() == 1
        && loopOutCombo
        && static_cast<LoopMode>(loopOutCombo->currentData().toInt())
               == LoopMode::None;
    expect(loopOutRedoOk,
           "G11 production signals synchronize Loop Out Redo",
           QStringLiteral("redo did not automatically restore the model, selected row, combo, and history index"),
           passed, failed);

    Timeline emptyTimeline;
    emptyTimeline.videoTracks()[0]->addClip(makeClip(KeyframeManager{}));
    emptyTimeline.videoTracks()[0]->setSelectedClip(0);
    GraphEditorPanel emptyPanel;
    emptyPanel.setTimeline(&emptyTimeline);
    emptyPanel.setSelectedClip(0, 0);
    emptyPanel.show();
    QApplication::processEvents();
    QComboBox *emptyLoopOutCombo = findLoopOutCombo(emptyPanel);
    const bool noTrackDisabledOk =
        emptyLoopOutCombo
        && !emptyLoopOutCombo->isEnabled()
        && static_cast<LoopMode>(emptyLoopOutCombo->currentData().toInt()) == LoopMode::None;
    expect(noTrackDisabledOk,
           "G12 Loop Out control disables without a keyframe track",
           QStringLiteral("empty Graph Editor left Loop Out editable or stale"),
           passed, failed);

    qInfo().noquote()
        << QStringLiteral("[graph-editor] summary: %1 passed, %2 failed")
               .arg(passed)
               .arg(failed);
    return failed == 0 ? 0 : 1;
}
