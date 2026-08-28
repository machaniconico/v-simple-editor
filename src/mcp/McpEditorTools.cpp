#include "McpEditorTools.h"

#include "McpToolRegistry.h"
#include "../CaptionEditorDialog.h"
#include "../MainWindow.h"
#include "../RenderQueue.h"
#include "../TimelineFrameRenderer.h"
#include "../Timeline.h"
#include "../TrimOps.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"

#include <QAction>
#include <QBuffer>
#include <QColor>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

#include <cmath>
#include <limits>

// Exporter.cpp: GUI の exportVideo と同じラウドネス正規化ゲインを export_video にも適用する。
double exporter_loudnessGainDb();

namespace mcp {

namespace {

QJsonObject objectSchema(const QJsonObject& properties = {})
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("additionalProperties"), false}
    };
}

// QJsonObject には QHash のような unite() が無いので、共通のクリップ指定
// プロパティにツール固有のプロパティを重ねる小さなヘルパを用意する。
// 同じキーがあれば extra 側が勝つ。
QJsonObject mergedProperties(const QJsonObject& base, const QJsonObject& extra)
{
    QJsonObject merged = base;
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        merged.insert(it.key(), it.value());
    return merged;
}

QJsonObject schemaWithRequired(const QJsonObject& properties,
                               const QStringList& required)
{
    QJsonObject schema = objectSchema(properties);
    QJsonArray requiredArray;
    for (const QString& name : required)
        requiredArray.append(name);
    if (!requiredArray.isEmpty())
        schema.insert(QStringLiteral("required"), requiredArray);
    return schema;
}

// additionalProperties は付けない (応答キーが条件で増減するツールがある)。
QJsonObject outputSchemaOf(const QJsonObject& properties,
                           const QStringList& required)
{
    QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties}
    };
    QJsonArray requiredArray;
    for (const QString& name : required)
        requiredArray.append(name);
    if (!requiredArray.isEmpty())
        schema.insert(QStringLiteral("required"), requiredArray);
    return schema;
}

const QStringList& transitionTypeNames()
{
    static const QStringList names{
        QStringLiteral("None"),
        QStringLiteral("FadeIn"),
        QStringLiteral("FadeOut"),
        QStringLiteral("CrossDissolve"),
        QStringLiteral("WipeLeft"),
        QStringLiteral("WipeRight"),
        QStringLiteral("WipeUp"),
        QStringLiteral("WipeDown"),
        QStringLiteral("SlideLeft"),
        QStringLiteral("SlideRight"),
        QStringLiteral("SlideUp"),
        QStringLiteral("SlideDown"),
        QStringLiteral("DipToBlack"),
        QStringLiteral("DipToWhite"),
        QStringLiteral("IrisRound"),
        QStringLiteral("IrisBox"),
        QStringLiteral("ClockWipe"),
        QStringLiteral("BarnDoorHorizontal"),
        QStringLiteral("BarnDoorVertical"),
        QStringLiteral("PushLeft"),
        QStringLiteral("PushRight"),
        QStringLiteral("PushUp"),
        QStringLiteral("PushDown"),
        QStringLiteral("CrossZoom"),
        QStringLiteral("FilmDissolve"),
        QStringLiteral("SpinCW"),
        QStringLiteral("SpinCCW"),
        QStringLiteral("DitherDissolve"),
        QStringLiteral("IrisRoundClose"),
        QStringLiteral("IrisBoxClose"),
        QStringLiteral("BarnDoorHClose"),
        QStringLiteral("BarnDoorVClose"),
        QStringLiteral("ClockWipeCCW"),
        QStringLiteral("WhipPanLeft"),
        QStringLiteral("WhipPanRight"),
        QStringLiteral("Glitch"),
        QStringLiteral("LightLeak"),
        QStringLiteral("FlipHorizontal"),
        QStringLiteral("FlipVertical"),
        QStringLiteral("LensFlare"),
        QStringLiteral("FilmBurn"),
        QStringLiteral("Pixelate"),
        QStringLiteral("BlurDissolve"),
        QStringLiteral("CameraShake"),
        QStringLiteral("ColorChannelShift")
    };
    return names;
}

QJsonArray transitionTypeEnum()
{
    QJsonArray result;
    for (const QString& name : transitionTypeNames())
        result.append(name);
    return result;
}

bool transitionTypeFromName(const QString& name, TransitionType* out)
{
    const int index = transitionTypeNames().indexOf(name);
    if (index < 0)
        return false;
    if (out)
        *out = static_cast<TransitionType>(index);
    return true;
}

QJsonObject transitionToJson(const Transition& transition)
{
    return QJsonObject{
        {QStringLiteral("type"), transitionTypeNames().at(static_cast<int>(transition.type))},
        {QStringLiteral("durationSec"),
         transition.type == TransitionType::None ? 0.0 : transition.duration}
    };
}

QJsonObject transitionOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("type"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("type"), QStringLiteral("durationSec")});
}

ToolDescriptor withOutputSchema(ToolDescriptor tool,
                                const QJsonObject& outputSchema)
{
    tool.outputSchema = outputSchema;
    return tool;
}

QJsonObject clipOutputItemSchema()
{
    const QJsonObject properties{
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("displayName"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("filePath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("inPointSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("outPointSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("speed"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("volume"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("opacity"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("linkGroup"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("selected"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("leadIn"), transitionOutputItemSchema()},
        {QStringLiteral("trailOut"), transitionOutputItemSchema()},
        {QStringLiteral("textOverlayCount"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0}
        }}
    };
    return outputSchemaOf(properties, {
        QStringLiteral("index"), QStringLiteral("displayName"),
        QStringLiteral("filePath"), QStringLiteral("startSec"),
        QStringLiteral("durationSec"), QStringLiteral("inPointSec"),
        QStringLiteral("outPointSec"), QStringLiteral("speed"),
        QStringLiteral("volume"), QStringLiteral("opacity"),
        QStringLiteral("linkGroup"), QStringLiteral("selected"),
        QStringLiteral("leadIn"), QStringLiteral("trailOut"),
        QStringLiteral("textOverlayCount")
    });
}

QJsonObject trackOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clips"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), clipOutputItemSchema()}
        }}
    }, {QStringLiteral("index"), QStringLiteral("clips")});
}

QJsonObject captionOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("endSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("text"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("index"), QStringLiteral("startSec"),
        QStringLiteral("endSec"), QStringLiteral("text")});
}

QJsonObject commandOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("label"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("menuPath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("risk"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("enabled"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("id"), QStringLiteral("label"),
        QStringLiteral("menuPath"), QStringLiteral("risk"),
        QStringLiteral("enabled")});
}

QJsonObject importedClipOutputItemSchema()
{
    return outputSchemaOf(QJsonObject{
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("kind"), QStringLiteral("trackIndex"),
        QStringLiteral("clipIndex"), QStringLiteral("startSec"),
        QStringLiteral("durationSec")});
}

QJsonObject clipSelectorProperties()
{
    return QJsonObject{
        {QStringLiteral("kind"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{
                QStringLiteral("video"), QStringLiteral("audio")
            }},
            {QStringLiteral("default"), QStringLiteral("video")},
            {QStringLiteral("description"),
             QStringLiteral("video または audio。省略時は video")}
        }},
        {QStringLiteral("trackIndex"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0},
            {QStringLiteral("default"), 0},
            {QStringLiteral("description"),
             QStringLiteral("0-based のトラック番号。省略時は 0")}
        }},
        {QStringLiteral("clipIndex"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0},
            {QStringLiteral("description"),
             QStringLiteral("トラック内の 0-based のクリップ番号。get_timeline の index に対応する。必須")}
        }}
    };
}

bool setError(QString* err, const QString& message)
{
    if (err)
        *err = message;
    return false;
}

bool rejectUnknownArguments(const QJsonObject& args, const QStringList& allowed,
                            QString* err)
{
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        if (!allowed.contains(it.key()))
            return setError(err, QStringLiteral("unknown argument: %1").arg(it.key()));
    }
    return true;
}

bool requiredFiniteNumber(const QJsonObject& args, const QString& name,
                          double* out, QString* err)
{
    const QJsonValue value = args.value(name);
    if (!value.isDouble())
        return setError(err, QStringLiteral("%1 must be a finite number").arg(name));
    const double number = value.toDouble();
    if (!std::isfinite(number))
        return setError(err, QStringLiteral("%1 must be a finite number").arg(name));
    if (out)
        *out = number;
    return true;
}

bool finiteNumberForMcp(const QJsonObject& args, const QString& name,
                        double* out, QString* err)
{
    const QJsonValue value = args.value(name);
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        return setError(err, QStringLiteral("%1 は有限な数で指定してください").arg(name));
    if (out)
        *out = value.toDouble();
    return true;
}

bool nonNegativeInteger(const QJsonObject& args, const QString& name,
                        int defaultValue, int* out, QString* err)
{
    if (!args.contains(name)) {
        if (out)
            *out = defaultValue;
        return true;
    }

    const QJsonValue value = args.value(name);
    if (!value.isDouble())
        return setError(err, QStringLiteral("%1 must be a non-negative integer").arg(name));
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0
        || std::floor(number) != number
        || number > static_cast<double>(std::numeric_limits<int>::max())) {
        return setError(err, QStringLiteral("%1 must be a non-negative integer").arg(name));
    }
    if (out)
        *out = static_cast<int>(number);
    return true;
}

bool positiveInteger(const QJsonObject& args, const QString& name,
                     int defaultValue, int* out, QString* err)
{
    if (!args.contains(name)) {
        if (out)
            *out = defaultValue;
        return true;
    }

    const QJsonValue value = args.value(name);
    if (!value.isDouble())
        return setError(err, QStringLiteral("%1 は正の整数で指定してください").arg(name));
    const double number = value.toDouble();
    if (!std::isfinite(number) || number <= 0.0
        || std::floor(number) != number
        || number > static_cast<double>(std::numeric_limits<int>::max())) {
        return setError(err, QStringLiteral("%1 は正の整数で指定してください").arg(name));
    }
    if (out)
        *out = static_cast<int>(number);
    return true;
}

bool positiveFiniteNumber(const QJsonObject& args, const QString& name,
                          double defaultValue, double* out, QString* err)
{
    if (!args.contains(name)) {
        if (out)
            *out = defaultValue;
        return true;
    }
    double number = 0.0;
    if (!finiteNumberForMcp(args, name, &number, err))
        return false;
    if (number <= 0.0)
        return setError(err, QStringLiteral("%1 は 0 より大きい数で指定してください").arg(name));
    if (out)
        *out = number;
    return true;
}

bool encodePng(const QImage& image, QByteArray* encoded)
{
    if (!encoded || image.isNull())
        return false;
    encoded->clear();
    QBuffer buffer(encoded);
    if (!buffer.open(QIODevice::WriteOnly))
        return false;
    return image.save(&buffer, "PNG");
}

constexpr int kDefaultFrameMaxWidth = 640;
constexpr qsizetype kMaxFrameResponseBytes = 1024 * 1024;
// JSON のキー・MCP envelope 分を予約し、base64 本体をこの上限内に収める。
constexpr qsizetype kFrameResponseOverhead = 4096;

struct ClipTarget {
    TimelineTrack* track = nullptr;
    // 解決済みの宛先。Timeline の *ByIndex API にそのまま渡すためのもので、
    // 呼び出し側が args を読み直さないようにする (既定値の解釈が二箇所に散らない)。
    bool audio = false;
    int trackIndex = 0;
    int clipIndex = -1;
    double startSec = 0.0;
    double endSec = 0.0;
};

