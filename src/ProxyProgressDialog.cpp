#include "ProxyProgressDialog.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
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
    m_progressBar->setRange(0, 0); // indeterminate until first percent arrives

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

    // Auto-hide timer used after a cancel completes. Stored as a member
    // (not QTimer::singleShot) so onProxyStarted / onProxyFinished can
    // stop a pending hide if the user immediately kicks off another
    // generation — otherwise the 800ms timer would fire mid-new-job and
    // pull the dialog out from under it.
    m_autoHideTimer = new QTimer(this);
    m_autoHideTimer->setSingleShot(true);
    m_autoHideTimer->setInterval(800);
    connect(m_autoHideTimer, &QTimer::timeout, this, &QDialog::hide);
}

void ProxyProgressDialog::onProxyStarted(const QString &clipName)
{
    // Cancel any pending auto-hide from a prior cancel — otherwise the
    // 800ms timer scheduled in onProxyCancelled would fire mid-new-job.
    m_autoHideTimer->stop();
    m_clipLabel->setText(tr("処理中: %1").arg(clipName));
    m_progressBar->setRange(0, 0); // indeterminate until first percent arrives
    m_progressBar->setValue(0);
    m_statusLabel->clear();

    // Reset the cancel button to its active state. onProxyFinished /
    // onProxyCancelled rewire it to「閉じる」+ accept; without this
    // restoration a follow-up generation would inherit the stale wiring
    // and the user would lose the ability to cancel — the button just
    // closes the dialog while the new ffmpeg job keeps running silently
    // in the background.
    m_cancelButton->setText(tr("キャンセル"));
    m_cancelButton->setEnabled(true);
    disconnect(m_cancelButton, nullptr, nullptr, nullptr);
    connect(m_cancelButton, &QPushButton::clicked, this, [this]() {
        m_statusLabel->setText(tr("キャンセル中..."));
        m_cancelButton->setEnabled(false);
        emit cancelRequested();
    });

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
    m_autoHideTimer->stop();
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
    // Auto-hide so the user doesn't have to manually dismiss a residual
    // "cancelled" dialog before kicking off a new generation. hide() (not
    // accept()) keeps the QDialog instance alive so onProxyStarted can
    // resurrect it for the next run. The timer is a member so a fresh
    // generation kicked off within 800ms can stop() it before it fires.
    m_autoHideTimer->start();
}

void ProxyProgressDialog::closeEvent(QCloseEvent *event)
{
    // If a generation is in flight, treat close as cancel request rather
    // than orphaning the ffmpeg child process. We detect "in flight" by
    // the cancel button still saying its initial label — onProxyFinished /
    // onProxyCancelled both rewrite the button text to「閉じる」, so any
    // other text means we're still mid-job regardless of determinate vs
    // indeterminate progress bar mode.
    if (m_cancelButton->text() == tr("キャンセル") && m_cancelButton->isEnabled()) {
        emit cancelRequested();
    }
    QDialog::closeEvent(event);
}
