#include "NodePropertiesPanel.h"

#include "NodeLibrary.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

void deleteLayoutItemTree(QLayoutItem *item)
{
    if (!item) {
        return;
    }

    if (QWidget *widget = item->widget()) {
        widget->hide();
        widget->deleteLater();
    } else if (QLayout *layout = item->layout()) {
        while (QLayoutItem *child = layout->takeAt(0)) {
            deleteLayoutItemTree(child);
        }
    }

    delete item;
}

} // namespace

NodePropertiesPanel::NodePropertiesPanel(QWidget *parent)
    : QScrollArea(parent)
{
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWidgetResizable(true);

    m_formWidget = new QWidget(this);
    m_layout = new QVBoxLayout(m_formWidget);
    m_layout->setContentsMargins(8, 8, 8, 8);
    m_layout->setSpacing(6);
    m_layout->addStretch();

    setWidget(m_formWidget);

    buildPlaceholder();
}

void NodePropertiesPanel::setSelection(NodeGraph *graph, int nodeId)
{
    if (m_graph == graph && m_nodeId == nodeId) {
        return;
    }

    m_graph = graph;
    m_nodeId = nodeId;

    clearForm();

    if (!graph || nodeId < 0) {
        buildPlaceholder();
        return;
    }

    buildForm(graph, nodeId);
}

void NodePropertiesPanel::clearForm()
{
    while (m_layout->count() > 0) {
        deleteLayoutItemTree(m_layout->takeAt(0));
    }
}

void NodePropertiesPanel::buildPlaceholder()
{
    auto *label = new QLabel(tr("\u30ce\u30fc\u30c9\u672a\u9078\u629e"), m_formWidget);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color: palette(mid); padding: 24px;"));
    m_layout->insertWidget(0, label);
}

void NodePropertiesPanel::buildForm(NodeGraph *graph, int nodeId)
{
    GraphNode *node = graph->node(nodeId);
    if (!node) {
        buildPlaceholder();
        return;
    }

    const nodelib::NodeTypeDescriptor *desc =
        nodelib::NodeRegistry::instance().descriptor(node->typeName);

    auto *titleLabel = new QLabel(
        QStringLiteral("%1 (%2)").arg(node->displayName, node->typeName),
        m_formWidget);
    QFont font = titleLabel->font();
    font.setBold(true);
    titleLabel->setFont(font);
    titleLabel->setStyleSheet(QStringLiteral("padding-bottom: 4px;"));
    m_layout->insertWidget(0, titleLabel);

    auto *formLayout = new QFormLayout();
    formLayout->setSpacing(4);
    formLayout->setContentsMargins(0, 4, 0, 0);

    const QVariantMap defaultsMap = desc ? desc->defaultParams : QVariantMap();
    const QStringList keys = node->params.keys();

    for (const QString &key : keys) {
        const QVariant value = node->params.value(key);

        QWidget *editorWidget = nullptr;

        if (value.userType() == qMetaTypeId<QColor>()) {
            const QColor currentColor = value.value<QColor>();
            auto *colorBtn = new QPushButton(m_formWidget);
            colorBtn->setText(QStringLiteral("     "));
            colorBtn->setFixedSize(32, 24);
            QPalette pal = colorBtn->palette();
            pal.setColor(QPalette::Button, currentColor);
            colorBtn->setAutoFillBackground(true);
            colorBtn->setPalette(pal);
            colorBtn->setStyleSheet(
                QStringLiteral("QPushButton { border: 1px solid palette(mid); "
                               "border-radius: 3px; background-color: %1; }")
                    .arg(currentColor.name()));

            connect(colorBtn, &QPushButton::clicked, this,
                    [this, nodeId, key, colorBtn]() {
                        const QColor oldColor =
                            colorBtn->palette().color(QPalette::Button);
                        QColor newColor =
                            QColorDialog::getColor(oldColor, this, QString(),
                                                   QColorDialog::DontUseNativeDialog);
                        if (!newColor.isValid()) {
                            return;
                        }
                        QPalette pal = colorBtn->palette();
                        pal.setColor(QPalette::Button, newColor);
                        colorBtn->setPalette(pal);
                        colorBtn->setStyleSheet(
                            QStringLiteral("QPushButton { border: 1px solid "
                                           "palette(mid); border-radius: 3px; "
                                           "background-color: %1; }")
                                .arg(newColor.name()));
                        emit paramChanged(nodeId, key, QVariant::fromValue(newColor));
                    });

            editorWidget = colorBtn;

        } else if (value.userType() == QMetaType::Double) {
            auto *spinBox = new QDoubleSpinBox(m_formWidget);
            spinBox->setRange(-1000.0, 1000.0);
            spinBox->setSingleStep(0.1);
            spinBox->setDecimals(4);
            spinBox->setValue(value.toDouble());

            connect(spinBox,
                    qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [this, nodeId, key](double v) {
                        emit paramChanged(nodeId, key, QVariant::fromValue(v));
                    });

            editorWidget = spinBox;

        } else if (value.userType() == QMetaType::Int
                   || value.userType() == QMetaType::LongLong
                   || value.userType() == QMetaType::UInt
                   || value.userType() == QMetaType::ULongLong) {
            auto *spinBox = new QSpinBox(m_formWidget);
            spinBox->setRange(-10000, 10000);
            spinBox->setValue(value.toInt());

            connect(spinBox, qOverload<int>(&QSpinBox::valueChanged), this,
                    [this, nodeId, key](int v) {
                        emit paramChanged(nodeId, key, QVariant::fromValue(v));
                    });

            editorWidget = spinBox;

        } else if (value.userType() == QMetaType::Bool) {
            auto *checkBox = new QCheckBox(m_formWidget);
            checkBox->setChecked(value.toBool());

            connect(checkBox, &QCheckBox::toggled, this,
                    [this, nodeId, key](bool v) {
                        emit paramChanged(nodeId, key, QVariant::fromValue(v));
                    });

            editorWidget = checkBox;

        } else if (value.userType() == QMetaType::QString) {
            const QString strVal = value.toString();

            QStringList allowedValues;
            const QString valuesKey = key + QStringLiteral("__values");
            if (defaultsMap.contains(valuesKey)) {
                const QVariant v = defaultsMap.value(valuesKey);
                if (v.typeId() == QMetaType::QStringList) {
                    allowedValues = v.toStringList();
                } else if (v.typeId() == QMetaType::QVariantList) {
                    for (const QVariant &item : v.toList()) {
                        allowedValues.append(item.toString());
                    }
                }
            }

            if (!allowedValues.isEmpty()) {
                auto *comboBox = new QComboBox(m_formWidget);
                comboBox->addItems(allowedValues);
                comboBox->setCurrentText(strVal);

                connect(comboBox, &QComboBox::currentTextChanged, this,
                        [this, nodeId, key](const QString &v) {
                            emit paramChanged(nodeId, key, QVariant::fromValue(v));
                        });

                editorWidget = comboBox;
            } else {
                auto *lineEdit = new QLineEdit(m_formWidget);
                lineEdit->setText(strVal);

                connect(lineEdit, &QLineEdit::textEdited, this,
                        [this, nodeId, key](const QString &v) {
                            emit paramChanged(nodeId, key, QVariant::fromValue(v));
                        });

                editorWidget = lineEdit;
            }

        } else {
            auto *label = new QLabel(value.toString(), m_formWidget);
            editorWidget = label;
        }

        if (editorWidget) {
            formLayout->addRow(key, editorWidget);
        }
    }

    m_layout->insertLayout(1, formLayout);
    m_layout->addStretch();
}
