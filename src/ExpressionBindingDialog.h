#pragma once

#include "ClipExpressionBindings.h"

#include <QDialog>
#include <QSize>

class QComboBox;
class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSplitter;

class ExpressionBindingDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExpressionBindingDialog(QWidget *parent = nullptr);

    // Populate the list from b.boundPaths() + b.expression(path) for each path.
    void setBindings(const exprbind::ClipExpressionBindings &b);

    // Return the current (edited) bindings.
    exprbind::ClipExpressionBindings bindings() const;

    // Store context hints used to build sample ExpressionContext for preview.
    // dynamicTimeOffsetSec is added to the sample time, as the render does on a split right half.
    void setContextHints(double durationSeconds, double fps, QSize canvas,
                         double dynamicTimeOffsetSec = 0.0);

    // Test seam: the sample context the preview evaluates at local time t.
    ExpressionContext sampleContextForTest(double t) const { return buildSampleContext(t); }

signals:
    void bindingsChanged();

private slots:
    void onAddBinding();
    void onRemoveBinding();
    void onSelectionChanged();
    void onExpressionTextChanged();

private:
    // Build sample ExpressionContext for a given time t.
    ExpressionContext buildSampleContext(double t) const;

    // Refresh validity label and evaluated-value read-out for currentCode.
    void updateValidityAndPreview(const QString &code);

    // Left panel
    QListWidget       *m_listWidget   = nullptr;
    QPushButton       *m_addButton    = nullptr;
    QPushButton       *m_removeButton = nullptr;

    // Right panel – expression editor
    QPlainTextEdit    *m_codeEdit     = nullptr;
    QLabel            *m_validityLabel = nullptr;
    QLabel            *m_previewLabel  = nullptr;

    // Functions reference (collapsible)
    QGroupBox         *m_funcBox      = nullptr;
    QListWidget       *m_funcList     = nullptr;

    // OK / Cancel
    QDialogButtonBox  *m_buttonBox    = nullptr;

    // Internal state
    exprbind::ClipExpressionBindings m_bindings;
    bool   m_updating     = false; // guard re-entrant updates

    // Context hints
    double m_duration     = 0.0;
    double m_fps          = 30.0;
    QSize  m_canvas       = QSize(1920, 1080);
    double m_dynamicTimeOffsetSec = 0.0;
};
