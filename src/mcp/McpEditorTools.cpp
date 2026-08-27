#include "McpEditorTools.h"

#include "McpToolRegistry.h"
#include "../CaptionEditorDialog.h"
#include "../MainWindow.h"
#include "../RenderQueue.h"
#include "../TimelineFrameRenderer.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "../VideoPlayer.h"

#include <QAction>
#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QStringList>
#include <QtGlobal>

#include <cmath>
#include <limits>

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
        {QStringLiteral("selected"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}
    };
    return outputSchemaOf(properties, {
        QStringLiteral("index"), QStringLiteral("displayName"),
        QStringLiteral("filePath"), QStringLiteral("startSec"),
        QStringLiteral("durationSec"), QStringLiteral("inPointSec"),
        QStringLiteral("outPointSec"), QStringLiteral("speed"),
        QStringLiteral("volume"), QStringLiteral("opacity"),
        QStringLiteral("linkGroup"), QStringLiteral("selected")
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
            }}
        }},
        {QStringLiteral("trackIndex"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0}
        }},
        {QStringLiteral("clipIndex"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("minimum"), 0}
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
    if (!track)
        return setError(err, QStringLiteral("track index is out of range"));
    if (clipIndex >= track->clipCount())
        return setError(err, QStringLiteral("clip index is out of range"));

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
        {QStringLiteral("selected"), selected}
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

ToolHandler McpEditorTools::guardedWrite(const QString& toolName, ToolHandler inner)
{
    return [this, toolName, inner](const QJsonObject& args, QString* err) -> QJsonObject {
        if (!beginExclusiveWrite(toolName, err))
            return {};
        struct Reset {
            McpEditorTools* tools;
            ~Reset() { tools->endExclusiveWrite(); }
        } reset{this};
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
                {QStringLiteral("audioTrackCount"), audioTracks.size()}
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
        {QStringLiteral("audioTrackCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("projectName"), QStringLiteral("width"),
        QStringLiteral("height"), QStringLiteral("fps"),
        QStringLiteral("durationSec"), QStringLiteral("playheadSec"),
        QStringLiteral("videoTrackCount"), QStringLiteral("audioTrackCount")}));

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
        QStringLiteral("byteSize"), QStringLiteral("base64Bytes")}));

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
        QStringLiteral("status"), QStringLiteral("progress")}));

    const QJsonObject timelineProperties{
        {QStringLiteral("kind"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{
                QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("all")
            }}
        }}
    };
    m_registry->registerTool(withOutputSchema({
        QStringLiteral("get_timeline"),
        QStringLiteral("タイムライン上の全クリップをトラック別に秒単位で返す。編集前の現状確認に使う。"),
        objectSchema(timelineProperties),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("kind")}, err))
                return {};
            if (!m_window) {
                if (err)
                    *err = QStringLiteral("editor not available");
                return {};
            }

            const QString kind = args.value(QStringLiteral("kind"))
                .toString(QStringLiteral("all"));
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
    }, {}));

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
        }}
    }, {QStringLiteral("captions")}));

    const QJsonObject commandProperties{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")}
        }}
    };
    m_registry->registerTool(withOutputSchema({
        QStringLiteral("list_commands"),
        QStringLiteral("エディタで利用できるお気に入り登録可能なコマンドを列挙する。id、表示名、メニュー階層、危険度 (safe / blocking / quit)、有効状態を返す。blocking のコマンドは run_command で既定では実行を拒否される。"),
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
        {QStringLiteral("total"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("commands"), QStringLiteral("total")}));
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
        {QStringLiteral("videoBitrate"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
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
        {QStringLiteral("risk"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }, {QStringLiteral("ok"), QStringLiteral("id"),
        QStringLiteral("label"), QStringLiteral("risk")});

    const QJsonObject splitClipOutputSchema = outputSchemaOf(QJsonObject{
        {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("newClipCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
    }, {QStringLiteral("ok"), QStringLiteral("newClipCount")});

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
        {QStringLiteral("value"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("ok"), QStringLiteral("property"),
        QStringLiteral("value")});

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
        {QStringLiteral("playheadSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
    }, {QStringLiteral("ok"), QStringLiteral("playheadSec")});

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
        QStringLiteral("現在のタイムラインを動画ファイルへ非同期で書き出す。tools/call はジョブ投入後すぐに jobId を返し、完了は get_export_status で確認する。width / height / fps の省略時は現在のプロジェクト設定を使い、videoBitrate は kbps。"),
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
                                         QStringLiteral("videoBitrate")}, err))
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

            RenderQueue* queue = ensureRenderQueue(err);
            if (!queue)
                return {};

            RenderJob job;
            job.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            job.name = QFileInfo(outputPath).fileName();
            if (job.name.isEmpty())
                job.name = outputPath;
            job.projectFilePath = m_window->m_projectFilePath;
            job.outputPath = outputPath;
            job.width = width;
            job.height = height;
            job.codec = videoCodec;
            job.bitrateBps = static_cast<qint64>(videoBitrate) * 1000;
            job.startUs = 0;
            job.endUs = 0;
            job.timeline = timeline();
            job.exportConfig = QJsonObject{
                {QStringLiteral("width"), width},
                {QStringLiteral("height"), height},
                {QStringLiteral("fps"), fps},
                {QStringLiteral("videoCodec"), videoCodec},
                {QStringLiteral("videoBitrate"), videoBitrate},
                {QStringLiteral("audioCodec"), QStringLiteral("aac")},
                {QStringLiteral("audioBitrate"), 192}
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
            queue->addJob(job);
            m_exportJobObservations.insert(job.uuid,
                                           ExportJobObservation{
                                               QStringLiteral("queued"), 0, QString()});

            // start() は Timeline の解決やレンダースレッドの準備を行うため、
            // tools/call の中では待たない。GUI イベントループへ移して jobId を
            // 先に返し、実処理の状態は get_export_status で確認できるようにする。
            const QPointer<RenderQueue> queueGuard(queue);
            QTimer::singleShot(0, m_window, [queueGuard]() {
                if (queueGuard)
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
                {QStringLiteral("videoBitrate"), videoBitrate}
            };
        })
    }, exportVideoOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("import_media"),
        QStringLiteral("ダイアログを開かずに素材を指定トラックへ取り込む。映像と音声の組は既存のGUI経路と同じlinkGroupでリンクし、Undo 1 回で取り消せる。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("filePath"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }},
            {QStringLiteral("trackIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0},
                {QStringLiteral("default"), 0}
            }},
            {QStringLiteral("startSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }}
        }, {QStringLiteral("filePath")}),
        guardedWrite(QStringLiteral("import_media"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("filePath"),
                                         QStringLiteral("trackIndex"),
                                         QStringLiteral("startSec")}, err))
                return {};
            QString filePath;
            if (!requiredString(args, QStringLiteral("filePath"), &filePath, err))
                return {};
            if (filePath.isEmpty())
                return setError(err, QStringLiteral("ファイルが見つかりません: %1").arg(filePath)),
                       QJsonObject();
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
                                         &importResult, err))
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
        QStringLiteral("指定クリップを選択する。Timeline と MainWindow の両方の選択状態をGUIクリックと同じ規則で更新する。"),
        schemaWithRequired(clipProperties,
                           {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                            QStringLiteral("clipIndex")}),
        guardedWrite(QStringLiteral("select_clip"),
                     [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"),
                                         QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex")}, err))
                return {};
            if (!args.contains(QStringLiteral("kind"))
                || !args.value(QStringLiteral("kind")).isString())
                return setError(err, QStringLiteral("kind must be video or audio")),
                       QJsonObject();
            const QString kind = args.value(QStringLiteral("kind")).toString();
            if (kind != QStringLiteral("video") && kind != QStringLiteral("audio"))
                return setError(err, QStringLiteral("kind must be video or audio")),
                       QJsonObject();
            if (!args.contains(QStringLiteral("trackIndex"))
                || !args.contains(QStringLiteral("clipIndex")))
                return setError(err, QStringLiteral("trackIndex and clipIndex are required")),
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
                const int undoIndexBefore = undoManager
                    ? undoManager->currentIndex() : -1;
                // The QAction owns its own undo policy. Saving here would
                // double-stack actions that already save an undo state.
                command.action->trigger();
                const int undoIndexAfter = undoManager
                    ? undoManager->currentIndex() : -1;
                syncSelectionAfterEdit();
                QJsonObject response{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("label"), command.label},
                    {QStringLiteral("risk"), risk}
                };
                const bool undoRecorded = undoManager
                    && undoIndexAfter > undoIndexBefore;
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
        QStringLiteral("指定クリップを指定時刻で 2 つに分割する。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
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
                {QStringLiteral("newClipCount"), target.track->clipCount()}
            };
        })
    }, splitClipOutputSchema));

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("delete_clip"),
        QStringLiteral("指定クリップを削除し、必要なら後続クリップを詰める。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("ripple"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")}
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
        QStringLiteral("指定クリップを指定開始時刻へ移動する。必要なら別トラックへ移し、連続配置でも並べ替える。置けない要求はok:falseで理由と実際に置ける時刻を返す。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("newStartSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }},
            {QStringLiteral("newTrackIndex"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0}
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
                    newTrackIndex, &moveResult, err))
                return {};
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
        QStringLiteral("指定クリップの音量、不透明度、速度、パン、映像スケールを設定する。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("property"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                // enum を出しておくと LLM が存在しないプロパティ名を投げてこない。
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("volume"), QStringLiteral("opacity"),
                    QStringLiteral("speed"), QStringLiteral("pan"),
                    QStringLiteral("videoScale")
                }}
            }},
            {QStringLiteral("value"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
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
            if (!currentTimeline->setClipPropertyByIndex(
                    target.audio, target.trackIndex, target.clipIndex, property, value, err))
                return {};
            syncSelectionAfterEdit();
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("property"), property},
                {QStringLiteral("value"), value}
            };
        })
    }, setClipPropertyOutputSchema));

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

    m_registry->registerTool(withOutputSchema({
        QStringLiteral("set_playhead"),
        QStringLiteral("再生ヘッドを指定時刻へ移動する。VideoPlayer もシークし、停止中はプレビューが指定時刻のフレームに更新される (描画はイベントループ後)。編集状態を変える操作ではなく、タイムライン編集の Undo / redo には影響しない。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
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
