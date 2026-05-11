#include "MographText.h"
#include "TextAnimator.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QRectF>
#include <cmath>

// --- Constructor ---

MographText::MographText() = default;

// --- Builder API ---

void MographText::applyLowerThird(TextAnimator *animator, const QString &topLine, const QString &subLine)
{
    if (!animator) return;

    QString displayText = topLine.isEmpty() ? QStringLiteral("Name") : topLine;
    if (!subLine.isEmpty())
        displayText += QLatin1String("\n") + subLine;

    QFont font(QStringLiteral("Arial"), 28, QFont::Bold);
    animator->setText(displayText, font, QPointF(60, 0));

    TextAnimConfig config;
    config.animation = CharAnimationType::SlideInLeft;
    config.duration = 0.5;
    config.delay = 0.03;
    config.easing = TextAnimEasing::EaseOut;
    animator->setAnimation(config);
}

void MographText::applyHeadlineKinetic(TextAnimator *animator, const QString &text)
{
    if (!animator) return;

    QString displayText = text.isEmpty() ? QStringLiteral("HEADLINE") : text;

    QFont font(QStringLiteral("Arial Black"), 48, QFont::Bold);
    animator->setText(displayText, font, QPointF(40, 0));

    TextAnimConfig config;
    config.animation = CharAnimationType::BounceIn;
    config.duration = 0.6;
    config.delay = 0.05;
    config.easing = TextAnimEasing::Linear;
    config.amplitude = 15.0;
    animator->setAnimation(config);
}

void MographText::applyTitleCard(TextAnimator *animator, const QString &text, const QColor &accent)
{
    if (!animator) return;

    QString displayText = text.isEmpty() ? QStringLiteral("Title") : text;

    QFont font(QStringLiteral("Arial"), 36, QFont::Bold);
    animator->setText(displayText, font, QPointF(0, 0));

    TextAnimConfig config;
    config.animation = CharAnimationType::ScaleUp;
    config.duration = 0.5;
    config.delay = 0.04;
    config.easing = TextAnimEasing::EaseOut;
    config.customColor = accent;
    animator->setAnimation(config);
}

void MographText::applyCalloutBox(TextAnimator *animator, const QString &text)
{
    if (!animator) return;

    QString displayText = text.isEmpty() ? QStringLiteral("Callout") : text;

    QFont font(QStringLiteral("Arial"), 24, QFont::Normal);
    animator->setText(displayText, font, QPointF(80, 0));

    TextAnimConfig config;
    config.animation = CharAnimationType::FadeInLetters;
    config.duration = 0.4;
    config.delay = 0.04;
    config.easing = TextAnimEasing::EaseOut;
    animator->setAnimation(config);
}

void MographText::applySportsScore(TextAnimator *animator, const QString &team1, const QString &score)
{
    if (!animator) return;

    QString t1 = team1.isEmpty() ? QStringLiteral("TEAM 1") : team1;
    QString sc = score.isEmpty() ? QStringLiteral("0 - 0") : score;
    QString displayText = t1 + QLatin1String("  ") + sc;

    QFont font(QStringLiteral("Arial"), 32, QFont::Bold);
    animator->setText(displayText, font, QPointF(40, 0));

    TextAnimConfig config;
    config.animation = CharAnimationType::SlideInRight;
    config.duration = 0.4;
    config.delay = 0.03;
    config.easing = TextAnimEasing::EaseOut;
    animator->setAnimation(config);
}

// --- Template dispatch ---

QStringList MographText::templateNames()
{
    return QStringList()
        << QStringLiteral("Lower Third")
        << QStringLiteral("Headline Kinetic")
        << QStringLiteral("Title Card")
        << QStringLiteral("Callout Box")
        << QStringLiteral("Sports Score");
}

void MographText::applyTemplate(TextAnimator *animator, const QString &name, const QStringList &args)
{
    if (!animator) return;

    MographText mograph;
    if (name == QLatin1String("Lower Third")) {
        mograph.applyLowerThird(animator,
                                args.value(0, QStringLiteral("Name")),
                                args.value(1, QStringLiteral("Title")));
    } else if (name == QLatin1String("Headline Kinetic")) {
        mograph.applyHeadlineKinetic(animator, args.value(0, QStringLiteral("HEADLINE")));
    } else if (name == QLatin1String("Title Card")) {
        QColor accent = args.value(0).isEmpty() ? QColor(0, 122, 204) : QColor(args.value(0));
        mograph.applyTitleCard(animator, args.value(1, QStringLiteral("Title")), accent);
    } else if (name == QLatin1String("Callout Box")) {
        mograph.applyCalloutBox(animator, args.value(0, QStringLiteral("Callout")));
    } else if (name == QLatin1String("Sports Score")) {
        mograph.applySportsScore(animator,
                                 args.value(0, QStringLiteral("TEAM 1")),
                                 args.value(1, QStringLiteral("0 - 0")));
    }
}

// --- Render helpers ---

