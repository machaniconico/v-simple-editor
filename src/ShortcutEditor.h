#pragma once

#include <QDialog>
#include <QAction>
#include <QKeySequence>
#include <QString>
#include <QVector>
#include <QMap>
#include <QTableWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

struct ShortcutEntry {
    QString id;
    QString displayName;
    QKeySequence defaultKey;
    QKeySequence currentKey;
    QAction *action;
};

class ShortcutManager
{
public:
    static ShortcutManager &instance();

    void registerShortcut(const QString &id, const QString &displayName,
                          const QKeySequence &defaultKeySequence, QAction *action);

    void resetToDefaults();
    void saveShortcuts();
    void loadShortcuts();

    QKeySequence shortcutForId(const QString &id) const;
    QVector<ShortcutEntry> allShortcuts() const;

    // Apply a new key sequence to a registered shortcut
    void applyShortcut(const QString &id, const QKeySequence &key);

private:
    ShortcutManager() = default;

    QMap<QString, ShortcutEntry> m_shortcuts;
};

class ShortcutEditorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ShortcutEditorDialog(QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onSearchChanged(const QString &text);
    void onCellClicked(int row, int column);
    void onResetRow();
    void onResetAll();
    void onApply();

private:
    void setupUI();
    void populateTable(const QString &filter = QString());
    void startCapture(int row);
    void stopCapture();
    void applyConflictCheck(const QString &id, const QKeySequence &key, int row);
    QString rowId(int row) const;

    QLineEdit *m_searchEdit;
    QTableWidget *m_table;
    QLabel *m_hintLabel;
    QPushButton *m_resetRowBtn;
    QPushButton *m_resetAllBtn;
    QPushButton *m_applyBtn;

    int m_captureRow = -1;
    bool m_capturing = false;
};
