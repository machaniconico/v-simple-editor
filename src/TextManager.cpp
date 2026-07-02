#include "TextManager.h"
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QFile>
#include <QTextStream>
#include <QTextLayout>
#include <QTextOption>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QDir>
#include <cmath>

// --- TextAnimation ---

QString TextAnimation::typeName(TextAnimationType t) {
    switch (t) {
    case TextAnimationType::None:       return "None";
    case TextAnimationType::FadeIn:     return "Fade In";
    case TextAnimationType::FadeOut:    return "Fade Out";
    case TextAnimationType::FadeInOut:  return "Fade In/Out";
    case TextAnimationType::SlideLeft:  return "Slide Left";
    case TextAnimationType::SlideRight: return "Slide Right";
    case TextAnimationType::SlideUp:    return "Slide Up";
    case TextAnimationType::SlideDown:  return "Slide Down";
    case TextAnimationType::Typewriter: return "Typewriter";
    case TextAnimationType::Bounce:     return "Bounce";
    case TextAnimationType::ScaleIn:    return "Scale In";
    case TextAnimationType::Pop:        return "Pop";
    }
    return "Unknown";
}

QVector<TextAnimationType> TextAnimation::allTypes() {
    return { TextAnimationType::None, TextAnimationType::FadeIn, TextAnimationType::FadeOut,
             TextAnimationType::FadeInOut, TextAnimationType::SlideLeft, TextAnimationType::SlideRight,
             TextAnimationType::SlideUp, TextAnimationType::SlideDown, TextAnimationType::Typewriter,
             TextAnimationType::Bounce, TextAnimationType::ScaleIn, TextAnimationType::Pop };
}

// --- TextTemplate ---

TextTemplate TextTemplate::fromOverlay(const EnhancedTextOverlay &o, const QString &name) {
    TextTemplate t;
    t.name = name;
    t.font = o.font;
    t.color = o.color;
    t.backgroundColor = o.backgroundColor;
    t.outlineColor = o.outlineColor;
    t.outlineWidth = o.outlineWidth;
    t.outline2Color = o.outline2Color;
    t.outline2Width = o.outline2Width;
    t.shadow = o.shadow;
    t.glow = o.glow;
    t.animIn = o.animIn;
    t.animOut = o.animOut;
    t.opacity = o.opacity;
    return t;
}

EnhancedTextOverlay TextTemplate::applyTo(const EnhancedTextOverlay &o) const {
    EnhancedTextOverlay result = o;
    result.font = font;
    result.color = color;
    result.backgroundColor = backgroundColor;
    result.outlineColor = outlineColor;
    result.outlineWidth = outlineWidth;
    result.outline2Color = outline2Color;
    result.outline2Width = outline2Width;
    result.shadow = shadow;
    result.glow = glow;
    result.animIn = animIn;
    result.animOut = animOut;
    result.opacity = opacity;
    result.templateName = name;
    return result;
}

// --- TextManager ---

void TextManager::addOverlay(const EnhancedTextOverlay &overlay) { m_overlays.append(overlay); }
void TextManager::removeOverlay(int index) {
    if (index >= 0 && index < m_overlays.size()) m_overlays.removeAt(index);
}
void TextManager::updateOverlay(int index, const EnhancedTextOverlay &overlay) {
    if (index >= 0 && index < m_overlays.size()) m_overlays[index] = overlay;
}
void TextManager::moveOverlay(int from, int to) {
    if (from >= 0 && from < m_overlays.size() && to >= 0 && to < m_overlays.size())
        m_overlays.move(from, to);
}

void TextManager::renderAll(QImage &frame, double currentTime) const {
    for (const auto &overlay : m_overlays)
        EnhancedTextRenderer::render(frame, overlay, currentTime);
}

// Template file path
static QString templateFilePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + "/text_templates.json";
}

void TextManager::saveTemplate(const TextTemplate &tmpl) {
    auto templates = loadTemplates();
    // Replace if exists
    for (auto &t : templates) {
        if (t.name == tmpl.name) { t = tmpl; goto save; }
    }
    templates.append(tmpl);
save:
    QJsonArray arr;
    for (const auto &t : templates) {
        QJsonObject obj;
        obj["name"] = t.name;
        obj["fontFamily"] = t.font.family();
        obj["fontSize"] = t.font.pointSize();
        obj["fontBold"] = t.font.bold();
        obj["color"] = t.color.name(QColor::HexArgb);
        obj["bgColor"] = t.backgroundColor.name(QColor::HexArgb);
        obj["outlineColor"] = t.outlineColor.name(QColor::HexArgb);
        obj["outlineWidth"] = t.outlineWidth;
        obj["outline2Color"] = t.outline2Color.name(QColor::HexArgb);
        obj["outline2Width"] = t.outline2Width;
        obj["shadowEnabled"] = t.shadow.enabled;
        obj["shadowOffX"] = t.shadow.offsetX;
        obj["shadowOffY"] = t.shadow.offsetY;
        obj["shadowBlur"] = t.shadow.blur;
        obj["shadowColor"] = t.shadow.color.name(QColor::HexArgb);
        obj["shadowOpacity"] = t.shadow.opacity;
        obj["glowEnabled"] = t.glow.enabled;
        obj["glowRadius"] = t.glow.radius;
        obj["glowColor"] = t.glow.color.name(QColor::HexArgb);
        obj["glowOpacity"] = t.glow.opacity;
        obj["animInType"] = static_cast<int>(t.animIn.type);
        obj["animInDur"] = t.animIn.duration;
        obj["animOutType"] = static_cast<int>(t.animOut.type);
        obj["animOutDur"] = t.animOut.duration;
        obj["opacity"] = t.opacity;
        arr.append(obj);
    }
    QFile file(templateFilePath());
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(arr).toJson());
}

