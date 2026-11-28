#include "MogrtTemplate.h"
#include "ExternalGenerator.h"
#include "Timeline.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtGlobal>

#include <cmath>
#include <cstring>

namespace mogrt {

namespace {

double easeOutCubic(double t)
{
    t = qBound(0.0, t, 1.0);
    const double inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

quint32 crc32Bytes(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (int i = 0; i < data.size(); ++i) {
        crc ^= static_cast<uchar>(data[i]);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

void putU16Le(QByteArray &out, quint16 v)
{
    out.append(static_cast<char>(v & 0xFFu));
    out.append(static_cast<char>((v >> 8) & 0xFFu));
}

void putU32Le(QByteArray &out, quint32 v)
{
    out.append(static_cast<char>(v & 0xFFu));
    out.append(static_cast<char>((v >> 8) & 0xFFu));
    out.append(static_cast<char>((v >> 16) & 0xFFu));
    out.append(static_cast<char>((v >> 24) & 0xFFu));
}

quint16 getU16Le(const uchar *p)
{
    return static_cast<quint16>(p[0] | (p[1] << 8));
}

quint32 getU32Le(const uchar *p)
{
    return static_cast<quint32>(p[0])
         | (static_cast<quint32>(p[1]) << 8)
         | (static_cast<quint32>(p[2]) << 16)
         | (static_cast<quint32>(p[3]) << 24);
}

QString styleToString(TemplateStyle s)
{
    switch (s) {
    case TemplateStyle::LowerThird:      return QStringLiteral("lower-third");
    case TemplateStyle::SocialBadge:     return QStringLiteral("social-badge");
    case TemplateStyle::CountdownTimer:  return QStringLiteral("countdown");
    case TemplateStyle::ProgressChapter: return QStringLiteral("progress-chapter");
    case TemplateStyle::SubscribeBanner: return QStringLiteral("subscribe-banner");
    }
    return QStringLiteral("lower-third");
}

TemplateStyle styleFromString(const QString &s)
{
    if (s == QStringLiteral("social-badge"))     return TemplateStyle::SocialBadge;
    if (s == QStringLiteral("countdown"))        return TemplateStyle::CountdownTimer;
    if (s == QStringLiteral("progress-chapter")) return TemplateStyle::ProgressChapter;
    if (s == QStringLiteral("subscribe-banner")) return TemplateStyle::SubscribeBanner;
    return TemplateStyle::LowerThird;
}

QString controlTypeToString(ControlType t)
{
    switch (t) {
    case ControlType::Text:   return QStringLiteral("text");
    case ControlType::Color:  return QStringLiteral("color");
    case ControlType::Number: return QStringLiteral("number");
    case ControlType::Bool:   return QStringLiteral("bool");
    }
    return QStringLiteral("text");
}

ControlType controlTypeFromString(const QString &s)
{
    if (s == QStringLiteral("color"))  return ControlType::Color;
    if (s == QStringLiteral("number")) return ControlType::Number;
    if (s == QStringLiteral("bool"))   return ControlType::Bool;
    return ControlType::Text;
}

MogrtControl makeTextCtrl(const QString &key, const QString &label, const QString &val)
{
    MogrtControl c;
    c.key = key;
    c.label = label;
    c.type = ControlType::Text;
    c.textValue = val;
    return c;
}

MogrtControl makeColorCtrl(const QString &key, const QString &label, const QColor &col)
{
    MogrtControl c;
    c.key = key;
    c.label = label;
    c.type = ControlType::Color;
    c.colorValue = col;
    return c;
}

MogrtControl makeNumberCtrl(const QString &key, const QString &label, double val, double lo, double hi)
{
    MogrtControl c;
    c.key = key;
    c.label = label;
    c.type = ControlType::Number;
    c.numberValue = val;
    c.minValue = lo;
    c.maxValue = hi;
    return c;
}

MogrtControl makeBoolCtrl(const QString &key, const QString &label, bool val)
{
    MogrtControl c;
    c.key = key;
    c.label = label;
    c.type = ControlType::Bool;
    c.boolValue = val;
    return c;
}

} // namespace

QString MogrtTemplate::textParam(const QString &key, const QString &fallback) const
{
    for (const auto &c : controls)
        if (c.key == key) return c.textValue;
    return fallback;
}

QColor MogrtTemplate::colorParam(const QString &key, const QColor &fallback) const
{
    for (const auto &c : controls)
        if (c.key == key && c.colorValue.isValid()) return c.colorValue;
    return fallback;
}

double MogrtTemplate::numberParam(const QString &key, double fallback) const
{
    for (const auto &c : controls)
        if (c.key == key) return c.numberValue;
    return fallback;
}

bool MogrtTemplate::boolParam(const QString &key, bool fallback) const
{
    for (const auto &c : controls)
        if (c.key == key) return c.boolValue;
    return fallback;
}

void MogrtTemplate::setTextParam(const QString &key, const QString &val)
{
    for (auto &c : controls)
        if (c.key == key) { c.textValue = val; return; }
}

void MogrtTemplate::setColorParam(const QString &key, const QColor &val)
{
    for (auto &c : controls)
        if (c.key == key) { c.colorValue = val; return; }
}

void MogrtTemplate::setNumberParam(const QString &key, double val)
{
    for (auto &c : controls)
        if (c.key == key) { c.numberValue = qBound(c.minValue, val, c.maxValue); return; }
}

void MogrtTemplate::setBoolParam(const QString &key, bool val)
{
    for (auto &c : controls)
        if (c.key == key) { c.boolValue = val; return; }
}

QVector<MogrtTemplate> builtInTemplates()
{
    QVector<MogrtTemplate> out;

    {
        MogrtTemplate t;
        t.id = QStringLiteral("builtin-lower-third");
        t.name = QStringLiteral("モダンスライド・ローワーサード");
        t.category = QStringLiteral("Titles");
        t.style = TemplateStyle::LowerThird;
        t.durationSec = 5.0;
        t.controls = {
            makeTextCtrl(QStringLiteral("title"), QStringLiteral("メインタイトル"), QStringLiteral("Speaker Name")),
            makeTextCtrl(QStringLiteral("subtitle"), QStringLiteral("サブタイトル"), QStringLiteral("Creative Director")),
            makeColorCtrl(QStringLiteral("accent"), QStringLiteral("アクセント色"), QColor(59, 130, 246)),
            makeColorCtrl(QStringLiteral("bg"), QStringLiteral("背景色"), QColor(15, 23, 42, 225)),
            makeBoolCtrl(QStringLiteral("showBar"), QStringLiteral("縦バー表示"), true)
        };
        out.append(t);
    }
    {
        MogrtTemplate t;
        t.id = QStringLiteral("builtin-social-badge");
        t.name = QStringLiteral("SNS ハンドル・コールアウト");
        t.category = QStringLiteral("Social");
        t.style = TemplateStyle::SocialBadge;
        t.durationSec = 4.0;
        t.controls = {
            makeTextCtrl(QStringLiteral("handle"), QStringLiteral("アカウント名"), QStringLiteral("@creator_studio")),
            makeTextCtrl(QStringLiteral("platform"), QStringLiteral("プラットフォーム"), QStringLiteral("YouTube / X")),
            makeColorCtrl(QStringLiteral("accent"), QStringLiteral("バッジ色"), QColor(236, 72, 153))
        };
        out.append(t);
    }
    {
        MogrtTemplate t;
        t.id = QStringLiteral("builtin-countdown");
        t.name = QStringLiteral("カウントダウンタイマー");
        t.category = QStringLiteral("Utility");
        t.style = TemplateStyle::CountdownTimer;
        t.durationSec = 10.0;
        t.controls = {
            makeNumberCtrl(QStringLiteral("startSec"), QStringLiteral("開始秒数"), 10.0, 1.0, 3600.0),
            makeTextCtrl(QStringLiteral("label"), QStringLiteral("ラベル"), QStringLiteral("STARTING SOON")),
            makeColorCtrl(QStringLiteral("accent"), QStringLiteral("リング色"), QColor(34, 211, 238))
        };
        out.append(t);
    }
    {
        MogrtTemplate t;
        t.id = QStringLiteral("builtin-progress");
        t.name = QStringLiteral("チャプター進捗バー");
        t.category = QStringLiteral("Editorial");
        t.style = TemplateStyle::ProgressChapter;
        t.durationSec = 8.0;
        t.controls = {
            makeTextCtrl(QStringLiteral("chapter"), QStringLiteral("チャプター名"), QStringLiteral("01. Introduction")),
            makeColorCtrl(QStringLiteral("accent"), QStringLiteral("バー色"), QColor(250, 204, 21)),
            makeNumberCtrl(QStringLiteral("barHeight"), QStringLiteral("バー高さ (px)"), 12.0, 4.0, 48.0)
        };
        out.append(t);
    }
    {
        MogrtTemplate t;
        t.id = QStringLiteral("builtin-subscribe");
        t.name = QStringLiteral("チャンネル登録バナー");
        t.category = QStringLiteral("Social");
        t.style = TemplateStyle::SubscribeBanner;
        t.durationSec = 4.5;
        t.controls = {
            makeTextCtrl(QStringLiteral("cta"), QStringLiteral("ボタン文言"), QStringLiteral("SUBSCRIBE")),
            makeTextCtrl(QStringLiteral("channel"), QStringLiteral("チャンネル名"), QStringLiteral("V-Simple Studio")),
            makeColorCtrl(QStringLiteral("accent"), QStringLiteral("ボタン色"), QColor(239, 68, 68))
        };
        out.append(t);
    }
    return out;
}

MogrtTemplate templateById(const QString &id)
{
    for (const auto &t : builtInTemplates())
        if (t.id == id) return t;
    return builtInTemplates().first();
}

QJsonObject templateToJson(const MogrtTemplate &tpl)
{
    QJsonObject o;
    o[QStringLiteral("schema")] = QStringLiteral("veditor.mogrt.v1");
    o[QStringLiteral("id")] = tpl.id;
    o[QStringLiteral("name")] = tpl.name;
    o[QStringLiteral("category")] = tpl.category;
    o[QStringLiteral("style")] = styleToString(tpl.style);
    o[QStringLiteral("width")] = tpl.width;
    o[QStringLiteral("height")] = tpl.height;
    o[QStringLiteral("fps")] = tpl.fps;
    o[QStringLiteral("durationSec")] = tpl.durationSec;

    QJsonArray arr;
    for (const auto &c : tpl.controls) {
        QJsonObject co;
        co[QStringLiteral("key")] = c.key;
        co[QStringLiteral("label")] = c.label;
        co[QStringLiteral("type")] = controlTypeToString(c.type);
        if (c.type == ControlType::Text) {
            co[QStringLiteral("value")] = c.textValue;
        } else if (c.type == ControlType::Color) {
            co[QStringLiteral("value")] = c.colorValue.name(QColor::HexArgb);
        } else if (c.type == ControlType::Number) {
            co[QStringLiteral("value")] = c.numberValue;
            co[QStringLiteral("min")] = c.minValue;
            co[QStringLiteral("max")] = c.maxValue;
        } else if (c.type == ControlType::Bool) {
            co[QStringLiteral("value")] = c.boolValue;
        }
        arr.append(co);
    }
    o[QStringLiteral("controls")] = arr;
    return o;
}

MogrtTemplate templateFromJson(const QJsonObject &obj, bool *okOut)
{
    MogrtTemplate t;
    if (obj.isEmpty() || !obj.contains(QStringLiteral("controls"))) {
        if (okOut) *okOut = false;
        return t;
    }
    t.id = obj.value(QStringLiteral("id")).toString(QStringLiteral("custom-mogrt"));
    t.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Custom MOGRT"));
    t.category = obj.value(QStringLiteral("category")).toString(QStringLiteral("Custom"));
    t.style = styleFromString(obj.value(QStringLiteral("style")).toString());
    t.width = qBound(64, obj.value(QStringLiteral("width")).toInt(1920), 7680);
    t.height = qBound(64, obj.value(QStringLiteral("height")).toInt(1080), 4320);
    t.fps = qBound(1.0, obj.value(QStringLiteral("fps")).toDouble(30.0), 120.0);
    t.durationSec = qBound(0.5, obj.value(QStringLiteral("durationSec")).toDouble(5.0), 600.0);

    for (const QJsonValue &v : obj.value(QStringLiteral("controls")).toArray()) {
        if (!v.isObject()) continue;
        const QJsonObject co = v.toObject();
        MogrtControl c;
        c.key = co.value(QStringLiteral("key")).toString();
        c.label = co.value(QStringLiteral("label")).toString(c.key);
        c.type = controlTypeFromString(co.value(QStringLiteral("type")).toString());
        if (c.type == ControlType::Text) {
            c.textValue = co.value(QStringLiteral("value")).toString();
        } else if (c.type == ControlType::Color) {
            c.colorValue = QColor(co.value(QStringLiteral("value")).toString(QStringLiteral("#ffffffff")));
        } else if (c.type == ControlType::Number) {
            c.minValue = co.value(QStringLiteral("min")).toDouble(0.0);
            c.maxValue = co.value(QStringLiteral("max")).toDouble(1000.0);
            c.numberValue = qBound(c.minValue, co.value(QStringLiteral("value")).toDouble(0.0), c.maxValue);
        } else if (c.type == ControlType::Bool) {
            c.boolValue = co.value(QStringLiteral("value")).toBool(false);
        }
        if (!c.key.isEmpty())
            t.controls.append(c);
    }
    if (okOut) *okOut = true;
    return t;
}

bool exportMogrtArchive(const MogrtTemplate &tpl, const QString &filePath, QString *errorOut)
{
    const QByteArray defJson = QJsonDocument(templateToJson(tpl)).toJson(QJsonDocument::Indented);
    const QByteArray entryName("definition.json");
    const quint32 crc = crc32Bytes(defJson);
    const quint32 sz = static_cast<quint32>(defJson.size());

    // Build standard PKZIP (Method 0 = Store) archive containing definition.json
    QByteArray zip;
    const quint32 localOffset = 0;
    // Local file header (PK\x03\x04)
    putU32Le(zip, 0x04034B50u);
    putU16Le(zip, 20); // version needed
    putU16Le(zip, 0);  // flags
    putU16Le(zip, 0);  // method = store
    putU16Le(zip, 0);  // mod time
    putU16Le(zip, 0);  // mod date
    putU32Le(zip, crc);
    putU32Le(zip, sz);
    putU32Le(zip, sz);
    putU16Le(zip, static_cast<quint16>(entryName.size()));
    putU16Le(zip, 0);  // extra len
    zip.append(entryName);
    zip.append(defJson);

    const quint32 cdOffset = static_cast<quint32>(zip.size());
    // Central directory header (PK\x01\x02)
    putU32Le(zip, 0x02014B50u);
    putU16Le(zip, 20);
    putU16Le(zip, 20);
    putU16Le(zip, 0);
    putU16Le(zip, 0);
    putU16Le(zip, 0);
    putU16Le(zip, 0);
    putU32Le(zip, crc);
    putU32Le(zip, sz);
    putU32Le(zip, sz);
    putU16Le(zip, static_cast<quint16>(entryName.size()));
    putU16Le(zip, 0);
    putU16Le(zip, 0);
    putU16Le(zip, 0);
    putU16Le(zip, 0);
    putU32Le(zip, 0);
    putU32Le(zip, localOffset);
    zip.append(entryName);

    const quint32 cdSize = static_cast<quint32>(zip.size()) - cdOffset;
    // End of central directory (PK\x05\x06)
    putU32Le(zip, 0x06054B50u);
    putU16Le(zip, 0);
    putU16Le(zip, 0);
    putU16Le(zip, 1);
    putU16Le(zip, 1);
    putU32Le(zip, cdSize);
    putU32Le(zip, cdOffset);
    putU16Le(zip, 0);

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("MOGRT ファイルを書き込めません: %1").arg(filePath);
        return false;
    }
    return f.write(zip) == zip.size();
}

MogrtTemplate importMogrtArchive(const QString &filePath, bool *okOut, QString *errorOut)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (okOut) *okOut = false;
        if (errorOut) *errorOut = QStringLiteral("MOGRT ファイルを開けません: %1").arg(filePath);
        return {};
    }
    const QByteArray raw = f.readAll();
    QByteArray jsonPayload;

