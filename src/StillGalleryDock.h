#pragma once

#include <QDockWidget>
#include <QImage>

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QSlider;

namespace stillstore { class StillStore; }

class StillGalleryDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit StillGalleryDock(QWidget *parent = nullptr);

    void setStore(stillstore::StillStore *store);
    void refresh();
    void selectStill(const QString &id);

signals:
    void stillSelected(const QString &id, const QImage &image);
    void stillRemoved(const QString &id);
    void comparisonModeChanged(int modeIndex);
    void comparisonPositionChanged(double position);

private slots:
    void onItemDoubleClicked(QListWidgetItem *item);
    void showItemMenu(const QPoint &position);

private:
    QString idForItem(const QListWidgetItem *item) const;
    void updatePositionLabel(int value);

    stillstore::StillStore *m_store = nullptr;
    QListWidget *m_list = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QSlider *m_positionSlider = nullptr;
    QLabel *m_positionLabel = nullptr;
};
