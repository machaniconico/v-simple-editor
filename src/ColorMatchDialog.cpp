#include "ColorMatchDialog.h"

#include "Timeline.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QVariant>
#include <QVBoxLayout>

bool applyColorMatchLutToSelectedTimelineClip(Timeline *timeline,
                                              const QString &lutPath,
                                              double lutIntensity,
                                              QString *errorMessage);

namespace {

QString sanitizedFileStem(QString stem)
{
    stem = stem.trimmed();
    if (stem.isEmpty())
        stem = QStringLiteral("Untitled");
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                 QStringLiteral("_"));
    stem.replace(QRegularExpression(QStringLiteral("_+")),
                 QStringLiteral("_"));
    stem = stem.left(80).trimmed();
    return stem.isEmpty() ? QStringLiteral("Untitled") : stem;
}

QString projectPathFromObject(const QObject *object)
{
    if (!object)
        return {};

    const QVariant projectFilePath = object->property("projectFilePath");
    if (projectFilePath.isValid() && !projectFilePath.toString().isEmpty())
        return projectFilePath.toString();

    const QVariant currentProjectFilePath = object->property("currentProjectFilePath");
    if (currentProjectFilePath.isValid() && !currentProjectFilePath.toString().isEmpty())
        return currentProjectFilePath.toString();

    if (const auto *widget = qobject_cast<const QWidget *>(object)) {
        const QString windowPath = widget->windowFilePath();
        if (!windowPath.isEmpty())
            return windowPath;
    }

    return {};
}

Timeline *timelineFromParentChain(QObject *object)
{
    for (QObject *cursor = object; cursor; cursor = cursor->parent()) {
        if (auto *timeline = cursor->findChild<Timeline *>())
            return timeline;
    }
    return (qApp && qApp->activeWindow())
        ? qApp->activeWindow()->findChild<Timeline *>()
        : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// ctor
// ---------------------------------------------------------------------------
ColorMatchDialog::ColorMatchDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Color Match"));
    setWindowFlags(Qt::Window);

    // ---- button row ----
    m_btnReference = new QPushButton(tr("Reference image..."), this);
    m_btnTarget    = new QPushButton(tr("Target image..."),    this);

    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(m_btnReference);
    btnRow->addWidget(m_btnTarget);

    // ---- thumbnail row ----
    m_lblRefThumb = new QLabel(this);
    m_lblRefThumb->setFixedSize(200, 150);
    m_lblRefThumb->setAlignment(Qt::AlignCenter);
    m_lblRefThumb->setText(tr("Reference"));
    m_lblRefThumb->setStyleSheet("border: 1px solid gray;");

    m_lblTgtThumb = new QLabel(this);
    m_lblTgtThumb->setFixedSize(200, 150);
    m_lblTgtThumb->setAlignment(Qt::AlignCenter);
    m_lblTgtThumb->setText(tr("Target"));
    m_lblTgtThumb->setStyleSheet("border: 1px solid gray;");

    auto *thumbRow = new QHBoxLayout;
    thumbRow->addWidget(m_lblRefThumb);
    thumbRow->addWidget(m_lblTgtThumb);

    // ---- before / after preview ----
    m_lblBefore = new QLabel(this);
    m_lblBefore->setFixedSize(200, 150);
    m_lblBefore->setAlignment(Qt::AlignCenter);
    m_lblBefore->setText(tr("Before"));
    m_lblBefore->setStyleSheet("border: 1px solid gray;");

    m_lblAfter = new QLabel(this);
    m_lblAfter->setFixedSize(200, 150);
    m_lblAfter->setAlignment(Qt::AlignCenter);
    m_lblAfter->setText(tr("After"));
    m_lblAfter->setStyleSheet("border: 1px solid gray;");

    auto *previewRow = new QHBoxLayout;
    previewRow->addWidget(m_lblBefore);
    previewRow->addWidget(m_lblAfter);

    // ---- bottom controls ----
    auto *lblSize = new QLabel(tr("LUT size:"), this);
    m_cbLutSize   = new QComboBox(this);
    m_cbLutSize->addItem(QStringLiteral("17"),  17);
    m_cbLutSize->addItem(QStringLiteral("33"),  33);
    m_cbLutSize->addItem(QStringLiteral("65"),  65);
    m_cbLutSize->setCurrentIndex(1); // default 33

    m_btnGenerate = new QPushButton(tr("Generate && Export LUT..."), this);
    m_btnGenerate->setEnabled(false);
    m_btnApply = new QPushButton(tr("選択クリップへ適用"), this);
    m_btnApply->setEnabled(false);

    auto *ctrlRow = new QHBoxLayout;
    ctrlRow->addWidget(lblSize);
    ctrlRow->addWidget(m_cbLutSize);
    ctrlRow->addStretch();
    ctrlRow->addWidget(m_btnApply);
    ctrlRow->addWidget(m_btnGenerate);

    // ---- assemble ----
    auto *root = new QVBoxLayout(this);
    root->addLayout(btnRow);
    root->addLayout(thumbRow);
    root->addLayout(previewRow);
    root->addLayout(ctrlRow);

    // ---- connections ----
    connect(m_btnReference, &QPushButton::clicked, this, &ColorMatchDialog::onSelectReference);
    connect(m_btnTarget,    &QPushButton::clicked, this, &ColorMatchDialog::onSelectTarget);
    connect(m_btnGenerate,  &QPushButton::clicked, this, &ColorMatchDialog::onGenerate);
    connect(m_btnApply,     &QPushButton::clicked, this, &ColorMatchDialog::onApplyToSelectedClip);
}

