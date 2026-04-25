#pragma once

#include <QDialog>

class QTableWidget;
class QLabel;

class ProxyManagementDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProxyManagementDialog(QWidget *parent = nullptr);

private:
    void refreshTable();

    QTableWidget *m_table = nullptr;
    QLabel *m_totalSizeLabel = nullptr;
};
