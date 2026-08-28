#include "UndoManager.h"

UndoManager::UndoManager(QObject *parent)
    : QObject(parent)
{
}

void UndoManager::saveState(const TimelineState &state, const QString &description)
{
    m_redoStack.clear();
    m_undoStack.push({state, description});
    if (m_undoStack.size() > MAX_UNDO)
        m_undoStack.removeFirst();
    ++m_saveSerial;
    emit stateChanged();
    emit historyChanged();
}

TimelineState UndoManager::undo()
{
    if (!canUndo()) return m_undoStack.top().state;
    m_redoStack.push(m_undoStack.pop());
    emit stateChanged();
    emit historyChanged();
    return m_undoStack.top().state;
}

TimelineState UndoManager::redo()
{
    if (!canRedo()) return m_undoStack.top().state;
    m_undoStack.push(m_redoStack.pop());
    emit stateChanged();
    emit historyChanged();
    return m_undoStack.top().state;
}

QString UndoManager::undoDescription() const
{
    if (!canUndo()) return QString();
    return m_undoStack.top().description;
}

QString UndoManager::redoDescription() const
{
    if (!canRedo()) return QString();
    return m_redoStack.top().description;
}

QStringList UndoManager::historyDescriptions() const
{
    QStringList list;
    for (const auto &entry : m_undoStack)
        list.append(entry.description);
    for (int i = m_redoStack.size() - 1; i >= 0; --i)
        list.append(m_redoStack.at(i).description);
    return list;
}

bool UndoManager::jumpTo(int index)
{
    const int total = m_undoStack.size() + m_redoStack.size();
    if (index < 0 || index >= total)
        return false;

    const int current = currentIndex();
    if (index == current)
        return true;

    const int diff = index - current;
    if (diff < 0) {
        for (int i = 0; i < -diff; ++i) {
            if (!canUndo())
                return false;
            undo();
        }
    } else {
        for (int i = 0; i < diff; ++i) {
            if (!canRedo())
                return false;
            redo();
        }
    }
    if (!m_undoStack.isEmpty())
        emit stateJumpRequested(m_undoStack.top().state);
    return true;
}

void UndoManager::clear()
{
    m_undoStack.clear();
    m_redoStack.clear();
    emit stateChanged();
    emit historyChanged();
}
