#include "ExternalGenerator.h"
#include "CreatorTools.h"
#include "ExternalGeneratorEdge.h"
#include "Timeline.h"
#include "libavcore/Encode.h"

#include <QCache>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
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
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>

namespace extgen {

namespace {

constexpr int kMaxPreviewWidth = 640;
constexpr int kMaxPreviewHeight = 360;
constexpr int kMemoryCacheMaxFrames = 64;

QMutex g_cacheMutex;
QCache<QString, QImage> g_memoryCache(kMemoryCacheMaxFrames);

QMutex g_testRendererMutex;
QHash<QString, TestRendererFn> g_testRenderers;

thread_local bool tl_previewModeHint = false;

QString normalizePathKey(const QString &path)
{
    if (path.trimmed().isEmpty())
        return {};
    const QFileInfo fi(path);
    QString dirPath = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    dirPath = QDir::cleanPath(dirPath);
#ifdef Q_OS_WIN
    return dirPath.toLower();
#else
    return dirPath;
#endif
}

QSize scaledPreviewSize(QSize outSize)
{
    if (!outSize.isValid() || outSize.isEmpty())
        return QSize(320, 180);
    if (outSize.width() <= kMaxPreviewWidth && outSize.height() <= kMaxPreviewHeight)
        return outSize;
    return outSize.scaled(kMaxPreviewWidth, kMaxPreviewHeight, Qt::KeepAspectRatio);
}

QImage stampCornerBadge(const QImage &base, const QString &badgeText, const QColor &bgColor)
{
    if (base.isNull())
        return {};
    QImage out = base.convertToFormat(QImage::Format_RGBA8888);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f = p.font();
    f.setPixelSize(qBound(11, out.height() / 28, 22));
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    const QRect textRect = fm.boundingRect(badgeText);
    const int padX = 10;
    const int padY = 5;
    const QRect box(12, 12, textRect.width() + padX * 2, textRect.height() + padY * 2);
    p.setBrush(bgColor);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(box, 5, 5);
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, badgeText);
    return out;
}

quint32 readU32Le(const uchar *p)
{
    return static_cast<quint32>(p[0])
         | (static_cast<quint32>(p[1]) << 8)
         | (static_cast<quint32>(p[2]) << 16)
         | (static_cast<quint32>(p[3]) << 24);
}

void writeU32Le(uchar *p, quint32 v)
{
    p[0] = static_cast<uchar>(v & 0xFFu);
    p[1] = static_cast<uchar>((v >> 8) & 0xFFu);
    p[2] = static_cast<uchar>((v >> 16) & 0xFFu);
    p[3] = static_cast<uchar>((v >> 24) & 0xFFu);
}

PluginManifest loadManifestFile(const QString &manifestPath)
{
    PluginManifest m;
    QFile f(manifestPath);
    if (!f.open(QIODevice::ReadOnly))
        return m;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return m;
    const QJsonObject o = doc.object();
    m.id = o.value(QStringLiteral("id")).toString().trimmed();
    m.name = o.value(QStringLiteral("name")).toString().trimmed();
    if (m.name.isEmpty())
        m.name = m.id;
    m.protocol = o.value(QStringLiteral("protocol")).toString(QStringLiteral("stdio-rgba")).trimmed();
    m.manifestDir = QFileInfo(manifestPath).absolutePath();
    const QString exeRel = o.value(QStringLiteral("executable")).toString().trimmed();
    if (!exeRel.isEmpty()) {
        const QFileInfo exeInfo(QDir(m.manifestDir), exeRel);
        m.executablePath = exeInfo.absoluteFilePath();
    }
    for (const QJsonValue &v : o.value(QStringLiteral("args")).toArray())
        m.extraArgs.append(v.toString());
    for (const QJsonValue &v : o.value(QStringLiteral("extensions")).toArray())
        m.extensions.append(v.toString());
    m.valid = !m.id.isEmpty() && !m.executablePath.isEmpty() && QFileInfo::exists(m.executablePath);
    return m;
}

} // namespace

bool isExternalGeneratorFilePath(const QString &filePath)
{
    return filePath.startsWith(QLatin1String(kUriPrefix));
}

GeneratorSpec parseGeneratorUri(const QString &uri)
{
    GeneratorSpec spec;
    if (!isExternalGeneratorFilePath(uri))
        return spec;
    const QUrl url(uri);
    const QUrlQuery q(url);
    spec.renderer = q.queryItemValue(QStringLiteral("renderer"), QUrl::FullyDecoded).trimmed();
    if (spec.renderer.isEmpty())
        spec.renderer = QStringLiteral("dummy");
    spec.entryPath = q.queryItemValue(QStringLiteral("entry"), QUrl::FullyDecoded);
    const QString paramsRaw = q.queryItemValue(QStringLiteral("params"), QUrl::FullyDecoded);
    if (!paramsRaw.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(paramsRaw.toUtf8());
        if (doc.isObject())
            spec.params = doc.object();
    }
    const QString fpsStr = q.queryItemValue(QStringLiteral("fps"), QUrl::FullyDecoded);
    bool fpsOk = false;
    const double fpsVal = fpsStr.toDouble(&fpsOk);
    spec.fps = (fpsOk && std::isfinite(fpsVal) && fpsVal > 0.0) ? qBound(1.0, fpsVal, 240.0) : 30.0;
    spec.cacheDir = q.queryItemValue(QStringLiteral("cache"), QUrl::FullyDecoded);
    return spec;
}

