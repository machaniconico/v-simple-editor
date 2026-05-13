#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct PluginInfo {
    QString id;
    QString name;
    QString version;
    QString category;
    QString vendor;
    QString description;
    QString manifestPath;
    QStringList paramNames;
    QStringList tags;
};

class PluginManifestScanner : public QObject {
    Q_OBJECT
public:
    static QVector<PluginInfo> scanFolder(const QString& dir, int maxDepth = 3);
    static QStringList lastWarnings();
};
