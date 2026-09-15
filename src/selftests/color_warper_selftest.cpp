#include "../VideoEffect.h"
#include "../ProjectFile.h"
#include "../EffectPreset.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
QImage pixel(const QColor &c) {
    QImage image(1, 1, QImage::Format_RGB888);
    image.fill(c); return image;
}
QColor corrected(const QColor &c, const ColorCorrection &cc) {
    return VideoEffectProcessor::applyColorCorrection(pixel(c), cc).pixelColor(0, 0);
}
bool samePixels(const QImage &a, const QImage &b) {
    if (a.size()!=b.size() || a.format()!=b.format()) return false;
    for (int y=0; y<a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y), a.width()*a.depth()/8)!=0) return false;
    return true;
}
bool sameWarp(const HueSatWarp &a, const HueSatWarp &b) {
    for (int r=0; r<3; ++r)
        for (int h=0; h<12; ++h)
            if (a.hueShiftDeg[r][h]!=b.hueShiftDeg[r][h] || a.satScale[r][h]!=b.satScale[r][h]) return false;
    return true;
}
}
int runColorWarperSelftest()
{
    int passed=0, failed=0;
    auto check=[&](int gate, bool ok) {
        std::fprintf(stderr, "%s G%d\n", ok ? "PASS" : "FAIL", gate);
        ok ? ++passed : ++failed;
    };
    QImage input(256, 8, QImage::Format_RGB888);
    for (int y=0; y<input.height(); ++y)
        for (int x=0; x<input.width(); ++x)
            input.setPixelColor(x,y,QColor(x,(x*17+y*31)%256,(x*7+y*53)%256));
    ColorCorrection cc;
    colorwarper::resetCallCountForTest();
    bool identity=samePixels(input,VideoEffectProcessor::applyColorCorrection(input,cc));
    // Exercise real pre-existing non-default pipelines with the new branch bypassed.
    for (int i=0; i<3; ++i) {
        cc.brightness=7; cc.saturation=18; cc.temperature=-9;
        cc.liftR=i ? 0.08 : 0; cc.logMidG=i==2 ? 0.1 : 0;
        colorwarper::setDisabledForTest(true);
        const auto bypass=VideoEffectProcessor::applyColorCorrection(input,cc);
        colorwarper::setDisabledForTest(false);
        identity &= samePixels(bypass,VideoEffectProcessor::applyColorCorrection(input,cc));
    }
    check(1, identity && colorwarper::callCountForTest()==0);
    cc.reset();
    for (int r=0; r<3; ++r) cc.hueSatWarp.hueShiftDeg[r][0]=30;
    const auto red=corrected(QColor(255,0,0),cc);
    check(2, std::abs(red.hsvHueF()*360.0-30.0)<=1.0
        && samePixels(pixel(QColor(0,255,255)),VideoEffectProcessor::applyColorCorrection(pixel(QColor(0,255,255)),cc))
        && colorwarper::callCountForTest()>0);
    cc.reset();
    for (int h=0; h<12; ++h) cc.hueSatWarp.satScale[2][h]=0.5f;
    const double s1=corrected(QColor::fromHsvF(0,1,1),cc).hsvSaturationF();
    const double s75=corrected(QColor::fromHsvF(0,0.75,1),cc).hsvSaturationF();
    const double s10=corrected(QColor::fromHsvF(0,0.1,1),cc).hsvSaturationF();
    check(3,std::abs(s1-0.5)<=0.01 && s75>0.5 && s75<0.75
        && std::abs(s75-0.5625)<=0.01 && std::abs(s10-0.1)<=0.01);
    cc.reset();
    for (int r=0; r<3; ++r) cc.hueSatWarp.hueShiftDeg[r][0]=60;
    const double a=corrected(QColor::fromHsvF(355.0/360,1,1),cc).hsvHueF()*360;
    const double b=corrected(QColor::fromHsvF(5.0/360,1,1),cc).hsvHueF()*360;
    check(4,std::abs(std::remainder(a-b,360.0))<=12.0);
    cc.reset();
    ProjectData data;
    ClipInfo clip; clip.filePath=QStringLiteral("color-warper.mov"); clip.duration=1; clip.outPoint=1;
    // Keep the enclosing correction object present to test nested default omission.
    clip.colorCorrection.brightness=1;
    data.videoTracks={{clip}};
    const QString defaults=ProjectFile::toJsonString(data);
    bool jsonOk=!defaults.contains(QStringLiteral("hueSatWarp"))
        && !PresetLibrary::colorCorrectionToJson(cc).contains("hueSatWarp");
    for (int r=0; r<3; ++r)
        for (int h=0; h<12; ++h) {
            cc.hueSatWarp.hueShiftDeg[r][h]=static_cast<float>(r*12+h-18);
            cc.hueSatWarp.satScale[r][h]=static_cast<float>(r*12+h)/32.0f;
        }
    data.videoTracks[0][0].colorCorrection=cc;
    const QJsonObject preset=PresetLibrary::colorCorrectionToJson(cc);
    const auto warp=preset["hueSatWarp"].toObject();
    jsonOk &= warp["hueShift"].toArray().size()==36 && warp["satScale"].toArray().size()==36;
    jsonOk &= sameWarp(cc.hueSatWarp,PresetLibrary::colorCorrectionFromJson(preset).hueSatWarp);
    ProjectData loaded;
    jsonOk &= ProjectFile::fromJsonString(ProjectFile::toJsonString(data),loaded);
    jsonOk &= !loaded.videoTracks.isEmpty() && !loaded.videoTracks[0].isEmpty()
        && sameWarp(cc.hueSatWarp,loaded.videoTracks[0][0].colorCorrection.hueSatWarp);
    ProjectData defaultLoaded;
    jsonOk &= ProjectFile::fromJsonString(defaults,defaultLoaded)
        && ProjectFile::toJsonString(defaultLoaded)==defaults;
    check(5,jsonOk);
    bool nonDefault=!cc.isDefault();
    cc.hueSatWarp=HueSatWarp{};
    bool resetDefault=cc.isDefault();
    cc.hueSatWarp.satScale[0][0]=0.5f;
    check(6,nonDefault && resetDefault && !cc.isDefault());
    std::fprintf(stderr,"summary: %d PASS, %d FAIL\n",passed,failed);
    return failed;
}
