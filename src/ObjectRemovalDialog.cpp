#include "ObjectRemovalDialog.h"

#include "FrameExport.h"

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QDir>
#include <QPixmap>
#include <QStandardItemModel>

#include <cmath>

namespace {

QLabel *makePreviewLabel(QWidget *parent, const QString &text)
{
    auto *label = new QLabel(text, parent);
    label->setMinimumSize(360, 230);
    label->setAlignment(Qt::AlignCenter);
    label->setFrameShape(QFrame::StyledPanel);
    label->setFrameShadow(QFrame::Sunken);
    label->setStyleSheet(
        QStringLiteral("QLabel { background: #17191d; color: #8f98a5; "
                       "border: 1px solid #3b414b; }") );
    return label;
}

QSpinBox *makePixelSpin(QWidget *parent, int value, int maximum)
{
    auto *spin = new QSpinBox(parent);
    spin->setRange(0, maximum);
    spin->setValue(value);
    spin->setSuffix(QStringLiteral(" px"));
    return spin;
}

} // namespace

ObjectRemovalDialog::ObjectRemovalDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("オブジェクト除去 / コンテンツに応じた塗りつぶし"));
    setModal(true);
    resize(980, 700);

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
    rangeRow->addSpacing(8);
    rangeRow->addWidget(new QLabel(QStringLiteral("表示"), scopeGroup));
    m_previewFrameSpin = new QSpinBox(scopeGroup);
    m_previewFrameSpin->setSuffix(QStringLiteral(" frame"));
    rangeRow->addWidget(m_previewFrameSpin);
    rangeRow->addStretch(1);
    scopeForm->addRow(QStringLiteral("フレーム範囲:"), rangeRow);

    m_maskCombo = new QComboBox(scopeGroup);
    m_maskCombo->addItem(QStringLiteral("クリップマスク"), 0);
    m_maskCombo->addItem(QStringLiteral("ロトブラシマスク"), 1);
    scopeForm->addRow(QStringLiteral("マスクソース:"), m_maskCombo);
    root->addWidget(scopeGroup);

    auto *previewGroup = new QGroupBox(QStringLiteral("プレビュー"), this);
    auto *previewLayout = new QVBoxLayout(previewGroup);
    auto *previewViews = new QHBoxLayout;
    auto *beforeColumn = new QVBoxLayout;
    auto *afterColumn = new QVBoxLayout;
    beforeColumn->addWidget(new QLabel(QStringLiteral("処理前"), previewGroup));
    afterColumn->addWidget(new QLabel(QStringLiteral("処理後"), previewGroup));
    m_beforeView = makePreviewLabel(previewGroup,
                                    QStringLiteral("プレビューを実行してください"));
    m_afterView = makePreviewLabel(previewGroup,
                                   QStringLiteral("プレビューを実行してください"));
    beforeColumn->addWidget(m_beforeView, 1);
    afterColumn->addWidget(m_afterView, 1);
    previewViews->addLayout(beforeColumn, 1);
    previewViews->addLayout(afterColumn, 1);
    previewLayout->addLayout(previewViews, 1);
    m_statusLabel = new QLabel(QStringLiteral("現在フレームを確認できます。"), previewGroup);
    m_statusLabel->setWordWrap(true);
    previewLayout->addWidget(m_statusLabel);
    root->addWidget(previewGroup, 1);

    auto *parametersGroup = new QGroupBox(QStringLiteral("パラメータ"), this);
    auto *parametersForm = new QFormLayout(parametersGroup);
    m_temporalRadiusSpin = new QSpinBox(parametersGroup);
    m_temporalRadiusSpin->setRange(0, 240);
    m_temporalRadiusSpin->setValue(12);
    m_temporalRadiusSpin->setSuffix(QStringLiteral(" frame"));
    m_temporalStrideSpin = new QSpinBox(parametersGroup);
    m_temporalStrideSpin->setRange(1, 60);
    m_temporalStrideSpin->setValue(1);
    m_temporalStrideSpin->setSuffix(QStringLiteral(" frame"));
    m_trackingCheck = new QCheckBox(
        QStringLiteral("背景整列（平面トラッキング）"), parametersGroup);
    m_trackingCheck->setChecked(false);
    m_trackingCheck->setToolTip(QStringLiteral(
        "物体トラッキングの変位は使わず、背景用の平面トラッキングだけを参照します。"));
    m_dilateSpin = makePixelSpin(parametersGroup, 2, 64);
    m_featherSpin = makePixelSpin(parametersGroup, 3, 64);
    m_trustSpin = new QDoubleSpinBox(parametersGroup);
    m_trustSpin->setRange(0.0, 1.0);
    m_trustSpin->setSingleStep(0.05);
    m_trustSpin->setDecimals(2);
    m_trustSpin->setValue(0.65);
    m_spatialRadiusSpin = makePixelSpin(parametersGroup, 24, 256);
    parametersForm->addRow(QStringLiteral("時間参照半径:"), m_temporalRadiusSpin);
    parametersForm->addRow(QStringLiteral("時間参照間引き:"), m_temporalStrideSpin);
    parametersForm->addRow(QStringLiteral("位置合わせ:"), m_trackingCheck);
    parametersForm->addRow(QStringLiteral("マスク膨張:"), m_dilateSpin);
    parametersForm->addRow(QStringLiteral("羽根:"), m_featherSpin);
    parametersForm->addRow(QStringLiteral("時間サンプル信頼度:"), m_trustSpin);
    parametersForm->addRow(QStringLiteral("空間探索半径:"), m_spatialRadiusSpin);
    root->addWidget(parametersGroup);

    auto *buttonRow = new QHBoxLayout;
    m_previewButton = new QPushButton(QStringLiteral("プレビュー"), this);
    m_previewButton->setDefault(true);
    m_applyButton = new QPushButton(QStringLiteral("適用"), this);
    m_cancelButton = new QPushButton(QStringLiteral("閉じる"), this);
    m_applyButton->setEnabled(false);
    buttonRow->addWidget(m_previewButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_applyButton);
    buttonRow->addWidget(m_cancelButton);
    root->addLayout(buttonRow);

    connect(m_previewButton, &QPushButton::clicked,
            this, &ObjectRemovalDialog::onPreviewClicked);
    connect(m_applyButton, &QPushButton::clicked,
            this, &ObjectRemovalDialog::onApplyClicked);
    connect(m_cancelButton, &QPushButton::clicked,
            this, &ObjectRemovalDialog::onCancelClicked);
}

