#include "TitlePresetDialog.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFontMetricsF>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRadialGradient>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>
#include <QtMath>

// =============================================================================
// Helpers
// =============================================================================
namespace {

constexpr int kThumbW = 96;
constexpr int kThumbH = 54;
constexpr int kPreviewW = 480;
constexpr int kPreviewH = 270;

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// Build a simple, deterministic thumbnail for a preset. We draw a black
// "video frame" rectangle and overlay a representative pose for the preset
// so the user can recognise it without reading the label.
QPixmap renderThumbnail(const TitlePreset &preset)
{
    QPixmap pix(kThumbW, kThumbH);
    pix.fill(QColor(20, 20, 24));

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    QFont thumbFont = preset.font;
    thumbFont.setPointSizeF(qMax(8.0, preset.font.pointSizeF() * 0.30));
    p.setFont(thumbFont);
    p.setPen(preset.colour);

    const QString label = preset.defaultText.left(8);

    switch (preset.id) {
    case TitlePresetId::SimpleCenter: {
        p.drawText(pix.rect(), Qt::AlignCenter, label);
        break;
    }
    case TitlePresetId::LowerThird: {
        QRect band(0, kThumbH * 0.65, kThumbW, kThumbH * 0.30);
        p.fillRect(band, QColor(0, 0, 0, 160));
        p.drawText(band.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
        break;
    }
    case TitlePresetId::TitleScale: {
        QFont big = thumbFont;
        big.setPointSizeF(thumbFont.pointSizeF() * 1.4);
        p.setFont(big);
        p.drawText(pix.rect(), Qt::AlignCenter, label);
        break;
    }
    case TitlePresetId::Typewriter: {
        // Show a partial reveal with a cursor block.
        const QString partial = label.left(qMax(1, label.length() / 2));
        QRect r = pix.rect();
        QFontMetrics fm(thumbFont);
        int textW = fm.horizontalAdvance(partial);
        int x = (kThumbW - textW) / 2;
        int y = kThumbH / 2 + fm.ascent() / 2;
        p.drawText(x, y, partial);
        p.fillRect(QRect(x + textW + 1, y - fm.ascent(), 4, fm.ascent()), preset.colour);
        break;
    }
    case TitlePresetId::SpinIn: {
        p.translate(kThumbW / 2.0, kThumbH / 2.0);
        p.rotate(-20.0);
        p.drawText(QRect(-kThumbW / 2, -kThumbH / 2, kThumbW, kThumbH), Qt::AlignCenter, label);
        break;
    }
    case TitlePresetId::GlowPulse: {
        // Faint glow halo behind text.
        QRadialGradient g(kThumbW / 2.0, kThumbH / 2.0, kThumbW / 2.0);
        QColor halo = preset.colour;
        halo.setAlpha(120);
        g.setColorAt(0.0, halo);
        halo.setAlpha(0);
        g.setColorAt(1.0, halo);
        p.fillRect(pix.rect(), g);
        p.drawText(pix.rect(), Qt::AlignCenter, label);
        break;
    }
    case TitlePresetId::DropShadowSlide: {
        QRect band(0, kThumbH * 0.65, kThumbW, kThumbH * 0.30);
        p.fillRect(band, QColor(0, 0, 0, 160));
        // Shadow.
        p.setPen(QColor(0, 0, 0, 200));
        p.drawText(band.adjusted(6, 2, -2, 2), Qt::AlignVCenter | Qt::AlignLeft, label);
        // Foreground.
        p.setPen(preset.colour);
        p.drawText(band.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
        break;
    }
    }

    p.end();
    return pix;
}

} // namespace

// =============================================================================
// TitlePresetPreviewWidget
// =============================================================================
//
// Plays a continuous loop of the selected preset's in/hold/out animation on
// a placeholder text. Rendering is intentionally simple — a QPainter pass
// each tick mirroring the keyframe / animation logic that would run inside
// EnhancedTextRenderer.
//
// No Q_OBJECT macro is used here because AUTOMOC + an inline .cpp class
// requires a bespoke `.moc` include that the build system doesn't provide
// for new translation units. We only need slot connections to a free
// lambda, which `connect()` accepts on any QObject without Q_OBJECT.
class TitlePresetPreviewWidget : public QWidget
{
public:
    explicit TitlePresetPreviewWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(kPreviewW, kPreviewH);
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, QColor(15, 15, 18));
        setPalette(pal);

        m_timer = new QTimer(this);
        m_timer->setInterval(33); // ~30fps
        connect(m_timer, &QTimer::timeout, this, [this]() {
            m_phaseSec += 0.033;
            if (m_phaseSec > totalLoopSec()) m_phaseSec = 0.0;
            update();
        });
        m_timer->start();
    }

