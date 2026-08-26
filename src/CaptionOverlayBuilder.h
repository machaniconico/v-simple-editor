#pragma once

#include "CaptionStyle.h"
#include "CaptionTrack.h"
#include "TextManager.h"

#include <QString>
#include <QVector>

class CaptionOverlayBuilder final
{
public:
    inline static constexpr const char *kGeneratedTemplateName =
        "__vse_generated_single_word_caption_v1__";

    static QString generatedTemplateName();
    static QVector<EnhancedTextOverlay> build(const caption::Track &track,
                                               const caption::Style &style);
    static QVector<EnhancedTextOverlay> buildSingleWordOverlays(
        const caption::Track &track, const caption::Style &style)
    {
        return build(track, style);
    }

    // Matches the normal text-baker contract: [startTime, endTime).
    static bool isActiveAt(const EnhancedTextOverlay &overlay, double timeSeconds);

    // Generated captions are stored as one start-time-sorted suffix after
    // ordinary authored titles.  Return the suffix boundary and the one
    // word active at timeSeconds in O(authored titles + log(words)).
    static int generatedSuffixBegin(const QVector<EnhancedTextOverlay> &overlays);
    static int activeGeneratedOverlayIndex(
        const QVector<EnhancedTextOverlay> &overlays, double timeSeconds);
};
