#pragma once
#include <QByteArray>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QVector>

// ---------------------------------------------------------------------------
// projtmpl — Project Template Library
// ---------------------------------------------------------------------------

namespace projtmpl {

struct TemplateMeta {
    QString id;
    QString name;
    QString description;
    QString category;
    int     width   = 1920;
    int     height  = 1080;
    int     fps     = 30;
    bool    builtIn = true;
};

class TemplateLibrary {
public:
    static QVector<TemplateMeta> builtInTemplates();
    static QByteArray            createProjectFromTemplate(const QString &templateId);
    static QString               userTemplateDir();
    static bool                  saveUserTemplate(const TemplateMeta &meta);
    static QVector<TemplateMeta> userTemplates();
    static QVector<TemplateMeta> allTemplates();
};

} // namespace projtmpl
