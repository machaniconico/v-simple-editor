#include "DeflickerDialog.h"

#include "FrameExport.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QWidget>
#include <QDir>

#include <algorithm>
#include <cmath>
#include <numeric>

class DeflickerGraphWidget : public QWidget
{
public:
    explicit DeflickerGraphWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(220);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAutoFillBackground(true);
    }

    void setSeries(const QVector<double> &before, const QVector<double> &after)
    {
        m_before = before;
        m_after = after;
        update();
    }

    void clearSeries()
    {
        m_before.clear();
        m_after.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(QStringLiteral("#17191d")));

        const QRectF plot = QRectF(rect()).adjusted(48.0, 18.0, -18.0, -34.0);
        painter.setPen(QPen(QColor(QStringLiteral("#3b414b")), 1.0));
        painter.drawRect(plot);

        if (m_before.isEmpty() && m_after.isEmpty()) {
            painter.setPen(QColor(QStringLiteral("#8f98a5")));
            painter.drawText(plot, Qt::AlignCenter,
                             QStringLiteral("解析ボタンで輝度推移を表示"));
            return;
        }

        double minimum = 0.0;
        double maximum = 255.0;
        bool firstValue = true;
        auto includeSeries = [&](const QVector<double> &values) {
            for (const double value : values) {
                if (!std::isfinite(value))
                    continue;
                if (firstValue) {
                    minimum = maximum = value;
                    firstValue = false;
                } else {
                    minimum = qMin(minimum, value);
                    maximum = qMax(maximum, value);
                }
            }
        };
        includeSeries(m_before);
        includeSeries(m_after);
        if (firstValue) {
            minimum = 0.0;
            maximum = 255.0;
        }
        if (maximum - minimum < 1.0) {
            const double center = (maximum + minimum) * 0.5;
            minimum = center - 1.0;
            maximum = center + 1.0;
        } else {
            const double padding = (maximum - minimum) * 0.08;
            minimum -= padding;
            maximum += padding;
        }
        minimum = qMax(0.0, minimum);
        maximum = qMin(255.0, maximum);
        if (maximum <= minimum)
            maximum = minimum + 1.0;

        painter.setPen(QPen(QColor(QStringLiteral("#30353d")), 1.0,
                            Qt::DashLine));
        for (int i = 1; i < 4; ++i) {
            const double y = plot.top() + plot.height() * i / 4.0;
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        painter.setPen(QColor(QStringLiteral("#8f98a5")));
        painter.drawText(QRectF(4.0, plot.top() - 8.0, 40.0, 18.0),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(maximum, 'f', 0));
        painter.drawText(QRectF(4.0, plot.bottom() - 8.0, 40.0, 18.0),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(minimum, 'f', 0));

        auto drawSeries = [&](const QVector<double> &values, const QColor &color) {
            if (values.isEmpty())
                return;
            QPainterPath path;
            const int count = values.size();
            for (int i = 0; i < count; ++i) {
                const double value = std::isfinite(values.at(i))
                    ? values.at(i) : minimum;
                const double x = count <= 1
                    ? plot.center().x()
                    : plot.left() + plot.width() * i / (count - 1.0);
                const double t = (value - minimum) / (maximum - minimum);
                const double y = plot.bottom() - plot.height() * qBound(0.0, t, 1.0);
                if (i == 0)
                    path.moveTo(x, y);
                else
                    path.lineTo(x, y);
            }
            painter.setPen(QPen(color, 2.0));
            painter.drawPath(path);
        };

        drawSeries(m_before, QColor(QStringLiteral("#8f98a5")));
        drawSeries(m_after, QColor(QStringLiteral("#4da3ff")));

        const QRectF legend(plot.left(), rect().bottom() - 27.0,
                            plot.width(), 20.0);
        painter.setPen(QPen(QColor(QStringLiteral("#8f98a5")), 2.0));
        painter.drawLine(QPointF(legend.left(), legend.center().y()),
                         QPointF(legend.left() + 20.0, legend.center().y()));
        painter.setPen(QColor(QStringLiteral("#c8cdd5")));
        painter.drawText(QRectF(legend.left() + 28.0, legend.top(), 70.0, legend.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("処理前"));
        const double secondX = legend.left() + 112.0;
        painter.setPen(QPen(QColor(QStringLiteral("#4da3ff")), 2.0));
        painter.drawLine(QPointF(secondX, legend.center().y()),
                         QPointF(secondX + 20.0, legend.center().y()));
        painter.setPen(QColor(QStringLiteral("#c8cdd5")));
        painter.drawText(QRectF(secondX + 28.0, legend.top(), 70.0, legend.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("補正後"));
    }

private:
    QVector<double> m_before;
    QVector<double> m_after;
};

namespace {

QSpinBox *makePixelSpin(QWidget *parent, int value, int maximum)
{
    auto *spin = new QSpinBox(parent);
    spin->setRange(0, maximum);
    spin->setValue(value);
    spin->setSuffix(QStringLiteral(" px"));
    return spin;
}

QDoubleSpinBox *makeGainSpin(QWidget *parent, double value)
{
    auto *spin = new QDoubleSpinBox(parent);
    spin->setRange(0.0, 8.0);
    spin->setDecimals(2);
    spin->setSingleStep(0.05);
    spin->setValue(value);
    return spin;
}

} // namespace

DeflickerDialog::DeflickerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("フリッカー除去"));
    setModal(true);
    resize(980, 760);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *scopeGroup = new QGroupBox(QStringLiteral("対象"), this);
    auto *scopeForm = new QFormLayout(scopeGroup);
    m_clipValue = new QLabel(QStringLiteral("未設定"), scopeGroup);
    m_clipValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    scopeForm->addRow(QStringLiteral("クリップ:"), m_clipValue);

    auto *rangeRow = new QHBoxLayout;
    m_startSpin = new QSpinBox(scopeGroup);
    m_endSpin = new QSpinBox(scopeGroup);
    m_startSpin->setSuffix(QStringLiteral(" frame"));
    m_endSpin->setSuffix(QStringLiteral(" frame"));
    rangeRow->addWidget(new QLabel(QStringLiteral("開始"), scopeGroup));
    rangeRow->addWidget(m_startSpin);
    rangeRow->addSpacing(8);
    rangeRow->addWidget(new QLabel(QStringLiteral("終了"), scopeGroup));
    rangeRow->addWidget(m_endSpin);
    rangeRow->addStretch(1);
    scopeForm->addRow(QStringLiteral("フレーム範囲:"), rangeRow);
    root->addWidget(scopeGroup);

    auto *graphGroup = new QGroupBox(QStringLiteral("輝度推移"), this);
    auto *graphLayout = new QVBoxLayout(graphGroup);
    m_graph = new DeflickerGraphWidget(graphGroup);
    graphLayout->addWidget(m_graph, 1);
    m_statusLabel = new QLabel(
        QStringLiteral("解析前。範囲と設定を確認して解析を実行してください。"),
        graphGroup);
    m_statusLabel->setWordWrap(true);
    graphLayout->addWidget(m_statusLabel);
    root->addWidget(graphGroup, 1);

    auto *settingsGroup = new QGroupBox(QStringLiteral("補正設定"), this);
    auto *settingsForm = new QFormLayout(settingsGroup);
    m_modeCombo = new QComboBox(settingsGroup);
    m_modeCombo->addItem(QStringLiteral("全体輝度"),
                         static_cast<int>(deflicker::Mode::GlobalLuma));
    m_modeCombo->addItem(QStringLiteral("RGB 個別"),
                         static_cast<int>(deflicker::Mode::GlobalRgb));
    m_modeCombo->addItem(QStringLiteral("ローリングバンド"),
                         static_cast<int>(deflicker::Mode::RollingBands));
    settingsForm->addRow(QStringLiteral("モード:"), m_modeCombo);

    m_windowSpin = new QSpinBox(settingsGroup);
    m_windowSpin->setRange(1, 99);
    m_windowSpin->setSingleStep(2);
    m_windowSpin->setValue(9);
    m_windowSpin->setSuffix(QStringLiteral(" frame"));
    settingsForm->addRow(QStringLiteral("時間窓:"), m_windowSpin);

    auto *strengthRow = new QHBoxLayout;
    m_strengthSlider = new QSlider(Qt::Horizontal, settingsGroup);
    m_strengthSlider->setRange(0, 100);
    m_strengthSlider->setValue(100);
    m_strengthValue = new QLabel(QStringLiteral("100 %"), settingsGroup);
    m_strengthValue->setMinimumWidth(52);
    strengthRow->addWidget(m_strengthSlider, 1);
    strengthRow->addWidget(m_strengthValue);
    settingsForm->addRow(QStringLiteral("補正強度:"), strengthRow);

    auto *gainRow = new QHBoxLayout;
    m_minGainSpin = makeGainSpin(settingsGroup, 0.5);
    m_maxGainSpin = makeGainSpin(settingsGroup, 2.0);
    gainRow->addWidget(new QLabel(QStringLiteral("下限"), settingsGroup));
    gainRow->addWidget(m_minGainSpin);
    gainRow->addSpacing(8);
    gainRow->addWidget(new QLabel(QStringLiteral("上限"), settingsGroup));
    gainRow->addWidget(m_maxGainSpin);
    gainRow->addStretch(1);
    settingsForm->addRow(QStringLiteral("ゲイン:"), gainRow);

    m_medianCheck = new QCheckBox(QStringLiteral("目標値に中央値を使う"), settingsGroup);
    m_medianCheck->setChecked(true);
    settingsForm->addRow(QString(), m_medianCheck);

    m_bandHeightSpin = new QSpinBox(settingsGroup);
    m_bandHeightSpin->setRange(1, 2048);
    m_bandHeightSpin->setValue(8);
    m_bandHeightSpin->setSuffix(QStringLiteral(" px"));
    settingsForm->addRow(QStringLiteral("帯の高さ:"), m_bandHeightSpin);

    auto *bandSmoothRow = new QHBoxLayout;
    m_bandSmoothingSlider = new QSlider(Qt::Horizontal, settingsGroup);
    m_bandSmoothingSlider->setRange(0, 100);
    m_bandSmoothingSlider->setValue(50);
    m_bandSmoothingValue = new QLabel(QStringLiteral("50 %"), settingsGroup);
    m_bandSmoothingValue->setMinimumWidth(52);
    bandSmoothRow->addWidget(m_bandSmoothingSlider, 1);
    bandSmoothRow->addWidget(m_bandSmoothingValue);
    settingsForm->addRow(QStringLiteral("帯間平滑化:"), bandSmoothRow);
    root->addWidget(settingsGroup);

    auto *regionGroup = new QGroupBox(QStringLiteral("解析領域"), this);
    auto *regionLayout = new QVBoxLayout(regionGroup);
    m_useMaskCheck = new QCheckBox(QStringLiteral("現在のマスクを使う"), regionGroup);
    m_useMaskCheck->setEnabled(false);
    regionLayout->addWidget(m_useMaskCheck);
    auto *regionRow = new QHBoxLayout;
    m_regionXSpin = makePixelSpin(regionGroup, 0, 100000);
    m_regionYSpin = makePixelSpin(regionGroup, 0, 100000);
    m_regionWidthSpin = makePixelSpin(regionGroup, 0, 100000);
    m_regionHeightSpin = makePixelSpin(regionGroup, 0, 100000);
    regionRow->addWidget(new QLabel(QStringLiteral("X"), regionGroup));
    regionRow->addWidget(m_regionXSpin);
    regionRow->addWidget(new QLabel(QStringLiteral("Y"), regionGroup));
    regionRow->addWidget(m_regionYSpin);
    regionRow->addWidget(new QLabel(QStringLiteral("幅"), regionGroup));
    regionRow->addWidget(m_regionWidthSpin);
    regionRow->addWidget(new QLabel(QStringLiteral("高さ"), regionGroup));
    regionRow->addWidget(m_regionHeightSpin);
    regionRow->addStretch(1);
    regionLayout->addLayout(regionRow);
    auto *regionHint = new QLabel(
        QStringLiteral("幅または高さが 0 のときは全画面を解析します。"
                       "マスクはキャンバス座標からフレーム解像度へ自動変換します。"),
        regionGroup);
    regionHint->setStyleSheet(QStringLiteral("color: #8f98a5;"));
    regionLayout->addWidget(regionHint);
    root->addWidget(regionGroup);

    auto *buttonRow = new QHBoxLayout;
    m_analyzeButton = new QPushButton(QStringLiteral("解析"), this);
    m_analyzeButton->setDefault(true);
    m_applyButton = new QPushButton(QStringLiteral("適用"), this);
    m_cancelButton = new QPushButton(QStringLiteral("中止"), this);
    m_cancelButton->setVisible(false);
    m_closeButton = new QPushButton(QStringLiteral("閉じる"), this);
    m_applyButton->setEnabled(false);
    buttonRow->addWidget(m_analyzeButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_cancelButton);
    buttonRow->addWidget(m_applyButton);
    buttonRow->addWidget(m_closeButton);
    root->addLayout(buttonRow);

    connect(m_analyzeButton, &QPushButton::clicked,
            this, &DeflickerDialog::onAnalyzeClicked);
    connect(m_applyButton, &QPushButton::clicked,
            this, &DeflickerDialog::onApplyClicked);
    connect(m_cancelButton, &QPushButton::clicked,
            this, &DeflickerDialog::requestCancel);
    connect(m_closeButton, &QPushButton::clicked,
            this, &QDialog::reject);
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DeflickerDialog::onModeChanged);
    connect(m_useMaskCheck, &QCheckBox::toggled,
            this, &DeflickerDialog::onUseMaskToggled);
    connect(m_strengthSlider, &QSlider::valueChanged, this, [this](int value) {
        m_strengthValue->setText(QStringLiteral("%1 %").arg(value));
        if (!m_busy)
            clearAnalysis();
    });
    connect(m_bandSmoothingSlider, &QSlider::valueChanged, this, [this](int value) {
        m_bandSmoothingValue->setText(QStringLiteral("%1 %").arg(value));
        if (!m_busy)
            clearAnalysis();
    });

    const auto invalidate = [this]() {
        if (!m_busy)
            clearAnalysis();
    };
    connect(m_startSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, invalidate);
    connect(m_endSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, invalidate);
    connect(m_windowSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, invalidate);
    connect(m_minGainSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, invalidate);
    connect(m_maxGainSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, invalidate);
    connect(m_medianCheck, &QCheckBox::toggled, this,
            [invalidate](bool) { invalidate(); });
    connect(m_bandHeightSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, invalidate);
    connect(m_regionXSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, invalidate);
    connect(m_regionYSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, invalidate);
    connect(m_regionWidthSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, invalidate);
    connect(m_regionHeightSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, invalidate);

    updateModeControls();
}

