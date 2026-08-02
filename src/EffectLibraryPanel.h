#pragma once

#include "EffectLibraryModel.h"

#include <QDockWidget>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

class EffectLibraryPanel : public QDockWidget
{
    Q_OBJECT

public:
    explicit EffectLibraryPanel(QWidget *parent = nullptr);

    efxlib::EffectLibraryModel &model() { return m_model; }
    const efxlib::EffectLibraryModel &model() const { return m_model; }

    QString selectedEntryId() const { return m_selectedId; }
    bool previewEnabled() const;
    void setThumbnailSource(const QImage &source);
    void refreshCatalog();

signals:
    void applyRequested(const QString &id);
    void previewRequested(const QString &id, bool enabled);
    void keyframeRequested(const QString &id, const QString &paramName);
    void savePresetRequested();
    void renamePresetRequested(const QString &id);
    void deletePresetRequested(const QString &id);

private slots:
    void refreshItems();
    void scheduleVisibleThumbnails();
    void refreshVisibleThumbnails();
    void selectionChanged();
    void previewToggled(bool enabled);
    void showContextMenu(const QPoint &position);

private:
    void buildInspector();
    void clearInspector();
    void updateFilterCount();

    efxlib::EffectLibraryModel m_model;
    QListWidget *m_grid = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_category = nullptr;
    QCheckBox *m_favoritesOnly = nullptr;
    QCheckBox *m_preview = nullptr;
    QLabel *m_filterCount = nullptr;
    QWidget *m_footageEmptyState = nullptr;
    QPushButton *m_openFootageFolder = nullptr;
    QScrollArea *m_inspectorScroll = nullptr;
    QWidget *m_inspectorWidget = nullptr;
    QVBoxLayout *m_inspectorLayout = nullptr;
    QImage m_thumbnailSource;
    qint64 m_thumbnailSourceKey = 0;
    QHash<QString, QPixmap> m_thumbnailCache;
    QString m_selectedId;
};
