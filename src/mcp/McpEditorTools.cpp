#include "McpEditorTools.h"

#include "McpToolRegistry.h"
#include "../CaptionEditorDialog.h"
#include "../MainWindow.h"
#include "../Timeline.h"

#include <QAction>
#include <QJsonArray>
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

QJsonObject clipToJson(const ClipInfo& clip, int clipIndex, double startSec)
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
        {QStringLiteral("linkGroup"), clip.linkGroup}
    };
}

QJsonArray tracksToJson(const QVector<QVector<ClipInfo>>& tracks)
{
    QJsonArray result;
    for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        QJsonArray clips;
        double cursorSec = 0.0;
        const QVector<ClipInfo>& track = tracks.at(trackIndex);
        for (int clipIndex = 0; clipIndex < track.size(); ++clipIndex) {
            const ClipInfo& clip = track.at(clipIndex);
            // TimelineSequence::duration() and Timeline's placement logic both
            // treat leadInSec as a gap before the clip, then add effectiveDuration().
            // Therefore cursor + leadInSec is the absolute timeline start.
            cursorSec += clip.leadInSec;
            clips.append(clipToJson(clip, clipIndex, cursorSec));
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

Timeline* McpEditorTools::timeline() const
{
    return m_window ? m_window->m_timeline : nullptr;
}

void McpEditorTools::registerReadTools()
{
    if (!m_registry)
        return;

    m_registry->registerTool({
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
    });

    const QJsonObject timelineProperties{
        {QStringLiteral("kind"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("enum"), QJsonArray{
                QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("all")
            }}
        }}
    };
    m_registry->registerTool({
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
                              tracksToJson(currentTimeline->allVideoTracks()));
            if (result.contains(QStringLiteral("audio")))
                result.insert(QStringLiteral("audio"),
                              tracksToJson(currentTimeline->allAudioTracks()));
            return result;
        }
    });

    m_registry->registerTool({
        QStringLiteral("get_captions"),
        QStringLiteral("字幕エディタに保持されている字幕クリップを秒単位で返す。字幕内容と現在の編集状態の確認に使う。"),
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
            return QJsonObject{{QStringLiteral("captions"), captions}};
        }
    });

    const QJsonObject commandProperties{
        {QStringLiteral("query"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("string")}
        }}
    };
    m_registry->registerTool({
        QStringLiteral("list_commands"),
        QStringLiteral("エディタで利用できるお気に入り登録可能なコマンドを列挙する。id、表示名、メニュー階層、実行可否の確認に使う。"),
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
                    {QStringLiteral("enabled"),
                     command.action ? command.action->isEnabled() : false}
                });
            }
            return QJsonObject{
                {QStringLiteral("commands"), commands},
                {QStringLiteral("total"), m_window->m_favoritableActions.size()}
            };
        }
    });
}

