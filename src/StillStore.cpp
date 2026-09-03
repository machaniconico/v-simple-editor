#include "StillStore.h"

#include "FrameExport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace stillstore {
namespace {

QString indexPath(const QString &directory)
{
    return QDir(directory).filePath(QStringLiteral("index.json"));
}

QString fileNameForStill(const Still &still)
{
    return QFileInfo(still.filePath).fileName();
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

void StillStore::setBaseDirOverride(const QString &directory)
{
    m_baseDirOverride = directory;
}

QString StillStore::baseDir() const
{
    if (!m_baseDirOverride.isEmpty())
        return QDir::cleanPath(m_baseDirOverride);

    QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::home().filePath(QStringLiteral(".veditor"));
    return QDir(base).filePath(QStringLiteral("stills"));
}

bool StillStore::ensureBaseDir(QString *error) const
{
    const QString directory = baseDir();
    if (!directory.isEmpty() && QDir().mkpath(directory))
        return true;
    setError(error, QStringLiteral("スチル保存先を作成できません: %1").arg(directory));
    return false;
}

QVector<Still> StillStore::list(QString *error) const
{
    if (error)
        error->clear();

    const QString directory = baseDir();
    QFile file(indexPath(directory));
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("スチル一覧を開けません: %1").arg(file.errorString()));
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("スチル一覧が壊れています: %1").arg(parseError.errorString()));
        return {};
    }

    QVector<Still> result;
    const QJsonArray entries = document.object().value(QStringLiteral("stills")).toArray();
    result.reserve(entries.size());
    for (const QJsonValue &value : entries) {
        const QJsonObject object = value.toObject();
        const QString fileName = object.value(QStringLiteral("fileName")).toString();
        if (fileName.isEmpty() || QFileInfo(fileName).fileName() != fileName
            || !fileName.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
            continue;
        }

        Still still;
        still.id = object.value(QStringLiteral("id")).toString();
        still.filePath = QDir(directory).filePath(fileName);
        still.timestamp = QDateTime::fromString(
            object.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
        still.projectName = object.value(QStringLiteral("projectName")).toString();
        still.label = object.value(QStringLiteral("label")).toString();
        if (!still.id.isEmpty())
            result.append(still);
    }

    std::sort(result.begin(), result.end(), [](const Still &a, const Still &b) {
        return a.timestamp > b.timestamp;
    });
    return result;
}

bool StillStore::writeIndex(const QVector<Still> &stills, QString *error) const
{
    if (!ensureBaseDir(error))
        return false;

    QJsonArray entries;
    for (const Still &still : stills) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), still.id);
        object.insert(QStringLiteral("fileName"), fileNameForStill(still));
        object.insert(QStringLiteral("timestamp"),
                      still.timestamp.toUTC().toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("projectName"), still.projectName);
        object.insert(QStringLiteral("label"), still.label);
        entries.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("stills"), entries);

    QSaveFile file(indexPath(baseDir()));
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("スチル一覧を書き込めません: %1").arg(file.errorString()));
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size() || !file.commit()) {
        setError(error, QStringLiteral("スチル一覧の保存に失敗しました: %1").arg(file.errorString()));
        return false;
    }
    return true;
}

bool StillStore::save(const QImage &image, const QString &projectName,
                      const QString &label, Still *saved, QString *error)
{
    if (error)
        error->clear();
    if (image.isNull()) {
        setError(error, QStringLiteral("保存するフレームが空です。"));
        return false;
    }
    if (!ensureBaseDir(error))
        return false;

    Still still;
    still.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    still.timestamp = QDateTime::currentDateTimeUtc();
    still.projectName = projectName;
    still.label = label;
    const QString fileName = QStringLiteral("still-%1-%2.png")
        .arg(still.timestamp.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")),
             still.id.left(8));
    still.filePath = QDir(baseDir()).filePath(fileName);

    if (!frameexport::saveFrameImage(image, still.filePath,
                                     frameexport::ImageFormat::Png, error)) {
        return false;
    }

    QString listError;
    QVector<Still> stills = list(&listError);
    if (!listError.isEmpty()) {
        QFile::remove(still.filePath);
        setError(error, listError);
        return false;
    }
    stills.prepend(still);
    if (!writeIndex(stills, error)) {
        QFile::remove(still.filePath);
        return false;
    }

    if (saved)
        *saved = still;
    return true;
}

bool StillStore::remove(const QString &id, QString *error)
{
    if (error)
        error->clear();
    QString listError;
    QVector<Still> stills = list(&listError);
    if (!listError.isEmpty()) {
        setError(error, listError);
        return false;
    }

    auto it = std::find_if(stills.begin(), stills.end(), [&id](const Still &still) {
        return still.id == id;
    });
    if (it == stills.end()) {
        setError(error, QStringLiteral("指定されたスチルが見つかりません。"));
        return false;
    }

    const QString imagePath = it->filePath;
    stills.erase(it);
    if (!writeIndex(stills, error))
        return false;
    if (QFile::exists(imagePath) && !QFile::remove(imagePath)) {
        setError(error, QStringLiteral("スチル画像を削除できません: %1").arg(imagePath));
        return false;
    }
    return true;
}

bool StillStore::setLabel(const QString &id, const QString &label, QString *error)
{
    if (error)
        error->clear();
    QString listError;
    QVector<Still> stills = list(&listError);
    if (!listError.isEmpty()) {
        setError(error, listError);
        return false;
    }

    auto it = std::find_if(stills.begin(), stills.end(), [&id](const Still &still) {
        return still.id == id;
    });
    if (it == stills.end()) {
        setError(error, QStringLiteral("指定されたスチルが見つかりません。"));
        return false;
    }
    it->label = label;
    return writeIndex(stills, error);
}

} // namespace stillstore