void DeflickerDialog::setContext(
    const deflicker::DeflickerDialogContext &context)
{
    m_context = context;
    m_frameCache.clear();
    m_frameCacheOrder.clear();
    m_maskRegion = context.maskRegion;
    m_maskCanvasSize = context.maskCanvasSize;

    const int frameCount = qMax(1, context.frameCount);
    m_startSpin->setRange(0, frameCount - 1);
    m_endSpin->setRange(0, frameCount - 1);
    const int defaultFrame = qBound(0, context.defaultFrame, frameCount - 1);
    m_startSpin->setValue(0);
    m_endSpin->setValue(frameCount - 1);
    m_clipValue->setText(context.clipLabel.isEmpty()
                             ? QStringLiteral("未設定") : context.clipLabel);

    m_useMaskCheck->setEnabled(!m_maskRegion.isEmpty());
    m_useMaskCheck->setChecked(false);
    m_regionXSpin->setValue(qMax(0, m_maskRegion.x()));
    m_regionYSpin->setValue(qMax(0, m_maskRegion.y()));
    m_regionWidthSpin->setValue(qMax(0, m_maskRegion.width()));
    m_regionHeightSpin->setValue(qMax(0, m_maskRegion.height()));
    Q_UNUSED(defaultFrame);

    const bool canRun = static_cast<bool>(m_context.frameFetcher)
        && static_cast<bool>(applyFrameFetcher())
        && static_cast<bool>(m_context.sequenceImporter)
        && context.frameCount > 0;
    m_applyButton->setEnabled(canRun);
    m_analyzeButton->setEnabled(static_cast<bool>(m_context.frameFetcher)
                                && context.frameCount > 0);
    m_statusLabel->setText(QStringLiteral(
        "解析前。範囲と設定を確認して解析を実行してください。"));
    m_graph->clearSeries();
    updateModeControls();
}