QString buildGeneratorUri(const GeneratorSpec &spec)
{
    QUrl url(QString::fromLatin1(kUriPrefix));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("renderer"),
                   spec.renderer.trimmed().isEmpty() ? QStringLiteral("dummy") : spec.renderer.trimmed());
    if (!spec.entryPath.isEmpty())
        q.addQueryItem(QStringLiteral("entry"), spec.entryPath);
    if (!spec.params.isEmpty()) {
        const QString json = QString::fromUtf8(
            QJsonDocument(spec.params).toJson(QJsonDocument::Compact));
        q.addQueryItem(QStringLiteral("params"), json);
    }
    const double fps = (std::isfinite(spec.fps) && spec.fps > 0.0) ? qBound(1.0, spec.fps, 240.0) : 30.0;
    q.addQueryItem(QStringLiteral("fps"), QString::number(fps, 'g', 6));
    if (!spec.cacheDir.isEmpty())
        q.addQueryItem(QStringLiteral("cache"), spec.cacheDir);
    url.setQuery(q);
    return url.toString(QUrl::FullyEncoded);
}

QString computeSpecHash(const GeneratorSpec &spec)
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(spec.renderer.toUtf8());
    h.addData("\0", 1);
    h.addData(spec.entryPath.toUtf8());
    h.addData("\0", 1);
    if (!spec.entryPath.isEmpty() && QFileInfo::exists(spec.entryPath)) {
        const qint64 mtime = QFileInfo(spec.entryPath).lastModified().toMSecsSinceEpoch();
        h.addData(QByteArray::number(mtime));
    }
    h.addData("\0", 1);
    h.addData(QJsonDocument(spec.params).toJson(QJsonDocument::Compact));
    h.addData("\0", 1);
    h.addData(QByteArray::number(spec.fps, 'f', 4));
    return QString::fromLatin1(h.result().toHex().left(16));
}

QString canonicalTrustFolderForEntry(const QString &entryPath)
{
    return normalizePathKey(entryPath);
}

bool isFolderTrusted(const QString &entryPath)
{
    const QString key = normalizePathKey(entryPath);
    if (key.isEmpty())
        return false;
    const QStringList list = trustedFolders();
    for (const QString &trusted : list) {
        const QString normTrusted = normalizePathKey(trusted);
        if (normTrusted.isEmpty())
            continue;
        if (key == normTrusted || key.startsWith(normTrusted + QLatin1Char('/')))
            return true;
    }
    return false;
}

void trustFolder(const QString &entryPath)
{
    const QString key = normalizePathKey(entryPath);
    if (key.isEmpty())
        return;
    QSettings s;
    s.beginGroup(QStringLiteral("ExternalGenerator"));
    QStringList list = s.value(QStringLiteral("TrustedFolders")).toStringList();
    bool exists = false;
    for (const QString &item : list) {
        if (normalizePathKey(item) == key) {
            exists = true;
            break;
        }
    }
    if (!exists) {
        list.append(key);
        s.setValue(QStringLiteral("TrustedFolders"), list);
        s.sync();
    }
    s.endGroup();
}

void revokeFolderTrust(const QString &entryPath)
{
    const QString key = normalizePathKey(entryPath);
    if (key.isEmpty())
        return;
    QSettings s;
    s.beginGroup(QStringLiteral("ExternalGenerator"));
    QStringList list = s.value(QStringLiteral("TrustedFolders")).toStringList();
    QStringList kept;
    for (const QString &item : list) {
        if (normalizePathKey(item) != key)
            kept.append(item);
    }
    s.setValue(QStringLiteral("TrustedFolders"), kept);
    s.sync();
    s.endGroup();
}

void clearTrustedFolders()
{
    QSettings s;
    s.beginGroup(QStringLiteral("ExternalGenerator"));
    s.remove(QStringLiteral("TrustedFolders"));
    s.sync();
    s.endGroup();
}

QStringList trustedFolders()
{
    QSettings s;
    s.beginGroup(QStringLiteral("ExternalGenerator"));
    const QStringList list = s.value(QStringLiteral("TrustedFolders")).toStringList();
    s.endGroup();
    return list;
}

bool ensureEntryTrustedInteractive(const QString &entryPath, QWidget *parent)
{
    if (entryPath.trimmed().isEmpty())
        return true;
    if (isFolderTrusted(entryPath))
        return true;
    const QString folder = canonicalTrustFolderForEntry(entryPath);
    const auto answer = QMessageBox::question(
        parent,
        QStringLiteral("外部ジェネレータの実行許可"),
        QStringLiteral("次のフォルダ内の外部ジェネレータスクリプト／HTML を実行します。\n\n%1\n\n"
                       "信頼できるプロジェクトのフォルダのみ許可してください。\nこのフォルダを許可しますか？")
            .arg(folder),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer == QMessageBox::Yes) {
        trustFolder(entryPath);
        return true;
    }
    return false;
}

