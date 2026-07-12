#include "ProjectTemplate.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace projtmpl {

// ---------------------------------------------------------------------------
// builtInTemplates
// ---------------------------------------------------------------------------
QVector<TemplateMeta> TemplateLibrary::builtInTemplates()
{
    QVector<TemplateMeta> list;

    TemplateMeta t;

    t = {};
    t.id          = QStringLiteral("yt1080p30");
    t.name        = QStringLiteral("YouTube 1080p30");
    t.description = QStringLiteral("Standard YouTube upload: 1920x1080 at 30 fps.");
    t.category    = QStringLiteral("YouTube");
    t.width       = 1920; t.height = 1080; t.fps = 30; t.builtIn = true;
    list.append(t);

    t = {};
    t.id          = QStringLiteral("ytshorts");
    t.name        = QStringLiteral("YouTube Shorts 9:16");
    t.description = QStringLiteral("Vertical short-form for YouTube Shorts.");
    t.category    = QStringLiteral("Shorts");
    t.width       = 1080; t.height = 1920; t.fps = 30; t.builtIn = true;
    list.append(t);

    t = {};
    t.id          = QStringLiteral("tiktok");
    t.name        = QStringLiteral("TikTok 9:16");
    t.description = QStringLiteral("Vertical short-form for TikTok.");
    t.category    = QStringLiteral("Shorts");
    t.width       = 1080; t.height = 1920; t.fps = 30; t.builtIn = true;
    list.append(t);

    t = {};
    t.id          = QStringLiteral("igreels");
    t.name        = QStringLiteral("Instagram Reels");
    t.description = QStringLiteral("Vertical short-form for Instagram Reels.");
    t.category    = QStringLiteral("Shorts");
    t.width       = 1080; t.height = 1920; t.fps = 30; t.builtIn = true;
    list.append(t);

    t = {};
    t.id          = QStringLiteral("cine4k24");
    t.name        = QStringLiteral("Cinematic 4K24");
    t.description = QStringLiteral("4K cinematic at 24 fps — film-style master.");
    t.category    = QStringLiteral("Film");
    t.width       = 3840; t.height = 2160; t.fps = 24; t.builtIn = true;
    list.append(t);

    t = {};
    t.id          = QStringLiteral("podcast720");
    t.name        = QStringLiteral("Podcast 720p");
    t.description = QStringLiteral("Audio-first podcast recording at 1280x720.");
    t.category    = QStringLiteral("Audio");
    t.width       = 1280; t.height = 720; t.fps = 30; t.builtIn = true;
    list.append(t);

    return list;
}

// ---------------------------------------------------------------------------
// createProjectFromTemplate
// ---------------------------------------------------------------------------
QByteArray TemplateLibrary::createProjectFromTemplate(const QString &templateId)
{
    const QVector<TemplateMeta> all = allTemplates();
    for (const TemplateMeta &m : all) {
        if (m.id == templateId) {
            QJsonObject config;
            config[QStringLiteral("name")]   = m.name;
            config[QStringLiteral("width")]  = m.width;
            config[QStringLiteral("height")] = m.height;
            config[QStringLiteral("fps")]    = m.fps;

            QJsonObject obj;
            obj[QStringLiteral("version")] = 1;
            obj[QStringLiteral("config")] = config;
            obj[QStringLiteral("videoTracks")] = QJsonArray{};
            obj[QStringLiteral("audioTracks")] = QJsonArray{};
            obj[QStringLiteral("playheadPos")] = 0.0;
            obj[QStringLiteral("markIn")] = -1.0;
            obj[QStringLiteral("markOut")] = -1.0;
            obj[QStringLiteral("zoomLevel")] = 10;
            return QJsonDocument(obj).toJson(QJsonDocument::Compact);
        }
    }
    return QByteArray{};
}

// ---------------------------------------------------------------------------
// userTemplateDir — creates if missing
// ---------------------------------------------------------------------------
QString TemplateLibrary::userTemplateDir()
{
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/templates");
    QDir{}.mkpath(path);
    return path;
}

// ---------------------------------------------------------------------------
// saveUserTemplate
// ---------------------------------------------------------------------------
bool TemplateLibrary::saveUserTemplate(const TemplateMeta &meta)
{
    QJsonObject obj;
    obj[QStringLiteral("id")]          = meta.id;
    obj[QStringLiteral("name")]        = meta.name;
    obj[QStringLiteral("description")] = meta.description;
    obj[QStringLiteral("category")]    = meta.category;
    obj[QStringLiteral("width")]       = meta.width;
    obj[QStringLiteral("height")]      = meta.height;
    obj[QStringLiteral("fps")]         = meta.fps;

    const QString filePath =
        userTemplateDir() + QStringLiteral("/") + meta.id + QStringLiteral(".json");
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}

// ---------------------------------------------------------------------------
// userTemplates
// ---------------------------------------------------------------------------
QVector<TemplateMeta> TemplateLibrary::userTemplates()
{
    QVector<TemplateMeta> list;
    QDir dir(userTemplateDir());
    const QStringList entries = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &entry : entries) {
        QFile f(dir.filePath(entry));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject obj =
            QJsonDocument::fromJson(f.readAll()).object();
        if (obj.isEmpty())
            continue;
        TemplateMeta m;
        m.id          = obj[QStringLiteral("id")].toString();
        m.name        = obj[QStringLiteral("name")].toString();
        m.description = obj[QStringLiteral("description")].toString();
        m.category    = obj[QStringLiteral("category")].toString();
        m.width       = obj[QStringLiteral("width")].toInt(1920);
        m.height      = obj[QStringLiteral("height")].toInt(1080);
        m.fps         = obj[QStringLiteral("fps")].toInt(30);
        m.builtIn     = false;
        list.append(m);
    }
    return list;
}

// ---------------------------------------------------------------------------
// allTemplates
// ---------------------------------------------------------------------------
QVector<TemplateMeta> TemplateLibrary::allTemplates()
{
    QVector<TemplateMeta> all = builtInTemplates();
    all.append(userTemplates());
    return all;
}

} // namespace projtmpl