deflicker::DeflickerParams DeflickerDialog::params(const QSize &frameSize) const
{
    deflicker::DeflickerParams result;
    result.mode = static_cast<deflicker::Mode>(
        m_modeCombo->currentData().toInt());
    result.temporalWindow = m_windowSpin->value();
    result.strength = m_strengthSlider->value() / 100.0;
    result.useMedianTarget = m_medianCheck->isChecked();
    result.minGain = m_minGainSpin->value();
    result.maxGain = m_maxGainSpin->value();
    result.analysisRegion = analysisRegion(frameSize);
    result.bandHeight = m_bandHeightSpin->value();
    result.bandSmoothing = m_bandSmoothingSlider->value() / 100.0;
    return result;
}

QRect DeflickerDialog::analysisRegion(const QSize &frameSize) const
{
    if (m_useMaskCheck->isChecked() && !m_maskRegion.isEmpty()) {
        if (frameSize.isEmpty())
            return m_maskRegion;
        return deflicker::scaleAnalysisRegion(
            m_maskRegion, m_maskCanvasSize, frameSize);
    }
    if (m_regionWidthSpin->value() <= 0 || m_regionHeightSpin->value() <= 0)
        return {};
    return QRect(m_regionXSpin->value(), m_regionYSpin->value(),
                 m_regionWidthSpin->value(), m_regionHeightSpin->value());
}