void ObjectRemovalDialog::setContext(
    const objremoval::ObjectRemovalDialogContext &context)
{
    m_context = context;
    m_frameCache.clear();

    const int frameCount = qMax(1, context.frameCount);
    m_startSpin->setRange(0, frameCount - 1);
    m_endSpin->setRange(0, frameCount - 1);
    m_previewFrameSpin->setRange(0, frameCount - 1);
    const int defaultFrame = qBound(0, context.defaultFrame, frameCount - 1);
    m_startSpin->setValue(0);
    m_endSpin->setValue(frameCount - 1);
    m_previewFrameSpin->setValue(defaultFrame);
    m_clipValue->setText(context.clipLabel.isEmpty()
                             ? QStringLiteral("未設定")
                             : context.clipLabel);

    if (auto *model = qobject_cast<QStandardItemModel *>(m_maskCombo->model())) {
        model->item(0)->setEnabled(context.clipMaskAvailable);
        model->item(1)->setEnabled(context.rotoMaskAvailable);
    }
    if (context.clipMaskAvailable)
        m_maskCombo->setCurrentIndex(0);
    else if (context.rotoMaskAvailable)
        m_maskCombo->setCurrentIndex(1);
    const bool backgroundAlignmentReady = context.backgroundAlignmentAvailable
        && static_cast<bool>(context.backgroundAlignmentOffsetFetcher);
    m_trackingCheck->setEnabled(backgroundAlignmentReady);
    // Background alignment is opt-in. The mask/object tracker is not a
    // substitute and therefore never enables this control.
    m_trackingCheck->setChecked(false);
    m_applyButton->setEnabled(context.frameFetcher && context.sequenceImporter
                              && (context.clipMaskAvailable
                                  || context.rotoMaskAvailable));

    m_statusLabel->setText(QStringLiteral("フレーム %1 を表示できます。")
                               .arg(defaultFrame));
    showImage(m_beforeView, {}, QStringLiteral("プレビューを実行してください"));
    showImage(m_afterView, {}, QStringLiteral("プレビューを実行してください"));
}