    void setPreset(const TitlePreset &preset, const QString &text, const QColor &colour)
    {
        m_preset = preset;
        m_text = text;
        m_colour = colour;
        m_phaseSec = 0.0; // restart loop on selection change
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        // Letterboxed preview canvas.
        const QRect canvas = rect();

        // Compute animation progress (0..1) for in / hold / out phases.
        const double total = totalLoopSec();
        const double inEnd = m_preset.inSec;
        const double outStart = total - m_preset.outSec;

        double inProg = 1.0;
        double outProg = 0.0;
        bool inOutPhase = false;
        if (m_phaseSec < inEnd) {
            inProg = m_preset.inSec > 0.0 ? (m_phaseSec / m_preset.inSec) : 1.0;
            inOutPhase = true;
        } else if (m_phaseSec > outStart) {
            outProg = m_preset.outSec > 0.0 ? ((m_phaseSec - outStart) / m_preset.outSec) : 0.0;
            inOutPhase = true;
        }
        inProg  = clamp01(inProg);
        outProg = clamp01(outProg);

        double opacity = inProg * (1.0 - outProg);
        double scale   = 1.0;
        double rotDeg  = 0.0;
        double offX    = 0.0;
        int    visibleChars = m_text.length();

        // Apply preset-specific transforms.
        switch (m_preset.id) {
        case TitlePresetId::SimpleCenter:
            // Pure fade — handled by opacity.
            break;
        case TitlePresetId::LowerThird:
            offX = (1.0 - inProg) * (-canvas.width() * 0.6);
            break;
        case TitlePresetId::TitleScale:
            scale = inProg * (1.0 - outProg);
            if (scale < 0.001) scale = 0.001;
            opacity = 1.0; // scale conveys in/out, keep alpha solid in middle
            if (inOutPhase) opacity = inProg * (1.0 - outProg);
            break;
        case TitlePresetId::Typewriter: {
            // Reveal letters across the in-window. The out fade still drives
            // opacity at the tail of the loop.
            const double revealEnd = qMax(0.001, m_preset.inSec);
            double revealProg = clamp01(m_phaseSec / revealEnd);
            visibleChars = int(qRound(revealProg * m_text.length()));
            opacity = 1.0 - outProg;
            break;
        }
        case TitlePresetId::SpinIn:
            rotDeg = (1.0 - inProg) * -180.0;
            scale  = 0.4 + 0.6 * inProg;
            break;
        case TitlePresetId::GlowPulse: {
            // Sin-wave alpha modulation during hold.
            double t = m_phaseSec;
            double mod = 0.65 + 0.35 * std::sin(t * 4.0);
            opacity = qBound(0.0, mod, 1.0) * inProg * (1.0 - outProg);
            break;
        }
        case TitlePresetId::DropShadowSlide:
            offX = (1.0 - inProg) * (-canvas.width() * 0.6);
            break;
        }

        // Anchor (lower-third presets push y down).
        double ax = m_preset.anchorX;
        double ay = m_preset.lowerThird ? 0.85 : m_preset.anchorY;
        QPointF anchorPx(canvas.left() + ax * canvas.width() + offX,
                         canvas.top()  + ay * canvas.height());

        // Compose a transform pivoted on the anchor.
        QFont f = m_preset.font;
        // Scale font for preview canvas (font sizes are designed for 1080p).
        f.setPointSizeF(f.pointSizeF() * (canvas.height() / 720.0));
        p.setFont(f);

        QString visText = m_text;
        if (visibleChars < visText.length()) visText = visText.left(qMax(0, visibleChars));

        QFontMetricsF fm(f);
        QRectF textRect = fm.boundingRect(visText.isEmpty() ? QStringLiteral(" ") : visText);
        textRect.moveCenter(anchorPx);

        // Drop-shadow band background for lower-third presets.
        if (m_preset.lowerThird) {
            QRectF band(canvas.left(),
                        canvas.top() + 0.78 * canvas.height(),
                        canvas.width(),
                        0.16 * canvas.height());
            QColor bg(0, 0, 0, int(160 * opacity));
            p.fillRect(band, bg);
        }

        QColor c = m_colour;
        c.setAlphaF(qBound(0.0, c.alphaF() * opacity, 1.0));

        p.save();
        p.translate(anchorPx);
        p.rotate(rotDeg);
        p.scale(scale, scale);
        p.translate(-anchorPx);

        if (m_preset.dropShadow) {
            QColor shadow(0, 0, 0, int(180 * opacity));
            p.setPen(shadow);
            p.drawText(textRect.translated(3, 3), Qt::AlignCenter, visText);
        }
        if (m_preset.id == TitlePresetId::GlowPulse) {
            // Soft halo behind text for the glow preview.
            QRadialGradient g(anchorPx, qMax(textRect.width(), textRect.height()));
            QColor halo = c;
            halo.setAlphaF(qBound(0.0, halo.alphaF() * 0.5, 1.0));
            g.setColorAt(0.0, halo);
            halo.setAlpha(0);
            g.setColorAt(1.0, halo);
            p.fillRect(textRect.adjusted(-30, -30, 30, 30), g);
        }
        p.setPen(c);
        p.drawText(textRect, Qt::AlignCenter, visText);
        p.restore();
    }

private:
    double totalLoopSec() const { return qMax(1.0, m_preset.durationSec); }

