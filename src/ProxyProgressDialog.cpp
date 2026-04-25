#include "ProxyProgressDialog.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

ProxyProgressDialog::ProxyProgressDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("プロキシ生成中"));
    setModal(false);
    setMinimumWidth(420);

    m_clipLabel = new QLabel(tr("待機中..."), this);
    m_clipLabel->setWordWrap(true);

    m_progressBar = new QProgressBar(this);
    // Indeterminate by default — switches to determinate when a percent
    // arrives via onProxyProgress.
    m_progressBar->setRange(0, 0);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #888;");

    m_cancelButton = new QPushButton(tr("キャンセル"), this);
    connect(m_cancelButton, &QPushButton::clicked, this, [this]() {
        m_statusLabel->setText(tr("キャンセル中..."));
        m_cancelButton->setEnabled(false);
        emit cancelRequested();
    });

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_cancelButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_clipLabel);
    layout->addWidget(m_progressBar);
    layout->addWidget(m_statusLabel);
    layout->addLayout(buttons);
}

void ProxyProgressDialog::onProxyStarted(const QString &clipName)
{
    m_clipLabel->setText(tr("処理中: %1").arg(clipName));
    m_progressBar->setRange(0, 0); // indeterminate until first percent arrives
    m_progressBar->setValue(0);
    m_statusLabel->clear();
    m_cancelButton->setEnabled(true);
    if (!isVisible())
        show();
    raise();
    activateWindow();
}

void ProxyProgressDialog::onProxyProgress(const QString &clipName, int percent)
{
    if (!clipName.isEmpty())
        m_clipLabel->setText(tr("処理中: %1").arg(clipName));
    if (percent < 0) {
        m_progressBar->setRange(0, 0);
        return;
    }
    if (m_progressBar->maximum() == 0)
        m_progressBar->setRange(0, 100);
    m_progressBar->setValue(percent);
}

void ProxyProgressDialog::onProxyFinished(const QString &clipName, bool ok)
{
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(ok ? 100 : 0);
    m_statusLabel->setText(ok ? tr("完了: %1").arg(clipName)
                              : tr("エラー: %1").arg(clipName));
    m_cancelButton->setText(tr("閉じる"));
    m_cancelButton->setEnabled(true);
    disconnect(m_cancelButton, nullptr, nullptr, nullptr);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::accept);
}

void ProxyProgressDialog::onProxyCancelled(const QString &clipName)
{
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_statusLabel->setText(tr("キャンセル済み: %1").arg(clipName));
    m_cancelButton->setText(tr("閉じる"));
    m_cancelButton->setEnabled(true);
    disconnect(m_cancelButton, nullptr, nullptr, nullptr);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::accept);
}

void ProxyProgressDialog::closeEvent(QCloseEvent *event)
{
    // If a generation is in flight, treat close as cancel request rather
    // than orphaning the ffmpeg child process.
    if (m_progressBar->maximum() == 0 && m_cancelButton->isEnabled()) {
        emit cancelRequested();
    }
    QDialog::closeEvent(event);
}
