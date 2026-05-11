#pragma once

#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QStringList>

class TextAnimator;

class MographText
{
public:
    MographText();

    // --- Builder API: configure a TextAnimator for common mograph layouts ---

    void applyLowerThird(TextAnimator *animator, const QString &topLine, const QString &subLine);
    void applyHeadlineKinetic(TextAnimator *animator, const QString &text);
    void applyTitleCard(TextAnimator *animator, const QString &text, const QColor &accent = QColor(0, 122, 204));
    void applyCalloutBox(TextAnimator *animator, const QString &text);
    void applySportsScore(TextAnimator *animator, const QString &team1, const QString &score);

    // --- Template dispatch ---

    static QStringList templateNames();
    static void applyTemplate(TextAnimator *animator, const QString &name, const QStringList &args);

    // --- Render helper: returns a QImage with the template rendered at the given time ---

    static QImage renderTemplate(TextAnimator *animator, QSize canvas, double time,
                                 const QString &tmplate, const QStringList &args);

    // --- Serialisation ---

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

    // --- Accessors ---

    QString templateName() const { return m_templateName; }
    QStringList args() const { return m_args; }
    void setTemplateName(const QString &name) { m_templateName = name; }
    void setArgs(const QStringList &a) { m_args = a; }

private:
    // Internal render helpers for each template
    static QImage renderLowerThird(QSize canvas, double time, const QStringList &args);
    static QImage renderHeadlineKinetic(QSize canvas, double time, const QStringList &args);
    static QImage renderTitleCard(QSize canvas, double time, const QStringList &args);
    static QImage renderCalloutBox(QSize canvas, double time, const QStringList &args);
    static QImage renderSportsScore(QSize canvas, double time, const QStringList &args);

    QString m_templateName;
    QStringList m_args;
};