void TextManager::removeTemplate(const QString &name) {
    auto templates = loadTemplates();
    templates.erase(std::remove_if(templates.begin(), templates.end(),
        [&](const TextTemplate &t) { return t.name == name; }), templates.end());
    // Re-save
    QJsonArray arr;
    for (const auto &t : templates) {
        QJsonObject obj;
        obj["name"] = t.name;
        // minimal re-save
        arr.append(obj);
    }
    QFile file(templateFilePath());
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(arr).toJson());
}

QVector<TextTemplate> TextManager::loadTemplates() {
    QVector<TextTemplate> templates;
    QFile file(templateFilePath());
    if (!file.open(QIODevice::ReadOnly)) return templates;
    QJsonArray arr = QJsonDocument::fromJson(file.readAll()).array();
    for (const auto &v : arr) {
        QJsonObject obj = v.toObject();
        TextTemplate t;
        t.name = obj["name"].toString();
        t.font = QFont(obj["fontFamily"].toString("Arial"), obj["fontSize"].toInt(32),
                        obj["fontBold"].toBool(true) ? QFont::Bold : QFont::Normal);
        t.color = QColor(obj["color"].toString("#ffffffff"));
        t.backgroundColor = QColor(obj["bgColor"].toString("#a0000000"));
        t.outlineColor = QColor(obj["outlineColor"].toString("#ff000000"));
        t.outlineWidth = obj["outlineWidth"].toInt(2);
        t.outline2Color = QColor(obj["outline2Color"].toString("#00000000"));
        t.outline2Width = obj["outline2Width"].toInt(0);
        t.shadow.enabled = obj["shadowEnabled"].toBool();
        t.shadow.offsetX = obj["shadowOffX"].toDouble(3);
        t.shadow.offsetY = obj["shadowOffY"].toDouble(3);
        t.shadow.blur = obj["shadowBlur"].toDouble(4);
        t.shadow.color = QColor(obj["shadowColor"].toString("#b4000000"));
        t.shadow.opacity = obj["shadowOpacity"].toDouble(1.0);
        t.glow.enabled = obj["glowEnabled"].toBool(false);
        t.glow.radius = obj["glowRadius"].toDouble(8.0);
        t.glow.color = QColor(obj["glowColor"].toString("#ffffff00"));
        t.glow.opacity = obj["glowOpacity"].toDouble(0.9);
        t.animIn.type = static_cast<TextAnimationType>(obj["animInType"].toInt());
        t.animIn.duration = obj["animInDur"].toDouble(0.5);
        t.animOut.type = static_cast<TextAnimationType>(obj["animOutType"].toInt());
        t.animOut.duration = obj["animOutDur"].toDouble(0.5);
        t.opacity = obj["opacity"].toDouble(1.0);
        templates.append(t);
    }
    return templates;
}

TextTemplate TextManager::defaultTemplate(const QString &name) {
    TextTemplate t;
    t.name = name;
    t.font = QFont("Arial", 32, QFont::Bold);
    t.color = Qt::white;
    t.backgroundColor = QColor(0, 0, 0, 160);
    t.outlineColor = Qt::black;
    t.outlineWidth = 2;
    t.opacity = 1.0;
    return t;
}

// --- SRT Import ---

QVector<EnhancedTextOverlay> TextManager::importSRT(const QString &filePath) {
    QVector<EnhancedTextOverlay> overlays;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return overlays;

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    auto parseTime = [](const QString &s) -> double {
        // Format: HH:MM:SS,mmm
        QStringList parts = s.trimmed().replace(',', '.').split(':');
        if (parts.size() != 3) return 0.0;
        return parts[0].toDouble() * 3600 + parts[1].toDouble() * 60 + parts[2].toDouble();
    };

    enum State { WaitIndex, WaitTime, ReadText };
    State state = WaitIndex;
    EnhancedTextOverlay current;
    QStringList textLines;

    while (!stream.atEnd()) {
        QString line = stream.readLine();

        switch (state) {
        case WaitIndex:
            if (!line.trimmed().isEmpty() && line.trimmed().toInt() > 0)
                state = WaitTime;
            break;
        case WaitTime:
            if (line.contains("-->")) {
                QStringList times = line.split("-->");
                if (times.size() == 2) {
                    current.startTime = parseTime(times[0]);
                    current.endTime = parseTime(times[1]);
                }
                textLines.clear();
                state = ReadText;
            }
            break;
        case ReadText:
            if (line.trimmed().isEmpty()) {
                current.text = textLines.join('\n');
                current.x = 0.5;
                current.y = 0.85;
                overlays.append(current);
                current = EnhancedTextOverlay{};
                state = WaitIndex;
            } else {
                textLines.append(line);
            }
            break;
        }
    }
    // Handle last entry
    if (!textLines.isEmpty()) {
        current.text = textLines.join('\n');
        current.x = 0.5;
        current.y = 0.85;
        overlays.append(current);
    }

    return overlays;
}

