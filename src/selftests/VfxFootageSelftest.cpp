#include "../EffectLibraryModel.h"
#include "../LayerCompositor.h"
#include "../Timeline.h"
#include "../VfxFootageLibrary.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QDir>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>

namespace {

bool sameImage(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format()
        || a.sizeInBytes() != b.sizeInBytes()) {
        return false;
    }
    return a.sizeInBytes() == 0
        || std::memcmp(a.constBits(), b.constBits(),
                       static_cast<size_t>(a.sizeInBytes())) == 0;
}

bool hasNonBlackPixel(const QImage &image)
{
    if (image.isNull())
        return false;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qGray(line[x]) > 0)
                return true;
        }
    }
    return false;
}

bool touchFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("fixture") == 7;
}

void restoreEnv(const char *name, bool wasSet, const QByteArray &oldValue)
{
    if (wasSet)
        qputenv(name, oldValue);
    else
        qunsetenv(name);
}

} // namespace

int runVfxFootageSelftest()
{
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    auto check = [&](int gate, const char *name, bool ok,
                     const QString &detail = QString()) {
        const QByteArray text = detail.toUtf8();
        std::printf("[vfx-footage] %s G%d %s%s%s\n",
                    ok ? "PASS" : "FAIL", gate, name,
                    detail.isEmpty() ? "" : " - ",
                    detail.isEmpty() ? "" : text.constData());
        if (ok)
            ++passed;
        else
            ++failed;
    };

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        check(0, "temporary footage directory", false);
        return 1;
    }

    // G1: a missing directory is an empty catalog, never an exception or a
    // fatal error. This is the regression gate for startup with no assets.
    const QVector<vfxfootage::FootageItem> missing =
        vfxfootage::VfxFootageLibrary::scan(
            QDir(tempDir.path()).filePath(QStringLiteral("missing")));
    check(1, "missing directory returns zero items", missing.isEmpty());

    const QString fire = QDir(tempDir.path()).filePath(QStringLiteral("fire_01.mp4"));
    const QString smoke = QDir(tempDir.path()).filePath(QStringLiteral("smoke_02.MOV"));
    const QString webm = QDir(tempDir.path()).filePath(QStringLiteral("spark.webm"));
    const QString unknown = QDir(tempDir.path()).filePath(QStringLiteral("foo.mp4"));
    const bool fixturesCreated = touchFile(fire) && touchFile(smoke)
        && touchFile(webm) && touchFile(unknown)
        && touchFile(QDir(tempDir.path()).filePath(QStringLiteral("notes.txt")))
        && touchFile(QDir(tempDir.path()).filePath(QStringLiteral("still.jpg")));
    const QVector<vfxfootage::FootageItem> scanned =
        vfxfootage::VfxFootageLibrary::scan(tempDir.path());

    // G2: only the three supported movie suffixes are retained, including
    // case-insensitive MOV, while unrelated files are ignored.
    check(2, "mp4/mov/webm filter excludes txt/jpg",
          fixturesCreated && scanned.size() == 4);

    bool fireCategoryOk = false;
    bool smokeCategoryOk = false;
    bool unknownKept = false;
    for (const vfxfootage::FootageItem &item : scanned) {
        const QString name = QFileInfo(item.filePath).fileName();
        if (name == QStringLiteral("fire_01.mp4")
            && item.category == QStringLiteral("炎")) {
            fireCategoryOk = true;
        }
        if (name == QStringLiteral("smoke_02.MOV")
            && item.category == QStringLiteral("煙")) {
            smokeCategoryOk = true;
        }
        if (name == QStringLiteral("foo.mp4")
            && item.category == QStringLiteral("その他")) {
            unknownKept = true;
        }
    }
    // G3: known prefixes map to the Japanese category and unknown footage is
    // still retained under その他 rather than being silently dropped.
    check(3, "category inference keeps fire/smoke/other",
          fireCategoryOk && smokeCategoryOk && unknownKept);

    const bool oldFootageDirSet = qEnvironmentVariableIsSet(
        "VEDITOR_VFX_FOOTAGE_DIR");
    const QByteArray oldFootageDir = qgetenv("VEDITOR_VFX_FOOTAGE_DIR");
    const bool oldStateSet = qEnvironmentVariableIsSet(
        "VEDITOR_EFFECT_LIBRARY_STATE");
    const QByteArray oldState = qgetenv("VEDITOR_EFFECT_LIBRARY_STATE");
    qputenv("VEDITOR_VFX_FOOTAGE_DIR", tempDir.path().toUtf8());
    qputenv("VEDITOR_EFFECT_LIBRARY_STATE",
            QDir(tempDir.path()).filePath(QStringLiteral("state.json")).toUtf8());

    efxlib::EffectLibraryModel model;
    const QVector<efxlib::LibraryEntry> entries = model.entries();
    QString footageId;
    bool integrationOk = false;
    for (const efxlib::LibraryEntry &entry : entries) {
        if (entry.kind != efxlib::SourceKind::Footage)
            continue;
        if (footageId.isEmpty())
            footageId = entry.id;
        if (!model.footageFilePath(entry.id).isEmpty()
            && entry.tags.contains(QStringLiteral("footage"))) {
            integrationOk = true;
        }
    }
    // G4: scan output is exposed through the existing EffectLibraryModel with
    // SourceKind::Footage and a recoverable absolute source path.
    check(4, "footage scan integrates into EffectLibraryModel",
          integrationOk && !footageId.isEmpty());

    QImage blackLevelInput(3, 1, QImage::Format_ARGB32);
    blackLevelInput.setPixel(0, 0, qRgba(16, 16, 16, 255));
    blackLevelInput.setPixel(1, 0, qRgba(17, 17, 17, 255));
    blackLevelInput.setPixel(2, 0, qRgba(40, 50, 60, 123));
    const QImage tightened = vfxfootage::VfxFootageLibrary::applyBlackLevel(
        blackLevelInput, 16);
    const QImage noOpBlackLevel = vfxfootage::VfxFootageLibrary::applyBlackLevel(
        blackLevelInput, 0);
    // G5: <=16 is zeroed, a value above the threshold is untouched, and 0 is
    // a bit-exact no-op. This fails if the default is accidentally changed to
    // 10 or if the operation is implemented as a destructive global scale.
    check(5, "black level threshold is exact and bounded",
          qRed(tightened.pixel(0, 0)) == 0
              && qGreen(tightened.pixel(0, 0)) == 0
              && qBlue(tightened.pixel(0, 0)) == 0
              && tightened.pixel(1, 0) == blackLevelInput.pixel(1, 0)
              && tightened.pixel(2, 0) == blackLevelInput.pixel(2, 0)
              && sameImage(noOpBlackLevel, blackLevelInput));

    QImage base(2, 1, QImage::Format_ARGB32);
    base.setPixel(0, 0, qRgb(31, 47, 63));
    base.setPixel(1, 0, qRgb(80, 90, 100));
    QImage overlay(2, 1, QImage::Format_ARGB32);
    overlay.setPixel(0, 0, qRgba(0, 0, 0, 255));
    overlay.setPixel(1, 0, qRgba(255, 255, 255, 255));
    const QImage screened = vfxfootage::VfxFootageLibrary::screenComposite(
        base, overlay);
    // G6: Screen is the existing LayerCompositor mode. Opaque black preserves
    // the base bit-for-bit; the white source region becomes brighter.
    check(6, "Screen preserves black and brightens white",
          screened.pixel(0, 0) == base.pixel(0, 0)
              && qRed(screened.pixel(1, 0)) > qRed(base.pixel(1, 0)));

    QImage intensityInput(2, 1, QImage::Format_ARGB32);
    intensityInput.setPixel(0, 0, qRgb(100, 50, 200));
    intensityInput.setPixel(1, 0, qRgb(200, 180, 250));
    const QImage unity = vfxfootage::VfxFootageLibrary::applyIntensity(
        intensityInput, 1.0);
    const QImage doubled = vfxfootage::VfxFootageLibrary::applyIntensity(
        intensityInput, 2.0);
    // G7: strength is separate from opacity, unity is exact, and gain clamps
    // every channel at 255.
    check(7, "intensity unity/doubling/clamp",
          sameImage(unity, intensityInput)
              && qRed(doubled.pixel(0, 0)) == 200
              && qGreen(doubled.pixel(0, 0)) == 100
              && qBlue(doubled.pixel(0, 0)) == 255
              && qRed(doubled.pixel(1, 0)) == 255);

    const QImage deterministicA = vfxfootage::VfxFootageLibrary::screenComposite(
        base,
        vfxfootage::VfxFootageLibrary::applyIntensity(
            vfxfootage::VfxFootageLibrary::applyBlackLevel(
                overlay, 16), 1.75));
    const QImage deterministicB = vfxfootage::VfxFootageLibrary::screenComposite(
        base,
        vfxfootage::VfxFootageLibrary::applyIntensity(
            vfxfootage::VfxFootageLibrary::applyBlackLevel(
                overlay, 16), 1.75));
    // G8: the same source and parameters produce a byte-identical result.
    check(8, "VFX pixel pipeline is deterministic",
          sameImage(deterministicA, deterministicB));

    bool defaultsOk = false;
    bool configuredOk = false;
    if (!footageId.isEmpty()) {
        ClipInfo defaultClip;
        defaultClip.duration = 1.0;
        defaultsOk = model.applyToClip(footageId, defaultClip)
            && defaultClip.isVfxFootage
            && defaultClip.blendMode == BlendMode::Screen
            && defaultClip.vfxIntensity == 1.0
            && defaultClip.vfxBlackLevel == 16;

        configuredOk = model.setParameterOverride(
            footageId, QStringLiteral("vfxIntensity"), 2.0)
            && model.setParameterOverride(
                footageId, QStringLiteral("vfxBlackLevel"), 32)
            && model.setParameterOverride(
                footageId, QStringLiteral("blendMode"),
                QStringLiteral("Add"));
        ClipInfo configuredClip;
        configuredClip.duration = 1.0;
        configuredOk = configuredOk
            && model.applyToClip(footageId, configuredClip)
            && configuredClip.blendMode == BlendMode::Add
            && configuredClip.vfxIntensity == 2.0
            && configuredClip.vfxBlackLevel == 32;
    }
    // G9: the library's defaults and inspector overrides reach the actual
    // ClipInfo seam used by placement, including Add as an alternative mode.
    check(9, "footage defaults and overrides configure ClipInfo",
          defaultsOk && configuredOk);

    // G10: placement uses the existing timeline insertion primitive, lands
    // above V1 at the current playhead, and preserves the VFX controls.
    ClipInfo placementClip;
    placementClip.filePath = fire;
    placementClip.displayName = QStringLiteral("fire_01");
    placementClip.duration = 1.0;
    placementClip.outPoint = 1.0;
    placementClip.isVfxFootage = true;
    placementClip.blendMode = BlendMode::Screen;
    placementClip.vfxBlackLevel = 16;
    Timeline timeline;
    timeline.setPlayheadPosition(2.0);
    int placedTrack = -1;
    int placedClip = -1;
    const bool placed = timeline.insertVfxFootageAtPlayhead(
        placementClip, &placedTrack, &placedClip);
    check(10, "footage placement targets upper track at playhead",
          placed && placedTrack == 1 && placedClip == 0
              && timeline.videoTracks().at(1)->clips().at(0).leadInSec >= 1.999
              && timeline.videoTracks().at(1)->clips().at(0).isVfxFootage);

    // G11: use the repository's real footage when it is available. The
    // synthetic scan fixtures above are deliberately not binary videos, so
    // they must not make thumbnail decoding look green by accident. An empty
    // real-media directory is an explicit SKIP; an existing supported file
    // that cannot produce a frame is a failure.
    const QByteArray fixtureFootageDir = qgetenv("VEDITOR_VFX_FOOTAGE_DIR");
    qunsetenv("VEDITOR_VFX_FOOTAGE_DIR");
    const QVector<vfxfootage::FootageItem> realItems =
        vfxfootage::VfxFootageLibrary::scan(
            vfxfootage::VfxFootageLibrary::defaultDirectory());
    qputenv("VEDITOR_VFX_FOOTAGE_DIR", fixtureFootageDir);
    if (realItems.isEmpty()) {
        ++skipped;
        std::printf("[vfx-footage] SKIP G11 representative frame decode - "
                    "no real footage is available\n");
    } else {
        const QImage representative =
            vfxfootage::VfxFootageLibrary::representativeFrame(
                realItems.first().filePath);
        check(11, "representative frame is non-black for valid media",
              hasNonBlackPixel(representative), realItems.first().filePath);
    }

    restoreEnv("VEDITOR_VFX_FOOTAGE_DIR", oldFootageDirSet, oldFootageDir);
    restoreEnv("VEDITOR_EFFECT_LIBRARY_STATE", oldStateSet, oldState);

    std::printf("[vfx-footage] RESULT passed=%d failed=%d skipped=%d\n",
                passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}
