#pragma once

// src/MediaPool.h
// MP-1: MediaPool コア・モデル。NLE のメディアプール / ビン管理の SSOT。
// 純粋ロジック (QObject 不使用、デフォルトコピー可能)。現在時刻の取得は
// 呼び出し側責務 (importedAtIso は呼び出し側がセット) — テスト決定性のため
// MediaPool 内では QDateTime::currentDateTime を一切使わない。

#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>

namespace mediapool {

// メディア種別。Unknown は型不問 / 未判定を表す。
enum class MediaType {
    Video,
    Audio,
    Image,
    Unknown
};

QString    mediaTypeToString(MediaType type);
MediaType  mediaTypeFromString(const QString& text);

// 1 つの取り込み済みメディア素材。
struct MediaAsset {
    int       id = -1;
    QString   filePath;
    QString   displayName;
    MediaType type = MediaType::Unknown;
    qint64    durationMs = 0;
    int       width = 0;
    int       height = 0;
    double    frameRate = 0.0;
    qint64    fileSizeBytes = 0;
    QString   importedAtIso;   // 呼び出し側が ISO8601 でセット
    QStringList tags;
    QString   binId;           // 所属ビン id。空=ルート直下
    QString   colorLabel;
    QString   comment;
};

// ビン (フォルダ)。parentId 空=ルート直下。階層対応。
struct MediaBin {
    QString id;
    QString name;
    QString parentId;
};

// スマートビン: 条件で動的に素材を抽出する仮想ビン。
// typeFilter=Unknown は型不問。
struct SmartBin {
    QString   id;
    QString   name;
    QString   nameQuery;       // displayName 部分一致 (空=条件なし)
    MediaType typeFilter = MediaType::Unknown;
    QString   tagFilter;       // tags に含むか (空=条件なし)
};

// メディアプール本体。singleton 化しない普通の値クラス。
class MediaPool {
public:
    MediaPool() = default;

    // --- asset CRUD --------------------------------------------------------
    // filePath が既存と一致したら新規追加せず既存 id を返す (パス重複排除)。
    // 新規は monotonic id を採番して返す。
    int addAsset(const MediaAsset& asset);
    bool removeAsset(int id);
    const QVector<MediaAsset>& assets() const;
    const MediaAsset* getAsset(int id) const;   // 無ければ nullptr

    // --- bin CRUD ----------------------------------------------------------
    // 生成した bin id を返す (例: "bin-1")。
    QString createBin(const QString& name, const QString& parentId = QString());
    // bin 内 asset は binId を空に戻し、子 bin はルート化する。
    bool removeBin(const QString& binId);
    bool renameBin(const QString& binId, const QString& newName);
    const QVector<MediaBin>& bins() const;

    bool moveAssetToBin(int assetId, const QString& binId);   // binId="" でルートへ
    QVector<MediaAsset> assetsInBin(const QString& binId) const;

    // --- search ------------------------------------------------------------
    // displayName + tags + comment + filePath を case-insensitive 部分一致。
    // 空クエリは全件。
    QVector<MediaAsset> search(const QString& query) const;

    // --- smart bin ---------------------------------------------------------
    QString addSmartBin(const SmartBin& smartBin);   // id 採番して返す
    const QVector<SmartBin>& smartBins() const;
    // nameQuery (displayName 部分一致) AND typeFilter (Unknown 以外なら一致)
    // AND tagFilter (空でなければ tags に含む) で絞り込み。
    QVector<MediaAsset> evaluateSmartBin(const QString& smartBinId) const;

    // --- persistence -------------------------------------------------------
    QJsonObject toJson() const;            // assets/bins/smartBins/nextId 全状態
    void fromJson(const QJsonObject& obj); // 完全復元 (round-trip)
    void clear();

private:
    QVector<MediaAsset> m_assets;
    QVector<MediaBin>   m_bins;
    QVector<SmartBin>   m_smartBins;
    int m_nextAssetId = 1;
    int m_nextBinId = 1;
    int m_nextSmartBinId = 1;
};

} // namespace mediapool