QVector<EnhancedTextOverlay> TextManager::importVTT(const QString &filePath) {
    // VTT is similar to SRT but with "WEBVTT" header and . instead of , for time
    QVector<EnhancedTextOverlay> overlays;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return overlays;

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    // Skip WEBVTT header
    QString header = stream.readLine();
    if (!header.startsWith("WEBVTT")) return overlays;

    auto parseTime = [](const QString &s) -> double {
        QStringList parts = s.trimmed().split(':');
        if (parts.size() == 3)
            return parts[0].toDouble() * 3600 + parts[1].toDouble() * 60 + parts[2].toDouble();
        if (parts.size() == 2)
            return parts[0].toDouble() * 60 + parts[1].toDouble();
        return 0.0;
    };

    EnhancedTextOverlay current;
    QStringList textLines;
    bool readingText = false;

    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (line.contains("-->")) {
            QStringList times = line.split("-->");
            if (times.size() == 2) {
                current.startTime = parseTime(times[0]);
                current.endTime = parseTime(times[1]);
            }
            textLines.clear();
            readingText = true;
        } else if (readingText) {
            if (line.trimmed().isEmpty()) {
                current.text = textLines.join('\n');
                current.x = 0.5; current.y = 0.85;
                overlays.append(current);
                current = EnhancedTextOverlay{};
                readingText = false;
            } else {
                textLines.append(line);
            }
        }
    }
    if (!textLines.isEmpty()) {
        current.text = textLines.join('\n');
        current.x = 0.5; current.y = 0.85;
        overlays.append(current);
    }

    return overlays;
}

// --- SRT / CSV export ---

static QString formatSrtTime(double seconds) {
    const int totalMs = qMax(0, static_cast<int>(seconds * 1000.0));
    const int h = totalMs / 3600000;
    const int m = (totalMs % 3600000) / 60000;
    const int s = (totalMs % 60000) / 1000;
    const int ms = totalMs % 1000;
    return QString("%1:%2:%3,%4")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}

bool TextManager::exportSRT(const QVector<EnhancedTextOverlay> &overlays, const QString &filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    int idx = 1;
    for (const auto &ov : overlays) {
        if (ov.text.isEmpty()) continue;
        const double start = qMax(0.0, ov.startTime);
        const double end = (ov.endTime > start) ? ov.endTime : start + 1.0;
        out << idx++ << "\n";
        out << formatSrtTime(start) << " --> " << formatSrtTime(end) << "\n";
        out << ov.text << "\n\n";
    }
    file.close();
    return true;
}

bool TextManager::exportCSV(const QVector<EnhancedTextOverlay> &overlays, const QString &filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    // Header row — timestamps in seconds so downstream spreadsheets can
    // sort/filter numerically. The text column is RFC 4180 quoted when it
    // contains quotes, commas, or newlines.
    out << "index,start_sec,end_sec,text\n";
    auto quoteText = [](const QString &s) -> QString {
        const bool needsQuote = s.contains('"') || s.contains(',') || s.contains('\n');
        if (!needsQuote) return s;
        QString escaped = s;
        escaped.replace('"', "\"\"");
        return '"' + escaped + '"';
    };
    int idx = 1;
    for (const auto &ov : overlays) {
        if (ov.text.isEmpty()) continue;
        const double start = qMax(0.0, ov.startTime);
        const double end = (ov.endTime > start) ? ov.endTime : start + 1.0;
        out << idx++ << ','
            << QString::number(start, 'f', 3) << ','
            << QString::number(end, 'f', 3) << ','
            << quoteText(ov.text) << '\n';
    }
    file.close();
    return true;
}

// --- Serialization ---