bool readClipTarget(const QJsonObject& args, MainWindow* window,
                    Timeline* currentTimeline, ClipTarget* out, QString* err)
{
    QString kind = QStringLiteral("video");
    if (args.contains(QStringLiteral("kind"))) {
        const QJsonValue value = args.value(QStringLiteral("kind"));
        if (!value.isString())
            return setError(err, QStringLiteral("kind must be video or audio"));
        kind = value.toString();
    }
    if (kind != QStringLiteral("video") && kind != QStringLiteral("audio"))
        return setError(err, QStringLiteral("kind must be video or audio"));

    int trackIndex = 0;
    if (!nonNegativeInteger(args, QStringLiteral("trackIndex"), 0,
                            &trackIndex, err)) {
        return false;
    }
    if (out) {
        out->audio = kind == QStringLiteral("audio");
        out->trackIndex = trackIndex;
    }

    int clipIndex = -1;
    if (!args.contains(QStringLiteral("clipIndex")))
        return setError(err, QStringLiteral("clipIndex is required"));
    if (!nonNegativeInteger(args, QStringLiteral("clipIndex"), -1,
                            &clipIndex, err)) {
        return false;
    }

    if (!window || !currentTimeline)
        return setError(err, QStringLiteral("editor not available"));

    TimelineTrack* track = currentTimeline->trackAt(
        kind == QStringLiteral("audio"), trackIndex);
    if (!track) {
        const int trackCount = kind == QStringLiteral("audio")
            ? currentTimeline->audioTracks().size()
            : currentTimeline->videoTracks().size();
        return setError(err, QStringLiteral("track index is out of range (%1 トラックは %2 本: 0..%3)")
                                 .arg(kind).arg(trackCount).arg(trackCount - 1));
    }
    if (clipIndex >= track->clipCount()) {
        return setError(err, QStringLiteral("clip index is out of range (%1 トラック %2 のクリップは %3 個: 0..%4)")
                                 .arg(kind).arg(trackIndex).arg(track->clipCount())
                                 .arg(track->clipCount() - 1));
    }

    double cursor = 0.0;
    const QVector<ClipInfo>& clips = track->clips();
    for (int index = 0; index < clips.size(); ++index) {
        const ClipInfo& clip = clips.at(index);
        const double start = cursor + clip.leadInSec;
        const double end = start + clip.effectiveDuration();
        if (index == clipIndex) {
            if (!std::isfinite(start) || !std::isfinite(end) || end <= start)
                return setError(err, QStringLiteral("clip has no valid duration"));
            if (out) {
                // audio / trackIndex は上で解決済みなので、位置指定の集成初期化で
                // 丸ごと上書きしない (フィールドを足したときに黙って壊れる)。
                out->track = track;
                out->clipIndex = clipIndex;
                out->startSec = start;
                out->endSec = end;
            }
            return true;
        }
        cursor = end;
    }

    return setError(err, QStringLiteral("clip index is out of range"));
}

bool requiredString(const QJsonObject& args, const QString& name,
                    QString* out, QString* err)
{
    const QJsonValue value = args.value(name);
    if (!value.isString())
        return setError(err, QStringLiteral("%1 is required").arg(name));
    if (out)
        *out = value.toString();
    return true;
}

QString actionRiskToString(FavoritableActionRisk risk)
{
    switch (risk) {
    case FavoritableActionRisk::Safe:
        return QStringLiteral("safe");
    case FavoritableActionRisk::Blocking:
        return QStringLiteral("blocking");
    case FavoritableActionRisk::Quit:
        return QStringLiteral("quit");
    }
    return QStringLiteral("safe");
}

QJsonObject clipToJson(const ClipInfo& clip, int clipIndex, double startSec,
                       bool selected)
{
    const double outPoint = clip.outPoint > 0.0 ? clip.outPoint : clip.duration;
    const double durationSec = clip.speed > 0.0 ? clip.effectiveDuration() : 0.0;
    return QJsonObject{
        {QStringLiteral("index"), clipIndex},
        {QStringLiteral("displayName"), clip.displayName},
        {QStringLiteral("filePath"), clip.filePath},
        {QStringLiteral("startSec"), startSec},
        {QStringLiteral("durationSec"), durationSec},
        {QStringLiteral("inPointSec"), clip.inPoint},
        {QStringLiteral("outPointSec"), outPoint},
        {QStringLiteral("speed"), clip.speed},
        {QStringLiteral("volume"), clip.volume},
        {QStringLiteral("opacity"), clip.opacity},
        {QStringLiteral("linkGroup"), clip.linkGroup},
        {QStringLiteral("selected"), selected},
        {QStringLiteral("leadIn"), transitionToJson(clip.leadIn)},
        {QStringLiteral("trailOut"), transitionToJson(clip.trailOut)},
        {QStringLiteral("textOverlayCount"), clip.textManager.count()}
    };
}

QJsonArray tracksToJson(const QVector<TimelineTrack*>& tracks)
{
    QJsonArray result;
    for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        QJsonArray clips;
        double cursorSec = 0.0;
        const TimelineTrack *trackObject = tracks.at(trackIndex);
        const QVector<ClipInfo> emptyTrack;
        const QVector<ClipInfo>& track = trackObject ? trackObject->clips() : emptyTrack;
        for (int clipIndex = 0; clipIndex < track.size(); ++clipIndex) {
            const ClipInfo& clip = track.at(clipIndex);
            // TimelineSequence::duration() and Timeline's placement logic both
            // treat leadInSec as a gap before the clip, then add effectiveDuration().
            // Therefore cursor + leadInSec is the absolute timeline start.
            cursorSec += clip.leadInSec;
            clips.append(clipToJson(clip, clipIndex, cursorSec,
                                    trackObject && trackObject->isClipSelected(clipIndex)));
            cursorSec += clip.speed > 0.0 ? clip.effectiveDuration() : 0.0;
        }
        result.append(QJsonObject{
            {QStringLiteral("index"), trackIndex},
            {QStringLiteral("clips"), clips}
        });
    }
    return result;
}

} // namespace

McpEditorTools::McpEditorTools(MainWindow* window, McpToolRegistry* registry)
    : m_window(window)
    , m_registry(registry)
{
}

McpEditorTools::~McpEditorTools()
{
    // ハンドラのラムダは this を捕捉しているため、MainWindow が子の
    // RenderQueue を破棄する前に接続を外してダングリング参照を残さない。
    QObject::disconnect(m_exportProgressConnection);
    QObject::disconnect(m_exportCompletedConnection);
    m_observedRenderQueue = nullptr;
}

Timeline* McpEditorTools::timeline() const
{
    return m_window ? m_window->m_timeline : nullptr;
}

bool McpEditorTools::beginExclusiveWrite(const QString& toolName, QString* err)
{
    if (!m_activeWriteTool.isEmpty()) {
        return setError(err,
                        QStringLiteral("別の操作を実行中です (%1)。完了を待ってから再試行してください。")
                            .arg(m_activeWriteTool));
    }
    m_activeWriteTool = toolName;
    return true;
}

void McpEditorTools::endExclusiveWrite()
{
    m_activeWriteTool.clear();
}

namespace {

// export_video はライブの Timeline をレンダースレッドへ渡すので、レンダリング中に
// クリップ構成を変える変更系ツールは拒否する。選択・再生ヘッド・保存・字幕エディタ
// 側の一覧編集はタイムラインのクリップ配列を触らないので通す。
bool toolMutatesTimeline(const QString& toolName)
{
    static const QSet<QString> kNonMutating{
        QStringLiteral("export_video"), QStringLiteral("select_clip"),
        QStringLiteral("clear_selection"), QStringLiteral("set_playhead"),
        QStringLiteral("save_project"), QStringLiteral("add_caption"),
        QStringLiteral("remove_caption"), QStringLiteral("clear_captions")
    };
    return !kNonMutating.contains(toolName);
}

} // namespace

ToolHandler McpEditorTools::guardedWrite(const QString& toolName, ToolHandler inner)
{
    return [this, toolName, inner](const QJsonObject& args, QString* err) -> QJsonObject {
        if (!beginExclusiveWrite(toolName, err))
            return {};
        struct Reset {
            McpEditorTools* tools;
            ~Reset() { tools->endExclusiveWrite(); }
        } reset{this};
        RenderQueue* queue = m_window ? m_window->m_renderQueue : nullptr;
        if (queue && toolMutatesTimeline(toolName)) {
            for (const RenderJob& job : queue->jobs()) {
                if (job.status != RenderJobStatus::Rendering)
                    continue;
                return setError(err,
                                QStringLiteral("書き出し中 (jobId %1) はタイムラインを変更できません。get_export_status で done / failed になるのを待ってから再試行してください。")
                                    .arg(job.uuid)),
                       QJsonObject();
            }
        }
        return inner(args, err);
    };
}

void McpEditorTools::syncSelectionAfterEdit()
{
    Timeline* currentTimeline = timeline();
    if (!m_window || !currentTimeline)
        return;

    bool audioSelection = false;
    int selectedTrack = -1;
    int selectedClip = -1;
    bool invalidSelection = false;

    for (int trackIndex = 0;
         trackIndex < currentTimeline->videoTracks().size(); ++trackIndex) {
        TimelineTrack* track = currentTimeline->videoTracks().at(trackIndex);
        if (!track)
            continue;
        const int clipIndex = track->selectedClip();
        if (clipIndex < 0)
            continue;
        if (clipIndex >= track->clipCount()) {
            invalidSelection = true;
        } else if (selectedTrack < 0) {
            selectedTrack = trackIndex;
            selectedClip = clipIndex;
        }
    }

    for (int trackIndex = 0;
         trackIndex < currentTimeline->audioTracks().size(); ++trackIndex) {
        TimelineTrack* track = currentTimeline->audioTracks().at(trackIndex);
        if (!track)
            continue;
        const int clipIndex = track->selectedClip();
        if (clipIndex < 0)
            continue;
        if (clipIndex >= track->clipCount()) {
            invalidSelection = true;
        } else if (selectedTrack < 0) {
            audioSelection = true;
            selectedTrack = trackIndex;
            selectedClip = clipIndex;
        }
    }

    if (invalidSelection || selectedTrack < 0) {
        currentTimeline->clearSelection();
        m_window->m_selectedVideoTrackIndex = -1;
        m_window->m_selectedVideoClipIndexTracked = -1;
        if (m_window->m_player)
            m_window->m_player->setEditTargetByClip(-1, -1);
    } else {
        QString ignored;
        currentTimeline->selectClipByIndex(audioSelection, selectedTrack,
                                           selectedClip, &ignored);
        // select_clip と同じく、同値選択でシグナルが省略されても追跡値を揃える。
        m_window->m_selectedVideoTrackIndex = audioSelection ? -1 : selectedTrack;
        m_window->m_selectedVideoClipIndexTracked = selectedClip;
        if (m_window->m_player)
            m_window->m_player->setEditTargetByClip(
                audioSelection ? -1 : selectedTrack, selectedClip);
    }
    m_window->updateEditActions();
}

RenderQueue* McpEditorTools::ensureRenderQueue(QString* err)
{
    if (!m_window)
        return setError(err, QStringLiteral("editor not available")), nullptr;

    if (!m_window->m_renderQueue)
        m_window->m_renderQueue = new RenderQueue(m_window);
    observeRenderQueue(m_window->m_renderQueue);
    return m_window->m_renderQueue;
}

void McpEditorTools::observeRenderQueue(RenderQueue* queue)
{
    if (!queue || !m_window || m_observedRenderQueue == queue)
        return;

    QObject::disconnect(m_exportProgressConnection);
    QObject::disconnect(m_exportCompletedConnection);
    m_observedRenderQueue = queue;

    // RenderQueue の進捗シグナルはワーカースレッドから届くことがあるため、
    // MainWindow を context にして GUI スレッドで MCP 用スナップショットを更新する。
    m_exportProgressConnection = QObject::connect(
        queue, &RenderQueue::jobProgressUuid, m_window,
        [this](const QString& jobId, int percent) {
            ExportJobObservation& observation = m_exportJobObservations[jobId];
            // 完了通知より遅れて届く進捗通知で、最終スナップショットを
            // 99% などへ巻き戻さない。
            if (observation.status == QStringLiteral("done")
                || observation.status == QStringLiteral("failed"))
                return;
            observation.status = QStringLiteral("running");
            observation.progress = qBound(0, percent, 100);
        });
    m_exportCompletedConnection = QObject::connect(
        queue, &RenderQueue::jobCompletedUuid, m_window,
        [this](const QString& jobId, bool success, const QString& error) {
            ExportJobObservation& observation = m_exportJobObservations[jobId];
            observation.status = success ? QStringLiteral("done")
                                         : QStringLiteral("failed");
            observation.progress = success ? 100 : qBound(0, observation.progress, 100);
            observation.error = error;
        });
}

