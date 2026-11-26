#include "ContextMenuOrder.h"

#include "Timeline.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
}

namespace ctxmenu {

namespace {

constexpr const char *kSettingsOrg = "VSimpleEditor";
constexpr const char *kSettingsApp = "Preferences";

QString orderSettingsKey(Target target)
{
    return QStringLiteral("contextMenu/%1/order").arg(targetKey(target));
}

QString hiddenSettingsKey(Target target)
{
    return QStringLiteral("contextMenu/%1/hidden").arg(targetKey(target));
}

double probeAudioDurationSec(const QString &path)
{
    AVFormatContext *fmt = nullptr;
    double duration = 0.0;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->duration > 0)
            duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        avformat_close_input(&fmt);
    }
    return duration;
}

} // namespace

QString targetKey(Target target)
{
    switch (target) {
    case Target::PreviewText:       return QStringLiteral("preview_text");
    case Target::PreviewVideoClip:  return QStringLiteral("preview_video");
    case Target::PreviewBackground: return QStringLiteral("preview_background");
    case Target::TimelineAudioClip: return QStringLiteral("timeline_audio");
    }
    return QStringLiteral("preview_background");
}

QString targetDisplayName(Target target)
{
    switch (target) {
    case Target::PreviewText:       return QStringLiteral("プレビュー: テキスト");
    case Target::PreviewVideoClip:  return QStringLiteral("プレビュー: 映像クリップ");
    case Target::PreviewBackground: return QStringLiteral("プレビュー: 背景");
    case Target::TimelineAudioClip: return QStringLiteral("タイムライン: 音声クリップ");
    }
    return QStringLiteral("プレビュー: 背景");
}

QVector<Target> allTargets()
{
    return {
        Target::PreviewText,
        Target::PreviewVideoClip,
        Target::PreviewBackground,
        Target::TimelineAudioClip
    };
}

