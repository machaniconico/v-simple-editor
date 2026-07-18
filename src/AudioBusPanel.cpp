#include "AudioBusPanel.h"

#include <QListWidget>
#include <QLineEdit>
#include <QSlider>
#include <QToolButton>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QWidget>
#include <QSignalBlocker>
#include <QJsonObject>
#include <QJsonArray>

#include <cmath>

// gain スライダは 0..200 の整数で、100 が線形 1.0 (0 dB)。0 は無音、200 は
// 2.0 (約 +6 dB)。線形マッピングなので AudioBusRouting の純粋ゲイン積と素直に
// 噛み合う (dB ではなく線形係数を直接持つモデルのため)。
double AudioBusPanel::sliderToGain(int value)
{
    return static_cast<double>(value) / 100.0;
}

int AudioBusPanel::gainToSlider(double gain)
{
    int v = static_cast<int>(std::lround(gain * 100.0));
    if (v < 0) v = 0;
    if (v > 200) v = 200;
    return v;
}

AudioBusPanel::AudioBusPanel(QWidget *parent)
    : QDockWidget(QStringLiteral("オーディオ バス"), parent)
{
    setObjectName(QStringLiteral("AudioBusPanel"));

    auto *root = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    // --- バス一覧 ---------------------------------------------------------
    m_busList = new QListWidget(root);
    m_busList->setSelectionMode(QAbstractItemView::SingleSelection);
    rootLayout->addWidget(m_busList, /*stretch*/ 1);
    connect(m_busList, &QListWidget::itemSelectionChanged,
            this, &AudioBusPanel::onBusSelectionChanged);

    // --- 選択ストリップ ---------------------------------------------------
    m_stripWidget = new QGroupBox(QStringLiteral("選択中のバス"), root);
    auto *form = new QFormLayout(m_stripWidget);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);

    m_nameEdit = new QLineEdit(m_stripWidget);
    m_nameEdit->setPlaceholderText(QStringLiteral("バス名"));
    connect(m_nameEdit, &QLineEdit::editingFinished,
            this, &AudioBusPanel::onRenameBus);
    form->addRow(QStringLiteral("名前"), m_nameEdit);

    auto *gainRow = new QWidget(m_stripWidget);
    auto *gainLayout = new QHBoxLayout(gainRow);
    gainLayout->setContentsMargins(0, 0, 0, 0);
    gainLayout->setSpacing(6);
    m_gainSlider = new QSlider(Qt::Horizontal, gainRow);
    m_gainSlider->setRange(0, 200);
    m_gainSlider->setValue(100);
    connect(m_gainSlider, &QSlider::valueChanged,
            this, &AudioBusPanel::onGainSliderChanged);
    m_gainLabel = new QLabel(QStringLiteral("1.00"), gainRow);
    m_gainLabel->setMinimumWidth(40);
    m_gainLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    gainLayout->addWidget(m_gainSlider, /*stretch*/ 1);
    gainLayout->addWidget(m_gainLabel);
    form->addRow(QStringLiteral("ゲイン"), gainRow);

    auto *toggleRow = new QWidget(m_stripWidget);
    auto *toggleLayout = new QHBoxLayout(toggleRow);
    toggleLayout->setContentsMargins(0, 0, 0, 0);
    toggleLayout->setSpacing(6);
    m_muteButton = new QToolButton(toggleRow);
    m_muteButton->setText(QStringLiteral("ミュート"));
    m_muteButton->setCheckable(true);
    connect(m_muteButton, &QToolButton::toggled,
            this, &AudioBusPanel::onMuteToggled);
    m_soloButton = new QToolButton(toggleRow);
    m_soloButton->setText(QStringLiteral("ソロ"));
    m_soloButton->setCheckable(true);
    connect(m_soloButton, &QToolButton::toggled,
            this, &AudioBusPanel::onSoloToggled);
    toggleLayout->addWidget(m_muteButton);
    toggleLayout->addWidget(m_soloButton);
    toggleLayout->addStretch(1);
    form->addRow(QStringLiteral("状態"), toggleRow);

    m_outputCombo = new QComboBox(m_stripWidget);
    connect(m_outputCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AudioBusPanel::onOutputBusChanged);
    form->addRow(QStringLiteral("出力先"), m_outputCombo);

    rootLayout->addWidget(m_stripWidget);

    // --- 追加 / 削除 ------------------------------------------------------
    auto *buttonRow = new QWidget(root);
    auto *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(6);
    m_addButton = new QPushButton(QStringLiteral("バス追加"), buttonRow);
    connect(m_addButton, &QPushButton::clicked, this, &AudioBusPanel::onAddBus);
    m_removeButton = new QPushButton(QStringLiteral("削除"), buttonRow);
    connect(m_removeButton, &QPushButton::clicked, this, &AudioBusPanel::onRemoveBus);
    buttonLayout->addWidget(m_addButton);
    buttonLayout->addWidget(m_removeButton);
    buttonLayout->addStretch(1);
    rootLayout->addWidget(buttonRow);

    setWidget(root);
    refresh();
}