QImage MographText::renderLowerThird(QSize canvas, double time, const QStringList &args)
{
    QImage image(canvas, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QString topLine = args.value(0, QStringLiteral("Name"));
    QString subLine = args.value(1, QStringLiteral("Title"));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    QFontMetrics topFm(QFont(QStringLiteral("Arial"), 28, QFont::Bold));
    QFontMetrics subFm(QFont(QStringLiteral("Arial"), 18, QFont::Normal));

    int topW = topFm.horizontalAdvance(topLine);
    int subW = subFm.horizontalAdvance(subLine);
    int totalW = std::max(topW, subW) + 60;
    int totalH = topFm.height() + subFm.height() + 30;

    double x = 40;
    double y = canvas.height() - totalH - 40;

    // Animate slide-in (starts partially visible at t=0, fully in at t=0.5)
    double slideIn = std::clamp(time / 0.5, 0.0, 1.0);
    slideIn = 1.0 - (1.0 - slideIn) * (1.0 - slideIn); // ease out

    double offsetX = (1.0 - slideIn) * (-30.0);

    // Backing rectangle
    QColor barColor(30, 30, 30, 220);
    painter.setPen(Qt::NoPen);
    painter.setBrush(barColor);
    painter.drawRoundedRect(QRectF(x + offsetX, y, totalW, totalH), 6, 6);

    // Accent bar
    QColor accent(0, 122, 204);
    painter.setBrush(accent);
    painter.drawRoundedRect(QRectF(x + offsetX, y + totalH - 4, totalW, 4), 2, 2);

    // Text
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Arial"), 28, QFont::Bold));
    painter.drawText(QPointF(x + offsetX + 30, y + topFm.ascent() + 10), topLine);

    painter.setPen(QColor(200, 200, 200));
    painter.setFont(QFont(QStringLiteral("Arial"), 18, QFont::Normal));
    painter.drawText(QPointF(x + offsetX + 30, y + topFm.height() + subFm.ascent() + 10), subLine);

    painter.end();
    return image;
}

QImage MographText::renderHeadlineKinetic(QSize canvas, double time, const QStringList &args)
{
    QImage image(canvas, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QString text = args.value(0, QStringLiteral("HEADLINE"));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    QFont font(QStringLiteral("Arial Black"), 48, QFont::Bold);
    QFontMetrics fm(font);
    int textW = fm.horizontalAdvance(text);
    int textH = fm.height();

    double bounce = std::clamp(time / 0.6, 0.0, 1.0);
    // Simple bounce approximation
    double bounceY = 0.0;
    if (bounce < 1.0) {
        double t = bounce;
        if (t < 1.0 / 2.75) {
            bounceY = -(1.0 - 7.5625 * t * t) * 80.0;
        } else if (t < 2.0 / 2.75) {
            t -= 1.5 / 2.75;
            bounceY = -(1.0 - (7.5625 * t * t + 0.75)) * 80.0;
        } else if (t < 2.5 / 2.75) {
            t -= 2.25 / 2.75;
            bounceY = -(1.0 - (7.5625 * t * t + 0.9375)) * 80.0;
        } else {
            t -= 2.625 / 2.75;
            bounceY = -(1.0 - (7.5625 * t * t + 0.984375)) * 80.0;
        }
    }

    double opacity = std::clamp(bounce * 3.0, 0.0, 1.0);
    painter.setOpacity(opacity);

    // Shadow
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 60));
    painter.drawRoundedRect(QRectF((canvas.width() - textW) / 2.0 + 4,
                                    (canvas.height() - textH) / 2.0 + bounceY + 4,
                                    textW, textH), 8, 8);

    // Text
    painter.setPen(QColor(255, 255, 255));
    painter.setFont(font);
    painter.drawText(QPointF((canvas.width() - textW) / 2.0,
                              (canvas.height() - textH) / 2.0 + bounceY + fm.ascent()),
                     text);

    painter.end();
    return image;
}

QImage MographText::renderTitleCard(QSize canvas, double time, const QStringList &args)
{
    QImage image(canvas, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QColor accent = args.value(0).isEmpty() ? QColor(0, 122, 204) : QColor(args.value(0));
    QString text = args.value(1, QStringLiteral("Title"));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    QFont font(QStringLiteral("Arial"), 36, QFont::Bold);
    QFontMetrics fm(font);
    int textW = fm.horizontalAdvance(text);
    int textH = fm.height();

    double scale = std::clamp(time / 0.5, 0.0, 1.0);
    scale = 1.0 - (1.0 - scale) * (1.0 - scale); // ease out

    double opacity = scale;
    painter.setOpacity(opacity);

    // Center background panel
    double panelW = textW + 80;
    double panelH = textH + 40;
    double panelX = (canvas.width() - panelW) / 2.0;
    double panelY = (canvas.height() - panelH) / 2.0;

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 20, 20, 200));
    painter.drawRoundedRect(QRectF(panelX, panelY, panelW, panelH), 10, 10);

    // Accent line at bottom
    painter.setBrush(accent);
    painter.drawRoundedRect(QRectF(panelX, panelY + panelH - 5, panelW * scale, 5), 2, 2);

    // Text
    painter.setPen(Qt::white);
    painter.setFont(font);
    double scaledTextW = textW * scale;
    double textX = (canvas.width() - scaledTextW) / 2.0;
    painter.drawText(QPointF(textX, (canvas.height() - textH) / 2.0 + fm.ascent()), text);

    painter.end();
    return image;
}

