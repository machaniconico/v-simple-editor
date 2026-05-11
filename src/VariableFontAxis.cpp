#include "VariableFontAxis.h"
#include "Keyframe.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QRawFont>
#include <cmath>

VariableFontAxis::VariableFontAxis(QObject *parent)
    : QObject(parent)
{
}

void VariableFontAxis::setBaseFont(const QFont &font)
{
    m_baseFont = font;
}

void VariableFontAxis::setKeyframeManager(KeyframeManager *manager)
{
    m_keyframeManager = manager;
}

void VariableFontAxis::setAxisProperty(const QString &tag, const QString &kfProperty)
{
    m_axisProperties.insert(tag, kfProperty);
}

void VariableFontAxis::removeAxisProperty(const QString &tag)
{
    m_axisProperties.remove(tag);
}

bool VariableFontAxis::hasQtVariableFontSupport()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    return true;
#else
    return false;
#endif
}

void VariableFontAxis::applyAxisValue(QFont &font, const QString &tag, double value) const
{
    if (hasQtVariableFontSupport()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        if (const auto t = QFont::Tag::fromString(tag))
            font.setVariableAxis(*t, static_cast<float>(value));
#else
        Q_UNUSED(tag);
        Q_UNUSED(value);
#endif
    } else {
        // Fallback for older Qt: use QFontDatabase to find closest weight/style
        if (tag == QStringLiteral("wght")) {
            int weight = static_cast<int>(std::clamp(value, 0.0, 1000.0));
            font.setWeight(static_cast<QFont::Weight>(weight));
        } else if (tag == QStringLiteral("wdth")) {
            // Qt doesn't have direct width axis support pre-6.7,
            // but we can adjust stretch as a proxy
            int stretch = static_cast<int>(std::clamp(value, 0.0, 200.0) * 100 / 100.0);
            font.setStretch(stretch);
        } else if (tag == QStringLiteral("slnt")) {
            // For slant, use italic as a binary fallback
            font.setItalic(std::abs(value) > 0.5);
        }
    }
}

QFont VariableFontAxis::fontAt(double time) const
{
    QFont result = m_baseFont;

    if (!m_keyframeManager || m_axisProperties.isEmpty()) {
        return result;
    }

    for (auto it = m_axisProperties.constBegin(); it != m_axisProperties.constEnd(); ++it) {
        const QString &tag = it.key();
        const QString &kfProperty = it.value();

        double value = m_keyframeManager->valueAt(kfProperty, time, 0.0);
        applyAxisValue(result, tag, value);
    }

    return result;
}

QJsonObject VariableFontAxis::toJson() const
{
    QJsonObject obj;

    // Base font
    QJsonObject fontObj;
    fontObj[QStringLiteral("family")] = m_baseFont.family();
    fontObj[QStringLiteral("pointSize")] = m_baseFont.pointSize();
    fontObj[QStringLiteral("weight")] = m_baseFont.weight();
    fontObj[QStringLiteral("italic")] = m_baseFont.italic();
    obj[QStringLiteral("baseFont")] = fontObj;

    // Axis properties mapping
    QJsonObject axisObj;
    for (auto it = m_axisProperties.constBegin(); it != m_axisProperties.constEnd(); ++it) {
        axisObj[it.key()] = it.value();
    }
    obj[QStringLiteral("axisProperties")] = axisObj;

    return obj;
}

void VariableFontAxis::fromJson(const QJsonObject &obj)
{
    // Base font
    if (obj.contains(QStringLiteral("baseFont"))) {
        QJsonObject fontObj = obj[QStringLiteral("baseFont")].toObject();
        QFont font;
        font.setFamily(fontObj[QStringLiteral("family")].toString(QStringLiteral("Arial")));
        font.setPointSize(fontObj[QStringLiteral("pointSize")].toInt(12));
        font.setWeight(static_cast<QFont::Weight>(fontObj[QStringLiteral("weight")].toInt(QFont::Normal)));
        font.setItalic(fontObj[QStringLiteral("italic")].toBool(false));
        m_baseFont = font;
    }

    // Axis properties mapping
    if (obj.contains(QStringLiteral("axisProperties"))) {
        QJsonObject axisObj = obj[QStringLiteral("axisProperties")].toObject();
        m_axisProperties.clear();
        for (auto it = axisObj.constBegin(); it != axisObj.constEnd(); ++it) {
            m_axisProperties.insert(it.key(), it.value().toString());
        }
    }
}
