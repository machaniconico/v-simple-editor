#include "CaptionOverlayBuilder.h"

#include <QFont>
#include <QtGlobal>
#include <algorithm>

namespace {

QString trimUnicodeSpace(const QString &text)
{
    qsizetype first = 0;
    while (first < text.size() && text.at(first).isSpace())
        ++first;

    qsizetype last = text.size();
    while (last > first && text.at(last - 1).isSpace())
        --last;
    return text.mid(first, last - first);
}

QString normalizedSegmentForMatch(const QString &text)
{
    const QString normalized = text.normalized(QString::NormalizationForm_C);
    QString compact;
    compact.reserve(normalized.size());
    for (const QChar ch : normalized) {
        if (!ch.isSpace())
            compact.append(ch);
    }
    return compact;
}

QString normalizedWordForMatch(const QString &text)
{
    return trimUnicodeSpace(text).normalized(QString::NormalizationForm_C);
}

QStringList unicodeWhitespaceParts(const QString &text)
{
    QStringList parts;
    QString current;
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            if (!current.isEmpty()) {
                parts.append(current);
                current.clear();
            }
        } else {
            current.append(ch);
        }
    }
    if (!current.isEmpty())
        parts.append(current);
    return parts;
}

bool validWordTimingAndText(const caption::Clip &clip, QStringList *displayWords)
{
    if (clip.words.isEmpty())
        return false;

    QString concatenated;
    qint64 previousEndMs = clip.startMs;
    QStringList words;
    words.reserve(clip.words.size());

    for (const caption::Word &word : clip.words) {
        const QString displayText = trimUnicodeSpace(word.text);
        const QString normalizedText = normalizedWordForMatch(word.text);
        if (displayText.isEmpty()
            || normalizedText.isEmpty()
            || word.startMs < clip.startMs
            || word.endMs > clip.endMs
            || word.endMs <= word.startMs
            || word.startMs < previousEndMs) {
            return false;
        }
        words.append(displayText);
        concatenated.append(normalizedText);
        previousEndMs = word.endMs;
    }

    if (concatenated.compare(normalizedSegmentForMatch(clip.text), Qt::CaseSensitive) != 0)
        return false;

    if (displayWords)
        *displayWords = words;
    return true;
}

QPointF anchorBase(caption::Anchor anchor)
{
    switch (anchor) {
    case caption::Anchor::TopLeft:      return QPointF(0.0, 0.0);
    case caption::Anchor::TopCenter:    return QPointF(0.5, 0.0);
    case caption::Anchor::TopRight:     return QPointF(1.0, 0.0);
    case caption::Anchor::MiddleLeft:   return QPointF(0.0, 0.5);
    case caption::Anchor::MiddleCenter: return QPointF(0.5, 0.5);
    case caption::Anchor::MiddleRight:  return QPointF(1.0, 0.5);
    case caption::Anchor::BottomLeft:   return QPointF(0.0, 1.0);
    case caption::Anchor::BottomCenter: return QPointF(0.5, 1.0);
    case caption::Anchor::BottomRight:  return QPointF(1.0, 1.0);
    }
    return QPointF(0.5, 1.0);
}

int horizontalAlignment(caption::Anchor anchor)
{
    switch (anchor) {
    case caption::Anchor::TopLeft:
    case caption::Anchor::MiddleLeft:
    case caption::Anchor::BottomLeft:
        return Qt::AlignLeft | Qt::AlignVCenter;
    case caption::Anchor::TopRight:
    case caption::Anchor::MiddleRight:
    case caption::Anchor::BottomRight:
        return Qt::AlignRight | Qt::AlignVCenter;
    case caption::Anchor::TopCenter:
    case caption::Anchor::MiddleCenter:
    case caption::Anchor::BottomCenter:
        return Qt::AlignHCenter | Qt::AlignVCenter;
    }
    return Qt::AlignHCenter | Qt::AlignVCenter;
}

