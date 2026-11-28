#include "../GraphEditorPanel.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../EffectParamSchema.h"
#include "../TransformAnimator.h"
#include "../clipanim/ClipAnim.h"

#include <QApplication>
#include <QAction>
#include <QListWidget>
#include <QMenu>
#include <cmath>
#include <cstdio>

namespace {
bool samePayload(KeyframePoint actual, KeyframePoint expected)
{
    return actual.value == expected.value
        && actual.interpolation == expected.interpolation
        && actual.bezX1 == expected.bezX1 && actual.bezY1 == expected.bezY1
        && actual.bezX2 == expected.bezX2 && actual.bezY2 == expected.bezY2
        && actual.hasSpatialTangent == expected.hasSpatialTangent
        && actual.spatialOutX == expected.spatialOutX && actual.spatialOutY == expected.spatialOutY
        && actual.spatialInX == expected.spatialInX && actual.spatialInY == expected.spatialInY;
}

bool sameManager(const KeyframeManager &actual, const KeyframeManager &expected)
{
    if (actual.toJson() != expected.toJson())
        return false;
    for (const auto &track : expected.tracks()) {
        const auto *restored = actual.track(track.propertyName());
        if (!restored || restored->count() != track.count())
            return false;
        for (int i = 0; i < track.count(); ++i) {
            if (restored->keyframes()[i].time != track.keyframes()[i].time
                || !samePayload(restored->keyframes()[i], track.keyframes()[i]))
                return false;
        }
    }
    return true;
}
}

