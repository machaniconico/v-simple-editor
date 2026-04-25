#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;

// Modeless dialog showing the active proxy generation. Created on demand
// by MainWindow when ProxyManager emits proxyStarted, hidden again on
// proxyFinished / proxyCancelled. Cancel button forwards to
// ProxyManager::cancelGeneration().
class ProxyProgressDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProxyProgressDialog(QWidget *parent = nullptr);

public slots:
    void onProxyStarted(const QString &clipName);
    void onProxyProgress(const QString &clipName, int percent);
    void onProxyFinished(const QString &clipName, bool ok);
    void onProxyCancelled(const QString &clipName);

signals:
    void cancelRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QLabel *m_clipLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QLabel *m_statusLabel = nullptr;
};
