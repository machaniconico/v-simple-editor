#pragma once

#include <QDialog>

#include "CommandSearch.h"

class QLineEdit;
class QListWidget;

class CommandPaletteDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CommandPaletteDialog(QWidget* parent = nullptr);

    void setCommands(const QVector<cmdsearch::CommandEntry>& commands);

signals:
    void commandTriggered(const QString& id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onSearchTextChanged(const QString& text);
    void acceptCurrentItem();

private:
    void rebuildList(const QVector<int>& order);

    QLineEdit* m_searchEdit = nullptr;
    QListWidget* m_listWidget = nullptr;
    QVector<cmdsearch::CommandEntry> m_commands;
    bool m_accepting = false;  // re-entrancy guard (double-click fires activated+doubleClicked)
};