    if (raw.size() >= 30 && raw[0] == 'P' && raw[1] == 'K' && raw[2] == 3 && raw[3] == 4) {
        // Walk PKZIP local file headers to locate definition.json (or first .json)
        int pos = 0;
        while (pos + 30 <= raw.size()) {
            const uchar *p = reinterpret_cast<const uchar *>(raw.constData() + pos);
            if (getU32Le(p) != 0x04034B50u)
                break;
            const quint16 method = getU16Le(p + 8);
            const quint32 storedCrc = getU32Le(p + 14);
            const quint32 compSz = getU32Le(p + 18);
            const quint16 nameLen = getU16Le(p + 26);
            const quint16 extraLen = getU16Le(p + 28);
            const int dataPos = pos + 30 + nameLen + extraLen;
            if (dataPos + static_cast<int>(compSz) > raw.size())
                break;
            const QString name = QString::fromUtf8(raw.constData() + pos + 30, nameLen);
            if (method == 0 && name.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
                const QByteArray payload = raw.mid(dataPos, static_cast<int>(compSz));
                if (storedCrc != 0 && crc32Bytes(payload) != storedCrc) {
                    if (okOut) *okOut = false;
                    if (errorOut) *errorOut = QStringLiteral("MOGRT アーカイブの CRC-32 が一致しません。");
                    return {};
                }
                jsonPayload = payload;
                if (name.compare(QStringLiteral("definition.json"), Qt::CaseInsensitive) == 0)
                    break;
            }
            pos = dataPos + static_cast<int>(compSz);
        }
    } else {
        // Standalone JSON fallback (.mogrt.json)
        jsonPayload = raw;
    }

