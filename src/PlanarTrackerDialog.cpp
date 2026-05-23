#include "PlanarTrackerDialog.h"

#include "PlanarTrackerPresetRegistry.h"

#include <algorithm>

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPolygonF>
#include <QProgressBar>
#include <QPushButton>
#include <QRectF>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtMath>

namespace {

QString makeUserPresetId(const QString& name)
{
    QString slug;
    bool lastWasDash = false;
    const QString lower = name.trimmed().toLower();
    for (const QChar ch : lower) {
        const ushort u = ch.unicode();
        const bool isAsciiLetter = (u >= 'a' && u <= 'z');
        const bool isAsciiDigit = (u >= '0' && u <= '9');
        if (isAsciiLetter || isAsciiDigit) {
            slug.append(ch);
            lastWasDash = false;
        } else if (!lastWasDash) {
            slug.append(QLatin1Char('-'));
            lastWasDash = true;
        }
    }

    while (slug.startsWith(QLatin1Char('-')))
        slug.remove(0, 1);
    while (slug.endsWith(QLatin1Char('-')))
        slug.chop(1);
    if (slug.isEmpty())
        slug = QStringLiteral("preset");

    const QString hash = QString::number(static_cast<qulonglong>(qHash(name)), 16);
    return QStringLiteral("user-%1-%2").arg(slug, hash);
}

} // namespace

// ============================================================================
// PlanarCornerWidget
// ============================================================================

PlanarCornerWidget::PlanarCornerWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Default corners: unit rectangle until a real image is set
    m_corners.tl = QPointF(0, 0);
    m_corners.tr = QPointF(1, 0);
    m_corners.br = QPointF(1, 1);
    m_corners.bl = QPointF(0, 1);
}

// ---------------------------------------------------------------------------
QRectF PlanarCornerWidget::imageRect() const
{
    if (m_image.isNull())
        return QRectF(0, 0, width(), height());

    const double iw = m_image.width();
    const double ih = m_image.height();
    const double ww = width();
    const double wh = height();

    const double scale = qMin(ww / iw, wh / ih);
    const double dw    = iw * scale;
    const double dh    = ih * scale;
    const double ox    = (ww - dw) * 0.5;
    const double oy    = (wh - dh) * 0.5;
    return QRectF(ox, oy, dw, dh);
}

// ---------------------------------------------------------------------------
QPointF PlanarCornerWidget::imageToWidget(const QPointF& imgPt) const
{
    const QRectF r = imageRect();
    if (m_image.isNull())
        return imgPt;
    return QPointF(r.x() + imgPt.x() * r.width()  / m_image.width(),
                   r.y() + imgPt.y() * r.height() / m_image.height());
}

// ---------------------------------------------------------------------------
QPointF PlanarCornerWidget::widgetToImage(const QPointF& widgetPt) const
{
    const QRectF r = imageRect();
    if (m_image.isNull() || r.width() <= 0 || r.height() <= 0)
        return widgetPt;
    return QPointF((widgetPt.x() - r.x()) * m_image.width()  / r.width(),
                   (widgetPt.y() - r.y()) * m_image.height() / r.height());
}

// ---------------------------------------------------------------------------
void PlanarCornerWidget::setReferenceImage(const QImage& image)
{
    m_image = image;
    update();
}

// ---------------------------------------------------------------------------
void PlanarCornerWidget::setCorners(const planar::CornerSet& corners)
{
    m_corners = corners;
    update();
}

// ---------------------------------------------------------------------------
planar::CornerSet PlanarCornerWidget::corners() const
{
    return m_corners;
}

// ---------------------------------------------------------------------------
int PlanarCornerWidget::hitTest(const QPointF& widgetPx) const
{
    const QPointF pts[4] = {
        imageToWidget(m_corners.tl),
        imageToWidget(m_corners.tr),
        imageToWidget(m_corners.br),
        imageToWidget(m_corners.bl),
    };
    int    bestIdx  = -1;
    double bestDist = 12.0;   // pixel threshold
    for (int i = 0; i < 4; ++i) {
        const double dx   = widgetPx.x() - pts[i].x();
        const double dy   = widgetPx.y() - pts[i].y();
        const double dist = qSqrt(dx * dx + dy * dy);
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx  = i;
        }
    }
    return bestIdx;
}

