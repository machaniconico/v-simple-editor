#include "../CaptionOverlayBuilder.h"
#include "../CaptionEditorDialog.h"
#include "../ProjectFile.h"
#include "../RemotionExport.h"
#include "../SubtitleTranslator.h"
#include "../Timeline.h"
#include "../TimelineFrameRenderer.h"
#include "../TextOverlayBake.h"
#include "../UndoManager.h"
#include "../libavcore/Encode.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDebug>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QtGlobal>

#include <cmath>

namespace {

constexpr double kEpsilon = 1.0e-9;

bool near(double actual, double expected)
{
    return std::abs(actual - expected) <= kEpsilon;
}

caption::Word makeWord(qint64 startMs, qint64 endMs, const QString &text)
{
    caption::Word word;
    word.startMs = startMs;
    word.endMs = endMs;
    word.text = text;
    return word;
}

caption::Clip makeClip(qint64 startMs, qint64 endMs, const QString &text,
                       const QList<caption::Word> &words = {})
{
    caption::Clip clip;
    clip.startMs = startMs;
    clip.endMs = endMs;
    clip.text = text;
    clip.words = words;
    return clip;
}

EnhancedTextOverlay makeNamedOverlay(const QString &text, const QString &templateName,
                                     double startTime, double endTime)
{
    EnhancedTextOverlay overlay;
    overlay.text = text;
    overlay.templateName = templateName;
    overlay.startTime = startTime;
    overlay.endTime = endTime;
    return overlay;
}

ClipInfo makeTimelineClip(const QString &name, double duration,
                          const QString &filePath = QString())
{
    ClipInfo clip;
    clip.filePath = filePath.isEmpty()
        ? QStringLiteral("/caption-selftest/") + name + QStringLiteral(".mp4")
        : filePath;
    clip.displayName = name;
    clip.duration = duration;
    clip.inPoint = 0.0;
    clip.outPoint = duration;
    return clip;
}

bool containsOverlayText(const ClipInfo &clip, const QString &text,
                         const QString &templateName)
{
    for (const EnhancedTextOverlay &overlay : clip.textManager.overlays()) {
        if (overlay.text == text && overlay.templateName == templateName)
            return true;
    }
    return false;
}

bool generatedRoundTripMatches(const QVector<EnhancedTextOverlay> &actual,
                               const QVector<EnhancedTextOverlay> &expected,
                               const QString &tag)
{
    if (actual.size() != expected.size())
        return false;
    for (int i = 0; i < actual.size(); ++i) {
        const EnhancedTextOverlay &value = actual.at(i);
        const EnhancedTextOverlay &oracle = expected.at(i);
        if (value.text != oracle.text
            || value.templateName != tag
            || value.font.family() != oracle.font.family()
            || value.font.pointSize() != oracle.font.pointSize()
            || value.font.bold() != oracle.font.bold()
            || value.font.italic() != oracle.font.italic()
            || value.color != oracle.color
            || value.backgroundColor != oracle.backgroundColor
            || value.outlineColor != oracle.outlineColor
            || value.outlineWidth != oracle.outlineWidth
            || !near(value.x, oracle.x)
            || !near(value.y, oracle.y)
            || !near(value.width, oracle.width)
            || !near(value.height, oracle.height)
            || value.alignment != oracle.alignment
            || value.wordWrap != oracle.wordWrap
            || value.shadow.enabled != oracle.shadow.enabled
            || value.shadow.color != oracle.shadow.color
            || !near(value.shadow.offsetX, oracle.shadow.offsetX)
            || !near(value.shadow.offsetY, oracle.shadow.offsetY)
            || !near(value.shadow.blur, oracle.shadow.blur)
            || !near(value.shadow.opacity, oracle.shadow.opacity)
            || value.animIn.type != oracle.animIn.type
            || !near(value.animIn.duration, oracle.animIn.duration)
            || !near(value.startTime, oracle.startTime)
            || !near(value.endTime, oracle.endTime)) {
            return false;
        }
    }
    return true;
}

bool clipsExcludeGeneratedCaptions(const QVector<ClipInfo> &clips,
                                   const QString &tag)
{
    for (const ClipInfo &clip : clips) {
        for (const EnhancedTextOverlay &overlay : clip.textManager.overlays()) {
            if (overlay.templateName == tag)
                return false;
        }
    }
    return true;
}

bool writeDarkSyntheticClip(const QString &path, QString *error)
{
    constexpr int kWidth = 160;
    constexpr int kHeight = 90;
    constexpr int kFps = 10;
    constexpr int kFrames = 10;

    libavcore::EncodeRequest request;
    request.width = kWidth;
    request.height = kHeight;
    request.fps = kFps;
    request.fpsNum = kFps;
    request.fpsDen = 1;
    request.videoBitrateBits = 500000;
    request.outputPath = path.toStdString();
    request.videoCodecName = "mpeg4";
    request.hwVendorHint = "none";
    request.useHardwareAccel = false;

    libavcore::FrameEncoder encoder;
    if (auto openError = encoder.open(request)) {
        if (error)
            *error = QString::fromStdString(*openError);
        return false;
    }

    QImage frame(kWidth, kHeight, QImage::Format_RGB888);
    frame.fill(QColor(20, 24, 28));
    for (int i = 0; i < kFrames; ++i) {
        if (!encoder.pushFrame(frame, i)) {
            if (error)
                *error = QStringLiteral("pushFrame failed at %1").arg(i);
            return false;
        }
    }
    if (auto finalizeError = encoder.finalize()) {
        if (error)
            *error = QString::fromStdString(*finalizeError);
        return false;
    }
    return true;
}

bool isStrongPrimary(const QColor &color, int primary)
{
    const int channels[] = {color.red(), color.green(), color.blue()};
    return channels[primary] > 150
        && channels[(primary + 1) % 3] < 110
        && channels[(primary + 2) % 3] < 110;
}

int strongPrimaryPixelCount(const QImage &source, int primary)
{
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (isStrongPrimary(color, primary))
                ++count;
        }
    }
    return count;
}

QRect strongPrimaryBounds(const QImage &source, int primary)
{
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (!isStrongPrimary(image.pixelColor(x, y), primary))
                continue;
            const QRect pixel(x, y, 1, 1);
            bounds = bounds.isNull() ? pixel : bounds.united(pixel);
        }
    }
    return bounds;
}

int strongRedPixelCount(const QImage &source)
{
    return strongPrimaryPixelCount(source, 0);
}

} // namespace