int runKeyframeCopySelftest()
{
    int passed = 0, failed = 0;
    auto gate = [&](int number, bool ok) {
        std::fprintf(stderr, "[keyframe-copy] %s G%d\n", ok ? "PASS" : "FAIL", number);
        ok ? ++passed : ++failed;
    };
    const QString sourceProperty = QStringLiteral("motion.scale");
    const QString targetProperty = QStringLiteral("effect.0.levels");
    KeyframeTrack source(sourceProperty, 1.0);
    // Exercise every interpolation ordinal, including Bezier and ElasticOut,
    // and non-default handles even on non-Bezier keys.
    QVector<int> indices;
    for (int i = 0; i < 9; ++i) {
        source.addKeyframe(1.0 + i * 0.25, i % 2 ? -12.0 : 80.0,
            static_cast<KeyframePoint::Interpolation>(i),
            0.12 + i * 0.01, -0.3, 0.77, 1.4, i % 2 == 0,
            0.11 + i, -0.22, 0.33, -0.44 - i);
        indices.append(i);
    }
    source.addKeyframe(10.0, 3.0); // Unselected sentinel must never be copied.
    ClipInfo clip;
    // Model-only test: no decoder or reference image is used.
    clip.filePath = QStringLiteral("keyframe-copy-model.mov");
    clip.duration = 12.0;
    clip.outPoint = 12.0;
    clip.keyframes.addTrack(source);
    ClipInfo destination = clip;
    destination.keyframes = KeyframeManager();
    destination.keyframes.addTrack(KeyframeTrack(targetProperty, 4.0));
    VideoEffect effect;
    effect.type = VideoEffectType::Posterize;
    destination.effects.append(effect);
    destination.leadInSec = 2.0;

    Timeline timeline;
    timeline.videoTracks()[0]->addClip(clip);
    timeline.videoTracks()[0]->addClip(destination);
    timeline.videoTracks()[0]->setSelectedClip(0);
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("コピー前"));
    GraphEditorPanel panel;
    panel.setTimeline(&timeline);
    panel.setSelectedClip(0, 0);
    auto model = [&](int index) { return timeline.videoTracks()[0]->clips()[index].keyframes; };
    const auto original = model(0);
    const int before = timeline.undoManager()->currentIndex();
    bool copied = panel.selectKeyframes(sourceProperty, indices) && panel.copySelectedKeyframes();
    panel.setPlayheadSeconds(5.0);
    bool pasted = panel.pasteKeyframes();
    auto shifted = model(0);
    const auto *shiftedTrack = shifted.track(sourceProperty);
    bool g1 = copied && pasted && shiftedTrack && shiftedTrack->count() == 19;
    if (g1) {
        for (int i = 0; i < 9; ++i) {
            const auto &key = shiftedTrack->keyframes()[9 + i];
            g1 = g1 && std::abs(key.time - (5.0 + i * 0.25)) < 1e-9
                && samePayload(key, source.keyframes()[i]);
        }
    }
    gate(1, g1);
    const bool singleStep = timeline.undoManager()->currentIndex() == before + 1;
    timeline.undo();
    QApplication::processEvents();
    bool undoOk = singleStep && timeline.undoManager()->currentIndex() == before
        && sameManager(model(0), original);

    // A separate panel demonstrates that the internal clipboard survives a
    // clip/panel change. Timeline time 18 = destination local time 4.
    GraphEditorPanel otherPanel;
    otherPanel.setTimeline(&timeline);
    otherPanel.setSelectedClip(0, 1);
    otherPanel.setPlayheadSeconds(18.0);
    const auto destinationBefore = model(1);
    const int crossBefore = timeline.undoManager()->currentIndex();
    bool g2 = otherPanel.pasteKeyframes();
    const auto pastedClip = timeline.videoTracks()[0]->clips()[1];
    const auto *target = pastedClip.keyframes.track(targetProperty);
    g2 = g2 && target && target->count() == 9;
    if (g2) {
        for (int i = 0; i < 9; ++i) {
            const double time = 4.0 + i * 0.25;
            const auto &key = target->keyframes()[i];
            const auto evaluated = clipanim::effectiveEffectsAt(pastedClip, time + 1e-6);
            g2 = g2 && std::abs(key.time - time) < 1e-9
                && samePayload(key, source.keyframes()[i])
                && evaluated.size() == 1
                && effectctrl::paramValue(evaluated[0], QStringLiteral("levels"))
                    == (i % 2 ? 2.0 : 32.0);
        }
    }
    gate(2, g2);
    undoOk = undoOk && timeline.undoManager()->currentIndex() == crossBefore + 1;
    timeline.undo();
    QApplication::processEvents();
    undoOk = undoOk && timeline.undoManager()->currentIndex() == crossBefore
        && sameManager(model(1), destinationBefore) && sameManager(model(0), original);

    panel.setSelectedClip(0, 0);
    panel.setPlayheadSeconds(1.25);
    const int collisionBefore = timeline.undoManager()->currentIndex();
    bool g3 = panel.pasteKeyframes();
    auto collision = model(0);
    const auto *collisionTrack = collision.track(sourceProperty);
    g3 = g3 && collisionTrack && collisionTrack->count() == 11;
    if (g3) {
        for (int i = 0; i < 9; ++i) {
            const auto &key = collisionTrack->keyframes()[i + 1];
            g3 = g3 && std::abs(key.time - (1.25 + i * 0.25)) < 1e-9
                && samePayload(key, source.keyframes()[i]);
        }
    }
    gate(3, g3);
    undoOk = undoOk && timeline.undoManager()->currentIndex() == collisionBefore + 1;
    timeline.undo();
    QApplication::processEvents();
    undoOk = undoOk && timeline.undoManager()->currentIndex() == collisionBefore
        && sameManager(model(0), original) && sameManager(model(1), destinationBefore);
    gate(4, undoOk);
    // G5: a separate, completely unanimated clip offers the source property
    // without mutating the model, then creates it through the toolbar action.
    ClipInfo unanimated;
    unanimated.filePath = QStringLiteral("keyframe-copy-empty-model.mov");
    unanimated.duration = 12.0;
    unanimated.outPoint = 12.0;
    unanimated.videoScale = 2.75;
    Timeline emptyTimeline;
    emptyTimeline.videoTracks()[0]->addClip(unanimated);
    emptyTimeline.videoTracks()[0]->setSelectedClip(0);
    emptyTimeline.undoManager()->saveState(emptyTimeline.currentState(), QStringLiteral("貼り付け前"));
    GraphEditorPanel emptyPanel;
    emptyPanel.setTimeline(&emptyTimeline);
    emptyPanel.setSelectedClip(0, 0);
    emptyPanel.setPlayheadSeconds(3.5);
    const int emptyBefore = emptyTimeline.undoManager()->currentIndex();
    auto emptyModel = [&]() { return emptyTimeline.videoTracks()[0]->clips()[0].keyframes; };
    bool g5 = emptyModel().tracks().isEmpty();
    const auto *destinations = emptyPanel.findChild<QListWidget *>(QStringLiteral("GraphEditorTrackList"));
    const auto destinationsByType = emptyPanel.findChildren<QListWidget *>();
    std::fprintf(stderr, "[keyframe-copy] G5 found=%s typeOnlyFirst=%s lists=%lld\n",
                 destinations ? qPrintable(destinations->objectName()) : "(null)",
                 destinationsByType.isEmpty() ? "(none)" : qPrintable(destinationsByType.first()->objectName()),
                 static_cast<long long>(destinationsByType.size()));
    g5 = g5 && destinations && destinations->currentItem()
        && destinations->currentItem()->data(Qt::UserRole).toString() == sourceProperty;
    QAction *pasteAction = nullptr;
    for (auto *menu : emptyPanel.findChildren<QMenu *>()) {
        QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
        for (auto *action : menu->actions()) {
            if (action->text() == QStringLiteral("再生ヘッド位置にキーフレームを貼り付け"))
                pasteAction = action;
        }
    }
    g5 = g5 && emptyModel().tracks().isEmpty()
        && emptyTimeline.undoManager()->currentIndex() == emptyBefore
        && pasteAction && pasteAction->isEnabled();
    if (pasteAction)
        pasteAction->trigger();
    const auto newManager = emptyModel();
    const auto *newTrack = newManager.track(sourceProperty);
    g5 = g5 && newManager.tracks().size() == 1 && newTrack
        && newTrack->defaultValue() == unanimated.videoScale && newTrack->count() == 9
        && emptyTimeline.undoManager()->currentIndex() == emptyBefore + 1;
    if (newTrack && newTrack->count() == 9) {
        for (int i = 0; i < 9; ++i) {
            const auto &key = newTrack->keyframes()[i];
            g5 = g5 && std::abs(key.time - (3.5 + i * 0.25)) < 1e-9
                && samePayload(key, source.keyframes()[i]);
        }
    }
    emptyTimeline.undo();
    QApplication::processEvents();
    g5 = g5 && emptyTimeline.undoManager()->currentIndex() == emptyBefore
        && sameManager(emptyModel(), unanimated.keyframes);
    gate(5, g5);

    // G6: Dynamic Zoom's unprefixed properties, plus every other transform
    // property, must create a destination on a completely unanimated clip.
    bool g6 = true;
    const QVector<TransformProperty> transformProperties = {
        TransformProperty::PositionX, TransformProperty::PositionY,
        TransformProperty::ScaleX, TransformProperty::ScaleY,
        TransformProperty::Rotation, TransformProperty::Opacity,
        TransformProperty::AnchorX, TransformProperty::AnchorY,
        TransformProperty::SkewX, TransformProperty::SkewY
    };
    for (const auto property : transformProperties) {
        const QString name = TransformAnimator::propertyName(property);
        KeyframeTrack transformSource(name, TransformAnimator::propertyDefaultValue(property));
        for (const auto &point : source.keyframes()) {
            transformSource.addKeyframe(point.time, point.value, point.interpolation,
                point.bezX1, point.bezY1, point.bezX2, point.bezY2,
                point.hasSpatialTangent, point.spatialOutX, point.spatialOutY,
                point.spatialInX, point.spatialInY);
        }
        ClipInfo transformClip = unanimated;
        transformClip.keyframes.addTrack(transformSource);
        Timeline transformTimeline;
        transformTimeline.videoTracks()[0]->addClip(transformClip);
        transformTimeline.videoTracks()[0]->addClip(unanimated);
        transformTimeline.videoTracks()[0]->setSelectedClip(0);
        transformTimeline.undoManager()->saveState(transformTimeline.currentState(),
                                                  QStringLiteral("変形貼り付け前"));
        GraphEditorPanel transformPanel;
        transformPanel.setTimeline(&transformTimeline);
        transformPanel.setSelectedClip(0, 0);
        const bool didCopy = transformPanel.selectKeyframes(name, indices)
            && transformPanel.copySelectedKeyframes();
        transformPanel.setSelectedClip(0, 1);
        transformPanel.setPlayheadSeconds(15.5); // second clip local time 3.5
        const int undoBefore = transformTimeline.undoManager()->currentIndex();
        g6 = g6 && transformTimeline.videoTracks()[0]->clips()[1].keyframes.tracks().isEmpty();
        const bool didPaste = transformPanel.pasteKeyframes();
        const auto result = transformTimeline.videoTracks()[0]->clips()[1].keyframes;
        const auto *created = result.track(name);
        g6 = g6 && didCopy && didPaste && result.tracks().size() == 1 && created
            && created->defaultValue() == TransformAnimator::propertyDefaultValue(property)
            && created->count() == 9
            && transformTimeline.undoManager()->currentIndex() == undoBefore + 1;
        if (created && created->count() == 9) {
            for (int i = 0; i < 9; ++i) {
                g6 = g6 && std::abs(created->keyframes()[i].time - (3.5 + i * 0.25)) < 1e-9
                    && samePayload(created->keyframes()[i], source.keyframes()[i]);
            }
        }
        transformTimeline.undo();
        QApplication::processEvents();
        g6 = g6 && transformTimeline.undoManager()->currentIndex() == undoBefore
            && sameManager(transformTimeline.videoTracks()[0]->clips()[1].keyframes,
                           unanimated.keyframes)
            && sameManager(transformTimeline.videoTracks()[0]->clips()[0].keyframes,
                           transformClip.keyframes);
    }
    // Unknown names must not inherit propertyFromName's PositionX fallback.
    ClipInfo unknownClip = unanimated;
    KeyframeTrack unknownSource = source;
    unknownSource.setPropertyName(QStringLiteral("unknown.transform.property"));
    unknownClip.keyframes.addTrack(unknownSource);
    Timeline unknownTimeline;
    unknownTimeline.videoTracks()[0]->addClip(unknownClip);
    unknownTimeline.videoTracks()[0]->addClip(unanimated);
    GraphEditorPanel unknownPanel;
    unknownPanel.setTimeline(&unknownTimeline);
    unknownPanel.setSelectedClip(0, 0);
    const bool unknownCopied = unknownPanel.selectKeyframes(unknownSource.propertyName(), indices)
        && unknownPanel.copySelectedKeyframes();
    unknownPanel.setSelectedClip(0, 1);
    const int unknownBefore = unknownTimeline.undoManager()->currentIndex();
    const bool unknownPasted = unknownPanel.pasteKeyframes();
    g6 = g6 && unknownCopied && !unknownPasted
        && unknownTimeline.videoTracks()[0]->clips()[1].keyframes.tracks().isEmpty()
        && unknownTimeline.undoManager()->currentIndex() == unknownBefore;
    gate(6, g6);

    // G7: exercise the production edit-callback filter. Real empty tracks
    // remain editable; placeholders never enter the addTrack loop, even if
    // a future view accidentally supplies keys for a placeholder.
    GraphEditorCurveTrack realCurve;
    realCurve.propertyName = sourceProperty;
    realCurve.track = source;
    KeyframePoint editedPoint = realCurve.track.keyframes().first();
    editedPoint.value = 42.0;
    realCurve.track.setKeyframePoint(0, editedPoint);
    GraphEditorCurveTrack placeholder;
    placeholder.propertyName = QStringLiteral("motion.opacity");
    placeholder.track = KeyframeTrack(placeholder.propertyName, 1.0);
    placeholder.isPastePlaceholder = true;
    GraphEditorCurveTrack realEmpty;
    realEmpty.propertyName = QStringLiteral("motion.rotation");
    realEmpty.track = KeyframeTrack(realEmpty.propertyName, 0.0);
    bool g7 = true;
    for (bool populated : {false, true}) {
        if (populated)
            placeholder.track.addKeyframe(1.0, 0.5);
        const auto editable = graphEditorEditableCurves({realCurve, placeholder, realEmpty});
        KeyframeManager editedManager;
        editedManager.addTrack(source);
        editedManager.addTrack(realEmpty.track);
        for (const auto &curve : editable) {
            KeyframeTrack editedTrack = curve.track;
            editedTrack.setPropertyName(curve.propertyName);
            editedManager.addTrack(editedTrack);
        }
        const auto *edited = editedManager.track(sourceProperty);
        g7 = g7 && editable.size() == 2 && editedManager.tracks().size() == 2
            && !editedManager.hasTrack(placeholder.propertyName)
            && editedManager.hasTrack(realEmpty.propertyName)
            && edited && samePayload(edited->keyframes().first(), editedPoint);
    }
    gate(7, g7);
    std::fprintf(stderr, "[keyframe-copy] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