void ColorMatchDialog::setProjectFilePath(const QString &path)
{
    m_projectFilePath = path;
}

// ---------------------------------------------------------------------------
// slots
// ---------------------------------------------------------------------------
void ColorMatchDialog::onSelectReference()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select Reference Image"), QString(),
        tr("Images (*.png *.jpg *.bmp)"));
    if (path.isEmpty()) return;

    m_refImage = QImage(path);
    if (!m_refImage.isNull()) {
        m_lblRefThumb->setPixmap(
            QPixmap::fromImage(m_refImage).scaled(
                200, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    updateGenerateButton();
    updatePreview();
}

void ColorMatchDialog::onSelectTarget()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select Target Image"), QString(),
        tr("Images (*.png *.jpg *.bmp)"));
    if (path.isEmpty()) return;

    m_tgtImage = QImage(path);
    if (!m_tgtImage.isNull()) {
        m_lblTgtThumb->setPixmap(
            QPixmap::fromImage(m_tgtImage).scaled(
                200, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    updateGenerateButton();
    updatePreview();
}

void ColorMatchDialog::onGenerate()
{
    if (m_refImage.isNull() || m_tgtImage.isNull()) return;

    colormatch::lut::Lut3D lut;
    if (!generateCurrentLut(&lut)) return;

    const QString savePath = QFileDialog::getSaveFileName(
        this, tr("Export LUT"), QStringLiteral("ColorMatchLUT.cube"),
        tr("CUBE LUT (*.cube)"));
    if (savePath.isEmpty()) return;

    const bool ok = colormatch::lut::exportCube(lut, savePath);
    if (!ok) {
        QMessageBox::warning(this, tr("Export Failed"),
                             tr("Could not write LUT file:\n%1").arg(savePath));
        return;
    }

    emit lutGenerated(savePath);
}

void ColorMatchDialog::onApplyToSelectedClip()
{
    if (m_refImage.isNull() || m_tgtImage.isNull()) return;

    Timeline *timeline = timelineFromParentChain(this);
    if (!timeline) {
        QMessageBox::warning(this, tr("Apply Failed"),
                             tr("No timeline is available."));
        return;
    }

    colormatch::lut::Lut3D lut;
    if (!generateCurrentLut(&lut)) return;

    const QString savePath = automaticLutPath();
    const QDir outDir = QFileInfo(savePath).absoluteDir();
    if (!outDir.exists() && !QDir().mkpath(outDir.absolutePath())) {
        QMessageBox::warning(this, tr("Apply Failed"),
                             tr("Could not create LUT directory:\n%1")
                                 .arg(outDir.absolutePath()));
        return;
    }

    if (!colormatch::lut::exportCube(lut, savePath)) {
        QMessageBox::warning(this, tr("Apply Failed"),
                             tr("Could not write LUT file:\n%1").arg(savePath));
        return;
    }

    QString error;
    if (!applyColorMatchLutToSelectedTimelineClip(timeline, savePath, 1.0, &error)) {
        QFile::remove(savePath);
        QMessageBox::warning(this, tr("Apply Failed"),
                             error.isEmpty()
                                 ? tr("Could not apply LUT to the selected clip.")
                                 : error);
        return;
    }

    emit lutGenerated(savePath);
    emit lutAppliedToSelectedClip(savePath);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
void ColorMatchDialog::updateGenerateButton()
{
    const bool ready = !m_refImage.isNull() && !m_tgtImage.isNull();
    m_btnGenerate->setEnabled(ready);
    if (m_btnApply)
        m_btnApply->setEnabled(ready);
}

void ColorMatchDialog::updatePreview()
{
    if (!m_tgtImage.isNull()) {
        m_lblBefore->setPixmap(
            QPixmap::fromImage(m_tgtImage).scaled(
                200, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    if (!m_refImage.isNull() && !m_tgtImage.isNull()) {
        const int lutSize = m_cbLutSize->currentData().toInt();
        const colormatch::analyze::ColorStats refStats =
            colormatch::analyze::analyzeImage(m_refImage);
        const colormatch::analyze::ColorStats tgtStats =
            colormatch::analyze::analyzeImage(m_tgtImage);
        const colormatch::lut::Lut3D lut =
            colormatch::lut::generateMatchLut(tgtStats, refStats, lutSize);
        const QImage preview = applyLutToImage(m_tgtImage, lut);
        m_lblAfter->setPixmap(
            QPixmap::fromImage(preview).scaled(
                200, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

bool ColorMatchDialog::generateCurrentLut(colormatch::lut::Lut3D *lut) const
{
    if (!lut || m_refImage.isNull() || m_tgtImage.isNull())
        return false;

    const int lutSize = m_cbLutSize->currentData().toInt();

    const colormatch::analyze::ColorStats refStats =
        colormatch::analyze::analyzeImage(m_refImage);
    const colormatch::analyze::ColorStats tgtStats =
        colormatch::analyze::analyzeImage(m_tgtImage);

    *lut = colormatch::lut::generateMatchLut(tgtStats, refStats, lutSize);
    return lut->size > 0 && !lut->data.isEmpty();
}

QString ColorMatchDialog::projectAdjacentLutDirectory() const
{
    QString projectPath = m_projectFilePath;
    if (projectPath.isEmpty()) {
        for (const QObject *cursor = this; cursor; cursor = cursor->parent()) {
            projectPath = projectPathFromObject(cursor);
            if (!projectPath.isEmpty())
                break;
        }
    }
    if (projectPath.isEmpty() && qApp && qApp->activeWindow())
        projectPath = projectPathFromObject(qApp->activeWindow());

    if (!projectPath.isEmpty()) {
        const QFileInfo projectInfo(projectPath);
        const QString stem = sanitizedFileStem(projectInfo.completeBaseName());
        return QDir(projectInfo.absolutePath())
            .filePath(stem + QStringLiteral("_ColorMatchLUTs"));
    }

    return QDir(QDir::currentPath()).filePath(QStringLiteral("ColorMatchLUTs"));
}

QString ColorMatchDialog::automaticLutPath() const
{
    QString projectPath = m_projectFilePath;
    if (projectPath.isEmpty()) {
        for (const QObject *cursor = this; cursor; cursor = cursor->parent()) {
            projectPath = projectPathFromObject(cursor);
            if (!projectPath.isEmpty())
                break;
        }
    }
    if (projectPath.isEmpty() && qApp && qApp->activeWindow())
        projectPath = projectPathFromObject(qApp->activeWindow());

    const QString base = projectPath.isEmpty()
        ? QStringLiteral("Untitled")
        : QFileInfo(projectPath).completeBaseName();
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    return QDir(projectAdjacentLutDirectory())
        .filePath(sanitizedFileStem(base)
                  + QStringLiteral("_ColorMatch_")
                  + stamp
                  + QStringLiteral(".cube"));
}

// static
QImage ColorMatchDialog::applyLutToImage(const QImage &img,
                                          const colormatch::lut::Lut3D &lut)
{
    if (img.isNull() || lut.size <= 0 || lut.data.isEmpty()) return img;

    QImage src = img.convertToFormat(QImage::Format_RGB32);
    QImage dst = src.copy();

    const int N = lut.size;
    const int w = src.width();
    const int h = src.height();

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb       *dstLine = reinterpret_cast<QRgb *>(dst.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int ri = qRed(px)   * (N - 1) / 255;
            const int gi = qGreen(px) * (N - 1) / 255;
            const int bi = qBlue(px)  * (N - 1) / 255;

            // Index: R varies fastest (.cube spec)
            const int idx = bi * N * N + gi * N + ri;
            const QVector3D &entry = lut.data.at(idx);

            dstLine[x] = qRgb(
                qBound(0, static_cast<int>(entry.x() * 255.0f + 0.5f), 255),
                qBound(0, static_cast<int>(entry.y() * 255.0f + 0.5f), 255),
                qBound(0, static_cast<int>(entry.z() * 255.0f + 0.5f), 255));
        }
    }
    return dst;
}
