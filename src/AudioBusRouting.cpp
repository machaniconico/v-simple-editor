// AudioBusRouting.cpp — AB-1: バス / サブミックス / AUX センドの純粋
// ルーティング & ゲイン解決エンジン。詳細仕様は AudioBusRouting.h を参照。

#include "AudioBusRouting.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>

namespace audiobus {

// ---------------------------------------------------------------------------
// 内部ヘルパー
// ---------------------------------------------------------------------------

int AudioBusRouting::indexOfBus(int busId) const
{
    if (busId < 0)
        return -1;
    for (int i = 0; i < m_buses.size(); ++i) {
        if (m_buses[i].id == busId)
            return i;
    }
    return -1;
}

const AudioBus* AudioBusRouting::bus(int busId) const
{
    const int idx = indexOfBus(busId);
    return idx >= 0 ? &m_buses[idx] : nullptr;
}

bool AudioBusRouting::anySolo() const
{
    for (const AudioBus& b : m_buses) {
        if (b.solo)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// バス CRUD
// ---------------------------------------------------------------------------

int AudioBusRouting::addBus(const QString& name)
{
    AudioBus b;
    b.id = m_nextBusId++;
    b.name = name;
    b.gain = 1.0;
    b.mute = false;
    b.solo = false;
    b.outputBusId = -1;   // master 直結で生成
    m_buses.append(b);
    return b.id;
}

bool AudioBusRouting::removeBus(int busId)
{
    const int idx = indexOfBus(busId);
    if (idx < 0)
        return false;

    m_buses.removeAt(idx);

    // このバスを出力先にしていた他バスを master(-1) に戻す。
    for (AudioBus& b : m_buses) {
        if (b.outputBusId == busId)
            b.outputBusId = -1;
    }

    // このバスを割当先にしていたトラックを master(-1) に戻す。
    for (auto it = m_trackBus.begin(); it != m_trackBus.end(); ++it) {
        if (it.value() == busId)
            it.value() = -1;
    }

    // このバスへの aux send を全て除去する。
    for (int i = m_auxSends.size() - 1; i >= 0; --i) {
        if (m_auxSends[i].busId == busId)
            m_auxSends.removeAt(i);
    }

    return true;
}

bool AudioBusRouting::renameBus(int busId, const QString& name)
{
    const int idx = indexOfBus(busId);
    if (idx < 0)
        return false;
    m_buses[idx].name = name;
    return true;
}

bool AudioBusRouting::setBusGain(int busId, double gain)
{
    if (gain < 0.0)
        return false;
    const int idx = indexOfBus(busId);
    if (idx < 0)
        return false;
    m_buses[idx].gain = gain;
    return true;
}

bool AudioBusRouting::setBusMute(int busId, bool mute)
{
    const int idx = indexOfBus(busId);
    if (idx < 0)
        return false;
    m_buses[idx].mute = mute;
    return true;
}

bool AudioBusRouting::setBusSolo(int busId, bool solo)
{
    const int idx = indexOfBus(busId);
    if (idx < 0)
        return false;
    m_buses[idx].solo = solo;
    return true;
}

// ---------------------------------------------------------------------------
// トラック割当
// ---------------------------------------------------------------------------

bool AudioBusRouting::assignTrackToBus(int trackIndex, int busId)
{
    if (busId < 0) {
        // master 直結に戻す。
        m_trackBus.insert(trackIndex, -1);
        return true;
    }
    if (indexOfBus(busId) < 0)
        return false;
    m_trackBus.insert(trackIndex, busId);
    return true;
}

int AudioBusRouting::trackBus(int trackIndex) const
{
    return m_trackBus.value(trackIndex, -1);
}

// ---------------------------------------------------------------------------
// AUX センド
// ---------------------------------------------------------------------------

void AudioBusRouting::addAuxSend(const AuxSend& send)
{
    for (AuxSend& s : m_auxSends) {
        if (s.trackIndex == send.trackIndex && s.busId == send.busId) {
            s.sendLevel = send.sendLevel;
            s.preFader = send.preFader;
            return;
        }
    }
    m_auxSends.append(send);
}

bool AudioBusRouting::removeAuxSend(int trackIndex, int busId)
{
    for (int i = 0; i < m_auxSends.size(); ++i) {
        if (m_auxSends[i].trackIndex == trackIndex
            && m_auxSends[i].busId == busId) {
            m_auxSends.removeAt(i);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// ゲイン解決
// ---------------------------------------------------------------------------

double AudioBusRouting::chainGainToMaster(int startBusId, bool soloActive) const
{
    // master 直結 (startBusId<0): 連鎖バス無し。係数 1.0。ただし solo がアクティブ
    // のとき、この経路は solo バスを 1 つも通らないので 0。
    if (startBusId < 0)
        return soloActive ? 0.0 : 1.0;

    double gain = 1.0;
    bool passedSolo = false;
    QSet<int> visited;

    int cur = startBusId;
    while (cur >= 0) {
        if (visited.contains(cur)) {
            // 循環検出: ここで安全に打ち切る (打ち切り時点の積を返す)。
            break;
        }
        visited.insert(cur);

        const int idx = indexOfBus(cur);
        if (idx < 0) {
            // 連鎖の途中で存在しないバスを指している = 無効経路。
            return 0.0;
        }
        const AudioBus& b = m_buses[idx];

        if (b.mute)
            return 0.0;
        if (b.solo)
            passedSolo = true;

        gain *= b.gain;
        cur = b.outputBusId;
    }

    // solo 規則: solo がアクティブなのに経路上で solo バスを 1 つも通っていない
    // 場合、この経路は実効的に 0。
    if (soloActive && !passedSolo)
        return 0.0;

    return gain;
}

double AudioBusRouting::resolveTrackToMasterGain(int trackIndex) const
{
    const bool soloActive = anySolo();
    const int busId = trackBus(trackIndex);
    // busId<0 (未割当 / master 直結): solo 非アクティブなら 1.0 (identity 保証)、
    // solo アクティブなら solo を通らないため 0。
    return chainGainToMaster(busId, soloActive);
}

double AudioBusRouting::resolveAuxSendGain(int trackIndex, int busId) const
{
    // 該当 aux send を探す。
    const AuxSend* found = nullptr;
    for (const AuxSend& s : m_auxSends) {
        if (s.trackIndex == trackIndex && s.busId == busId) {
            found = &s;
            break;
        }
    }
    if (!found)
        return 0.0;

    // 送り先バスが存在しなければ 0。
    if (indexOfBus(busId) < 0)
        return 0.0;

    const bool soloActive = anySolo();
    const double chain = chainGainToMaster(busId, soloActive);
    return found->sendLevel * chain;
}

bool AudioBusRouting::hasCycle() const
{
    // 各バスを起点に outputBusId 連鎖をたどり、訪問済みに再到達したら閉路。
    for (const AudioBus& start : m_buses) {
        QSet<int> visited;
        int cur = start.id;
        while (cur >= 0) {
            if (visited.contains(cur))
                return true;
            visited.insert(cur);
            const int idx = indexOfBus(cur);
            if (idx < 0)
                break;   // 無効参照 = 連鎖終端 (閉路ではない)
            cur = m_buses[idx].outputBusId;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 直列化
// ---------------------------------------------------------------------------

QJsonObject AudioBusRouting::toJson() const
{
    QJsonObject root;
    root.insert(QStringLiteral("nextBusId"), m_nextBusId);

    QJsonArray busArr;
    for (const AudioBus& b : m_buses) {
        QJsonObject bo;
        bo.insert(QStringLiteral("id"), b.id);
        bo.insert(QStringLiteral("name"), b.name);
        bo.insert(QStringLiteral("gain"), b.gain);
        bo.insert(QStringLiteral("mute"), b.mute);
        bo.insert(QStringLiteral("solo"), b.solo);
        bo.insert(QStringLiteral("outputBusId"), b.outputBusId);
        busArr.append(bo);
    }
    root.insert(QStringLiteral("buses"), busArr);

    QJsonArray sendArr;
    for (const AuxSend& s : m_auxSends) {
        QJsonObject so;
        so.insert(QStringLiteral("trackIndex"), s.trackIndex);
        so.insert(QStringLiteral("busId"), s.busId);
        so.insert(QStringLiteral("sendLevel"), s.sendLevel);
        so.insert(QStringLiteral("preFader"), s.preFader);
        sendArr.append(so);
    }
    root.insert(QStringLiteral("auxSends"), sendArr);

    QJsonArray trackArr;
    for (auto it = m_trackBus.constBegin(); it != m_trackBus.constEnd(); ++it) {
        QJsonObject to;
        to.insert(QStringLiteral("trackIndex"), it.key());
        to.insert(QStringLiteral("busId"), it.value());
        trackArr.append(to);
    }
    root.insert(QStringLiteral("trackBus"), trackArr);

    return root;
}

void AudioBusRouting::fromJson(const QJsonObject& obj)
{
    clear();

    const QJsonArray busArr = obj.value(QStringLiteral("buses")).toArray();
    for (const QJsonValue& v : busArr) {
        const QJsonObject bo = v.toObject();
        AudioBus b;
        b.id = bo.value(QStringLiteral("id")).toInt(-1);
        b.name = bo.value(QStringLiteral("name")).toString();
        b.gain = bo.value(QStringLiteral("gain")).toDouble(1.0);
        b.mute = bo.value(QStringLiteral("mute")).toBool(false);
        b.solo = bo.value(QStringLiteral("solo")).toBool(false);
        b.outputBusId = bo.value(QStringLiteral("outputBusId")).toInt(-1);
        m_buses.append(b);
    }

    const QJsonArray sendArr = obj.value(QStringLiteral("auxSends")).toArray();
    for (const QJsonValue& v : sendArr) {
        const QJsonObject so = v.toObject();
        AuxSend s;
        s.trackIndex = so.value(QStringLiteral("trackIndex")).toInt(-1);
        s.busId = so.value(QStringLiteral("busId")).toInt(-1);
        s.sendLevel = so.value(QStringLiteral("sendLevel")).toDouble(1.0);
        s.preFader = so.value(QStringLiteral("preFader")).toBool(false);
        m_auxSends.append(s);
    }

    const QJsonArray trackArr = obj.value(QStringLiteral("trackBus")).toArray();
    for (const QJsonValue& v : trackArr) {
        const QJsonObject to = v.toObject();
        const int trackIndex = to.value(QStringLiteral("trackIndex")).toInt(-1);
        const int busId = to.value(QStringLiteral("busId")).toInt(-1);
        m_trackBus.insert(trackIndex, busId);
    }

    // nextBusId は永続化値を尊重しつつ、既存 id の最大+1 を下限とする
    // (古い JSON や手編集で齟齬があっても採番衝突しないように)。
    int next = obj.value(QStringLiteral("nextBusId")).toInt(0);
    for (const AudioBus& b : m_buses) {
        if (b.id >= next)
            next = b.id + 1;
    }
    m_nextBusId = next;
}

void AudioBusRouting::clear()
{
    m_buses.clear();
    m_auxSends.clear();
    m_trackBus.clear();
    m_nextBusId = 0;
}

} // namespace audiobus
