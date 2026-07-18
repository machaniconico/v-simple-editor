// src/MediaPool.cpp
// MP-1: MediaPool コア・モデル実装。MediaPool.h のコントラクト参照。

#include "MediaPool.h"

#include <QJsonArray>
#include <QJsonValue>

namespace mediapool {

namespace {

bool binExists(const QVector<MediaBin>& bins, const QString& binId)
{
    if (binId.isEmpty()) {
        return true;
    }
    for (const MediaBin& bin : bins) {
        if (bin.id == binId) {
            return true;
        }
    }
    return false;
}

int numericSuffixAfterPrefix(const QString& text, const QString& prefix)
{
    if (!text.startsWith(prefix)) {
        return 0;
    }
    bool ok = false;
    const int value = text.mid(prefix.size()).toInt(&ok);
    return ok ? value : 0;
}

int jsonIntOrString(const QJsonObject& obj, const QString& key, int fallback)
{
    const QJsonValue value = obj.value(key);
    if (value.isDouble()) {
        return value.toInt(fallback);
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

bool parentCreatesCycle(const QVector<MediaBin>& bins,
                        const QString& childId,
                        const QString& parentId)
{
    QString cursor = parentId;
    for (int depth = 0; !cursor.isEmpty() && depth <= bins.size(); ++depth) {
        if (cursor == childId) {
            return true;
        }

        QString next;
        for (const MediaBin& bin : bins) {
            if (bin.id == cursor) {
                next = bin.parentId;
                break;
            }
        }
        if (next.isEmpty()) {
            return false;
        }
        cursor = next;
    }
    return !cursor.isEmpty();
}

} // namespace

QString mediaTypeToString(MediaType type)
{
    switch (type) {
    case MediaType::Video: return QStringLiteral("video");
    case MediaType::Audio: return QStringLiteral("audio");
    case MediaType::Image: return QStringLiteral("image");
    case MediaType::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

MediaType mediaTypeFromString(const QString& text)
{
    const QString t = text.trimmed().toLower();
    if (t == QStringLiteral("video")) return MediaType::Video;
    if (t == QStringLiteral("audio")) return MediaType::Audio;
    if (t == QStringLiteral("image")) return MediaType::Image;
    return MediaType::Unknown;
}

// ---------------------------------------------------------------------------
// asset CRUD
// ---------------------------------------------------------------------------
int MediaPool::addAsset(const MediaAsset& asset)
{
    // パス重複排除: 既存と filePath が一致したら既存 id を返す。
    if (!asset.filePath.isEmpty()) {
        for (const MediaAsset& existing : m_assets) {
            if (existing.filePath == asset.filePath) {
                return existing.id;
            }
        }
    }

    MediaAsset copy = asset;
    copy.id = m_nextAssetId++;
    m_assets.append(copy);
    return copy.id;
}

bool MediaPool::removeAsset(int id)
{
    for (int i = 0; i < m_assets.size(); ++i) {
        if (m_assets.at(i).id == id) {
            m_assets.removeAt(i);
            return true;
        }
    }
    return false;
}

const QVector<MediaAsset>& MediaPool::assets() const
{
    return m_assets;
}

const MediaAsset* MediaPool::getAsset(int id) const
{
    for (const MediaAsset& a : m_assets) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// bin CRUD
// ---------------------------------------------------------------------------
QString MediaPool::createBin(const QString& name, const QString& parentId)
{
    MediaBin bin;
    bin.id = QStringLiteral("bin-%1").arg(m_nextBinId++);
    bin.name = name;
    bin.parentId = binExists(m_bins, parentId) ? parentId : QString();
    m_bins.append(bin);
    return bin.id;
}

bool MediaPool::removeBin(const QString& binId)
{
    int idx = -1;
    for (int i = 0; i < m_bins.size(); ++i) {
        if (m_bins.at(i).id == binId) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return false;
    }

    // bin 内 asset は binId を空に戻す (ルート化)。
    for (MediaAsset& a : m_assets) {
        if (a.binId == binId) {
            a.binId.clear();
        }
    }
    // 子 bin はルート化 (親を空に付け替え)。
    for (MediaBin& b : m_bins) {
        if (b.parentId == binId) {
            b.parentId.clear();
        }
    }

    m_bins.removeAt(idx);
    return true;
}

bool MediaPool::renameBin(const QString& binId, const QString& newName)
{
    for (MediaBin& b : m_bins) {
        if (b.id == binId) {
            b.name = newName;
            return true;
        }
    }
    return false;
}

const QVector<MediaBin>& MediaPool::bins() const
{
    return m_bins;
}

bool MediaPool::moveAssetToBin(int assetId, const QString& binId)
{
    if (!binExists(m_bins, binId)) {
        return false;
    }
    for (MediaAsset& a : m_assets) {
        if (a.id == assetId) {
            a.binId = binId;
            return true;
        }
    }
    return false;
}

QVector<MediaAsset> MediaPool::assetsInBin(const QString& binId) const
{
    QVector<MediaAsset> result;
    for (const MediaAsset& a : m_assets) {
        if (a.binId == binId) {
            result.append(a);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------
QVector<MediaAsset> MediaPool::search(const QString& query) const
{
    QVector<MediaAsset> result;
    const QString needle = query.trimmed();
    if (needle.isEmpty()) {
        return m_assets;   // 空クエリは全件
    }

    for (const MediaAsset& a : m_assets) {
        bool matched = a.displayName.contains(needle, Qt::CaseInsensitive)
            || a.comment.contains(needle, Qt::CaseInsensitive)
            || a.filePath.contains(needle, Qt::CaseInsensitive);
        if (!matched) {
            for (const QString& tag : a.tags) {
                if (tag.contains(needle, Qt::CaseInsensitive)) {
                    matched = true;
                    break;
                }
            }
        }
        if (matched) {
            result.append(a);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// smart bin
// ---------------------------------------------------------------------------
QString MediaPool::addSmartBin(const SmartBin& smartBin)
{
    SmartBin copy = smartBin;
    copy.id = QStringLiteral("smart-%1").arg(m_nextSmartBinId++);
    m_smartBins.append(copy);
    return copy.id;
}

const QVector<SmartBin>& MediaPool::smartBins() const
{
    return m_smartBins;
}

QVector<MediaAsset> MediaPool::evaluateSmartBin(const QString& smartBinId) const
{
    const SmartBin* sb = nullptr;
    for (const SmartBin& s : m_smartBins) {
        if (s.id == smartBinId) {
            sb = &s;
            break;
        }
    }

    QVector<MediaAsset> result;
    if (!sb) {
        return result;
    }

    for (const MediaAsset& a : m_assets) {
        // nameQuery (displayName 部分一致)
        if (!sb->nameQuery.isEmpty()
            && !a.displayName.contains(sb->nameQuery, Qt::CaseInsensitive)) {
            continue;
        }
        // typeFilter (Unknown 以外なら一致)
        if (sb->typeFilter != MediaType::Unknown && a.type != sb->typeFilter) {
            continue;
        }
        // tagFilter (空でなければ tags に含む)
        if (!sb->tagFilter.isEmpty()) {
            bool hasTag = false;
            for (const QString& tag : a.tags) {
                if (tag.contains(sb->tagFilter, Qt::CaseInsensitive)) {
                    hasTag = true;
                    break;
                }
            }
            if (!hasTag) {
                continue;
            }
        }
        result.append(a);
    }
    return result;
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------
namespace {

QJsonObject assetToJson(const MediaAsset& a)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), a.id);
    o.insert(QStringLiteral("filePath"), a.filePath);
    o.insert(QStringLiteral("displayName"), a.displayName);
    o.insert(QStringLiteral("type"), mediaTypeToString(a.type));
    o.insert(QStringLiteral("durationMs"), static_cast<double>(a.durationMs));
    o.insert(QStringLiteral("width"), a.width);
    o.insert(QStringLiteral("height"), a.height);
    o.insert(QStringLiteral("frameRate"), a.frameRate);
    o.insert(QStringLiteral("fileSizeBytes"), static_cast<double>(a.fileSizeBytes));
    o.insert(QStringLiteral("importedAtIso"), a.importedAtIso);
    o.insert(QStringLiteral("tags"), QJsonArray::fromStringList(a.tags));
    o.insert(QStringLiteral("binId"), a.binId);
    o.insert(QStringLiteral("colorLabel"), a.colorLabel);
    o.insert(QStringLiteral("comment"), a.comment);
    return o;
}

MediaAsset assetFromJson(const QJsonObject& o)
{
    MediaAsset a;
    a.id = o.value(QStringLiteral("id")).toInt(-1);
    a.filePath = o.value(QStringLiteral("filePath")).toString();
    a.displayName = o.value(QStringLiteral("displayName")).toString();
    a.type = mediaTypeFromString(o.value(QStringLiteral("type")).toString());
    a.durationMs = static_cast<qint64>(o.value(QStringLiteral("durationMs")).toDouble());
    a.width = o.value(QStringLiteral("width")).toInt();
    a.height = o.value(QStringLiteral("height")).toInt();
    a.frameRate = o.value(QStringLiteral("frameRate")).toDouble();
    a.fileSizeBytes = static_cast<qint64>(o.value(QStringLiteral("fileSizeBytes")).toDouble());
    a.importedAtIso = o.value(QStringLiteral("importedAtIso")).toString();
    const QJsonArray tagArr = o.value(QStringLiteral("tags")).toArray();
    for (const QJsonValue& v : tagArr) {
        a.tags.append(v.toString());
    }
    a.binId = o.value(QStringLiteral("binId")).toString();
    a.colorLabel = o.value(QStringLiteral("colorLabel")).toString();
    a.comment = o.value(QStringLiteral("comment")).toString();
    return a;
}

QJsonObject binToJson(const MediaBin& b)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), b.id);
    o.insert(QStringLiteral("name"), b.name);
    o.insert(QStringLiteral("parentId"), b.parentId);
    return o;
}

MediaBin binFromJson(const QJsonObject& o)
{
    MediaBin b;
    b.id = o.value(QStringLiteral("id")).toString();
    b.name = o.value(QStringLiteral("name")).toString();
    b.parentId = o.value(QStringLiteral("parentId")).toString();
    return b;
}

QJsonObject smartBinToJson(const SmartBin& s)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), s.id);
    o.insert(QStringLiteral("name"), s.name);
    o.insert(QStringLiteral("nameQuery"), s.nameQuery);
    o.insert(QStringLiteral("typeFilter"), mediaTypeToString(s.typeFilter));
    o.insert(QStringLiteral("tagFilter"), s.tagFilter);
    return o;
}

SmartBin smartBinFromJson(const QJsonObject& o)
{
    SmartBin s;
    s.id = o.value(QStringLiteral("id")).toString();
    s.name = o.value(QStringLiteral("name")).toString();
    s.nameQuery = o.value(QStringLiteral("nameQuery")).toString();
    s.typeFilter = mediaTypeFromString(o.value(QStringLiteral("typeFilter")).toString());
    s.tagFilter = o.value(QStringLiteral("tagFilter")).toString();
    return s;
}

} // namespace

QJsonObject MediaPool::toJson() const
{
    QJsonObject root;

    QJsonArray assetArr;
    for (const MediaAsset& a : m_assets) {
        assetArr.append(assetToJson(a));
    }
    root.insert(QStringLiteral("assets"), assetArr);

    QJsonArray binArr;
    for (const MediaBin& b : m_bins) {
        binArr.append(binToJson(b));
    }
    root.insert(QStringLiteral("bins"), binArr);

    QJsonArray smartArr;
    for (const SmartBin& s : m_smartBins) {
        smartArr.append(smartBinToJson(s));
    }
    root.insert(QStringLiteral("smartBins"), smartArr);

    root.insert(QStringLiteral("nextAssetId"), m_nextAssetId);
    root.insert(QStringLiteral("nextBinId"), m_nextBinId);
    root.insert(QStringLiteral("nextSmartBinId"), m_nextSmartBinId);
    return root;
}

void MediaPool::fromJson(const QJsonObject& obj)
{
    clear();

    const QJsonArray assetArr = obj.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& v : assetArr) {
        m_assets.append(assetFromJson(v.toObject()));
    }

    const QJsonArray binArr = obj.value(QStringLiteral("bins")).toArray();
    for (const QJsonValue& v : binArr) {
        m_bins.append(binFromJson(v.toObject()));
    }

    const QJsonArray smartArr = obj.value(QStringLiteral("smartBins")).toArray();
    for (const QJsonValue& v : smartArr) {
        m_smartBins.append(smartBinFromJson(v.toObject()));
    }

    // Broken or hand-edited project JSON must not leave invisible bins/assets.
    for (MediaBin& bin : m_bins) {
        if (!binExists(m_bins, bin.parentId)
            || parentCreatesCycle(m_bins, bin.id, bin.parentId)) {
            bin.parentId.clear();
        }
    }
    for (MediaAsset& asset : m_assets) {
        if (!binExists(m_bins, asset.binId)) {
            asset.binId.clear();
        }
    }

    int maxAssetId = 0;
    for (const MediaAsset& a : m_assets) {
        maxAssetId = qMax(maxAssetId, a.id);
    }

    int maxBinId = 0;
    for (const MediaBin& b : m_bins) {
        maxBinId = qMax(maxBinId,
                        numericSuffixAfterPrefix(b.id, QStringLiteral("bin-")));
    }

    int maxSmartBinId = 0;
    for (const SmartBin& s : m_smartBins) {
        maxSmartBinId = qMax(maxSmartBinId,
                             numericSuffixAfterPrefix(s.id, QStringLiteral("smart-")));
    }

    // nextId は保存値を優先しつつ、欠落/型違い/古い値でも既存 id と衝突しない
    // よう最大値+1を下限にする。
    m_nextAssetId = qMax(jsonIntOrString(obj, QStringLiteral("nextAssetId"), 1),
                         maxAssetId + 1);
    m_nextBinId = qMax(jsonIntOrString(obj, QStringLiteral("nextBinId"), 1),
                       maxBinId + 1);
    m_nextSmartBinId = qMax(jsonIntOrString(obj, QStringLiteral("nextSmartBinId"), 1),
                            maxSmartBinId + 1);
}

void MediaPool::clear()
{
    m_assets.clear();
    m_bins.clear();
    m_smartBins.clear();
    m_nextAssetId = 1;
    m_nextBinId = 1;
    m_nextSmartBinId = 1;
}

} // namespace mediapool