EnhancedTextOverlay makeOverlay(const QString &text, qint64 startMs, qint64 endMs,
                                const caption::Style &style)
{
    EnhancedTextOverlay overlay;
    overlay.text = text;

    QFont font(style.fontFamily);
    font.setPointSize(style.fontSizePt);
    font.setBold(style.bold);
    font.setItalic(style.italic);
    overlay.font = font;
    overlay.color = style.textColor;
    overlay.outlineColor = style.outlineColor;
    overlay.outlineWidth = qMax(0, qRound(style.outlineThickness));

    overlay.backgroundColor = style.backgroundColor;
    if (!style.background)
        overlay.backgroundColor.setAlpha(0);

    overlay.width = qBound(0.0, style.maxWidthNormalized, 1.0);
    const QPointF anchor = anchorBase(style.anchor);
    // EnhancedTextOverlay stores x/y as the centre of its layout box.  Keep
    // an explicit max-width box inside the canvas even for left/right caption
    // anchors; placing its centre at 0 or 1 would clip half of the box.  The
    // vertical box remains auto-height, so only the normalized centre can be
    // clamped here (the baker derives the actual pixel height from the font).
    const double halfWidth = overlay.width * 0.5;
    double centerX = anchor.x();
    if (anchor.x() <= 0.0)
        centerX = halfWidth;
    else if (anchor.x() >= 1.0)
        centerX = 1.0 - halfWidth;
    centerX += style.anchorOffsetNormalized.x();
    overlay.x = qBound(halfWidth, centerX, 1.0 - halfWidth);
    // x/y are layout-box centres, not anchor edges.  Keep a small vertical
    // centre margin so Top + the Bottom-oriented default offset (-0.08)
    // cannot place half of the first line outside the frame.  The baker also
    // clamps using the measured pixel box for fonts larger than this margin.
    constexpr double kVerticalCenterMargin = 0.05;
    overlay.y = qBound(kVerticalCenterMargin,
                       anchor.y() + style.anchorOffsetNormalized.y(),
                       1.0 - kVerticalCenterMargin);
    overlay.alignment = horizontalAlignment(style.anchor);
    overlay.wordWrap = true;

    overlay.shadow.enabled = style.shadowColor.alpha() > 0;
    overlay.shadow.color = style.shadowColor;
    overlay.shadow.offsetX = style.shadowOffset.x();
    overlay.shadow.offsetY = style.shadowOffset.y();
    overlay.shadow.opacity = 1.0;

    overlay.startTime = static_cast<double>(startMs) / 1000.0;
    overlay.endTime = static_cast<double>(endMs) / 1000.0;
    overlay.animIn.type = TextAnimationType::Pop;
    overlay.animIn.duration = qMin(0.15, qMax(0.0, (overlay.endTime - overlay.startTime) * 0.5));
    overlay.templateName = CaptionOverlayBuilder::generatedTemplateName();
    return overlay;
}

void appendFallbackOverlays(QVector<EnhancedTextOverlay> *result,
                            const caption::Clip &clip,
                            const caption::Style &style)
{
    const QStringList parts = unicodeWhitespaceParts(clip.text);
    const qsizetype count = parts.size();
    if (count <= 0)
        return;

    const qint64 durationMs = clip.endMs - clip.startMs;
    for (qsizetype i = 0; i < count; ++i) {
        const qint64 startMs = clip.startMs + (durationMs * i) / count;
        const qint64 endMs = (i + 1 == count)
            ? clip.endMs
            : clip.startMs + (durationMs * (i + 1)) / count;
        result->append(makeOverlay(parts.at(i), startMs, endMs, style));
    }
}

} // namespace

QString CaptionOverlayBuilder::generatedTemplateName()
{
    return QString::fromLatin1(kGeneratedTemplateName);
}

QVector<EnhancedTextOverlay> CaptionOverlayBuilder::build(const caption::Track &track,
                                                          const caption::Style &style)
{
    QVector<EnhancedTextOverlay> result;
    for (const caption::Clip &clip : track.clips()) {
        if (clip.endMs <= clip.startMs || unicodeWhitespaceParts(clip.text).isEmpty())
            continue;

        QStringList displayWords;
        if (validWordTimingAndText(clip, &displayWords)) {
            result.reserve(result.size() + clip.words.size());
            for (qsizetype i = 0; i < clip.words.size(); ++i) {
                const caption::Word &word = clip.words.at(i);
                result.append(makeOverlay(displayWords.at(i), word.startMs, word.endMs, style));
            }
            continue;
        }

        appendFallbackOverlays(&result, clip, style);
    }
    return result;
}

bool CaptionOverlayBuilder::isActiveAt(const EnhancedTextOverlay &overlay,
                                       double timeSeconds)
{
    return timeSeconds >= overlay.startTime
        && (overlay.endTime <= 0.0 || timeSeconds < overlay.endTime);
}

int CaptionOverlayBuilder::generatedSuffixBegin(
    const QVector<EnhancedTextOverlay> &overlays)
{
    const QString tag = generatedTemplateName();
    for (int i = 0; i < overlays.size(); ++i) {
        if (overlays.at(i).templateName == tag)
            return i;
    }
    return overlays.size();
}

int CaptionOverlayBuilder::activeGeneratedOverlayIndex(
    const QVector<EnhancedTextOverlay> &overlays, double timeSeconds)
{
    const int beginIndex = generatedSuffixBegin(overlays);
    if (beginIndex >= overlays.size())
        return -1;

    const auto begin = overlays.cbegin() + beginIndex;
    const auto after = std::upper_bound(
        begin, overlays.cend(), timeSeconds,
        [](double time, const EnhancedTextOverlay &overlay) {
            return time < overlay.startTime;
        });
    if (after == begin)
        return -1;

    const int candidate = static_cast<int>(std::distance(overlays.cbegin(), after) - 1);
    return isActiveAt(overlays.at(candidate), timeSeconds) ? candidate : -1;
}