QStringList pluginSearchDirs()
{
    QStringList dirs;
    const QString envDir = qEnvironmentVariable("VEDITOR_GENERATOR_PLUGIN_DIR").trimmed();
    if (!envDir.isEmpty())
        dirs << QDir::cleanPath(envDir);
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty())
        dirs << QDir(appDir).filePath(QStringLiteral("plugins/generators"));
    const QString localData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!localData.isEmpty())
        dirs << QDir(localData).filePath(QStringLiteral("plugins/generators"));
    return dirs;
}

QVector<PluginManifest> discoverPlugins()
{
    QVector<PluginManifest> found;
    QSet<QString> seenIds;
    for (const QString &root : pluginSearchDirs()) {
        QDir dir(root);
        if (!dir.exists())
            continue;
        // Direct generator.json inside root
        if (dir.exists(QStringLiteral("generator.json"))) {
            const PluginManifest m = loadManifestFile(dir.filePath(QStringLiteral("generator.json")));
            if (m.valid && !seenIds.contains(m.id)) {
                seenIds.insert(m.id);
                found.append(m);
            }
        }
        // Subdirectory per plugin: <root>/<pluginId>/generator.json
        const QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &sub : subdirs) {
            const QString manifestPath = dir.filePath(sub + QStringLiteral("/generator.json"));
            if (!QFileInfo::exists(manifestPath))
                continue;
            const PluginManifest m = loadManifestFile(manifestPath);
            if (m.valid && !seenIds.contains(m.id)) {
                seenIds.insert(m.id);
                found.append(m);
            }
        }
    }
    return found;
}

PluginManifest findPlugin(const QString &rendererId)
{
    const QVector<PluginManifest> plugins = discoverPlugins();
    for (const auto &p : plugins) {
        if (p.id.compare(rendererId, Qt::CaseInsensitive) == 0)
            return p;
    }
    return {};
}

bool isRendererAvailable(const QString &rendererId)
{
    if (rendererId == QStringLiteral("dummy") || rendererId == QStringLiteral("visualizer"))
        return true;
    {
        QMutexLocker lock(&g_testRendererMutex);
        if (g_testRenderers.contains(rendererId))
            return true;
    }
    if (rendererId == QStringLiteral("edge"))
        return !edge::findBrowserExecutable().isEmpty();
    return findPlugin(rendererId).valid;
}

QByteArray encodeVgenFrame(const QImage &image)
{
    if (image.isNull())
        return {};
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    const quint32 w = static_cast<quint32>(rgba.width());
    const quint32 h = static_cast<quint32>(rgba.height());
    const quint32 rowBytes = w * 4u;
    const quint32 byteLen = rowBytes * h;
    QByteArray out(16 + static_cast<int>(byteLen), Qt::Uninitialized);
    uchar *dst = reinterpret_cast<uchar *>(out.data());
    dst[0] = 'V'; dst[1] = 'G'; dst[2] = 'E'; dst[3] = 'N';
    writeU32Le(dst + 4, w);
    writeU32Le(dst + 8, h);
    writeU32Le(dst + 12, byteLen);
    uchar *pixels = dst + 16;
    for (quint32 y = 0; y < h; ++y) {
        std::memcpy(pixels + y * rowBytes, rgba.constScanLine(static_cast<int>(y)), rowBytes);
    }
    return out;
}

bool decodeVgenFrame(const QByteArray &data, QImage *outImage)
{
    if (data.size() < 16)
        return false;
    const uchar *src = reinterpret_cast<const uchar *>(data.constData());
    if (src[0] != 'V' || src[1] != 'G' || src[2] != 'E' || src[3] != 'N')
        return false;
    const quint32 w = readU32Le(src + 4);
    const quint32 h = readU32Le(src + 8);
    const quint32 byteLen = readU32Le(src + 12);
    if (w == 0 || h == 0 || w > 16384 || h > 16384)
        return false;
    const quint64 expected = static_cast<quint64>(w) * static_cast<quint64>(h) * 4ull;
    if (byteLen != expected || static_cast<quint64>(data.size() - 16) < expected)
        return false;
    QImage img(static_cast<int>(w), static_cast<int>(h), QImage::Format_RGBA8888);
    const quint32 rowBytes = w * 4u;
    const uchar *pixels = src + 16;
    for (quint32 y = 0; y < h; ++y) {
        std::memcpy(img.scanLine(static_cast<int>(y)), pixels + y * rowBytes, rowBytes);
    }
    if (outImage)
        *outImage = img;
    return true;
}