    if (jsonPayload.isEmpty()) {
        if (okOut) *okOut = false;
        if (errorOut) *errorOut = QStringLiteral("MOGRT 内に definition.json が見つかりません。");
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(jsonPayload);
    if (!doc.isObject()) {
        if (okOut) *okOut = false;
        if (errorOut) *errorOut = QStringLiteral("MOGRT definition.json が不正です。");
        return {};
    }
    bool ok = false;
    const MogrtTemplate tpl = templateFromJson(doc.object(), &ok);
    if (okOut) *okOut = ok;
    if (!ok && errorOut)
        *errorOut = QStringLiteral("MOGRT テンプレート形式が不正です。");
    return tpl;
}

QImage renderMogrtFrame(const MogrtTemplate &tpl, double timeSec, QSize outSize)
{
    const QSize sz = (outSize.isValid() && !outSize.isEmpty()) ? outSize : QSize(640, 360);
    QImage img(sz, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);

    const double W = sz.width();
    const double H = sz.height();
    const double dur = qMax(0.5, tpl.durationSec);
    const double t = qBound(0.0, timeSec, dur);
    const double enter = easeOutCubic(t / qMin(0.65, dur * 0.35));
    const double exitStart = qMax(0.0, dur - 0.45);
    const double exitAlpha = (t > exitStart) ? qBound(0.0, 1.0 - (t - exitStart) / 0.45, 1.0) : 1.0;

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setOpacity(exitAlpha);

    const QColor accent = tpl.colorParam(QStringLiteral("accent"), QColor(59, 130, 246));

    if (tpl.style == TemplateStyle::LowerThird) {
        const QColor bg = tpl.colorParam(QStringLiteral("bg"), QColor(15, 23, 42, 225));
        const QString title = tpl.textParam(QStringLiteral("title"), QStringLiteral("Title"));
        const QString sub = tpl.textParam(QStringLiteral("subtitle"), QStringLiteral("Subtitle"));
        const bool showBar = tpl.boolParam(QStringLiteral("showBar"), true);

        const double boxW = W * 0.52;
        const double boxH = H * 0.22;
        const double slideX = W * 0.06 - (1.0 - enter) * W * 0.22;
        const double boxY = H * 0.70;
        const QRectF box(slideX, boxY, boxW, boxH);

        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(box, 8.0, 8.0);
        if (showBar) {
            p.setBrush(accent);
            p.drawRoundedRect(QRectF(box.left(), box.top(), qMax(5.0, W * 0.012), box.height()), 3.0, 3.0);
        }

        QFont tf = p.font();
        tf.setBold(true);
        tf.setPixelSize(qBound(12, static_cast<int>(boxH * 0.36), 54));
        p.setFont(tf);
        p.setPen(Qt::white);
        p.drawText(QRectF(box.left() + W * 0.03, box.top() + boxH * 0.12, boxW - W * 0.04, boxH * 0.45),
                   Qt::AlignLeft | Qt::AlignVCenter, title);

        QFont sf = p.font();
        sf.setBold(false);
        sf.setPixelSize(qBound(10, static_cast<int>(boxH * 0.24), 34));
        p.setFont(sf);
        p.setPen(QColor(203, 213, 225));
        p.drawText(QRectF(box.left() + W * 0.03, box.top() + boxH * 0.56, boxW - W * 0.04, boxH * 0.34),
                   Qt::AlignLeft | Qt::AlignVCenter, sub);
    } else if (tpl.style == TemplateStyle::SocialBadge) {
        const QString handle = tpl.textParam(QStringLiteral("handle"), QStringLiteral("@user"));
        const QString platform = tpl.textParam(QStringLiteral("platform"), QStringLiteral("Social"));
        const double pillW = W * 0.42 * enter;
        const double pillH = H * 0.14;
        const QRectF pill(W * 0.05, H * 0.78, qMax(12.0, pillW), pillH);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(pill, pillH * 0.5, pillH * 0.5);

        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(qBound(11, static_cast<int>(pillH * 0.38), 40));
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(pill.adjusted(16, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("%1  •  %2").arg(platform, handle));
    } else if (tpl.style == TemplateStyle::CountdownTimer) {
        const double startSec = qMax(1.0, tpl.numberParam(QStringLiteral("startSec"), 10.0));
        const double remaining = qMax(0.0, startSec - t);
        const QString label = tpl.textParam(QStringLiteral("label"), QStringLiteral("COUNTDOWN"));
        const double radius = qMin(W, H) * 0.26;
        const QPointF c(W * 0.5, H * 0.48);

        p.setPen(QPen(QColor(255, 255, 255, 50), qMax(4.0, radius * 0.12)));
        p.setBrush(QColor(15, 23, 42, 190));
        p.drawEllipse(c, radius, radius);

        const double frac = qBound(0.0, remaining / startSec, 1.0);
        p.setPen(QPen(accent, qMax(4.0, radius * 0.12), Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(c.x() - radius, c.y() - radius, radius * 2.0, radius * 2.0),
                  90 * 16, static_cast<int>(-360.0 * frac * 16.0));

        QFont nf = p.font();
        nf.setBold(true);
        nf.setPixelSize(qBound(16, static_cast<int>(radius * 0.65), 96));
        p.setFont(nf);
        p.setPen(Qt::white);
        const int secs = static_cast<int>(std::ceil(remaining));
        p.drawText(QRectF(c.x() - radius, c.y() - radius * 0.5, radius * 2.0, radius),
                   Qt::AlignCenter, QString::number(secs));

        QFont lf = p.font();
        lf.setPixelSize(qBound(10, static_cast<int>(H * 0.055), 32));
        p.setFont(lf);
        p.setPen(accent);
        p.drawText(QRectF(0, c.y() + radius + 8, W, H * 0.12), Qt::AlignCenter, label);
    } else if (tpl.style == TemplateStyle::ProgressChapter) {
        const QString chapter = tpl.textParam(QStringLiteral("chapter"), QStringLiteral("Chapter 1"));
        const double barH = qBound(4.0, tpl.numberParam(QStringLiteral("barHeight"), 12.0) * (H / 720.0), H * 0.1);
        const double prog = qBound(0.0, t / dur, 1.0);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(15, 23, 42, 200));
        p.drawRect(QRectF(0.0, H - barH * 3.2, W, barH * 3.2));
        p.setBrush(accent);
        p.drawRect(QRectF(0.0, H - barH, W * prog, barH));

        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(qBound(10, static_cast<int>(barH * 1.4), 34));
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRectF(W * 0.03, H - barH * 3.1, W * 0.94, barH * 2.0),
                   Qt::AlignLeft | Qt::AlignVCenter, chapter);
    } else if (tpl.style == TemplateStyle::SubscribeBanner) {
        const QString cta = tpl.textParam(QStringLiteral("cta"), QStringLiteral("SUBSCRIBE"));
        const QString chan = tpl.textParam(QStringLiteral("channel"), QStringLiteral("Channel"));
        const double pulse = 1.0 + 0.04 * std::sin(t * 6.0);
        const double cardW = W * 0.56;
        const double cardH = H * 0.18;
        const double yOff = H * 0.74 + (1.0 - enter) * H * 0.28;
        const QRectF card((W - cardW) * 0.5, yOff, cardW, cardH);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(18, 22, 32, 235));
        p.drawRoundedRect(card, 12.0, 12.0);

        const double btnW = cardW * 0.34 * pulse;
        const double btnH = cardH * 0.62 * pulse;
        const QRectF btn(card.right() - btnW - cardH * 0.22,
                         card.center().y() - btnH * 0.5, btnW, btnH);
        p.setBrush(accent);
        p.drawRoundedRect(btn, 8.0, 8.0);

        QFont cf = p.font();
        cf.setBold(true);
        cf.setPixelSize(qBound(11, static_cast<int>(cardH * 0.34), 40));
        p.setFont(cf);
        p.setPen(Qt::white);
        p.drawText(QRectF(card.left() + 18, card.top(), cardW * 0.56, cardH),
                   Qt::AlignLeft | Qt::AlignVCenter, chan);
        p.drawText(btn, Qt::AlignCenter, cta);
    }

