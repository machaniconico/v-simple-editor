#pragma once

// src/WorkspaceManager.h
// WS-1: WorkspaceManager 純粋モデル。名前付きワークスペース (ドックレイアウトの
// 保存・切替、Premiere/Resolve のワークスペース相当) のデータモデル SSOT。
//
// 状態 blob は QMainWindow::saveState() / saveGeometry() が返す QByteArray を
// そのまま保持するだけで、実際の save / restore は UI 側 (WS-3) が担う。本クラスは
// QObject / QWidget を一切持たない純粋な値クラスで、QApplication 不要・決定的。
// headless 単体テスト可能 (--selftest=workspace, needsQApplication=false)。
//
// 設計方針:
//   - namespace workspace。QObject 非継承、デフォルトコピー / ムーブ可能、singleton 不使用。
//   - 空名 / 不在 / 重複は安全に false を返す (副作用なし)。
//   - JSON は windowState / geometry を base64 文字列で round-trip し、currentName も保存する。

#include <QString>
#include <QStringList>
#include <QVector>
#include <QByteArray>
#include <QJsonObject>

namespace workspace {

// 1 つの名前付きワークスペース。windowState / geometry は UI 側で
// QMainWindow::saveState() / saveGeometry() が返す不透明 blob をそのまま保持する。
struct Workspace {
    QString    name;
    QByteArray windowState;
    QByteArray geometry;
};

// 名前付きワークスペースの集合を管理する純粋モデル。
// 挿入順を保持し (QVector)、同名は上書き、新規は末尾追加する。
class WorkspaceManager {
public:
    WorkspaceManager() = default;

    // 同名が既にあれば上書き、無ければ追加。空名は false。
    bool addOrUpdate(const QString& name, const QByteArray& windowState, const QByteArray& geometry);

    // 指定名のワークスペースを削除。不在 / 空名は false。
    // 削除した名前が currentName と一致する場合は currentName をクリアする。
    bool remove(const QString& name);

    // oldName を newName に改名。oldName 不在 / 空名、newName 空、
    // newName が (oldName 以外の) 既存と重複する場合は false。
    // currentName が oldName と一致していれば newName に追従させる。
    bool rename(const QString& oldName, const QString& newName);

    // 指定名のワークスペースを返す。無ければ nullptr。
    const Workspace* get(const QString& name) const;

    // 全ワークスペース名を挿入順で返す。
    QStringList names() const;

    // 全ワークスペースを挿入順で返す。
    const QVector<Workspace>& workspaces() const { return m_workspaces; }

    int count() const { return m_workspaces.size(); }

    // 現在選択中ワークスペース名 (UI のアクティブレイアウト)。
    QString currentName() const { return m_currentName; }
    void    setCurrent(const QString& name) { m_currentName = name; }

    // 全削除 (currentName も含む)。
    void clear();

    // windowState / geometry を base64 文字列で round-trip。currentName も保存。
    QJsonObject toJson() const;
    void        fromJson(const QJsonObject& obj);

    // 既定ワークスペース投入ヘルパ。blob は空のまま名前だけ用意する
    // (実際の blob は UI が後で addOrUpdate で埋める)。既存があれば何もしない。
    void ensureDefaults();

private:
    int indexOf(const QString& name) const;

    QVector<Workspace> m_workspaces;
    QString            m_currentName;
};

} // namespace workspace