QVector<ActionDef> defaultActionsFor(Target target)
{
    switch (target) {
    case Target::PreviewText:
        return {
            { QStringLiteral("text.inline_edit"),        QStringLiteral("テキストを編集 (インライン)"), true, false },
            { QStringLiteral("text.add_sfx"),            QStringLiteral("効果音を追加…"),               true, false },
            { QStringLiteral("text.duration"),           QStringLiteral("表示時間を変更…"),             true, true  },
            { QStringLiteral("text.style_color"),        QStringLiteral("色を変更…"),                   true, false },
            { QStringLiteral("text.font_size"),          QStringLiteral("フォントサイズを変更…"),       true, false },
            { QStringLiteral("text.center"),             QStringLiteral("中央揃え"),                    true, true  },
            { QStringLiteral("text.duplicate"),          QStringLiteral("テキストを複製"),              true, false },
            { QStringLiteral("text.delete"),             QStringLiteral("テキストを削除"),              true, true  },
            { QStringLiteral("common.display_settings"), QStringLiteral("表示設定"),                    true, false },
        };
    case Target::PreviewVideoClip:
        return {
            { QStringLiteral("video.split"),             QStringLiteral("再生ヘッドで分割"),            true, false },
            { QStringLiteral("video.duplicate"),         QStringLiteral("複製"),                        true, false },
            { QStringLiteral("video.delete"),            QStringLiteral("削除"),                        true, true  },
            { QStringLiteral("video.speed"),             QStringLiteral("速度を変更…"),                 true, false },
            { QStringLiteral("video.speed_ramp"),        QStringLiteral("スピードランプ…"),             true, false },
            { QStringLiteral("video.reset_transform"),   QStringLiteral("変形をリセット"),              true, true  },
            { QStringLiteral("video.effects"),           QStringLiteral("エフェクトパネルを開く"),      true, false },
            { QStringLiteral("video.color"),             QStringLiteral("カラー調整を開く"),            true, false },
            { QStringLiteral("video.sns_fit"),           QStringLiteral("SNS フィット"),                true, true  },
            { QStringLiteral("video.silence_cut"),       QStringLiteral("無音を自動カット…"),           true, false },
            { QStringLiteral("video.beat_markers"),      QStringLiteral("ビートでマーカー…"),           true, true  },
            { QStringLiteral("common.display_settings"), QStringLiteral("表示設定"),                    true, false },
        };
    case Target::PreviewBackground:
        return {
            { QStringLiteral("bg.add_text"),                QStringLiteral("テキストを追加"),               true, false },
            { QStringLiteral("bg.add_clip"),                QStringLiteral("クリップを追加…"),              true, true  },
            { QStringLiteral("common.pause_on_rightclick"), QStringLiteral("右クリックで一時停止"),         true, true  },
            { QStringLiteral("common.safe_zone"),           QStringLiteral("SNS セーフゾーン"),             true, false },
            { QStringLiteral("common.monitoring"),          QStringLiteral("モニタリング (露出/フォーカス)"), true, false },
            { QStringLiteral("common.onion_skin"),          QStringLiteral("オニオンスキン"),               true, false },
            { QStringLiteral("common.preview_quality"),     QStringLiteral("再生プレビュー品質"),           true, false },
        };
    case Target::TimelineAudioClip:
        return {
            { QStringLiteral("audio.cut"),               QStringLiteral("カット"),                      true, false },
            { QStringLiteral("audio.copy"),              QStringLiteral("コピー"),                      true, false },
            { QStringLiteral("audio.delete"),            QStringLiteral("削除"),                        true, false },
            { QStringLiteral("audio.replace"),           QStringLiteral("置き換え"),                    true, true  },
            { QStringLiteral("audio.unlink"),            QStringLiteral("同期を切る"),                  true, true  },
            { QStringLiteral("audio.normalize"),         QStringLiteral("ノーマライズ"),                true, false },
            { QStringLiteral("audio.dialogue_leveler"),  QStringLiteral("ダイアログレベラー…"),         true, false },
            { QStringLiteral("audio.music_remix"),       QStringLiteral("ミュージックリミックス…"),     true, true  },
            { QStringLiteral("audio.channel_map"),       QStringLiteral("チャンネルマッピング"),        true, true  },
            { QStringLiteral("audio.clip_eq"),           QStringLiteral("クリップ EQ…"),                true, false },
            { QStringLiteral("audio.enabled"),           QStringLiteral("クリップを有効にする"),        true, false },
            { QStringLiteral("audio.label"),             QStringLiteral("ラベルカラー"),                true, true  },
            { QStringLiteral("audio.transitions"),       QStringLiteral("音声トランジション"),          true, false },
            { QStringLiteral("audio.new_sequence"),      QStringLiteral("このクリップから新規シーケンス"), true, true },
            { QStringLiteral("audio.storyline"),         QStringLiteral("主ストーリーライン接続 / 解除"), true, false },
        };
    }
    return {};
}

QVector<ResolvedItem> resolveOrder(const QVector<ActionDef> &defaults,
                                   const QStringList &savedOrder,
                                   const QSet<QString> &savedHidden)
{
    QHash<QString, ActionDef> defById;
    defById.reserve(defaults.size());
    for (const auto &d : defaults)
        defById.insert(d.id, d);

    QStringList finalOrder;
    finalOrder.reserve(defaults.size());
    QSet<QString> seen;

    for (const QString &id : savedOrder) {
        if (!defById.contains(id) || seen.contains(id))
            continue;
        seen.insert(id);
        finalOrder.append(id);
    }
    for (const auto &d : defaults) {
        if (!seen.contains(d.id)) {
            seen.insert(d.id);
            finalOrder.append(d.id);
        }
    }

    const bool isDefaultOrder = [&]() {
        if (finalOrder.size() != defaults.size())
            return false;
        for (int i = 0; i < defaults.size(); ++i) {
            if (finalOrder[i] != defaults[i].id)
                return false;
        }
        return true;
    }();

    QVector<ResolvedItem> out;
    out.reserve(finalOrder.size());
    int visibleCount = 0;
    for (const QString &id : finalOrder) {
        const ActionDef &d = defById[id];
        const bool vis = !savedHidden.contains(id) && d.defaultVisible;
        if (vis)
            ++visibleCount;
        ResolvedItem item;
        item.id = d.id;
        item.label = d.label;
        item.visible = vis;
        item.separatorAfter = isDefaultOrder && d.separatorAfterDefault;
        out.append(item);
    }

    // Never hide every single action in a context menu.
    if (visibleCount == 0) {
        for (auto &item : out)
            item.visible = true;
    }
    return out;
}

