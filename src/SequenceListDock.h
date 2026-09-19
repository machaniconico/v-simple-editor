#pragma once

#include <QDockWidget>
#include <QPointer>

class Timeline;
class QListWidget;

class SequenceListDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit SequenceListDock(Timeline *timeline, QWidget *parent = nullptr);

private:
    void refresh();
    QPointer<Timeline> m_timeline;
    QListWidget *m_list = nullptr;
};
