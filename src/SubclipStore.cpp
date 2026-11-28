#include "SubclipStore.h"
#include "Timeline.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
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

#include <cmath>

namespace subclips {

namespace {

QJsonArray serialize(const QVector<SubclipEntry> &list)
{
    QJsonArray arr;
    for (const auto &s : list) {
        QJsonObject o;
        o[QStringLiteral("id")] = s.id;
        o[QStringLiteral("name")] = s.name;
        o[QStringLiteral("sourcePath")] = s.sourcePath;
        o[QStringLiteral("sourceDuration")] = s.sourceDuration;
        o[QStringLiteral("inPoint")] = s.inPoint;
        o[QStringLiteral("outPoint")] = s.outPoint;
        if (!s.notes.isEmpty())
            o[QStringLiteral("notes")] = s.notes;
        arr.append(o);
    }
    return arr;
}

QVector<SubclipEntry> deserialize(const QJsonArray &arr)
{
    QVector<SubclipEntry> out;
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        SubclipEntry s;
        s.id = o.value(QStringLiteral("id")).toString();
        s.name = o.value(QStringLiteral("name")).toString();
        s.sourcePath = o.value(QStringLiteral("sourcePath")).toString();
        s.sourceDuration = qMax(0.0, o.value(QStringLiteral("sourceDuration")).toDouble(0.0));
        s.inPoint = qMax(0.0, o.value(QStringLiteral("inPoint")).toDouble(0.0));
        s.outPoint = qMax(s.inPoint, o.value(QStringLiteral("outPoint")).toDouble(s.inPoint));
        s.notes = o.value(QStringLiteral("notes")).toString();
        if (s.id.isEmpty())
            s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!s.sourcePath.isEmpty() && s.outPoint > s.inPoint)
            out.append(s);
    }
    return out;
}

} // namespace

QVector<SubclipEntry> loadSubclips(const QString &overrideJsonPath)
{
    if (!overrideJsonPath.isEmpty()) {
        QFile f(overrideJsonPath);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        return doc.isArray() ? deserialize(doc.array()) : QVector<SubclipEntry>{};
    }
    QSettings s;
    s.beginGroup(QStringLiteral("Subclips"));
    const QByteArray raw = s.value(QStringLiteral("ListJson")).toByteArray();
    s.endGroup();
    if (raw.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    return doc.isArray() ? deserialize(doc.array()) : QVector<SubclipEntry>{};
}

bool saveSubclips(const QVector<SubclipEntry> &list, const QString &overrideJsonPath)
{
    const QByteArray payload = QJsonDocument(serialize(list)).toJson(QJsonDocument::Indented);
    if (!overrideJsonPath.isEmpty()) {
        QDir().mkpath(QFileInfo(overrideJsonPath).absolutePath());
        QFile f(overrideJsonPath);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        return f.write(payload) == payload.size();
    }
    QSettings s;
    s.beginGroup(QStringLiteral("Subclips"));
    s.setValue(QStringLiteral("ListJson"), payload);
    s.sync();
    s.endGroup();
    return true;
}

SubclipEntry createSubclip(const QString &name,
                           const QString &sourcePath,
                           double inPoint,
                           double outPoint,
                           double sourceDuration,
                           const QString &notes,
                           const QString &overrideJsonPath)
{
    SubclipEntry e;
    if (sourcePath.trimmed().isEmpty())
        return e;
    const double cleanIn = qMax(0.0, std::isfinite(inPoint) ? inPoint : 0.0);
    const double cleanOut = qMax(cleanIn + 0.01, std::isfinite(outPoint) ? outPoint : cleanIn + 1.0);
    e.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    e.sourcePath = sourcePath;
    e.inPoint = cleanIn;
    e.outPoint = cleanOut;
    e.sourceDuration = qMax(cleanOut, std::isfinite(sourceDuration) ? sourceDuration : cleanOut);
    e.name = name.trimmed().isEmpty()
        ? QStringLiteral("%1 [%2s-%3s]").arg(QFileInfo(sourcePath).baseName())
              .arg(cleanIn, 0, 'f', 1).arg(cleanOut, 0, 'f', 1)
        : name.trimmed();
    e.notes = notes;

    QVector<SubclipEntry> list = loadSubclips(overrideJsonPath);
    list.append(e);
    saveSubclips(list, overrideJsonPath);
    return e;
}

SubclipEntry createSubclipFromClip(const ClipInfo &clip,
                                   const QString &customName,
                                   const QString &overrideJsonPath)
{
    const double srcOut = (clip.outPoint > clip.inPoint) ? clip.outPoint : clip.duration;
    const double srcDur = qMax(clip.duration, srcOut);
    const QString baseName = !customName.trimmed().isEmpty()
        ? customName.trimmed()
        : (clip.displayName.isEmpty() ? QFileInfo(clip.filePath).baseName() : clip.displayName)
              + QStringLiteral(" (サブクリップ)");
    return createSubclip(baseName, clip.filePath, clip.inPoint, srcOut, srcDur, {}, overrideJsonPath);
}

bool removeSubclip(const QString &id, const QString &overrideJsonPath)
{
    QVector<SubclipEntry> list = loadSubclips(overrideJsonPath);
    QVector<SubclipEntry> kept;
    for (const auto &s : list) {
        if (s.id != id)
            kept.append(s);
    }
    if (kept.size() == list.size())
        return false;
    return saveSubclips(kept, overrideJsonPath);
}

bool insertSubclipIntoTimeline(Timeline *timeline,
                               const SubclipEntry &entry,
                               int videoTrackIdx)
{
    if (!timeline || entry.sourcePath.isEmpty() || entry.outPoint <= entry.inPoint)
        return false;
    const auto tracks = timeline->videoTracks();
    if (videoTrackIdx < 0 || videoTrackIdx >= tracks.size() || !tracks[videoTrackIdx])
        return false;
    TimelineTrack *tr = tracks[videoTrackIdx];
    if (tr->isLocked())
        return false;

    ClipInfo c;
    c.filePath = entry.sourcePath;
    c.displayName = entry.name;
    c.inPoint = entry.inPoint;
    c.outPoint = entry.outPoint;
    c.duration = qMax(entry.sourceDuration, entry.outPoint);
    tr->addClip(c);
    timeline->saveUndoState(QStringLiteral("サブクリップを挿入 (%1)").arg(entry.name));
    return true;
}

SubclipManagerDialog::SubclipManagerDialog(Timeline *timeline,
                                           QWidget *parent,
                                           const QString &overrideJsonPath)
    : QDialog(parent)
    , m_timeline(timeline)
    , m_overridePath(overrideJsonPath)
{
    setWindowTitle(QStringLiteral("サブクリップ管理 (仮想トリム素材)"));
    resize(560, 400);

    auto *root = new QVBoxLayout(this);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(QStringLiteral("subclipSearchEdit"));
    m_searchEdit->setPlaceholderText(QStringLiteral("サブクリップ名・ファイル名で検索…"));
    root->addWidget(m_searchEdit);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("subclipList"));
    root->addWidget(m_list, 1);

    auto *row = new QHBoxLayout();
    auto *addBtn = new QPushButton(QStringLiteral("ファイルから新規作成…"), this);
    auto *delBtn = new QPushButton(QStringLiteral("削除"), this);
    auto *insertBtn = new QPushButton(QStringLiteral("タイムラインへ挿入"), this);
    auto *closeBtn = new QPushButton(QStringLiteral("閉じる"), this);
    row->addWidget(addBtn);
    row->addWidget(delBtn);
    row->addStretch();
    row->addWidget(insertBtn);
    row->addWidget(closeBtn);
    root->addLayout(row);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &SubclipManagerDialog::reloadList);
    connect(addBtn, &QPushButton::clicked, this, &SubclipManagerDialog::onCreateManual);
    connect(delBtn, &QPushButton::clicked, this, &SubclipManagerDialog::onDeleteSelected);
    connect(insertBtn, &QPushButton::clicked, this, &SubclipManagerDialog::onInsertSelected);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &SubclipManagerDialog::onInsertSelected);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    reloadList();
}