QVector<ResolvedItem> loadLayout(Target target, QSettings *settings)
{
    const QVector<ActionDef> defaults = defaultActionsFor(target);
    QSettings fallback(QString::fromLatin1(kSettingsOrg), QString::fromLatin1(kSettingsApp));
    QSettings &s = settings ? *settings : fallback;
    const QStringList savedOrder = s.value(orderSettingsKey(target)).toStringList();
    const QStringList hiddenList = s.value(hiddenSettingsKey(target)).toStringList();
    QSet<QString> savedHidden(hiddenList.cbegin(), hiddenList.cend());
    return resolveOrder(defaults, savedOrder, savedHidden);
}

void saveLayout(Target target,
                const QStringList &order,
                const QSet<QString> &hidden,
                QSettings *settings)
{
    QSettings fallback(QString::fromLatin1(kSettingsOrg), QString::fromLatin1(kSettingsApp));
    QSettings &s = settings ? *settings : fallback;
    const QVector<ActionDef> defaults = defaultActionsFor(target);
    const QVector<ResolvedItem> resolved = resolveOrder(defaults, order, hidden);

    QStringList cleanOrder;
    QStringList cleanHidden;
    for (const auto &item : resolved) {
        cleanOrder.append(item.id);
        if (!item.visible)
            cleanHidden.append(item.id);
    }
    s.setValue(orderSettingsKey(target), cleanOrder);
    s.setValue(hiddenSettingsKey(target), cleanHidden);
}

void resetLayout(Target target, QSettings *settings)
{
    QSettings fallback(QString::fromLatin1(kSettingsOrg), QString::fromLatin1(kSettingsApp));
    QSettings &s = settings ? *settings : fallback;
    s.remove(orderSettingsKey(target));
    s.remove(hiddenSettingsKey(target));
}

