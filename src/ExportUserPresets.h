#pragma once
#include "ExportDialog.h"
#include <QJsonObject>

struct ExportUserPreset {
    QString name;
    ExportConfig config;
};

namespace ExportUserPresets {
QString directory();
QJsonObject toJson(const ExportConfig &config);
ExportConfig fromJson(const QJsonObject &json);
QVector<ExportUserPreset> load();
bool save(const ExportUserPreset &preset, QString *error = nullptr);
bool remove(const QString &name, QString *error = nullptr);
bool resolve(const QString &name, ExportConfig *config, QString *error = nullptr);
}