int SubclipManagerDialog::subclipCount() const
{
    return m_list ? m_list->count() : 0;
}

void SubclipManagerDialog::reloadList()
{
    m_entries = loadSubclips(m_overridePath);
    const QString q = m_searchEdit->text().trimmed().toLower();
    m_list->clear();
    for (const auto &s : m_entries) {
        if (!q.isEmpty() && !s.name.toLower().contains(q) && !s.sourcePath.toLower().contains(q))
            continue;
        const QString text = QStringLiteral("%1  [%2s - %3s, 長さ %4s]  —  %5")
            .arg(s.name)
            .arg(s.inPoint, 0, 'f', 2)
            .arg(s.outPoint, 0, 'f', 2)
            .arg(s.duration(), 0, 'f', 2)
            .arg(QFileInfo(s.sourcePath).fileName());
        auto *item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, s.id);
    }
}

void SubclipManagerDialog::onCreateManual()
{
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("サブクリップ元メディアを選択"));
    if (file.isEmpty()) return;
    bool ok1 = false, ok2 = false;
    const double inSec = QInputDialog::getDouble(this, QStringLiteral("サブクリップ In 点"),
                                                 QStringLiteral("開始位置 (秒):"), 0.0, 0.0, 86400.0, 2, &ok1);
    if (!ok1) return;
    const double outSec = QInputDialog::getDouble(this, QStringLiteral("サブクリップ Out 点"),
                                                  QStringLiteral("終了位置 (秒):"), inSec + 3.0,
                                                  inSec + 0.05, 86400.0, 2, &ok2);
    if (!ok2) return;
    createSubclip(QString(), file, inSec, outSec, outSec, {}, m_overridePath);
    reloadList();
}

void SubclipManagerDialog::onInsertSelected()
{
    auto *item = m_list->currentItem();
    if (!item || !m_timeline) return;
    const QString id = item->data(Qt::UserRole).toString();
    for (const auto &s : m_entries) {
        if (s.id == id) {
            insertSubclipIntoTimeline(m_timeline, s, 0);
            accept();
            return;
        }
    }
}

void SubclipManagerDialog::onDeleteSelected()
{
    auto *item = m_list->currentItem();
    if (!item) return;
    removeSubclip(item->data(Qt::UserRole).toString(), m_overridePath);
    reloadList();
}

} // namespace subclips
