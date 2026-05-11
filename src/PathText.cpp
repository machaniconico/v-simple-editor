#include "PathText.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QRegularExpression>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <QStringList>

namespace {

constexpr double kLengthEpsilon = 1.0e-6;
constexpr double kMarginEpsilon = 1.0e-6;

double clampUnit(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

double clampNonNegative(double value)
{
    return std::max(value, 0.0);
}

QString svgNumber(double value)
{
    return QString::number(value, 'g', 16);
}

QString pathToSvgString(const QPainterPath &path)
{
    if (path.isEmpty()) {
        return QString();
    }

    QStringList commands;
    commands.reserve(path.elementCount());

    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        switch (element.type) {
        case QPainterPath::MoveToElement:
            commands.append(QStringLiteral("M %1 %2")
                                .arg(svgNumber(element.x))
                                .arg(svgNumber(element.y)));
            break;
        case QPainterPath::LineToElement:
            commands.append(QStringLiteral("L %1 %2")
                                .arg(svgNumber(element.x))
                                .arg(svgNumber(element.y)));
            break;
        case QPainterPath::CurveToElement:
            if (i + 2 >= path.elementCount()) {
                break;
            }

            {
                const QPainterPath::Element c1 = path.elementAt(i);
                const QPainterPath::Element c2 = path.elementAt(i + 1);
                const QPainterPath::Element end = path.elementAt(i + 2);
                commands.append(QStringLiteral("C %1 %2 %3 %4 %5 %6")
                                    .arg(svgNumber(c1.x))
                                    .arg(svgNumber(c1.y))
                                    .arg(svgNumber(c2.x))
                                    .arg(svgNumber(c2.y))
                                    .arg(svgNumber(end.x))
                                    .arg(svgNumber(end.y)));
            }
            i += 2;
            break;
        case QPainterPath::CurveToDataElement:
            break;
        }
    }

    return commands.join(QLatin1Char(' '));
}

QPainterPath svgStringToPath(const QString &svg)
{
    QPainterPath path;
    if (svg.trimmed().isEmpty()) {
        return path;
    }

    static const QRegularExpression tokenRegex(
        QStringLiteral(R"([A-Za-z]|[-+]?(?:\d+\.\d+|\d+\.?|\.\d+)(?:[eE][-+]?\d+)?)"));

    QRegularExpressionMatchIterator matches = tokenRegex.globalMatch(svg);
    QStringList tokens;
    while (matches.hasNext()) {
        tokens.append(matches.next().captured(0));
    }

    auto isCommandToken = [](const QString &token) {
        return token.size() == 1 && token.at(0).isLetter();
    };

    auto readNumber = [&tokens](int index) {
        return tokens.at(index).toDouble();
    };

    int index = 0;
    QChar command;
    while (index < tokens.size()) {
        if (isCommandToken(tokens.at(index))) {
            command = tokens.at(index).at(0).toUpper();
            ++index;
        }

        if (command.isNull()) {
            break;
        }

        switch (command.toLatin1()) {
        case 'M':
            if (index + 1 >= tokens.size()) {
                return path;
            }
            path.moveTo(readNumber(index), readNumber(index + 1));
            index += 2;
            break;
        case 'L':
            if (index + 1 >= tokens.size()) {
                return path;
            }
            path.lineTo(readNumber(index), readNumber(index + 1));
            index += 2;
            break;
        case 'C':
            if (index + 5 >= tokens.size()) {
                return path;
            }
            path.cubicTo(readNumber(index), readNumber(index + 1),
                         readNumber(index + 2), readNumber(index + 3),
                         readNumber(index + 4), readNumber(index + 5));
            index += 6;
            break;
        case 'Z':
            path.closeSubpath();
            break;
        default:
            return path;
        }
    }

    return path;
}

} // namespace

PathText::PathText(QObject *parent)
    : QObject(parent)
{
}

void PathText::setPath(const QPainterPath &path)
{
    m_path = path;
    m_pathLength = m_path.length();
}

void PathText::setText(const QString &text, const QFont &font)
{
    m_text = text;
    m_font = font;
    rebuildGlyphs();
}

void PathText::setBrushColor(const QColor &color)
{
    m_brushColor = color;
}

void PathText::setReversed(bool reversed)
{
    m_reversed = reversed;
}

void PathText::setFirstMargin(double margin)
{
    m_firstMargin = clampNonNegative(margin);
}

void PathText::setLastMargin(double margin)
{
    m_lastMargin = clampNonNegative(margin);
}

void PathText::setPerpendicularOffset(double offset)
{
    m_perpendicularOffset = offset;
}

