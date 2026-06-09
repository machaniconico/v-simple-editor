#include "../CaptionStyle.h"

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QVector>

#include <cstdio>

namespace {

bool styleEquals(const caption::Style& lhs, const caption::Style& rhs)
{
    return lhs.fontFamily == rhs.fontFamily
        && lhs.fontSizePt == rhs.fontSizePt
        && lhs.bold == rhs.bold
        && lhs.italic == rhs.italic
        && lhs.textColor == rhs.textColor
        && lhs.outlineColor == rhs.outlineColor
        && lhs.outlineThickness == rhs.outlineThickness
        && lhs.shadowColor == rhs.shadowColor
        && lhs.shadowOffset == rhs.shadowOffset
        && lhs.background == rhs.background
        && lhs.backgroundColor == rhs.backgroundColor
        && lhs.anchor == rhs.anchor
        && lhs.anchorOffsetNormalized == rhs.anchorOffsetNormalized
        && lhs.maxWidthNormalized == rhs.maxWidthNormalized;
}

const caption::Style* styleByName(const QVector<caption::StylePreset>& presets, const QString& name)
{
    for (const caption::StylePreset& preset : presets) {
        if (preset.displayName == name)
            return &preset.style;
    }
    return nullptr;
}

void printGate(const char* gate, bool ok, const QString& reason, int& passed, int& failed)
{
    if (ok) {
        std::printf("%s: PASS\n", gate);
        ++passed;
        return;
    }

    const QByteArray reasonUtf8 = reason.toUtf8();
    std::printf("%s: FAIL - %s\n", gate, reasonUtf8.constData());
    ++failed;
}

} // namespace

int runCapcutCaptionSelftest()
{
    int passed = 0;
    int failed = 0;

    const QVector<caption::StylePreset> presets = caption::capCutStylePresets();

    bool validPresetShape = presets.size() == 5;
    for (const caption::StylePreset& preset : presets) {
        validPresetShape = validPresetShape
            && !preset.displayName.trimmed().isEmpty()
            && preset.style.fontSizePt > 0;
    }
    printGate("G1",
              validPresetShape,
              QStringLiteral("expected exactly 5 presets with names and positive fontSizePt"),
              passed,
              failed);

    QSet<QString> names;
    for (const caption::StylePreset& preset : presets)
        names.insert(preset.displayName);
    printGate("G2",
              names.size() == presets.size(),
              QStringLiteral("display names must be unique"),
              passed,
              failed);

    QSet<QString> textColors;
    QSet<int> fontSizes;
    QSet<int> anchors;
    for (const caption::StylePreset& preset : presets) {
        textColors.insert(preset.style.textColor.name(QColor::HexArgb));
        fontSizes.insert(preset.style.fontSizePt);
        anchors.insert(static_cast<int>(preset.style.anchor));
    }
    printGate("G3",
              textColors.size() >= 3 || fontSizes.size() >= 3 || anchors.size() >= 3,
              QStringLiteral("expected at least 3 distinct values across textColor, fontSizePt, or anchor"),
              passed,
              failed);

    const caption::Style* boxBlack = styleByName(presets, QStringLiteral("ボックス・ブラック"));
    const caption::Style* popWhite = styleByName(presets, QStringLiteral("ポップ・ホワイト"));
    printGate("G4",
              boxBlack != nullptr
                  && popWhite != nullptr
                  && boxBlack->background
                  && boxBlack->backgroundColor.alpha() > 0
                  && (!popWhite->background || popWhite->backgroundColor.alpha() == 0),
              QStringLiteral("expected ボックス・ブラック background and ポップ・ホワイト no background"),
              passed,
              failed);

    const caption::Style defaults = caption::defaultStyle();
    bool defaultMatchesPreset = false;
    for (const caption::StylePreset& preset : presets) {
        if (styleEquals(defaults, preset.style)) {
            defaultMatchesPreset = true;
            break;
        }
    }
    printGate("G5",
              !defaultMatchesPreset,
              QStringLiteral("default caption::Style must not equal any preset"),
              passed,
              failed);

    std::printf("[capcut-caption] summary passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
