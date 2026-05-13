#pragma once
#include <QString>
#include <QList>

namespace caption {

struct Clip {
    qint64  startMs = 0;
    qint64  endMs   = 0;
    QString text;
    QString actor;  // 話者識別 (optional, 空文字 OK)

    qint64 durationMs() const { return endMs - startMs; }
    bool   isValid()    const { return endMs > startMs && !text.isEmpty(); }
};

class Track {
public:
    Track() = default;

    void addClip(const Clip& c);
    void removeClipAt(int index);
    void updateClip(int index, const Clip& c);
    void clear();

    QList<Clip> clips() const;
    Clip        clipAt(int index) const;
    int         clipCount() const;

    // 該当時刻に表示すべき clip 群 (startMs <= t < endMs)
    QList<Clip> clipsAtTime(qint64 timeMs) const;

    void sortByStart();

private:
    QList<Clip> m_clips;
};

} // namespace caption