QJsonArray TextManager::toJson(const QVector<EnhancedTextOverlay> &overlays) {
    QJsonArray arr;
    for (const auto &o : overlays) {
        QJsonObject obj;
        obj["text"] = o.text;
        obj["fontFamily"] = o.font.family();
        obj["fontSize"] = o.font.pointSize();
        obj["fontBold"] = o.font.bold();
        obj["color"] = o.color.name(QColor::HexArgb);
        obj["bgColor"] = o.backgroundColor.name(QColor::HexArgb);
        if (o.letterSpacing != 0.0)
            obj["letterSpacing"] = o.letterSpacing;
        if (o.lineSpacing != 0.0)
            obj["lineSpacing"] = o.lineSpacing;
        obj["x"] = o.x; obj["y"] = o.y;
        obj["width"] = o.width; obj["height"] = o.height;
        obj["rotation"] = o.rotation;
        obj["scale"] = o.scale;
        obj["opacity"] = o.opacity;
        obj["startTime"] = o.startTime;
        obj["endTime"] = o.endTime;
        obj["outlineColor"] = o.outlineColor.name(QColor::HexArgb);
        obj["outlineWidth"] = o.outlineWidth;
        obj["outline2Color"] = o.outline2Color.name(QColor::HexArgb);
        obj["outline2Width"] = o.outline2Width;
        obj["gradientEnabled"] = o.gradientEnabled;
        obj["gradientStart"] = o.gradientStart.name(QColor::HexArgb);
        obj["gradientEnd"] = o.gradientEnd.name(QColor::HexArgb);
        obj["gradientAngle"] = o.gradientAngle;
        obj["gradientType"] = o.gradientType;
        obj["gradientMidpoint"] = o.gradientMidpoint;
        obj["gradientReverse"] = o.gradientReverse;
        if (!o.gradientStops.isEmpty()) {
            QJsonArray stopsArr;
            for (const auto &s : o.gradientStops) {
                QJsonObject sObj;
                sObj["position"] = s.position;
                sObj["color"] = s.color.name(QColor::HexArgb);
                sObj["opacity"] = s.opacity;
                stopsArr.append(sObj);
            }
            obj["gradientStops"] = stopsArr;
        }
        obj["shadowEnabled"] = o.shadow.enabled;
        obj["shadowOffX"] = o.shadow.offsetX;
        obj["shadowOffY"] = o.shadow.offsetY;
        obj["shadowBlur"] = o.shadow.blur;
        obj["shadowColor"] = o.shadow.color.name(QColor::HexArgb);
        obj["shadowOpacity"] = o.shadow.opacity;
        obj["glowEnabled"] = o.glow.enabled;
        obj["glowRadius"] = o.glow.radius;
        obj["glowColor"] = o.glow.color.name(QColor::HexArgb);
        obj["glowOpacity"] = o.glow.opacity;
        obj["animInType"] = static_cast<int>(o.animIn.type);
        obj["animInDur"] = o.animIn.duration;
        obj["animOutType"] = static_cast<int>(o.animOut.type);
        obj["animOutDur"] = o.animOut.duration;
        obj["alignment"] = o.alignment;
        obj["wordWrap"] = o.wordWrap;
        obj["templateName"] = o.templateName;

        if (!o.positionKeyframes.isEmpty()) {
            QJsonArray pkArr;
            for (const auto &pk : o.positionKeyframes) {
                QJsonObject pkObj;
                pkObj["time"] = pk.time;
                pkObj["cx"] = pk.cx;
                pkObj["cy"] = pk.cy;
                pkArr.append(pkObj);
            }
            obj["positionKeyframes"] = pkArr;
        }

        if (!o.rubyAnnotations.isEmpty()) {
            QJsonArray rubyArr;
            for (const auto &r : o.rubyAnnotations) {
                QJsonObject rObj;
                rObj["start"] = r.startIndex;
                rObj["length"] = r.length;
                rObj["ruby"] = r.ruby;
                rubyArr.append(rObj);
            }
            obj["ruby"] = rubyArr;
        }
        arr.append(obj);
    }
    return arr;
}

