#include "TextManager.h"
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QFile>
#include <QTextStream>
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
        obj["shadowEnabled"] = o.shadow.enabled;
        obj["shadowOffX"] = o.shadow.offsetX;
        obj["shadowOffY"] = o.shadow.offsetY;
        obj["shadowBlur"] = o.shadow.blur;
        obj["shadowColor"] = o.shadow.color.name(QColor::HexArgb);
        obj["animInType"] = static_cast<int>(o.animIn.type);
        obj["animInDur"] = o.animIn.duration;
        obj["animOutType"] = static_cast<int>(o.animOut.type);
        obj["animOutDur"] = o.animOut.duration;
        obj["alignment"] = o.alignment;
        obj["wordWrap"] = o.wordWrap;
        obj["templateName"] = o.templateName;

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
        o.shadow.enabled = obj["shadowEnabled"].toBool();
        o.shadow.offsetX = obj["shadowOffX"].toDouble(3);
        o.shadow.offsetY = obj["shadowOffY"].toDouble(3);
        o.shadow.blur = obj["shadowBlur"].toDouble(4);
        o.shadow.color = QColor(obj["shadowColor"].toString("#b4000000"));
        o.animIn.type = static_cast<TextAnimationType>(obj["animInType"].toInt());
        o.animIn.duration = obj["animInDur"].toDouble(0.5);
        o.animOut.type = static_cast<TextAnimationType>(obj["animOutType"].toInt());
        o.animOut.duration = obj["animOutDur"].toDouble(0.5);
        o.alignment = obj["alignment"].toInt(Qt::AlignCenter);
        o.wordWrap = obj["wordWrap"].toBool(true);
        o.templateName = obj["templateName"].toString();

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
    scaledFont.setPointSizeF(overlay.font.pointSizeF() * animScale);
    painter.setFont(scaledFont);
    QFontMetrics fm(scaledFont);

    QString displayText = overlay.text.left(visibleChars);
    int flags = overlay.alignment | (overlay.wordWrap ? Qt::TextWordWrap : 0);
    QRect textBounds = fm.boundingRect(QRect(0, 0, frame.width() - 40, 0), flags, displayText);

    int px = static_cast<int>((overlay.x + animX) * frame.width()) - textBounds.width() / 2;
    int py = static_cast<int>((overlay.y + animY) * frame.height()) - textBounds.height() / 2;
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

    // Shadow
    if (overlay.shadow.enabled)
        renderShadow(painter, displayText, drawRect, overlay);

    // Outline (second/outer first, then inner)
    renderOutline(painter, displayText, drawRect, overlay);

    // Main text
    painter.setPen(overlay.color);
    painter.drawText(drawRect, flags, displayText);

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

void EnhancedTextRenderer::renderShadow(QPainter &painter, const QString &text,
                                          const QRect &rect, const EnhancedTextOverlay &overlay)
{
    QRect shadowRect = rect.translated(
        static_cast<int>(overlay.shadow.offsetX),
        static_cast<int>(overlay.shadow.offsetY));
    painter.setPen(overlay.shadow.color);
    painter.drawText(shadowRect, overlay.alignment | Qt::TextWordWrap, text);
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