    return img;
}

QImage renderMogrtFromParams(const QJsonObject &params, double timeSec, QSize outSize)
{
    bool ok = false;
    MogrtTemplate tpl = templateFromJson(params, &ok);
    if (!ok)
        tpl = builtInTemplates().first();
    return renderMogrtFrame(tpl, timeSec, outSize);
}

QString buildMogrtGeneratorUri(const MogrtTemplate &tpl)
{
    extgen::GeneratorSpec spec;
    spec.renderer = QStringLiteral("mogrt");
    spec.params = templateToJson(tpl);
    spec.fps = tpl.fps;
    return extgen::buildGeneratorUri(spec);
}

bool insertMogrtClip(Timeline *timeline,
                     const MogrtTemplate &tpl,
                     int videoTrackIdx,
                     QString *errorOut)
{
    if (!timeline) {
        if (errorOut) *errorOut = QStringLiteral("タイムラインが無効です。");
        return false;
    }
    const auto tracks = timeline->videoTracks();
    if (videoTrackIdx < 0 || videoTrackIdx >= tracks.size() || !tracks[videoTrackIdx]) {
        if (errorOut) *errorOut = QStringLiteral("挿入先ビデオトラックがありません。");
        return false;
    }
    TimelineTrack *tr = tracks[videoTrackIdx];
    if (tr->isLocked()) {
        if (errorOut) *errorOut = QStringLiteral("対象トラックがロックされています。");
        return false;
    }

    const double dur = qMax(0.5, tpl.durationSec);
    ClipInfo clip;
    clip.filePath = buildMogrtGeneratorUri(tpl);
    clip.displayName = QStringLiteral("MOGRT: %1").arg(tpl.name);
    clip.duration = dur;
    clip.inPoint = 0.0;
    clip.outPoint = dur;
    tr->addClip(clip);
    timeline->saveUndoState(QStringLiteral("MOGRT テンプレートを挿入 (%1)").arg(tpl.name));
    return true;
}

// ============================================================================
// MogrtBrowserDialog
// ============================================================================

MogrtBrowserDialog::MogrtBrowserDialog(Timeline *timeline, QWidget *parent)
    : QDialog(parent)
    , m_timeline(timeline)
    , m_templates(builtInTemplates())
{
    setWindowTitle(QStringLiteral("モーショングラフィックス・テンプレート (MOGRT)"));
    resize(620, 520);

    auto *root = new QVBoxLayout(this);
    auto *topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel(QStringLiteral("テンプレート:"), this));
    m_templateCombo = new QComboBox(this);
    for (const auto &t : m_templates)
        m_templateCombo->addItem(QStringLiteral("[%1] %2").arg(t.category, t.name), t.id);
    topRow->addWidget(m_templateCombo, 1);