QVector<EnhancedTextOverlay> TextManager::fromJson(const QJsonArray &arr) {
    QVector<EnhancedTextOverlay> overlays;
    for (const auto &v : arr) {
        QJsonObject obj = v.toObject();
        EnhancedTextOverlay o;
        o.text = obj["text"].toString();
        o.font = QFont(obj["fontFamily"].toString("Arial"), obj["fontSize"].toInt(32),
                        obj["fontBold"].toBool(true) ? QFont::Bold : QFont::Normal);
        o.color = QColor(obj["color"].toString("#ffffffff"));
        o.backgroundColor = QColor(obj["bgColor"].toString("#a0000000"));
        o.letterSpacing = obj["letterSpacing"].toDouble(0.0);
        o.lineSpacing = obj["lineSpacing"].toDouble(0.0);
        o.x = obj["x"].toDouble(0.5); o.y = obj["y"].toDouble(0.85);
        o.width = obj["width"].toDouble(); o.height = obj["height"].toDouble();
        o.rotation = obj["rotation"].toDouble();
        o.scale = obj["scale"].toDouble(1.0);
        o.opacity = obj["opacity"].toDouble(1.0);
        o.startTime = obj["startTime"].toDouble();
        o.endTime = obj["endTime"].toDouble();
        o.outlineColor = QColor(obj["outlineColor"].toString("#ff000000"));
        o.outlineWidth = obj["outlineWidth"].toInt(2);
        o.outline2Color = QColor(obj["outline2Color"].toString("#00000000"));
        o.outline2Width = obj["outline2Width"].toInt();
        o.gradientEnabled = obj["gradientEnabled"].toBool(false);
        o.gradientStart = QColor(obj["gradientStart"].toString("#ffffffff"));
        o.gradientEnd = QColor(obj["gradientEnd"].toString("#ffffc800"));
        o.gradientAngle = obj["gradientAngle"].toDouble(90.0);
        o.gradientType = obj["gradientType"].toInt(0);
        o.gradientMidpoint = obj["gradientMidpoint"].toDouble(50.0);
        o.gradientReverse = obj["gradientReverse"].toBool(false);
        if (obj.contains("gradientStops")) {
            const QJsonArray stopsArr = obj["gradientStops"].toArray();
            for (const auto &v : stopsArr) {
                const QJsonObject sObj = v.toObject();
                GradientStop s;
                s.position = sObj["position"].toDouble(0.0);
                s.color = QColor(sObj["color"].toString("#ffffffff"));
                s.opacity = sObj["opacity"].toDouble(1.0);
                o.gradientStops.append(s);
            }
        }
        o.shadow.enabled = obj["shadowEnabled"].toBool();
        o.shadow.offsetX = obj["shadowOffX"].toDouble(3);
        o.shadow.offsetY = obj["shadowOffY"].toDouble(3);
        o.shadow.blur = obj["shadowBlur"].toDouble(4);
        o.shadow.color = QColor(obj["shadowColor"].toString("#b4000000"));
        o.shadow.opacity = obj["shadowOpacity"].toDouble(1.0);
        o.glow.enabled = obj["glowEnabled"].toBool(false);
        o.glow.radius = obj["glowRadius"].toDouble(8.0);
        o.glow.color = QColor(obj["glowColor"].toString("#ffffff00"));
        o.glow.opacity = obj["glowOpacity"].toDouble(0.9);
        o.animIn.type = static_cast<TextAnimationType>(obj["animInType"].toInt());
        o.animIn.duration = obj["animInDur"].toDouble(0.5);
        o.animOut.type = static_cast<TextAnimationType>(obj["animOutType"].toInt());
        o.animOut.duration = obj["animOutDur"].toDouble(0.5);
        o.alignment = obj["alignment"].toInt(Qt::AlignCenter);
        o.wordWrap = obj["wordWrap"].toBool(true);
        o.templateName = obj["templateName"].toString();

        if (obj.contains("positionKeyframes")) {
            const QJsonArray pkArr = obj["positionKeyframes"].toArray();
            for (const auto &v : pkArr) {
                const QJsonObject pkObj = v.toObject();
                PositionKeyframe pk;
                pk.time = pkObj["time"].toDouble(0.0);
                pk.cx = pkObj["cx"].toDouble(0.5);
                pk.cy = pkObj["cy"].toDouble(0.5);
                o.positionKeyframes.append(pk);
            }
        }

        if (obj.contains("ruby")) {
            for (const auto &rv : obj["ruby"].toArray()) {
                QJsonObject rObj = rv.toObject();
                RubyAnnotation r;
                r.startIndex = rObj["start"].toInt();
                r.length = rObj["length"].toInt(1);
                r.ruby = rObj["ruby"].toString();
                o.rubyAnnotations.append(r);
            }
        }
        overlays.append(o);
    }
    return overlays;
}

// ===== Enhanced Text Renderer =====

namespace {

struct SpacedTextLine {
    QString text;
    qreal width = 0.0;
};

QVector<SpacedTextLine> layoutSpacedTextLines(const QString &text, const QFont &font,
                                              qreal maxWidth, bool wordWrap)
{
    QVector<SpacedTextLine> lines;
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(wordWrap ? QTextOption::WordWrap : QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    const qreal lineWidth = qMax<qreal>(1.0, maxWidth);
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(lineWidth);
        QString lineText = text.mid(line.textStart(), line.textLength());
        lineText.remove(QChar::LineSeparator);
        lineText.remove(QLatin1Char('\n'));
        lineText.remove(QLatin1Char('\r'));
        lines.append({lineText, line.naturalTextWidth()});
    }
    layout.endLayout();
    if (lines.isEmpty() && !text.isEmpty()) {
        const QFontMetrics fm(font);
        lines.append({text, static_cast<qreal>(fm.horizontalAdvance(text))});
    }
    return lines;
}

QPainterPath spacedTextPath(const QVector<SpacedTextLine> &lines, const QRect &rect,
                            const QFont &font, int alignment, double lineSpacing)
{
    QPainterPath path;
    if (lines.isEmpty())
        return path;

    const QFontMetrics fm(font);
    const double lineAdvance = qMax(1.0, static_cast<double>(fm.height()) + lineSpacing);
    double baselineY = rect.top() + fm.ascent();
    for (const SpacedTextLine &line : lines) {
        if (!line.text.isEmpty()) {
            double x = rect.left();
            if (alignment & Qt::AlignRight)
                x = rect.right() + 1.0 - line.width;
            else if (alignment & Qt::AlignHCenter)
                x = rect.left() + (rect.width() - line.width) * 0.5;
            path.addText(x, baselineY, font, line.text);
        }
        baselineY += lineAdvance;
    }
    return path;
}

QRect spacedTextBounds(const QString &text, const QFont &font, int maxWidth,
                       bool wordWrap, double lineSpacing)
{
    const QVector<SpacedTextLine> lines =
        layoutSpacedTextLines(text, font, qMax(1, maxWidth), wordWrap);
    const QFontMetrics fm(font);
    qreal maxLineWidth = 0.0;
    for (const SpacedTextLine &line : lines)
        maxLineWidth = qMax(maxLineWidth, line.width);
    const int lineCount = qMax(1, lines.size());
    const double lineAdvance = qMax(1.0, static_cast<double>(fm.height()) + lineSpacing);
    const double totalHeight =
        fm.height() + qMax(0, lineCount - 1) * lineAdvance;
    return QRect(0, 0,
                 static_cast<int>(std::ceil(maxLineWidth)),
                 static_cast<int>(std::ceil(qMax(1.0, totalHeight))));
}

} // namespace

