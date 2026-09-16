#include "ShortcutManager.h"

#include <QAction>
#include <QSettings>

namespace shortcut {

// ---------------------------------------------------------------------------
// Preset binding tables
// ---------------------------------------------------------------------------
QList<QPair<QString, QKeySequence>> ShortcutManager::presetBindingTable(Preset p)
{
    using P = QPair<QString, QKeySequence>;
    QList<P> table;

    switch (p) {
    case Preset::VEditor:
        // VEditor defaults come from registerAction; new operations are below.
        break;

    case Preset::Premiere:
        table << P{"file.open",          QKeySequence("Ctrl+O")}
              << P{"file.save",          QKeySequence("Ctrl+S")}
              << P{"file.save_as",       QKeySequence("Ctrl+Shift+S")}
              << P{"file.export",        QKeySequence("Ctrl+M")}
              << P{"edit.undo",          QKeySequence("Ctrl+Z")}
              << P{"edit.redo",          QKeySequence("Ctrl+Shift+Z")}
              << P{"edit.copy",          QKeySequence("Ctrl+C")}
              << P{"edit.paste",         QKeySequence("Ctrl+V")}
              << P{"edit.cut",           QKeySequence("Ctrl+X")}
              << P{"edit.split",         QKeySequence("Ctrl+K")}
              << P{"edit.delete",        QKeySequence("Delete")}
              << P{"playback.toggle",    QKeySequence("Space")}
              << P{"playback.mark_in",   QKeySequence("I")}
              << P{"playback.mark_out",  QKeySequence("O")}
              << P{"timeline.zoom_in",   QKeySequence("=")}
              << P{"timeline.zoom_out",  QKeySequence("-")}
              << P{"timeline.ripple_delete", QKeySequence("Shift+Delete")}
              << P{"timeline.select_all", QKeySequence("Ctrl+A")}
              << P{"view.full_screen",   QKeySequence("Ctrl+Shift+F")};
        break;

    case Preset::FinalCutPro:
        // Qt normalises Cmd → Ctrl on non-macOS
        table << P{"file.open",          QKeySequence("Ctrl+O")}
              << P{"file.save",          QKeySequence("Ctrl+S")}
              << P{"file.save_as",       QKeySequence("Ctrl+Shift+S")}
              << P{"file.export",        QKeySequence("Ctrl+E")}
              << P{"edit.undo",          QKeySequence("Ctrl+Z")}
              << P{"edit.redo",          QKeySequence("Ctrl+Shift+Z")}
              << P{"edit.copy",          QKeySequence("Ctrl+C")}
              << P{"edit.paste",         QKeySequence("Ctrl+V")}
              << P{"edit.cut",           QKeySequence("Ctrl+X")}
              << P{"edit.split",         QKeySequence("Ctrl+B")}  // Blade
              << P{"edit.delete",        QKeySequence("Delete")}
              << P{"playback.toggle",    QKeySequence("Space")}
              << P{"playback.mark_in",   QKeySequence("I")}
              << P{"playback.mark_out",  QKeySequence("O")}
              << P{"timeline.zoom_in",   QKeySequence("Ctrl+=")}
              << P{"timeline.zoom_out",  QKeySequence("Ctrl+-")}
              << P{"timeline.select_all", QKeySequence("Ctrl+A")}
              << P{"view.full_screen",   QKeySequence("Ctrl+F")}
              << P{"playback.go_to_start", QKeySequence("Home")}
              << P{"playback.go_to_end",   QKeySequence("End")};
        break;

    case Preset::DaVinci:
        table << P{"file.open",          QKeySequence("Ctrl+O")}
              << P{"file.save",          QKeySequence("Ctrl+S")}
              << P{"file.save_as",       QKeySequence("Ctrl+Shift+S")}
              << P{"file.export",        QKeySequence("Ctrl+E")}
              << P{"edit.undo",          QKeySequence("Ctrl+Z")}
              << P{"edit.redo",          QKeySequence("Ctrl+Shift+Z")}
              << P{"edit.copy",          QKeySequence("Ctrl+C")}
              << P{"edit.paste",         QKeySequence("Ctrl+V")}
              << P{"edit.cut",           QKeySequence("Ctrl+X")}
              << P{"edit.split",         QKeySequence("Ctrl+B")}  // Split Clip
              << P{"edit.delete",        QKeySequence("Delete")}
              << P{"playback.toggle",    QKeySequence("Space")}
              << P{"playback.mark_in",   QKeySequence("I")}
              << P{"playback.mark_out",  QKeySequence("O")}
              << P{"timeline.zoom_in",   QKeySequence("Ctrl+=")}
              << P{"timeline.zoom_out",  QKeySequence("Ctrl+-")}
              << P{"timeline.select_all", QKeySequence("Ctrl+A")}
              << P{"view.full_screen",   QKeySequence("Ctrl+F")}
              << P{"timeline.ripple_delete", QKeySequence("Ctrl+Delete")}
              << P{"playback.go_to_start", QKeySequence("Home")}
              << P{"playback.go_to_end",   QKeySequence("End")}
              << P{"edit.select_clip",   QKeySequence("Ctrl+L")};
        break;
    }

    // Common timeline operations are available in every preset.
    table << P{"timeline.jump_timecode", QKeySequence("Ctrl+Shift+G")}
          << P{"timeline.zoom_fit_sequence", QKeySequence()}
          << P{"timeline.zoom_selection", QKeySequence()}
          << P{"timeline.select_forward", QKeySequence()}
          << P{"timeline.select_backward", QKeySequence()}
          << P{"timeline.blade_all", QKeySequence("Ctrl+Shift+K")}
          << P{"timeline.lift", QKeySequence()}
          << P{"timeline.duplicate", QKeySequence("Ctrl+D")}
          << P{"timeline.nudge_left", QKeySequence("Alt+Left")}
          << P{"timeline.nudge_right", QKeySequence("Alt+Right")}
          << P{"timeline.nudge_left10", QKeySequence("Alt+Shift+Left")}
          << P{"timeline.nudge_right10", QKeySequence("Alt+Shift+Right")}
          << P{"timeline.close_all_gaps", QKeySequence()};
    return table;
}

QKeySequence ShortcutManager::presetSequence(Preset p, const QString& actionId)
{
    const auto table = presetBindingTable(p);
    for (const auto& pair : table) {
        if (pair.first == actionId)
            return pair.second;
    }
    return QKeySequence();
}

// ---------------------------------------------------------------------------
// ShortcutManager
// ---------------------------------------------------------------------------
ShortcutManager::ShortcutManager(QObject* parent)
    : QObject(parent)
{
}

void ShortcutManager::registerAction(QAction*       action,
                                     const QString& actionId,
                                     const QString& displayName,
                                     const QString& category)
{
    // Update pointer if already registered
    if (m_actions.contains(actionId)) {
        m_actions[actionId] = action;
        return;
    }

    Binding b;
    b.actionId       = actionId;
    b.displayName    = displayName;
    b.category       = category;
    b.sequence       = action ? action->shortcut() : QKeySequence();
    b.defaultSequence = b.sequence;

    m_bindings.append(b);
    m_actions[actionId] = action;
}

QList<Binding> ShortcutManager::bindings() const
{
    return m_bindings;
}

Binding ShortcutManager::bindingFor(const QString& actionId) const
{
    for (const Binding& b : m_bindings) {
        if (b.actionId == actionId)
            return b;
    }
    return Binding{};
}

void ShortcutManager::setBinding(const QString& actionId, const QKeySequence& seq)
{
    for (Binding& b : m_bindings) {
        if (b.actionId == actionId) {
            b.sequence = seq;
            QPointer<QAction> act = m_actions.value(actionId);
            if (act)
                act->setShortcut(seq);
            emit bindingsChanged();
            return;
        }
    }
}

Preset ShortcutManager::currentPreset() const
{
    return m_currentPreset;
}

void ShortcutManager::applyPreset(Preset p)
{
    m_currentPreset = p;
    applyPresetInternal(p);
    emit bindingsChanged();
}

void ShortcutManager::applyPresetInternal(Preset p)
{
    for (Binding& b : m_bindings) {
        QKeySequence seq;
        if (p == Preset::VEditor) {
            seq = b.defaultSequence;
        } else {
            seq = presetSequence(p, b.actionId);
            if (seq.isEmpty())
                seq = b.defaultSequence;
        }
        b.sequence = seq;
        QPointer<QAction> act = m_actions.value(b.actionId);
        if (act)
            act->setShortcut(seq);
    }
}

QString ShortcutManager::presetDisplayName(Preset p)
{
    switch (p) {
    case Preset::VEditor:    return QStringLiteral("v-simple-editor");
    case Preset::Premiere:   return QStringLiteral("Premiere Pro 風");
    case Preset::FinalCutPro: return QStringLiteral("Final Cut Pro 風");
    case Preset::DaVinci:    return QStringLiteral("DaVinci Resolve 風");
    }
    return QString();
}

QList<Preset> ShortcutManager::availablePresets()
{
    return {Preset::VEditor, Preset::Premiere, Preset::FinalCutPro, Preset::DaVinci};
}

void ShortcutManager::resetAllToDefaults()
{
    applyPreset(Preset::VEditor);
}

void ShortcutManager::loadFromSettings()
{
    QSettings settings(QStringLiteral("VSimpleEditor"), QStringLiteral("Shortcuts"));

    // Restore preset
    const QString presetStr = settings.value(QStringLiteral("preset"),
                                              QStringLiteral("VEditor")).toString();
    Preset p = Preset::VEditor;
    if (presetStr == QLatin1String("Premiere"))
        p = Preset::Premiere;
    else if (presetStr == QLatin1String("FinalCutPro"))
        p = Preset::FinalCutPro;
    else if (presetStr == QLatin1String("DaVinci"))
        p = Preset::DaVinci;
    m_currentPreset = p;

    // Restore individual bindings
    settings.beginGroup(QStringLiteral("bindings"));
    const QStringList keys = settings.childKeys();
    for (const QString& actionId : keys) {
        // Only apply to registered actions
        bool registered = false;
        for (const Binding& b : m_bindings) {
            if (b.actionId == actionId) { registered = true; break; }
        }
        if (!registered)
            continue;

        const QString seqStr = settings.value(actionId).toString();
        QKeySequence seq(seqStr);
        setBinding(actionId, seq);
    }
    settings.endGroup();
}

void ShortcutManager::saveToSettings() const
{
    QSettings settings(QStringLiteral("VSimpleEditor"), QStringLiteral("Shortcuts"));

    // Save preset name
    QString presetStr;
    switch (m_currentPreset) {
    case Preset::VEditor:     presetStr = QStringLiteral("VEditor");     break;
    case Preset::Premiere:    presetStr = QStringLiteral("Premiere");    break;
    case Preset::FinalCutPro: presetStr = QStringLiteral("FinalCutPro"); break;
    case Preset::DaVinci:     presetStr = QStringLiteral("DaVinci");     break;
    }
    settings.setValue(QStringLiteral("preset"), presetStr);

    // Save individual bindings
    settings.beginGroup(QStringLiteral("bindings"));
    for (const Binding& b : m_bindings) {
        settings.setValue(b.actionId, b.sequence.toString());
    }
    settings.endGroup();
}

} // namespace shortcut