PreviewHit hitTestPreview(const Timeline *timeline,
                          double playheadSec,
                          const QRectF &letterboxRect,
                          const QPointF &widgetPos)
{
    PreviewHit result;
    if (!timeline || letterboxRect.width() <= 1.0 || letterboxRect.height() <= 1.0)
        return result;

    // Priority 1: Active visible text overlays (topmost / highest index first).
    const QVector<EnhancedTextOverlay> overlays = timeline->timelineTextOverlays();
    for (int i = overlays.size() - 1; i >= 0; --i) {
        const EnhancedTextOverlay &ov = overlays.at(i);
        if (!ov.visible || ov.text.isEmpty())
            continue;
        if (playheadSec < ov.startTime - 1e-6)
            continue;
        if (ov.endTime > 0.0 && playheadSec > ov.endTime + 1e-6)
            continue;

        double bw = ov.width;
        double bh = ov.height;
        if (bw <= 0.0 || bh <= 0.0) {
            const double estW = qBound(0.20, 0.045 * static_cast<double>(ov.text.size()), 0.80);
            const double pt = ov.font.pointSize() > 0 ? static_cast<double>(ov.font.pointSize()) : 32.0;
            const double estH = qBound(0.08, pt / 280.0, 0.35);
            if (bw <= 0.0) bw = estW;
            if (bh <= 0.0) bh = estH;
        }
        const QRectF r(letterboxRect.x() + (ov.x - bw / 2.0) * letterboxRect.width(),
                       letterboxRect.y() + (ov.y - bh / 2.0) * letterboxRect.height(),
                       bw * letterboxRect.width(),
                       bh * letterboxRect.height());
        if (r.contains(widgetPos)) {
            result.target = Target::PreviewText;
            result.overlayIndex = i;
            return result;
        }
    }

    // Priority 2: Active visible video clips inside letterboxRect.
    if (!letterboxRect.contains(widgetPos))
        return result;

    struct VideoHitCandidate {
        int trackIdx = -1;
        int clipIdx = -1;
        bool isTransformedPip = false;
        double area = 1.0;
    };
    QVector<VideoHitCandidate> hits;
    const QVector<TimelineTrack *> tracks = timeline->videoTracks();
    for (int t = 0; t < tracks.size(); ++t) {
        const TimelineTrack *track = tracks.at(t);
        if (!track || track->isHidden())
            continue;
        const QVector<ClipInfo> clips = track->clips();
        double cursor = 0.0;
        for (int c = 0; c < clips.size(); ++c) {
            const ClipInfo &clip = clips.at(c);
            const double start = cursor + qMax(0.0, clip.leadInSec);
            const double end = start + clip.effectiveDuration();
            cursor = end;
            if (!clip.visible || clip.isAdjustment)
                continue;
            if (playheadSec < start - 1e-6 || playheadSec >= end - 1e-6)
                continue;

            const double scale = clip.videoScale > 0.0 ? clip.videoScale : 1.0;
            const double cx = letterboxRect.x() + (0.5 + clip.videoDx) * letterboxRect.width();
            const double cy = letterboxRect.y() + (0.5 + clip.videoDy) * letterboxRect.height();
            const double hw = 0.5 * letterboxRect.width() * scale;
            const double hh = 0.5 * letterboxRect.height() * scale;

            double dx = widgetPos.x() - cx;
            double dy = widgetPos.y() - cy;
            if (std::abs(clip.rotation2DDegrees) > 1e-6) {
                const double rad = -clip.rotation2DDegrees * 3.14159265358979323846 / 180.0;
                const double cs = std::cos(rad);
                const double sn = std::sin(rad);
                const double rx = dx * cs - dy * sn;
                const double ry = dx * sn + dy * cs;
                dx = rx;
                dy = ry;
            }
            if (std::abs(dx) <= hw && std::abs(dy) <= hh) {
                VideoHitCandidate cand;
                cand.trackIdx = t;
                cand.clipIdx = c;
                cand.isTransformedPip = (scale < 0.999)
                    || std::abs(clip.videoDx) > 1e-4
                    || std::abs(clip.videoDy) > 1e-4;
                cand.area = scale * scale;
                hits.append(cand);
            }
        }
    }

    if (!hits.isEmpty()) {
        // Prefer transformed PiP / inset clips (smallest area, then higher track),
        // otherwise V1 (track 0) matching V1-wins stacking, otherwise highest V2+ track.
        std::sort(hits.begin(), hits.end(), [](const VideoHitCandidate &a, const VideoHitCandidate &b) {
            if (a.isTransformedPip != b.isTransformedPip)
                return a.isTransformedPip;
            if (a.isTransformedPip) {
                if (std::abs(a.area - b.area) > 1e-6)
                    return a.area < b.area;
                return a.trackIdx > b.trackIdx;
            }
            if ((a.trackIdx == 0) != (b.trackIdx == 0))
                return a.trackIdx == 0;
            return a.trackIdx > b.trackIdx;
        });
        result.target = Target::PreviewVideoClip;
        result.videoTrackIndex = hits.first().trackIdx;
        result.videoClipIndex = hits.first().clipIdx;
    }

    return result;
}

bool removeTextOverlay(Timeline *timeline, int overlayIndex)
{
    if (!timeline)
        return false;
    const QVector<TimelineTrack *> vTracks = timeline->videoTracks();
    if (vTracks.isEmpty() || !vTracks.first())
        return false;
    TimelineTrack *track = vTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;

    const int ordinaryCount = clips[0].textManager.count();
    QVector<EnhancedTextOverlay> gen = timeline->generatedCaptionOverlays();
    if (overlayIndex < 0 || overlayIndex >= ordinaryCount + gen.size())
        return false;

    if (overlayIndex < ordinaryCount) {
        clips[0].textManager.removeOverlay(overlayIndex);
        track->setClips(clips);
    } else {
        gen.removeAt(overlayIndex - ordinaryCount);
        timeline->restoreGeneratedCaptionOverlays(gen);
    }
    timeline->saveUndoState(QStringLiteral("テキストを削除"));
    return true;
}

