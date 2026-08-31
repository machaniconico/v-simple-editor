#include "../DynamicZoom.h"

#include <QIODevice>
#include <QTextStream>

#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {

constexpr double kEpsilon = 1.0e-9;

bool near(double left, double right)
{
    return std::abs(left - right) <= kEpsilon;
}

bool twoValues(const KeyframeTrack& track, double first, double last)
{
    return track.count() == 2
        && near(track.keyframes().first().value, first)
        && near(track.keyframes().last().value, last);
}

} // namespace

int runDynzoomSelftest()
{
    QTextStream err(stderr, QIODevice::WriteOnly);
    int passed = 0;
    int failed = 0;
    const auto gate = [&](int number, const QString& description, bool ok) {
        if (ok) {
            ++passed;
            err << "PASS G" << number << ' ' << description << '\n';
        } else {
            ++failed;
            err << "FAIL G" << number << ' ' << description << '\n';
        }
        err.flush();
    };

    const dynzoom::Rect full = dynzoom::presetRect(dynzoom::Preset::Full);

    const dynzoom::Result identity = dynzoom::build(
        full, full, 0.0, 2.0, dynzoom::Easing::Linear);
    gate(1, QStringLiteral("Full→Full は scale=1、position=0 の 2 KF"),
         twoValues(identity.positionX, 0.0, 0.0)
             && twoValues(identity.positionY, 0.0, 0.0)
             && twoValues(identity.scaleX, 1.0, 1.0)
             && twoValues(identity.scaleY, 1.0, 1.0)
             && identity.positionX.propertyName() == QStringLiteral("positionX")
             && identity.positionY.propertyName() == QStringLiteral("positionY")
             && identity.scaleX.propertyName() == QStringLiteral("scaleX")
             && identity.scaleY.propertyName() == QStringLiteral("scaleY"));

    const dynzoom::Result zoomIn = dynzoom::build(
        full, dynzoom::presetRect(dynzoom::Preset::ZoomIn),
        0.0, 2.0, dynzoom::Easing::EaseInOut);
    gate(2, QStringLiteral("ズームインは終端 scale が始端より大きい"),
         zoomIn.scaleX.count() == 2
             && zoomIn.scaleX.keyframes().last().value
                    > zoomIn.scaleX.keyframes().first().value);

    const dynzoom::Result panLeft = dynzoom::build(
        dynzoom::presetRect(dynzoom::Preset::PanRight),
        dynzoom::presetRect(dynzoom::Preset::PanLeft),
        0.0, 1.0, dynzoom::Easing::EaseInOut);
    bool positionXMonotonic = panLeft.positionX.count() == 2;
    double previous = panLeft.positionX.valueAt(0.0);
    for (double time : {0.25, 0.5, 0.75, 1.0}) {
        const double value = panLeft.positionX.valueAt(time);
        positionXMonotonic = positionXMonotonic && value >= previous - kEpsilon;
        previous = value;
    }
    gate(3, QStringLiteral("左パンは positionX が単調"), positionXMonotonic);

    const dynzoom::Result clamped = dynzoom::build(
        dynzoom::Rect{0.5, 0.5, 2.0}, full,
        0.0, 1.0, dynzoom::Easing::Linear);
    gate(4, QStringLiteral("w > 1 は 1 にクランプ"),
         clamped.scaleX.count() == 2
             && near(clamped.scaleX.keyframes().first().value, 1.0)
             && near(clamped.scaleY.keyframes().first().value, 1.0));

    constexpr double clipStart = 12.5;
    constexpr double clipDuration = 3.25;
    const dynzoom::Result timed = dynzoom::build(
        full, dynzoom::presetRect(dynzoom::Preset::ZoomIn),
        clipStart, clipDuration, dynzoom::Easing::Linear);
    const std::array<const KeyframeTrack *, 4> tracks{
        &timed.positionX, &timed.positionY, &timed.scaleX, &timed.scaleY
    };
    bool timesMatch = true;
    for (const KeyframeTrack *track : tracks) {
        timesMatch = timesMatch && track && track->count() == 2
            && near(track->keyframes().first().time, clipStart)
            && near(track->keyframes().last().time,
                    clipStart + clipDuration);
    }
    gate(5, QStringLiteral("KF 時刻は clipStart と clipStart+duration"),
         timesMatch);

    const dynzoom::Result ignoredLegacyHeight = dynzoom::build(
        dynzoom::Rect{0.5, 0.5, 1.0, 0.5}, full,
        0.0, 1.0, dynzoom::Easing::Linear);
    gate(6, QStringLiteral("h 入力は無視されキャンバス比に固定"),
         twoValues(ignoredLegacyHeight.positionX, 0.0, 0.0)
             && twoValues(ignoredLegacyHeight.positionY, 0.0, 0.0)
             && twoValues(ignoredLegacyHeight.scaleX, 1.0, 1.0)
             && twoValues(ignoredLegacyHeight.scaleY, 1.0, 1.0));

    KeyframeManager publicTracks;
    publicTracks.addTrack(zoomIn.positionX);
    publicTracks.addTrack(zoomIn.positionY);
    publicTracks.addTrack(zoomIn.scaleX);
    publicTracks.addTrack(zoomIn.scaleY);
    int publicKeyframeCount = 0;
    for (const KeyframeTrack& track : publicTracks.tracks())
        publicKeyframeCount += track.count();
    gate(7, QStringLiteral("公開 4 トラックだけが 8 KF の単一ソース"),
         publicTracks.tracks().size() == 4
             && publicKeyframeCount == 8
             && !publicTracks.hasTrack(QStringLiteral("motion.position.x"))
             && !publicTracks.hasTrack(QStringLiteral("motion.position.y"))
             && !publicTracks.hasTrack(QStringLiteral("motion.scale")));

    err << "summary: " << passed << " PASS, " << failed << " FAIL\n";
    err.flush();
    return failed;
}