void EnhancedTextRenderer::render(QImage &frame, const EnhancedTextOverlay &overlay, double currentTime)
{
    if (!overlay.visible || overlay.text.isEmpty()) return;
    if (currentTime < overlay.startTime) return;
    if (overlay.endTime > 0 && currentTime > overlay.endTime) return;

    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    // Calculate animation state
    double animX = 0, animY = 0, animOpacity = overlay.opacity, animScale = overlay.scale;
    int visibleChars = overlay.text.length();

    // In animation
    double elapsed = currentTime - overlay.startTime;
    if (overlay.animIn.type != TextAnimationType::None && elapsed < overlay.animIn.duration) {
        double progress = elapsed / overlay.animIn.duration;
        applyAnimation(overlay.animIn, progress, animX, animY, animOpacity, animScale,
                        overlay.text.length(), visibleChars);
    }

    // Out animation
    if (overlay.endTime > 0 && overlay.animOut.type != TextAnimationType::None) {
        double remaining = overlay.endTime - currentTime;
        if (remaining < overlay.animOut.duration && remaining > 0) {
            double progress = 1.0 - (remaining / overlay.animOut.duration);
            double outX = 0, outY = 0, outOpacity = 1.0, outScale = 1.0;
            int outChars = overlay.text.length();
            applyAnimation(overlay.animOut, progress, outX, outY, outOpacity, outScale,
                            overlay.text.length(), outChars);
            animX += outX;
            animY += outY;
            animOpacity *= outOpacity;
            animScale *= outScale;
            visibleChars = qMin(visibleChars, outChars);
        }
    }

    // Calculate text bounds
    QFont scaledFont = overlay.font;

    // Resolve position from keyframes if present
    double posX = overlay.x;
    double posY = overlay.y;
    const double relTime = currentTime - overlay.startTime;
    if (!overlay.positionKeyframes.isEmpty()) {
        const auto &kfs = overlay.positionKeyframes;
        if (relTime <= kfs.first().time) {
            posX = kfs.first().cx;
            posY = kfs.first().cy;
        } else if (relTime >= kfs.last().time) {
            posX = kfs.last().cx;
            posY = kfs.last().cy;
        } else {
            for (int i = 0; i < kfs.size() - 1; ++i) {
                if (relTime >= kfs[i].time && relTime <= kfs[i + 1].time) {
                    double t = (relTime - kfs[i].time) / (kfs[i + 1].time - kfs[i].time);
                    posX = kfs[i].cx + (kfs[i + 1].cx - kfs[i].cx) * t;
                    posY = kfs[i].cy + (kfs[i + 1].cy - kfs[i].cy) * t;
                    break;
                }
            }
        }
    }

    scaledFont.setPointSizeF(overlay.font.pointSizeF() * animScale);
    if (overlay.letterSpacing != 0.0)
        scaledFont.setLetterSpacing(QFont::AbsoluteSpacing,
                                    overlay.letterSpacing * animScale);
    painter.setFont(scaledFont);
    QFontMetrics fm(scaledFont);

    QString displayText = overlay.text.left(visibleChars);
    int flags = overlay.alignment | (overlay.wordWrap ? Qt::TextWordWrap : 0);
    QRect textBounds = (overlay.lineSpacing == 0.0)
        ? fm.boundingRect(QRect(0, 0, frame.width() - 40, 0), flags, displayText)
        : spacedTextBounds(displayText, scaledFont, frame.width() - 40,
                           overlay.wordWrap, overlay.lineSpacing * animScale);

    int px = static_cast<int>((posX + animX) * frame.width()) - textBounds.width() / 2;
    int py = static_cast<int>((posY + animY) * frame.height()) - textBounds.height() / 2;
    QRect drawRect(px, py, textBounds.width(), textBounds.height());

    painter.setOpacity(animOpacity);

    // Save state for rotation
    if (overlay.rotation != 0.0) {
        painter.save();
        painter.translate(drawRect.center());
        painter.rotate(overlay.rotation);
        painter.translate(-drawRect.center());
    }

    // Background
    if (overlay.backgroundColor.alpha() > 0)
        painter.fillRect(drawRect.adjusted(-8, -4, 8, 4), overlay.backgroundColor);

    // Order: shadow (deepest) -> glow -> outline -> fill. Each effect skips
    // its offscreen buffer entirely when disabled so unused effects cost
    // nothing. Outline is drawn last-but-one so it sits on top of the glow
    // halo but below the fill; the existing dual-outline path is reused.
    if (overlay.shadow.enabled)
        renderShadow(painter, displayText, drawRect, overlay);

    if (overlay.glow.enabled)
        renderGlow(painter, displayText, drawRect, overlay);

    if (overlay.lineSpacing != 0.0) {
        const QVector<SpacedTextLine> lines =
            layoutSpacedTextLines(displayText, scaledFont, drawRect.width(), overlay.wordWrap);
        const QPainterPath path =
            spacedTextPath(lines, drawRect, scaledFont, overlay.alignment,
                           overlay.lineSpacing * animScale);

        if (overlay.outline2Width > 0 && overlay.outline2Color.alpha() > 0) {
            painter.setPen(QPen(overlay.outline2Color,
                                overlay.outlineWidth + overlay.outline2Width * 2));
            painter.drawPath(path);
        }
        if (overlay.outlineWidth > 0) {
            painter.setPen(QPen(overlay.outlineColor, overlay.outlineWidth));
            painter.drawPath(path);
        }

        painter.fillPath(path, overlay.color);
    } else {
        // Outline (second/outer first, then inner)
        renderOutline(painter, displayText, drawRect, overlay);

        // Main text
        painter.setPen(overlay.color);
        painter.drawText(drawRect, flags, displayText);
    }

    // Ruby/Furigana
    if (!overlay.rubyAnnotations.isEmpty())
        renderRuby(painter, displayText, drawRect, overlay.rubyAnnotations, scaledFont);

    if (overlay.rotation != 0.0)
        painter.restore();
}