void AudioBusPanel::setRouting(audiobus::AudioBusRouting *routing)
{
    m_routing = routing;
    refresh();
}

int AudioBusPanel::selectedBusId() const
{
    if (!m_busList)
        return -1;
    QListWidgetItem *item = m_busList->currentItem();
    if (!item)
        return -1;
    bool ok = false;
    const int id = item->data(Qt::UserRole).toInt(&ok);
    return ok ? id : -1;
}

void AudioBusPanel::refresh()
{
    if (!m_busList)
        return;

    // 再構築中はリスト選択シグナルから来る updateStrip 等を抑止する。
    QSignalBlocker listBlocker(m_busList);
    m_updating = true;

    const int previouslySelected = selectedBusId();
    m_busList->clear();

    if (m_routing) {
        const QVector<audiobus::AudioBus> &buses = m_routing->buses();
        for (const audiobus::AudioBus &b : buses) {
            QString flags;
            if (b.mute) flags += QStringLiteral(" [M]");
            if (b.solo) flags += QStringLiteral(" [S]");
            const QString label = QStringLiteral("%1   x%2%3")
                                      .arg(b.name.isEmpty() ? QStringLiteral("(無名)") : b.name)
                                      .arg(b.gain, 0, 'f', 2)
                                      .arg(flags);
            auto *item = new QListWidgetItem(label, m_busList);
            item->setData(Qt::UserRole, b.id);
        }
    }

    // 直前の選択を可能なら復元する。
    if (previouslySelected >= 0) {
        for (int i = 0; i < m_busList->count(); ++i) {
            if (m_busList->item(i)->data(Qt::UserRole).toInt() == previouslySelected) {
                m_busList->setCurrentRow(i);
                break;
            }
        }
    }
    if (m_busList->currentRow() < 0 && m_busList->count() > 0)
        m_busList->setCurrentRow(0);

    m_updating = false;
    updateStrip();
}

void AudioBusPanel::rebuildOutputCombo(const audiobus::AudioBus *current)
{
    if (!m_outputCombo)
        return;
    QSignalBlocker blocker(m_outputCombo);
    m_outputCombo->clear();
    // index 0 = master (-1)。以降は自分以外の各バス。UserRole に busId を保持。
    m_outputCombo->addItem(QStringLiteral("マスター"), -1);
    int selectIndex = 0;
    if (m_routing && current) {
        const QVector<audiobus::AudioBus> &buses = m_routing->buses();
        for (const audiobus::AudioBus &b : buses) {
            if (b.id == current->id)
                continue;  // 自分自身への出力は不可 (循環防止)
            m_outputCombo->addItem(b.name.isEmpty() ? QStringLiteral("(無名)") : b.name, b.id);
            if (b.id == current->outputBusId)
                selectIndex = m_outputCombo->count() - 1;
        }
    }
    m_outputCombo->setCurrentIndex(selectIndex);
}

void AudioBusPanel::updateStrip()
{
    const int busId = selectedBusId();
    const audiobus::AudioBus *bus = (m_routing && busId >= 0) ? m_routing->bus(busId) : nullptr;
    const bool enabled = (bus != nullptr);

    // ストリップ全体と削除ボタンは選択がある時のみ有効。
    if (m_stripWidget) m_stripWidget->setEnabled(enabled);
    if (m_removeButton) m_removeButton->setEnabled(enabled);
    if (m_addButton) m_addButton->setEnabled(m_routing != nullptr);

    QSignalBlocker nameBlock(m_nameEdit);
    QSignalBlocker gainBlock(m_gainSlider);
    QSignalBlocker muteBlock(m_muteButton);
    QSignalBlocker soloBlock(m_soloButton);

    if (bus) {
        m_nameEdit->setText(bus->name);
        m_gainSlider->setValue(gainToSlider(bus->gain));
        m_gainLabel->setText(QStringLiteral("%1").arg(bus->gain, 0, 'f', 2));
        m_muteButton->setChecked(bus->mute);
        m_soloButton->setChecked(bus->solo);
    } else {
        m_nameEdit->clear();
        m_gainSlider->setValue(100);
        m_gainLabel->setText(QStringLiteral("1.00"));
        m_muteButton->setChecked(false);
        m_soloButton->setChecked(false);
    }
    rebuildOutputCombo(bus);
}