// ---------------------------------------------------------------------------
void PlanarCornerWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Background
    p.fillRect(rect(), QColor(30, 30, 30));

    // Image
    if (!m_image.isNull()) {
        const QRectF r = imageRect();
        p.drawImage(r, m_image);
    }

    // Quadrilateral outline
    const QPointF wPts[4] = {
        imageToWidget(m_corners.tl),
        imageToWidget(m_corners.tr),
        imageToWidget(m_corners.br),
        imageToWidget(m_corners.bl),
    };
    QPen linePen(QColor(0x40, 0x80, 0xff, 0x88));
    linePen.setWidthF(1.5);
    p.setPen(linePen);
    p.setBrush(Qt::NoBrush);
    {
        QPolygonF poly;
        for (const QPointF& pt : wPts)
            poly << pt;
        poly << wPts[0];   // close
        p.drawPolyline(poly);
    }

    // Corner handles
    const char* labels[4] = { "1", "2", "3", "4" };
    QPen circlePen(QColor(0x40, 0x80, 0xff));
    circlePen.setWidthF(2.0);
    p.setPen(circlePen);
    p.setBrush(QColor(0x40, 0x80, 0xff, 80));

    for (int i = 0; i < 4; ++i) {
        const QPointF& wp = wPts[i];
        p.drawEllipse(wp, 6.0, 6.0);

        // Number label slightly offset to top-right of the circle
        p.setPen(QColor(220, 220, 255));
        QFont f = font();
        f.setPointSize(8);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(wp.x() + 7, wp.y() - 12, 14, 12),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromLatin1(labels[i]));
        p.setPen(circlePen);
    }
}

// ---------------------------------------------------------------------------
void PlanarCornerWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragIndex = hitTest(event->position());
        if (m_dragIndex >= 0)
            update();
    }
    QWidget::mousePressEvent(event);
}

// ---------------------------------------------------------------------------
void PlanarCornerWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragIndex >= 0) {
        const QPointF imgPt = widgetToImage(event->position());
        switch (m_dragIndex) {
        case 0: m_corners.tl = imgPt; break;
        case 1: m_corners.tr = imgPt; break;
        case 2: m_corners.br = imgPt; break;
        case 3: m_corners.bl = imgPt; break;
        default: break;
        }
        emit cornersChanged(m_corners);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

// ---------------------------------------------------------------------------
void PlanarCornerWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        m_dragIndex = -1;
    QWidget::mouseReleaseEvent(event);
}

// ============================================================================
// PlanarTrackerDialog
// ============================================================================

