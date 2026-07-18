#include "SubtitleTrackRenderer.h"
#include "SubtitleKaraoke.h"

#include <QFontMetrics>
#include <QJsonArray>
#include <QTextLayout>
#include <QTextLine>

SubtitleTrackRenderer::SubtitleTrackRenderer(QObject *parent)
    : QObject(parent)
{
}

void SubtitleTrackRenderer::setSegments(const QVector<SubtitleSegment> &segments)
{
    m_segments = segments;
}

void SubtitleTrackRenderer::setStyle(const SubtitleStyle &style)
{
    m_style = style;
}

QString SubtitleTrackRenderer::textAt(double timeSec) const
{
    for (const auto &seg : m_segments) {
        if (timeSec >= seg.startTime && timeSec < seg.endTime) {
            return seg.text;
        }
    }
    return QString();
}

const SubtitleSegment* SubtitleTrackRenderer::activeSegmentAt(double timeSec) const
{
    for (const auto &seg : m_segments) {
        if (timeSec >= seg.startTime && timeSec < seg.endTime) {
            return &seg;
        }
    }
    return nullptr;
}

QStringList SubtitleTrackRenderer::wrapText(const QString &text, const QFont &font, int maxWidth)
{
    QStringList lines;
    if (text.isEmpty() || maxWidth <= 0) {
        if (!text.isEmpty())
            lines.append(text);
        return lines;
    }

    QFontMetrics fm(font);
    const QChar space(' ');
    const QChar newline('\n');

    // Split on explicit newlines first
    const auto paragraphs = text.split(newline, Qt::SkipEmptyParts);
    for (const QString &paragraph : paragraphs) {
        const auto words = paragraph.split(space, Qt::SkipEmptyParts);
        if (words.isEmpty()) {
            lines.append(QString());
            continue;
        }

        QString currentLine = words.first();
        for (int i = 1; i < words.size(); ++i) {
            const QString next = currentLine + space + words[i];
            if (fm.horizontalAdvance(next) > maxWidth) {
                lines.append(currentLine);
                currentLine = words[i];
            } else {
                currentLine = next;
            }
        }
        if (!currentLine.isEmpty())
            lines.append(currentLine);
    }

    if (lines.isEmpty())
        lines.append(text);

    return lines;
}