int duplicateTextOverlay(Timeline *timeline, int overlayIndex)
{
    if (!timeline)
        return -1;
    const QVector<EnhancedTextOverlay> all = timeline->timelineTextOverlays();
    if (overlayIndex < 0 || overlayIndex >= all.size())
        return -1;

    EnhancedTextOverlay copy = all.at(overlayIndex);
    copy.templateName.clear(); // copied overlay becomes an editable ordinary overlay
    copy.y = qMin(0.92, copy.y + 0.06);
    if (!timeline->addTextOverlayToFirstVideoClip(copy))
        return -1;

    const QVector<TimelineTrack *> vTracks = timeline->videoTracks();
    if (vTracks.isEmpty() || !vTracks.first() || vTracks.first()->clips().isEmpty())
        return -1;
    return vTracks.first()->clips().first().textManager.count() - 1;
}

bool updateTextOverlayColor(Timeline *timeline, int overlayIndex, const QColor &color)
{
    if (!timeline || !color.isValid())
        return false;
    const QVector<TimelineTrack *> vTracks = timeline->videoTracks();
    if (vTracks.isEmpty() || !vTracks.first())
        return false;
    TimelineTrack *track = vTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;

    const int ordinaryCount = clips[0].textManager.count();
    QVector<EnhancedTextOverlay> gen = timeline->generatedCaptionOverlays();
    if (overlayIndex < 0 || overlayIndex >= ordinaryCount + gen.size())
        return false;

    if (overlayIndex < ordinaryCount) {
        if (clips[0].textManager.overlay(overlayIndex).color == color)
            return false;
        clips[0].textManager.overlay(overlayIndex).color = color;
        track->setClips(clips);
    } else {
        const int gIdx = overlayIndex - ordinaryCount;
        if (gen[gIdx].color == color)
            return false;
        gen[gIdx].color = color;
        timeline->restoreGeneratedCaptionOverlays(gen);
    }
    timeline->saveUndoState(QStringLiteral("テキストの色を変更"));
    return true;
}

bool updateTextOverlayFontSize(Timeline *timeline, int overlayIndex, int pointSize)
{
    if (!timeline)
        return false;
    const int bounded = qBound(6, pointSize, 256);
    const QVector<TimelineTrack *> vTracks = timeline->videoTracks();
    if (vTracks.isEmpty() || !vTracks.first())
        return false;
    TimelineTrack *track = vTracks.first();
    QVector<ClipInfo> clips = track->clips();
    if (clips.isEmpty())
        return false;

    const int ordinaryCount = clips[0].textManager.count();
    QVector<EnhancedTextOverlay> gen = timeline->generatedCaptionOverlays();
    if (overlayIndex < 0 || overlayIndex >= ordinaryCount + gen.size())
        return false;

    if (overlayIndex < ordinaryCount) {
        QFont f = clips[0].textManager.overlay(overlayIndex).font;
        if (f.pointSize() == bounded)
            return false;
        f.setPointSize(bounded);
        clips[0].textManager.overlay(overlayIndex).font = f;
        track->setClips(clips);
    } else {
        const int gIdx = overlayIndex - ordinaryCount;
        QFont f = gen[gIdx].font;
        if (f.pointSize() == bounded)
            return false;
        f.setPointSize(bounded);
        gen[gIdx].font = f;
        timeline->restoreGeneratedCaptionOverlays(gen);
    }
    timeline->saveUndoState(QStringLiteral("テキストのサイズを変更"));
    return true;
}

bool centerTextOverlay(Timeline *timeline, int overlayIndex)
{
    if (!timeline)
        return false;
    const QVector<EnhancedTextOverlay> all = timeline->timelineTextOverlays();
    if (overlayIndex < 0 || overlayIndex >= all.size())
        return false;
    const EnhancedTextOverlay &ov = all.at(overlayIndex);
    return timeline->updateTextOverlayRect(overlayIndex, 0.5, 0.5, ov.width, ov.height);
}