void EnhancedTextRenderer::applyAnimation(const TextAnimation &anim, double progress,
                                            double &x, double &y, double &opacity, double &scale,
                                            int textLen, int &visibleChars)
{
    double ease = progress * progress * (3.0 - 2.0 * progress); // smoothstep

    switch (anim.type) {
    case TextAnimationType::FadeIn:
        opacity = ease;
        break;
    case TextAnimationType::FadeOut:
        opacity = 1.0 - ease;
        break;
    case TextAnimationType::FadeInOut:
        opacity = ease;
        break;
    case TextAnimationType::SlideLeft:
        x = -0.3 * (1.0 - ease);
        opacity = ease;
        break;
    case TextAnimationType::SlideRight:
        x = 0.3 * (1.0 - ease);
        opacity = ease;
        break;
    case TextAnimationType::SlideUp:
        y = 0.2 * (1.0 - ease);
        opacity = ease;
        break;
    case TextAnimationType::SlideDown:
        y = -0.2 * (1.0 - ease);
        opacity = ease;
        break;
    case TextAnimationType::Typewriter:
        visibleChars = static_cast<int>(textLen * ease);
        break;
    case TextAnimationType::Bounce: {
        double bounce = std::abs(std::sin(progress * M_PI * 3)) * (1.0 - progress);
        y = -0.05 * bounce;
        opacity = qMin(1.0, progress * 3.0);
        break;
    }
    case TextAnimationType::ScaleIn:
        scale = ease;
        opacity = ease;
        break;
    case TextAnimationType::Pop: {
        double overshoot = 1.0 + 0.3 * std::sin(progress * M_PI);
        scale = (progress < 1.0) ? overshoot * ease : 1.0;
        opacity = qMin(1.0, progress * 2.0);
        break;
    }
    default: break;
    }
}

// Cheap separable box blur on the alpha channel. Used for both drop shadow
// and outer glow. radius is clamped to a sane upper bound so a slider misuse
// never triggers a many-second stall. Blurs the alpha plane only and keeps
// the source colour, which matches how Photoshop layer styles render.
static void boxBlurAlphaInPlace(QImage &img, int radius)
{
    if (radius <= 0 || img.isNull()) return;
    const int r = qMin(radius, 32);
    const int w = img.width();
    const int h = img.height();
    if (w == 0 || h == 0) return;
    if (img.format() != QImage::Format_ARGB32_Premultiplied)
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // Single-pass per axis. Two passes total, each O(w*h). Cheap enough
    // to run once per frame for a single overlay.
    QVector<int> tmp(w * h);
    auto idx = [w](int x, int y) { return y * w + x; };

    // Horizontal pass: read alpha from img, write into tmp
    for (int y = 0; y < h; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        int sum = 0;
        for (int x = -r; x <= r; ++x) {
            const int cx = qBound(0, x, w - 1);
            sum += qAlpha(row[cx]);
        }
        const int span = 2 * r + 1;
        for (int x = 0; x < w; ++x) {
            tmp[idx(x, y)] = sum / span;
            const int xOut = qBound(0, x - r, w - 1);
            const int xIn  = qBound(0, x + r + 1, w - 1);
            sum += qAlpha(row[xIn]) - qAlpha(row[xOut]);
        }
    }

    // Vertical pass: read tmp, write back into img alpha (RGB stays black=0
    // — caller composites with a colour pen via SourceIn).
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int y = -r; y <= r; ++y) {
            const int cy = qBound(0, y, h - 1);
            sum += tmp[idx(x, cy)];
        }
        const int span = 2 * r + 1;
        for (int y = 0; y < h; ++y) {
            const int a = sum / span;
            QRgb *row = reinterpret_cast<QRgb*>(img.scanLine(y));
            row[x] = qRgba(0, 0, 0, a);
            const int yOut = qBound(0, y - r, h - 1);
            const int yIn  = qBound(0, y + r + 1, h - 1);
            sum += tmp[idx(x, yIn)] - tmp[idx(x, yOut)];
        }
    }
}