    auto *importBtn = new QPushButton(QStringLiteral(".mogrt 読込…"), this);
    auto *exportBtn = new QPushButton(QStringLiteral(".mogrt 保存…"), this);
    topRow->addWidget(importBtn);
    topRow->addWidget(exportBtn);
    root->addLayout(topRow);

    m_previewLabel = new QLabel(this);
    m_previewLabel->setMinimumSize(420, 200);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setStyleSheet(QStringLiteral("background: #141821; border: 1px solid #353945;"));
    root->addWidget(m_previewLabel);

    m_controlsHost = new QWidget(this);
    m_controlsForm = new QFormLayout(m_controlsHost);
    root->addWidget(m_controlsHost, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    if (QPushButton *okBtn = buttons->button(QDialogButtonBox::Ok))
        okBtn->setText(QStringLiteral("タイムラインへ挿入"));
    root->addWidget(buttons);

    connect(m_templateCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MogrtBrowserDialog::selectTemplateIndex);
    connect(importBtn, &QPushButton::clicked, this, &MogrtBrowserDialog::onImportFile);
    connect(exportBtn, &QPushButton::clicked, this, &MogrtBrowserDialog::onExportFile);
    connect(buttons, &QDialogButtonBox::accepted, this, &MogrtBrowserDialog::onInsertAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    selectTemplateIndex(0);
}

int MogrtBrowserDialog::templateCount() const
{
    return m_templates.size();
}

void MogrtBrowserDialog::selectTemplateIndex(int idx)
{
    if (idx < 0 || idx >= m_templates.size())
        return;
    m_current = m_templates[idx];
    rebuildControlsForm();
    updatePreview();
}

void MogrtBrowserDialog::rebuildControlsForm()
{
    while (m_controlsForm->rowCount() > 0)
        m_controlsForm->removeRow(0);

    for (const MogrtControl &ctrl : m_current.controls) {
        const QString key = ctrl.key;
        if (ctrl.type == ControlType::Text) {
            auto *edit = new QLineEdit(ctrl.textValue, m_controlsHost);
            connect(edit, &QLineEdit::textChanged, this, [this, key](const QString &txt) {
                m_current.setTextParam(key, txt);
                updatePreview();
            });
            m_controlsForm->addRow(ctrl.label + QStringLiteral(":"), edit);
        } else if (ctrl.type == ControlType::Number) {
            auto *spin = new QDoubleSpinBox(m_controlsHost);
            spin->setRange(ctrl.minValue, ctrl.maxValue);
            spin->setValue(ctrl.numberValue);
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, key](double v) {
                m_current.setNumberParam(key, v);
                updatePreview();
            });
            m_controlsForm->addRow(ctrl.label + QStringLiteral(":"), spin);
        } else if (ctrl.type == ControlType::Bool) {
            auto *chk = new QCheckBox(ctrl.label, m_controlsHost);
            chk->setChecked(ctrl.boolValue);
            connect(chk, &QCheckBox::toggled, this, [this, key](bool v) {
                m_current.setBoolParam(key, v);
                updatePreview();
            });
            m_controlsForm->addRow(chk);
        } else if (ctrl.type == ControlType::Color) {
            auto *btn = new QPushButton(ctrl.colorValue.name(QColor::HexArgb), m_controlsHost);
            connect(btn, &QPushButton::clicked, this, [this, key, btn]() {
                const QColor picked = QColorDialog::getColor(
                    m_current.colorParam(key), this, QStringLiteral("色を選択"),
                    QColorDialog::ShowAlphaChannel);
                if (picked.isValid()) {
                    m_current.setColorParam(key, picked);
                    btn->setText(picked.name(QColor::HexArgb));
                    updatePreview();
                }
            });
            m_controlsForm->addRow(ctrl.label + QStringLiteral(":"), btn);
        }
    }
}