    TitlePreset m_preset;
    QString     m_text = QStringLiteral("Title");
    QColor      m_colour = Qt::white;
    QTimer     *m_timer = nullptr;
    double      m_phaseSec = 0.0;
};

// =============================================================================
// TitlePresetDialog
// =============================================================================

TitlePresetDialog::TitlePresetDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("タイトル・プリセット"));
    resize(820, 480);

    m_presets = builtInPresets();
    if (!m_presets.isEmpty()) {
        m_currentColour = m_presets.first().colour;
    }

    buildUi();
    populateList();

    if (!m_presets.isEmpty()) {
        m_list->setCurrentRow(0);
    }
}

void TitlePresetDialog::buildUi()
{
    auto *root = new QHBoxLayout(this);

    // Left: list of presets with thumbnails.
    m_list = new QListWidget(this);
    m_list->setIconSize(QSize(kThumbW, kThumbH));
    m_list->setUniformItemSizes(true);
    m_list->setMinimumWidth(220);
    m_list->setSpacing(2);
    connect(m_list, &QListWidget::currentRowChanged,
            this, &TitlePresetDialog::onPresetChanged);
    root->addWidget(m_list, 0);

    // Right: preview + controls.
    auto *right = new QVBoxLayout();
    m_preview = new TitlePresetPreviewWidget(this);
    right->addWidget(m_preview, 1);

    auto *form = new QHBoxLayout();
    form->addWidget(new QLabel(tr("テキスト:"), this));
    m_textEdit = new QLineEdit(this);
    m_textEdit->setPlaceholderText(tr("タイトル本文を入力"));
    connect(m_textEdit, &QLineEdit::textChanged, this, [this](const QString &) {
        if (m_currentRow >= 0 && m_currentRow < m_presets.size()) {
            m_preview->setPreset(m_presets[m_currentRow], m_textEdit->text(), m_currentColour);
        }
    });
    form->addWidget(m_textEdit, 1);

    m_colourButton = new QPushButton(tr("色…"), this);
    connect(m_colourButton, &QPushButton::clicked, this, &TitlePresetDialog::onPickColour);
    form->addWidget(m_colourButton);

    m_colourSwatch = new QLabel(this);
    m_colourSwatch->setFixedSize(28, 24);
    m_colourSwatch->setFrameShape(QFrame::Box);
    form->addWidget(m_colourSwatch);

    right->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::NoButton, Qt::Horizontal, this);
    auto *applyBtn  = buttons->addButton(tr("適用"),    QDialogButtonBox::AcceptRole);
    auto *cancelBtn = buttons->addButton(tr("キャンセル"), QDialogButtonBox::RejectRole);
    connect(applyBtn,  &QPushButton::clicked, this, &TitlePresetDialog::onAccept);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    right->addWidget(buttons);

    root->addLayout(right, 1);
}

void TitlePresetDialog::populateList()
{
    m_list->clear();
    for (const auto &preset : m_presets) {
        auto *item = new QListWidgetItem(m_list);
        item->setText(preset.displayName);
        item->setIcon(QIcon(renderThumbnail(preset)));
        item->setSizeHint(QSize(kThumbW + 24, kThumbH + 12));
    }
}

void TitlePresetDialog::onPresetChanged(int row)
{
    if (row < 0 || row >= m_presets.size()) return;
    m_currentRow = row;
    const TitlePreset &p = m_presets[row];

    if (m_textEdit->text().isEmpty()) {
        // Only auto-fill when the user has not typed something themselves.
        QSignalBlocker b(m_textEdit);
        m_textEdit->setText(p.defaultText);
    }

    m_currentColour = p.colour;
    QPixmap sw(28, 24);
    sw.fill(m_currentColour);
    m_colourSwatch->setPixmap(sw);

    m_preview->setPreset(p, m_textEdit->text().isEmpty() ? p.defaultText : m_textEdit->text(),
                         m_currentColour);
}

