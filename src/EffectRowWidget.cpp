#include "EffectRowWidget.h"
#include "EffectKeyframeNavBar.h"
#include "EffectKeyframeToggle.h"
#include "EffectParamSchema.h"
#include "Keyframe.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QColorDialog>
#include <QStyle>
#include <QValidator>
#include <cmath>

namespace effectctrl {
namespace {

// ParamDef::defaultVal for Color params carries a packed QRgb (QColor::rgb()
// encoded as double); 0.0 means "no schema default" and keeps the legacy black.
QColor decodeColorDefault(double encodedDefault)
{
    if (encodedDefault != 0.0) {
        const auto rgb = static_cast<QRgb>(static_cast<unsigned int>(std::llround(encodedDefault)));
        return QColor::fromRgb(rgb);
    }
    return QColor(0, 0, 0);
}

class UnlimitedDoubleSpinBox : public QDoubleSpinBox
{
public:
    explicit UnlimitedDoubleSpinBox(QWidget *parent = nullptr)
        : QDoubleSpinBox(parent)
    {
    }

protected:
    QString textFromValue(double value) const override
    {
        if (value <= minimum())
            return QString();
        return QDoubleSpinBox::textFromValue(value);
    }

    double valueFromText(const QString &text) const override
    {
        if (text.trimmed().isEmpty())
            return minimum();
        return QDoubleSpinBox::valueFromText(text);
    }

    QValidator::State validate(QString &input, int &pos) const override
    {
        if (input.trimmed().isEmpty())
            return QValidator::Acceptable;
        return QDoubleSpinBox::validate(input, pos);
    }
};

double normalizedTimingValue(double value)
{
    return value < 0.0 ? -1.0 : value;
}

bool splitColorChannelParam(const QString &paramName, QString *baseParamName, QString *channel)
{
    const int dotPos = paramName.lastIndexOf(QLatin1Char('.'));
    if (dotPos <= 0 || dotPos == paramName.size() - 1)
        return false;

    const QString suffix = paramName.mid(dotPos + 1);
    if (suffix != QStringLiteral("r")
        && suffix != QStringLiteral("g")
        && suffix != QStringLiteral("b")) {
        return false;
    }

    if (baseParamName)
        *baseParamName = paramName.left(dotPos);
    if (channel)
        *channel = suffix;
    return true;
}

double colorChannelValue(const QColor &color, const QString &channel)
{
    if (channel == QStringLiteral("r"))
        return color.red();
    if (channel == QStringLiteral("g"))
        return color.green();
    if (channel == QStringLiteral("b"))
        return color.blue();
    return 0.0;
}

QDoubleSpinBox *createTimingSpinBox(QWidget *parent, double value)
{
    auto *spin = new UnlimitedDoubleSpinBox(parent);
    spin->setRange(-1.0, 999999.0);
    spin->setDecimals(3);
    spin->setSingleStep(1.0);
    spin->setValue(normalizedTimingValue(value));
    spin->setToolTip(QStringLiteral("空欄または -1 で無制限"));
    return spin;
}

} // namespace

EffectRowWidget::EffectRowWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
}

void EffectRowWidget::setEffect(const VideoEffect &effect)
{
    m_effect = effect;
    clearRows();
    auto schema = paramSchemaFor(effect.type);
    buildRows(schema);
}

VideoEffect EffectRowWidget::currentEffect() const
{
    return m_effect;
}