void SubtitleTrackRenderer::paintOnto(QPainter &painter, const QRectF &canvasRect, double timeSec) const
{
    const QString activeText = textAt(timeSec);
    if (m_style.karaokeEnabled) {
        const SubtitleSegment *seg = activeSegmentAt(timeSec);
        if (seg != nullptr && !seg->words.isEmpty()) {
            QStringList wordTexts;
            QVector<int> wordIndexes;
            wordTexts.reserve(seg->words.size());
            wordIndexes.reserve(seg->words.size());
            for (int i = 0; i < seg->words.size(); ++i) {
                const QString wordText = seg->words[i].text.trimmed();
                if (wordText.isEmpty())
                    continue;
                wordTexts.append(wordText);
                wordIndexes.append(i);
            }

            const QString karaokeText = wordTexts.join(QLatin1Char(' '));
            const int maxWidth = static_cast<int>(canvasRect.width() * m_style.maxWidthFraction);
            const QStringList wrappedLines = wrapText(karaokeText, m_style.font, maxWidth);
            if (!wrappedLines.isEmpty()) {
                QFontMetrics fm(m_style.font);
                const int lineHeight = fm.height();
                const int totalHeight = wrappedLines.size() * lineHeight;

                // Position: verticalPos from top, centered horizontally
                const double centerY = canvasRect.top() + canvasRect.height() * m_style.verticalPos;
                const double startY = centerY - static_cast<double>(totalHeight) / 2.0;
                const double centerX = canvasRect.center().x();

                // Draw optional background box
                if (m_style.boxEnabled) {
                    int maxLineWidth = 0;
                    for (const QString &line : wrappedLines) {
                        maxLineWidth = qMax(maxLineWidth, fm.horizontalAdvance(line));
                    }
                    const int padding = 8;
                    QRectF boxRect(
                        centerX - maxLineWidth / 2 - padding,
                        startY - padding,
                        maxLineWidth + padding * 2,
                        totalHeight + padding * 2
                    );

                    QColor boxColor = m_style.boxColor;
                    boxColor.setAlphaF(m_style.boxOpacity);
                    painter.fillRect(boxRect, boxColor);
                }

                // Draw text with outline
                painter.setFont(m_style.font);
                painter.setPen(m_style.color);

                const int activeWord = subtitlekaraoke::activeWordIndex(seg->words, timeSec);
                const int outlineW = static_cast<int>(m_style.outlineWidth);
                const int spaceWidth = fm.horizontalAdvance(QStringLiteral(" "));

                auto drawWords = [&](bool outlinePass) {
                    int wordCursor = 0;
                    for (int i = 0; i < wrappedLines.size(); ++i) {
                        const QString &line = wrappedLines[i];
                        const QStringList lineWords = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                        const int lineWidth = fm.horizontalAdvance(line);
                        double x = centerX - lineWidth / 2.0;
                        const double y = startY + i * lineHeight + fm.ascent();

                        for (int j = 0; j < lineWords.size() && wordCursor < wordTexts.size(); ++j, ++wordCursor) {
                            const QString &word = wordTexts[wordCursor];
                            const int globalWordIndex = wordIndexes[wordCursor];

                            if (outlinePass) {
                                if (outlineW <= 0)
                                    continue;

                                QPen outlinePen(m_style.outlineColor);
                                outlinePen.setWidth(outlineW);
                                outlinePen.setJoinStyle(Qt::RoundJoin);
                                painter.setPen(outlinePen);
                            } else {
                                const QColor fillColor = (globalWordIndex == activeWord)
                                    ? m_style.karaokeHighlightColor
                                    : m_style.color;
                                painter.setPen(fillColor);
                            }

                            painter.drawText(QPointF(x, y), word);

                            x += fm.horizontalAdvance(word);
                            if (j + 1 < lineWords.size())
                                x += spaceWidth;
                        }
                    }
                };

                if (outlineW > 0) {
                    drawWords(true);
                }
                drawWords(false);
                return;
            }
        }
    }
    if (activeText.isEmpty())
        return;

    const int maxWidth = static_cast<int>(canvasRect.width() * m_style.maxWidthFraction);
    const QStringList wrappedLines = wrapText(activeText, m_style.font, maxWidth);
    if (wrappedLines.isEmpty())
        return;

    QFontMetrics fm(m_style.font);
    const int lineHeight = fm.height();
    const int totalHeight = wrappedLines.size() * lineHeight;

    // Position: verticalPos from top, centered horizontally
    const double centerY = canvasRect.top() + canvasRect.height() * m_style.verticalPos;
    const double startY = centerY - static_cast<double>(totalHeight) / 2.0;
    const double centerX = canvasRect.center().x();

    // Draw optional background box
    if (m_style.boxEnabled) {
        int maxLineWidth = 0;
        for (const QString &line : wrappedLines) {
            maxLineWidth = qMax(maxLineWidth, fm.horizontalAdvance(line));
        }
        const int padding = 8;
        QRectF boxRect(
            centerX - maxLineWidth / 2 - padding,
            startY - padding,
            maxLineWidth + padding * 2,
            totalHeight + padding * 2
        );

        QColor boxColor = m_style.boxColor;
        boxColor.setAlphaF(m_style.boxOpacity);
        painter.fillRect(boxRect, boxColor);
    }

    // Draw text with outline
    painter.setFont(m_style.font);
    painter.setPen(m_style.color);

    const int outlineW = static_cast<int>(m_style.outlineWidth);
    if (outlineW > 0) {
        QPen outlinePen(m_style.outlineColor);
        outlinePen.setWidth(outlineW);
        outlinePen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(outlinePen);

        for (int i = 0; i < wrappedLines.size(); ++i) {
            const QString &line = wrappedLines[i];
            const int lineWidth = fm.horizontalAdvance(line);
            const double x = centerX - lineWidth / 2.0;
            const double y = startY + i * lineHeight + fm.ascent();
            painter.drawText(QPointF(x, y), line);
        }

        painter.setPen(m_style.color);
    }

    for (int i = 0; i < wrappedLines.size(); ++i) {
        const QString &line = wrappedLines[i];
        const int lineWidth = fm.horizontalAdvance(line);
        const double x = centerX - lineWidth / 2.0;
        const double y = startY + i * lineHeight + fm.ascent();
        painter.drawText(QPointF(x, y), line);
    }
}

QVector<EnhancedTextOverlay> SubtitleTrackRenderer::toOverlays() const
{
    // Delegate to SubtitleGenerator::toTextOverlays then apply style fields
    QVector<EnhancedTextOverlay> overlays = SubtitleGenerator::toTextOverlays(m_segments);

    for (auto &overlay : overlays) {
        overlay.font = m_style.font;
        overlay.color = m_style.color;
        overlay.outlineColor = m_style.outlineColor;
        overlay.outlineWidth = static_cast<int>(m_style.outlineWidth);
        overlay.y = m_style.verticalPos;
        overlay.alignment = m_style.alignment;
        overlay.wordWrap = true;

        if (m_style.boxEnabled) {
            QColor bgColor = m_style.boxColor;
            bgColor.setAlphaF(m_style.boxOpacity);
            overlay.backgroundColor = bgColor;
        }
    }

    return overlays;
}