bool insertAudioClipAtTime(Timeline *timeline,
                           const QString &audioPath,
                           double startSec,
                           int trackIdxOneBased)
{
    if (!timeline || audioPath.isEmpty())
        return false;
    const double targetSec = qMax(0.0, startSec);
    double duration = probeAudioDurationSec(audioPath);
    if (duration <= 0.0)
        return false;

    int targetIdx = qMax(0, trackIdxOneBased - 1);
    while (timeline->audioTracks().size() <= targetIdx)
        timeline->addAudioTrack(false);

    // Find a non-overlapping audio track starting from targetIdx; create a new track if occupied.
    TimelineTrack *chosenTrack = nullptr;
    for (int t = targetIdx; t < timeline->audioTracks().size(); ++t) {
        TimelineTrack *cand = timeline->audioTracks().at(t);
        if (!cand || cand->isLocked())
            continue;
        bool overlaps = false;
        double cursor = 0.0;
        for (const ClipInfo &c : cand->clips()) {
            const double cs = cursor + qMax(0.0, c.leadInSec);
            const double ce = cs + c.effectiveDuration();
            cursor = ce;
            if (targetSec < ce - 1e-6 && (targetSec + duration) > cs + 1e-6) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            chosenTrack = cand;
            break;
        }
    }
    if (!chosenTrack) {
        timeline->addAudioTrack(false);
        chosenTrack = timeline->audioTracks().last();
    }
    if (!chosenTrack)
        return false;

    ClipInfo clip;
    clip.filePath = audioPath;
    clip.displayName = QFileInfo(audioPath).fileName();
    clip.duration = duration;
    clip.outPoint = duration;
    clip.linkGroup = timeline->allocateLinkGroup();

    const QVector<ClipInfo> clips = chosenTrack->clips();
    double cursor = 0.0;
    int insertIdx = clips.size();
    double leadIn = targetSec;
    for (int i = 0; i < clips.size(); ++i) {
        const double cs = cursor + qMax(0.0, clips[i].leadInSec);
        const double ce = cs + clips[i].effectiveDuration();
        if (targetSec < cs - 1e-6) {
            insertIdx = i;
            leadIn = qMax(0.0, targetSec - cursor);
            break;
        }
        cursor = ce;
        leadIn = qMax(0.0, targetSec - cursor);
    }

    if (insertIdx < clips.size()) {
        chosenTrack->insertClipPreservingDownstream(insertIdx, clip, leadIn);
    } else {
        clip.leadInSec = leadIn;
        chosenTrack->addClip(clip);
    }
    timeline->saveUndoState(QStringLiteral("効果音を追加"));
    return true;
}

ContextMenuCustomizeDialog::ContextMenuCustomizeDialog(Target initialTarget,
                                                       QWidget *parent,
                                                       QSettings *settings)
    : QDialog(parent)
    , m_settings(settings)
    , m_activeTarget(initialTarget)
{
    setWindowTitle(QStringLiteral("右クリックメニューのカスタマイズ"));
    setMinimumSize(420, 440);

    auto *root = new QVBoxLayout(this);

    auto *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel(QStringLiteral("対象:"), this));
    m_targetCombo = new QComboBox(this);
    m_targetCombo->setObjectName(QStringLiteral("ctxMenuTargetCombo"));
    for (Target t : allTargets())
        m_targetCombo->addItem(targetDisplayName(t), static_cast<int>(t));
    const int initIdx = m_targetCombo->findData(static_cast<int>(initialTarget));
    if (initIdx >= 0)
        m_targetCombo->setCurrentIndex(initIdx);
    topRow->addWidget(m_targetCombo, 1);
    root->addLayout(topRow);

    auto *hint = new QLabel(
        QStringLiteral("ドラッグまたは「上へ / 下へ」で項目の順序を並べ替え、チェックで表示/非表示を切り替えられます。"),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #aaa; font-size: 11px;"));
    root->addWidget(hint);

    auto *bodyRow = new QHBoxLayout();
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("ctxMenuItemList"));
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    bodyRow->addWidget(m_list, 1);

    auto *btnCol = new QVBoxLayout();
    m_upBtn = new QPushButton(QStringLiteral("上へ"), this);
    m_upBtn->setObjectName(QStringLiteral("ctxMenuMoveUpBtn"));
    m_downBtn = new QPushButton(QStringLiteral("下へ"), this);
    m_downBtn->setObjectName(QStringLiteral("ctxMenuMoveDownBtn"));
    m_resetBtn = new QPushButton(QStringLiteral("既定に戻す"), this);
    m_resetBtn->setObjectName(QStringLiteral("ctxMenuResetBtn"));
    btnCol->addWidget(m_upBtn);
    btnCol->addWidget(m_downBtn);
    btnCol->addSpacing(12);
    btnCol->addWidget(m_resetBtn);
    btnCol->addStretch();
    bodyRow->addLayout(btnCol);
    root->addLayout(bodyRow, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    loadTargetIntoList(m_activeTarget);

    connect(m_targetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index < 0)
            return;
        captureCurrentListToDraft();
        m_activeTarget = static_cast<Target>(m_targetCombo->itemData(index).toInt());
        loadTargetIntoList(m_activeTarget);
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) { updateMoveButtons(); });
    connect(m_upBtn, &QPushButton::clicked, this, [this]() { moveSelectedRow(-1); });
    connect(m_downBtn, &QPushButton::clicked, this, [this]() { moveSelectedRow(1); });
    connect(m_resetBtn, &QPushButton::clicked, this, [this]() { resetCurrentTargetToDefault(); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        commitDrafts();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

Target ContextMenuCustomizeDialog::currentTarget() const
{
    return m_activeTarget;
}

QStringList ContextMenuCustomizeDialog::currentOrderIds() const
{
    QStringList ids;
    if (!m_list)
        return ids;
    ids.reserve(m_list->count());
    for (int i = 0; i < m_list->count(); ++i)
        ids.append(m_list->item(i)->data(Qt::UserRole).toString());
    return ids;
}

QSet<QString> ContextMenuCustomizeDialog::currentHiddenIds() const
{
    QSet<QString> hidden;
    if (!m_list)
        return hidden;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->checkState() != Qt::Checked)
            hidden.insert(m_list->item(i)->data(Qt::UserRole).toString());
    }
    return hidden;
}