int runCaptionOverlayBuilderSelftest()
{
    int passed = 0;
    int failed = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        if (ok) {
            ++passed;
            qInfo().noquote() << QStringLiteral("[caption-overlay-builder] PASS G%1 %2")
                .arg(gate).arg(QString::fromLatin1(name));
        } else {
            ++failed;
            qCritical().noquote() << QStringLiteral("[caption-overlay-builder] FAIL G%1 %2%3")
                .arg(gate).arg(QString::fromLatin1(name))
                .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
        }
    };

    check(0, "QApplication is available", QApplication::instance() != nullptr);
    if (!QApplication::instance())
        return failed;

    caption::Track timedTrack;
    timedTrack.addClip(makeClip(
        1000, 2300,
        QString::fromUtf8("Cafe\xCC\x81\xE3\x80\x80WORLD"),
        {makeWord(1000, 1450, QString::fromUtf8("Caf\xC3\xA9")),
         makeWord(1450, 2300, QStringLiteral("WORLD"))}));

    const caption::Clip retained = timedTrack.clipAt(0);
    check(1, "caption::Clip retains recognized words",
          retained.words.size() == 2
              && retained.words.at(0).startMs == 1000
              && retained.words.at(0).endMs == 1450
              && retained.words.at(0).text == QString::fromUtf8("Caf\xC3\xA9")
              && retained.words.at(1).startMs == 1450
              && retained.words.at(1).endMs == 2300);

    caption::Style style;
    style.fontFamily = QStringLiteral("Caption Oracle Sans");
    style.fontSizePt = 37;
    style.bold = true;
    style.italic = true;
    style.textColor = QColor(12, 34, 56, 231);
    style.outlineColor = QColor(65, 43, 21, 199);
    style.outlineThickness = 3.6;
    style.shadowColor = QColor(7, 8, 9, 177);
    style.shadowOffset = QPointF(-4.5, 6.25);
    style.background = true;
    style.backgroundColor = QColor(90, 80, 70, 123);
    style.anchor = caption::Anchor::TopRight;
    style.anchorOffsetNormalized = QPointF(-0.07, 0.11);
    style.maxWidthNormalized = 0.63;

    const QVector<EnhancedTextOverlay> timed =
        CaptionOverlayBuilder::build(timedTrack, style);
    check(2, "valid NFC and Unicode-space matched words use absolute timings",
          timed.size() == 2
              && timed.at(0).text == QString::fromUtf8("Caf\xC3\xA9")
              && near(timed.at(0).startTime, 1.0)
              && near(timed.at(0).endTime, 1.45)
              && timed.at(1).text == QStringLiteral("WORLD")
              && near(timed.at(1).startTime, 1.45)
              && near(timed.at(1).endTime, 2.3));

    const QString reserved = QStringLiteral("__vse_generated_single_word_caption_v1__");
    const bool styleCopied = timed.size() == 2
        && timed.at(0).templateName == reserved
        && CaptionOverlayBuilder::generatedTemplateName() == reserved
        && timed.at(0).font.family() == style.fontFamily
        && timed.at(0).font.pointSize() == style.fontSizePt
        && timed.at(0).font.bold() == style.bold
        && timed.at(0).font.italic() == style.italic
        && timed.at(0).color == style.textColor
        && timed.at(0).outlineColor == style.outlineColor
        && timed.at(0).outlineWidth == 4
        && timed.at(0).backgroundColor == style.backgroundColor
        && near(timed.at(0).x, 0.615)
        && near(timed.at(0).y, 0.11)
        && (timed.at(0).alignment & Qt::AlignHorizontal_Mask) == Qt::AlignRight
        && near(timed.at(0).width, style.maxWidthNormalized)
        && timed.at(0).wordWrap
        && timed.at(0).shadow.enabled
        && timed.at(0).shadow.color == style.shadowColor
        && near(timed.at(0).shadow.offsetX, style.shadowOffset.x())
        && near(timed.at(0).shadow.offsetY, style.shadowOffset.y())
        && timed.at(0).animIn.type == TextAnimationType::Pop
        && near(timed.at(0).animIn.duration, 0.15);
    check(3, "all caption style fields and reserved tag propagate", styleCopied);

    check(4, "overlay activity is start-inclusive and end-exclusive",
          timed.size() == 2
              && !CaptionOverlayBuilder::isActiveAt(timed.at(0), 0.999999)
              && CaptionOverlayBuilder::isActiveAt(timed.at(0), 1.0)
              && CaptionOverlayBuilder::isActiveAt(timed.at(0), 1.449999)
              && !CaptionOverlayBuilder::isActiveAt(timed.at(0), 1.45)
              && CaptionOverlayBuilder::isActiveAt(timed.at(1), 1.45)
              && !CaptionOverlayBuilder::isActiveAt(timed.at(1), 2.3));

    caption::Track invalidTimingTrack;
    invalidTimingTrack.addClip(makeClip(
        5000, 6001, QString::fromUtf8("one\xE2\x80\x83two\tthree"),
        {makeWord(5000, 5500, QStringLiteral("one")),
         makeWord(5400, 5700, QStringLiteral("two")),
         makeWord(5700, 6001, QStringLiteral("three"))}));
    const QVector<EnhancedTextOverlay> invalidTiming =
        CaptionOverlayBuilder::build(invalidTimingTrack, caption::Style{});
    check(5, "invalid timing falls back to exact equal Unicode-whitespace ranges",
          invalidTiming.size() == 3
              && invalidTiming.at(0).text == QStringLiteral("one")
              && near(invalidTiming.at(0).startTime, 5.0)
              && near(invalidTiming.at(0).endTime, 5.333)
              && invalidTiming.at(1).text == QStringLiteral("two")
              && near(invalidTiming.at(1).startTime, 5.333)
              && near(invalidTiming.at(1).endTime, 5.667)
              && invalidTiming.at(2).text == QStringLiteral("three")
              && near(invalidTiming.at(2).startTime, 5.667)
              && near(invalidTiming.at(2).endTime, 6.001));

    caption::Track caseMismatchTrack;
    caseMismatchTrack.addClip(makeClip(
        100, 1100, QStringLiteral("Hello World"),
        {makeWord(100, 900, QStringLiteral("hello")),
         makeWord(900, 1100, QStringLiteral("World"))}));
    const QVector<EnhancedTextOverlay> caseMismatch =
        CaptionOverlayBuilder::build(caseMismatchTrack, caption::Style{});
    check(6, "case-sensitive mismatch uses segment fallback",
          caseMismatch.size() == 2
              && caseMismatch.at(0).text == QStringLiteral("Hello")
              && near(caseMismatch.at(0).startTime, 0.1)
              && near(caseMismatch.at(0).endTime, 0.6)
              && caseMismatch.at(1).text == QStringLiteral("World")
              && near(caseMismatch.at(1).startTime, 0.6)
              && near(caseMismatch.at(1).endTime, 1.1));

    caption::Track noWhitespaceTrack;
    noWhitespaceTrack.addClip(makeClip(
        7000, 8100, QString::fromUtf8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE5\xAD\x97\xE5\xB9\x95"),
        {makeWord(7000, 7500, QString::fromUtf8("\xE6\x97\xA5\xE6\x9C\xAC"))}));
    const QVector<EnhancedTextOverlay> noWhitespace =
        CaptionOverlayBuilder::build(noWhitespaceTrack, caption::Style{});
    check(7, "fallback never character-splits text without whitespace",
          noWhitespace.size() == 1
              && noWhitespace.at(0).text == QString::fromUtf8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE5\xAD\x97\xE5\xB9\x95")
              && near(noWhitespace.at(0).startTime, 7.0)
              && near(noWhitespace.at(0).endTime, 8.1));

    caption::Style noBackgroundStyle;
    noBackgroundStyle.background = false;
    noBackgroundStyle.backgroundColor = QColor(11, 22, 33, 222);
    const QVector<EnhancedTextOverlay> transparentBackground =
        CaptionOverlayBuilder::build(noWhitespaceTrack, noBackgroundStyle);
    check(8, "disabled caption background preserves RGB and clears alpha",
          transparentBackground.size() == 1
              && transparentBackground.at(0).backgroundColor.red() == 11
              && transparentBackground.at(0).backgroundColor.green() == 22
              && transparentBackground.at(0).backgroundColor.blue() == 33
              && transparentBackground.at(0).backgroundColor.alpha() == 0);

    caption::Track topAnchorTrack;
    topAnchorTrack.addClip(makeClip(0, 1000, QStringLiteral("TOP")));
    caption::Style topAnchorStyle;
    topAnchorStyle.anchor = caption::Anchor::TopCenter;
    topAnchorStyle.fontFamily = QStringLiteral("Arial");
    topAnchorStyle.fontSizePt = 28;
    topAnchorStyle.textColor = QColor(255, 0, 0);
    topAnchorStyle.outlineColor = QColor(255, 0, 0);
    topAnchorStyle.background = false;
    const QVector<EnhancedTextOverlay> topAnchorOverlays =
        CaptionOverlayBuilder::build(topAnchorTrack, topAnchorStyle);
    QImage anchorSource(160, 90, QImage::Format_ARGB32_Premultiplied);
    anchorSource.fill(QColor(20, 24, 28));
    const QImage topAnchorFrame = textbake::bakeOverlays(
        anchorSource, topAnchorOverlays, 0.5, -1, 1.0);
    const QRect topAnchorBounds = strongPrimaryBounds(topAnchorFrame, 0);

    EnhancedTextOverlay ordinaryOffscreen = makeNamedOverlay(
        QStringLiteral("OFF"), QStringLiteral("ordinary-offscreen"), 0.0, 1.0);
    ordinaryOffscreen.font = QFont(QStringLiteral("Arial"), 28, QFont::Bold);
    ordinaryOffscreen.color = QColor(255, 0, 0);
    ordinaryOffscreen.outlineColor = QColor(255, 0, 0);
    ordinaryOffscreen.backgroundColor.setAlpha(0);
    ordinaryOffscreen.x = -0.5;
    ordinaryOffscreen.y = 0.5;
    ordinaryOffscreen.width = 0.3;
    ordinaryOffscreen.alignment = Qt::AlignLeft | Qt::AlignVCenter;
    const QImage ordinaryOffscreenFrame = textbake::bakeOverlays(
        anchorSource, {ordinaryOffscreen}, 0.5, -1, 1.0);
    check(8, "top captions remain visible without clamping ordinary offscreen motion",
          topAnchorOverlays.size() == 1
              && topAnchorOverlays.first().y >= 0.05
              && topAnchorBounds.isValid()
              && topAnchorBounds.top() > 0
              && strongRedPixelCount(ordinaryOffscreenFrame) == 0);

    ClipInfo firstClip = makeTimelineClip(QStringLiteral("first"), 2.0);
    ClipInfo secondClip = makeTimelineClip(QStringLiteral("second"), 2.0);
    const QString ordinaryTag = QStringLiteral("ordinary-title-template");
    firstClip.textManager.addOverlay(
        makeNamedOverlay(QStringLiteral("ordinary-first"), ordinaryTag, 0.0, 4.0));
    firstClip.textManager.addOverlay(
        makeNamedOverlay(QStringLiteral("stale-first"), reserved, 0.0, 0.2));
    secondClip.textManager.addOverlay(
        makeNamedOverlay(QStringLiteral("ordinary-second"), ordinaryTag, 0.0, 4.0));
    secondClip.textManager.addOverlay(
        makeNamedOverlay(QStringLiteral("ordinary-second-extra"), ordinaryTag, 0.0, 4.0));
    secondClip.textManager.addOverlay(
        makeNamedOverlay(QStringLiteral("stale-second"), reserved, 0.2, 0.4));

    Timeline timeline;
    timeline.videoTracks().first()->setClips({firstClip, secondClip});
    timeline.undoManager()->clear();
    timeline.undoManager()->saveState(timeline.currentState(), QStringLiteral("caption baseline"));
    const int baselineUndoIndex = timeline.undoManager()->currentIndex();
    QString applyError;
    const bool applied = timeline.applySingleWordCaptionOverlays(timed, &applyError);
    const QVector<ClipInfo> appliedClips = timeline.videoTracks().first()->clips();
    const QVector<EnhancedTextOverlay> mergedAfterApply = timeline.timelineTextOverlays();
    const bool centralApplyOk = applied
        && applyError.isEmpty()
        && timeline.undoManager()->currentIndex() == baselineUndoIndex + 1
        && generatedRoundTripMatches(timeline.generatedCaptionOverlays(), timed, reserved)
        && appliedClips.size() == 2
        && appliedClips.at(0).textManager.count() == 1
        && appliedClips.at(1).textManager.count() == 2
        && clipsExcludeGeneratedCaptions(appliedClips, reserved)
        && containsOverlayText(appliedClips.at(0), QStringLiteral("ordinary-first"), ordinaryTag)
        && containsOverlayText(appliedClips.at(1), QStringLiteral("ordinary-second"), ordinaryTag)
        && containsOverlayText(appliedClips.at(1), QStringLiteral("ordinary-second-extra"), ordinaryTag)
        && mergedAfterApply.size() == timed.size() + 1
        && mergedAfterApply.at(1).text == timed.first().text;
    check(9, "bulk apply stores one central generated-caption set and preserves ordinary overlays",
          centralApplyOk, applyError);

    timeline.undo();
    const QVector<ClipInfo> undoClips = timeline.videoTracks().first()->clips();
    const bool undoRestored = timeline.generatedCaptionOverlays().isEmpty()
        && undoClips.size() == 2
        && containsOverlayText(undoClips.at(0), QStringLiteral("ordinary-first"), ordinaryTag)
        && containsOverlayText(undoClips.at(1), QStringLiteral("ordinary-second"), ordinaryTag)
        && containsOverlayText(undoClips.at(0), QStringLiteral("stale-first"), reserved)
        && containsOverlayText(undoClips.at(1), QStringLiteral("stale-second"), reserved)
        && !timeline.canUndo();
    timeline.redo();
    const QVector<ClipInfo> redoClips = timeline.videoTracks().first()->clips();
    const bool redoRestored = generatedRoundTripMatches(
            timeline.generatedCaptionOverlays(), timed, reserved)
        && redoClips.size() == 2
        && clipsExcludeGeneratedCaptions(redoClips, reserved);

    const int beforeReapplyIndex = timeline.undoManager()->currentIndex();
    const bool reapplied = timeline.applySingleWordCaptionOverlays(timed, &applyError);
    const QVector<ClipInfo> reappliedClips = timeline.videoTracks().first()->clips();
    const bool reapplyOk = reapplied
        && timeline.undoManager()->currentIndex() == beforeReapplyIndex + 1
        && generatedRoundTripMatches(timeline.generatedCaptionOverlays(), timed, reserved)
        && clipsExcludeGeneratedCaptions(reappliedClips, reserved)
        && reappliedClips.at(0).textManager.count() == 1
        && reappliedClips.at(1).textManager.count() == 2;

    const int ordinaryCount = reappliedClips.isEmpty()
        ? -1
        : reappliedClips.first().textManager.count();
    const int beforeTimingEditIndex = timeline.undoManager()->currentIndex();
    const bool timingEdited = ordinaryCount >= 0
        && timeline.updateTextOverlayTime(ordinaryCount, 0.13, 99.0);
    const bool timingCentralOk = timingEdited
        && timeline.generatedCaptionOverlays().size() == timed.size()
        && near(timeline.generatedCaptionOverlays().first().startTime, 0.13)
        && near(timeline.generatedCaptionOverlays().first().endTime,
                timed.at(1).startTime)
        && timeline.undoManager()->currentIndex() == beforeTimingEditIndex + 1
        && clipsExcludeGeneratedCaptions(timeline.videoTracks().first()->clips(), reserved);
    timeline.undo();
    const bool timingUndoOk = generatedRoundTripMatches(
        timeline.generatedCaptionOverlays(), timed, reserved);
    timeline.redo();
    const bool timingRedoOk = timeline.generatedCaptionOverlays().size() == timed.size()
        && near(timeline.generatedCaptionOverlays().first().startTime, 0.13)
        && near(timeline.generatedCaptionOverlays().first().endTime,
                timed.at(1).startTime);
    // Leave the original generated timing active for persistence checks.
    timeline.undo();
    check(10, "central captions survive undo/redo, duplicate-free reapply, and timing edit undo",
          undoRestored
              && redoRestored
              && reapplyOk
              && timingCentralOk
              && timingUndoOk
              && timingRedoOk
              && timeline.undoManager()->currentIndex() == beforeTimingEditIndex,
          applyError);

    QVector<EnhancedTextOverlay> manySequentialCaptions;
    constexpr int kScaleCaptionCount = 300;
    manySequentialCaptions.reserve(kScaleCaptionCount);
    for (int i = 0; i < kScaleCaptionCount; ++i) {
        const double start = 1.0 + i * 2.5;
        manySequentialCaptions.append(makeNamedOverlay(
            QStringLiteral("word-%1").arg(i), reserved, start, start + 2.0));
    }
    Timeline scaleTimeline;
    scaleTimeline.resize(1000, 400);
    scaleTimeline.videoTracks().first()->setClips({
        makeTimelineClip(QStringLiteral("caption-scale"), 800.0),
    });
    QString scaleApplyError;
    const bool scaleApplied = scaleTimeline.applySingleWordCaptionOverlays(
        manySequentialCaptions, &scaleApplyError);
    scaleTimeline.undoManager()->clear();
    scaleTimeline.undoManager()->saveState(
        scaleTimeline.currentState(), QStringLiteral("caption scale baseline"));
    const int scaleUndoIndex = scaleTimeline.undoManager()->currentIndex();
    scaleTimeline.show();
    QApplication::processEvents();
    QWidget *textStrip = scaleTimeline.findChild<QWidget *>(
        QStringLiteral("TimelineTextStrip"));
    bool scaleDragApplied = false;
    if (textStrip) {
        const QPoint pressPoint(20, qMin(10, qMax(1, textStrip->height() / 2)));
        const QPoint movePoint(10, pressPoint.y());
        QMouseEvent press(QEvent::MouseButtonPress, pressPoint,
                          textStrip->mapToGlobal(pressPoint),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(textStrip, &press);
        QMouseEvent move(QEvent::MouseMove, movePoint,
                         textStrip->mapToGlobal(movePoint),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(textStrip, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, movePoint,
                            textStrip->mapToGlobal(movePoint),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(textStrip, &release);
        QApplication::processEvents();
        const QVector<EnhancedTextOverlay> dragged =
            scaleTimeline.generatedCaptionOverlays();
        scaleDragApplied = dragged.size() == kScaleCaptionCount
            && near(dragged.first().startTime, 0.0)
            && near(dragged.first().endTime, 2.0)
            && scaleTimeline.videoTracks().first()->clips().first().textManager.count() == 0
            && scaleTimeline.undoManager()->currentIndex() == scaleUndoIndex + 1;
        scaleTimeline.undo();
        scaleDragApplied = scaleDragApplied
            && scaleTimeline.generatedCaptionOverlays().size() == kScaleCaptionCount
            && near(scaleTimeline.generatedCaptionOverlays().first().startTime, 1.0);
    }
    scaleTimeline.hide();

    constexpr int kStressCaptionCount = 9000;
    constexpr int kStressClipCount = 100;
    QVector<EnhancedTextOverlay> stressCaptions;
    stressCaptions.reserve(kStressCaptionCount);
    for (int i = 0; i < kStressCaptionCount; ++i) {
        const double start = i * 0.02;
        stressCaptions.append(makeNamedOverlay(
            QStringLiteral("stress-%1").arg(i), reserved, start, start + 0.01));
    }
    QVector<ClipInfo> stressClips;
    stressClips.reserve(kStressClipCount);
    for (int i = 0; i < kStressClipCount; ++i)
        stressClips.append(makeTimelineClip(QStringLiteral("stress-clip-%1").arg(i), 2.0));
    Timeline stressTimeline;
    stressTimeline.videoTracks().first()->setClips(stressClips);
    QString stressError;
    const bool stressApplied = stressTimeline.applySingleWordCaptionOverlays(
        stressCaptions, &stressError);
    const bool stressSingleCopy = stressApplied
        && stressTimeline.generatedCaptionOverlays().size() == kStressCaptionCount
        && stressTimeline.timelineTextOverlays().size() == kStressCaptionCount
        && stressTimeline.generatedCaptionOverlays().first().text == QStringLiteral("stress-0")
        && stressTimeline.generatedCaptionOverlays().last().text
            == QStringLiteral("stress-%1").arg(kStressCaptionCount - 1)
        && clipsExcludeGeneratedCaptions(
            stressTimeline.videoTracks().first()->clips(), reserved);

    Timeline operationTimeline;
    operationTimeline.videoTracks().first()->setClips({
        makeTimelineClip(QStringLiteral("operation-video"), 4.0),
    });
    operationTimeline.audioTracks().first()->setClips({
        makeTimelineClip(QStringLiteral("operation-audio"), 4.0),
    });
    QString operationError;
    const bool operationApplied = operationTimeline.applySingleWordCaptionOverlays(
        timed, &operationError);
    operationTimeline.videoTracks().first()->setSelectedClip(0);
    operationTimeline.setPlayheadPosition(2.0);
    operationTimeline.splitAtPlayhead();
    const bool splitPreserved = operationTimeline.videoTracks().first()->clipCount() == 2
        && generatedRoundTripMatches(
            operationTimeline.generatedCaptionOverlays(), timed, reserved)
        && clipsExcludeGeneratedCaptions(
            operationTimeline.videoTracks().first()->clips(), reserved);
    operationTimeline.videoTracks().first()->setSelectedClip(0);
    operationTimeline.copySelectedClip();
    operationTimeline.pasteClip();
    const bool pastePreserved = operationTimeline.videoTracks().first()->clipCount() == 3
        && generatedRoundTripMatches(
            operationTimeline.generatedCaptionOverlays(), timed, reserved)
        && clipsExcludeGeneratedCaptions(
            operationTimeline.videoTracks().first()->clips(), reserved);
    // pasteClip selects both V/A in sequence; make the pasted video clip the
    // explicit delete target just as the UI does on the next clip click.
    operationTimeline.videoTracks().first()->setSelectedClip(1);
    operationTimeline.deleteSelectedClip();
    const bool deletePreserved = operationTimeline.videoTracks().first()->clipCount() == 2
        && generatedRoundTripMatches(
            operationTimeline.generatedCaptionOverlays(), timed, reserved)
        && clipsExcludeGeneratedCaptions(
            operationTimeline.videoTracks().first()->clips(), reserved);

    check(11, "central captions scale, drag, and survive split/copy/paste/delete without duplication",
          scaleApplied
              && scaleApplyError.isEmpty()
              && textStrip
              && textStrip->height() <= 54
              && textStrip->accessibleName() == QStringLiteral("Timeline text overlays")
              && scaleDragApplied
              && stressSingleCopy
              && stressError.isEmpty()
              && operationApplied
              && operationError.isEmpty()
              && splitPreserved
              && pastePreserved
              && deletePreserved,
          QStringLiteral("scale=%1 drag=%2 strip=%3 stress=%4 operation=%5 split=%6 paste=%7 delete=%8 "
                         "scaleError='%9' stressError='%10' operationError='%11'")
              .arg(scaleApplied)
              .arg(scaleDragApplied)
              .arg(textStrip != nullptr)
              .arg(stressSingleCopy)
              .arg(operationApplied)
              .arg(splitPreserved)
              .arg(pastePreserved)
              .arg(deletePreserved)
              .arg(scaleApplyError, stressError, operationError));

    const int beforeRejectedIndex = timeline.undoManager()->currentIndex();
    const QVector<EnhancedTextOverlay> beforeRejectedCaptions =
        timeline.generatedCaptionOverlays();
    QString rejectedError;
    const bool rejectedEmpty = !timeline.applySingleWordCaptionOverlays({}, &rejectedError);
    const QVector<ClipInfo> afterRejectedClips = timeline.videoTracks().first()->clips();
    Timeline noVideoTimeline;
    const int noVideoUndoIndex = noVideoTimeline.undoManager()->currentIndex();
    QString noVideoError;
    const bool rejectedNoVideo =
        !noVideoTimeline.applySingleWordCaptionOverlays(timed, &noVideoError);
    check(12, "empty captions and missing video target report errors without mutation",
          rejectedEmpty
              && !rejectedError.isEmpty()
              && timeline.undoManager()->currentIndex() == beforeRejectedIndex
              && generatedRoundTripMatches(
                  timeline.generatedCaptionOverlays(), beforeRejectedCaptions, reserved)
              && afterRejectedClips.size() == 2
              && clipsExcludeGeneratedCaptions(afterRejectedClips, reserved)
              && rejectedNoVideo
              && !noVideoError.isEmpty()
              && noVideoTimeline.generatedCaptionOverlays().isEmpty()
              && noVideoTimeline.undoManager()->currentIndex() == noVideoUndoIndex);

    ProjectData savedProject;
    savedProject.config.width = 160;
    savedProject.config.height = 90;
    savedProject.config.fps = 10;
    savedProject.videoTracks = timeline.allVideoTracks();
    savedProject.audioTracks = timeline.allAudioTracks();
    savedProject.generatedCaptionOverlays = timeline.generatedCaptionOverlays();
    const QString serializedProject = ProjectFile::toJsonString(savedProject);
    const QJsonObject serializedRoot = QJsonDocument::fromJson(
        serializedProject.toUtf8()).object();
    ProjectData loadedProject;
    const bool projectLoaded = ProjectFile::fromJsonString(
        serializedProject, loadedProject);
    QJsonArray legacyOverlayJson = TextManager::toJson({timed.first()});
    QJsonObject legacyOverlayObject = legacyOverlayJson.first().toObject();
    legacyOverlayObject.remove(QStringLiteral("fontItalic"));
    legacyOverlayJson[0] = legacyOverlayObject;
    const QVector<EnhancedTextOverlay> legacyOverlays =
        TextManager::fromJson(legacyOverlayJson);
    const bool legacyItalicDefaultOk = legacyOverlays.size() == 1
        && !legacyOverlays.first().font.italic();
    const bool persistenceOk = projectLoaded
        && serializedRoot.value(QStringLiteral("generatedCaptionOverlays")).toArray().size()
            == timed.size()
        && loadedProject.videoTracks.size() >= 1
        && loadedProject.videoTracks.first().size() == 2
        && containsOverlayText(loadedProject.videoTracks.first().at(0),
                               QStringLiteral("ordinary-first"), ordinaryTag)
        && containsOverlayText(loadedProject.videoTracks.first().at(1),
                               QStringLiteral("ordinary-second"), ordinaryTag)
        && containsOverlayText(loadedProject.videoTracks.first().at(1),
                               QStringLiteral("ordinary-second-extra"), ordinaryTag)
        && clipsExcludeGeneratedCaptions(loadedProject.videoTracks.first(), reserved)
        && generatedRoundTripMatches(
            loadedProject.generatedCaptionOverlays, timed, reserved)
        && legacyItalicDefaultOk;

    EnhancedTextOverlay sequenceAOverlay = timed.first();
    sequenceAOverlay.text = QStringLiteral("sequence-A-caption");
    sequenceAOverlay.startTime = 0.1;
    sequenceAOverlay.endTime = 0.4;
    EnhancedTextOverlay sequenceBOverlay = timed.first();
    sequenceBOverlay.text = QStringLiteral("sequence-B-caption");
    sequenceBOverlay.startTime = 0.6;
    sequenceBOverlay.endTime = 0.9;
    TimelineSequence sequenceA;
    sequenceA.id = QStringLiteral("caption-sequence-a");
    sequenceA.name = QStringLiteral("Caption Sequence A");
    sequenceA.videoTracks = {{makeTimelineClip(QStringLiteral("sequence-a-video"), 1.0)}};
    sequenceA.audioTracks = {{}};
    sequenceA.generatedCaptionOverlays = {sequenceAOverlay};
    TimelineSequence sequenceB;
    sequenceB.id = QStringLiteral("caption-sequence-b");
    sequenceB.name = QStringLiteral("Caption Sequence B");
    sequenceB.videoTracks = {{makeTimelineClip(QStringLiteral("sequence-b-video"), 1.0)}};
    sequenceB.audioTracks = {{}};
    sequenceB.generatedCaptionOverlays = {sequenceBOverlay};
    Timeline sequenceTimeline;
    sequenceTimeline.setSequences({sequenceA, sequenceB}, sequenceA.id);
    const bool sequenceLoadedA = sequenceTimeline.activeSequenceId() == sequenceA.id
        && sequenceTimeline.generatedCaptionOverlays().size() == 1
        && sequenceTimeline.generatedCaptionOverlays().first().text == sequenceAOverlay.text;
    const bool sequenceSwitchedB = sequenceTimeline.setActiveSequence(sequenceB.id)
        && sequenceTimeline.generatedCaptionOverlays().size() == 1
        && sequenceTimeline.generatedCaptionOverlays().first().text == sequenceBOverlay.text;
    const bool sequenceReturnedA = sequenceTimeline.setActiveSequence(sequenceA.id)
        && sequenceTimeline.generatedCaptionOverlays().size() == 1
        && sequenceTimeline.generatedCaptionOverlays().first().text == sequenceAOverlay.text;
    bool sequenceSnapshotsDistinct = false;
    const QVector<TimelineSequence> sequenceSnapshots = sequenceTimeline.sequences();
    for (const TimelineSequence &snapshot : sequenceSnapshots) {
        if (snapshot.id == sequenceB.id) {
            sequenceSnapshotsDistinct = snapshot.generatedCaptionOverlays.size() == 1
                && snapshot.generatedCaptionOverlays.first().text == sequenceBOverlay.text;
        }
    }
    // Project save/load carries the sequence store through clipParentEntries;
    // restore the active sequence's top-level tracks/captions in the same
    // order as MainWindow::loadProject, then prove both stored caption sets.
    Timeline restoredSequenceTimeline;
    restoredSequenceTimeline.setClipParentEntries(sequenceTimeline.clipParentEntries());
    restoredSequenceTimeline.restoreGeneratedCaptionOverlays(
        sequenceTimeline.generatedCaptionOverlays());
    restoredSequenceTimeline.restoreFromProject(
        sequenceTimeline.allVideoTracks(), sequenceTimeline.allAudioTracks(),
        0.0, -1.0, -1.0, 10);
    const bool persistedSequenceA =
        restoredSequenceTimeline.activeSequenceId() == sequenceA.id
        && restoredSequenceTimeline.generatedCaptionOverlays().size() == 1
        && restoredSequenceTimeline.generatedCaptionOverlays().first().text
            == sequenceAOverlay.text;
    const bool persistedSequenceB =
        restoredSequenceTimeline.setActiveSequence(sequenceB.id)
        && restoredSequenceTimeline.generatedCaptionOverlays().size() == 1
        && restoredSequenceTimeline.generatedCaptionOverlays().first().text
            == sequenceBOverlay.text;
    const bool persistedSequenceReturnA =
        restoredSequenceTimeline.setActiveSequence(sequenceA.id)
        && restoredSequenceTimeline.generatedCaptionOverlays().size() == 1
        && restoredSequenceTimeline.generatedCaptionOverlays().first().text
            == sequenceAOverlay.text;

    const QString mappedSourcePath = QStringLiteral("/caption-selftest/shared-source.mp4");
    ClipInfo fastClip = makeTimelineClip(
        QStringLiteral("mapped-fast"), 20.0, mappedSourcePath);
    fastClip.inPoint = 2.0;
    fastClip.outPoint = 6.0;
    fastClip.speed = 2.0;
    fastClip.leadInSec = 1.0;
    ClipInfo remappedClip = makeTimelineClip(
        QStringLiteral("mapped-remap"), 20.0, mappedSourcePath);
    remappedClip.inPoint = 10.0;
    remappedClip.outPoint = 14.0;
    remappedClip.leadInSec = 0.5;
    remappedClip.timeRemapCurve.addKey(0.0, 0.0);
    remappedClip.timeRemapCurve.addKey(4.0, 2.0);
    Timeline mappingTimeline;
    mappingTimeline.videoTracks().first()->setClips({fastClip, remappedClip});
    caption::Track sourceCaptionTrack;
    sourceCaptionTrack.addClip(makeClip(
        3000, 5000, QStringLiteral("fast"),
        {makeWord(3000, 5000, QStringLiteral("fast"))}));
    // The segment crosses the trim in-point while its retained word begins
    // exactly on it. The mapped segment must be clipped, not discarded.
    sourceCaptionTrack.addClip(makeClip(
        9500, 10500, QStringLiteral("edge"),
        {makeWord(10000, 10500, QStringLiteral("edge"))}));
    // This segment also intersects the clip, but all real timed words are
    // outside the retained source interval. It must not regenerate stale text.
    sourceCaptionTrack.addClip(makeClip(
        9500, 10500, QStringLiteral("drop"),
        {makeWord(9500, 9900, QStringLiteral("drop"))}));
    sourceCaptionTrack.addClip(makeClip(
        10500, 11500, QStringLiteral("remap"),
        {makeWord(10500, 11500, QStringLiteral("remap"))}));
    caption::Track mappedCaptionTrack;
    QString mappingError;
    const bool mappingSucceeded = mappingTimeline.mapSourceCaptionTrackToTimeline(
        sourceCaptionTrack, mappedSourcePath, &mappedCaptionTrack, &mappingError);
    const bool mappingOk = mappingSucceeded
        && mappingError.isEmpty()
        && mappedCaptionTrack.clipCount() == 3
        && mappedCaptionTrack.clipAt(0).text == QStringLiteral("fast")
        && mappedCaptionTrack.clipAt(0).startMs == 1500
        && mappedCaptionTrack.clipAt(0).endMs == 2500
        && mappedCaptionTrack.clipAt(0).words.size() == 1
        && mappedCaptionTrack.clipAt(0).words.first().startMs == 1500
        && mappedCaptionTrack.clipAt(0).words.first().endMs == 2500
        && mappedCaptionTrack.clipAt(1).text == QStringLiteral("edge")
        && mappedCaptionTrack.clipAt(1).startMs == 3500
        && mappedCaptionTrack.clipAt(1).endMs == 4500
        && mappedCaptionTrack.clipAt(1).words.size() == 1
        && mappedCaptionTrack.clipAt(1).words.first().startMs == 3500
        && mappedCaptionTrack.clipAt(1).words.first().endMs == 4500
        && mappedCaptionTrack.clipAt(2).text == QStringLiteral("remap")
        && mappedCaptionTrack.clipAt(2).startMs == 4500
        && mappedCaptionTrack.clipAt(2).endMs == 6500
        && mappedCaptionTrack.clipAt(2).words.size() == 1
        && mappedCaptionTrack.clipAt(2).words.first().startMs == 4500
        && mappedCaptionTrack.clipAt(2).words.first().endMs == 6500;

    check(13, "top-level persistence, sequence isolation, and source-to-timeline mapping round-trip",
          persistenceOk
              && sequenceLoadedA
              && sequenceSwitchedB
              && sequenceReturnedA
              && sequenceSnapshotsDistinct
              && persistedSequenceA
              && persistedSequenceB
              && persistedSequenceReturnA
              && mappingOk,
          mappingError);

    caption::Track translatable;
    translatable.addClip(makeClip(
        0, 1000, QStringLiteral("translate me"),
        {makeWord(0, 500, QStringLiteral("translate")),
         makeWord(500, 1000, QStringLiteral("me"))}));
    subxlat::TranslatorClient translator;
    caption::Track translated;
    bool translationFinished = false;
    QObject::connect(&translator, &subxlat::TranslatorClient::translateFinished,
                     [&translated, &translationFinished](const caption::Track &result) {
        translated = result;
        translationFinished = true;
    });
    subxlat::TranslateConfig translateConfig;
    translateConfig.provider = subxlat::Provider::Stub;
    translateConfig.targetLang = QStringLiteral("ja");
    translator.translateTrack(translatable, translateConfig);
    check(14, "translated text clears stale word timing",
          translationFinished
              && translated.clipCount() == 1
              && translated.clipAt(0).text != translatable.clipAt(0).text
              && translated.clipAt(0).words.isEmpty());

    CaptionEditorDialog editor;
    QCheckBox *singleWordControl =
        editor.findChild<QCheckBox *>(QStringLiteral("captionSingleWordModeCheckBox"));
    QPushButton *applyButton =
        editor.findChild<QPushButton *>(QStringLiteral("captionApplyToTimelineButton"));
    QTextEdit *clipTextEdit =
        editor.findChild<QTextEdit *>(QStringLiteral("captionClipTextEdit"));
    QSpinBox *clipStartSpin =
        editor.findChild<QSpinBox *>(QStringLiteral("captionClipStartMsSpinBox"));
    QTableWidget *clipTable = editor.findChild<QTableWidget *>();
    int applySignalCount = 0;
    QObject::connect(&editor, &CaptionEditorDialog::applyToTimelineRequested,
                     [&applySignalCount]() { ++applySignalCount; });
    if (applyButton)
        applyButton->click();
    const bool emptyUiRejected = applySignalCount == 0 && !editor.applyError().isEmpty();

    editor.setTrack(translatable);
    if (applyButton)
        applyButton->click();
    const bool validUiSignalled = applySignalCount == 1 && editor.applyError().isEmpty();
    editor.setSingleWordModeEnabled(false);
    const bool separateModeDisablesApply = applyButton && !applyButton->isEnabled();
    editor.setSingleWordModeEnabled(true);

    editor.setTrack(translatable);
    if (clipTable)
        clipTable->selectRow(0);
    QMetaObject::invokeMethod(&editor, "onClipRowChanged", Qt::DirectConnection,
                              Q_ARG(int, 0));
    QApplication::processEvents();
    if (clipTextEdit)
        clipTextEdit->setPlainText(QStringLiteral("edited body"));
    QApplication::processEvents();
    const bool textEditClearedWords = editor.track().clipCount() == 1
        && editor.track().clipAt(0).words.isEmpty();

    editor.setTrack(translatable);
    if (clipTable)
        clipTable->selectRow(0);
    QMetaObject::invokeMethod(&editor, "onClipRowChanged", Qt::DirectConnection,
                              Q_ARG(int, 0));
    QApplication::processEvents();
    if (clipStartSpin)
        clipStartSpin->setValue(1);
    QApplication::processEvents();
    const bool timeEditClearedWords = editor.track().clipCount() == 1
        && editor.track().clipAt(0).words.isEmpty();

    editor.setTrack(caption::Track{});
    QApplication::processEvents();
    const bool emptyTrackClearedEditors = clipTextEdit
        && clipStartSpin
        && clipTextEdit->toPlainText().isEmpty()
        && clipStartSpin->value() == 0
        && !clipTextEdit->isEnabled()
        && !clipStartSpin->isEnabled();

    check(15, "Caption Editor exposes accessible separate mode/apply controls and safe errors",
          singleWordControl
              && applyButton
              && clipTextEdit
              && clipStartSpin
              && clipTable
              && clipTable->editTriggers() == QAbstractItemView::NoEditTriggers
              && !singleWordControl->accessibleName().isEmpty()
              && !applyButton->accessibleName().isEmpty()
              && emptyUiRejected
              && validUiSignalled
              && separateModeDisablesApply
              && textEditClearedWords
              && timeEditClearedWords
              && emptyTrackClearedEditors);

    QTemporaryDir renderDir;
    QString mediaError;
    const QString mediaPath = renderDir.isValid()
        ? renderDir.filePath(QStringLiteral("caption_boundary.mp4"))
        : QString();
    const bool mediaReady = renderDir.isValid()
        && writeDarkSyntheticClip(mediaPath, &mediaError);
    bool rendererBoundaryOk = false;
    bool nestedRendererOk = false;
    if (mediaReady) {
        const QVector<ClipInfo> renderClips = {
            makeTimelineClip(QStringLiteral("render-first"), 1.0, mediaPath),
            makeTimelineClip(QStringLiteral("render-second"), 1.0, mediaPath),
        };
        Timeline baselineRenderTimeline;
        baselineRenderTimeline.videoTracks().first()->setClips(renderClips);
        Timeline captionRenderTimeline;
        captionRenderTimeline.videoTracks().first()->setClips(renderClips);

        caption::Track renderCaptionTrack;
        renderCaptionTrack.addClip(makeClip(200, 800, QStringLiteral("I")));
        renderCaptionTrack.addClip(makeClip(1200, 1800, QStringLiteral("I")));
        caption::Style renderStyle;
        renderStyle.fontFamily = QStringLiteral("Arial");
        renderStyle.fontSizePt = 28;
        renderStyle.bold = true;
        renderStyle.textColor = QColor(255, 0, 0);
        renderStyle.outlineColor = QColor(255, 0, 0);
        renderStyle.outlineThickness = 2.0;
        renderStyle.shadowColor = QColor(0, 255, 0, 255);
        renderStyle.shadowOffset = QPointF(-12.0, 0.0);
        renderStyle.background = true;
        renderStyle.backgroundColor = QColor(0, 0, 255, 255);
        renderStyle.anchor = caption::Anchor::MiddleRight;
        renderStyle.anchorOffsetNormalized = QPointF(0.0, 0.0);
        renderStyle.maxWidthNormalized = 0.5;
        const QVector<EnhancedTextOverlay> renderOverlays =
            CaptionOverlayBuilder::build(renderCaptionTrack, renderStyle);
        QString renderApplyError;
        const bool renderApplied = captionRenderTimeline.applySingleWordCaptionOverlays(
            renderOverlays, &renderApplyError);

        const QSize outputSize(160, 90);
        const QImage baselineFirstStart =
            tlrender::renderFrameAt(&baselineRenderTimeline, 200000, outputSize);
        const QImage captionFirstStart =
            tlrender::renderFrameAt(&captionRenderTimeline, 200000, outputSize);
        const QImage baselineFirstAnimated =
            tlrender::renderFrameAt(&baselineRenderTimeline, 350000, outputSize);
        const QImage captionFirstAnimated =
            tlrender::renderFrameAt(&captionRenderTimeline, 350000, outputSize);
        const QImage baselineFirstEnd =
            tlrender::renderFrameAt(&baselineRenderTimeline, 800000, outputSize);
        const QImage captionFirstEnd =
            tlrender::renderFrameAt(&captionRenderTimeline, 800000, outputSize);
        const QImage baselineSecondStart =
            tlrender::renderFrameAt(&baselineRenderTimeline, 1200000, outputSize);
        const QImage captionSecondStart =
            tlrender::renderFrameAt(&captionRenderTimeline, 1200000, outputSize);
        const QImage baselineSecondAnimated =
            tlrender::renderFrameAt(&baselineRenderTimeline, 1350000, outputSize);
        const QImage captionSecondAnimated =
            tlrender::renderFrameAt(&captionRenderTimeline, 1350000, outputSize);
        const QImage baselineSecondEnd =
            tlrender::renderFrameAt(&baselineRenderTimeline, 1800000, outputSize);
        const QImage captionSecondEnd =
            tlrender::renderFrameAt(&captionRenderTimeline, 1800000, outputSize);

        const QRect firstRedBounds = strongPrimaryBounds(captionFirstAnimated, 0);
        const QRect firstBlueBounds = strongPrimaryBounds(captionFirstAnimated, 2);
        const QRect secondRedBounds = strongPrimaryBounds(captionSecondAnimated, 0);
        const QRect secondBlueBounds = strongPrimaryBounds(captionSecondAnimated, 2);
        rendererBoundaryOk = renderApplied
            && renderApplyError.isEmpty()
            && !captionFirstStart.isNull()
            && !captionSecondStart.isNull()
            && strongRedPixelCount(captionFirstStart)
                <= strongRedPixelCount(baselineFirstStart) + 2
            && strongPrimaryPixelCount(captionFirstStart, 1)
                <= strongPrimaryPixelCount(baselineFirstStart, 1) + 2
            && strongPrimaryPixelCount(captionFirstStart, 2)
                <= strongPrimaryPixelCount(baselineFirstStart, 2) + 2
            && strongRedPixelCount(captionSecondStart)
                <= strongRedPixelCount(baselineSecondStart) + 2
            && strongPrimaryPixelCount(captionSecondStart, 1)
                <= strongPrimaryPixelCount(baselineSecondStart, 1) + 2
            && strongPrimaryPixelCount(captionSecondStart, 2)
                <= strongPrimaryPixelCount(baselineSecondStart, 2) + 2
            && strongRedPixelCount(captionFirstAnimated)
                > strongRedPixelCount(baselineFirstAnimated) + 3
            && strongRedPixelCount(captionSecondAnimated)
                > strongRedPixelCount(baselineSecondAnimated) + 3
            && strongPrimaryPixelCount(captionFirstAnimated, 1)
                > strongPrimaryPixelCount(baselineFirstAnimated, 1) + 3
            && strongPrimaryPixelCount(captionSecondAnimated, 1)
                > strongPrimaryPixelCount(baselineSecondAnimated, 1) + 3
            && firstBlueBounds.isValid()
            && secondBlueBounds.isValid()
            && firstBlueBounds.width() >= 75
            && secondBlueBounds.width() >= 75
            && firstBlueBounds.left() >= 78
            && secondBlueBounds.left() >= 78
            && firstRedBounds.isValid()
            && secondRedBounds.isValid()
            && firstRedBounds.right() >= 145
            && secondRedBounds.right() >= 145
            && strongRedPixelCount(captionFirstEnd)
                <= strongRedPixelCount(baselineFirstEnd) + 2
            && strongPrimaryPixelCount(captionFirstEnd, 1)
                <= strongPrimaryPixelCount(baselineFirstEnd, 1) + 2
            && strongPrimaryPixelCount(captionFirstEnd, 2)
                <= strongPrimaryPixelCount(baselineFirstEnd, 2) + 2
            && strongRedPixelCount(captionSecondEnd)
                <= strongRedPixelCount(baselineSecondEnd) + 2
            && strongPrimaryPixelCount(captionSecondEnd, 1)
                <= strongPrimaryPixelCount(baselineSecondEnd, 1) + 2
            && strongPrimaryPixelCount(captionSecondEnd, 2)
                <= strongPrimaryPixelCount(baselineSecondEnd, 2) + 2;

        TimelineSequence nestedBaselineSequence;
        nestedBaselineSequence.id = QStringLiteral("nested-caption-baseline");
        nestedBaselineSequence.name = QStringLiteral("Nested Caption Baseline");
        nestedBaselineSequence.videoTracks = {{
            makeTimelineClip(QStringLiteral("nested-baseline-media"), 1.0, mediaPath),
        }};
        nestedBaselineSequence.audioTracks = {{}};
        Timeline nestedBaselineTimeline;
        nestedBaselineTimeline.addSequence(nestedBaselineSequence);
        nestedBaselineTimeline.videoTracks().first()->setClips({
            nestedBaselineTimeline.makeSequenceClip(nestedBaselineSequence.id),
        });

        TimelineSequence nestedCaptionSequence = nestedBaselineSequence;
        nestedCaptionSequence.id = QStringLiteral("nested-caption-active");
        nestedCaptionSequence.name = QStringLiteral("Nested Caption Active");
        nestedCaptionSequence.generatedCaptionOverlays = {renderOverlays.first()};
        Timeline nestedCaptionTimeline;
        nestedCaptionTimeline.addSequence(nestedCaptionSequence);
        nestedCaptionTimeline.videoTracks().first()->setClips({
            nestedCaptionTimeline.makeSequenceClip(nestedCaptionSequence.id),
        });

        const QImage nestedBaselineAnimated = tlrender::renderFrameAt(
            &nestedBaselineTimeline, 350000, outputSize);
        const QImage nestedCaptionAnimated = tlrender::renderFrameAt(
            &nestedCaptionTimeline, 350000, outputSize);
        const QImage nestedBaselineEnd = tlrender::renderFrameAt(
            &nestedBaselineTimeline, 800000, outputSize);
        const QImage nestedCaptionEnd = tlrender::renderFrameAt(
            &nestedCaptionTimeline, 800000, outputSize);
        nestedRendererOk = !nestedCaptionAnimated.isNull()
            && strongRedPixelCount(nestedCaptionAnimated)
                > strongRedPixelCount(nestedBaselineAnimated) + 3
            && strongPrimaryPixelCount(nestedCaptionAnimated, 1)
                > strongPrimaryPixelCount(nestedBaselineAnimated, 1) + 3
            && strongPrimaryPixelCount(nestedCaptionAnimated, 2)
                > strongPrimaryPixelCount(nestedBaselineAnimated, 2) + 3
            && strongRedPixelCount(nestedCaptionEnd)
                <= strongRedPixelCount(nestedBaselineEnd) + 2
            && strongPrimaryPixelCount(nestedCaptionEnd, 1)
                <= strongPrimaryPixelCount(nestedBaselineEnd, 1) + 2
            && strongPrimaryPixelCount(nestedCaptionEnd, 2)
                <= strongPrimaryPixelCount(nestedBaselineEnd, 2) + 2;
    }
    check(16, "renderFrameAt honors caption style/bounds in root and nested sequences",
          rendererBoundaryOk && nestedRendererOk,
          mediaReady ? QString() : mediaError);

    QTemporaryDir remotionDir;
    ProjectData remotionData;
    remotionData.videoTracks = {{
        makeTimelineClip(QStringLiteral("remotion-first"), 1.0,
                         QStringLiteral("C:/caption-selftest/remotion-first.mp4")),
        makeTimelineClip(QStringLiteral("remotion-second"), 1.0,
                         QStringLiteral("C:/caption-selftest/remotion-second.mp4")),
    }};
    EnhancedTextOverlay remotionCaption = makeNamedOverlay(
        QStringLiteral("CENTRAL_CAPTION_CROSSES_CLIP_BOUNDARY"), reserved, 0.75, 1.25);
    remotionData.generatedCaptionOverlays = {remotionCaption};

    RemotionExportConfig remotionConfig;
    remotionConfig.outputDir = remotionDir.path();
    remotionConfig.projectName = QStringLiteral("central-caption-regression");
    remotionConfig.fps = 30;
    remotionConfig.includeAssets = false;
    remotionConfig.generatePackageJson = false;

    QString remotionError;
    RemotionExporter remotionExporter;
    QObject::connect(&remotionExporter, &RemotionExporter::exportError,
                     [&remotionError](const QString &message) {
                         remotionError = message;
                     });
    const bool remotionExported = remotionDir.isValid()
        && remotionExporter.exportProject(remotionConfig, remotionData);

    auto readGeneratedSource = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        QString source = QString::fromUtf8(file.readAll());
        source.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        return source;
    };
    const QString remotionBase = remotionDir.filePath(
        QStringLiteral("central-caption-regression/src"));
    const QString videoTsx = readGeneratedSource(remotionBase + QStringLiteral("/Video.tsx"));
    const QString timelineTs = readGeneratedSource(
        remotionBase + QStringLiteral("/lib/timeline.ts"));
    const QString captionSentinel = remotionCaption.text;
    const QString expectedRootVideoBlock = QStringLiteral(
        "      <Sequence from={23} durationInFrames={15}>\n"
        "        <TextOverlay\n"
        "          text={\"CENTRAL_CAPTION_CROSSES_CLIP_BOUNDARY\"}");
    const QString expectedRootTimelineBlock = QStringLiteral(
        "  textOverlays: [\n"
        "    { text: \"CENTRAL_CAPTION_CROSSES_CLIP_BOUNDARY\", "
        "startFrame: 23, durationInFrames: 15,");
    const bool remotionCaptionIsCentralAndUnique = remotionExported
        && !videoTsx.isEmpty()
        && !timelineTs.isEmpty()
        && videoTsx.count(captionSentinel) == 1
        && timelineTs.count(captionSentinel) == 1
        && videoTsx.count(QStringLiteral("Video Track 0 Clip ")) == 2
        && timelineTs.count(QStringLiteral("textOverlays: [")) == 1
        && videoTsx.contains(expectedRootVideoBlock)
        && timelineTs.contains(expectedRootTimelineBlock);
    check(17, "Remotion exports one central caption across multiple clips without duplication",
          remotionCaptionIsCentralAndUnique, remotionError);

    qInfo().noquote() << QStringLiteral("[caption-overlay-builder] summary: %1 PASS, %2 FAIL")
        .arg(passed).arg(failed);
    return failed == 0 ? 0 : failed;
}