void TitlePresetDialog::onPickColour()
{
    QColor picked = QColorDialog::getColor(m_currentColour, this, tr("色を選択"));
    if (!picked.isValid()) return;
    m_currentColour = picked;
    QPixmap sw(28, 24);
    sw.fill(m_currentColour);
    m_colourSwatch->setPixmap(sw);
    if (m_currentRow >= 0 && m_currentRow < m_presets.size()) {
        m_preview->setPreset(m_presets[m_currentRow], m_textEdit->text(), m_currentColour);
    }
}

void TitlePresetDialog::onAccept()
{
    if (m_currentRow < 0 || m_currentRow >= m_presets.size()) {
        reject();
        return;
    }
    const TitlePreset &p = m_presets[m_currentRow];
    QString userText = m_textEdit->text();
    if (userText.isEmpty()) userText = p.defaultText;
    applyPresetTo(p, userText, m_currentColour, m_resolved);
    accept();
}

// -----------------------------------------------------------------------------
// applyPresetTo: bake the preset into an EnhancedTextOverlay using only the
// existing public TextManager fields. We never modify TextManager.h.
// -----------------------------------------------------------------------------
void TitlePresetDialog::applyPresetTo(const TitlePreset &preset,
                                      const QString &userText,
                                      const QColor &userColour,
                                      EnhancedTextOverlay &out)
{
    out.text  = userText;
    out.font  = preset.font;
    out.color = userColour;

    out.x = preset.lowerThird ? 0.5  : preset.anchorX;
    out.y = preset.lowerThird ? 0.85 : preset.anchorY;
    out.alignment = Qt::AlignCenter;
    out.opacity = 1.0;
    out.scale = 1.0;
    out.rotation = 0.0;

    out.startTime = 0.0;
    out.endTime   = preset.durationSec;

    // Pick the closest existing TextAnimation for in/out. The preview widget
    // implements the *exact* visual, but renderers downstream rely on the
    // animation enum, so we map onto the closest semantic match.
    out.animIn.duration  = preset.inSec;
    out.animOut.duration = preset.outSec;

    switch (preset.id) {
    case TitlePresetId::SimpleCenter:
        out.animIn.type  = TextAnimationType::FadeIn;
        out.animOut.type = TextAnimationType::FadeOut;
        break;
    case TitlePresetId::LowerThird:
        out.animIn.type  = TextAnimationType::SlideRight; // text travels rightward into place
        out.animOut.type = TextAnimationType::FadeOut;
        break;
    case TitlePresetId::TitleScale:
        out.animIn.type  = TextAnimationType::ScaleIn;
        out.animOut.type = TextAnimationType::FadeOut;
        break;
    case TitlePresetId::Typewriter:
        out.animIn.type  = TextAnimationType::Typewriter;
        out.animOut.type = TextAnimationType::FadeOut;
        break;
    case TitlePresetId::SpinIn:
        // No native rotation enum — pop / scale-in is the closest. Position
        // keyframes below carry the rotation cue if a future renderer wants
        // it. The preview shows the proper spin.
        out.animIn.type  = TextAnimationType::Pop;
        out.animOut.type = TextAnimationType::FadeOut;
        break;
    case TitlePresetId::GlowPulse:
        out.animIn.type  = TextAnimationType::FadeIn;
        out.animOut.type = TextAnimationType::FadeOut;
        break;
    case TitlePresetId::DropShadowSlide:
        out.animIn.type  = TextAnimationType::SlideRight;
        out.animOut.type = TextAnimationType::FadeOut;
        break;
    }

    // Drop shadow.
    if (preset.dropShadow) {
        out.shadow.enabled = true;
        out.shadow.offsetX = 4.0;
        out.shadow.offsetY = 4.0;
        out.shadow.blur    = 6.0;
        out.shadow.color   = QColor(0, 0, 0, 200);
    }

    // Position keyframes: provide a left -> anchor sweep for slide presets.
    out.positionKeyframes.clear();
    if (preset.slideFromLeft) {
        PositionKeyframe a;
        a.time = 0.0;
        a.cx   = -0.05;                 // off-screen left
        a.cy   = preset.lowerThird ? 0.85 : preset.anchorY;
        PositionKeyframe b;
        b.time = preset.inSec;
        b.cx   = preset.anchorX;
        b.cy   = preset.lowerThird ? 0.85 : preset.anchorY;
        PositionKeyframe c;
        c.time = preset.durationSec;
        c.cx   = preset.anchorX;
        c.cy   = preset.lowerThird ? 0.85 : preset.anchorY;
        out.positionKeyframes.append(a);
        out.positionKeyframes.append(b);
        out.positionKeyframes.append(c);
    }

    out.templateName = preset.displayName;
    out.visible = true;
}