RenderOutcome renderViaSubprocess(const PluginManifest &plugin,
                                  const GeneratorSpec &spec,
                                  int frameIndex,
                                  QSize size,
                                  int timeoutMs)
{
    RenderOutcome out;
    if (!plugin.valid || !QFileInfo::exists(plugin.executablePath)) {
        out.missingRenderer = true;
        out.statusMessage = QStringLiteral("ジェネレータ未導入 (%1)").arg(spec.renderer);
        return out;
    }

    QProcess proc;
    proc.setProgram(plugin.executablePath);
    proc.setWorkingDirectory(plugin.manifestDir);

    if (plugin.protocol == QStringLiteral("cli-png")) {
        QTemporaryFile tmpPng(QDir::tempPath() + QStringLiteral("/veditor_extgen_XXXXXX.png"));
        if (!tmpPng.open()) {
            out.statusMessage = QStringLiteral("一時 PNG ファイル作成に失敗しました");
            return out;
        }
        const QString pngPath = tmpPng.fileName();
        tmpPng.close();

        QStringList args = plugin.extraArgs;
        args << QStringLiteral("--render-frame")
             << QStringLiteral("--entry") << spec.entryPath
             << QStringLiteral("--frame") << QString::number(frameIndex)
             << QStringLiteral("--fps") << QString::number(spec.fps, 'f', 4)
             << QStringLiteral("--width") << QString::number(size.width())
             << QStringLiteral("--height") << QString::number(size.height())
             << QStringLiteral("--params")
             << QString::fromUtf8(QJsonDocument(spec.params).toJson(QJsonDocument::Compact))
             << QStringLiteral("--out") << pngPath;
        proc.setArguments(args);
        proc.start();
        if (!proc.waitForFinished(timeoutMs)) {
            proc.kill();
            proc.waitForFinished(1500);
            out.statusMessage = QStringLiteral("外部ジェネレータがタイムアウトしました (%1 ms)").arg(timeoutMs);
            return out;
        }
        if (proc.exitCode() != 0) {
            out.statusMessage = QStringLiteral("外部ジェネレータ終了コード %1").arg(proc.exitCode());
            return out;
        }
        QImage loaded(pngPath);
        if (loaded.isNull()) {
            out.statusMessage = QStringLiteral("外部ジェネレータ出力 PNG の読込に失敗しました");
            return out;
        }
        if (loaded.size() != size)
            loaded = loaded.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        out.image = loaded.convertToFormat(QImage::Format_RGBA8888);
        out.ok = true;
        return out;
    }

    // Default: "stdio-rgba" — send JSON line on stdin, receive VGEN binary RGBA on stdout.
    QStringList args = plugin.extraArgs;
    args << QStringLiteral("--stdio-rgba");
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForStarted(3000)) {
        out.missingRenderer = true;
        out.statusMessage = QStringLiteral("外部ジェネレータ起動失敗: %1").arg(plugin.executablePath);
        return out;
    }
    QJsonObject req;
    req[QStringLiteral("entry")] = spec.entryPath;
    req[QStringLiteral("frame")] = frameIndex;
    req[QStringLiteral("fps")] = spec.fps;
    req[QStringLiteral("width")] = size.width();
    req[QStringLiteral("height")] = size.height();
    req[QStringLiteral("params")] = spec.params;
    QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact);
    line.append('\n');
    proc.write(line);
    proc.closeWriteChannel();

    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1500);
        out.statusMessage = QStringLiteral("外部ジェネレータがタイムアウトしました (%1 ms)").arg(timeoutMs);
        return out;
    }
    if (proc.exitCode() != 0) {
        out.statusMessage = QStringLiteral("外部ジェネレータ終了コード %1").arg(proc.exitCode());
        return out;
    }
    const QByteArray stdoutBytes = proc.readAllStandardOutput();
    QImage decoded;
    if (!decodeVgenFrame(stdoutBytes, &decoded)) {
        out.statusMessage = QStringLiteral("VGEN RGBA フレームのデコードに失敗しました");
        return out;
    }
    if (decoded.size() != size)
        decoded = decoded.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    out.image = decoded;
    out.ok = true;
    return out;
}

QString defaultCacheRootDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!base.isEmpty())
        return QDir(base).filePath(QStringLiteral("extgen-cache"));
    return QDir::tempPath() + QStringLiteral("/v-simple-editor-extgen-cache");
}

QString frameCachePath(const GeneratorSpec &spec, int frameIndex, QSize size)
{
    const QString root = spec.cacheDir.isEmpty() ? defaultCacheRootDir() : spec.cacheDir;
    const QString hash = computeSpecHash(spec);
    return QDir(root).filePath(
        QStringLiteral("%1/f_%2_%3x%4.png")
            .arg(hash).arg(frameIndex).arg(size.width()).arg(size.height()));
}

QString lastCachedFramePath(const GeneratorSpec &spec)
{
    const QString root = spec.cacheDir.isEmpty() ? defaultCacheRootDir() : spec.cacheDir;
    const QString hash = computeSpecHash(spec);
    return QDir(root).filePath(QStringLiteral("%1/last.png").arg(hash));
}

void storeFrameInCache(const GeneratorSpec &spec, int frameIndex, QSize size, const QImage &img)
{
    if (img.isNull())
        return;
    const QString path = frameCachePath(spec, frameIndex, size);
    const QString lastPath = lastCachedFramePath(spec);
    QDir().mkpath(QFileInfo(path).absolutePath());
    img.save(path, "PNG");
    img.save(lastPath, "PNG");

    QMutexLocker lock(&g_cacheMutex);
    g_memoryCache.insert(path, new QImage(img));
}