void EffectRowWidget::buildRows(const QVector<ParamDef> &schema)
{
    auto *layout = qobject_cast<QVBoxLayout *>(this->layout());

    auto *timingContainer = new QWidget(this);
    auto *timingLayout = new QHBoxLayout(timingContainer);
    timingLayout->setContentsMargins(2, 1, 2, 1);
    timingLayout->setSpacing(4);

    auto *startLabel = new QLabel(QStringLiteral("開始(秒)"), timingContainer);
    startLabel->setMinimumWidth(64);
    auto *startSpin = createTimingSpinBox(timingContainer, m_effect.startSec);
    auto *endLabel = new QLabel(QStringLiteral("終了(秒)"), timingContainer);
    endLabel->setMinimumWidth(64);
    auto *endSpin = createTimingSpinBox(timingContainer, m_effect.endSec);
    auto *resetButton = new QPushButton(timingContainer);
    resetButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    resetButton->setFixedSize(20, 20);
    resetButton->setToolTip(QStringLiteral("有効区間を無制限に戻す"));

    auto emitTimingValue = [this](const QString &paramName, QDoubleSpinBox *spin, double value) {
        const double normalized = normalizedTimingValue(value);
        if (normalized != value) {
            spin->blockSignals(true);
            spin->setValue(normalized);
            spin->blockSignals(false);
        }
        emit paramChanged(paramName, QVariant(normalized));
    };

    connect(startSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [emitTimingValue, startSpin](double value) {
                emitTimingValue(QStringLiteral("__effectStartSec"), startSpin, value);
            });
    connect(endSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [emitTimingValue, endSpin](double value) {
                emitTimingValue(QStringLiteral("__effectEndSec"), endSpin, value);
            });
    connect(resetButton, &QPushButton::clicked, this, [this, startSpin, endSpin]() {
        startSpin->blockSignals(true);
        endSpin->blockSignals(true);
        startSpin->setValue(-1.0);
        endSpin->setValue(-1.0);
        startSpin->blockSignals(false);
        endSpin->blockSignals(false);
        emit paramChanged(QStringLiteral("__effectTimingReset"), QVariant(true));
    });

    timingLayout->addWidget(startLabel);
    timingLayout->addWidget(startSpin, 1);
    timingLayout->addWidget(endLabel);
    timingLayout->addWidget(endSpin, 1);
    timingLayout->addWidget(resetButton);
    layout->addWidget(timingContainer);

    for (const auto &def : schema) {
        RowWidgets row;
        row.paramName = def.name;
        row.defaultVal = def.defaultVal;

        row.container = new QWidget(this);
        row.containerLayout = new QVBoxLayout(row.container);
        row.containerLayout->setContentsMargins(0, 0, 0, 0);
        row.containerLayout->setSpacing(2);

        auto *rowLayout = new QHBoxLayout();
        rowLayout->setContentsMargins(2, 1, 2, 1);
        rowLayout->setSpacing(4);

        row.kfToggle = new EffectKeyframeToggle(row.container);
        connect(row.kfToggle, &EffectKeyframeToggle::toggled, this, [this, def](bool now) {
            emit keyframeToggled(def.name, now);
        });
        rowLayout->addWidget(row.kfToggle);

        row.label = new QLabel(def.displayLabel, row.container);
        row.label->setMinimumWidth(80);
        rowLayout->addWidget(row.label);

        switch (def.type) {
        case ParamType::Float:
        case ParamType::Int: {
            row.spinBox = new QDoubleSpinBox(row.container);
            row.spinBox->setRange(def.minVal, def.maxVal);
            row.spinBox->setSingleStep(def.type == ParamType::Int ? 1.0 : 0.01);
            row.spinBox->setDecimals(def.type == ParamType::Int ? 0 : 4);
            row.spinBox->setValue(def.defaultVal);

            row.slider = new QSlider(Qt::Horizontal, row.container);
            int sliderRange = 1000;
            row.slider->setRange(0, sliderRange);
            double range = def.maxVal - def.minVal;
            int sliderPos = static_cast<int>((def.defaultVal - def.minVal) / range * sliderRange);
            row.slider->setValue(sliderPos);

            connect(row.spinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, row, def, range, sliderRange](double val) {
                int pos = static_cast<int>((val - def.minVal) / range * sliderRange);
                row.slider->blockSignals(true);
                row.slider->setValue(pos);
                row.slider->blockSignals(false);
                emit paramChanged(def.name, QVariant(val));
            });

            connect(row.slider, &QSlider::valueChanged, this, [this, row, def, range, sliderRange](int pos) {
                double val = def.minVal + (static_cast<double>(pos) / sliderRange) * range;
                if (def.type == ParamType::Int) {
                    val = std::round(val);
                }
                row.spinBox->blockSignals(true);
                row.spinBox->setValue(val);
                row.spinBox->blockSignals(false);
                emit paramChanged(def.name, QVariant(val));
            });

            rowLayout->addWidget(row.spinBox, 1);
            rowLayout->addWidget(row.slider, 2);
            break;
        }
        case ParamType::Bool: {
            row.checkBox = new QCheckBox(row.container);
            row.checkBox->setChecked(def.defaultVal != 0.0);
            connect(row.checkBox, &QCheckBox::toggled, this, [this, def](bool checked) {
                emit paramChanged(def.name, QVariant(checked));
            });
            rowLayout->addWidget(row.checkBox);
            break;
        }
        case ParamType::Color: {
            row.colorButton = new QPushButton(row.container);
            QColor initColor = decodeColorDefault(def.defaultVal);
            if (def.name.contains("color", Qt::CaseInsensitive) || def.name.contains("Color")) {
                initColor = m_effect.keyColor;
            }
            row.colorButton->setStyleSheet(QString("background-color: %1; border: 1px solid #555; border-radius: 3px;")
                                           .arg(initColor.name()));
            row.colorButton->setMinimumSize(32, 20);
            row.colorButton->setMaximumSize(48, 24);
            connect(row.colorButton, &QPushButton::clicked, this, [this, def, row]() {
                QColor currentColor;
                if (def.name.contains("color", Qt::CaseInsensitive) || def.name.contains("Color")) {
                    currentColor = m_effect.keyColor;
                } else {
                    currentColor = row.colorButton->palette().color(QPalette::Button);
                }
                QColor newColor = QColorDialog::getColor(currentColor, this, def.displayLabel);
                if (newColor.isValid()) {
                    row.colorButton->setStyleSheet(QString("background-color: %1; border: 1px solid #555; border-radius: 3px;")
                                                   .arg(newColor.name()));
                    setColorParam(m_effect, def.name, newColor);
                    emit paramChanged(def.name, QVariant(newColor));
                }
            });
            rowLayout->addWidget(row.colorButton);
            rowLayout->addStretch();
            break;
        }
        case ParamType::Choice: {
            row.comboBox = new QComboBox(row.container);
            row.comboBox->addItems(def.choices);
            int defaultIdx = def.choices.indexOf(def.name.isEmpty() ? QString() : QString::number(def.defaultVal));
            if (defaultIdx < 0) defaultIdx = 0;
            row.comboBox->setCurrentIndex(defaultIdx);
            connect(row.comboBox, &QComboBox::currentTextChanged, this, [this, def](const QString &text) {
                emit paramChanged(def.name, QVariant(text));
            });
            rowLayout->addWidget(row.comboBox, 1);
            break;
        }
        }

        row.resetButton = new QPushButton(row.container);
        row.resetButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        row.resetButton->setFixedSize(20, 20);
        row.resetButton->setToolTip("Reset to default");
        connect(row.resetButton, &QPushButton::clicked, this, [this, def, row]() {
            switch (def.type) {
            case ParamType::Float:
            case ParamType::Int:
                if (row.spinBox) {
                    row.spinBox->blockSignals(true);
                    row.spinBox->setValue(def.defaultVal);
                    row.spinBox->blockSignals(false);
                }
                if (row.slider) {
                    double range = def.maxVal - def.minVal;
                    int pos = static_cast<int>((def.defaultVal - def.minVal) / range * 1000);
                    row.slider->blockSignals(true);
                    row.slider->setValue(pos);
                    row.slider->blockSignals(false);
                }
                emit paramChanged(def.name, QVariant(def.defaultVal));
                break;
            case ParamType::Bool:
                if (row.checkBox) {
                    row.checkBox->blockSignals(true);
                    row.checkBox->setChecked(def.defaultVal != 0.0);
                    row.checkBox->blockSignals(false);
                }
                emit paramChanged(def.name, QVariant(def.defaultVal != 0.0));
                break;
            case ParamType::Color: {
                QColor defaultColor = decodeColorDefault(def.defaultVal);
                if (row.colorButton) {
                    row.colorButton->setStyleSheet(QString("background-color: %1; border: 1px solid #555; border-radius: 3px;")
                                                   .arg(defaultColor.name()));
                }
                setColorParam(m_effect, def.name, defaultColor);
                emit paramChanged(def.name, QVariant(defaultColor));
                break;
            }
            case ParamType::Choice:
                if (row.comboBox) {
                    row.comboBox->blockSignals(true);
                    row.comboBox->setCurrentIndex(0);
                    row.comboBox->blockSignals(false);
                }
                if (!def.choices.isEmpty()) {
                    emit paramChanged(def.name, QVariant(def.choices.first()));
                }
                break;
            }
        });
        rowLayout->addWidget(row.resetButton);

        row.containerLayout->addLayout(rowLayout);
        layout->addWidget(row.container);
        m_rows.append(row);
    }
}

