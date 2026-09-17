#include "ExportUserPresets.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QRegularExpression>

namespace ExportUserPresets {
QString directory()
{
    const QString overrideDir = qEnvironmentVariable("VEDITOR_EXPORT_PRESET_DIR");
    if (!overrideDir.isEmpty()) return overrideDir;
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/.veditor";
    return base + "/export-presets";
}

static QString pathFor(const QString &name)
{
    QString safe = name;
    safe.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
    // Hash prevents sanitized names and Windows case folding from colliding.
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex());
    return QDir(directory()).filePath("preset-" + safe.left(48) + "-" + hash + ".json");
}

QJsonObject toJson(const ExportConfig &c)
{
    QJsonObject j;
    j.insert("videoCodec", c.videoCodec);
    j.insert("audioCodec", c.audioCodec);
    j.insert("container", c.container);
    j.insert("videoBitrate", c.videoBitrate);
    j.insert("audioBitrate", c.audioBitrate);
    j.insert("width", c.width);
    j.insert("height", c.height);
    j.insert("fps", c.fps);
    j.insert("useHardwareAccel", c.useHardwareAccel);
    j.insert("hwEncoder", c.hwEncoder);
    j.insert("maxFileSizeMB", c.maxFileSizeMB);
    j.insert("hdr10", c.hdr10);
    j.insert("proresProfile", c.proresProfile);
    j.insert("exportMarkedRangeOnly", c.exportMarkedRangeOnly);
    j.insert("audioOnly", c.audioOnly);
    j.insert("crf", c.crf);
    j.insert("rateControl", c.rateControl == ExportConfig::RateControl::Crf ? "crf" : "bitrate");
    QJsonObject hdr;
    hdr.insert("mode", c.hdrSettings.mode);
    hdr.insert("masterDisplayLuminanceMin", c.hdrSettings.masterDisplayLuminanceMin);
    hdr.insert("masterDisplayLuminanceMax", c.hdrSettings.masterDisplayLuminanceMax);
    hdr.insert("maxCll", c.hdrSettings.maxCll);
    hdr.insert("maxFall", c.hdrSettings.maxFall);
    hdr.insert("previewToneMap", c.hdrSettings.previewToneMap);
    j.insert("hdrSettings", hdr);
    return j;
}

ExportConfig fromJson(const QJsonObject &j)
{
    ExportConfig c;
    c.videoCodec = j.value("videoCodec").toString(c.videoCodec);
    c.audioCodec = j.value("audioCodec").toString(c.audioCodec);
    c.container = j.value("container").toString(c.container);
    c.videoBitrate = j.value("videoBitrate").toInt(c.videoBitrate);
    c.audioBitrate = j.value("audioBitrate").toInt(c.audioBitrate);
    c.width = j.value("width").toInt(c.width);
    c.height = j.value("height").toInt(c.height);
    c.fps = j.value("fps").toInt(c.fps);
    c.useHardwareAccel = j.value("useHardwareAccel").toBool(c.useHardwareAccel);
    c.hwEncoder = j.value("hwEncoder").toString(c.hwEncoder);
    c.maxFileSizeMB = j.value("maxFileSizeMB").toInt(c.maxFileSizeMB);
    c.hdr10 = j.value("hdr10").toBool(c.hdr10);
    c.proresProfile = j.value("proresProfile").toInt(c.proresProfile);
    c.exportMarkedRangeOnly = j.value("exportMarkedRangeOnly").toBool(c.exportMarkedRangeOnly);
    c.audioOnly = j.value("audioOnly").toBool(c.audioOnly);
    c.crf = j.value("crf").toInt(c.crf);
    c.rateControl = j.value("rateControl").toString() == "crf" ? ExportConfig::RateControl::Crf : ExportConfig::RateControl::Bitrate;
    const auto hdr = j.value("hdrSettings").toObject();
    c.hdrSettings.mode = hdr.value("mode").toString(c.hdrSettings.mode);
    c.hdrSettings.masterDisplayLuminanceMin = hdr.value("masterDisplayLuminanceMin").toDouble(c.hdrSettings.masterDisplayLuminanceMin);
    c.hdrSettings.masterDisplayLuminanceMax = hdr.value("masterDisplayLuminanceMax").toDouble(c.hdrSettings.masterDisplayLuminanceMax);
    c.hdrSettings.maxCll = hdr.value("maxCll").toInt(c.hdrSettings.maxCll);
    c.hdrSettings.maxFall = hdr.value("maxFall").toInt(c.hdrSettings.maxFall);
    c.hdrSettings.previewToneMap = hdr.value("previewToneMap").toString(c.hdrSettings.previewToneMap);
    return c;
}

QVector<ExportUserPreset> load()
{
    QVector<ExportUserPreset> result;
    const QDir dir(directory());
    for (const QString &entry : dir.entryList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile file(dir.filePath(entry));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto doc = QJsonDocument::fromJson(file.readAll());
        const auto obj = doc.object();
        const QString name = obj.value("name").toString();
        if (name.trimmed().isEmpty() || !obj.value("config").isObject()) continue;
        result.append({name, fromJson(obj.value("config").toObject())});
    }
    return result;
}

bool save(const ExportUserPreset &preset, QString *error)
{
    if (preset.name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("プリセット名を入力してください");
        return false;
    }
    if (!QDir().mkpath(directory())) {
        if (error) *error = QStringLiteral("プリセット保存先を作成できません");
        return false;
    }
    QSaveFile file(pathFor(preset.name));
    const QByteArray data = QJsonDocument(QJsonObject{{"name", preset.name}, {"config", toJson(preset.config)}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool remove(const QString &name, QString *error)
{
    QFile file(pathFor(name));
    if (file.remove()) return true;
    if (error) *error = file.errorString();
    return false;
}

bool resolve(const QString &name, ExportConfig *config, QString *error)
{
    for (const auto &p : ExportDialog::presets()) {
        if (p.name != name) continue;
        ExportConfig c = *config; // Built-ins retain project resolution and fps.
        c.videoCodec = p.videoCodec;
        c.audioCodec = p.audioCodec;
        c.container = p.container;
        c.videoBitrate = p.videoBitrate;
        c.audioBitrate = p.audioBitrate;
        c.maxFileSizeMB = p.maxFileSizeMB;
        c.hdr10 = p.hdr10;
        c.proresProfile = p.proresProfile;
        if (p.proresProfile >= 0) { c.hwEncoder = "none"; c.useHardwareAccel = false; }
        *config = c;
        return true;
    }
    for (const auto &p : load()) {
        if (p.name == name) { *config = p.config; return true; }
    }
    if (error) *error = QStringLiteral("プリセットが見つかりません: %1").arg(name);
    return false;
}
}