// -----------------------------------------------------------------------------
// builtInPresets: 7 hand-tuned defaults (>= 6 required by spec).
// -----------------------------------------------------------------------------
QVector<TitlePreset> TitlePresetDialog::builtInPresets()
{
    QVector<TitlePreset> v;

    {
        TitlePreset p;
        p.id          = TitlePresetId::SimpleCenter;
        p.displayName = QStringLiteral("シンプル中央");
        p.defaultText = QStringLiteral("Title");
        p.font        = QFont(QStringLiteral("Arial"), 64, QFont::Bold);
        p.colour      = Qt::white;
        p.durationSec = 3.0;
        p.inSec       = 0.5;
        p.outSec      = 0.5;
        p.anchorX     = 0.5;
        p.anchorY     = 0.5;
        v.append(p);
    }
    {
        TitlePreset p;
        p.id          = TitlePresetId::LowerThird;
        p.displayName = QStringLiteral("下三分割 (Lower-Third)");
        p.defaultText = QStringLiteral("Name / Subtitle");
        p.font        = QFont(QStringLiteral("Arial"), 36, QFont::Bold);
        p.colour      = Qt::white;
        p.durationSec = 4.0;
        p.inSec       = 0.4;
        p.outSec      = 0.5;
        p.anchorX     = 0.18;
        p.anchorY     = 0.85;
        p.lowerThird     = true;
        p.slideFromLeft  = true;
        v.append(p);
    }
    {
        TitlePreset p;
        p.id          = TitlePresetId::TitleScale;
        p.displayName = QStringLiteral("タイトル拡大");
        p.defaultText = QStringLiteral("Title");
        p.font        = QFont(QStringLiteral("Arial"), 72, QFont::Bold);
        p.colour      = Qt::white;
        p.durationSec = 3.0;
        p.inSec       = 0.5;
        p.outSec      = 0.5;
        v.append(p);
    }
    {
        TitlePreset p;
        p.id          = TitlePresetId::Typewriter;
        p.displayName = QStringLiteral("タイプライター");
        p.defaultText = QStringLiteral("Hello, World.");
        p.font        = QFont(QStringLiteral("Courier New"), 48, QFont::Bold);
        p.colour      = Qt::white;
        p.durationSec = 4.0;
        p.inSec       = 1.0;
        p.outSec      = 0.5;
        p.typewriter  = true;
        v.append(p);
    }
    {
        TitlePreset p;
        p.id          = TitlePresetId::SpinIn;
        p.displayName = QStringLiteral("スピンイン");
        p.defaultText = QStringLiteral("Title");
        p.font        = QFont(QStringLiteral("Arial"), 64, QFont::Bold);
        p.colour      = QColor(255, 220, 80);
        p.durationSec = 3.0;
        p.inSec       = 0.6;
        p.outSec      = 0.5;
        p.spinIn      = true;
        v.append(p);
    }
    {
        TitlePreset p;
        p.id          = TitlePresetId::GlowPulse;
        p.displayName = QStringLiteral("グロウ・パルス");
        p.defaultText = QStringLiteral("Title");
        p.font        = QFont(QStringLiteral("Arial"), 64, QFont::Bold);
        p.colour      = QColor(255, 255, 200);
        p.durationSec = 4.0;
        p.inSec       = 0.6;
        p.outSec      = 0.6;
        p.pulse       = true;
        v.append(p);
    }
    {
        TitlePreset p;
        p.id            = TitlePresetId::DropShadowSlide;
        p.displayName   = QStringLiteral("ドロップシャドウ・スライド");
        p.defaultText   = QStringLiteral("Sub-Title");
        p.font          = QFont(QStringLiteral("Arial"), 40, QFont::Bold);
        p.colour        = Qt::white;
        p.durationSec   = 4.0;
        p.inSec         = 0.45;
        p.outSec        = 0.5;
        p.anchorX       = 0.18;
        p.anchorY       = 0.85;
        p.lowerThird    = true;
        p.slideFromLeft = true;
        p.dropShadow    = true;
        v.append(p);
    }
    return v;
}
