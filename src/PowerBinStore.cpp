#include "PowerBinStore.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QUuid>
#include <QVBoxLayout>

namespace powerbin {

namespace {

QJsonArray serializeAssets(const QVector<AssetEntry> &assets)
{
    QJsonArray arr;
    for (const auto &a : assets) {
        QJsonObject o;
        o[QStringLiteral("id")] = a.id;
        o[QStringLiteral("name")] = a.name;
        o[QStringLiteral("filePath")] = a.filePath;
        o[QStringLiteral("category")] = a.category;
        if (!a.notes.isEmpty())
            o[QStringLiteral("notes")] = a.notes;
        arr.append(o);
    }
    return arr;
}

QVector<AssetEntry> deserializeAssets(const QJsonArray &arr)
{
    QVector<AssetEntry> out;
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        AssetEntry a;
        a.id = o.value(QStringLiteral("id")).toString();
        a.name = o.value(QStringLiteral("name")).toString();
        a.filePath = o.value(QStringLiteral("filePath")).toString();
        a.category = o.value(QStringLiteral("category")).toString(QStringLiteral("お気に入り"));
        a.notes = o.value(QStringLiteral("notes")).toString();
        if (a.id.isEmpty())
            a.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!a.filePath.isEmpty())
            out.append(a);
    }
    return out;
}

} // namespace

QStringList defaultCategories()
{
    return {
        QStringLiteral("お気に入り"),
        QStringLiteral("イントロ/アウトロ"),
        QStringLiteral("BGM/効果音"),
        QStringLiteral("ロゴ/素材"),
        QStringLiteral("外部ジェネレータ")
    };
}

QVector<AssetEntry> loadAssets(const QString &overrideJsonPath)
{
    if (!overrideJsonPath.isEmpty()) {
        QFile f(overrideJsonPath);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        return doc.isArray() ? deserializeAssets(doc.array()) : QVector<AssetEntry>{};
    }
    QSettings s;
    s.beginGroup(QStringLiteral("PowerBins"));
    const QByteArray raw = s.value(QStringLiteral("AssetsJson")).toByteArray();
    s.endGroup();
    if (raw.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    return doc.isArray() ? deserializeAssets(doc.array()) : QVector<AssetEntry>{};
}

bool saveAssets(const QVector<AssetEntry> &assets, const QString &overrideJsonPath)
{
    const QByteArray payload = QJsonDocument(serializeAssets(assets)).toJson(QJsonDocument::Indented);
    if (!overrideJsonPath.isEmpty()) {
        QDir().mkpath(QFileInfo(overrideJsonPath).absolutePath());
        QFile f(overrideJsonPath);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        return f.write(payload) == payload.size();
    }
    QSettings s;
    s.beginGroup(QStringLiteral("PowerBins"));
    s.setValue(QStringLiteral("AssetsJson"), payload);
    s.sync();
    s.endGroup();
    return true;
}

AssetEntry addAsset(const QString &filePath,
                    const QString &category,
                    const QString &displayName,
                    const QString &overrideJsonPath)
{
    QVector<AssetEntry> assets = loadAssets(overrideJsonPath);
    AssetEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.filePath = filePath;
    entry.name = displayName.trimmed().isEmpty() ? QFileInfo(filePath).fileName() : displayName.trimmed();
    entry.category = category.trimmed().isEmpty() ? QStringLiteral("お気に入り") : category.trimmed();
    assets.append(entry);
    saveAssets(assets, overrideJsonPath);
    return entry;
}

bool removeAsset(const QString &assetId, const QString &overrideJsonPath)
{
    QVector<AssetEntry> assets = loadAssets(overrideJsonPath);
    const int before = assets.size();
    QVector<AssetEntry> kept;
    for (const auto &a : assets) {
        if (a.id != assetId)
            kept.append(a);
    }
    if (kept.size() == before)
        return false;
    return saveAssets(kept, overrideJsonPath);
}

QVector<AssetEntry> filterAssets(const QVector<AssetEntry> &assets,
                                 const QString &category,
                                 const QString &searchQuery)
{
    const QString q = searchQuery.trimmed().toLower();
    const bool allCat = category.isEmpty() || category == QStringLiteral("すべて");
    QVector<AssetEntry> out;
    for (const auto &a : assets) {
        if (!allCat && a.category != category)
            continue;
        if (!q.isEmpty()
            && !a.name.toLower().contains(q)
            && !a.filePath.toLower().contains(q)
            && !a.category.toLower().contains(q)) {
            continue;
        }
        out.append(a);
    }
    return out;
}

PowerBinDialog::PowerBinDialog(QWidget *parent, const QString &overrideJsonPath)
    : QDialog(parent)
    , m_overridePath(overrideJsonPath)
{
    setWindowTitle(QStringLiteral("パワービン (プロジェクト横断共有アセット)"));
    resize(560, 420);

    auto *root = new QVBoxLayout(this);
    auto *topRow = new QHBoxLayout();
    m_categoryCombo = new QComboBox(this);
    m_categoryCombo->setObjectName(QStringLiteral("powerBinCategoryCombo"));
    m_categoryCombo->addItem(QStringLiteral("すべて"));
    for (const QString &c : defaultCategories())
        m_categoryCombo->addItem(c);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(QStringLiteral("powerBinSearchEdit"));
    m_searchEdit->setPlaceholderText(QStringLiteral("アセット名・パスで絞り込み…"));

    topRow->addWidget(new QLabel(QStringLiteral("カテゴリ:"), this));
    topRow->addWidget(m_categoryCombo);
    topRow->addWidget(m_searchEdit, 1);
    root->addLayout(topRow);

    m_listWidget = new QListWidget(this);
    m_listWidget->setObjectName(QStringLiteral("powerBinList"));
    root->addWidget(m_listWidget, 1);

    auto *btnRow = new QHBoxLayout();
    auto *addBtn = new QPushButton(QStringLiteral("ファイルを追加…"), this);
    auto *removeBtn = new QPushButton(QStringLiteral("削除"), this);
    auto *insertBtn = new QPushButton(QStringLiteral("タイムラインへ挿入"), this);
    insertBtn->setObjectName(QStringLiteral("powerBinInsertButton"));
    auto *closeBtn = new QPushButton(QStringLiteral("閉じる"), this);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(removeBtn);
    btnRow->addStretch();
    btnRow->addWidget(insertBtn);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(m_categoryCombo, &QComboBox::currentTextChanged, this, &PowerBinDialog::reloadList);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &PowerBinDialog::reloadList);
    connect(addBtn, &QPushButton::clicked, this, &PowerBinDialog::onAddFiles);
    connect(removeBtn, &QPushButton::clicked, this, &PowerBinDialog::onRemoveSelected);
    connect(insertBtn, &QPushButton::clicked, this, &PowerBinDialog::onInsertSelected);
    connect(m_listWidget, &QListWidget::itemDoubleClicked, this, &PowerBinDialog::onInsertSelected);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    reloadList();
}

