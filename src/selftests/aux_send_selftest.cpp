#include "../AudioBusPanel.h"
#include "../AudioBusRouting.h"
#include "../Timeline.h"
#include "../UndoManager.h"
#include "SelftestGate.h"

#include <QApplication>
#include <QListWidget>
#include <QSignalSpy>
#include <QTableWidget>
#include <cmath>

namespace {
// Mirrors the MainWindow wiring: the panel records one undo point per send edit
// (sendEdited, before routingChanged) and routing travels through the bridge.
struct Harness {
    Timeline timeline;
    audiobus::AudioBusRouting routing;
    AudioBusPanel panel;

    Harness()
    {
        routing.addBus(QStringLiteral("A"));
        routing.addBus(QStringLiteral("B"));
        timeline.setBusRoutingBridge(
            [this] { return routing.toJson(); },
            [this](const QJsonObject &json) {
                routing.fromJson(json);
                panel.setRouting(&routing);
            });
        panel.setTrackCountProvider([this] { return timeline.audioTrackCount(); });
        panel.setRouting(&routing);
        QObject::connect(&panel, &AudioBusPanel::routingChanged,
                         &timeline, &Timeline::absorbIntoCurrentUndoPoint);
        QObject::connect(&panel, &AudioBusPanel::sendEdited, &timeline, [this] {
            timeline.saveUndoState(QStringLiteral("AUX センドを変更"));
        });
        timeline.undoManager()->clear();
        timeline.saveUndoState(QStringLiteral("基準"));
        if (auto *list = panel.findChild<QListWidget *>(QStringLiteral("audioBusList")))
            list->setCurrentRow(1);  // bus id 1
    }
    QTableWidget *table() { return panel.findChild<QTableWidget *>(QStringLiteral("auxSendTable")); }
    quint64 serial() { return timeline.undoManager()->saveSerial(); }
};

bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }
}

int runAuxSendSelftest()
{
    selftest::GateTally tally;
    {
        Harness h;
        QTableWidget *table = h.table();
        const quint64 before = h.serial();
        QSignalSpy edited(&h.panel, &AudioBusPanel::sendEdited);
        QSignalSpy changed(&h.panel, &AudioBusPanel::routingChanged);
        if (table && table->rowCount() > 0 && table->item(0, 1))
            table->item(0, 1)->setText(QStringLiteral("-6"));
        const auto &sends = h.routing.auxSends();
        const bool one = sends.size() == 1 && sends[0].trackIndex == 0
            && sends[0].busId == 1 && near(sends[0].sendLevel, 0.501187) && !sends[0].preFader;
        const std::string detail = QString::asprintf("sends=%d serialDelta=%llu edited=%d changed=%d",
            int(sends.size()), static_cast<unsigned long long>(h.serial() - before),
            int(edited.count()), int(changed.count())).toStdString();
        tally.gate(1, table && one && h.serial() == before + 1 && edited.count() == 1
                   && changed.count() == 1, detail);

        // G2: one undo removes the send, redo restores it.
        h.timeline.undo();
        const int undoneSends = int(h.routing.auxSends().size());
        h.timeline.redo();
        const int redoneSends = int(h.routing.auxSends().size());
        const int redoneBus = redoneSends == 1 ? h.routing.auxSends()[0].busId : -1;
        const double redoneLevel = redoneSends == 1 ? h.routing.auxSends()[0].sendLevel : -1.0;
        tally.gate(2, undoneSends == 0 && redoneSends == 1 && redoneBus == 1
                   && near(redoneLevel, 0.501187),
                   QString::asprintf("undoneSends=%d redoneSends=%d redoneBus=%d redoneLevel=%.6f",
                                     undoneSends, redoneSends, redoneBus, redoneLevel).toStdString());

        // G3 (negative controls): a fractional level (0.5 shows as "-6.0") must survive a
        // pre-fader toggle, a hidden refresh writes nothing, invalid text is rejected,
        // and -60 dB removes the entry.
        h.routing.addAuxSend({0, 1, 0.5, false});
        h.panel.refresh();
        const quint64 serialF = h.serial();
        edited.clear();
        changed.clear();
        table->item(0, 2)->setCheckState(Qt::Checked);
        const bool kept = h.routing.auxSends().size() == 1;
        const double keptLevel = kept ? h.routing.auxSends()[0].sendLevel : -1.0;
        const bool keptPre = kept && h.routing.auxSends()[0].preFader;
        const qint64 serialDelta = qint64(h.serial()) - qint64(serialF);
        const bool toggled = kept && std::fabs(keptLevel - 0.5) < 1e-9 && keptPre && serialDelta == 1;

        h.panel.hide();
        const QJsonObject snapshot = h.routing.toJson();
        const quint64 serial0 = h.serial();
        edited.clear();
        changed.clear();
        h.panel.refresh();
        const bool quiet = h.routing.toJson() == snapshot && h.serial() == serial0
            && edited.count() == 0 && changed.count() == 0;
        table->item(0, 1)->setText(QStringLiteral("abc"));
        const bool rejected = h.routing.toJson() == snapshot && h.serial() == serial0
            && edited.count() == 0 && table->item(0, 1)->text() == QStringLiteral("-6.0");
        table->item(0, 1)->setText(QStringLiteral("-60"));
        const bool removed = h.routing.auxSends().isEmpty() && h.serial() == serial0 + 1
            && edited.count() == 1;
        tally.gate(3, toggled && quiet && rejected && removed,
                   QString::asprintf("toggled=%d level=%.9f preFader=%d serialDelta=%lld quiet=%d "
                                     "rejected=%d removed=%d", int(toggled), keptLevel, int(keptPre),
                                     static_cast<long long>(serialDelta), int(quiet), int(rejected),
                                     int(removed)).toStdString());
    }
    return tally.finish("aux-send");
}
