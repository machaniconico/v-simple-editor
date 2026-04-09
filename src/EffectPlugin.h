#pragma once

#include <QString>
#include <QImage>
#include <QVector>
#include <QPair>
#include <memory>

// --- Abstract Effect Plugin Interface ---

class EffectPlugin
{
public:
    virtual ~EffectPlugin() = default;

    virtual QString name() const = 0;
    virtual QString category() const = 0;  // "Color", "Distort", "Stylize", etc.
    virtual QString description() const = 0;

    // Parameter definition: name, min, max, default
    struct ParamDef {
        QString name;
        double min;
        double max;
        double defaultValue;
    };
    virtual QVector<ParamDef> parameterDefs() const = 0;

    // Process a frame with the given parameter values
    virtual QImage process(const QImage &input, const QVector<double> &params) const = 0;
};

// --- Plugin Registry (singleton) ---

class PluginRegistry
{
public:
    static PluginRegistry &instance();

    void registerPlugin(std::shared_ptr<EffectPlugin> plugin);
    void unregisterPlugin(const QString &name);

    QVector<std::shared_ptr<EffectPlugin>> allPlugins() const;
    QVector<std::shared_ptr<EffectPlugin>> pluginsByCategory(const QString &category) const;
    std::shared_ptr<EffectPlugin> findByName(const QString &name) const;

    QStringList categories() const;
    int count() const { return m_plugins.size(); }

private:
    PluginRegistry();
    void registerBuiltins();

    QVector<std::shared_ptr<EffectPlugin>> m_plugins;
};

// --- Built-in Plugin Implementations ---

class GlowPlugin : public EffectPlugin {
public:
    QString name() const override { return "Glow"; }
    QString category() const override { return "Stylize"; }
    QString description() const override { return "Adds a soft glow effect"; }
    QVector<ParamDef> parameterDefs() const override {
        return { {"Radius", 1.0, 30.0, 10.0}, {"Intensity", 0.0, 1.0, 0.5} };
    }
    QImage process(const QImage &input, const QVector<double> &params) const override;
};

class EmbossPlugin : public EffectPlugin {
public:
    QString name() const override { return "Emboss"; }
    QString category() const override { return "Stylize"; }
    QString description() const override { return "Creates an embossed 3D look"; }
    QVector<ParamDef> parameterDefs() const override {
        return { {"Strength", 0.0, 5.0, 1.0} };
    }
    QImage process(const QImage &input, const QVector<double> &params) const override;
};

class PosterizePlugin : public EffectPlugin {
public:
    QString name() const override { return "Posterize"; }
    QString category() const override { return "Stylize"; }
    QString description() const override { return "Reduces color levels for a poster effect"; }
    QVector<ParamDef> parameterDefs() const override {
        return { {"Levels", 2.0, 32.0, 4.0} };
    }
    QImage process(const QImage &input, const QVector<double> &params) const override;
};

class EdgeDetectPlugin : public EffectPlugin {
public:
    QString name() const override { return "Edge Detect"; }
    QString category() const override { return "Stylize"; }
    QString description() const override { return "Highlights edges in the image (Sobel)"; }
    QVector<ParamDef> parameterDefs() const override {
        return { {"Threshold", 0.0, 255.0, 30.0} };
    }
    QImage process(const QImage &input, const QVector<double> &params) const override;
};

class ColorShiftPlugin : public EffectPlugin {
public:
    QString name() const override { return "Color Shift"; }
    QString category() const override { return "Color"; }
    QString description() const override { return "Shifts RGB channels independently"; }
    QVector<ParamDef> parameterDefs() const override {
        return { {"Red Shift", -100.0, 100.0, 0.0},
                 {"Green Shift", -100.0, 100.0, 0.0},
                 {"Blue Shift", -100.0, 100.0, 0.0} };
    }
    QImage process(const QImage &input, const QVector<double> &params) const override;
};
