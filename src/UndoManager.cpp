#include "UndoManager.h"

UndoManager::UndoManager(QObject *parent)
    : QObject(parent)
{
}

void UndoManager::saveState(const TimelineState &state, const QString &description)
{
    const bool wasClean = isClean();
    m_redoStack.clear();
    const quint64 id = ++m_saveSerial;
    m_undoStack.push({state, description, id});
    if (m_undoStack.size() > MAX_UNDO)
        m_undoStack.removeFirst();
    emit stateChanged();
    emit historyChanged();
    if (isClean() != wasClean)
        emit cleanChanged();
}

TimelineState UndoManager::undo()
{
    if (!canUndo()) return m_undoStack.top().state;
    const bool wasClean = isClean();
    m_redoStack.push(m_undoStack.pop());
    emit stateChanged();
    emit historyChanged();
    if (isClean() != wasClean)
        emit cleanChanged();
    return m_undoStack.top().state;
}

TimelineState UndoManager::redo()
{
    if (!canRedo()) return m_undoStack.top().state;
    const bool wasClean = isClean();
    m_undoStack.push(m_redoStack.pop());
    emit stateChanged();
    emit historyChanged();
    if (isClean() != wasClean)
        emit cleanChanged();
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
    const bool wasClean = isClean();
    m_undoStack.clear();
    m_redoStack.clear();
    m_cleanEntryId = 0;
    emit stateChanged();
    emit historyChanged();
    if (isClean() != wasClean)
        emit cleanChanged();
}

void UndoManager::markClean()
{
    const bool wasClean = isClean();
    m_cleanEntryId = m_undoStack.isEmpty() ? 0 : m_undoStack.top().id;
    if (isClean() != wasClean)
        emit cleanChanged();
}

bool UndoManager::isClean() const
{
    return !m_undoStack.isEmpty() && m_undoStack.top().id == m_cleanEntryId;
}

void UndoManager::updateCurrentExternalTrackState(const QJsonObject &state)
{
    if (!m_undoStack.isEmpty()) m_undoStack.top().state.externalTrackState = state;
}

void UndoManager::updateCurrentProjectOutput(bool explicitOutput)
{
    if (!m_undoStack.isEmpty()) m_undoStack.top().state.projectExplicitOutput = explicitOutput;
}

void UndoManager::updateCurrentState(const TimelineState &state)
{
    if (m_undoStack.isEmpty()) return;
    const bool wasClean = isClean();
    // The replaced content is no longer what was saved: the clean entry becomes unreachable.
    if (wasClean)
        m_cleanEntryId = 0;
    m_undoStack.top().state = state;
    if (!m_redoStack.isEmpty()) {
        m_redoStack.clear();
        emit stateChanged();
        emit historyChanged();
    }
    if (isClean() != wasClean)
        emit cleanChanged();
}
