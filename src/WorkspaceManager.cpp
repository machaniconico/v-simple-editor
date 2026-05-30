// src/WorkspaceManager.cpp
// WS-1: WorkspaceManager 純粋モデル実装。すべて決定的・副作用最小。

#include "WorkspaceManager.h"

#include <QJsonArray>
#include <QJsonValue>

namespace workspace {

int WorkspaceManager::indexOf(const QString& name) const
{
    for (int i = 0; i < m_workspaces.size(); ++i) {
        if (m_workspaces.at(i).name == name)
            return i;
    }
    return -1;
}

bool WorkspaceManager::addOrUpdate(const QString& name,
                                   const QByteArray& windowState,
                                   const QByteArray& geometry)
{
    if (name.isEmpty())
        return false;

    const int idx = indexOf(name);
    if (idx >= 0) {
        m_workspaces[idx].windowState = windowState;
        m_workspaces[idx].geometry    = geometry;
        return true;
    }

    Workspace ws;
    ws.name        = name;
    ws.windowState = windowState;
    ws.geometry    = geometry;
    m_workspaces.append(ws);
    return true;
}

bool WorkspaceManager::remove(const QString& name)
{
    if (name.isEmpty())
        return false;

    const int idx = indexOf(name);
    if (idx < 0)
        return false;

    m_workspaces.removeAt(idx);
    if (m_currentName == name)
        m_currentName.clear();
    return true;
}

bool WorkspaceManager::rename(const QString& oldName, const QString& newName)
{
    if (oldName.isEmpty() || newName.isEmpty())
        return false;

    const int idx = indexOf(oldName);
    if (idx < 0)
        return false;

    // 改名先が (自分以外の) 既存名と重複する場合は失敗。
    if (newName != oldName) {
        const int existing = indexOf(newName);
        if (existing >= 0)
            return false;
    }

    m_workspaces[idx].name = newName;
    if (m_currentName == oldName)
        m_currentName = newName;
    return true;
}

const Workspace* WorkspaceManager::get(const QString& name) const
{
    const int idx = indexOf(name);
    if (idx < 0)
        return nullptr;
    return &m_workspaces.at(idx);
}

QStringList WorkspaceManager::names() const
{
    QStringList out;
    out.reserve(m_workspaces.size());
    for (const Workspace& ws : m_workspaces)
        out.append(ws.name);
    return out;
}

void WorkspaceManager::clear()
{
    m_workspaces.clear();
    m_currentName.clear();
}

QJsonObject WorkspaceManager::toJson() const
{
    QJsonObject root;

    QJsonArray arr;
    for (const Workspace& ws : m_workspaces) {
        QJsonObject obj;
        obj.insert(QStringLiteral("name"), ws.name);
        obj.insert(QStringLiteral("windowState"),
                   QString::fromLatin1(ws.windowState.toBase64()));
        obj.insert(QStringLiteral("geometry"),
                   QString::fromLatin1(ws.geometry.toBase64()));
        arr.append(obj);
    }
    root.insert(QStringLiteral("workspaces"), arr);
    root.insert(QStringLiteral("currentName"), m_currentName);
    return root;
}

void WorkspaceManager::fromJson(const QJsonObject& obj)
{
    clear();

    const QJsonArray arr = obj.value(QStringLiteral("workspaces")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject wsObj = v.toObject();
        const QString name = wsObj.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;

        Workspace ws;
        ws.name = name;
        ws.windowState = QByteArray::fromBase64(
            wsObj.value(QStringLiteral("windowState")).toString().toLatin1());
        ws.geometry = QByteArray::fromBase64(
            wsObj.value(QStringLiteral("geometry")).toString().toLatin1());
        m_workspaces.append(ws);
    }

    m_currentName = obj.value(QStringLiteral("currentName")).toString();
}

void WorkspaceManager::ensureDefaults()
{
    // 既存があれば何もしない (UI 側の blob を温存)。
    if (!m_workspaces.isEmpty())
        return;

    // blob は空のまま名前だけ投入する。実際の blob は UI が後で埋める。
    static const char* const kDefaults[] = {
        "編集",
        "カラー",
        "オーディオ",
        "エフェクト",
    };
    for (const char* name : kDefaults)
        addOrUpdate(QString::fromUtf8(name), QByteArray(), QByteArray());
}

} // namespace workspace