void McpEditorTools::registerWriteTools()
{
    if (!m_registry)
        return;

    const QJsonObject clipProperties = clipSelectorProperties();

    m_registry->registerTool({
        QStringLiteral("run_command"),
        QStringLiteral("お気に入りコマンドを実行する。内容によってはタイムラインを変更する破壊的操作で、変更は Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("id"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}
            }}
        }, {QStringLiteral("id")}),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {QStringLiteral("id")}, err))
                return {};
            QString id;
            if (!requiredString(args, QStringLiteral("id"), &id, err))
                return {};
            if (!m_window)
                return setError(err, QStringLiteral("editor not available")), QJsonObject();

            for (const auto& command : m_window->m_favoritableActions) {
                if (command.id != id)
                    continue;
                if (!command.action || !command.action->isEnabled()) {
                    if (err)
                        *err = QStringLiteral("command is disabled: %1").arg(id);
                    return {};
                }
                // The QAction owns its own undo policy. Saving here would
                // double-stack actions that already save an undo state.
                command.action->trigger();
                return QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("label"), command.label}
                };
            }
            setError(err, QStringLiteral("command not found: %1").arg(id));
            return {};
        }
    });

    m_registry->registerTool({
        QStringLiteral("split_clip"),
        QStringLiteral("指定クリップを指定時刻で 2 つに分割する。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("timeSec")}),
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
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("newClipCount"), target.track->clipCount()}
            };
        }
    });

    m_registry->registerTool({
        QStringLiteral("delete_clip"),
        QStringLiteral("指定クリップを削除し、必要なら後続クリップを詰める。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("ripple"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")}
            }}
        }), {QStringLiteral("clipIndex")}),
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
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("remainingClipCount"), target.track->clipCount()}
            };
        }
    });

    m_registry->registerTool({
        QStringLiteral("move_clip"),
        QStringLiteral("指定クリップを同一トラック内の指定開始時刻へ移動する。タイムラインを変更する破壊的操作で、Ctrl+Z / undo ツールで戻せる。"),
        schemaWithRequired(mergedProperties(clipProperties, QJsonObject{
            {QStringLiteral("newStartSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }}
        }), {QStringLiteral("clipIndex"), QStringLiteral("newStartSec")}),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args,
                                        {QStringLiteral("kind"), QStringLiteral("trackIndex"),
                                         QStringLiteral("clipIndex"), QStringLiteral("newStartSec")},
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
            Timeline* currentTimeline = timeline();
            double settledStart = 0.0;
            if (!currentTimeline->moveClipByIndex(
                    target.audio, target.trackIndex, target.clipIndex, newStartSec, &settledStart, err))
                return {};
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("startSec"), settledStart}
            };
        }
    });

    m_registry->registerTool({
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
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("property"), property},
                {QStringLiteral("value"), value}
            };
        }
    });

    m_registry->registerTool({
        QStringLiteral("add_caption"),
        QStringLiteral("字幕エディタの字幕一覧に 1 件追加する。タイムラインへの反映は字幕エディタの「適用」が必要で、"
                       "この操作は Ctrl+Z / undo ツールの対象外 (取り消しは字幕エディタ側で行う)。"),
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
            if (!m_window || !m_window->m_captionEditorDialog)
                return setError(err, QStringLiteral("caption editor is not open")), QJsonObject();

            caption::Track track = m_window->m_captionEditorDialog->track();
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
            m_window->m_captionEditorDialog->setTrack(track);
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("index"), insertIndex},
                {QStringLiteral("captionCount"), track.clipCount()}
            };
        }
    });

    m_registry->registerTool({
        QStringLiteral("set_playhead"),
        QStringLiteral("再生ヘッドを指定時刻へ移動する。編集状態を変える操作ではなく、タイムライン編集の Undo / redo には影響しない。"),
        schemaWithRequired(QJsonObject{
            {QStringLiteral("timeSec"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}
            }}
        }, {QStringLiteral("timeSec")}),
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
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("playheadSec"), timeSec}
            };
        }
    });

    m_registry->registerTool({
        QStringLiteral("undo"),
        QStringLiteral("直前のタイムライン変更を取り消す破壊的操作。Ctrl+Z / undo ツールで記録済みの変更を戻せる。"),
        objectSchema(),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            if (!timeline()->canUndo())
                return QJsonObject{{QStringLiteral("ok"), false},
                                   {QStringLiteral("reason"), QStringLiteral("nothing to undo")}};
            timeline()->undo();
            return QJsonObject{{QStringLiteral("ok"), true}};
        }
    });

    m_registry->registerTool({
        QStringLiteral("redo"),
        QStringLiteral("直前に取り消したタイムライン変更を再適用する破壊的操作。Ctrl+Y / redo ツールで変更を戻し直せる。"),
        objectSchema(),
        [this](const QJsonObject& args, QString* err) -> QJsonObject {
            if (!rejectUnknownArguments(args, {}, err))
                return {};
            if (!m_window || !timeline())
                return setError(err, QStringLiteral("editor not available")), QJsonObject();
            if (!timeline()->canRedo())
                return QJsonObject{{QStringLiteral("ok"), false},
                                   {QStringLiteral("reason"), QStringLiteral("nothing to redo")}};
            timeline()->redo();
            return QJsonObject{{QStringLiteral("ok"), true}};
        }
    });
}

} // namespace mcp