PlanarTrackerDialog::PlanarTrackerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("プラナートラッカー"));
    setObjectName(QStringLiteral("planarTrackerDialog"));
    resize(880, 640);

    planar_tracker_preset::Registry::instance().reloadFromSettings();

    m_presetCombo = new QComboBox(this);

    m_descriptionLabel = new QLabel(this);
    m_descriptionLabel->setWordWrap(true);
    m_descriptionLabel->setMinimumHeight(40);
    {
        QPalette p = m_descriptionLabel->palette();
        p.setColor(QPalette::WindowText, p.color(QPalette::Disabled, QPalette::WindowText));
        m_descriptionLabel->setPalette(p);
    }
    m_descriptionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // --- Corner widget (left ~60%) ---
    m_cornerWidget = new PlanarCornerWidget(this);

    // --- Parameters form (right ~40%) ---
    m_patchSizeSpin = new QSpinBox(this);
    m_patchSizeSpin->setRange(16, 128);
    m_patchSizeSpin->setValue(32);
    m_patchSizeSpin->setSuffix(tr(" px"));

    m_searchRadiusSpin = new QSpinBox(this);
    m_searchRadiusSpin->setRange(4, 64);
    m_searchRadiusSpin->setValue(16);
    m_searchRadiusSpin->setSuffix(tr(" px"));

    m_dampingSpin = new QSpinBox(this);
    m_dampingSpin->setRange(0, 100);
    m_dampingSpin->setValue(30);
    m_dampingSpin->setSuffix(tr(" %"));

    m_saveCustomPresetButton = new QPushButton(tr("カスタム preset 保存"), this);
    m_deletePresetBtn = new QPushButton(tr("選択中の preset を削除"), this);
    m_resetPresetButton = new QPushButton(tr("Reset to defaults"), this);
    m_exportPresetButton = new QPushButton(tr("Preset を JSON エクスポート"), this);
    m_importPresetButton = new QPushButton(tr("Preset を JSON インポート"), this);
    m_deletePresetBtn->setEnabled(false);

    m_resetButton = new QPushButton(tr("リセット"), this);
    m_trackButton = new QPushButton(tr("追跡実行"), this);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setVisible(false);

    m_summaryLabel = new QLabel(QStringLiteral("--"), this);
    m_summaryLabel->setWordWrap(true);

    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    // --- Right panel layout ---
    auto* form = new QFormLayout;
    form->addRow(tr("パッチサイズ:"),    m_patchSizeSpin);
    form->addRow(tr("探索半径:"),        m_searchRadiusSpin);
    form->addRow(tr("ダンピング:"),      m_dampingSpin);

    auto* presetButtonsTop = new QHBoxLayout;
    presetButtonsTop->addWidget(m_saveCustomPresetButton);
    presetButtonsTop->addWidget(m_deletePresetBtn);
    presetButtonsTop->addWidget(m_resetPresetButton);

    auto* presetButtonsBottom = new QHBoxLayout;
    presetButtonsBottom->addWidget(m_exportPresetButton);
    presetButtonsBottom->addWidget(m_importPresetButton);

    auto* rightLayout = new QVBoxLayout;
    rightLayout->addWidget(m_presetCombo);
    rightLayout->addWidget(m_descriptionLabel);
    rightLayout->addLayout(presetButtonsTop);
    rightLayout->addLayout(presetButtonsBottom);
    rightLayout->addLayout(form);
    rightLayout->addWidget(m_resetButton);
    rightLayout->addWidget(m_trackButton);
    rightLayout->addWidget(m_progress);
    rightLayout->addWidget(m_summaryLabel);
    rightLayout->addStretch();
    rightLayout->addWidget(m_buttonBox);

    // --- Main horizontal layout ---
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->addWidget(m_cornerWidget, 6);   // ~60%
    mainLayout->addLayout(rightLayout,    4);   // ~40%

    // --- Signal connections ---
    connect(m_cornerWidget, &PlanarCornerWidget::cornersChanged,
            this, [this](const planar::CornerSet& c) {
                m_corners = c;
                rebuildSummary();
            });

    connect(m_presetCombo, &QComboBox::currentIndexChanged,
            this, &PlanarTrackerDialog::onPresetSelectionChanged);
    connect(m_saveCustomPresetButton, &QPushButton::clicked,
            this, &PlanarTrackerDialog::onSaveCustomPreset);
    connect(m_deletePresetBtn, &QPushButton::clicked,
            this, &PlanarTrackerDialog::onDeleteSelectedPreset);
    connect(m_resetPresetButton, &QPushButton::clicked,
            this, &PlanarTrackerDialog::onResetPresetToDefaults);
    connect(m_exportPresetButton, &QPushButton::clicked,
            this, &PlanarTrackerDialog::onExportPreset);
    connect(m_importPresetButton, &QPushButton::clicked,
            this, &PlanarTrackerDialog::onImportPreset);

    connect(m_resetButton, &QPushButton::clicked,
            this, &PlanarTrackerDialog::onResetCorners);
    connect(m_trackButton, &QPushButton::clicked,
            this, &PlanarTrackerDialog::onTrackClicked);

    connect(m_patchSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &PlanarTrackerDialog::onPatchSizeChanged);
    connect(m_searchRadiusSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &PlanarTrackerDialog::onSearchRadiusChanged);
    connect(m_dampingSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &PlanarTrackerDialog::onDampingChanged);

    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &PlanarTrackerDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    rebuildPresetCombo();

    // Sync params with initial spin values
    m_params.patchSizePx    = m_patchSizeSpin->value();
    m_params.searchRadiusPx = m_searchRadiusSpin->value();
    m_params.dampingFactor  = m_dampingSpin->value() / 100.0;

    rebuildSummary();
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::setReferenceFrame(const QImage& frame)
{
    m_reference = frame;
    m_cornerWidget->setReferenceImage(frame);
    onResetCorners();   // reset corners to new image bounds
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::setFrames(const QList<QImage>& frames)
{
    m_frames = frames;
    if (!frames.isEmpty() && m_reference.isNull())
        setReferenceFrame(frames.first());
    rebuildSummary();
}

// ---------------------------------------------------------------------------
planar::CornerSet PlanarTrackerDialog::currentCorners() const
{
    return m_corners;
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::setCorners(const planar::CornerSet& corners)
{
    m_corners = corners;
    m_cornerWidget->setCorners(corners);
}

// ---------------------------------------------------------------------------
QList<planar::Frame> PlanarTrackerDialog::trackResult() const
{
    return m_result;
}

// ---------------------------------------------------------------------------
planar_tracker_preset::PlanarTrackerPreset PlanarTrackerDialog::selectedPreset() const
{
    planar_tracker_preset::PlanarTrackerPreset preset;
    const int index = currentPresetIndex();
    if (index >= 0) {
        preset = m_presets.at(index);
    } else {
        preset.id = QStringLiteral("custom");
        preset.displayName = tr("Custom");
    }

    preset.patchSizePx = static_cast<double>(m_patchSizeSpin->value());
    preset.searchRadiusPx = static_cast<double>(m_searchRadiusSpin->value());
    preset.dampingFactor = m_dampingSpin->value() / 100.0;
    return preset;
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::accept()
{
    emit presetApplied(selectedPreset());
    QDialog::accept();
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onPresetSelectionChanged(int index)
{
    updateDeletePresetButton();

    bool ok = false;
    const int presetIndex = m_presetCombo->itemData(index).toInt(&ok);
    if (!ok || presetIndex < 0 || presetIndex >= m_presets.size()) {
        if (m_descriptionLabel)
            m_descriptionLabel->setText(tr("説明: なし"));
        return;
    }

    const planar_tracker_preset::PlanarTrackerPreset& preset = m_presets.at(presetIndex);
    if (m_descriptionLabel) {
        m_descriptionLabel->setText(
            preset.description.isEmpty() ? tr("説明: なし") : preset.description);
    }
    applyPresetToWidgets(preset);
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onSaveCustomPreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               tr("カスタム preset 保存"),
                                               tr("名前:"),
                                               QLineEdit::Normal,
                                               QString(),
                                               &ok).trimmed();
    if (!ok || name.isEmpty())
        return;

    planar_tracker_preset::PlanarTrackerPreset preset = selectedPreset();
    preset.id = makeUserPresetId(name);
    preset.displayName = name;

    if (planar_tracker_preset::Registry::instance().saveUserPreset(preset))
        rebuildPresetCombo(preset.id);
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onDeleteSelectedPreset()
{
    const int index = currentPresetIndex();
    if (index < 0)
        return;

    const planar_tracker_preset::PlanarTrackerPreset preset = m_presets.at(index);
    if (planar_tracker_preset::findBuiltin(preset.id).has_value())
        return;

    const QMessageBox::StandardButton answer =
        QMessageBox::question(this,
                              tr("選択中の preset を削除"),
                              tr("「%1」を削除しますか?").arg(preset.displayName),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    if (planar_tracker_preset::Registry::instance().removeUserPreset(preset.id))
        rebuildPresetCombo();
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onResetPresetToDefaults()
{
    const int index = currentPresetIndex();
    if (index < 0)
        return;

    const planar_tracker_preset::PlanarTrackerPreset& preset = m_presets.at(index);
    applyPresetToWidgets(preset);
    if (m_descriptionLabel) {
        m_descriptionLabel->setText(
            preset.description.isEmpty() ? tr("説明: なし") : preset.description);
    }
    updateDeletePresetButton();
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onExportPreset()
{
    QString fileName = QFileDialog::getSaveFileName(this,
                                                    tr("Preset を JSON エクスポート"),
                                                    QString(),
                                                    tr("Planar Tracker Preset JSON (*.json)"));
    if (fileName.isEmpty())
        return;
    if (!fileName.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        fileName.append(QStringLiteral(".json"));

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this,
                             tr("Preset を JSON エクスポート"),
                             tr("JSON ファイルを書き込めませんでした。"));
        return;
    }

    const QJsonObject obj = planar_tracker_preset::toJson(selectedPreset());
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        QMessageBox::warning(this,
                             tr("Preset を JSON エクスポート"),
                             tr("JSON ファイルを書き込めませんでした。"));
    }
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onImportPreset()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this,
        tr("Preset を JSON インポート"),
        QString(),
        tr("Planar Tracker Preset JSON (*.json)"));
    if (fileName.isEmpty())
        return;

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this,
                             tr("Preset を JSON インポート"),
                             tr("JSON ファイルを読み込めませんでした。"));
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(this,
                             tr("Preset を JSON インポート"),
                             tr("JSON が不正です"));
        return;
    }

    auto imported = planar_tracker_preset::fromJson(doc.object());
    if (!imported) {
        QMessageBox::warning(this,
                             tr("Preset を JSON インポート"),
                             tr("JSON が不正です"));
        return;
    }

    imported->id = makeUserPresetId(imported->displayName);
    if (!planar_tracker_preset::Registry::instance().saveUserPreset(*imported)) {
        QMessageBox::warning(this,
                             tr("Preset を JSON インポート"),
                             tr("Preset を保存できませんでした。"));
        return;
    }

    rebuildPresetCombo(imported->id);
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::rebuildPresetCombo(const QString& selectedId)
{
    m_presets = planar_tracker_preset::Registry::instance().allPresets();
    std::sort(m_presets.begin(), m_presets.end(),
              [](const planar_tracker_preset::PlanarTrackerPreset& lhs,
                 const planar_tracker_preset::PlanarTrackerPreset& rhs) {
                  const int byName = QString::localeAwareCompare(lhs.displayName, rhs.displayName);
                  if (byName != 0)
                      return byName < 0;
                  return lhs.id < rhs.id;
              });

    const bool comboWasBlocked = m_presetCombo->blockSignals(true);
    m_presetCombo->clear();

    int selectedIndex = m_presets.isEmpty() ? -1 : 0;
    for (int i = 0; i < m_presets.size(); ++i) {
        const planar_tracker_preset::PlanarTrackerPreset& preset = m_presets.at(i);
        m_presetCombo->addItem(preset.displayName, i);
        if (!selectedId.isEmpty() && preset.id == selectedId)
            selectedIndex = i;
    }

    m_presetCombo->setCurrentIndex(selectedIndex);
    m_presetCombo->blockSignals(comboWasBlocked);

    if (selectedIndex >= 0) {
        const planar_tracker_preset::PlanarTrackerPreset& preset = m_presets.at(selectedIndex);
        applyPresetToWidgets(preset);
        if (m_descriptionLabel) {
            m_descriptionLabel->setText(
                preset.description.isEmpty() ? tr("説明: なし") : preset.description);
        }
    } else {
        if (m_descriptionLabel)
            m_descriptionLabel->setText(tr("説明: なし"));
    }
    updateDeletePresetButton();
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::applyPresetToWidgets(
    const planar_tracker_preset::PlanarTrackerPreset& preset)
{
    setPresetWidgetSignalsBlocked(true);

    m_patchSizeSpin->setValue(qRound(preset.patchSizePx));
    m_searchRadiusSpin->setValue(qRound(preset.searchRadiusPx));
    m_dampingSpin->setValue(qRound(preset.dampingFactor * 100.0));

    setPresetWidgetSignalsBlocked(false);

    m_params.patchSizePx = static_cast<double>(m_patchSizeSpin->value());
    m_params.searchRadiusPx = static_cast<double>(m_searchRadiusSpin->value());
    m_params.dampingFactor = m_dampingSpin->value() / 100.0;
    m_params.maxFramesPerCall = preset.maxFramesPerCall;
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::setPresetWidgetSignalsBlocked(bool blocked)
{
    m_patchSizeSpin->blockSignals(blocked);
    m_searchRadiusSpin->blockSignals(blocked);
    m_dampingSpin->blockSignals(blocked);
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::updateDeletePresetButton()
{
    if (!m_deletePresetBtn)
        return;

    const int index = currentPresetIndex();
    if (index < 0) {
        m_deletePresetBtn->setEnabled(false);
        return;
    }

    m_deletePresetBtn->setEnabled(
        !planar_tracker_preset::findBuiltin(m_presets.at(index).id).has_value());
}

// ---------------------------------------------------------------------------
int PlanarTrackerDialog::currentPresetIndex() const
{
    bool ok = false;
    const int index = m_presetCombo->currentData().toInt(&ok);
    if (!ok || index < 0 || index >= m_presets.size())
        return -1;
    return index;
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onResetCorners()
{
    const double w = m_reference.isNull() ? 640.0 : m_reference.width();
    const double h = m_reference.isNull() ? 360.0 : m_reference.height();
    m_corners = planar::CornerSet::rectangle(
        QRectF(w * 0.1, h * 0.1, w * 0.8, h * 0.8));
    m_cornerWidget->setCorners(m_corners);
    rebuildSummary();
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onTrackClicked()
{
    if (m_frames.isEmpty()) {
        QMessageBox::information(this, tr("情報"),
                                 tr("フレームが投入されていません。"));
        return;
    }

    // Sync params from UI
    m_params.patchSizePx    = m_patchSizeSpin->value();
    m_params.searchRadiusPx = m_searchRadiusSpin->value();
    m_params.dampingFactor  = m_dampingSpin->value() / 100.0;

    planar::Tracker tracker;
    tracker.setParams(m_params);

    m_result.clear();
    m_progress->setRange(0, m_frames.size());
    m_progress->setValue(0);
    m_progress->setVisible(true);

    // First frame is the reference
    tracker.setReferenceFrame(m_frames.first(), m_corners);

    for (int i = 1; i < m_frames.size(); ++i) {
        const qint64 timeMs = static_cast<qint64>(i) * 33LL;   // ~30 fps
        planar::Frame f = tracker.trackNextFrame(m_frames[i], i, timeMs);
        m_result.append(f);
        m_progress->setValue(i);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    m_progress->setVisible(false);
    rebuildSummary();
    emit trackComputed(m_result);
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onPatchSizeChanged(int value)
{
    m_params.patchSizePx = static_cast<double>(value);
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onSearchRadiusChanged(int value)
{
    m_params.searchRadiusPx = static_cast<double>(value);
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::onDampingChanged(int value)
{
    m_params.dampingFactor = value / 100.0;
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::rebuildSummary()
{
    const int total   = m_frames.size();
    const int tracked = m_result.size();

    double avgConf = 0.0;
    for (const planar::Frame& f : m_result)
        avgConf += f.confidence;
    if (tracked > 0)
        avgConf /= tracked;

    const QString text = tr("投入: %1 / 追跡済: %2 / 平均信頼度: %3")
                             .arg(total)
                             .arg(tracked)
                             .arg(avgConf, 0, 'f', 2);
    m_summaryLabel->setText(text);
}