void EffectRowWidget::clearRows()
{
    m_rows.clear();
    auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
    while (layout->count() > 0) {
        QLayoutItem *item = layout->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        } else if (item->layout()) {
            delete item->layout();
        }
        delete item;
    }
}

double EffectRowWidget::getParamValue(int rowIdx) const
{
    if (rowIdx < 0 || rowIdx >= m_rows.size()) return 0.0;
    const auto &row = m_rows[rowIdx];
    if (row.spinBox) return row.spinBox->value();
    if (row.checkBox) return row.checkBox->isChecked() ? 1.0 : 0.0;
    return 0.0;
}

void EffectRowWidget::setParamHasTrack(const QString &paramName, bool has)
{
    for (const auto &row : m_rows) {
        if (row.paramName == paramName && row.kfToggle) {
            row.kfToggle->setHasTrack(has);
            return;
        }
    }
}

bool EffectRowWidget::paramHasTrack(const QString &paramName) const
{
    for (const auto &row : m_rows) {
        if (row.paramName == paramName && row.kfToggle) {
            return row.kfToggle->hasTrack();
        }
    }
    return false;
}

double EffectRowWidget::getParamValueByName(const QString &paramName) const
{
    QString baseParamName;
    QString channel;
    const bool colorChannel = splitColorChannelParam(paramName, &baseParamName, &channel);
    const QString lookupName = colorChannel ? baseParamName : paramName;

    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].paramName == lookupName) {
            if (colorChannel && m_rows[i].colorButton) {
                return colorChannelValue(colorParamValue(m_effect, lookupName), channel);
            }
            return getParamValue(i);
        }
    }
    return 0.0;
}

void EffectRowWidget::setParamKeyframeTrack(const QString &paramName, KeyframeTrack *track,
                                            double clipDurationSeconds, double playheadSeconds)
{
    for (auto &row : m_rows) {
        if (row.paramName != paramName) {
            continue;
        }

        const bool shouldShow = row.kfToggle && row.kfToggle->hasTrack() && track;
        if (!shouldShow) {
            if (row.navBar) {
                row.containerLayout->removeWidget(row.navBar);
                delete row.navBar;
                row.navBar = nullptr;
            }
            return;
        }

        if (!row.navBar) {
            row.navBar = new EffectKeyframeNavBar(row.container);
            row.navBar->setFixedHeight(24);
            row.containerLayout->addWidget(row.navBar);
            connect(row.navBar, &EffectKeyframeNavBar::trackChanged, this, [this, paramName]() {
                emit keyframeTrackChanged(paramName);
            });
        }

        row.navBar->setTrack(track, clipDurationSeconds, playheadSeconds);
        return;
    }
}

void EffectRowWidget::setPlayhead(double seconds)
{
    for (auto &row : m_rows) {
        if (row.navBar) {
            row.navBar->setPlayhead(seconds);
        }
    }
}

} // namespace effectctrl