QJsonObject McpEditorTools::exportStatus(const QString& jobId, QString* err)
{
    if (!m_window)
        return setError(err, QStringLiteral("editor not available")), QJsonObject();

    RenderQueue* queue = m_window->m_renderQueue;
    if (queue) {
        // status の照会でもシグナル監視を有効にする。MCP 以外から投入された
        // RenderQueue ジョブも、照会開始後は同じ進捗スナップショットで返す。
        observeRenderQueue(queue);

        for (const RenderJob& job : queue->jobs()) {
            if (job.uuid != jobId)
                continue;

            QString status;
            switch (job.status) {
            case RenderJobStatus::Pending:
                status = QStringLiteral("queued");
                break;
            case RenderJobStatus::Rendering:
                status = QStringLiteral("running");
                break;
            case RenderJobStatus::Completed:
                status = QStringLiteral("done");
                break;
            case RenderJobStatus::Failed:
            case RenderJobStatus::Cancelled:
                status = QStringLiteral("failed");
                break;
            }

            int progress = job.status == RenderJobStatus::Completed
                ? 100 : qBound(0, job.progressPercent, 100);
            const auto observation = m_exportJobObservations.constFind(jobId);
            if (observation != m_exportJobObservations.constEnd()) {
                if (status == QStringLiteral("running")
                    || status == QStringLiteral("queued")) {
                    progress = observation->progress;
                } else if (status == QStringLiteral("done")) {
                    progress = 100;
                }
            }

            QJsonObject result{
                {QStringLiteral("ok"), true},
                {QStringLiteral("jobId"), jobId},
                {QStringLiteral("status"), status},
                {QStringLiteral("progress"), progress}
            };
            if (!job.outputPath.isEmpty())
                result.insert(QStringLiteral("outputPath"), job.outputPath);
            if (status == QStringLiteral("failed")) {
                const QString jobError = !job.error.isEmpty()
                    ? job.error : job.errorMessage;
                if (!jobError.isEmpty())
                    result.insert(QStringLiteral("error"), jobError);
                else if (observation != m_exportJobObservations.constEnd()
                         && !observation->error.isEmpty())
                    result.insert(QStringLiteral("error"), observation->error);
                else
                    result.insert(QStringLiteral("error"),
                                  QStringLiteral("書き出しに失敗しました"));
            }
            return result;
        }
    }

    // RenderQueue の clearCompleted() などで完了ジョブ本体が片付けられても、
    // MCP が観測した最終スナップショットは McpEditorTools の存続中保持する。
    // したがって jobId の寿命は通常 MainWindow / プロセス終了までである。
    const auto observation = m_exportJobObservations.constFind(jobId);
    if (observation != m_exportJobObservations.constEnd()) {
        QJsonObject result{
            {QStringLiteral("ok"), true},
            {QStringLiteral("jobId"), jobId},
            {QStringLiteral("status"), observation->status},
            {QStringLiteral("progress"), qBound(0, observation->progress, 100)}
        };
        if (observation->status == QStringLiteral("failed")
            && !observation->error.isEmpty()) {
            result.insert(QStringLiteral("error"), observation->error);
        } else if (observation->status == QStringLiteral("failed")) {
            result.insert(QStringLiteral("error"),
                          QStringLiteral("書き出しに失敗しました"));
        }
        return result;
    }

    return setError(err, QStringLiteral("不明な jobId: %1").arg(jobId)), QJsonObject();
}

void McpEditorTools::registerReadTools()
{
    if (!m_registry)
        return;

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_project_info"),
        QStringLiteral("プロジェクトの設定、尺、再生ヘッド位置、トラック数を秒単位で返す。編集前の現状確認に使う。"),
        objectSchema(),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window) {
                if (err)
                    *err = QStringLiteral("editor not available");
                return {};
            }

            const Timeline* currentTimeline = timeline();
            const QVector<QVector<ClipInfo>> videoTracks = currentTimeline
                ? currentTimeline->allVideoTracks() : QVector<QVector<ClipInfo>>();
            const QVector<QVector<ClipInfo>> audioTracks = currentTimeline
                ? currentTimeline->allAudioTracks() : QVector<QVector<ClipInfo>>();
            // MainWindow owns the project configuration; when its Timeline is
            // absent there is no loaded editor project, so expose empty/zero values.
            const bool projectAvailable = currentTimeline != nullptr;
            return QJsonObject{
                {QStringLiteral("projectName"), projectAvailable
                    ? m_window->m_projectConfig.name : QString()},
                {QStringLiteral("width"), projectAvailable
                    ? m_window->m_projectConfig.width : 0},
                {QStringLiteral("height"), projectAvailable
                    ? m_window->m_projectConfig.height : 0},
                {QStringLiteral("fps"), projectAvailable
                    ? static_cast<double>(m_window->m_projectConfig.fps) : 0.0},
                {QStringLiteral("durationSec"), currentTimeline
                    ? currentTimeline->totalDuration() : 0.0},
                {QStringLiteral("playheadSec"), currentTimeline
                    ? currentTimeline->playheadPosition() : 0.0},
                {QStringLiteral("videoTrackCount"), videoTracks.size()},
                {QStringLiteral("audioTrackCount"), audioTracks.size()},
                {QStringLiteral("hasUnsavedChanges"), m_window->isWindowModified()}
            };
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("projectName"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("width"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("height"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("fps"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("playheadSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("videoTrackCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("audioTrackCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("hasUnsavedChanges"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("projectName"), QStringLiteral("width"),
        QStringLiteral("height"), QStringLiteral("fps"),
        QStringLiteral("durationSec"), QStringLiteral("playheadSec"),
        QStringLiteral("videoTrackCount"), QStringLiteral("audioTrackCount"),
        QStringLiteral("hasUnsavedChanges")})));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_frame"),
        QStringLiteral("指定したタイムライン時刻の合成フレームを PNG 画像として返す。maxWidth 省略時は 640px 以下に縮小し、LLM のコンテキストを圧迫しないよう応答を 1MB 以内に抑える。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }},
            {QStringLiteral("maxWidth"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 1}
            }}
        }, {QStringLiteral("timeSec")}),
        {},
        [this](const QJsonObject& args, QString* err,
               QJsonArray* content) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("timeSec"),
                                         QStringLiteral("maxWidth")}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("エディタまたはタイムラインを利用できません")),
                       QJsonObject();
            if (!content)
                return setError(err, QStringLiteral("画像コンテンツの出力先を利用できません")),
                       QJsonObject();

            double timeSec = 0.0;
            if (!args.contains(QStringLiteral("timeSec"))) {
                if (err)
                    *err = QStringLiteral("timeSec は必須です");
                return {};
            }
            if (!finiteNumberForMcp(args, QStringLiteral("timeSec"),
                                    &timeSec, err))
                return {};

            const double durationSec = qMax(0.0, timeline()->totalDuration());
            // Timeline の終端は次のフレームが存在しない半開区間なので、
            // 有効範囲を [0, duration) として終端も明示的に拒否する。
            if (durationSec <= 0.0 || timeSec < 0.0 || timeSec >= durationSec) {
                return setError(err,
                                QStringLiteral("timeSec がタイムライン範囲外です: %1 (範囲 0-%2 未満)")
                                    .arg(timeSec, 0, 'f', 6)
                                    .arg(durationSec, 0, 'f', 6)),
                       QJsonObject();
            }

            int maxWidth = kDefaultFrameMaxWidth;
            if (!positiveInteger(args, QStringLiteral("maxWidth"),
                                 kDefaultFrameMaxWidth, &maxWidth, err))
                return {};

            const int canvasWidth = qMax(1, m_window->m_projectConfig.width);
            const int canvasHeight = qMax(1, m_window->m_projectConfig.height);
            const int renderWidth = qMax(1, qMin(canvasWidth, maxWidth));
            const int renderHeight = qMax(1, qRound(
                static_cast<double>(canvasHeight) * renderWidth
                / static_cast<double>(canvasWidth)));
            const qint64 usec = qRound64(timeSec * 1'000'000.0);
            QImage image = tlrender::renderFrameAt(
                timeline(), usec, QSize(renderWidth, renderHeight));
            if (image.isNull())
                return setError(err, QStringLiteral("指定時刻のフレームをレンダリングできませんでした")),
                       QJsonObject();
            if (image.format() != QImage::Format_RGBA8888)
                image = image.convertToFormat(QImage::Format_RGBA8888);

            // 指定幅が大きくても、base64 と JSON envelope を含む応答全体が
            // 1MB を超えないよう PNG の再圧縮ではなく画像自体を段階的に縮小する。
            QByteArray png;
            QByteArray base64;
            bool fitsResponseLimit = false;
            for (int attempt = 0; attempt < 16; ++attempt) {
                if (!encodePng(image, &png))
                    return setError(err, QStringLiteral("フレームを PNG にエンコードできませんでした")),
                           QJsonObject();
                base64 = png.toBase64();
                if (base64.size() + kFrameResponseOverhead
                        <= kMaxFrameResponseBytes
                    || image.width() <= 1) {
                    fitsResponseLimit = base64.size() + kFrameResponseOverhead
                        <= kMaxFrameResponseBytes;
                    break;
                }

                const int nextWidth = qMax(1, qMin(image.width() - 1,
                                                   qRound(image.width() * 0.8)));
                const int nextHeight = qMax(1, qRound(
                    static_cast<double>(image.height()) * nextWidth
                    / static_cast<double>(image.width())));
                image = image.scaled(QSize(nextWidth, nextHeight),
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
            // ループ上限で縮小した場合にも、content と payload が同じ PNG を
            // 参照するよう最後の画像を必ず再エンコードして判定する。
            if (!fitsResponseLimit) {
                if (!encodePng(image, &png))
                    return setError(err, QStringLiteral("フレームを PNG にエンコードできませんでした")),
                           QJsonObject();
                base64 = png.toBase64();
                fitsResponseLimit = base64.size() + kFrameResponseOverhead
                    <= kMaxFrameResponseBytes;
            }
            if (!fitsResponseLimit)
                return setError(err, QStringLiteral("PNG 応答を 1MB 以内に縮小できませんでした")),
                       QJsonObject();

            content->append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("image")},
                {QStringLiteral("data"), QString::fromLatin1(base64)},
                {QStringLiteral("mimeType"), QStringLiteral("image/png")}
            });
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("timeSec"), timeSec},
                {QStringLiteral("width"), image.width()},
                {QStringLiteral("height"), image.height()},
                {QStringLiteral("byteSize"), static_cast<qint64>(png.size())},
                {QStringLiteral("base64Bytes"), static_cast<qint64>(base64.size())}
            };
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("timeSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("width"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("height"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("byteSize"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("base64Bytes"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("timeSec"),
        QStringLiteral("width"), QStringLiteral("height"),
        QStringLiteral("byteSize"), QStringLiteral("base64Bytes")})));

    const QJsonObject exportStatusProperties{
        {QStringLiteral("jobId"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")}
        }}
    };
    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_export_status"),
        QStringLiteral("非同期 export_video ジョブの状態と進捗を返す。status は queued / running / done / failed。失敗時は error を含める。"),
        schemaWithRequired(exportStatusProperties, {QStringLiteral("jobId")}),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("jobId")}, err))
                return {};
            QString jobId;
            if (!requiredString(args, QStringLiteral("jobId"), &jobId, err)) {
                if (err)
                    *err = QStringLiteral("jobId は必須です");
                return {};
            }
            if (jobId.trimmed().isEmpty())
                return setError(err, QStringLiteral("jobId は必須です")), QJsonObject();
            return exportStatus(jobId, err);
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("jobId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("status"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("progress"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("outputPath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok"), QStringLiteral("jobId"),
        QStringLiteral("status"), QStringLiteral("progress")})));

    const QJsonObject timelineProperties{
        {QStringLiteral("kind"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{
                QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("all")
            }},
            {QStringLiteral("default"), QStringLiteral("all")},
            {QStringLiteral("description"),
             QStringLiteral("video、audio、all のいずれか。省略時は all")}
        }}
    };
    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_timeline"),
        QStringLiteral("タイムライン上の全クリップをトラック別に秒単位で返す。kind: video / audio / all (省略時は all)。編集前の現状確認に使う。"),
        objectSchema(timelineProperties),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("kind")}, err))
                return {};
            QString kind = QStringLiteral("all");
            if (args.contains(QStringLiteral("kind"))) {
                const QJsonValue value = args.value(QStringLiteral("kind"));
                if (!value.isString()
                    || (value.toString() != QStringLiteral("video")
                        && value.toString() != QStringLiteral("audio")
                        && value.toString() != QStringLiteral("all"))) {
                    return setError(err, QStringLiteral("kind must be video, audio or all")),
                           QJsonObject();
                }
                kind = value.toString();
            }
            if (!m_window) {
                if (err)
                    *err = QStringLiteral("editor not available");
                return {};
            }

            QJsonObject result;
            if (kind == QStringLiteral("video") || kind == QStringLiteral("all"))
                result.insert(QStringLiteral("video"), QJsonArray());
            if (kind == QStringLiteral("audio") || kind == QStringLiteral("all"))
                result.insert(QStringLiteral("audio"), QJsonArray());
            const Timeline* currentTimeline = timeline();
            if (!currentTimeline)
                return result;

            if (result.contains(QStringLiteral("video")))
                result.insert(QStringLiteral("video"),
                              tracksToJson(currentTimeline->videoTracks()));
            if (result.contains(QStringLiteral("audio")))
                result.insert(QStringLiteral("audio"),
                              tracksToJson(currentTimeline->audioTracks()));
            return result;
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("video"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), trackOutputItemSchema()}
        }},
        {QStringLiteral("audio"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), trackOutputItemSchema()}
        }}
    }, {})));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_captions"),
        QStringLiteral("captions は字幕エディタの内容、timelineCaptions はタイムラインに適用済みの1語字幕 (undo で戻るのは後者だけ) を秒単位で返す。"),
        objectSchema(),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window) {
                if (err)
                    *err = QStringLiteral("editor not available");
                return {};
            }

            QJsonArray captions;
            if (m_window->m_captionEditorDialog) {
                const QList<caption::Clip> clips =
                    m_window->m_captionEditorDialog->track().clips();
                for (int index = 0; index < clips.size(); ++index) {
                    const caption::Clip& clip = clips.at(index);
                    captions.append(QJsonObject{
                        {QStringLiteral("index"), index},
                        {QStringLiteral("startSec"),
                         static_cast<double>(clip.startMs) / 1000.0},
                        {QStringLiteral("endSec"),
                         static_cast<double>(clip.endMs) / 1000.0},
                        {QStringLiteral("text"), clip.text}
                    });
                }
            }

            QJsonArray timelineCaptions;
            const Timeline* currentTimeline = timeline();
            if (currentTimeline) {
                const QVector<EnhancedTextOverlay>& overlays =
                    currentTimeline->generatedCaptionOverlays();
                for (int index = 0; index < overlays.size(); ++index) {
                    const EnhancedTextOverlay& overlay = overlays.at(index);
                    timelineCaptions.append(QJsonObject{
                        {QStringLiteral("index"), index},
                        {QStringLiteral("startSec"), overlay.startTime},
                        {QStringLiteral("endSec"), overlay.endTime},
                        {QStringLiteral("text"), overlay.text}
                    });
                }
            }
            return QJsonObject{
                {QStringLiteral("captions"), captions},
                {QStringLiteral("timelineCaptions"), timelineCaptions},
                {QStringLiteral("timelineCaptionCount"), timelineCaptions.size()}
            };
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("captions"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), captionOutputItemSchema()}
        }},
        {QStringLiteral("timelineCaptions"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), captionOutputItemSchema()}
        }},
        {QStringLiteral("timelineCaptionCount"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")}
        }}
    }, {QStringLiteral("captions"), QStringLiteral("timelineCaptions"),
        QStringLiteral("timelineCaptionCount")})));

    const QJsonObject commandProperties{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("description"),
             QStringLiteral("id / 表示名 / メニュー階層に対する大文字小文字を区別しない部分一致フィルタ。省略時は全件")}
        }}
    };
    m_registry->registerTool(withOutputSchema({
        QStringLiteral("list_commands"),
        QStringLiteral("エディタで利用できるお気に入り登録可能なコマンドを列挙する。query を指定すると id / 表示名 / メニュー階層を大文字小文字を区別せず部分一致で絞り込む。query 省略時は全件 (約 230 件、JSON で約 60KB) を返すので、通常は query で絞り込むこと。id、表示名、メニュー階層、危険度 (safe / blocking / quit)、有効状態を返す。blocking のコマンドは run_command で既定では実行を拒否される。"),
        objectSchema(commandProperties),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("query")}, err))
                return {};
            if (!m_window) {
                if (err)
                    *err = QStringLiteral("editor not available");
                return {};
            }

            const QString query = args.value(QStringLiteral("query")).toString();
            QJsonArray commands;
            for (const auto& command : m_window->m_favoritableActions) {
                const bool matches = query.isEmpty()
                    || command.id.contains(query, Qt::CaseInsensitive)
                    || command.label.contains(query, Qt::CaseInsensitive)
                    || command.menuPath.contains(query, Qt::CaseInsensitive);
                if (!matches)
                    continue;
                commands.append(QJsonObject{
                    {QStringLiteral("id"), command.id},
                    {QStringLiteral("label"), command.label},
                    {QStringLiteral("menuPath"), command.menuPath},
                    {QStringLiteral("risk"), actionRiskToString(command.risk)},
                    {QStringLiteral("enabled"),
                     command.action ? command.action->isEnabled() : false}
                });
            }
            return QJsonObject{
                {QStringLiteral("commands"), commands},
                {QStringLiteral("total"), m_window->m_favoritableActions.size()}
            };
        }
    }, outputSchemaOf(QJsonObject{
        {QStringLiteral("commands"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), commandOutputItemSchema()}
        }},
        {QStringLiteral("total"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("description"),
             QStringLiteral("フィルタ前の全コマンド数 (commands の件数ではない)")}
        }}
    }, {QStringLiteral("commands"), QStringLiteral("total")})));
}

