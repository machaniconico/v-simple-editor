#pragma once
#include <QByteArray>
#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;

class ProjectTemplateDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectTemplateDialog(QWidget *parent = nullptr);

signals:
    void projectCreated(const QByteArray &projectJson);

private slots:
    void onSelectionChanged();
    void onCreateClicked();

private:
    QListWidget  *m_list      = nullptr;
    QLabel       *m_preview   = nullptr;
    QPushButton  *m_createBtn = nullptr;
};
