#pragma once

#include "Expression.h"

#include <utility>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace exprbind {

// ---------------------------------------------------------------------------
// Property-path registry
// ---------------------------------------------------------------------------

struct PropertyPathInfo {
    QString path;
    QString label;
};

QVector<PropertyPathInfo> knownPropertyPathInfos();
QStringList knownPropertyPaths();

// ---------------------------------------------------------------------------
// Path validator
// ---------------------------------------------------------------------------

// Returns empty string when propPath is a known canonical path.
// Returns a short error string otherwise.
QString validatePath(const QString &propPath);

// ---------------------------------------------------------------------------
// Per-clip expression bindings (propPath -> expression code)
// ---------------------------------------------------------------------------

class ClipExpressionBindings
{
public:
    ClipExpressionBindings() = default;

    // Bind an expression to a property path.
    // Passing an empty/whitespace-only code string is equivalent to clearing.
    void setExpression(const QString &propPath, const QString &code);

    // Remove the binding for propPath (no-op if not bound).
    void clearExpression(const QString &propPath);

    // Returns true when a non-empty expression is bound for propPath.
    bool hasExpression(const QString &propPath) const;

    // Returns the bound code string, or empty string if not bound.
    QString expression(const QString &propPath) const;

    // Returns the list of currently bound property paths.
    QStringList boundPaths() const;

    // Evaluate the expression bound to propPath given the provided context and
    // the upstream keyframe value.  Falls back to keyframeValue when:
    //   - no expression is bound for propPath, OR
    //   - the bound code is empty/whitespace, OR
    //   - expression evaluation fails (Expression::evaluate returns !success), OR
    //   - the resulting value is not finite (NaN / inf).
    // Never throws; never returns NaN or inf.
    double resolve(const QString &propPath,
                   const ExpressionContext &ctx,
                   double keyframeValue) const;

    // Runtime-only source sampler, supplied by MainWindow; never serialized.
    void setAudioLevelSampler(std::function<double(double)> sampler) {
        m_audioLevelAtTime = std::move(sampler);
    }

    // Serialisation: JSON object mapping propPath -> code for every bound path.
    QJsonObject toJson() const;

    // Deserialisation: reads a JSON object mapping propPath -> code.
    // Only accepts string values; ignores non-string entries (lenient).
    // An empty or absent JSON object produces an empty bindings set.
    void fromJson(const QJsonObject &obj);

    // True when no bindings are stored.
    bool isEmpty() const;

private:
    std::function<double(double)> m_audioLevelAtTime;
    QHash<QString, QString> m_bindings; // propPath -> expression code
};

} // namespace exprbind