QImage DeflickerDialog::fetchFrame(int frameIndex)
{
    if (frameIndex < 0 || frameIndex >= qMax(1, m_context.frameCount)
        || !m_context.frameFetcher) {
        return {};
    }
    if (m_frameCache.contains(frameIndex)) {
        m_frameCacheOrder.removeAll(frameIndex);
        m_frameCacheOrder.append(frameIndex);
        return m_frameCache.value(frameIndex);
    }
    const QImage image = m_context.frameFetcher(frameIndex);
    m_frameCache.insert(frameIndex, image);
    m_frameCacheOrder.removeAll(frameIndex);
    m_frameCacheOrder.append(frameIndex);
    constexpr int kFrameCacheLimit = 3;
    while (m_frameCacheOrder.size() > kFrameCacheLimit) {
        const int oldest = m_frameCacheOrder.takeFirst();
        m_frameCache.remove(oldest);
    }
    return image;
}

deflicker::FrameFetcher DeflickerDialog::applyFrameFetcher() const
{
    return m_context.sourceFrameFetcher
        ? m_context.sourceFrameFetcher : m_context.frameFetcher;
}

QString DeflickerDialog::outputDirectory(QString *error) const
{
    if (error)
        error->clear();
    QFileInfo sourceInfo(m_context.sourcePath);
    QDir parent = sourceInfo.absoluteDir();
    if (!parent.exists() && !parent.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("クリップのフォルダを作成できません。\n%1")
                .arg(parent.absolutePath());
        return {};
    }

    QString baseName = sourceInfo.completeBaseName().trimmed();
    if (baseName.isEmpty())
        baseName = QStringLiteral("deflicker");
    const QString folderBase = baseName + QStringLiteral("_deflicker");
    QString path = parent.filePath(folderBase);
    int suffix = 2;
    while (QDir(path).exists())
        path = parent.filePath(folderBase + QStringLiteral("_%1").arg(suffix++));
    if (!parent.mkpath(QFileInfo(path).fileName())) {
        if (error)
            *error = QStringLiteral("出力フォルダを作成できません。\n%1").arg(path);
        return {};
    }
    return path;
}