QJsonObject SubtitleTrackRenderer::toJson() const
{
    QJsonObject obj;

    // Serialize segments
    QJsonArray segArray;
    for (const auto &seg : m_segments) {
        QJsonObject segObj;
        segObj["startTime"] = seg.startTime;
        segObj["endTime"] = seg.endTime;
        segObj["text"] = seg.text;
        segObj["language"] = seg.language;
        segObj["confidence"] = seg.confidence;
        if (!seg.words.isEmpty()) {
            QJsonArray wordArray;
            for (const SubtitleWord &word : seg.words) {
                QJsonObject wordObj;
                wordObj["word"] = word.text;
                wordObj["start"] = word.startTime;
                wordObj["end"] = word.endTime;
                wordArray.append(wordObj);
            }
            segObj["words"] = wordArray;
        }
        segArray.append(segObj);
    }
    obj["segments"] = segArray;

    // Serialize style
    QJsonObject styleObj;
    styleObj["fontFamily"] = m_style.font.family();
    styleObj["fontSize"] = m_style.font.pointSize();
    styleObj["fontBold"] = m_style.font.bold();
    styleObj["fontItalic"] = m_style.font.italic();
    styleObj["color"] = m_style.color.name(QColor::HexArgb);
    styleObj["outlineColor"] = m_style.outlineColor.name(QColor::HexArgb);
    styleObj["outlineWidth"] = m_style.outlineWidth;
    styleObj["boxEnabled"] = m_style.boxEnabled;
    styleObj["boxColor"] = m_style.boxColor.name(QColor::HexArgb);
    styleObj["boxOpacity"] = m_style.boxOpacity;
    styleObj["verticalPos"] = m_style.verticalPos;
    styleObj["maxWidthFraction"] = m_style.maxWidthFraction;
    styleObj["alignment"] = m_style.alignment;
    if (m_style.karaokeEnabled) {
        styleObj["karaokeEnabled"] = true;
        styleObj["karaokeHighlightColor"] = m_style.karaokeHighlightColor.name(QColor::HexArgb);
    }
    obj["style"] = styleObj;

    return obj;
}

void SubtitleTrackRenderer::fromJson(const QJsonObject &json)
{
    m_style.karaokeEnabled = false;
    m_style.karaokeHighlightColor = QColor(255, 210, 0);

    // Deserialize segments
    m_segments.clear();
    if (json.contains("segments")) {
        QJsonArray segArray = json["segments"].toArray();
        for (const QJsonValue &val : segArray) {
            QJsonObject segObj = val.toObject();
            SubtitleSegment seg;
            seg.startTime = segObj["startTime"].toDouble();
            seg.endTime = segObj["endTime"].toDouble();
            seg.text = segObj["text"].toString();
            seg.language = segObj["language"].toString();
            seg.confidence = segObj["confidence"].toDouble();
            if (segObj.contains("words") && segObj["words"].isArray()) {
                QJsonArray wordArray = segObj["words"].toArray();
                for (const QJsonValue &wordVal : wordArray) {
                    QJsonObject wordObj = wordVal.toObject();
                    const bool hasText = wordObj.contains("word") || wordObj.contains("text");
                    const bool hasStart = wordObj.contains("start") || wordObj.contains("t0");
                    const bool hasEnd = wordObj.contains("end") || wordObj.contains("t1");
                    if (!hasText || !hasStart || !hasEnd)
                        continue;

                    SubtitleWord word;
                    word.text = wordObj.contains("word")
                        ? wordObj["word"].toString()
                        : wordObj["text"].toString();
                    word.startTime = wordObj.contains("start")
                        ? wordObj["start"].toDouble()
                        : wordObj["t0"].toDouble();
                    word.endTime = wordObj.contains("end")
                        ? wordObj["end"].toDouble()
                        : wordObj["t1"].toDouble();
                    seg.words.append(word);
                }
            }
            m_segments.append(seg);
        }
    }

    // Deserialize style
    if (json.contains("style")) {
        QJsonObject styleObj = json["style"].toObject();

        QFont font;
        font.setFamily(styleObj["fontFamily"].toString("Arial"));
        font.setPointSizeF(styleObj["fontSize"].toDouble(28.0));
        font.setBold(styleObj["fontBold"].toBool(false));
        font.setItalic(styleObj["fontItalic"].toBool(false));
        m_style.font = font;

        m_style.color = QColor(styleObj["color"].toString("#FFFFFFFF"));
        m_style.outlineColor = QColor(styleObj["outlineColor"].toString("#FF000000"));
        m_style.outlineWidth = styleObj["outlineWidth"].toDouble(2.0);
        m_style.boxEnabled = styleObj["boxEnabled"].toBool(false);
        m_style.boxColor = QColor(styleObj["boxColor"].toString("#FF000000"));
        m_style.boxOpacity = styleObj["boxOpacity"].toDouble(0.6);
        m_style.verticalPos = styleObj["verticalPos"].toDouble(0.85);
        m_style.maxWidthFraction = styleObj["maxWidthFraction"].toDouble(0.8);
        m_style.alignment = styleObj["alignment"].toInt(Qt::AlignHCenter);
        m_style.karaokeEnabled = styleObj.contains("karaokeEnabled")
            ? styleObj["karaokeEnabled"].toBool(false)
            : false;
        m_style.karaokeHighlightColor = styleObj.contains("karaokeHighlightColor")
            ? QColor(styleObj["karaokeHighlightColor"].toString("#FFFFD200"))
            : QColor(255, 210, 0);
    }
}