void ContextMenuCustomizeDialog::moveSelectedRow(int delta)
{
    if (!m_list)
        return;
    const int row = m_list->currentRow();
    const int dst = row + delta;
    if (row < 0 || dst < 0 || dst >= m_list->count() || dst == row)
        return;
    QListWidgetItem *item = m_list->takeItem(row);
    m_list->insertItem(dst, item);
    m_list->setCurrentRow(dst);
    updateMoveButtons();
}

void ContextMenuCustomizeDialog::resetCurrentTargetToDefault()
{
    m_drafts.remove(static_cast<int>(m_activeTarget));
    const QVector<ActionDef> defaults = defaultActionsFor(m_activeTarget);
    SavedLayout defLayout;
    for (const auto &d : defaults) {
        defLayout.order.append(d.id);
        if (!d.defaultVisible)
            defLayout.hidden.insert(d.id);
    }
    m_drafts.insert(static_cast<int>(m_activeTarget), defLayout);
    loadTargetIntoList(m_activeTarget);
}

void ContextMenuCustomizeDialog::commitDrafts()
{
    captureCurrentListToDraft();
    for (auto it = m_drafts.cbegin(); it != m_drafts.cend(); ++it) {
        const Target t = static_cast<Target>(it.key());
        saveLayout(t, it.value().order, it.value().hidden, m_settings);
    }
}

void ContextMenuCustomizeDialog::captureCurrentListToDraft()
{
    if (!m_list || m_list->count() == 0)
        return;
    SavedLayout draft;
    draft.order = currentOrderIds();
    draft.hidden = currentHiddenIds();
    m_drafts.insert(static_cast<int>(m_activeTarget), draft);
}

void ContextMenuCustomizeDialog::loadTargetIntoList(Target target)
{
    m_list->clear();
    QVector<ResolvedItem> items;
    const int key = static_cast<int>(target);
    if (m_drafts.contains(key)) {
        const SavedLayout &draft = m_drafts[key];
        items = resolveOrder(defaultActionsFor(target), draft.order, draft.hidden);
    } else {
        items = loadLayout(target, m_settings);
    }
    for (const auto &it : items) {
        auto *row = new QListWidgetItem(it.label, m_list);
        row->setData(Qt::UserRole, it.id);
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        row->setCheckState(it.visible ? Qt::Checked : Qt::Unchecked);
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    updateMoveButtons();
}

void ContextMenuCustomizeDialog::updateMoveButtons()
{
    if (!m_list || !m_upBtn || !m_downBtn)
        return;
    const int row = m_list->currentRow();
    m_upBtn->setEnabled(row > 0);
    m_downBtn->setEnabled(row >= 0 && row + 1 < m_list->count());
}

} // namespace ctxmenu