QImage loadFrameFromCache(const GeneratorSpec &spec, int frameIndex, QSize size)
{
    const QString path = frameCachePath(spec, frameIndex, size);
    {
        QMutexLocker lock(&g_cacheMutex);
        if (QImage *cached = g_memoryCache.object(path))
            return *cached;
    }
    if (QFileInfo::exists(path)) {
        QImage img(path);
        if (!img.isNull()) {
            img = img.convertToFormat(QImage::Format_RGBA8888);
            QMutexLocker lock(&g_cacheMutex);
            g_memoryCache.insert(path, new QImage(img));
            return img;
        }
    }
    return {};
}

QImage loadLastCachedFrame(const GeneratorSpec &spec)
{
    const QString lastPath = lastCachedFramePath(spec);
    if (!QFileInfo::exists(lastPath))
        return {};
    QImage img(lastPath);
    return img.isNull() ? QImage() : img.convertToFormat(QImage::Format_RGBA8888);
}

void clearMemoryCache()
{
    QMutexLocker lock(&g_cacheMutex);
    g_memoryCache.clear();
}

QImage renderMissingPlaceholder(const GeneratorSpec &spec, QSize size, const QString &detail)
{
    const QSize sz = (size.isValid() && !size.isEmpty()) ? size : QSize(640, 360);
    QImage img(sz, QImage::Format_RGBA8888);
    img.fill(QColor(26, 30, 42, 240));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(235, 160, 50), 3));
    p.drawRoundedRect(img.rect().adjusted(8, 8, -8, -8), 10, 10);

    QFont titleFont = p.font();
    titleFont.setBold(true);
    titleFont.setPixelSize(qBound(14, sz.height() / 16, 34));
    p.setFont(titleFont);
    p.setPen(QColor(255, 220, 120));
    p.drawText(QRect(20, sz.height() / 4, sz.width() - 40, sz.height() / 4),
               Qt::AlignCenter | Qt::TextWordWrap,
               QStringLiteral("ジェネレータ未導入 (%1)").arg(spec.renderer));

    QFont subFont = p.font();
    subFont.setBold(false);
    subFont.setPixelSize(qBound(11, sz.height() / 26, 20));
    p.setFont(subFont);
    p.setPen(QColor(210, 218, 230));
    const QString entryBase = spec.entryPath.isEmpty()
        ? QStringLiteral("(エントリ未指定)")
        : QFileInfo(spec.entryPath).fileName();
    const QString body = detail.isEmpty()
        ? QStringLiteral("%1\nプラグインを導入するか、作成元 PC で動画にベイクしてください。").arg(entryBase)
        : QStringLiteral("%1\n%2").arg(entryBase, detail);
    p.drawText(QRect(24, sz.height() / 2, sz.width() - 48, sz.height() / 3),
               Qt::AlignCenter | Qt::TextWordWrap, body);
    return img;
}

QImage renderUntrustedPlaceholder(const GeneratorSpec &spec, QSize size)
{
    const QSize sz = (size.isValid() && !size.isEmpty()) ? size : QSize(640, 360);
    QImage img(sz, QImage::Format_RGBA8888);
    img.fill(QColor(46, 28, 22, 240));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(240, 95, 70), 3));
    p.drawRoundedRect(img.rect().adjusted(8, 8, -8, -8), 10, 10);

    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(qBound(14, sz.height() / 16, 32));
    p.setFont(f);
    p.setPen(QColor(255, 195, 160));
    p.drawText(QRect(20, sz.height() / 3, sz.width() - 40, sz.height() / 3),
               Qt::AlignCenter | Qt::TextWordWrap,
               QStringLiteral("未許可の外部ジェネレータ (%1)\n右クリック→外部ジェネレータ設定からフォルダを許可してください")
                   .arg(spec.renderer));
    return img;
}

QImage renderDummyFrame(const GeneratorSpec &spec, int frameIndex, QSize size)
{
    const QSize sz = (size.isValid() && !size.isEmpty()) ? size : QSize(640, 360);
    QImage img(sz, QImage::Format_RGBA8888);
    const bool transparentBg = spec.params.value(QStringLiteral("transparent")).toBool(false);
    if (transparentBg) {
        img.fill(Qt::transparent);
    } else {
        const int r = qBound(20, (frameIndex * 7 + 40) % 220, 220);
        const int g = qBound(30, (frameIndex * 5 + 80) % 200, 200);
        const int b = 150;
        img.fill(QColor(r, g, b, 255));
    }
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Animated accent bar driven by frameIndex
    const double phase = std::fmod(qMax(0, frameIndex) / qMax(1.0, spec.fps), 1.0);
    const int barW = qMax(8, static_cast<int>(sz.width() * (0.15 + 0.7 * phase)));
    const int barH = qMax(6, sz.height() / 12);
    p.fillRect(QRect(sz.width() / 10, sz.height() * 2 / 3, barW, barH),
               QColor(255, 220, 60, 240));

    const QString title = spec.params.value(QStringLiteral("title")).toString(QStringLiteral("External Generator"));
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(qBound(12, sz.height() / 12, 42));
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRect(16, sz.height() / 5, sz.width() - 32, sz.height() / 3),
               Qt::AlignCenter | Qt::TextWordWrap,
               QStringLiteral("%1  #%2").arg(title).arg(frameIndex));
    return img;
}

