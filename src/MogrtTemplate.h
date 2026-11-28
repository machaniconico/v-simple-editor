#pragma once

#include <QColor>
#include <QDialog>
#include <QImage>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QVector>

class QComboBox;
class QFormLayout;
class QLabel;
class QWidget;
class Timeline;

namespace mogrt {

enum class TemplateStyle {
    LowerThird = 0,
    SocialBadge,
    CountdownTimer,
    ProgressChapter,
    SubscribeBanner
};

enum class ControlType {
    Text = 0,
    Color,
    Number,
    Bool
};

struct MogrtControl {
    QString key;
    QString label;
    ControlType type = ControlType::Text;
    QString textValue;
    QColor colorValue = Qt::white;
    double numberValue = 0.0;
    double minValue = 0.0;
    double maxValue = 100.0;
    bool boolValue = false;
};

struct MogrtTemplate {
    QString id;
    QString name;
    QString category;
    TemplateStyle style = TemplateStyle::LowerThird;
    int width = 1920;
    int height = 1080;
    double fps = 30.0;
    double durationSec = 5.0;
    QVector<MogrtControl> controls;

    QString textParam(const QString &key, const QString &fallback = {}) const;
    QColor colorParam(const QString &key, const QColor &fallback = Qt::white) const;
    double numberParam(const QString &key, double fallback = 0.0) const;
    bool boolParam(const QString &key, bool fallback = false) const;
    void setTextParam(const QString &key, const QString &val);
    void setColorParam(const QString &key, const QColor &val);
    void setNumberParam(const QString &key, double val);
    void setBoolParam(const QString &key, bool val);
};

// Built-in Essential Graphics MOGRT gallery (5 templates).
QVector<MogrtTemplate> builtInTemplates();
MogrtTemplate templateById(const QString &id);

// Serialize/deserialize template JSON (definition.json inside .mogrt ZIP).
QJsonObject templateToJson(const MogrtTemplate &tpl);
MogrtTemplate templateFromJson(const QJsonObject &obj, bool *okOut = nullptr);

// Pure C++ PKZIP `.mogrt` archive export/import (definition.json + CRC-32).
// Also accepts standalone `.mogrt.json` files on import.
bool exportMogrtArchive(const MogrtTemplate &tpl, const QString &filePath, QString *errorOut = nullptr);
MogrtTemplate importMogrtArchive(const QString &filePath, bool *okOut = nullptr, QString *errorOut = nullptr);

// SSOT animated renderer (called directly by ExternalGenerator for `renderer=mogrt`).
QImage renderMogrtFrame(const MogrtTemplate &tpl, double timeSec, QSize outSize);
QImage renderMogrtFromParams(const QJsonObject &params, double timeSec, QSize outSize);
QString buildMogrtGeneratorUri(const MogrtTemplate &tpl);

// Insert a parameterized MOGRT clip onto Timeline video track in 1 Undo step.
bool insertMogrtClip(Timeline *timeline,
                     const MogrtTemplate &tpl,
                     int videoTrackIdx = 0,
                     QString *errorOut = nullptr);

class MogrtBrowserDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MogrtBrowserDialog(Timeline *timeline, QWidget *parent = nullptr);

    MogrtTemplate currentTemplate() const { return m_current; }
    int templateCount() const;

private:
    void selectTemplateIndex(int idx);
    void rebuildControlsForm();
    void updatePreview();
    void onImportFile();
    void onExportFile();
    void onInsertAndAccept();

    Timeline *m_timeline = nullptr;
    QVector<MogrtTemplate> m_templates;
    MogrtTemplate m_current;

    QComboBox *m_templateCombo = nullptr;
    QWidget *m_controlsHost = nullptr;
    QFormLayout *m_controlsForm = nullptr;
    QLabel *m_previewLabel = nullptr;
};

} // namespace mogrt
