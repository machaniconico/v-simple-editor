#include "../MediaPool.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <cstdio>

int runMediaPoolRatingSelftest()
{
    using namespace mediapool;
    int passed = 0;
    int failed = 0;
    const auto check = [&](int gate, bool ok) {
        ok ? ++passed : ++failed;
        std::fprintf(stderr, "[media-pool-rating] %s G%d\n", ok ? "PASS" : "FAIL", gate);
    };

    MediaPool pool;
    QVector<int> ids;
    const QStringList names = {QStringLiteral("Alpha"), QStringLiteral("Echo"),
        QStringLiteral("Gamma"), QStringLiteral("Delta"), QStringLiteral("Foxtrot")};
    for (int i = 0; i < names.size(); ++i) {
        MediaAsset asset;
        asset.filePath = QStringLiteral("/clips/%1.mov").arg(i);
        asset.displayName = names[i];
        ids.append(pool.addAsset(asset));
    }

    // G1: Default keys are omitted; old JSON loads with defaults; ratings persist.
    const QJsonObject defaults = pool.toJson();
    bool persistence = true;
    for (const QJsonValue &value : defaults.value(QStringLiteral("assets")).toArray()) {
        const auto asset = value.toObject();
        persistence = persistence && !asset.contains(QStringLiteral("flag"))
            && !asset.contains(QStringLiteral("stars"));
    }
    MediaPool oldProject;
    oldProject.fromJson(defaults);
    for (const auto &asset : oldProject.assets())
        persistence = persistence && asset.flag == AssetFlag::None && asset.stars == 0;
    persistence = persistence && pool.setAssetFlag(ids[0], AssetFlag::Favorite)
        && pool.setAssetFlag(ids[1], AssetFlag::Favorite)
        && pool.setAssetFlag(ids[2], AssetFlag::Rejected);
    for (int i = 0; i < ids.size(); ++i)
        persistence = pool.setAssetStars(ids[i], i + 1) && persistence;
    const QJsonObject rated = pool.toJson();
    MediaPool restored;
    restored.fromJson(QJsonDocument::fromJson(QJsonDocument(rated).toJson()).object());
    persistence = persistence && restored.toJson() == rated;
    for (int i = 0; i < ids.size(); ++i) {
        const auto *asset = restored.getAsset(ids[i]);
        persistence = persistence && asset && asset->stars == i + 1
            && asset->flag == pool.getAsset(ids[i])->flag;
    }
    persistence = persistence && !pool.setAssetFlag(-1, AssetFlag::Favorite)
        && !pool.setAssetStars(-1, 2)
        && !pool.setAssetFlag(ids[0], static_cast<AssetFlag>(99));
    restored.setAssetStars(ids[0], -1);
    restored.setAssetStars(ids[1], 99);
    persistence = persistence && restored.getAsset(ids[0])->stars == 0
        && restored.getAsset(ids[1])->stars == 5;
    for (int id : ids) {
        restored.setAssetFlag(id, AssetFlag::None);
        restored.setAssetStars(id, 0);
    }
    persistence = persistence && restored.toJson() == defaults;
    check(1, persistence);

    // G2: Five assets, two favorites, one rejected, one unused.
    QSet<QString> used;
    for (int i = 0; i < 4; ++i)
        used.insert(pool.getAsset(ids[i])->filePath);
    const auto unused = pool.filtered({}, AssetFilterMode::Unused, used);
    check(2, pool.filtered({}, AssetFilterMode::All, used).size() == 5
        && pool.filtered({}, AssetFilterMode::Favorites, used).size() == 2
        && pool.filtered({}, AssetFilterMode::ExcludeRejected, used).size() == 4
        && unused.size() == 1 && unused.first().id == ids[4]);

    // G3: Rename affects labels only; missing IDs fail.
    MediaPool renamed = pool;
    const QString binId = renamed.createBin(QStringLiteral("旧ビン"));
    const QString path = renamed.getAsset(ids[0])->filePath;
    check(3, renamed.renameAsset(ids[0], QStringLiteral("新しい素材"))
        && renamed.renameBin(binId, QStringLiteral("新ビン"))
        && renamed.getAsset(ids[0])->displayName == QStringLiteral("新しい素材")
        && renamed.getAsset(ids[0])->filePath == path
        && renamed.bins().first().name == QStringLiteral("新ビン")
        && !renamed.renameAsset(-1, QStringLiteral("不明"))
        && !renamed.renameBin(QStringLiteral("missing"), QStringLiteral("不明")));

    // G4: Name query AND favorite status, including case-insensitive search.
    const auto hits = pool.filtered(QStringLiteral("a"), AssetFilterMode::Favorites, used);
    const auto upper = pool.filtered(QStringLiteral(" A "), AssetFilterMode::Favorites, used);
    check(4, pool.search(QStringLiteral("a")).size() == 3
        && hits.size() == 1 && hits.first().id == ids[0]
        && upper.size() == 1 && upper.first().id == ids[0]
        && pool.filtered(QStringLiteral("missing"), AssetFilterMode::Favorites, used).isEmpty());

    std::fprintf(stderr, "[media-pool-rating] summary: %d PASS, %d FAIL\n", passed, failed);
    return failed;
}
