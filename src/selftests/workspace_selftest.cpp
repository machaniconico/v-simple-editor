#include <QByteArray>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../WorkspaceManager.h"

// WS-2: WorkspaceManager 純粋モデルの headless selftest。
// QApplication / QSettings / ファイル I/O は一切使わず、CRUD と base64 JSON
// round-trip を決定的に検証する (--selftest=workspace, needsQApplication=false)。

int runWorkspaceSelftest()
{
    qInfo().noquote() << "[workspace] selftest start";
    int passed = 0, failed = 0;
    auto pass = [&](const char* name) { ++passed; qInfo().noquote() << "[workspace] PASS" << name; };
    auto fail = [&](const char* name, const QString& msg) { ++failed; qWarning().noquote() << "[workspace] FAIL" << name << ":" << msg; };

    // 非 ASCII を含むバイナリ blob (saveState() 相当の不透明データを模す)。
    const QByteArray blobA = QByteArray::fromHex("00ff10ab7f80fe01");
    const QByteArray blobB = QByteArray::fromHex("deadbeef0011223344");
    const QByteArray geomA = QByteArray::fromHex("0102fffe7f00");
    const QByteArray geomB = QByteArray::fromHex("aabbccdd");

    // G1: 新規追加が count / names に反映され、get で取得できる。
    {
        workspace::WorkspaceManager m;
        const bool added = m.addOrUpdate(QStringLiteral("編集"), blobA, geomA);
        const workspace::Workspace* w = m.get(QStringLiteral("編集"));
        if (added && m.count() == 1
            && m.names() == QStringList{ QStringLiteral("編集") }
            && w != nullptr && w->name == QStringLiteral("編集")
            && w->windowState == blobA && w->geometry == geomA) {
            pass("G1 addOrUpdate inserts and is retrievable");
        } else {
            fail("G1 addOrUpdate insert",
                 QStringLiteral("added=%1 count=%2 hasGet=%3")
                     .arg(added).arg(m.count()).arg(w != nullptr));
        }
    }

    // G2: 同名 addOrUpdate は上書き (count 増えず blob 更新)。
    {
        workspace::WorkspaceManager m;
        m.addOrUpdate(QStringLiteral("編集"), blobA, geomA);
        const bool updated = m.addOrUpdate(QStringLiteral("編集"), blobB, geomB);
        const workspace::Workspace* w = m.get(QStringLiteral("編集"));
        if (updated && m.count() == 1
            && w != nullptr && w->windowState == blobB && w->geometry == geomB) {
            pass("G2 addOrUpdate same name overwrites without growth");
        } else {
            fail("G2 addOrUpdate overwrite",
                 QStringLiteral("updated=%1 count=%2").arg(updated).arg(m.count()));
        }
    }

    // G3: 空名の addOrUpdate は false (副作用なし)。
    {
        workspace::WorkspaceManager m;
        const bool r = m.addOrUpdate(QString(), blobA, geomA);
        if (!r && m.count() == 0) {
            pass("G3 empty-name addOrUpdate returns false");
        } else {
            fail("G3 empty name", QStringLiteral("r=%1 count=%2").arg(r).arg(m.count()));
        }
    }

    // G4: remove で消える、不在 remove は false。
    {
        workspace::WorkspaceManager m;
        m.addOrUpdate(QStringLiteral("編集"), blobA, geomA);
        m.addOrUpdate(QStringLiteral("カラー"), blobB, geomB);
        const bool removed = m.remove(QStringLiteral("編集"));
        const bool removeMissing = m.remove(QStringLiteral("存在しない"));
        if (removed && !removeMissing && m.count() == 1
            && m.get(QStringLiteral("編集")) == nullptr
            && m.get(QStringLiteral("カラー")) != nullptr) {
            pass("G4 remove deletes present, false on missing");
        } else {
            fail("G4 remove",
                 QStringLiteral("removed=%1 removeMissing=%2 count=%3")
                     .arg(removed).arg(removeMissing).arg(m.count()));
        }
    }

    // G5: rename で名前が変わる、newName 重複 / 空は false。
    {
        workspace::WorkspaceManager m;
        m.addOrUpdate(QStringLiteral("編集"), blobA, geomA);
        m.addOrUpdate(QStringLiteral("カラー"), blobB, geomB);
        const bool ok = m.rename(QStringLiteral("編集"), QStringLiteral("配色"));
        const bool dup = m.rename(QStringLiteral("配色"), QStringLiteral("カラー"));
        const bool empty = m.rename(QStringLiteral("配色"), QString());
        const workspace::Workspace* renamed = m.get(QStringLiteral("配色"));
        if (ok && !dup && !empty
            && renamed != nullptr && renamed->windowState == blobA
            && m.get(QStringLiteral("編集")) == nullptr) {
            pass("G5 rename succeeds; duplicate/empty newName rejected");
        } else {
            fail("G5 rename",
                 QStringLiteral("ok=%1 dup=%2 empty=%3 hasRenamed=%4")
                     .arg(ok).arg(dup).arg(empty).arg(renamed != nullptr));
        }
    }

    // G6: get(不在) は nullptr。
    {
        workspace::WorkspaceManager m;
        m.addOrUpdate(QStringLiteral("編集"), blobA, geomA);
        if (m.get(QStringLiteral("無い")) == nullptr && m.get(QString()) == nullptr) {
            pass("G6 get on missing name returns nullptr");
        } else {
            fail("G6 get missing", QStringLiteral("non-null returned"));
        }
    }

    // G7: setCurrent/currentName が反映、remove で current が消えたときに安全。
    {
        workspace::WorkspaceManager m;
        m.addOrUpdate(QStringLiteral("編集"), blobA, geomA);
        m.addOrUpdate(QStringLiteral("カラー"), blobB, geomB);
        m.setCurrent(QStringLiteral("編集"));
        const bool setOk = m.currentName() == QStringLiteral("編集");
        m.remove(QStringLiteral("編集"));
        const bool clearedOnRemove = m.currentName().isEmpty();
        m.setCurrent(QStringLiteral("カラー"));
        m.remove(QStringLiteral("存在しない"));
        const bool keptOnOtherRemove = m.currentName() == QStringLiteral("カラー");
        if (setOk && clearedOnRemove && keptOnOtherRemove) {
            pass("G7 setCurrent/currentName reflect; remove clears current safely");
        } else {
            fail("G7 current tracking",
                 QStringLiteral("setOk=%1 clearedOnRemove=%2 keptOnOtherRemove=%3")
                     .arg(setOk).arg(clearedOnRemove).arg(keptOnOtherRemove));
        }
    }

    // G8: 非 ASCII バイナリの windowState / geometry が get で完全一致保持。
    {
        workspace::WorkspaceManager m;
        m.addOrUpdate(QStringLiteral("バイナリ"), blobA, geomA);
        const workspace::Workspace* w = m.get(QStringLiteral("バイナリ"));
        if (w != nullptr
            && w->windowState == blobA && w->windowState.size() == blobA.size()
            && w->geometry == geomA && w->geometry.size() == geomA.size()) {
            pass("G8 binary blobs preserved byte-exact");
        } else {
            fail("G8 binary preservation", QStringLiteral("mismatch in stored blob"));
        }
    }

    // G9: toJson/fromJson round-trip。件数 + 各 name + blob (base64 経由) + currentName 完全一致。
    {
        workspace::WorkspaceManager src;
        src.addOrUpdate(QStringLiteral("編集"), blobA, geomA);
        src.addOrUpdate(QStringLiteral("カラー"), blobB, geomB);
        src.addOrUpdate(QStringLiteral("空白"), QByteArray(), QByteArray());
        src.setCurrent(QStringLiteral("カラー"));

        const QJsonObject json = src.toJson();
        workspace::WorkspaceManager dst;
        dst.fromJson(json);

        bool ok = dst.count() == src.count()
            && dst.names() == src.names()
            && dst.currentName() == src.currentName();
        for (int i = 0; ok && i < src.count(); ++i) {
            const workspace::Workspace& a = src.workspaces().at(i);
            const workspace::Workspace& b = dst.workspaces().at(i);
            ok = a.name == b.name
                && a.windowState == b.windowState
                && a.geometry == b.geometry;
        }
        if (ok) {
            pass("G9 toJson/fromJson round-trips names, base64 blobs, currentName");
        } else {
            fail("G9 json round-trip",
                 QStringLiteral("srcCount=%1 dstCount=%2 srcCur=%3 dstCur=%4")
                     .arg(src.count()).arg(dst.count())
                     .arg(src.currentName()).arg(dst.currentName()));
        }
    }

    // G10: clear で空に。
    {
        workspace::WorkspaceManager m;
        m.addOrUpdate(QStringLiteral("編集"), blobA, geomA);
        m.setCurrent(QStringLiteral("編集"));
        m.clear();
        if (m.count() == 0 && m.names().isEmpty() && m.currentName().isEmpty()
            && m.get(QStringLiteral("編集")) == nullptr) {
            pass("G10 clear empties workspaces and currentName");
        } else {
            fail("G10 clear",
                 QStringLiteral("count=%1 cur=%2").arg(m.count()).arg(m.currentName()));
        }
    }

    qInfo().noquote().nospace() << "[workspace] selftest end, passed=" << passed << " failed=" << failed;
    return failed == 0 ? 0 : 1;
}