void McpEditorTools::registerWriteTools()
{
    if (!m_registry)
        return;

    const QJsonObject clipProperties = clipSelectorProperties();

    const QJsonObject exportVideoOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("jobId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("status"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{QStringLiteral("queued")}}
        }},
        {QStringLiteral("progress"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("outputPath"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("width"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("height"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("fps"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("videoCodec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("videoBitrate"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("audioCodec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("audioBitrate"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("jobId"),
        QStringLiteral("status"), QStringLiteral("progress"),
        QStringLiteral("outputPath"), QStringLiteral("width"),
        QStringLiteral("height"), QStringLiteral("fps"),
        QStringLiteral("videoCodec"), QStringLiteral("videoBitrate")});

    const QJsonObject importMediaOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("clips"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), importedClipOutputItemSchema()}
        }},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("ok"), QStringLiteral("clips")});

    const QJsonObject saveProjectOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok"), QStringLiteral("path")});

    const QJsonObject openProjectOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok"), QStringLiteral("path")});

    const QJsonObject selectClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex")});

    const QJsonObject clearSelectionOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("ok")});

    const QJsonObject runCommandOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("label"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("risk"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("undoRecorded"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("undoDescription"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok"), QStringLiteral("id"),
        QStringLiteral("label"), QStringLiteral("risk"),
        QStringLiteral("undoRecorded")});

    const QJsonObject splitClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("newClipCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("leftIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("rightIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("newClipCount"),
        QStringLiteral("leftIndex"), QStringLiteral("rightIndex")});

    const QJsonObject deleteClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("remainingClipCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("remainingClipCount")});

    const QJsonObject moveClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("actualStartSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("startSec"),
        QStringLiteral("actualStartSec"), QStringLiteral("trackIndex")});

    const QJsonObject setClipPropertyOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("property"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("value"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("linkedApplied"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("ok"), QStringLiteral("property"),
        QStringLiteral("value")});

    const QJsonObject trimClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("edge"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("ripple"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("endSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex"),
        QStringLiteral("edge"), QStringLiteral("ripple"),
        QStringLiteral("startSec"), QStringLiteral("endSec")});

    const QJsonObject setTransitionOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("kind"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("trackIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("clipIndex"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("type"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("durationSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("leadIn"), transitionOutputItemSchema()},
        {QStringLiteral("trailOut"), transitionOutputItemSchema()}
    }, {QStringLiteral("ok"), QStringLiteral("kind"),
        QStringLiteral("trackIndex"), QStringLiteral("clipIndex"),
        QStringLiteral("type"), QStringLiteral("durationSec"),
        QStringLiteral("leadIn"), QStringLiteral("trailOut")});

    const QJsonObject addTextOverlayOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("text"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("startSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("endSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("x"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("y"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("fontSize"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("color"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("clipIndices"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
        }}
    }, {QStringLiteral("ok"), QStringLiteral("index"),
        QStringLiteral("text"), QStringLiteral("startSec"),
        QStringLiteral("endSec"), QStringLiteral("x"),
        QStringLiteral("y"), QStringLiteral("fontSize"),
        QStringLiteral("color")});

    const QJsonObject addCaptionOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("index"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("captionCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("index"),
        QStringLiteral("captionCount")});

    const QJsonObject applyCaptionsOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("appliedCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("captionCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("timelineCaptionCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("appliedCount"),
        QStringLiteral("captionCount"), QStringLiteral("timelineCaptionCount")});

    const QJsonObject setPlayheadOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("playheadSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("playing"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("previewSeekRequested"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    }, {QStringLiteral("ok"), QStringLiteral("playheadSec"),
        QStringLiteral("playing"), QStringLiteral("previewSeekRequested")});

    const QJsonObject undoOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok")});

    const QJsonObject redoOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok")});

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("export_video"),
        QStringLiteral("現在のタイムラインを動画ファイルへ非同期で書き出す。tools/call はジョブ投入後すぐに jobId を返し、完了は get_export_status で確認する。width / height / fps の省略時は現在のプロジェクト設定を使い、videoBitrate / audioBitrate は kbps (既定 10000 / 192)、videoCodec / audioCodec は ffmpeg のエンコーダ名 (既定 libx264 / aac)。音声はトリム・分割・並べ替え・音量・ミュートを反映したタイムラインのミックスを ffmpeg で作ってから多重化する (ffmpeg が PATH に無いと単純な 1 クリップ構成以外は failed になる)。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("outputPath"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("width"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 2}
            }},
            {QStringLiteral("height"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 2}
            }},
            {QStringLiteral("fps"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("exclusiveMinimum"), 0}
            }},
            {QStringLiteral("videoCodec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("videoBitrate"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 1},
                {QStringLiteral("description"), QStringLiteral("kbps")}
            }},
            {QStringLiteral("audioCodec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"), QStringLiteral("ffmpeg の音声エンコーダ名。既定 aac")}
            }},
            {QStringLiteral("audioBitrate"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 1},
                {QStringLiteral("description"), QStringLiteral("kbps。既定 192")}
            }}
        }, {QStringLiteral("outputPath")}),
        guardedWrite(QStringLiteral("export_video"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("outputPath"),
                                         QStringLiteral("width"),
                                         QStringLiteral("height"),
                                         QStringLiteral("fps"),
                                         QStringLiteral("videoCodec"),
                                         QStringLiteral("videoBitrate"),
                                         QStringLiteral("audioCodec"),
                                         QStringLiteral("audioBitrate")}, err))
                return {};

            QString outputPath;
            if (!requiredString(args, QStringLiteral("outputPath"),
                                &outputPath, err)) {
                if (err)
                    *err = QStringLiteral("outputPath は必須です");
                return {};
            }
            outputPath = outputPath.trimmed();
            if (outputPath.isEmpty())
                return setError(err, QStringLiteral("outputPath は必須です")),
                       QJsonObject();

            const QFileInfo outputInfo(outputPath);
            if (!outputInfo.absoluteDir().exists()) {
                return setError(err,
                                QStringLiteral("出力先の親ディレクトリが存在しません: %1")
                                    .arg(outputInfo.absoluteDir().absolutePath())),
                       QJsonObject();
            }
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("エディタまたはタイムラインを利用できません")),
                       QJsonObject();
            if (timeline()->totalDuration() <= 0.0)
                return setError(err, QStringLiteral("タイムラインが空です。import_media で素材を追加してください")),
                       QJsonObject();

            const int defaultWidth = qMax(2, m_window->m_projectConfig.width);
            const int defaultHeight = qMax(2, m_window->m_projectConfig.height);
            const double defaultFps = qMax(1, m_window->m_projectConfig.fps);
            int width = defaultWidth;
            int height = defaultHeight;
            if (!positiveInteger(args, QStringLiteral("width"), defaultWidth,
                                 &width, err)
                || !positiveInteger(args, QStringLiteral("height"), defaultHeight,
                                    &height, err))
                return {};
            if (width < 2 || height < 2)
                return setError(err, QStringLiteral("width と height は 2 以上で指定してください")),
                       QJsonObject();

            double fps = defaultFps;
            if (!positiveFiniteNumber(args, QStringLiteral("fps"), defaultFps,
                                      &fps, err))
                return {};

            // ProjectConfig が保持する書き出し設定は現状サイズと fps までなので、
            // codec / bitrate は ExportConfig と同じ既定値を使う。
            QString videoCodec = QStringLiteral("libx264");
            if (args.contains(QStringLiteral("videoCodec"))) {
                const QJsonValue value = args.value(QStringLiteral("videoCodec"));
                if (!value.isString() || value.toString().trimmed().isEmpty())
                    return setError(err, QStringLiteral("videoCodec は空でない文字列で指定してください")),
                           QJsonObject();
                videoCodec = value.toString().trimmed();
            }

            int videoBitrate = 10000; // kbps。ExportConfig の既定値と合わせる。
            if (!positiveInteger(args, QStringLiteral("videoBitrate"),
                                 videoBitrate, &videoBitrate, err))
                return {};

            QString audioCodec = QStringLiteral("aac");
            if (args.contains(QStringLiteral("audioCodec"))) {
                const QJsonValue value = args.value(QStringLiteral("audioCodec"));
                if (!value.isString() || value.toString().trimmed().isEmpty())
                    return setError(err, QStringLiteral("audioCodec は空でない文字列で指定してください")),
                           QJsonObject();
                audioCodec = value.toString().trimmed();
            }
            int audioBitrate = 192; // kbps。ExportConfig の既定値と合わせる。
            if (!positiveInteger(args, QStringLiteral("audioBitrate"),
                                 audioBitrate, &audioBitrate, err))
                return {};

            RenderQueue* queue = ensureRenderQueue(err);
            if (!queue)
                return {};

            RenderJob job;
            job.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            job.name = QFileInfo(outputPath).fileName();
            if (job.name.isEmpty())
                job.name = outputPath;
            // projectFilePath は RenderQueue の音声 mux 元を兼ねる。MCP のプロジェクト
            // パスは常にプロジェクト JSON で音声ソースにならないので渡さない (空なら
            // RenderQueue は V1 先頭クリップの元音声へフォールバックする)。タイムラインの
            // 音声ミックスが必要なら、下の遅延ステップでそのパスを入れる。
            job.projectFilePath.clear();
            job.outputPath = outputPath;
            job.width = width;
            job.height = height;
            job.codec = videoCodec;
            job.bitrateBps = static_cast<qint64>(videoBitrate) * 1000;
            job.startUs = 0;
            job.endUs = 0;
            job.timeline = timeline();
            const double loudnessGainDb = exporter_loudnessGainDb();
            job.loudnessGainDb = loudnessGainDb;
            job.exportConfig = QJsonObject{
                {QStringLiteral("width"), width},
                {QStringLiteral("height"), height},
                {QStringLiteral("fps"), fps},
                {QStringLiteral("videoCodec"), videoCodec},
                {QStringLiteral("videoBitrate"), videoBitrate},
                {QStringLiteral("audioCodec"), audioCodec},
                {QStringLiteral("audioBitrate"), audioBitrate},
                {QStringLiteral("loudnessGainDb"), loudnessGainDb}
            };

            // ライブ Timeline を渡す経路は GUI の表示内容をそのまま使うため、
            // RenderQueue のワーカー開始前に MainWindow 側の補助データを同期する。
            m_window->syncProjectLightingToTimeline();
            QHash<QString, TimelineTrackMatteEntry> matteEntries;
            matteEntries.reserve(m_window->m_trackMatteClipEntries.size());
            for (auto it = m_window->m_trackMatteClipEntries.cbegin();
                 it != m_window->m_trackMatteClipEntries.cend(); ++it) {
                TimelineTrackMatteEntry entry;
                entry.matteType = it.value().matteType;
                entry.matteSourceClipId = it.value().matteSourceClipId;
                matteEntries.insert(it.key(), entry);
            }
            timeline()->setTrackMatteEntries(matteEntries);
            queue->setAcesPipeline(m_window->m_acesPipeline);
            queue->setLoudnessGainDb(loudnessGainDb);
            m_exportJobObservations.insert(job.uuid,
                                           ExportJobObservation{
                                               QStringLiteral("queued"), 0, QString()});

            // 音声ミックス (ffmpeg 同期実行) と start() は tools/call の中では走らせない。
            // jobId を先に返し、GUI イベントループでミックスを作ってからジョブを投入する。
            // それまで get_export_status は観測テーブルの queued を返す。
            const QPointer<RenderQueue> queueGuard(queue);
            const QPointer<MainWindow> windowGuard(m_window);
            const QString jobId = job.uuid;
            QTimer::singleShot(0, m_window, [this, queueGuard, windowGuard, job, jobId]() mutable {
                if (!queueGuard || !windowGuard || windowGuard->m_mcpTools != this)
                    return;
                QString audioMixError;
                const QString audioMixPath = windowGuard->prepareExportAudioMix(&audioMixError);
                if (!audioMixError.isEmpty()) {
                    ExportJobObservation& observation = m_exportJobObservations[jobId];
                    observation.status = QStringLiteral("failed");
                    observation.error = audioMixError;
                    return;
                }
                if (!audioMixPath.isEmpty())
                    job.projectFilePath = audioMixPath;
                queueGuard->addJob(job);
                queueGuard->start();
            });

            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("jobId"), job.uuid},
                {QStringLiteral("status"), QStringLiteral("queued")},
                {QStringLiteral("progress"), 0},
                {QStringLiteral("outputPath"), outputPath},
                {QStringLiteral("width"), width},
                {QStringLiteral("height"), height},
                {QStringLiteral("fps"), fps},
                {QStringLiteral("videoCodec"), videoCodec},
                {QStringLiteral("videoBitrate"), videoBitrate},
                {QStringLiteral("audioCodec"), audioCodec},
                {QStringLiteral("audioBitrate"), audioBitrate}
            };
        })
    }, exportVideoOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("import_media"),
        QStringLiteral("ダイアログを開かずに素材を指定トラックへ取り込む。trackIndex は映像・音声で同じ番号のトラックペアを指し、存在しなければ両方を作成する (0-based、既定 0)。startSec 省略時は指定した映像トラックの末尾に追記し、音声も同じ開始時刻に置く。startSec 指定時は既存クリップと重なる位置には配置せずエラーで拒否する (丸めない)。kind 既定 auto はファイルのストリーム構成で決め、映像の無いファイル (BGM / ナレーション) は音声トラックだけへ置く。video / audio で片側だけ取り込める。映像と音声の組は既存のGUI経路と同じlinkGroupでリンクし、Undo 1 回で取り消せる。動画/音声として開けないファイルはエラー。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("filePath"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("kind"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("auto"), QStringLiteral("video"), QStringLiteral("audio")
                }},
                {QStringLiteral("default"), QStringLiteral("auto")},
                {QStringLiteral("description"),
                 QStringLiteral("auto: 映像があれば V/A の組、無ければ音声だけ。video: 映像だけ。audio: 音声だけ (BGM やナレーションの追加)。省略時は auto")}
            }},
            {QStringLiteral("trackIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("default"), 0},
                {QStringLiteral("description"),
                 QStringLiteral("映像・音声で同じ番号の 0-based トラック番号。存在しなければ両方を作成する。省略時は 0")}
            }},
            {QStringLiteral("startSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("description"),
                 QStringLiteral("タイムライン絶対時刻 (秒)。省略時は指定映像トラックの末尾に追記し、音声も同じ開始時刻に置く。既存クリップと重なる場合はエラーで拒否する (丸めない)")}
            }}
        }, {QStringLiteral("filePath")}),
        guardedWrite(QStringLiteral("import_media"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("filePath"),
                                         QStringLiteral("trackIndex"),
                                         QStringLiteral("startSec"),
                                         QStringLiteral("kind")}, err))
                return {};
            QString filePath;
            if (!requiredString(args, QStringLiteral("filePath"), &filePath, err))
                return {};
            if (filePath.isEmpty())
                return setError(err, QStringLiteral("ファイルが見つかりません: %1").arg(filePath)),
                       QJsonObject();
            Timeline::ImportMediaKind importKind = Timeline::ImportMediaKind::Auto;
            if (args.contains(QStringLiteral("kind"))) {
                const QJsonValue kindValue = args.value(QStringLiteral("kind"));
                const QString kind = kindValue.isString() ? kindValue.toString() : QString();
                if (kind == QStringLiteral("auto"))
                    importKind = Timeline::ImportMediaKind::Auto;
                else if (kind == QStringLiteral("video"))
                    importKind = Timeline::ImportMediaKind::VideoOnly;
                else if (kind == QStringLiteral("audio"))
                    importKind = Timeline::ImportMediaKind::AudioOnly;
                else
                    return setError(err, QStringLiteral("kind must be auto, video or audio")),
                           QJsonObject();
            }
            int trackIndex = 0;
            if (!nonNegativeInteger(args, QStringLiteral("trackIndex"), 0,
                                    &trackIndex, err))
                return {};
            double startSec = -1.0;
            if (args.contains(QStringLiteral("startSec"))) {
                if (!requiredFiniteNumber(args, QStringLiteral("startSec"),
                                          &startSec, err))
                    return {};
                if (startSec < 0.0)
                    return setError(err, QStringLiteral("startSec must be non-negative")),
                           QJsonObject();
            }
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            Timeline::MediaImportResult importResult;
            if (!timeline()->importMedia(filePath, trackIndex, startSec,
                                         &importResult, err, importKind))
                return {};

            QJsonArray addedClips;
            const auto appendClip = [&addedClips](const QString &kind,
                                                   int track,
                                                   int index,
                                                   double start,
                                                   double duration) {
                if (track < 0 || index < 0)
                    return;
                addedClips.append(QJsonObject{
                    {QStringLiteral("kind"), kind},
                    {QStringLiteral("trackIndex"), track},
                    {QStringLiteral("clipIndex"), index},
                    {QStringLiteral("startSec"), start},
                    {QStringLiteral("durationSec"), duration}
                });
            };
            appendClip(QStringLiteral("video"), importResult.videoTrackIndex,
                       importResult.videoClipIndex, importResult.videoStartSec,
                       importResult.durationSec);
            appendClip(QStringLiteral("audio"), importResult.audioTrackIndex,
                       importResult.audioClipIndex, importResult.audioStartSec,
                       importResult.durationSec);

            m_window->hideWelcomeScreen();
            m_window->setWindowModified(true);
            m_window->updateStatusInfo();
            m_window->updateEditActions();
            QJsonObject response{
                {QStringLiteral("ok"), true},
                {QStringLiteral("clips"), addedClips}
            };
            if (!addedClips.isEmpty()) {
                const QJsonObject first = addedClips.first().toObject();
                response.insert(QStringLiteral("kind"), first.value(QStringLiteral("kind")));
                response.insert(QStringLiteral("trackIndex"),
                                first.value(QStringLiteral("trackIndex")));
                response.insert(QStringLiteral("clipIndex"),
                                first.value(QStringLiteral("clipIndex")));
                response.insert(QStringLiteral("startSec"),
                                first.value(QStringLiteral("startSec")));
                response.insert(QStringLiteral("durationSec"),
                                first.value(QStringLiteral("durationSec")));
            }
            return response;
        })
    }, importMediaOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("save_project"),
        QStringLiteral("プロジェクトを指定パスへ保存する。path 省略時は既存の保存先へ上書きし、未保存プロジェクトではエラーを返す。ダイアログは開かない。"),
        objectSchema(QJsonObject{
            {QStringLiteral("path"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }}
        }),
        guardedWrite(QStringLiteral("save_project"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("path")}, err))
                return {};
            if (!m_window)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            QString path = m_window->m_projectFilePath;
            if (args.contains(QStringLiteral("path"))) {
                if (!requiredString(args, QStringLiteral("path"), &path, err))
                    return {};
            }
            if (path.trimmed().isEmpty())
                return setError(err, QStringLiteral("保存先のパスを指定してください")),
                       QJsonObject();

            QString saveError;
            if (!m_window->saveProjectToPath(path, &saveError))
                return setError(err, saveError), QJsonObject();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("path"), m_window->m_projectFilePath}
            };
        })
    }, saveProjectOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("open_project"),
        QStringLiteral("指定パスのプロジェクトを読み込む。ダイアログや未保存変更の確認は行わない。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("path"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }}
        }, {QStringLiteral("path")}),
        guardedWrite(QStringLiteral("open_project"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("path")}, err))
                return {};
            QString path;
            if (!requiredString(args, QStringLiteral("path"), &path, err))
                return {};
            if (!m_window)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            QString openError;
            if (!m_window->openProjectFromPath(path, &openError))
                return setError(err, openError), QJsonObject();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("path"), m_window->m_projectFilePath}
            };
        })
    }, openProjectOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("select_clip"),
        QStringLiteral("指定クリップを選択する。kind / trackIndex 省略時は video トラック 0 (他のクリップ系ツールと同じ既定)。clipIndex は get_timeline の index。Timeline と MainWindow の両方の選択状態を GUI クリックと同じ規則で更新する。"),
        schemaWithRequired(clipProperties, {QStringLiteral("clipIndex")}),
        guardedWrite(QStringLiteral("select_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"),
                                         QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex")}, err))
                return {};
            QString kind = QStringLiteral("video");
            if (args.contains(QStringLiteral("kind"))) {
                if (!args.value(QStringLiteral("kind")).isString())
                    return setError(err, QStringLiteral("kind must be video or audio")),
                           QJsonObject();
                kind = args.value(QStringLiteral("kind")).toString();
            }
            if (kind != QStringLiteral("video") && kind != QStringLiteral("audio"))
                return setError(err, QStringLiteral("kind must be video or audio")),
                       QJsonObject();
            if (!args.contains(QStringLiteral("clipIndex")))
                return setError(err, QStringLiteral("clipIndex is required")),
                       QJsonObject();
            int trackIndex = 0;
            int clipIndex = 0;
            if (!nonNegativeInteger(args, QStringLiteral("trackIndex"), 0,
                                    &trackIndex, err)
                || !nonNegativeInteger(args, QStringLiteral("clipIndex"), 0,
                                       &clipIndex, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            const bool audio = kind == QStringLiteral("audio");
            if (!timeline()->selectClipByIndex(audio, trackIndex, clipIndex, err))
                return {};

            // Timeline の通知が同値選択で省略されても MainWindow の追跡値を
            // 必ず同期させ、selectedVideoClipRef() が同じ対象を返すようにする。
            m_window->m_selectedVideoTrackIndex = audio ? -1 : trackIndex;
            m_window->m_selectedVideoClipIndexTracked = clipIndex;
            m_window->updateEditActions();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), kind},
                {QStringLiteral("trackIndex"), trackIndex},
                {QStringLiteral("clipIndex"), clipIndex}
            };
        })
    }, selectClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("clear_selection"),
        QStringLiteral("タイムライン上の選択をすべて解除する。"),
        objectSchema(),
        guardedWrite(QStringLiteral("clear_selection"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            timeline()->clearSelection();
            m_window->m_selectedVideoTrackIndex = -1;
            m_window->m_selectedVideoClipIndexTracked = -1;
            m_window->updateEditActions();
            return QJsonObject{{QStringLiteral("ok"), true}};
        })
    }, clearSelectionOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("run_command"),
        QStringLiteral("お気に入り登録可能なコマンドを id で実行する。危険度を返し、blocking のコマンドは allowBlocking:true のときだけ実行する。quit のコマンドは MCP から常に実行できない。タイムラインを変更する操作は、コマンド自身が undo を記録する場合だけ Ctrl+Z / undo ツールで戻せる。応答の undoRecorded で判定すること (ダイアログを開くコマンドは応答時点では false になり得る)。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("id"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("allowBlocking"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("default"), false}
            }}
        }, {QStringLiteral("id")}),
        guardedWrite(QStringLiteral("run_command"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("id"),
                                         QStringLiteral("allowBlocking")}, err))
                return {};
            QString id;
            if (!requiredString(args, QStringLiteral("id"), &id, err))
                return {};
            bool allowBlocking = false;
            if (args.contains(QStringLiteral("allowBlocking"))) {
                const QJsonValue allowBlockingValue =
                    args.value(QStringLiteral("allowBlocking"));
                if (!allowBlockingValue.isBool()) {
                    setError(err, QStringLiteral("allowBlocking は boolean で指定してください"));
                    return {};
                }
                allowBlocking = allowBlockingValue.toBool();
            }
            if (!m_window)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            for (const auto& command : m_window->m_favoritableActions) {
                if (command.id != id)
                    continue;

                const QString risk = actionRiskToString(command.risk);
                // 危険度の拒否は enabled 判定より先に行う。終了は常に拒否し、
                // Blocking はユーザーが明示的に許可した場合だけ QAction を trigger する。
                if (command.risk == FavoritableActionRisk::Quit) {
                    setError(err, QStringLiteral("このコマンドはエディタを終了させるため MCP からは実行できません。"));
                    return {};
                }
                if (command.risk == FavoritableActionRisk::Blocking && !allowBlocking) {
                    setError(err, QStringLiteral("このコマンドはモーダルダイアログを開くため既定では実行しません。ユーザが画面で操作する必要があります。どうしても実行する場合は allowBlocking:true を指定してください。"));
                    return {};
                }
                if (!command.action || !command.action->isEnabled()) {
                    if (err)
                        *err = QStringLiteral("command is disabled: %1").arg(id);
                    return {};
                }
                UndoManager* undoManager = timeline()
                    ? timeline()->undoManager() : nullptr;
                // saveSerial は MAX_UNDO で先頭が落ちても増えるので、長い編集
                // セッションでも「この操作で undo が積まれたか」を正しく判定できる。
                const quint64 undoSerialBefore = undoManager
                    ? undoManager->saveSerial() : 0;
                // The QAction owns its own undo policy. Saving here would
                // double-stack actions that already save an undo state.
                command.action->trigger();
                const quint64 undoSerialAfter = undoManager
                    ? undoManager->saveSerial() : 0;
                syncSelectionAfterEdit();
                QJsonObject response{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("label"), command.label},
                    {QStringLiteral("risk"), risk}
                };
                const bool undoRecorded = undoManager
                    && undoSerialAfter > undoSerialBefore;
                response.insert(QStringLiteral("undoRecorded"), undoRecorded);
                if (undoRecorded)
                    response.insert(QStringLiteral("undoDescription"),
                                    undoManager->undoDescription());
                return response;
            }
            setError(err, QStringLiteral("command not found: %1").arg(id));
            return {};
        })
    }, runCommandOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("split_clip"),
        QStringLiteral("指定クリップを指定時刻で 2 つに分割する。timeSec はタイムライン絶対時刻 (秒、クリップ内オフセットではない)。クリップの開始・終了から 0.05 秒以内は拒否される。分割後は左側が元の clipIndex、右側が clipIndex+1 になり、後続クリップの index が 1 ずれる。linkGroup が同じクリップ (例: 対になる音声) も同時に同じ時刻で分割される。範囲カットは split_clip(開始) → split_clip(終了, clipIndex+1) → delete_clip(中央, ripple:true) の順で呼び出し、各操作後に get_timeline で index を再確認する。kind/trackIndex 省略時は video トラック 0。clipIndex は get_timeline の index。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("description"),
                 QStringLiteral("分割位置。タイムライン絶対時刻 (秒)、クリップ相対ではない。クリップの開始・終了から 0.05 秒以内の位置は 'split point is outside the clip' で拒否される。")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("timeSec")}),
        guardedWrite(QStringLiteral("split_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("timeSec")},
                                        err))
                return {};
            double timeSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("timeSec"), &timeSec, err))
                return {};
            if (timeSec < 0.0)
                return setError(err, QStringLiteral("timeSec must be non-negative")), QJsonObject();

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            // 分割点の範囲判定は Timeline::splitClipByIndex が
            // splitAtPlayhead と同じ 0.05 秒マージンで行う。ここで別の閾値を
            // 持つと境界付近で判定とメッセージが食い違うので持たない。

            Timeline* currentTimeline = timeline();
            if (!currentTimeline->splitClipByIndex(
                    target.audio, target.trackIndex, target.clipIndex, timeSec, err))
                return {};
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("newClipCount"), target.track->clipCount()},
                {QStringLiteral("leftIndex"), target.clipIndex},
                {QStringLiteral("rightIndex"), target.clipIndex + 1}
            };
        })
    }, splitClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("delete_clip"),
        QStringLiteral("指定クリップを削除し、必要なら後続クリップを詰める。linkGroup が同じクリップ (例: 対になる音声) も同時に削除される。削除後は後続クリップの index が 1 ずれる。ripple:true で後続クリップを前に詰める (既定 false)。詰まるのは削除したクリップと同じ linkGroup を持つトラックだけで、他のトラック (V2 の B ロールや A2 の BGM) はずれない。kind/trackIndex 省略時は video トラック 0。clipIndex は get_timeline の index。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("ripple"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("default"), false},
                {QStringLiteral("description"),
                 QStringLiteral("true で削除後の後続クリップを詰める (既定 false)")}
            }}
        }), {QStringLiteral("clipIndex")}),
        guardedWrite(QStringLiteral("delete_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("ripple")},
                                        err))
                return {};
            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            bool ripple = false;
            if (args.contains(QStringLiteral("ripple"))) {
                if (!args.value(QStringLiteral("ripple")).isBool()) {
                    setError(err, QStringLiteral("ripple must be a boolean"));
                    return {};
                }
                ripple = args.value(QStringLiteral("ripple")).toBool();
            }

            Timeline* currentTimeline = timeline();
            if (!currentTimeline->deleteClipByIndex(
                    target.audio, target.trackIndex, target.clipIndex, ripple, err))
                return {};
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("remainingClipCount"), target.track->clipCount()}
            };
        })
    }, deleteClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("move_clip"),
        QStringLiteral("指定クリップを指定開始時刻へ移動する。newStartSec はタイムライン絶対時刻 (秒)。必要なら別トラックへ移し、連続配置でも並べ替える。置けない要求はok:falseで理由と実際に置ける時刻を返す。kind/trackIndex 省略時は video トラック 0。clipIndex は get_timeline の index。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("newStartSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("description"),
                 QStringLiteral("移動先のタイムライン絶対時刻 (秒)。クリップ内相対時刻ではない。")}
            }},
            {QStringLiteral("newTrackIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("description"),
                 QStringLiteral("移動先の 0-based トラック番号。省略時は現在のトラック")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("newStartSec")}),
        guardedWrite(QStringLiteral("move_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("newStartSec"),
                                         QStringLiteral("newTrackIndex")},
                                        err))
                return {};
            double newStartSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("newStartSec"),
                                      &newStartSec, err)) {
                return {};
            }
            if (newStartSec < 0.0)
                return setError(err, QStringLiteral("newStartSec must be non-negative")), QJsonObject();

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            int newTrackIndex = target.trackIndex;
            if (!nonNegativeInteger(args, QStringLiteral("newTrackIndex"),
                                    target.trackIndex, &newTrackIndex, err))
                return {};
            Timeline* currentTimeline = timeline();
            Timeline::MoveClipResult moveResult;
            if (!currentTimeline->moveClipByIndex(
                    target.audio, target.trackIndex, target.clipIndex, newStartSec,
                    newTrackIndex, &moveResult, err)) {
                // 既定プロジェクトは V1/A1 の 1 段しかない。LLM がトラックを増やす
                // 手段 (run_command のトラック追加コマンド) をエラー文で案内する。
                if (err && err->contains(QStringLiteral("newTrackIndex"))) {
                    const QString needle = target.audio
                        ? QStringLiteral("オーディオトラックを追加")
                        : QStringLiteral("ビデオトラックを追加");
                    for (const auto& command : m_window->m_favoritableActions) {
                        if (!command.label.contains(needle))
                            continue;
                        *err += QStringLiteral("。トラックを追加するには run_command で id \"%1\" (%2) を実行してください")
                                    .arg(command.id, command.label);
                        break;
                    }
                }
                return {};
            }
            if (!moveResult.reason.isEmpty()) {
                return QJsonObject{
                    {QStringLiteral("ok"), false},
                    // 既存の move_clip 応答キーを失わないよう、失敗時も
                    // startSec は actualStartSec と同じ値で返す。
                    {QStringLiteral("startSec"), moveResult.actualStartSec},
                    {QStringLiteral("actualStartSec"), moveResult.actualStartSec},
                    {QStringLiteral("reason"), moveResult.reason},
                    {QStringLiteral("trackIndex"), newTrackIndex}
                };
            }
            if (moveResult.moved)
                m_window->setWindowModified(true);
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                // startSec は既存ツールの応答キーとして維持する。
                {QStringLiteral("startSec"), moveResult.actualStartSec},
                {QStringLiteral("actualStartSec"), moveResult.actualStartSec},
                {QStringLiteral("trackIndex"), moveResult.trackIndex},
                {QStringLiteral("clipIndex"), moveResult.clipIndex}
            };
        })
    }, moveClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_clip_property"),
        QStringLiteral("指定クリップのプロパティを設定する。property と有効範囲: volume 0..2 (1.0=原音の音量、0=無音)、opacity 0..1 (1.0=不透明)、speed 0.25..4 (1.0=等速)、pan -1..1 (0=中央)、videoScale 0.1..10 (1.0=等倍)。範囲外は out of range エラーで拒否される。speed は実尺が変わるため同じ linkGroup の音声クリップにも同時に適用する (応答の linkedApplied)。他のプロパティは指定クリップだけ。現在の volume/opacity/speed は get_timeline のクリップ情報で確認できる。kind/trackIndex 省略時は video トラック 0。clipIndex は get_timeline の index。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("property"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                // enum を出しておくと LLM が存在しないプロパティ名を投げてこない。
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("volume"), QStringLiteral("opacity"),
                    QStringLiteral("speed"), QStringLiteral("pan"),
                    QStringLiteral("videoScale")
                }},
                {QStringLiteral("description"),
                 QStringLiteral("設定対象。volume / opacity / speed / pan / videoScale")}
            }},
            {QStringLiteral("value"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("description"),
                 QStringLiteral("property に応じた値。volume: 0..2 (1.0=原音、0=無音)、opacity: 0..1 (1.0=不透明)、speed: 0.25..4 (1.0=等速)、pan: -1..1 (0=中央)、videoScale: 0.1..10 (1.0=等倍)。範囲外は拒否される。")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("property"), QStringLiteral("value")}),
        guardedWrite(QStringLiteral("set_clip_property"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("property"),
                                         QStringLiteral("value")},
                                        err))
                return {};
            QString property;
            if (!requiredString(args, QStringLiteral("property"), &property, err))
                return {};
            double value = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("value"), &value, err))
                return {};

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};

            Timeline* currentTimeline = timeline();
            // speed は実尺を変えるので、リンクした音声も同時に変えないと V/A の
            // 長さが食い違う。他のプロパティは指定クリップだけに適用する。
            const bool applyToLinked = property == QStringLiteral("speed");
            if (!currentTimeline->setClipPropertyByIndex(
                    target.audio, target.trackIndex, target.clipIndex, property, value, err,
                    applyToLinked))
                return {};
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("property"), property},
                {QStringLiteral("value"), value},
                {QStringLiteral("linkedApplied"), applyToLinked}
            };
        })
    }, setClipPropertyOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("trim_clip"),
        QStringLiteral("指定した映像クリップを、タイムライン絶対時刻 (秒) の timeSec でトリムする。edge=in はクリップの開始位置を保ったまま timeSec 時点の内容を新しい先頭にし、以降が (timeSec−開始) だけ左へ詰まる (RippleIn)。edge=out は末尾を timeSec にし後続が詰まる (RippleOut)。kind は video のみ対応し、同じ linkGroup の音声クリップ (A1 など) も同じ量だけトリムして映像と同期を保つ。ripple は既定 true。現在のトリムエンジンに非リップル種別がないため ripple:false は拒否する。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("edge"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("in"), QStringLiteral("out")
                }},
                {QStringLiteral("description"),
                 QStringLiteral("トリムする端。in は先頭、out は末尾")}
            }},
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("description"),
                 QStringLiteral("目標位置。タイムライン絶対時刻 (秒)、クリップ内相対時刻ではない")}
            }},
            {QStringLiteral("ripple"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("default"), true},
                {QStringLiteral("description"),
                 QStringLiteral("後続クリップを詰めるリップル。既定 true。false は現在未対応")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("edge"),
            QStringLiteral("timeSec")}),
        guardedWrite(QStringLiteral("trim_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("edge"),
                                         QStringLiteral("timeSec"), QStringLiteral("ripple")},
                                        err))
                return {};
            QString edge;
            if (!requiredString(args, QStringLiteral("edge"), &edge, err))
                return {};
            if (edge != QStringLiteral("in") && edge != QStringLiteral("out"))
                return setError(err, QStringLiteral("edge must be in or out")), QJsonObject();

            double timeSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("timeSec"), &timeSec, err))
                return {};
            if (timeSec < 0.0)
                return setError(err, QStringLiteral("timeSec must be non-negative")), QJsonObject();

            bool ripple = true;
            if (args.contains(QStringLiteral("ripple"))) {
                if (!args.value(QStringLiteral("ripple")).isBool())
                    return setError(err, QStringLiteral("ripple must be a boolean")), QJsonObject();
                ripple = args.value(QStringLiteral("ripple")).toBool();
            }
            if (!ripple)
                return setError(err, QStringLiteral("ripple:false is not supported by the trim engine")),
                       QJsonObject();

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            if (target.audio)
                return setError(err, QStringLiteral("trim_clip supports video clips only")), QJsonObject();
            if (target.track->isLocked())
                return setError(err, QStringLiteral("track is locked")), QJsonObject();

            const trimops::TrimType trimType = edge == QStringLiteral("in")
                ? trimops::TrimType::RippleIn : trimops::TrimType::RippleOut;
            const double deltaSec = timeSec
                - (edge == QStringLiteral("in") ? target.startSec : target.endSec);
            Timeline* currentTimeline = timeline();
            if (!currentTimeline->applyTrimLinked(target.track, target.clipIndex,
                                                  trimType, deltaSec, err))
                return {};
            // Timeline::applyTrimLinked (TimelineTrack::applyTrim) emits modified()
            // but deliberately does not push an undo state; keep this MCP
            // operation (video + linked audio) as one undo step.
            currentTimeline->saveUndoState(QStringLiteral("トリム"));
            syncSelectionAfterEdit();

            const ClipInfo& trimmed = target.track->clips().at(target.clipIndex);
            const double newStartSec = target.startSec;
            const double newEndSec = newStartSec + trimmed.effectiveDuration();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), target.trackIndex},
                {QStringLiteral("clipIndex"), target.clipIndex},
                {QStringLiteral("edge"), edge},
                {QStringLiteral("ripple"), ripple},
                {QStringLiteral("startSec"), newStartSec},
                {QStringLiteral("endSec"), newEndSec}
            };
        })
    }, trimClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_transition"),
        QStringLiteral("V1 (video トラック 0) の指定クリップにトランジションを設定する。type は TransitionType の識別子で None は leadIn / trailOut のトランジションを解除する。FadeIn はクリップ先頭の leadIn、それ以外はクリップ末尾の trailOut に適用する。FadeOut は次クリップの leadIn=FadeIn、FadeIn は前クリップの trailOut=FadeOut、その他は次クリップの leadIn に同型を同時に設定し、A1 の同 index にもミラーする。None は隣接側も解除する。durationSec は秒、既定 0.5、範囲 0.1..5.0。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("type"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), transitionTypeEnum()},
                {QStringLiteral("description"),
                 QStringLiteral("TransitionType の識別子。None で解除")}
            }},
            {QStringLiteral("durationSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0.1},
                {QStringLiteral("maximum"), 5.0},
                {QStringLiteral("default"), 0.5},
                {QStringLiteral("description"),
                 QStringLiteral("トランジション長 (秒)。0.1..5.0、既定 0.5。None では無視")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("type")}),
        guardedWrite(QStringLiteral("set_transition"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("type"),
                                         QStringLiteral("durationSec")},
                                        err))
                return {};
            QString typeName;
            if (!requiredString(args, QStringLiteral("type"), &typeName, err))
                return {};
            TransitionType type = TransitionType::None;
            if (!transitionTypeFromName(typeName, &type))
                return setError(err, QStringLiteral("type is not a valid TransitionType identifier")),
                       QJsonObject();

            double durationSec = 0.5;
            if (!positiveFiniteNumber(args, QStringLiteral("durationSec"), 0.5,
                                      &durationSec, err))
                return {};
            if (durationSec < 0.1 || durationSec > 5.0)
                return setError(err, QStringLiteral("durationSec must be in range [0.1, 5.0] seconds")),
                       QJsonObject();

            ClipTarget target;
            if (!readClipTarget(args, m_window, timeline(), &target, err))
                return {};
            if (target.audio || target.trackIndex != 0)
                return setError(err, QStringLiteral("set_transition supports video track 0 only")),
                       QJsonObject();
            if (type == TransitionType::None) {
                const ClipInfo& targetClip = target.track->clips().at(target.clipIndex);
                if (targetClip.leadIn.type == TransitionType::None
                    && targetClip.trailOut.type == TransitionType::None) {
                    return setError(err, QStringLiteral("clip has no transition to clear")),
                           QJsonObject();
                }
            }

            Timeline* currentTimeline = timeline();
            bool previousAudio = false;
            int previousTrack = -1;
            int previousClip = -1;
            for (int trackIndex = 0;
                 trackIndex < currentTimeline->videoTracks().size(); ++trackIndex) {
                TimelineTrack* track = currentTimeline->videoTracks().at(trackIndex);
                if (track && track->selectedClip() >= 0) {
                    previousTrack = trackIndex;
                    previousClip = track->selectedClip();
                    break;
                }
            }
            if (previousTrack < 0) {
                for (int trackIndex = 0;
                     trackIndex < currentTimeline->audioTracks().size(); ++trackIndex) {
                    TimelineTrack* track = currentTimeline->audioTracks().at(trackIndex);
                    if (track && track->selectedClip() >= 0) {
                        previousAudio = true;
                        previousTrack = trackIndex;
                        previousClip = track->selectedClip();
                        break;
                    }
                }
            }

            auto restoreSelection = [&]() {
                QString ignored;
                if (previousTrack >= 0)
                    currentTimeline->selectClipByIndex(previousAudio, previousTrack,
                                                       previousClip, &ignored);
                else
                    currentTimeline->clearSelection();
                syncSelectionAfterEdit();
            };
            QString selectionError;
            if (!currentTimeline->selectClipByIndex(false, 0, target.clipIndex,
                                                    &selectionError)) {
                restoreSelection();
                return setError(err, selectionError), QJsonObject();
            }

            Transition transition;
            transition.type = type;
            transition.duration = durationSec;
            if (type == TransitionType::None)
                currentTimeline->clearTransitionsOnSelected();
            else
                currentTimeline->applyTransitionToSelected(transition);
            restoreSelection();

            const ClipInfo& updated = target.track->clips().at(target.clipIndex);
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("kind"), QStringLiteral("video")},
                {QStringLiteral("trackIndex"), 0},
                {QStringLiteral("clipIndex"), target.clipIndex},
                {QStringLiteral("type"), transitionTypeNames().at(static_cast<int>(type))},
                {QStringLiteral("durationSec"),
                 type == TransitionType::None ? 0.0 : durationSec},
                {QStringLiteral("leadIn"), transitionToJson(updated.leadIn)},
                {QStringLiteral("trailOut"), transitionToJson(updated.trailOut)}
            };
        })
    }, setTransitionOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("add_text_overlay"),
        QStringLiteral("V1 に通常のテキスト／テロップを追加する。startSec と endSec はタイムライン絶対時刻 (秒) で、endSec は startSec より後にする。区間と重なる V1 の全クリップに付くので、クリップ境界をまたいでも表示される (重なるクリップが無ければエラー。応答の clipIndices が付いたクリップ)。x / y は正規化座標 0..1、fontSize はポイント単位で 6..256 (既定 32)、color は QColor/CSS 形式 (既定 #ffffff)。この操作は Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("text"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"), QStringLiteral("表示するテキスト。空文字列は不可")}
            }},
            {QStringLiteral("startSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("description"), QStringLiteral("表示開始位置 (秒、タイムライン絶対時刻)")}
            }},
            {QStringLiteral("endSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("description"), QStringLiteral("表示終了位置 (秒、タイムライン絶対時刻)")}
            }},
            {QStringLiteral("x"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0.0},
                {QStringLiteral("maximum"), 1.0},
                {QStringLiteral("default"), 0.5},
                {QStringLiteral("description"), QStringLiteral("中心 X の正規化座標 0..1、既定 0.5")}
            }},
            {QStringLiteral("y"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("minimum"), 0.0},
                {QStringLiteral("maximum"), 1.0},
                {QStringLiteral("default"), 0.85},
                {QStringLiteral("description"), QStringLiteral("中心 Y の正規化座標 0..1、既定 0.85")}
            }},
            {QStringLiteral("fontSize"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 6},
                {QStringLiteral("maximum"), 256},
                {QStringLiteral("default"), 32},
                {QStringLiteral("description"), QStringLiteral("フォントサイズ (pt)、6..256、既定 32")}
            }},
            {QStringLiteral("color"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("default"), QStringLiteral("#ffffff")},
                {QStringLiteral("description"), QStringLiteral("文字色。QColor/CSS 形式、既定 #ffffff")}
            }}
        }, {QStringLiteral("text"), QStringLiteral("startSec"), QStringLiteral("endSec")}),
        guardedWrite(QStringLiteral("add_text_overlay"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("text"), QStringLiteral("startSec"),
                                         QStringLiteral("endSec"), QStringLiteral("x"),
                                         QStringLiteral("y"), QStringLiteral("fontSize"),
                                         QStringLiteral("color")},
                                        err))
                return {};
            QString text;
            if (!requiredString(args, QStringLiteral("text"), &text, err))
                return {};
            if (text.trimmed().isEmpty())
                return setError(err, QStringLiteral("text must not be empty")), QJsonObject();

            double startSec = 0.0;
            double endSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("startSec"), &startSec, err)
                || !requiredFiniteNumber(args, QStringLiteral("endSec"), &endSec, err))
                return {};
            if (startSec < 0.0 || endSec < 0.0)
                return setError(err, QStringLiteral("text times must be non-negative")), QJsonObject();
            if (endSec <= startSec)
                return setError(err, QStringLiteral("endSec must be greater than startSec")),
                       QJsonObject();

            double x = 0.5;
            double y = 0.85;
            if (args.contains(QStringLiteral("x"))) {
                if (!finiteNumberForMcp(args, QStringLiteral("x"), &x, err))
                    return {};
                if (x < 0.0 || x > 1.0)
                    return setError(err, QStringLiteral("x must be in range [0, 1]")),
                           QJsonObject();
            }
            if (args.contains(QStringLiteral("y"))) {
                if (!finiteNumberForMcp(args, QStringLiteral("y"), &y, err))
                    return {};
                if (y < 0.0 || y > 1.0)
                    return setError(err, QStringLiteral("y must be in range [0, 1]")),
                           QJsonObject();
            }

            int fontSize = 32;
            if (!positiveInteger(args, QStringLiteral("fontSize"), 32,
                                 &fontSize, err))
                return {};
            if (fontSize < 6 || fontSize > 256)
                return setError(err, QStringLiteral("fontSize must be in range [6, 256]")),
                       QJsonObject();

            QString colorText = QStringLiteral("#ffffff");
            if (args.contains(QStringLiteral("color"))
                && !requiredString(args, QStringLiteral("color"), &colorText, err))
                return {};
            const QColor color(colorText);
            if (!color.isValid())
                return setError(err, QStringLiteral("color must be a valid QColor/CSS color")),
                       QJsonObject();

            Timeline* currentTimeline = timeline();
            if (!m_window || !currentTimeline)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            EnhancedTextOverlay overlay;
            overlay.text = text;
            QFont font = overlay.font;
            font.setPointSize(fontSize);
            overlay.font = font;
            overlay.color = color;
            overlay.backgroundColor = QColor(0, 0, 0, 0);
            overlay.x = x;
            overlay.y = y;
            overlay.startTime = startSec;
            overlay.endTime = endSec;
            // レンダラはその時刻にアクティブな V1 クリップのオーバーレイだけを焼き込む
            // ので、区間と重なる全クリップへ付ける (clip 0 固定だと 2 個目以降の
            // クリップの時間帯に出したテキストが表示されない)。
            const QVector<int> touchedClips =
                currentTimeline->addTextOverlayToVideoClipsInRange(overlay, startSec, endSec);
            if (touchedClips.isEmpty()) {
                return setError(err, QStringLiteral("V1 に startSec..endSec (%1..%2 秒) と重なるクリップがありません。get_timeline でクリップの時間帯を確認してください")
                                         .arg(startSec).arg(endSec)),
                       QJsonObject();
            }
            if (m_window->m_player)
                m_window->m_player->setTextOverlays(currentTimeline->timelineTextOverlays());

            QJsonArray clipIndices;
            for (int clipIndex : touchedClips)
                clipIndices.append(clipIndex);
            const int index = currentTimeline->videoTracks().first()->clips()
                                  .at(touchedClips.first()).textManager.count() - 1;
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("index"), index},
                {QStringLiteral("clipIndices"), clipIndices},
                {QStringLiteral("text"), overlay.text},
                {QStringLiteral("startSec"), overlay.startTime},
                {QStringLiteral("endSec"), overlay.endTime},
                {QStringLiteral("x"), overlay.x},
                {QStringLiteral("y"), overlay.y},
                {QStringLiteral("fontSize"), overlay.font.pointSize()},
                {QStringLiteral("color"), overlay.color.name(QColor::HexArgb)}
            };
        })
    }, addTextOverlayOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("add_caption"),
        QStringLiteral("字幕エディタが未オープンなら内部で生成する (画面には出さない)。字幕エディタの字幕一覧に 1 件追加し、"
                       "タイムラインへの反映は apply_captions を呼ぶ。この操作自体は undo 対象外。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("text"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("startSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }},
            {QStringLiteral("endSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }}
        }, {QStringLiteral("text"), QStringLiteral("startSec"), QStringLiteral("endSec")}),
        guardedWrite(QStringLiteral("add_caption"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("text"), QStringLiteral("startSec"),
                                         QStringLiteral("endSec")}, err))
                return {};
            QString text;
            if (!requiredString(args, QStringLiteral("text"), &text, err))
                return {};
            double startSec = 0.0;
            double endSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("startSec"), &startSec, err)
                || !requiredFiniteNumber(args, QStringLiteral("endSec"), &endSec, err)) {
                return {};
            }
            if (startSec < 0.0 || endSec < 0.0)
                return setError(err, QStringLiteral("caption times must be non-negative")), QJsonObject();
            if (endSec <= startSec) {
                setError(err, QStringLiteral("endSec must be greater than startSec"));
                return {};
            }
            constexpr double kMaxCaptionSec =
                static_cast<double>(std::numeric_limits<qint64>::max()) / 1000.0;
            if (startSec > kMaxCaptionSec || endSec > kMaxCaptionSec)
                return setError(err, QStringLiteral("caption time is too large")), QJsonObject();
            CaptionEditorDialog* dialog = m_window
                ? m_window->ensureCaptionEditorDialog() : nullptr;
            if (!dialog)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            caption::Track track = dialog->track();
            const QList<caption::Clip> oldClips = track.clips();
            const qint64 startMs = qRound64(startSec * 1000.0);
            const qint64 endMs = qRound64(endSec * 1000.0);
            if (endMs <= startMs) {
                setError(err, QStringLiteral("endSec must be greater than startSec"));
                return {};
            }
            int insertIndex = 0;
            for (const caption::Clip& clip : oldClips) {
                if (clip.startMs <= startMs)
                    ++insertIndex;
            }

            // 字幕は CaptionEditorDialog が持つ caption::Track 側の状態で、
            // Timeline::currentState() のスナップショットには入らない。ここで
            // saveUndoState を呼ぶと「押しても何も戻らない Ctrl+Z」を 1 段積むだけ
            // になるので呼ばない。取り消しは字幕エディタ側の責務。
            caption::Clip captionClip;
            captionClip.startMs = startMs;
            captionClip.endMs = endMs;
            captionClip.text = text;
            track.addClip(captionClip);
            track.sortByStart();
            dialog->setTrack(track);
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("index"), insertIndex},
                {QStringLiteral("captionCount"), track.clipCount()}
            };
        })
    }, addCaptionOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("apply_captions"),
        QStringLiteral("字幕エディタに保持されている字幕を V1 の1語字幕オーバーレイとしてタイムラインへ適用する (字幕エディタの「1語字幕をタイムラインに適用」と同じ経路)。既存の生成済み1語字幕は置き換える。Ctrl+Z / undo ツールで戻せる (戻るのはタイムライン側だけで、字幕エディタの一覧は戻らない)。"),
        objectSchema(),
        guardedWrite(QStringLiteral("apply_captions"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("エディタまたはタイムラインを利用できません")),
                       QJsonObject();

            CaptionEditorDialog* dialog = m_window->ensureCaptionEditorDialog();
            if (!dialog)
                return setError(err, QStringLiteral("エディタまたはタイムラインを利用できません")),
                       QJsonObject();
            const caption::Track track = dialog->track();
            if (track.clipCount() <= 0)
                return setError(err,
                                QStringLiteral("適用できる字幕がありません。add_caption で追加してください。")),
                       QJsonObject();

            for (const caption::Clip& clip : track.clips()) {
                if (clip.text.trimmed().isEmpty())
                    return setError(err,
                                    QStringLiteral("空の字幕はタイムラインへ適用できません。")),
                           QJsonObject();
                if (clip.endMs <= clip.startMs)
                    return setError(err,
                                    QStringLiteral("字幕の終了時刻は開始時刻より後にしてください。")),
                           QJsonObject();
            }

            QString error;
            int appliedCount = 0;
            if (!m_window->applyCaptionEditorTrackToTimeline(&error, &appliedCount))
                return setError(err, error), QJsonObject();

            m_window->updateEditActions();
            Timeline* currentTimeline = timeline();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("appliedCount"), appliedCount},
                {QStringLiteral("captionCount"), dialog->track().clipCount()},
                {QStringLiteral("timelineCaptionCount"), currentTimeline
                    ? currentTimeline->generatedCaptionOverlays().size() : 0}
            };
        })
    }, applyCaptionsOutputSchema));

    const QJsonObject captionListOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("captionCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("captionCount")});

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("remove_caption"),
        QStringLiteral("字幕エディタの一覧から index (get_captions の captions[].index) の字幕を 1 件削除する。タイムラインへ反映するには apply_captions を呼ぶ。この操作自体は undo 対象外。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("index"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0}
            }}
        }, {QStringLiteral("index")}),
        guardedWrite(QStringLiteral("remove_caption"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("index")}, err))
                return {};
            if (!args.contains(QStringLiteral("index")))
                return setError(err, QStringLiteral("index is required")), QJsonObject();
            int index = -1;
            if (!nonNegativeInteger(args, QStringLiteral("index"), -1, &index, err))
                return {};
            CaptionEditorDialog* dialog = m_window
                ? m_window->ensureCaptionEditorDialog() : nullptr;
            if (!dialog)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            caption::Track track = dialog->track();
            if (index < 0 || index >= track.clipCount()) {
                return setError(err, QStringLiteral("index is out of range (字幕は %1 件: 0..%2)")
                                         .arg(track.clipCount()).arg(track.clipCount() - 1)),
                       QJsonObject();
            }
            track.removeClipAt(index);
            dialog->setTrack(track);
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("captionCount"), track.clipCount()}
            };
        })
    }, captionListOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("clear_captions"),
        QStringLiteral("字幕エディタの一覧を空にする。タイムライン上の生成済み字幕はそのままなので、消したい場合は続けて apply_captions を呼べないことに注意 (空の一覧は適用できない)。undo ツールでタイムライン側を戻す。この操作自体は undo 対象外。"),
        objectSchema(),
        guardedWrite(QStringLiteral("clear_captions"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            CaptionEditorDialog* dialog = m_window
                ? m_window->ensureCaptionEditorDialog() : nullptr;
            if (!dialog)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            caption::Track track = dialog->track();
            track.clear();
            dialog->setTrack(track);
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("captionCount"), track.clipCount()}
            };
        })
    }, captionListOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_playhead"),
        QStringLiteral("再生ヘッドを指定時刻へ移動する。timeSec が範囲外の場合はエラーにせず [0, タイムライン総尺] に丸め、実際に設定した位置を playheadSec で返す。VideoPlayer もシークし、停止中はプレビューが指定時刻のフレームに更新される (描画はイベントループ後)。応答の playing は呼び出し前に再生中だったか、previewSeekRequested は VideoPlayer にシークを要求したかを示す。編集状態を変える操作ではなく、タイムライン編集の Undo / redo には影響しない。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")},
                {QStringLiteral("description"),
                 QStringLiteral("タイムライン絶対時刻 (秒)。範囲外は [0, タイムライン総尺] に丸める。")}
            }}
        }, {QStringLiteral("timeSec")}),
        guardedWrite(QStringLiteral("set_playhead"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("timeSec")}, err))
                return {};
            double timeSec = 0.0;
            if (!requiredFiniteNumber(args, QStringLiteral("timeSec"), &timeSec, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            if (timeSec < 0.0)
                timeSec = 0.0;
            const double duration = qMax(0.0, timeline()->totalDuration());
            timeSec = qMin(timeSec, duration);
            timeline()->setPlayheadPosition(timeSec);
            VideoPlayer* player = m_window->m_player;
            const bool playing = player && player->isPlaying();
            if (player)
                player->seek(qRound(timeSec * 1000.0));
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("playheadSec"), timeSec},
                {QStringLiteral("playing"), playing},
                {QStringLiteral("previewSeekRequested"), player != nullptr}
            };
        })
    }, setPlayheadOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("undo"),
        QStringLiteral("直前のタイムライン変更を取り消す破壊的操作。Ctrl+Z / undo ツールで記録済みの変更を戻せる。"),
        objectSchema(),
        guardedWrite(QStringLiteral("undo"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            if (!timeline()->canUndo())
                return QJsonObject{{QStringLiteral("ok"), false},
                                   {QStringLiteral("reason"), QStringLiteral("nothing to undo")}};
            timeline()->undo();
            syncSelectionAfterEdit();
            return QJsonObject{{QStringLiteral("ok"), true}};
        })
    }, undoOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("redo"),
        QStringLiteral("直前に取り消したタイムライン変更を再適用する破壊的操作。Ctrl+Y / redo ツールで変更を戻し直せる。"),
        objectSchema(),
        guardedWrite(QStringLiteral("redo"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            if (!timeline()->canRedo())
                return QJsonObject{{QStringLiteral("ok"), false},
                                   {QStringLiteral("reason"), QStringLiteral("nothing to redo")}};
            timeline()->redo();
            syncSelectionAfterEdit();
            return QJsonObject{{QStringLiteral("ok"), true}};
        })
    }, redoOutputSchema));
}

} // namespace mcp
