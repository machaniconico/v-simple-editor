#include "PluginManifest.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

static QStringList s_lastWarnings;

QVector<PluginInfo> PluginManifestScanner::scanFolder(const QString& dir, int maxDepth)
{
    s_lastWarnings.clear();

    QVector<PluginInfo> results;

    if (!QDir(dir).exists())
        return results;

    const QString baseDir = QDir(dir).absolutePath();
    // Depth is measured via relative path separator count below (not absolute depth).

    QDirIterator it(baseDir, QStringList() << "*.json",
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        const QString filePath = it.next();

        // Depth guard: count path separators relative to base
        const QString relative = QDir(baseDir).relativeFilePath(filePath);
        const int depth = relative.count('/');
        if (depth > maxDepth - 1)
            continue;

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            s_lastWarnings.append(QStringLiteral("cannot open: ") + filePath);
            continue;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();

        if (doc.isNull()) {
            s_lastWarnings.append(QStringLiteral("invalid JSON: ") + filePath);
            continue;
        }

        if (!doc.isObject())
            continue;

        const QJsonObject obj = doc.object();

        const QString pluginType = obj.value(QStringLiteral("pluginType")).toString();
        if (pluginType != QLatin1String("ofx") && pluginType != QLatin1String("veditor"))
            continue;

        const QString id = obj.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            s_lastWarnings.append(QStringLiteral("missing id in: ") + filePath);
            continue;
        }

        PluginInfo info;
        info.id           = id;
        info.name         = obj.value(QStringLiteral("name")).toString();
        info.version      = obj.value(QStringLiteral("version")).toString();
        info.category     = obj.value(QStringLiteral("category")).toString();
        info.vendor       = obj.value(QStringLiteral("vendor")).toString();
        info.description  = obj.value(QStringLiteral("description")).toString();
        info.manifestPath = QFileInfo(filePath).absoluteFilePath();

        const QJsonArray params = obj.value(QStringLiteral("params")).toArray();
        for (const QJsonValue& elem : params) {
            const QString pname = elem.toObject().value(QStringLiteral("name")).toString();
            if (!pname.isEmpty())
                info.paramNames.append(pname);
        }

        const QJsonArray tags = obj.value(QStringLiteral("tags")).toArray();
        for (const QJsonValue& elem : tags) {
            const QString tag = elem.toString();
            if (!tag.isEmpty())
                info.tags.append(tag);
        }

        results.append(info);
    }

    std::sort(results.begin(), results.end(),
              [](const PluginInfo& a, const PluginInfo& b) {
                  return a.manifestPath < b.manifestPath;
              });

    return results;
}

QStringList PluginManifestScanner::lastWarnings()
{
    return s_lastWarnings;
}