RenderOutcome renderGeneratorFrame(const GeneratorSpec &spec,
                                   int frameIndex,
                                   QSize outSize,
                                   bool isPreview)
{
    RenderOutcome out;
    const QSize targetSize = (outSize.isValid() && !outSize.isEmpty()) ? outSize : QSize(640, 360);
    const QSize renderSize = isPreview ? scaledPreviewSize(targetSize) : targetSize;
    const int safeFrame = qMax(0, frameIndex);

    // 1. Check cache first (exact frame & resolution)
    if (QImage cached = loadFrameFromCache(spec, safeFrame, renderSize); !cached.isNull()) {
        out.image = (cached.size() == targetSize)
            ? cached
            : cached.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        out.ok = true;
        out.fromCache = true;
        return out;
    }

    // 2. Built-in safe dummy renderer (no external code execution)
    if (spec.renderer == QStringLiteral("dummy")) {
        QImage img = renderDummyFrame(spec, safeFrame, renderSize);
        storeFrameInCache(spec, safeFrame, renderSize, img);
        out.image = (img.size() == targetSize)
            ? img
            : img.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        out.ok = true;
        return out;
    }

    // 3. R3 Trust gate for all external code renderers (Edge / QuickJS / custom plugins)
    if (!spec.entryPath.isEmpty() && !isFolderTrusted(spec.entryPath)) {
        out.untrusted = true;
        out.statusMessage = QStringLiteral("未許可のフォルダです: %1")
            .arg(canonicalTrustFolderForEntry(spec.entryPath));
        out.image = renderUntrustedPlaceholder(spec, targetSize);
        return out;
    }

    // 4. Deterministic test renderer hook (selftests never spawn real Edge/QuickJS)
    TestRendererFn testFn;
    {
        QMutexLocker lock(&g_testRendererMutex);
        testFn = g_testRenderers.value(spec.renderer);
    }
    if (testFn) {
        QImage img = testFn(spec, safeFrame, renderSize);
        if (!img.isNull()) {
            img = img.convertToFormat(QImage::Format_RGBA8888);
            storeFrameInCache(spec, safeFrame, renderSize, img);
            out.image = (img.size() == targetSize)
                ? img
                : img.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            out.ok = true;
            return out;
        }
    }

    // 5. Dispatch to concrete external renderer
    if (spec.renderer == QStringLiteral("edge")) {
        out = edge::renderFrameViaEdgeCdp(spec, safeFrame, renderSize);
    } else {
        const PluginManifest plugin = findPlugin(spec.renderer);
        if (!plugin.valid) {
            out.missingRenderer = true;
            out.statusMessage = QStringLiteral("ジェネレータ未導入 (%1)").arg(spec.renderer);
        } else {
            out = renderViaSubprocess(plugin, spec, safeFrame, renderSize);
        }
    }

    if (out.ok && !out.image.isNull()) {
        storeFrameInCache(spec, safeFrame, renderSize, out.image);
        if (out.image.size() != targetSize) {
            out.image = out.image.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        return out;
    }

    // 6. Missing-renderer / failure fallback: show last cached frame if available, else placeholder card.
    if (QImage last = loadLastCachedFrame(spec); !last.isNull()) {
        if (last.size() != targetSize)
            last = last.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        out.image = stampCornerBadge(
            last,
            QStringLiteral("キャッシュ表示 (ジェネレータ未導入: %1)").arg(spec.renderer),
            QColor(180, 95, 20, 210));
        out.fromCache = true;
        return out;
    }

    out.image = renderMissingPlaceholder(spec, targetSize, out.statusMessage);
    return out;
}

QImage renderGeneratorClipFrame(const ClipInfo &clip,
                                double clipLocalSec,
                                QSize outSize,
                                const Timeline *timeline,
                                bool isPreview)
{
    const GeneratorSpec spec = parseGeneratorUri(clip.filePath);
    if (spec.renderer == QStringLiteral("visualizer")) {
        return creatortools::renderAudioVisualizerFrame(spec.params, clipLocalSec, outSize, timeline);
    }
    const double fps = (spec.fps > 0.0) ? spec.fps : 30.0;
    const int frameIndex = qMax(0, static_cast<int>(std::floor(qMax(0.0, clipLocalSec) * fps + 1e-6)));
    const bool usePreview = isPreview || previewModeHintForCurrentThread();
    return renderGeneratorFrame(spec, frameIndex, outSize, usePreview).image;
}

BakeResult bakeExternalGeneratorClip(Timeline *timeline,
                                     int trackIdx,
                                     int clipIdx,
                                     const QString &outputPath,
                                     QSize renderSize,
                                     double fps,
                                     bool forceSoftwareCodec)
{
    BakeResult res;
    if (!timeline) {
        res.error = QStringLiteral("タイムラインが無効です。");
        return res;
    }
    const QVector<TimelineTrack *> tracks = timeline->videoTracks();
    if (trackIdx < 0 || trackIdx >= tracks.size() || !tracks[trackIdx]) {
        res.error = QStringLiteral("トラックが見つかりません。");
        return res;
    }
    TimelineTrack *track = tracks[trackIdx];
    if (track->isLocked()) {
        res.error = QStringLiteral("トラックがロックされています。");
        return res;
    }
    QVector<ClipInfo> clips = track->clips();
    if (clipIdx < 0 || clipIdx >= clips.size()) {
        res.error = QStringLiteral("クリップが見つかりません。");
        return res;
    }
    const ClipInfo &clip = clips[clipIdx];
    if (!isExternalGeneratorFilePath(clip.filePath)) {
        res.error = QStringLiteral("外部ジェネレータクリップではありません。");
        return res;
    }
    const GeneratorSpec spec = parseGeneratorUri(clip.filePath);
    const double useFps = (fps > 0.0) ? fps : (spec.fps > 0.0 ? spec.fps : 30.0);
    const double clipDur = qMax(1.0 / useFps, clip.effectiveDuration());
    const int totalFrames = qMax(1, static_cast<int>(std::ceil(clipDur * useFps)));
    const QSize sz = (renderSize.isValid() && !renderSize.isEmpty()) ? renderSize : QSize(1280, 720);

    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    libavcore::EncodeRequest req;
    req.width = (sz.width() + 1) & ~1;
    req.height = (sz.height() + 1) & ~1;
    req.fps = qMax(1, qRound(useFps));
    req.videoBitrateBits = 6'000'000;
    req.outputPath = outputPath.toStdString();
    if (forceSoftwareCodec) {
        req.videoCodecName = "mpeg4";
        req.hwVendorHint = "none";
        req.useHardwareAccel = false;
    } else {
        req.videoCodecName = "mpeg4";
        req.hwVendorHint = "none";
        req.useHardwareAccel = false;
    }

    libavcore::FrameEncoder encoder;
    if (const auto err = encoder.open(req); err.has_value()) {
        res.error = QString::fromStdString(*err);
        return res;
    }

    for (int f = 0; f < totalFrames; ++f) {
        const RenderOutcome ro = renderGeneratorFrame(spec, f, QSize(req.width, req.height), /*isPreview=*/false);
        if (ro.image.isNull()) {
            res.error = QStringLiteral("フレーム %1 の描画に失敗しました。").arg(f);
            return res;
        }
        // Composite onto black for opaque MP4 bake
        QImage rgbFrame(req.width, req.height, QImage::Format_RGB888);
        rgbFrame.fill(Qt::black);
        {
            QPainter p(&rgbFrame);
            p.drawImage(0, 0, ro.image);
        }
        if (!encoder.pushFrameRgb24(rgbFrame.constBits(), rgbFrame.bytesPerLine(), f)) {
            res.error = QStringLiteral("フレーム %1 のエンコードに失敗しました。").arg(f);
            return res;
        }
        ++res.framesRendered;
    }
    if (const auto err = encoder.finalize(); err.has_value()) {
        res.error = QString::fromStdString(*err);
        return res;
    }

    clips[clipIdx].filePath = outputPath;
    clips[clipIdx].inPoint = 0.0;
    clips[clipIdx].outPoint = clipDur;
    clips[clipIdx].duration = clipDur;
    track->setClips(clips);
    timeline->saveUndoState(QStringLiteral("外部ジェネレータをベイク"));

    res.ok = true;
    res.outputPath = outputPath;
    return res;
}

void registerTestRenderer(const QString &rendererId, TestRendererFn fn)
{
    QMutexLocker lock(&g_testRendererMutex);
    g_testRenderers.insert(rendererId, std::move(fn));
}

void clearTestRenderers()
{
    QMutexLocker lock(&g_testRendererMutex);
    g_testRenderers.clear();
}

void setPreviewModeHintForCurrentThread(bool isPreview)
{
    tl_previewModeHint = isPreview;
}

bool previewModeHintForCurrentThread()
{
    return tl_previewModeHint;
}

ExternalGeneratorDialog::ExternalGeneratorDialog(const GeneratorSpec &initialSpec,
                                                 double initialDurationSec,
                                                 QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("外部ジェネレータクリップ設定"));
    setMinimumWidth(480);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    m_rendererCombo = new QComboBox(this);
    m_rendererCombo->setObjectName(QStringLiteral("extgenRendererCombo"));
    auto addRendererOption = [&](const QString &id, const QString &label) {
        const bool avail = isRendererAvailable(id);
        const QString status = avail ? QStringLiteral(" [導入済み]") : QStringLiteral(" [未導入]");
        m_rendererCombo->addItem(label + status, id);
    };
    addRendererOption(QStringLiteral("edge"), QStringLiteral("HTML / React (Edge Headless CDP)"));
    addRendererOption(QStringLiteral("quickjs"), QStringLiteral("JavaScript スクリプト (QuickJS プラグイン)"));
    addRendererOption(QStringLiteral("visualizer"), QStringLiteral("オーディオビジュアライザ (音声連動スペクトラム/波形)"));
    addRendererOption(QStringLiteral("dummy"), QStringLiteral("組み込みダミー描画係 (テスト／プレースホルダ)"));
    for (const auto &p : discoverPlugins()) {
        if (p.id != QStringLiteral("quickjs") && p.id != QStringLiteral("edge") && p.id != QStringLiteral("dummy"))
            m_rendererCombo->addItem(p.name + QStringLiteral(" [プラグイン]"), p.id);
    }
    const int initIdx = m_rendererCombo->findData(initialSpec.renderer);
    if (initIdx >= 0)
        m_rendererCombo->setCurrentIndex(initIdx);
    form->addRow(QStringLiteral("描画係:"), m_rendererCombo);

    auto *entryRow = new QHBoxLayout();
    m_entryEdit = new QLineEdit(initialSpec.entryPath, this);
    m_entryEdit->setObjectName(QStringLiteral("extgenEntryEdit"));
    m_entryEdit->setPlaceholderText(QStringLiteral("HTML / JS エントリファイル (dummy の場合は空欄可)"));
    auto *browseBtn = new QPushButton(QStringLiteral("参照…"), this);
    entryRow->addWidget(m_entryEdit, 1);
    entryRow->addWidget(browseBtn);
    form->addRow(QStringLiteral("スクリプト / HTML:"), entryRow);

    auto *trustRow = new QHBoxLayout();
    m_trustLabel = new QLabel(this);
    m_trustLabel->setObjectName(QStringLiteral("extgenTrustLabel"));
    m_trustButton = new QPushButton(QStringLiteral("このフォルダを許可"), this);
    m_trustButton->setObjectName(QStringLiteral("extgenTrustButton"));
    trustRow->addWidget(m_trustLabel, 1);
    trustRow->addWidget(m_trustButton);
    form->addRow(QStringLiteral("実行許可 (セキュリティ):"), trustRow);

    m_fpsSpin = new QDoubleSpinBox(this);
    m_fpsSpin->setObjectName(QStringLiteral("extgenFpsSpin"));
    m_fpsSpin->setRange(1.0, 240.0);
    m_fpsSpin->setDecimals(2);
    m_fpsSpin->setValue(initialSpec.fps > 0.0 ? initialSpec.fps : 30.0);
    form->addRow(QStringLiteral("FPS:"), m_fpsSpin);

    m_durationSpin = new QDoubleSpinBox(this);
    m_durationSpin->setObjectName(QStringLiteral("extgenDurationSpin"));
    m_durationSpin->setRange(0.1, 3600.0);
    m_durationSpin->setDecimals(2);
    m_durationSpin->setSuffix(QStringLiteral(" 秒"));
    m_durationSpin->setValue(initialDurationSec > 0.0 ? initialDurationSec : 5.0);
    form->addRow(QStringLiteral("クリップ長さ:"), m_durationSpin);

    m_paramsEdit = new QPlainTextEdit(this);
    m_paramsEdit->setObjectName(QStringLiteral("extgenParamsEdit"));
    m_paramsEdit->setMaximumHeight(110);
    const QJsonObject initParams = initialSpec.params.isEmpty()
        ? QJsonObject{{QStringLiteral("title"), QStringLiteral("Hello Wave 25")}}
        : initialSpec.params;
    m_paramsEdit->setPlainText(
        QString::fromUtf8(QJsonDocument(initParams).toJson(QJsonDocument::Indented)));
    form->addRow(QStringLiteral("パラメータ JSON:"), m_paramsEdit);

    root->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    connect(browseBtn, &QPushButton::clicked, this, &ExternalGeneratorDialog::browseEntry);
    connect(m_entryEdit, &QLineEdit::textChanged, this, &ExternalGeneratorDialog::refreshTrustStatus);
    connect(m_trustButton, &QPushButton::clicked, this, [this]() {
        const QString p = m_entryEdit->text().trimmed();
        if (!p.isEmpty()) {
            trustFolder(p);
            refreshTrustStatus();
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refreshTrustStatus();
}

GeneratorSpec ExternalGeneratorDialog::spec() const
{
    GeneratorSpec s;
    s.renderer = m_rendererCombo ? m_rendererCombo->currentData().toString() : QStringLiteral("dummy");
    s.entryPath = m_entryEdit ? m_entryEdit->text().trimmed() : QString();
    s.fps = m_fpsSpin ? m_fpsSpin->value() : 30.0;
    if (m_paramsEdit) {
        const QJsonDocument doc = QJsonDocument::fromJson(m_paramsEdit->toPlainText().toUtf8());
        if (doc.isObject())
            s.params = doc.object();
    }
    return s;
}

double ExternalGeneratorDialog::durationSec() const
{
    return m_durationSpin ? m_durationSpin->value() : 5.0;
}

void ExternalGeneratorDialog::refreshTrustStatus()
{
    const QString path = m_entryEdit ? m_entryEdit->text().trimmed() : QString();
    if (path.isEmpty()) {
        m_trustLabel->setText(QStringLiteral("外部ファイル未使用"));
        m_trustButton->setEnabled(false);
        return;
    }
    const bool trusted = isFolderTrusted(path);
    m_trustLabel->setText(trusted ? QStringLiteral("許可済みフォルダ") : QStringLiteral("未許可フォルダ"));
    m_trustButton->setEnabled(!trusted);
}

void ExternalGeneratorDialog::browseEntry()
{
    const QString picked = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("外部ジェネレータのエントリファイルを選択"),
        m_entryEdit->text(),
        QStringLiteral("Generator Files (*.html *.htm *.js *.mjs *.py);;All Files (*.*)"));
    if (!picked.isEmpty())
        m_entryEdit->setText(picked);
}

} // namespace extgen
