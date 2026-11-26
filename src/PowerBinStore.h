#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

class QComboBox;
class QLineEdit;
class QListWidget;

namespace powerbin {

struct AssetEntry {
    QString id;
    QString name;
    QString filePath;
    QString category; // e.g. "イントロ/アウトロ", "BGM/効果音", "ロゴ/素材", "お気に入り"
    QString notes;
};

QStringList defaultCategories();

// Persistent cross-project Power Bin store (DaVinci Resolve Power Bins equivalent).
// Stored as JSON in QSettings (or custom path override for deterministic testing).
QVector<AssetEntry> loadAssets(const QString &overrideJsonPath = {});
bool saveAssets(const QVector<AssetEntry> &assets, const QString &overrideJsonPath = {});
AssetEntry addAsset(const QString &filePath,
                    const QString &category = QStringLiteral("お気に入り"),
                    const QString &displayName = {},
                    const QString &overrideJsonPath = {});
bool removeAsset(const QString &assetId, const QString &overrideJsonPath = {});
QVector<AssetEntry> filterAssets(const QVector<AssetEntry> &assets,
                                 const QString &category,
                                 const QString &searchQuery);

// Dialog / Browser UI for managing and inserting Power Bin assets into any project.
class PowerBinDialog : public QDialog {
    Q_OBJECT
public:
    explicit PowerBinDialog(QWidget *parent = nullptr,
                            const QString &overrideJsonPath = {});

    QString selectedFilePath() const { return m_selectedFilePath; }

signals:
    void insertAssetRequested(const QString &filePath);

private:
    void reloadList();
    void onAddFiles();
    void onRemoveSelected();
    void onInsertSelected();

    QString m_overridePath;
    QString m_selectedFilePath;
    QVector<AssetEntry> m_assets;
    QComboBox *m_categoryCombo = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_listWidget = nullptr;
};

} // namespace powerbin