QImage MographText::renderCalloutBox(QSize canvas, double time, const QStringList &args)
{
    QImage image(canvas, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QString text = args.value(0, QStringLiteral("Callout"));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    QFont font(QStringLiteral("Arial"), 24, QFont::Normal);
    QFontMetrics fm(font);
    int textW = fm.horizontalAdvance(text);
    int textH = fm.height();

    double fadeIn = std::clamp(time / 0.4, 0.0, 1.0);
    painter.setOpacity(fadeIn);

    double padding = 20;
    double boxW = textW + padding * 2;
    double boxH = textH + padding * 2;
    double boxX = (canvas.width() - boxW) / 2.0;
    double boxY = (canvas.height() - boxH) / 2.0;

    // Callout box background
    painter.setPen(QPen(QColor(255, 200, 0), 2));
    painter.setBrush(QColor(255, 200, 0, 40));
    painter.drawRoundedRect(QRectF(boxX, boxY, boxW, boxH), 8, 8);

    // Small pointer triangle at bottom
    QPointF pts[3] = {
        QPointF(canvas.width() / 2.0 - 8, boxY + boxH),
        QPointF(canvas.width() / 2.0 + 8, boxY + boxH),
        QPointF(canvas.width() / 2.0, boxY + boxH + 12)
    };
    painter.setBrush(QColor(255, 200, 0, 40));
    painter.setPen(QPen(QColor(255, 200, 0), 2));
    painter.drawPolygon(pts, 3);

    // Text
    painter.setPen(QColor(255, 255, 255));
    painter.setFont(font);
    painter.drawText(QPointF(boxX + padding, boxY + padding + fm.ascent()), text);

    painter.end();
    return image;
}

QImage MographText::renderSportsScore(QSize canvas, double time, const QStringList &args)
{
    QImage image(canvas, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QString team1 = args.value(0, QStringLiteral("TEAM 1"));
    QString score = args.value(1, QStringLiteral("0 - 0"));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    QFont teamFont(QStringLiteral("Arial"), 24, QFont::Bold);
    QFont scoreFont(QStringLiteral("Arial Black"), 36, QFont::Bold);
    QFontMetrics teamFm(teamFont);
    QFontMetrics scoreFm(scoreFont);

    int teamW = teamFm.horizontalAdvance(team1);
    int scoreW = scoreFm.horizontalAdvance(score);
    int totalW = teamW + scoreW + 60;
    int totalH = std::max(teamFm.height(), scoreFm.height()) + 30;

    double slideIn = std::clamp(time / 0.4, 0.0, 1.0);
    slideIn = 1.0 - (1.0 - slideIn) * (1.0 - slideIn); // ease out

    double offsetX = (1.0 - slideIn) * 300.0;

    double x = canvas.width() - totalW - 40 - offsetX;
    double y = 40;

    // Background panel
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 20, 20, 230));
    painter.drawRoundedRect(QRectF(x, y, totalW, totalH), 8, 8);

    // Score accent bar (left side)
    painter.setBrush(QColor(220, 50, 50));
    painter.drawRoundedRect(QRectF(x, y, 6, totalH), 3, 3);

    // Team name
    painter.setPen(Qt::white);
    painter.setFont(teamFont);
    painter.drawText(QPointF(x + 20, y + teamFm.ascent() + 5), team1);

    // Score
    painter.setFont(scoreFont);
    double scoreX = x + teamW + 40;
    painter.drawText(QPointF(scoreX, y + scoreFm.ascent() + 5), score);

    painter.end();
    return image;
}

// --- Public renderTemplate dispatch ---

QImage MographText::renderTemplate(TextAnimator *animator, QSize canvas, double time,
                                    const QString &tmplate, const QStringList &args)
{
    if (tmplate == QLatin1String("Lower Third"))
        return renderLowerThird(canvas, time, args);
    if (tmplate == QLatin1String("Headline Kinetic"))
        return renderHeadlineKinetic(canvas, time, args);
    if (tmplate == QLatin1String("Title Card"))
        return renderTitleCard(canvas, time, args);
    if (tmplate == QLatin1String("Callout Box"))
        return renderCalloutBox(canvas, time, args);
    if (tmplate == QLatin1String("Sports Score"))
        return renderSportsScore(canvas, time, args);

    // Unknown template: return blank transparent image
    QImage blank(canvas, QImage::Format_ARGB32_Premultiplied);
    blank.fill(Qt::transparent);
    return blank;
}

// --- Serialisation ---

QJsonObject MographText::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("template")] = m_templateName;

    QJsonArray argsArr;
    for (const auto &arg : m_args)
        argsArr.append(arg);
    obj[QStringLiteral("args")] = argsArr;

    return obj;
}

void MographText::fromJson(const QJsonObject &obj)
{
    m_templateName = obj[QStringLiteral("template")].toString();

    m_args.clear();
    QJsonArray argsArr = obj[QStringLiteral("args")].toArray();
    for (const auto &val : argsArr)
        m_args.append(val.toString());
}
