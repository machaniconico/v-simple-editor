#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QDateTime>
#include <QHash>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>
#include <QDialog>
#include <QTabWidget>
#include <QTableWidget>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QCheckBox>
#include <QProgressBar>
#include <QSplitter>
#include <QGroupBox>

// ─────────────────────────────────────────────
// Data structures
// ─────────────────────────────────────────────

enum class NetworkJobStatus {
    Pending,
    Assigned,
    Rendering,
    Complete,
    Failed
};

struct RenderJobNet {
    QString jobId;           // UUID string
    QString projectFilePath;
    QString outputPath;
    int startFrame = 0;
    int endFrame   = 0;
    QString exportPreset;
    NetworkJobStatus status = NetworkJobStatus::Pending;
    double progress = 0.0;   // 0–100
    QString assignedNode;    // nodeId of assigned RenderNode
};

enum class NodeStatus {
    Idle,
    Busy,
    Offline
};

struct RenderNode {
    QString nodeId;
    QString hostname;
    QString ipAddress;
    quint16 port = 0;
    NodeStatus status = NodeStatus::Offline;
    int cpuCores = 0;
    QString gpuName;
    QString currentJob;      // jobId currently assigned
    QDateTime lastSeen;
};

// ─────────────────────────────────────────────
// NetworkRenderServer
// ─────────────────────────────────────────────

class NetworkRenderServer : public QObject
{
    Q_OBJECT

public:
    explicit NetworkRenderServer(QObject *parent = nullptr);
    ~NetworkRenderServer();

    bool startServer(quint16 port = 9920);
    void stopServer();
    bool isListening() const;
    quint16 serverPort() const;

    QString addRenderJob(const RenderJobNet &job);
    void cancelJob(const QString &jobId);
    void distributeJobs();

    QVector<RenderJobNet> jobs() const { return m_jobs; }
    QVector<RenderNode>   nodes() const;

    // Segment size used by distributeJobs()
    void setSegmentSize(int frames) { m_segmentSize = qMax(1, frames); }
    int  segmentSize() const { return m_segmentSize; }

    void setAutoDistribute(bool on) { m_autoDistribute = on; }
    bool autoDistribute() const { return m_autoDistribute; }

signals:
    void nodeConnected(RenderNode node);
    void nodeDisconnected(RenderNode node);
    void jobProgress(QString jobId, double progress);
    void jobComplete(QString jobId, QString outputPath);
    void allJobsComplete();

private slots:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void onHeartbeatTimeout();

private:
    void handleMessage(QTcpSocket *socket, const QJsonObject &msg);
    void handleHello(QTcpSocket *socket, const QJsonObject &msg);
    void handleHeartbeat(QTcpSocket *socket, const QJsonObject &msg);
    void handleJobProgress(QTcpSocket *socket, const QJsonObject &msg);
    void handleJobComplete(QTcpSocket *socket, const QJsonObject &msg);
    void handleJobFailed(QTcpSocket *socket, const QJsonObject &msg);

    void sendMessage(QTcpSocket *socket, const QJsonObject &msg);
    void assignJobToNode(const QString &jobId, const QString &nodeId);
    bool tryAssignPendingJob(const QString &nodeId);
    void checkAllComplete();
    void assembleSegments(const QString &masterJobId);

    QString nodeIdForSocket(QTcpSocket *socket) const;
    RenderNode *findNode(const QString &nodeId);
    RenderJobNet *findJob(const QString &jobId);

    QTcpServer *m_server = nullptr;

    // socket → nodeId
    QHash<QTcpSocket *, QString> m_socketToNode;
    // nodeId → socket
    QHash<QString, QTcpSocket *> m_nodeToSocket;
    // nodeId → RenderNode
    QHash<QString, RenderNode>   m_nodes;

    QVector<RenderJobNet> m_jobs;

    // Per-socket read buffer (newline-delimited JSON)
    QHash<QTcpSocket *, QByteArray> m_readBuffers;

    QTimer *m_heartbeatTimer = nullptr;

    int  m_segmentSize    = 300; // frames per chunk
    bool m_autoDistribute = true;

    // Segment tracking: masterJobId → list of segment jobIds
    QHash<QString, QStringList> m_segmentMap;
    // segmentJobId → masterJobId
    QHash<QString, QString>     m_segmentParent;

};

// ─────────────────────────────────────────────
// NetworkRenderClient
// ─────────────────────────────────────────────

class NetworkRenderClient : public QObject
{
    Q_OBJECT

public:
    explicit NetworkRenderClient(QObject *parent = nullptr);
    ~NetworkRenderClient();

    void connectToServer(const QString &host, quint16 port);
    void disconnectFromServer();
    bool isConnected() const;

    // Client self-description (set before connecting)
    void setHostname(const QString &hostname) { m_hostname = hostname; }
    void setCpuCores(int cores)               { m_cpuCores = cores; }
    void setGpuName(const QString &gpu)       { m_gpuName  = gpu; }

    // Call this to report progress for the currently executing job
    void reportProgress(const QString &jobId, double progress);
    void reportComplete(const QString &jobId, const QString &outputPath);
    void reportFailed(const QString &jobId, const QString &error);

signals:
    void connected();
    void disconnected();
    void jobReceived(RenderJobNet job);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onHeartbeat();

private:
    void handleMessage(const QJsonObject &msg);
    void sendMessage(const QJsonObject &msg);
    void sendHello();

    QTcpSocket *m_socket    = nullptr;
    QTimer     *m_heartbeat = nullptr;
    QByteArray  m_readBuffer;

    QString m_hostname;
    QString m_nodeId;
    int     m_cpuCores = 1;
    QString m_gpuName;
};

// ─────────────────────────────────────────────
// NetworkRenderDialog
// ─────────────────────────────────────────────

class NetworkRenderDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NetworkRenderDialog(NetworkRenderServer *server, QWidget *parent = nullptr);

private slots:
    void onStartServer();
    void onStopServer();
    void onAddJob();
    void onCancelJob();

    void onNodeConnected(const RenderNode &node);
    void onNodeDisconnected(const RenderNode &node);
    void onJobProgress(const QString &jobId, double progress);
    void onJobComplete(const QString &jobId, const QString &outputPath);
    void onAllJobsComplete();

    void refreshNodesTable();
    void refreshJobsTable();
    void updateThroughput();

private:
    void setupUI();
    void applySettings();
    int findNodeRow(const QString &nodeId) const;
    int findJobRow(const QString &jobId) const;
    QString statusText(NetworkJobStatus s) const;
    QString nodeStatusText(NodeStatus s) const;

    NetworkRenderServer *m_server;

    // Tabs
    QTabWidget   *m_tabs;

    // Tab 1 – Nodes
    QTableWidget *m_nodesTable;

    // Tab 2 – Jobs
    QTableWidget *m_jobsTable;

    // Tab 3 – Settings
    QSpinBox  *m_portSpin;
    QCheckBox *m_autoDistributeCheck;
    QSpinBox  *m_segmentSizeSpin;

    // Bottom bar
    QPushButton *m_startBtn;
    QPushButton *m_stopBtn;
    QPushButton *m_addJobBtn;
    QPushButton *m_cancelJobBtn;

    QLabel      *m_statusLabel;
    QLabel      *m_throughputLabel;
    QProgressBar *m_overallProgress;

    QTimer *m_refreshTimer = nullptr;
};
