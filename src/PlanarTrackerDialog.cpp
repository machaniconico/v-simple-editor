#include "PlanarTrackerDialog.h"

#include "PlanarTrackerPresetRegistry.h"
#include "FeatureTracker.h"
#include "libavcore/FrameGrab.h"
#include <QThread>
#include <cmath>
#include <memory>
#include <utility>

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
    m_cameraSolveButton = new QPushButton(tr("3D カメラを解析"), this);
    m_cameraSolveButton->setObjectName(QStringLiteral("cameraSolveButton"));
    m_cameraSolveButton->setEnabled(false);
    rightLayout->addWidget(m_cameraSolveButton);
    connect(m_cameraSolveButton, &QPushButton::clicked,
            this, &PlanarTrackerDialog::onCameraSolveClicked);
    rightLayout->addWidget(m_progress);
    rightLayout->addWidget(m_summaryLabel);
    auto* featureForm = new QFormLayout;
    auto* limits = new QLabel(tr("特徴点トラッキング (3D) — 上限 %1 フレーム / %2 点")
        .arg(sfm::kMaxBundleFrames).arg(sfm::kMaxBundlePoints), this);
    limits->setObjectName(QStringLiteral("featureLimits"));
    limits->setWordWrap(true);
    rightLayout->addWidget(limits);
    m_featureStart = new QSpinBox(this);
    m_featureEnd = new QSpinBox(this);
    m_featureMaxPoints = new QSpinBox(this);
    m_featureStart->setObjectName(QStringLiteral("featureStart"));
    m_featureEnd->setObjectName(QStringLiteral("featureEnd"));
    m_featureMaxPoints->setObjectName(QStringLiteral("featureMaxPoints"));
    m_featureMaxPoints->setRange(8, sfm::kMaxBundlePoints);
    m_featureMaxPoints->setValue(sfm::kMaxBundlePoints);
    featureForm->addRow(tr("開始フレーム (素材・0始まり):"), m_featureStart);
    featureForm->addRow(tr("終了フレーム (含む):"), m_featureEnd);
    featureForm->addRow(tr("最大特徴点数:"), m_featureMaxPoints);
    rightLayout->addLayout(featureForm);
    m_featureButton = new QPushButton(tr("特徴点を追跡"), this);
    m_featureButton->setObjectName(QStringLiteral("featureTrackButton"));
    m_featureButton->setEnabled(false);
    m_featureProgress = new QProgressBar(this);
    m_featureProgress->setVisible(false);
    m_featureSummary = new QLabel(this);
    m_featureSummary->setObjectName(QStringLiteral("featureSummary"));
    m_featureSummary->setWordWrap(true);
    rightLayout->addWidget(m_featureButton);
    rightLayout->addWidget(m_featureProgress);
    rightLayout->addWidget(m_featureSummary);
    connect(m_featureButton, &QPushButton::clicked,
            this, &PlanarTrackerDialog::onFeatureTrackClicked);
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
                m_result.clear();
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
    m_result.clear();
    m_reference = frame;
    m_cornerWidget->setReferenceImage(frame);
    onResetCorners();   // reset corners to new image bounds
}

