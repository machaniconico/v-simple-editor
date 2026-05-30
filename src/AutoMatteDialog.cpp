#include "AutoMatteDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QPushButton>
#include <QTabWidget>
#include <QFileDialog>
#include <QPixmap>
#include <QGroupBox>

#include <cstdint>
#include <vector>

namespace {

constexpr int kPreviewSize = 320;
// ライブプレビューの純粋処理は縮小サイズで実行 (重い全解像度処理を避ける)。
constexpr int kProcessMax  = 256;

QImage scaledToFit(const QImage &image, int maxSide)
{
    if (image.isNull() || maxSide <= 0)
        return QImage();
    if (image.width() <= maxSide && image.height() <= maxSide)
        return image;
    return image.scaled(maxSide, maxSide,
                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QImage scaledExact(const QImage &image, const QSize &size)
{
    if (image.isNull() || size.isEmpty())
        return QImage();
    if (image.size() == size)
        return image;
    return image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

} // namespace

AutoMatteDialog::AutoMatteDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("自動背景除去 / マッティング"));
    setModal(false);

    // --- プレビュー (タブ: 元画像 / マット / 合成結果) ---
    auto makePreviewLabel = [this](const QString &placeholder) -> QLabel * {
        auto *lbl = new QLabel(placeholder);
        lbl->setFixedSize(kPreviewSize, kPreviewSize);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet("background: #1a1a1a; border: 1px solid #444; color: #888;");
        return lbl;
    };

    m_sourceView = makePreviewLabel(tr("元画像 (未設定)"));
    m_matteView  = makePreviewLabel(tr("マット"));
    m_resultView = makePreviewLabel(tr("合成結果"));

    m_tabs = new QTabWidget;
    m_tabs->addTab(m_sourceView, tr("元画像"));
    m_tabs->addTab(m_matteView,  tr("マット"));
    m_tabs->addTab(m_resultView, tr("合成結果"));

    // --- パラメータ コントロール ---
    m_thresholdSlider = new QSlider(Qt::Horizontal);
    m_thresholdSlider->setRange(0, 100);
    m_thresholdSlider->setValue(static_cast<int>(m_params.threshold * 100));

    auto makeSpin = [](int initial) -> QSpinBox * {
        auto *s = new QSpinBox;
        s->setRange(0, 32);
        s->setValue(initial);
        s->setSuffix(QStringLiteral(" px"));
        return s;
    };
    m_erodeSpin   = makeSpin(m_params.erode);
    m_dilateSpin  = makeSpin(m_params.dilate);
    m_featherSpin = makeSpin(m_params.featherRadius);

    m_spillSlider = new QSlider(Qt::Horizontal);
    m_spillSlider->setRange(0, 100);
    m_spillSlider->setValue(static_cast<int>(m_params.spillSuppress * 100));

    connect(m_thresholdSlider, &QSlider::valueChanged,  this, &AutoMatteDialog::onParamChanged);
    connect(m_erodeSpin,   QOverload<int>::of(&QSpinBox::valueChanged), this, &AutoMatteDialog::onParamChanged);
    connect(m_dilateSpin,  QOverload<int>::of(&QSpinBox::valueChanged), this, &AutoMatteDialog::onParamChanged);
    connect(m_featherSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &AutoMatteDialog::onParamChanged);
    connect(m_spillSlider, &QSlider::valueChanged,      this, &AutoMatteDialog::onParamChanged);

    auto *form = new QFormLayout;
    form->addRow(tr("しきい値:"),       m_thresholdSlider);
    form->addRow(tr("収縮 (erode):"),   m_erodeSpin);
    form->addRow(tr("膨張 (dilate):"),  m_dilateSpin);
    form->addRow(tr("フェザー:"),       m_featherSpin);
    form->addRow(tr("スピル抑制:"),     m_spillSlider);

    auto *formBox = new QGroupBox(tr("パラメータ"));
    formBox->setLayout(form);

    // --- 画像読み込み / 適用ボタン ---
    m_loadPlateBtn = new QPushButton(tr("プレート読み込み..."));
    m_loadBgBtn    = new QPushButton(tr("新背景読み込み..."));
    m_applyBtn     = new QPushButton(tr("適用"));
    m_applyBtn->setDefault(true);
    m_applyBtn->setEnabled(false);

    connect(m_loadPlateBtn, &QPushButton::clicked, this, &AutoMatteDialog::onLoadPlateClicked);
    connect(m_loadBgBtn,    &QPushButton::clicked, this, &AutoMatteDialog::onLoadBackgroundClicked);
    connect(m_applyBtn,     &QPushButton::clicked, this, &AutoMatteDialog::onApplyClicked);

    auto *loadRow = new QHBoxLayout;
    loadRow->addWidget(m_loadPlateBtn);
    loadRow->addWidget(m_loadBgBtn);

    auto *btnRow = new QHBoxLayout;
    auto *closeBtn = new QPushButton(tr("閉じる"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(m_applyBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);

    // --- 組み立て ---
    auto *root = new QVBoxLayout(this);
    root->addWidget(m_tabs);
    root->addLayout(loadRow);
    root->addWidget(formBox);
    root->addLayout(btnRow);
}

// --- public setters ---

void AutoMatteDialog::setSourceImage(const QImage &fg)
{
    m_source = fg;
    refreshPreviewInputs();
    rebuild(false);
}

void AutoMatteDialog::setBackgroundPlate(const QImage &plate)
{
    m_plate = plate;
    refreshPreviewInputs();
    rebuild(false);
}

void AutoMatteDialog::setNewBackground(const QImage &bg)
{
    m_newBg = bg;
    refreshPreviewInputs();
    rebuild(false);
}

// --- slots ---

void AutoMatteDialog::onLoadPlateClicked()
{
    QString path = QFileDialog::getOpenFileName(
        this,
        tr("クリーンプレートを開く"),
        QString(),
        tr("画像 (*.png *.jpg *.jpeg *.bmp *.tiff *.tif *.webp);;すべてのファイル (*)"));
    if (path.isEmpty())
        return;

    QImage img(path);
    if (img.isNull())
        return;

    setBackgroundPlate(img);
}

void AutoMatteDialog::onLoadBackgroundClicked()
{
    QString path = QFileDialog::getOpenFileName(
        this,
        tr("新背景を開く"),
        QString(),
        tr("画像 (*.png *.jpg *.jpeg *.bmp *.tiff *.tif *.webp);;すべてのファイル (*)"));
    if (path.isEmpty())
        return;

    QImage img(path);
    if (img.isNull())
        return;

    setNewBackground(img);
}

void AutoMatteDialog::onParamChanged()
{
    m_params.threshold     = m_thresholdSlider->value() / 100.0;
    m_params.erode         = m_erodeSpin->value();
    m_params.dilate        = m_dilateSpin->value();
    m_params.featherRadius = m_featherSpin->value();
    m_params.spillSuppress = m_spillSlider->value() / 100.0;
    rebuild(false);
}

void AutoMatteDialog::onApplyClicked()
{
    if (m_source.isNull())
        return;

    // 適用時だけ元解像度で再計算し、呼び出し側へ返す結果を縮小プレビューで上書きしない。
    rebuild(true);
    if (m_matte.isNull() && m_result.isNull())
        return;
    emit applied();
    accept();
}

// --- private helpers ---

void AutoMatteDialog::rebuild(bool fullResolution)
{
    if (m_source.isNull()) {
        // 画像未設定時は安全に no-op (プレースホルダのまま)。
        m_matte  = QImage();
        m_result = QImage();
        updatePreviews();
        return;
    }

    // ライブプレビューではキャッシュ済み縮小画像、適用時は元解像度を使う。
    QImage src = fullResolution ? m_source : m_previewSourceProcess;
    if (src.isNull())
        src = fullResolution ? m_source : scaledToFit(m_source, kProcessMax);
    const int w = src.width();
    const int h = src.height();
    if (w <= 0 || h <= 0) {
        m_matte  = QImage();
        m_result = QImage();
        updatePreviews();
        return;
    }

    std::vector<uint8_t> fgRgba = automatte::qimageToRgba(src);

    // 1) マット生成: プレートあり → 差分マット、無ければ autoSegment。
    std::vector<uint8_t> rawMatte;
    if (!m_plate.isNull()) {
        QImage plate = fullResolution ? scaledExact(m_plate, src.size()) : m_previewPlateProcess;
        if (plate.isNull() || plate.size() != src.size())
            plate = scaledExact(m_plate, src.size());
        std::vector<uint8_t> plateRgba = automatte::qimageToRgba(plate);
        rawMatte = automatte::differenceMatte(fgRgba, plateRgba, w, h, m_params.threshold);
    } else {
        rawMatte = automatte::autoSegment(fgRgba, w, h, m_params.threshold);
    }

    // 2) 精緻化 (erode → dilate → feather)。
    std::vector<uint8_t> matte = automatte::refineMatte(rawMatte, w, h, m_params);

    // 3) スピル抑制 (任意)。
    std::vector<uint8_t> cleaned = fgRgba;
    if (m_params.spillSuppress > 0.0 && !matte.empty()) {
        cleaned = automatte::suppressSpill(fgRgba, matte, w, h, m_params.spillSuppress);
    }

    // 4) 出力: 新背景あり → composite、無ければ applyMatteAsAlpha (透過)。
    std::vector<uint8_t> outRgba;
    if (matte.empty()) {
        outRgba = cleaned;
    } else if (!m_newBg.isNull()) {
        QImage bg = fullResolution ? scaledExact(m_newBg, src.size()) : m_previewBgProcess;
        if (bg.isNull() || bg.size() != src.size())
            bg = scaledExact(m_newBg, src.size());
        std::vector<uint8_t> bgRgba = automatte::qimageToRgba(bg);
        outRgba = automatte::composite(cleaned, bgRgba, matte, w, h);
        if (outRgba.empty())
            outRgba = cleaned;
    } else {
        outRgba = automatte::applyMatteAsAlpha(cleaned, matte, w, h);
    }

    // QImage 化して保持。
    m_result = automatte::rgbaToQImage(outRgba, w, h);

    // マットをグレースケール QImage 化 (matte は長さ w*h の 1ch)。
    if (matte.empty()) {
        m_matte = QImage();
    } else {
        const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::vector<uint8_t> matteRgba(pixels * 4u);
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t v = matte[i];
            matteRgba[i * 4 + 0] = v;
            matteRgba[i * 4 + 1] = v;
            matteRgba[i * 4 + 2] = v;
            matteRgba[i * 4 + 3] = 255;
        }
        m_matte = automatte::rgbaToQImage(matteRgba, w, h);
    }

    if (!fullResolution)
        updatePreviews();
}

void AutoMatteDialog::refreshPreviewInputs()
{
    m_sourcePreview = scaledToFit(m_source, kPreviewSize);
    m_previewSourceProcess = scaledToFit(m_source, kProcessMax);

    if (!m_previewSourceProcess.isNull()) {
        const QSize processSize = m_previewSourceProcess.size();
        m_previewPlateProcess = scaledExact(m_plate, processSize);
        m_previewBgProcess = scaledExact(m_newBg, processSize);
    } else {
        m_previewPlateProcess = QImage();
        m_previewBgProcess = QImage();
    }

    if (m_applyBtn)
        m_applyBtn->setEnabled(!m_source.isNull());
}

void AutoMatteDialog::updatePreviews()
{
    // 元画像。
    if (m_source.isNull()) {
        m_sourceView->setText(tr("元画像 (未設定)"));
    } else {
        const QImage shown = m_sourcePreview.isNull()
            ? scaledToFit(m_source, kPreviewSize)
            : m_sourcePreview;
        m_sourceView->setPixmap(
            QPixmap::fromImage(shown).scaled(
                kPreviewSize, kPreviewSize,
                Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    // マット。
    if (m_matte.isNull()) {
        m_matteView->setText(tr("マット"));
    } else {
        m_matteView->setPixmap(
            QPixmap::fromImage(m_matte).scaled(
                kPreviewSize, kPreviewSize,
                Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    // 合成結果 (透過の場合はチェッカーボード上で表示)。
    if (m_result.isNull()) {
        m_resultView->setText(tr("合成結果"));
    } else {
        QImage shown;
        if (m_newBg.isNull()) {
            // 透過マット結果: 市松模様の上に重ねて透過を可視化。
            QImage rgba = m_result.convertToFormat(QImage::Format_RGBA8888);
            QImage bg = makeCheckerboard(rgba.width(), rgba.height());
            QImage out(rgba.width(), rgba.height(), QImage::Format_RGB32);
            for (int y = 0; y < rgba.height(); ++y) {
                const uint8_t *src = rgba.constScanLine(y);
                const QRgb *bgp = reinterpret_cast<const QRgb *>(bg.constScanLine(y));
                QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
                for (int x = 0; x < rgba.width(); ++x) {
                    const uint8_t r = src[x * 4 + 0];
                    const uint8_t g = src[x * 4 + 1];
                    const uint8_t b = src[x * 4 + 2];
                    const double a = src[x * 4 + 3] / 255.0;
                    const int rr = static_cast<int>(r * a + qRed(bgp[x])   * (1.0 - a));
                    const int gg = static_cast<int>(g * a + qGreen(bgp[x]) * (1.0 - a));
                    const int bb = static_cast<int>(b * a + qBlue(bgp[x])  * (1.0 - a));
                    dst[x] = qRgb(rr, gg, bb);
                }
            }
            shown = out;
        } else {
            shown = m_result;
        }
        m_resultView->setPixmap(
            QPixmap::fromImage(shown).scaled(
                kPreviewSize, kPreviewSize,
                Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

QImage AutoMatteDialog::makeCheckerboard(int w, int h, int tileSize) const
{
    QImage cb(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool even = ((x / tileSize) + (y / tileSize)) % 2 == 0;
            cb.setPixel(x, y, even ? qRgb(200, 200, 200) : qRgb(140, 140, 140));
        }
    }
    return cb;
}