void DeflickerDialog::clearAnalysis()
{
    m_graph->clearSeries();
    if (!m_busy)
        m_statusLabel->setText(QStringLiteral(
            "設定が変更されました。解析を実行すると補正前後の推移を表示します。"));
}

void DeflickerDialog::setBusy(bool busy)
{
    m_busy = busy;
    if (busy)
        m_cancelRequested = false;
    m_analyzeButton->setEnabled(!busy && static_cast<bool>(m_context.frameFetcher));
    m_applyButton->setEnabled(!busy && static_cast<bool>(applyFrameFetcher())
                              && static_cast<bool>(m_context.sequenceImporter));
    m_cancelButton->setVisible(busy);
    m_cancelButton->setEnabled(busy && !m_cancelRequested);
    m_closeButton->setEnabled(!busy);
    m_startSpin->setEnabled(!busy);
    m_endSpin->setEnabled(!busy);
    m_modeCombo->setEnabled(!busy);
    m_windowSpin->setEnabled(!busy);
    m_strengthSlider->setEnabled(!busy);
    m_minGainSpin->setEnabled(!busy);
    m_maxGainSpin->setEnabled(!busy);
    m_medianCheck->setEnabled(!busy);
    m_bandHeightSpin->setEnabled(!busy && m_modeCombo->currentData().toInt()
                                 == static_cast<int>(deflicker::Mode::RollingBands));
    m_bandSmoothingSlider->setEnabled(!busy && m_modeCombo->currentData().toInt()
                                      == static_cast<int>(deflicker::Mode::RollingBands));
    m_useMaskCheck->setEnabled(!busy && !m_maskRegion.isEmpty());
    const bool manualRegion = !busy && !m_useMaskCheck->isChecked();
    m_regionXSpin->setEnabled(manualRegion);
    m_regionYSpin->setEnabled(manualRegion);
    m_regionWidthSpin->setEnabled(manualRegion);
    m_regionHeightSpin->setEnabled(manualRegion);
}