void AudioBusPanel::onBusSelectionChanged()
{
    if (m_updating)
        return;
    updateStrip();
}

void AudioBusPanel::onAddBus()
{
    if (!m_routing)
        return;
    const int newId = m_routing->addBus(QStringLiteral("バス %1").arg(m_routing->buses().size() + 1));
    refresh();
    // 追加したバスを選択する。
    if (m_busList) {
        for (int i = 0; i < m_busList->count(); ++i) {
            if (m_busList->item(i)->data(Qt::UserRole).toInt() == newId) {
                m_busList->setCurrentRow(i);
                break;
            }
        }
    }
    emit routingChanged();
}

void AudioBusPanel::onRemoveBus()
{
    if (!m_routing)
        return;
    const int busId = selectedBusId();
    if (busId < 0)
        return;
    if (!m_routing->removeBus(busId))
        return;
    refresh();
    emit routingChanged();
}

void AudioBusPanel::onRenameBus()
{
    if (m_updating || !m_routing || !m_nameEdit)
        return;
    const int busId = selectedBusId();
    if (busId < 0)
        return;
    const audiobus::AudioBus *bus = m_routing->bus(busId);
    if (!bus)
        return;
    const QString newName = m_nameEdit->text();
    if (newName == bus->name)
        return;  // 変化が無ければ no-op (editingFinished の多重発火対策)
    if (!m_routing->renameBus(busId, newName))
        return;
    refresh();
    emit routingChanged();
}

void AudioBusPanel::onGainSliderChanged(int value)
{
    if (m_updating || !m_routing)
        return;
    const int busId = selectedBusId();
    if (busId < 0)
        return;
    const double gain = sliderToGain(value);
    if (!m_routing->setBusGain(busId, gain))
        return;
    if (m_gainLabel)
        m_gainLabel->setText(QStringLiteral("%1").arg(gain, 0, 'f', 2));
    // リスト行のテキストもゲイン表示を含むので更新する。再選択を保つため
    // refresh() を使うが、選択 id は復元される。
    refresh();
    emit routingChanged();
}

void AudioBusPanel::onMuteToggled(bool checked)
{
    if (m_updating || !m_routing)
        return;
    const int busId = selectedBusId();
    if (busId < 0)
        return;
    if (!m_routing->setBusMute(busId, checked))
        return;
    refresh();
    emit routingChanged();
}

void AudioBusPanel::onSoloToggled(bool checked)
{
    if (m_updating || !m_routing)
        return;
    const int busId = selectedBusId();
    if (busId < 0)
        return;
    if (!m_routing->setBusSolo(busId, checked))
        return;
    refresh();
    emit routingChanged();
}

void AudioBusPanel::onOutputBusChanged(int comboIndex)
{
    if (m_updating || !m_routing || !m_outputCombo)
        return;
    if (comboIndex < 0)
        return;
    const int busId = selectedBusId();
    if (busId < 0)
        return;
    const audiobus::AudioBus *bus = m_routing->bus(busId);
    if (!bus)
        return;
    const int targetBusId = m_outputCombo->itemData(comboIndex).toInt();
    if (targetBusId == bus->outputBusId)
        return;
    // バスのサブミックス先 (outputBusId) を変更する。AB-1 の公開 API には
    // 専用の setBusOutput が無く、バス配列は const 参照でしか公開されていない。
    // そこで toJson / fromJson 経由で安全に書き換える: 現在の routing を JSON 化し、
    // 該当バスの outputBusId だけ差し替えて読み戻す。fromJson は nextBusId を
    // 永続化値と既存 id 最大+1 の大きい方で復元するので、採番カウンタや他バス・
    // トラック割当・auxSend を壊さずに出力先のみ変更できる。
    QJsonObject snapshot = m_routing->toJson();
    QJsonArray busesArr = snapshot.value(QStringLiteral("buses")).toArray();
    for (int i = 0; i < busesArr.size(); ++i) {
        QJsonObject o = busesArr.at(i).toObject();
        if (o.value(QStringLiteral("id")).toInt(-1) == busId) {
            o[QStringLiteral("outputBusId")] = targetBusId;
            busesArr[i] = o;
            break;
        }
    }
    snapshot[QStringLiteral("buses")] = busesArr;
    audiobus::AudioBusRouting candidate;
    candidate.fromJson(snapshot);
    if (candidate.hasCycle()) {
        refresh();
        return;
    }
    *m_routing = candidate;
    refresh();
    emit routingChanged();
}