objremoval::ObjectRemovalParams ObjectRemovalDialog::params() const
{
    objremoval::ObjectRemovalParams result;
    result.temporalRadius = m_temporalRadiusSpin->value();
    result.temporalStride = m_temporalStrideSpin->value();
    result.useBackgroundAlignment = m_trackingCheck->isChecked();
    result.maskDilatePx = m_dilateSpin->value();
    result.featherPx = m_featherSpin->value();
    result.temporalTrustThreshold = m_trustSpin->value();
    result.spatialRadius = m_spatialRadiusSpin->value();
    return result;
}

QImage ObjectRemovalDialog::fetchFrame(int frameIndex)
{
    if (frameIndex < 0 || frameIndex >= qMax(1, m_context.frameCount)
        || !m_context.frameFetcher) {
        return {};
    }
    if (m_frameCache.contains(frameIndex))
        return m_frameCache.value(frameIndex);
    const QImage image = m_context.frameFetcher(frameIndex);
    m_frameCache.insert(frameIndex, image);
    return image;
}

QImage ObjectRemovalDialog::fetchMask(int frameIndex, const QSize &size) const
{
    if (m_maskCombo->currentIndex() == 0) {
        if (!m_context.clipMaskFetcher)
            return {};
        return m_context.clipMaskFetcher(frameIndex, size);
    }
    if (!m_context.rotoMaskFetcher)
        return {};
    return m_context.rotoMaskFetcher(frameIndex, size);
}

QImage ObjectRemovalDialog::processFrame(int frameIndex)
{
    const objremoval::ObjectRemovalParams removalParams = params();
    const int radius = qMax(0, removalParams.temporalRadius);
    const auto trimCache = [this, frameIndex, radius]() {
        objremoval::trimObjectRemovalFrameCache(
            m_frameCache, frameIndex, m_context.frameCount, radius);
    };

    const QImage target = fetchFrame(frameIndex);
    if (target.isNull()) {
        trimCache();
        return {};
    }

    const QImage mask = fetchMask(frameIndex, target.size());
    if (mask.isNull()) {
        trimCache();
        return {};
    }

    objremoval::ObjectRemovalTemporalSources sources;
    sources.frameFetcher = [this](int index) { return fetchFrame(index); };
    sources.maskTrackingOffsetFetcher = m_context.maskTrackingOffsetFetcher;
    sources.backgroundAlignmentOffsetFetcher =
        m_context.backgroundAlignmentOffsetFetcher;
    const objremoval::ObjectRemovalNeighborSet neighborSet =
        objremoval::collectTemporalNeighbors(
            frameIndex, m_context.frameCount, radius, sources,
            removalParams.useBackgroundAlignment);
    const QImage result = objremoval::removeObject(
        target, mask, neighborSet.neighbors, neighborSet.backgroundOffsets,
        removalParams);
    trimCache();
    return result;
}

void ObjectRemovalDialog::setBusy(bool busy)
{
    m_busy = busy;
    if (!busy)
        m_cancelRequested = false;
    m_previewButton->setEnabled(!busy);
    m_applyButton->setEnabled(!busy);
    m_cancelButton->setEnabled(true);
    m_cancelButton->setText(busy ? QStringLiteral("中止")
                                : QStringLiteral("閉じる"));
    m_startSpin->setEnabled(!busy);
    m_endSpin->setEnabled(!busy);
    m_previewFrameSpin->setEnabled(!busy);
    m_maskCombo->setEnabled(!busy);
    m_temporalRadiusSpin->setEnabled(!busy);
    m_temporalStrideSpin->setEnabled(!busy);
    m_trackingCheck->setEnabled(
        !busy && m_context.backgroundAlignmentAvailable
        && static_cast<bool>(m_context.backgroundAlignmentOffsetFetcher));
    m_dilateSpin->setEnabled(!busy);
    m_featherSpin->setEnabled(!busy);
    m_trustSpin->setEnabled(!busy);
    m_spatialRadiusSpin->setEnabled(!busy);
}