void MogrtBrowserDialog::updatePreview()
{
    const QImage img = renderMogrtFrame(m_current, qMin(1.2, m_current.durationSec * 0.4), QSize(420, 200));
    m_previewLabel->setPixmap(QPixmap::fromImage(img));
}

void MogrtBrowserDialog::onImportFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("MOGRT テンプレートを読み込む"), QString(),
        QStringLiteral("Motion Graphics Template (*.mogrt *.json);;All Files (*)"));
    if (path.isEmpty())
        return;
    bool ok = false;
    QString err;
    const MogrtTemplate loaded = importMogrtArchive(path, &ok, &err);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("MOGRT 読込失敗"), err);
        return;
    }
    m_templates.append(loaded);
    m_templateCombo->addItem(QStringLiteral("[%1] %2").arg(loaded.category, loaded.name), loaded.id);
    m_templateCombo->setCurrentIndex(m_templates.size() - 1);
}

void MogrtBrowserDialog::onExportFile()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("MOGRT テンプレートを保存"),
        QDir::homePath() + QDir::separator() + m_current.id + QStringLiteral(".mogrt"),
        QStringLiteral("Motion Graphics Template (*.mogrt);;All Files (*)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!exportMogrtArchive(m_current, path, &err)) {
        QMessageBox::warning(this, QStringLiteral("MOGRT 保存失敗"), err);
    }
}

void MogrtBrowserDialog::onInsertAndAccept()
{
    QString err;
    if (!insertMogrtClip(m_timeline, m_current, 0, &err)) {
        QMessageBox::warning(this, QStringLiteral("MOGRT 挿入失敗"), err);
        return;
    }
    accept();
}

} // namespace mogrt