void PowerBinDialog::reloadList()
{
    m_assets = loadAssets(m_overridePath);
    const QVector<AssetEntry> filtered = filterAssets(
        m_assets, m_categoryCombo->currentText(), m_searchEdit->text());
    m_listWidget->clear();
    for (const auto &a : filtered) {
        const bool exists = QFileInfo::exists(a.filePath)
            || a.filePath.startsWith(QStringLiteral("veditor://"));
        const QString label = QStringLiteral("[%1] %2%3  —  %4")
            .arg(a.category, a.name, exists ? QString() : QStringLiteral(" (オフライン)"), a.filePath);
        auto *item = new QListWidgetItem(label, m_listWidget);
        item->setData(Qt::UserRole, a.id);
        item->setData(Qt::UserRole + 1, a.filePath);
    }
}

void PowerBinDialog::onAddFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("パワービンへ共有アセットを追加"));
    if (files.isEmpty())
        return;
    const QString cat = m_categoryCombo->currentText() == QStringLiteral("すべて")
        ? QStringLiteral("お気に入り")
        : m_categoryCombo->currentText();
    for (const QString &f : files)
        addAsset(f, cat, QString(), m_overridePath);
    reloadList();
}

void PowerBinDialog::onRemoveSelected()
{
    auto *item = m_listWidget->currentItem();
    if (!item) return;
    removeAsset(item->data(Qt::UserRole).toString(), m_overridePath);
    reloadList();
}

void PowerBinDialog::onInsertSelected()
{
    auto *item = m_listWidget->currentItem();
    if (!item) return;
    m_selectedFilePath = item->data(Qt::UserRole + 1).toString();
    emit insertAssetRequested(m_selectedFilePath);
    accept();
}

} // namespace powerbin
