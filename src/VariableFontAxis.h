#pragma once

#include <QFont>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>

class KeyframeManager;

class VariableFontAxis : public QObject
{
    Q_OBJECT

public:
    explicit VariableFontAxis(QObject *parent = nullptr);

    void setBaseFont(const QFont &font);
    QFont baseFont() const { return m_baseFont; }

    void setKeyframeManager(KeyframeManager *manager);
    KeyframeManager *keyframeManager() const { return m_keyframeManager; }

    void setAxisProperty(const QString &tag, const QString &kfProperty);
    void removeAxisProperty(const QString &tag);
    QMap<QString, QString> axisProperties() const { return m_axisProperties; }

    QFont fontAt(double time) const;

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    static bool hasQtVariableFontSupport();
    void applyAxisValue(QFont &font, const QString &tag, double value) const;

    QFont m_baseFont;
    KeyframeManager *m_keyframeManager = nullptr;
    QMap<QString, QString> m_axisProperties; // axis tag (e.g. "wght") -> keyframe property name
};
