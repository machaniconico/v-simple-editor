#pragma once

#include <QString>
#include <QStringList>

class TextAnimator;

class TextAnimPresets
{
public:
    static QStringList presetNames();
    static bool applyPreset(TextAnimator *animator, const QString &presetName);
    static QString presetDescription(const QString &presetName);
};