QImage PathText::renderFrame(QSize size, double progress, double offset) const
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    if (!size.isValid() || m_path.isEmpty() || m_text.isEmpty() || m_glyphs.isEmpty()) {
        return image;
    }

    const double clampedProgress = clampUnit(progress);
    if (clampedProgress <= 0.0 || m_pathLength <= kLengthEpsilon || m_totalAdvance <= kLengthEpsilon) {
        return image;
    }

    const double firstMargin = clampNonNegative(m_firstMargin);
    const double lastMargin = clampNonNegative(m_lastMargin);
    const double usableLength = std::max(0.0, m_pathLength - firstMargin - lastMargin);
    if (usableLength <= kMarginEpsilon) {
        return image;
    }

    const double visibleAdvance = m_totalAdvance * clampedProgress;
    const double combinedOffset = m_perpendicularOffset + offset;

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_brushColor);

    double advanceCursor = 0.0;
    for (const GlyphData &glyph : m_glyphs) {
        const double glyphAdvance = glyph.advance;
        const double glyphMidAdvance = advanceCursor + (glyphAdvance * 0.5);
        if (glyphMidAdvance - kLengthEpsilon > visibleAdvance) {
            break;
        }

        double distanceAlongPath = 0.0;
        if (m_reversed) {
            distanceAlongPath = m_pathLength - firstMargin - glyphMidAdvance;
            if (distanceAlongPath < lastMargin - kMarginEpsilon ||
                distanceAlongPath > m_pathLength - firstMargin + kMarginEpsilon) {
                advanceCursor += glyphAdvance;
                continue;
            }
        } else {
            distanceAlongPath = firstMargin + glyphMidAdvance;
            if (distanceAlongPath < firstMargin - kMarginEpsilon ||
                distanceAlongPath > m_pathLength - lastMargin + kMarginEpsilon) {
                advanceCursor += glyphAdvance;
                continue;
            }
        }

        distanceAlongPath = std::clamp(distanceAlongPath, 0.0, m_pathLength);
        const qreal pathPercent = m_path.percentAtLength(distanceAlongPath);
        const QPointF pathPoint = m_path.pointAtPercent(pathPercent);
        const qreal tangentAngle = m_path.angleAtPercent(pathPercent);

        QTransform transform;
        transform.translate(pathPoint.x(), pathPoint.y());
        transform.rotate(-tangentAngle);
        transform.translate(-glyphAdvance * 0.5, -combinedOffset);
        painter.drawPath(transform.map(glyph.path));

        advanceCursor += glyphAdvance;
    }

    return image;
}

QJsonObject PathText::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("text")] = m_text;

    QJsonObject fontObj;
    fontObj[QStringLiteral("family")] = m_font.family();
    fontObj[QStringLiteral("pointSize")] = m_font.pointSize();
    fontObj[QStringLiteral("weight")] = m_font.weight();
    fontObj[QStringLiteral("italic")] = m_font.italic();
    fontObj[QStringLiteral("pixelSize")] = m_font.pixelSize();
    obj[QStringLiteral("font")] = fontObj;

    obj[QStringLiteral("brushColor")] = m_brushColor.name(QColor::HexArgb);
    obj[QStringLiteral("path")] = pathToSvgString(m_path);
    obj[QStringLiteral("reversed")] = m_reversed;
    obj[QStringLiteral("perpendicularOffset")] = m_perpendicularOffset;
    obj[QStringLiteral("firstMargin")] = m_firstMargin;
    obj[QStringLiteral("lastMargin")] = m_lastMargin;
    return obj;
}

void PathText::fromJson(const QJsonObject &obj)
{
    QFont font;
    const QJsonObject fontObj = obj[QStringLiteral("font")].toObject();
    font.setFamily(fontObj[QStringLiteral("family")].toString(font.family()));

    const int pointSize = fontObj[QStringLiteral("pointSize")].toInt(-1);
    const int pixelSize = fontObj[QStringLiteral("pixelSize")].toInt(-1);
    if (pointSize > 0) {
        font.setPointSize(pointSize);
    } else if (pixelSize > 0) {
        font.setPixelSize(pixelSize);
    }

    font.setWeight(static_cast<QFont::Weight>(
        fontObj[QStringLiteral("weight")].toInt(font.weight())));
    font.setItalic(fontObj[QStringLiteral("italic")].toBool(font.italic()));

    setText(obj[QStringLiteral("text")].toString(), font);
    setBrushColor(QColor(obj[QStringLiteral("brushColor")].toString(
        m_brushColor.name(QColor::HexArgb))));
    setPath(svgStringToPath(obj[QStringLiteral("path")].toString()));
    setReversed(obj[QStringLiteral("reversed")].toBool(false));
    setFirstMargin(obj[QStringLiteral("firstMargin")].toDouble(0.0));
    setLastMargin(obj[QStringLiteral("lastMargin")].toDouble(0.0));
    setPerpendicularOffset(obj[QStringLiteral("perpendicularOffset")].toDouble(0.0));
}

void PathText::rebuildGlyphs()
{
    m_glyphs.clear();
    m_totalAdvance = 0.0;

    if (m_text.isEmpty()) {
        return;
    }

    const QFontMetricsF metrics(m_font);
    m_glyphs.reserve(m_text.size());
    for (const QChar ch : m_text) {
        GlyphData glyph;
        glyph.advance = metrics.horizontalAdvance(ch);
        glyph.path.addText(QPointF(0.0, 0.0), m_font, QString(ch));
        m_totalAdvance += glyph.advance;
        m_glyphs.push_back(glyph);
    }
}