// ---------------------------------------------------------------------------
void PlanarTrackerDialog::setFrames(const QList<QImage>& frames)
{
    m_planarFrameLoader = {};
    m_result.clear();
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
    m_result.clear();
    m_corners = corners;
    m_cornerWidget->setCorners(corners);
    rebuildSummary();
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
// PRD-PROJECT-PRESET US-PP-4: restore dialog widgets from persisted project state.
void PlanarTrackerDialog::setInitialState(const PlanarTrackerProjectState& s)
{
    // Restore last-used preset selection by id
    if (!s.lastPresetId.isEmpty() && m_presetCombo) {
        for (int i = 0; i < m_presets.size(); ++i) {
            if (m_presets.at(i).id == s.lastPresetId) {
                m_presetCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    // Restore widget values without triggering change signals
    setPresetWidgetSignalsBlocked(true);

    if (m_patchSizeSpin)
        m_patchSizeSpin->setValue(static_cast<int>(s.patchSizePx));
    if (m_searchRadiusSpin)
        m_searchRadiusSpin->setValue(static_cast<int>(s.searchRadiusPx));
    if (m_dampingSpin)
        m_dampingSpin->setValue(static_cast<int>(s.dampingFactor * 100.0));

    setPresetWidgetSignalsBlocked(false);
}

// ---------------------------------------------------------------------------
// PRD-PROJECT-PRESET US-PP-4: snapshot current dialog widget state for persistence.
PlanarTrackerProjectState PlanarTrackerDialog::currentState() const
{
    PlanarTrackerProjectState s;

    const int idx = currentPresetIndex();
    if (idx >= 0 && idx < m_presets.size())
        s.lastPresetId = m_presets.at(idx).id;

    if (m_patchSizeSpin)
        s.patchSizePx = static_cast<double>(m_patchSizeSpin->value());
    if (m_searchRadiusSpin)
        s.searchRadiusPx = static_cast<double>(m_searchRadiusSpin->value());
    if (m_dampingSpin)
        s.dampingFactor = m_dampingSpin->value() / 100.0;

    return s;
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
    m_result.clear();
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
    if (m_planarFrameLoader) {
        const auto frames = m_planarFrameLoader();
        if (frames.isEmpty()) return;
        setFrames(frames);
        m_planarFrameLoader = {};
    }
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
    m_result.append(planar::Frame{0, 0, m_corners, 1.0});

    for (int i = 1; i < m_frames.size(); ++i) {
        const qint64 timeMs = qRound64(double(i) * 1000.0 / m_cameraFps);
        planar::Frame f = tracker.trackNextFrame(m_frames[i], i, timeMs);
        m_result.append(f);
        m_progress->setValue(i);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    m_progress->setVisible(false);
    rebuildSummary();
    emit trackComputed(m_result);
}

void PlanarTrackerDialog::setCameraSolveContext(QSize canvas, double fov,
                                               double fps, double startSec)
{
    m_cameraCanvas = canvas;
    m_cameraFov = fov;
    m_cameraFps = fps > 0 ? fps : 30.0;
    m_cameraStartSec = startSec;
}

void PlanarTrackerDialog::onCameraSolveClicked()
{
    if (m_result.isEmpty())
        return;
    QVector<QPolygonF> corners;
    QVector<double> confidence;
    for (const auto& frame : m_result) {
        const auto& c = frame.corners;
        corners.append(QPolygonF{c.tl, c.tr, c.br, c.bl});
        confidence.append(frame.confidence);
    }
    const auto poses = camsolve::solveSequence(corners, confidence, 0,
        camsolve::makeIntrinsicsFromFocalPx(m_cameraFov, m_cameraCanvas));
    int valid = 0;
    double residual = 0;
    for (const auto& pose : poses) {
        if (pose.valid) {
            ++valid;
            residual += pose.residual;
        }
    }
    const QString summary = tr("有効フレーム: %1 / %2\n平均残差: %3")
        .arg(valid).arg(poses.size())
        .arg(valid ? QString::number(residual / valid, 'g', 6) : tr("算出不可"));
    if (!valid) {
        QMessageBox::information(this, tr("3D カメラ解析"), summary);
        return;
    }
    if (QMessageBox::question(this, tr("3D カメラ解析"),
            summary + tr("\nカメラのキーフレームとして適用しますか？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        emit cameraSolveApplied(poses, m_cameraFps, m_cameraStartSec);
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
    m_cameraSolveButton->setEnabled(!m_result.isEmpty());
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

PlanarTrackerDialog::~PlanarTrackerDialog()
{
    if (m_featureWorker) {
        m_featureWorker->requestInterruption();
        m_featureWorker->wait();
        delete m_featureWorker;
    }
}

void PlanarTrackerDialog::setPlanarFrameLoader(std::function<QList<QImage>()> loader)
{
    setFrames({});
    m_planarFrameLoader = std::move(loader);
}

void PlanarTrackerDialog::setFeatureTrackingSource(const QString& path, double fps, int frameCount)
{
    if (m_featureWorker) return;
    m_featurePath = path;
    m_featureFps = fps;
    m_featureFrameCount = frameCount;
    m_featureTracks = {};
    m_featureAnalysisSize = {};
    m_featureSummary->clear();
    m_featureStartFrame = 0;
    m_featureProgress->setVisible(false);
    m_featureStart->setToolTip(tr("素材の先頭を0として、毎秒%1フレームで解析します。タイムラインの速度変更・トリムは適用しません。")
        .arg(fps));
    m_featureEnd->setToolTip(m_featureStart->toolTip());
    m_featureStart->setRange(0, std::max(0, frameCount - 1));
    m_featureEnd->setRange(0, std::max(0, frameCount - 1));
    m_featureStart->setValue(0);
    m_featureEnd->setValue(std::max(0, std::min(frameCount, sfm::kMaxBundleFrames) - 1));
    m_featureButton->setEnabled(!path.isEmpty());
}

void PlanarTrackerDialog::onFeatureTrackClicked()
{
    if (m_featureWorker) return;
    m_featureTracks = {};
    m_featureAnalysisSize = {};
    const int first = m_featureStart->value(), last = m_featureEnd->value();
    const int count = last - first + 1, maxPoints = m_featureMaxPoints->value();
    QString error;
    if (count < 2 || first < 0 || last >= m_featureFrameCount)
        error = tr("範囲が不正です。最低2フレームを指定してください。");
    else if (count > sfm::kMaxBundleFrames)
        error = tr("メモリ使用量を制限するため、上限は%1フレームです。範囲を短くしてください。")
            .arg(sfm::kMaxBundleFrames);
    else if (maxPoints < 8)
        error = tr("特徴点は最低8点必要です。");
    else if (m_featurePath.isEmpty() || !std::isfinite(m_featureFps) || m_featureFps <= 0)
        error = tr("動画素材またはフレームレートが不正です。");
    if (!error.isEmpty()) {
        m_featureSummary->setText(error);
        emit featureTrackingFinished(false);
        return;
    }
    m_featureStartFrame = first;
    m_featureButton->setEnabled(false);
    m_featureStart->setEnabled(false);
    m_featureEnd->setEnabled(false);
    m_featureMaxPoints->setEnabled(false);
    m_featureProgress->setVisible(true);
    m_featureProgress->setRange(0, count);
    m_featureProgress->setValue(0);
    m_featureSummary->setText(tr("解析用フレームを読み込み中…"));
    struct Result { sfm::MultiViewTracks tracks; QSize size; QString error; int complete = 0; };
    auto result = std::make_shared<Result>();
    const QString path = m_featurePath;
    const double fps = m_featureFps;
    m_featureWorker = QThread::create([this, result, path, fps, first, count, maxPoints] {
        QVector<QImage> frames;
        // Bounding both dimensions also bounds extreme-aspect-ratio memory use.
        for (int i = 0; i < count; ++i) {
            if (QThread::currentThread()->isInterruptionRequested()) return;
            QImage frame = libavcore::grabFrameAt(path, double(first + i) / fps, QSize(1280, 1280));
            if (frame.isNull()) { result->error = tr("フレームを取得できませんでした。"); return; }
            const int shorter = std::min(frame.width(), frame.height());
            if (shorter > 720)
                frame = frame.scaled(qRound(frame.width() * 720.0 / shorter),
                    qRound(frame.height() * 720.0 / shorter), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            frame = frame.convertToFormat(QImage::Format_Grayscale8);
            if (i == 0) {
                result->size = frame.size();
                if (feattrack::detectShiTomasi(frame, maxPoints).size() < 8) {
                    result->error = tr("特徴点が不足しています。最低8点が必要です。"); return;
                }
            } else if (frame.size() != result->size) {
                result->error = tr("フレームの解像度が途中で変化しています。"); return;
            }
            frames.append(frame);
            QMetaObject::invokeMethod(this, [this, i] { m_featureProgress->setValue(i + 1); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this] {
            m_featureProgress->setRange(0, 0);
            m_featureSummary->setText(tr("特徴点を追跡中…"));
        }, Qt::QueuedConnection);
        result->tracks = sfm::trackAcrossFrames(frames, maxPoints);
        for (const auto& track : result->tracks.tracks)
            if (track.observations.size() == count) ++result->complete;
        if (result->complete < 8)
            result->error = tr("最後まで追跡できた特徴点が不足しています。最低8点が必要です。");
    });
    connect(m_featureWorker, &QThread::finished, this, [this, result] {
        m_featureWorker->deleteLater();
        m_featureWorker = nullptr;
        m_featureButton->setEnabled(true);
        m_featureStart->setEnabled(true);
        m_featureEnd->setEnabled(true);
        m_featureMaxPoints->setEnabled(true);
        m_featureProgress->setRange(0, 100);
        m_featureProgress->setValue(100);
        m_featureAnalysisSize = result->size;
        const bool success = result->error.isEmpty() && result->tracks.frameCount >= 2;
        if (success) {
            m_featureTracks = result->tracks;
            const int total = int(m_featureTracks.tracks.size());
            m_featureSummary->setText(tr("追跡点数: %1 / %2、フレーム数: %3、脱落率: %4%")
                .arg(result->complete).arg(total).arg(m_featureTracks.frameCount)
                .arg(100.0 * (total - result->complete) / total, 0, 'f', 1));
        } else m_featureSummary->setText(result->error);
        emit featureTrackingFinished(success);
    });
    m_featureWorker->start();
}