void ObjectRemovalDialog::showImage(QLabel *label, const QImage &image,
                                    const QString &emptyText) const
{
    if (!label)
        return;
    if (image.isNull()) {
        label->setPixmap(QPixmap());
        label->setText(emptyText);
        return;
    }
    const QPixmap pixmap = QPixmap::fromImage(image).scaled(
        label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    label->setText(QString());
    label->setPixmap(pixmap);
}

QString ObjectRemovalDialog::outputDirectory(QString *error) const
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
        baseName = QStringLiteral("object_removal");
    QString folderName = baseName + QStringLiteral("_object_removal");
    QString path = parent.filePath(folderName);
    int suffix = 2;
    while (QDir(path).exists())
        path = parent.filePath(folderName + QStringLiteral("_%1").arg(suffix++));
    if (!parent.mkpath(QFileInfo(path).fileName())) {
        if (error)
            *error = QStringLiteral("出力フォルダを作成できません。\n%1").arg(path);
        return {};
    }
    return path;
}

void ObjectRemovalDialog::onPreviewClicked()
{
    if (m_busy)
        return;
    const int frameIndex = m_previewFrameSpin->value();
    setBusy(true);
    m_statusLabel->setText(QStringLiteral("フレーム %1 を処理中...").arg(frameIndex));
    QApplication::processEvents();

    const QImage before = fetchFrame(frameIndex);
    const QImage after = processFrame(frameIndex);
    showImage(m_beforeView, before, QStringLiteral("フレームを取得できません"));
    showImage(m_afterView, after, QStringLiteral("処理できません"));
    if (m_cancelRequested) {
        m_statusLabel->setText(QStringLiteral("プレビューを中止しました。"));
    } else if (before.isNull() || after.isNull()) {
        m_statusLabel->setText(QStringLiteral(
            "フレームまたは選択したマスクを取得できません。マスクソースを確認してください。"));
    } else {
        m_statusLabel->setText(QStringLiteral("フレーム %1 のプレビューを更新しました。")
                                   .arg(frameIndex));
    }
    setBusy(false);
}

void ObjectRemovalDialog::onApplyClicked()
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
    const QString folder = outputDirectory(&error);
    if (folder.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }

    setBusy(true);
    QStringList paths;
    paths.reserve(end - start + 1);
    for (int frameIndex = start; frameIndex <= end; ++frameIndex) {
        if (m_cancelRequested) {
            m_statusLabel->setText(QStringLiteral("適用を中止しました。"));
            setBusy(false);
            return;
        }
        m_statusLabel->setText(QStringLiteral("フレーム %1 / %2 を処理中...")
                                   .arg(frameIndex - start + 1).arg(end - start + 1));
        QApplication::processEvents();
        if (m_cancelRequested) {
            m_statusLabel->setText(QStringLiteral("適用を中止しました。"));
            setBusy(false);
            return;
        }
        const QImage result = processFrame(frameIndex);
        if (result.isNull()) {
            setBusy(false);
            QMessageBox::warning(this, windowTitle(),
                                 QStringLiteral("フレーム %1 の処理に失敗しました。")
                                     .arg(frameIndex));
            return;
        }

        const QString path = QDir(folder).filePath(
            QStringLiteral("frame_%1.png").arg(frameIndex - start, 6, 10,
                                                QLatin1Char('0')));
        if (!frameexport::saveFrameImage(result, path,
                                         frameexport::ImageFormat::Png, &error)) {
            setBusy(false);
            QMessageBox::warning(this, windowTitle(),
                                 QStringLiteral("PNG の保存に失敗しました。\n%1").arg(error));
            return;
        }
        paths.append(path);
    }

    if (!m_context.sequenceImporter(paths, qMax(1.0, m_context.fps), &error)) {
        setBusy(false);
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("新規クリップへの取り込みに失敗しました。\n%1")
                                 .arg(error));
        return;
    }

    m_statusLabel->setText(QStringLiteral("%1 フレームを書き出し、新規クリップへ取り込みました。")
                               .arg(paths.size()));
    setBusy(false);
    accept();
}

void ObjectRemovalDialog::onCancelClicked()
{
    if (m_busy) {
        m_cancelRequested = true;
        m_statusLabel->setText(QStringLiteral("現在のフレーム処理後に中止します。"));
        return;
    }
    reject();
}
