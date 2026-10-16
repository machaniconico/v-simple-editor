#pragma once

#include <functional>

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

// --- Expression evaluation context ---

struct ExpressionContext {
    double time = 0.0;          // current time in seconds
    int layerIndex = 0;         // index of the layer being evaluated
    double fps = 30.0;          // project frame rate
    double duration = 0.0;      // clip/comp duration in seconds
    int canvasWidth = 1920;     // composition width
    int canvasHeight = 1080;    // composition height
    double value = 0.0;         // current property value (pre-expression)
    // Optional: returns underlying animated-property value at time t.
    // Default empty.  When set, smooth() / loopIn() / loopOut() can sample it.
    std::function<double(double)> sampleValueAtTime;
    // Clip-local seconds; empty means silence.
    std::function<double(double)> audioLevelAtTime;
};

// --- Expression evaluation result ---

struct ExpressionResult {
    double value = 0.0;
    QString error;
    bool success = false;

    static ExpressionResult ok(double v) { return {v, {}, true}; }
    static ExpressionResult fail(const QString &msg) { return {0.0, msg, false}; }
};

// --- Expression evaluator (stateless, static interface) ---

class Expression
{
public:
    // Evaluate an expression string in the given context
    static ExpressionResult evaluate(const QString &expressionString,
                                     const ExpressionContext &context);

    // Validate syntax without evaluating (returns empty string on success)
    static QString validate(const QString &expressionString);

    // List all available built-in functions and variables
    static QStringList availableFunctions();

private:
    Expression() = delete;
};

// --- Per-property expression binding ---

struct PropertyExpression {
    QString propertyName;
    QString expressionCode;
    bool enabled = true;
};

// --- Expression engine (manages multiple property expressions) ---

class ExpressionEngine
{
public:
    ExpressionEngine() = default;

    // --- Expression management ---

    void addExpression(const QString &propertyName, const QString &code);
    void removeExpression(const QString &propertyName);
    void setEnabled(const QString &propertyName, bool enabled);

    bool hasExpression(const QString &propertyName) const;
    const PropertyExpression *expression(const QString &propertyName) const;

    int count() const { return m_expressions.size(); }
    QStringList propertyNames() const;

    // --- Evaluation ---

    // Evaluate all active (enabled) expressions and return name->value map
    QMap<QString, double> evaluateAll(const ExpressionContext &context) const;

    // Evaluate a single property expression
    ExpressionResult evaluateProperty(const QString &propertyName,
                                      const ExpressionContext &context) const;

    // --- Serialisation ---

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    QVector<PropertyExpression> m_expressions;
};