// Build an alpha mask of the text path at the given rect, padded by
// padPx on every side so the blur kernel has room. Returns the buffer
// plus the top-left offset (in frame coords) where it should be drawn.
static QImage rasterTextAlpha(const QString &text, const QRect &rect,
                              const QFont &font, int alignFlags, int padPx)
{
    const int W = qMax(1, rect.width()  + padPx * 2);
    const int H = qMax(1, rect.height() + padPx * 2);
    QImage buf(W, H, QImage::Format_ARGB32_Premultiplied);
    buf.fill(Qt::transparent);
    QPainter p(&buf);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font);
    p.setPen(Qt::black); // alpha-only — colour is replaced by caller via SourceIn
    p.drawText(QRect(padPx, padPx, rect.width(), rect.height()),
               alignFlags, text);
    p.end();
    return buf;
}

void EnhancedTextRenderer::renderShadow(QPainter &painter, const QString &text,
                                          const QRect &rect, const EnhancedTextOverlay &overlay)
{
    const double blur = qMax(0.0, overlay.shadow.blur);
    const int pad = qMax(8, static_cast<int>(blur * 2.0) + 4);
    const int alignFlags = overlay.alignment | Qt::TextWordWrap;

    QImage buf = rasterTextAlpha(text, rect, painter.font(), alignFlags, pad);
    boxBlurAlphaInPlace(buf, static_cast<int>(blur));

    // Tint the blurred alpha mask with the shadow colour using SourceIn.
    {
        QPainter pp(&buf);
        pp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        pp.fillRect(buf.rect(), overlay.shadow.color);
    }

    const double opacity = qBound(0.0, overlay.shadow.opacity, 1.0);
    const double prevOp = painter.opacity();
    painter.setOpacity(prevOp * opacity);
    painter.drawImage(rect.left() - pad + static_cast<int>(overlay.shadow.offsetX),
                      rect.top()  - pad + static_cast<int>(overlay.shadow.offsetY),
                      buf);
    painter.setOpacity(prevOp);
}

void EnhancedTextRenderer::renderGlow(QPainter &painter, const QString &text,
                                       const QRect &rect, const EnhancedTextOverlay &overlay)
{
    const double radius = qMax(0.0, overlay.glow.radius);
    const int pad = qMax(8, static_cast<int>(radius * 2.0) + 4);
    const int alignFlags = overlay.alignment | Qt::TextWordWrap;

    QImage buf = rasterTextAlpha(text, rect, painter.font(), alignFlags, pad);
    boxBlurAlphaInPlace(buf, static_cast<int>(radius));

    // Tint the blurred mask with the glow colour. Draw it twice with
    // CompositionMode_Plus so the halo intensifies near the text edges,
    // matching Photoshop's "Outer Glow" Screen blend behaviour cheaply.
    {
        QPainter pp(&buf);
        pp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        pp.fillRect(buf.rect(), overlay.glow.color);
    }

    const double opacity = qBound(0.0, overlay.glow.opacity, 1.0);
    const double prevOp = painter.opacity();
    const QPainter::CompositionMode prevMode = painter.compositionMode();
    painter.setOpacity(prevOp * opacity);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.drawImage(rect.left() - pad, rect.top() - pad, buf);
    painter.setCompositionMode(prevMode);
    painter.setOpacity(prevOp);
}

void EnhancedTextRenderer::renderOutline(QPainter &painter, const QString &text,
                                           const QRect &rect, const EnhancedTextOverlay &overlay)
{
    QPainterPath path;
    QFontMetrics fm(painter.font());
    path.addText(rect.left(), rect.top() + fm.ascent(), painter.font(), text);

    // Outer outline (second outline)
    if (overlay.outline2Width > 0 && overlay.outline2Color.alpha() > 0) {
        painter.setPen(QPen(overlay.outline2Color, overlay.outlineWidth + overlay.outline2Width * 2));
        painter.drawPath(path);
    }

    // Inner outline
    if (overlay.outlineWidth > 0) {
        painter.setPen(QPen(overlay.outlineColor, overlay.outlineWidth));
        painter.drawPath(path);
    }
}

void EnhancedTextRenderer::renderRuby(QPainter &painter, const QString &baseText,
                                        const QRect &rect, const QVector<RubyAnnotation> &annotations,
                                        const QFont &baseFont)
{
    QFont rubyFont = baseFont;
    rubyFont.setPointSizeF(baseFont.pointSizeF() * 0.5);
    QFontMetrics baseFm(baseFont);
    QFontMetrics rubyFm(rubyFont);

    painter.setFont(rubyFont);

    for (const auto &ann : annotations) {
        if (ann.startIndex >= baseText.length()) continue;

        // Calculate position of annotated characters
        QString before = baseText.left(ann.startIndex);
        QString annotated = baseText.mid(ann.startIndex, ann.length);

        int beforeWidth = baseFm.horizontalAdvance(before);
        int annotatedWidth = baseFm.horizontalAdvance(annotated);
        int rubyWidth = rubyFm.horizontalAdvance(ann.ruby);

        // Center ruby above annotated text
        int rubyX = rect.left() + beforeWidth + (annotatedWidth - rubyWidth) / 2;
        int rubyY = rect.top() - rubyFm.height() + 2; // above the text

        painter.drawText(rubyX, rubyY + rubyFm.ascent(), ann.ruby);
    }

    painter.setFont(baseFont); // restore
}