void DeflickerDialog::requestCancel()
{
    if (!m_busy)
        return;
    m_cancelRequested = true;
    m_cancelButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("中止要求を受け付けました。現在のフレームを終了しています..."));
}

void DeflickerDialog::finishCancelled()
{
    setBusy(false);
    m_statusLabel->setText(QStringLiteral("処理を中止しました。タイムラインは変更されていません。"));
}

void DeflickerDialog::closeEvent(QCloseEvent *event)
{
    if (m_busy) {
        requestCancel();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void DeflickerDialog::keyPressEvent(QKeyEvent *event)
{
    if (m_busy && event->key() == Qt::Key_Escape) {
        requestCancel();
        event->ignore();
        return;
    }
    QDialog::keyPressEvent(event);
}

void DeflickerDialog::updateModeControls()
{
    const bool rolling = m_modeCombo->currentData().toInt()
        == static_cast<int>(deflicker::Mode::RollingBands);
    m_bandHeightSpin->setEnabled(!m_busy && rolling);
    m_bandSmoothingSlider->setEnabled(!m_busy && rolling);
}

void DeflickerDialog::onModeChanged(int)
{
    updateModeControls();
    if (!m_busy)
        clearAnalysis();
}

void DeflickerDialog::onUseMaskToggled(bool checked)
{
    if (checked && !m_maskRegion.isEmpty()) {
        m_regionXSpin->setValue(qMax(0, m_maskRegion.x()));
        m_regionYSpin->setValue(qMax(0, m_maskRegion.y()));
        m_regionWidthSpin->setValue(qMax(0, m_maskRegion.width()));
        m_regionHeightSpin->setValue(qMax(0, m_maskRegion.height()));
    }
    const bool manualRegion = !m_busy && !checked;
    m_regionXSpin->setEnabled(manualRegion);
    m_regionYSpin->setEnabled(manualRegion);
    m_regionWidthSpin->setEnabled(manualRegion);
    m_regionHeightSpin->setEnabled(manualRegion);
    if (!m_busy)
        clearAnalysis();
}

void DeflickerDialog::onAnalyzeClicked()
{
    if (m_busy)
        return;

    const int start = m_startSpin->value();
    const int end = m_endSpin->value();
    setBusy(true);
    QImage sample = fetchFrame(start);
    if (sample.isNull()) {
        setBusy(false);
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("フレーム %1 のデコードに失敗しました。")
                                 .arg(start));
        return;
    }

    const QSize frameSize = sample.size();
    sample = QImage();
    const deflicker::DeflickerParams currentParams = params(frameSize);
    const int frameCount = end - start + 1;
    QVector<double> after;
    after.reserve(frameCount);
    const deflicker::FrameFetcher previewFetcher =
        [this, start](int relativeFrame) {
            const int frameIndex = start + relativeFrame;
            m_statusLabel->setText(QStringLiteral(
                "フレーム %1 / %2 を解析用に読み込み中...")
                                       .arg(relativeFrame + 1)
                                       .arg(m_endSpin->value() - m_startSpin->value() + 1));
            QApplication::processEvents();
            if (m_cancelRequested)
                return QImage();
            return fetchFrame(frameIndex);
        };
    const deflicker::StreamingProcessResult result =
        deflicker::processSequenceStreaming(
            frameCount, previewFetcher, currentParams,
            [this, &after, &currentParams](int relativeFrame,
                                           const QImage &corrected, QString *) {
                if (m_cancelRequested)
                    return true;
                m_statusLabel->setText(QStringLiteral(
                    "フレーム %1 を補正結果として解析中...")
                                           .arg(relativeFrame + 1));
                after.append(deflicker::analyzeFrame(
                    corrected, currentParams).lumaMean);
                QApplication::processEvents();
                return true;
            },
            [this]() { return m_cancelRequested; });
    if (result.cancelled) {
        finishCancelled();
        return;
    }
    if (!result.success) {
        setBusy(false);
        QMessageBox::warning(this, windowTitle(), result.error);
        return;
    }

    QVector<double> before;
    before.reserve(static_cast<int>(result.stats.size()));
    for (const deflicker::FrameStats &stats : result.stats)
        before.append(stats.lumaMean);

    m_graph->setSeries(before, after);
    const double beforeMean = before.isEmpty() ? 0.0
        : std::accumulate(before.cbegin(), before.cend(), 0.0)
            / static_cast<double>(before.size());
    const double afterMean = after.isEmpty() ? 0.0
        : std::accumulate(after.cbegin(), after.cend(), 0.0)
            / static_cast<double>(after.size());
    m_statusLabel->setText(QStringLiteral(
        "%1 フレームを解析しました。平均輝度: %2 → %3。")
                               .arg(static_cast<int>(result.stats.size()))
                               .arg(beforeMean, 0, 'f', 2)
                               .arg(afterMean, 0, 'f', 2));
    setBusy(false);
}

void DeflickerDialog::onApplyClicked()
{
    if (m_busy)
        return;
    if (!m_context.sequenceImporter) {
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("新規クリップを取り込む経路が設定されていません。"));
        return;
    }

    const int start = m_startSpin->value();
    const int end = m_endSpin->value();
    if (end < start) {
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("終了フレームは開始フレーム以降にしてください。"));
        return;
    }

    QString error;
    setBusy(true);
    const deflicker::FrameFetcher sourceFetcher = applyFrameFetcher();
    if (!sourceFetcher) {
        setBusy(false);
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("原本フレーム取得経路が設定されていません。"));
        return;
    }

    QImage sample = sourceFetcher(start);
    if (sample.isNull()) {
        setBusy(false);
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("フレーム %1 のデコードに失敗しました。")
                                 .arg(start));
        return;
    }
    const QSize frameSize = sample.size();
    sample = QImage();
    const deflicker::DeflickerParams currentParams = params(frameSize);
    const int frameCount = end - start + 1;

    const QString folder = outputDirectory(&error);
    if (folder.isEmpty()) {
        setBusy(false);
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }

    QStringList paths;
    paths.reserve(frameCount);
    const deflicker::FrameFetcher sourceRangeFetcher =
        [this, sourceFetcher, start, frameCount](int relativeFrame) {
            m_statusLabel->setText(QStringLiteral(
                "フレーム %1 / %2 を原本から読み込み中...")
                                       .arg(relativeFrame + 1)
                                       .arg(frameCount));
            QApplication::processEvents();
            if (m_cancelRequested)
                return QImage();
            return sourceFetcher(start + relativeFrame);
        };
    const auto cleanupOutput = [&folder]() {
        QDir(folder).removeRecursively();
    };
    const deflicker::StreamingProcessResult result =
        deflicker::processSequenceStreaming(
            frameCount, sourceRangeFetcher, currentParams,
            [this, &paths, &folder, frameCount](int relativeFrame,
                                                const QImage &corrected,
                                                QString *saveError) {
                m_statusLabel->setText(QStringLiteral(
                    "フレーム %1 / %2 を PNG 保存中...")
                                           .arg(relativeFrame + 1)
                                           .arg(frameCount));
                QApplication::processEvents();
                if (m_cancelRequested)
                    return true;
                const QString path = QDir(folder).filePath(
                    QStringLiteral("frame_%1.png")
                        .arg(relativeFrame, 6, 10, QLatin1Char('0')));
                if (!frameexport::saveFrameImage(
                        corrected, path, frameexport::ImageFormat::Png, saveError)) {
                    return false;
                }
                paths.append(path);
                return true;
            },
            [this]() { return m_cancelRequested; });

    if (result.cancelled) {
        cleanupOutput();
        finishCancelled();
        return;
    }
    if (!result.success || paths.size() != frameCount) {
        cleanupOutput();
        setBusy(false);
        const QString message = result.error.isEmpty()
            ? QStringLiteral("フレーム補正に失敗しました。")
            : QStringLiteral("フレーム補正に失敗しました。\n%1").arg(result.error);
        QMessageBox::warning(this, windowTitle(), message);
        return;
    }

    if (!m_context.sequenceImporter(paths, qMax(1.0, m_context.fps), &error)) {
        cleanupOutput();
        setBusy(false);
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("新規クリップへの取り込みに失敗しました。\n%1")
                                 .arg(error));
        return;
    }

    m_statusLabel->setText(QStringLiteral(
        "%1 フレームを書き出し、新規クリップへ取り込みました。")
                               .arg(paths.size()));
    setBusy(false);
    accept();
}
