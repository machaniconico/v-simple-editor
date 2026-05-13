#include "CaptionTrack.h"

#include <algorithm>

namespace caption {

void Track::addClip(const Clip& c)
{
    m_clips.append(c);
}

void Track::removeClipAt(int index)
{
    if (index < 0 || index >= m_clips.size())
        return;
    m_clips.removeAt(index);
}

void Track::updateClip(int index, const Clip& c)
{
    if (index < 0 || index >= m_clips.size())
        return;
    m_clips[index] = c;
}

void Track::clear()
{
    m_clips.clear();
}

QList<Clip> Track::clips() const
{
    return m_clips;
}

Clip Track::clipAt(int index) const
{
    if (index < 0 || index >= m_clips.size())
        return Clip{};
    return m_clips[index];
}

int Track::clipCount() const
{
    return m_clips.size();
}

QList<Clip> Track::clipsAtTime(qint64 timeMs) const
{
    QList<Clip> result;
    for (const Clip& c : m_clips) {
        if (c.startMs <= timeMs && timeMs < c.endMs)
            result.append(c);
    }
    return result;
}

void Track::sortByStart()
{
    std::stable_sort(m_clips.begin(), m_clips.end(),
        [](const Clip& a, const Clip& b) { return a.startMs < b.startMs; });
}

} // namespace caption
