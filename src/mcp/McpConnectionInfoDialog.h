#pragma once

#include <QDialog>
#include <QString>

class McpConnectionInfoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit McpConnectionInfoDialog(const QString& endpoint,
                                     const QString& token,
                                     QWidget *parent = nullptr);
};
