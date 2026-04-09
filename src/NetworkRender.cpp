#include "NetworkRender.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QThread>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QSizePolicy>

// ─────────────────────────────────────────────
// JSON message type constants
// ─────────────────────────────────────────────

static const char *MSG_HELLO         = "HELLO";
static const char *MSG_HEARTBEAT     = "HEARTBEAT";
static const char *MSG_ASSIGN_JOB    = "ASSIGN_JOB";
static const char *MSG_JOB_PROGRESS  = "JOB_PROGRESS";
static const char *MSG_JOB_COMPLETE  = "JOB_COMPLETE";
static const char *MSG_JOB_FAILED    = "JOB_FAILED";

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────

static QJsonObject jobToJson(const RenderJobNet &job)
{
    QJsonObject obj;
    obj["jobId"]           = job.jobId;
    obj["projectFilePath"] = job.projectFilePath;
    obj["outputPath"]      = job.outputPath;
    obj["startFrame"]      = job.startFrame;
    obj["endFrame"]        = job.endFrame;
    obj["exportPreset"]    = job.exportPreset;
    return obj;
}

static RenderJobNet jobFromJson(const QJsonObject &obj)
{
    RenderJobNet job;
    job.jobId           = obj["jobId"].toString();
    job.projectFilePath = obj["projectFilePath"].toString();
    job.outputPath      = obj["outputPath"].toString();
    job.startFrame      = obj["startFrame"].toInt();
    job.endFrame        = obj["endFrame"].toInt();
    job.exportPreset    = obj["exportPreset"].toString();
    return job;
}

// ═══════════════════════════════════════════════
// NetworkRenderServer
// ═══════════════════════════════════════════════

NetworkRenderServer::NetworkRenderServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection,
            this, &NetworkRenderServer::onNewConnection);

    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(10000); // check every 10 s
    connect(m_heartbeatTimer, &QTimer::timeout,
            this, &NetworkRenderServer::onHeartbeatTimeout);
}

NetworkRenderServer::~NetworkRenderServer()
{
    stopServer();
}

bool NetworkRenderServer::startServer(quint16 port)
{
    if (m_server->isListening())
        return true;

    if (!m_server->listen(QHostAddress::Any, port))
        return false;

    m_heartbeatTimer->start();
    return true;
}

void NetworkRenderServer::stopServer()
{
    m_heartbeatTimer->stop();

    // Close all client sockets
    for (QTcpSocket *socket : m_socketToNode.keys()) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    m_socketToNode.clear();
    m_nodeToSocket.clear();
    m_nodes.clear();
    m_readBuffers.clear();

    m_server->close();
}

bool NetworkRenderServer::isListening() const
{
    return m_server->isListening();
}

quint16 NetworkRenderServer::serverPort() const
{
    return m_server->serverPort();
}

QString NetworkRenderServer::addRenderJob(const RenderJobNet &job)
{
    RenderJobNet j = job;
    if (j.jobId.isEmpty())
        j.jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    j.status   = NetworkJobStatus::Pending;
    j.progress = 0.0;
    m_jobs.append(j);

    if (m_autoDistribute)
        distributeJobs();

    return j.jobId;
}

void NetworkRenderServer::cancelJob(const QString &jobId)
{
    RenderJobNet *job = findJob(jobId);
    if (!job)
        return;

    // If assigned, notify the node
    if (!job->assignedNode.isEmpty()) {
        QTcpSocket *socket = m_nodeToSocket.value(job->assignedNode, nullptr);
        if (socket) {
            QJsonObject msg;
            msg["type"]  = "CANCEL_JOB";
            msg["jobId"] = jobId;
            sendMessage(socket, msg);
        }
        RenderNode *node = findNode(job->assignedNode);
        if (node) {
            node->status     = NodeStatus::Idle;
            node->currentJob = QString();
        }
    }

    job->status = NetworkJobStatus::Failed;
}

void NetworkRenderServer::distributeJobs()
{
    // Collect idle nodes
    QStringList idleNodes;
    for (const RenderNode &node : m_nodes) {
        if (node.status == NodeStatus::Idle)
            idleNodes.append(node.nodeId);
    }

    if (idleNodes.isEmpty())
        return;

    // For each pending job, split into segments and assign
    for (RenderJobNet &job : m_jobs) {
        if (job.status != NetworkJobStatus::Pending)
            continue;
        if (idleNodes.isEmpty())
            break;

        int totalFrames = job.endFrame - job.startFrame + 1;
        if (totalFrames <= m_segmentSize) {
            // Single segment – assign directly
            const QString &nodeId = idleNodes.takeFirst();
            assignJobToNode(job.jobId, nodeId);
        } else {
            // Split into segments
            QStringList segIds;
            int frame = job.startFrame;
            while (frame <= job.endFrame) {
                int segEnd = qMin(frame + m_segmentSize - 1, job.endFrame);

                RenderJobNet seg;
                seg.jobId           = QUuid::createUuid().toString(QUuid::WithoutBraces);
                seg.projectFilePath = job.projectFilePath;
                seg.startFrame      = frame;
                seg.endFrame        = segEnd;
                seg.exportPreset    = job.exportPreset;

                // Segment output gets a suffix so we can concat later
                QFileInfo fi(job.outputPath);
                seg.outputPath = fi.dir().filePath(
                    fi.baseName() + QString("_seg%1.").arg(segIds.size()) + fi.completeSuffix());

                seg.status = NetworkJobStatus::Pending;
                m_jobs.append(seg);
                segIds.append(seg.jobId);

                m_segmentParent[seg.jobId] = job.jobId;
                frame = segEnd + 1;
            }

            m_segmentMap[job.jobId] = segIds;

            // Mark master job as Assigned (it waits for segments)
            job.status = NetworkJobStatus::Assigned;

            // Assign segments to available nodes
            for (const QString &segId : segIds) {
                if (idleNodes.isEmpty())
                    break;
                const QString &nodeId = idleNodes.takeFirst();
                assignJobToNode(segId, nodeId);
            }
        }
    }
}

// ── Private ──────────────────────────────────

QVector<RenderNode> NetworkRenderServer::nodes() const
{
    return m_nodes.values().toVector();
}

void NetworkRenderServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();

        connect(socket, &QTcpSocket::readyRead,
                this, &NetworkRenderServer::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected,
                this, &NetworkRenderServer::onSocketDisconnected);

        // Temporary placeholder until HELLO arrives
        m_readBuffers[socket] = QByteArray();
    }
}

void NetworkRenderServer::onSocketReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket)
        return;

    m_readBuffers[socket] += socket->readAll();

    // Process all complete newline-delimited messages
    QByteArray &buf = m_readBuffers[socket];
    int newlineIdx;
    while ((newlineIdx = buf.indexOf('\n')) != -1) {
        QByteArray line = buf.left(newlineIdx).trimmed();
        buf.remove(0, newlineIdx + 1);

        if (line.isEmpty())
            continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;

        handleMessage(socket, doc.object());
    }
}

void NetworkRenderServer::onSocketDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket)
        return;

    QString nodeId = m_socketToNode.value(socket);
    if (!nodeId.isEmpty()) {
        RenderNode node = m_nodes.value(nodeId);
        node.status = NodeStatus::Offline;

        // Re-queue any job that was assigned to this node
        if (!node.currentJob.isEmpty()) {
            RenderJobNet *job = findJob(node.currentJob);
            if (job && (job->status == NetworkJobStatus::Assigned ||
                        job->status == NetworkJobStatus::Rendering)) {
                job->status       = NetworkJobStatus::Pending;
                job->assignedNode = QString();
                job->progress     = 0.0;
            }
        }

        m_nodes.remove(nodeId);
        m_nodeToSocket.remove(nodeId);
        m_socketToNode.remove(socket);
        m_readBuffers.remove(socket);

        emit nodeDisconnected(node);
    }

    socket->deleteLater();
}

void NetworkRenderServer::onHeartbeatTimeout()
{
    QDateTime cutoff = QDateTime::currentDateTime().addSecs(-30);

    // Find nodes that have not sent a heartbeat for 30 seconds
    QStringList stale;
    for (const RenderNode &node : m_nodes) {
        if (node.lastSeen < cutoff)
            stale.append(node.nodeId);
    }

    for (const QString &nodeId : stale) {
        QTcpSocket *socket = m_nodeToSocket.value(nodeId, nullptr);
        if (socket)
            socket->disconnectFromHost();
    }

    // Try to distribute any pending jobs to newly idle nodes
    if (m_autoDistribute)
        distributeJobs();
}

void NetworkRenderServer::handleMessage(QTcpSocket *socket, const QJsonObject &msg)
{
    QString type = msg["type"].toString();

    if (type == MSG_HELLO)
        handleHello(socket, msg);
    else if (type == MSG_HEARTBEAT)
        handleHeartbeat(socket, msg);
    else if (type == MSG_JOB_PROGRESS)
        handleJobProgress(socket, msg);
    else if (type == MSG_JOB_COMPLETE)
        handleJobComplete(socket, msg);
    else if (type == MSG_JOB_FAILED)
        handleJobFailed(socket, msg);
}

void NetworkRenderServer::handleHello(QTcpSocket *socket, const QJsonObject &msg)
{
    RenderNode node;
    node.nodeId    = msg["nodeId"].toString();
    node.hostname  = msg["hostname"].toString();
    node.ipAddress = socket->peerAddress().toString();
    node.port      = static_cast<quint16>(msg["port"].toInt());
    node.cpuCores  = msg["cpuCores"].toInt(1);
    node.gpuName   = msg["gpuName"].toString();
    node.status    = NodeStatus::Idle;
    node.lastSeen  = QDateTime::currentDateTime();

    if (node.nodeId.isEmpty())
        node.nodeId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    m_nodes[node.nodeId]      = node;
    m_socketToNode[socket]    = node.nodeId;
    m_nodeToSocket[node.nodeId] = socket;

    emit nodeConnected(node);

    if (m_autoDistribute)
        tryAssignPendingJob(node.nodeId);
}

void NetworkRenderServer::handleHeartbeat(QTcpSocket *socket, const QJsonObject &msg)
{
    Q_UNUSED(msg)
    QString nodeId = nodeIdForSocket(socket);
    if (nodeId.isEmpty())
        return;

    if (m_nodes.contains(nodeId))
        m_nodes[nodeId].lastSeen = QDateTime::currentDateTime();
}

void NetworkRenderServer::handleJobProgress(QTcpSocket *socket, const QJsonObject &msg)
{
    Q_UNUSED(socket)
    QString jobId   = msg["jobId"].toString();
    double  progress = msg["progress"].toDouble();

    RenderJobNet *job = findJob(jobId);
    if (!job)
        return;

    job->progress = qBound(0.0, progress, 100.0);

    // If this is a segment, roll up progress to the master job
    QString masterId = m_segmentParent.value(jobId);
    if (!masterId.isEmpty()) {
        const QStringList &segs = m_segmentMap.value(masterId);
        double total = 0.0;
        for (const QString &sid : segs) {
            const RenderJobNet *seg = findJob(sid);
            if (seg)
                total += seg->progress;
        }
        double masterPct = segs.isEmpty() ? 0.0 : total / segs.size();
        RenderJobNet *master = findJob(masterId);
        if (master) {
            master->progress = masterPct;
            emit jobProgress(masterId, masterPct);
        }
    } else {
        emit jobProgress(jobId, progress);
    }
}

void NetworkRenderServer::handleJobComplete(QTcpSocket *socket, const QJsonObject &msg)
{
    Q_UNUSED(socket)
    QString jobId      = msg["jobId"].toString();
    QString outputPath = msg["outputPath"].toString();

    RenderJobNet *job = findJob(jobId);
    if (!job)
        return;

    job->status   = NetworkJobStatus::Complete;
    job->progress = 100.0;

    // Free the node
    RenderNode *node = findNode(job->assignedNode);
    if (node) {
        node->status     = NodeStatus::Idle;
        node->currentJob = QString();
    }

    // Check if this is a segment
    QString masterId = m_segmentParent.value(jobId);
    if (!masterId.isEmpty()) {
        const QStringList &segs = m_segmentMap.value(masterId);
        bool allDone = true;
        for (const QString &sid : segs) {
            const RenderJobNet *seg = findJob(sid);
            if (!seg || seg->status != NetworkJobStatus::Complete) {
                allDone = false;
                break;
            }
        }
        if (allDone) {
            assembleSegments(masterId);
        } else {
            // Assign remaining pending segments
            if (node)
                tryAssignPendingJob(node->nodeId);
        }
    } else {
        emit jobComplete(jobId, outputPath);
        checkAllComplete();
        if (node)
            tryAssignPendingJob(node->nodeId);
    }
}

void NetworkRenderServer::handleJobFailed(QTcpSocket *socket, const QJsonObject &msg)
{
    Q_UNUSED(socket)
    QString jobId = msg["jobId"].toString();
    RenderJobNet *job = findJob(jobId);
    if (!job)
        return;

    job->status   = NetworkJobStatus::Failed;
    job->progress = 0.0;

    // Free the node
    RenderNode *node = findNode(job->assignedNode);
    if (node) {
        node->status     = NodeStatus::Idle;
        node->currentJob = QString();
    }

    // If this is a segment, fail the master too
    QString masterId = m_segmentParent.value(jobId);
    if (!masterId.isEmpty()) {
        RenderJobNet *master = findJob(masterId);
        if (master)
            master->status = NetworkJobStatus::Failed;
    }

    if (node)
        tryAssignPendingJob(node->nodeId);
}

void NetworkRenderServer::sendMessage(QTcpSocket *socket, const QJsonObject &msg)
{
    QByteArray data = QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n';
    socket->write(data);
}

void NetworkRenderServer::assignJobToNode(const QString &jobId, const QString &nodeId)
{
    RenderJobNet *job  = findJob(jobId);
    RenderNode   *node = findNode(nodeId);
    QTcpSocket   *socket = m_nodeToSocket.value(nodeId, nullptr);

    if (!job || !node || !socket)
        return;

    job->status       = NetworkJobStatus::Assigned;
    job->assignedNode = nodeId;

    node->status     = NodeStatus::Busy;
    node->currentJob = jobId;

    QJsonObject msg;
    msg["type"] = MSG_ASSIGN_JOB;
    msg["job"]  = jobToJson(*job);
    sendMessage(socket, msg);
}

bool NetworkRenderServer::tryAssignPendingJob(const QString &nodeId)
{
    RenderNode *node = findNode(nodeId);
    if (!node || node->status != NodeStatus::Idle)
        return false;

    for (RenderJobNet &job : m_jobs) {
        if (job.status == NetworkJobStatus::Pending) {
            assignJobToNode(job.jobId, nodeId);
            return true;
        }
    }
    return false;
}

void NetworkRenderServer::checkAllComplete()
{
    for (const RenderJobNet &job : m_jobs) {
        if (job.status == NetworkJobStatus::Pending  ||
            job.status == NetworkJobStatus::Assigned ||
            job.status == NetworkJobStatus::Rendering)
            return;
    }
    emit allJobsComplete();
}

void NetworkRenderServer::assembleSegments(const QString &masterJobId)
{
    RenderJobNet *master = findJob(masterJobId);
    if (!master)
        return;

    const QStringList &segs = m_segmentMap.value(masterJobId);

    // Write ffmpeg concat list file
    QFileInfo fi(master->outputPath);
    QString concatFile = fi.dir().filePath(fi.baseName() + "_concat.txt");

    QFile f(concatFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        master->status = NetworkJobStatus::Failed;
        return;
    }

    for (const QString &sid : segs) {
        const RenderJobNet *seg = findJob(sid);
        if (seg)
            f.write(QString("file '%1'\n").arg(seg->outputPath).toUtf8());
    }
    f.close();

    // Run ffmpeg concat demuxer
    QProcess *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    QString outputPath = master->outputPath;
    QString masterJobIdCopy = masterJobId;

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc, master, masterJobIdCopy, outputPath, concatFile]
            (int exitCode, QProcess::ExitStatus exitStatus) {
        // master pointer may be invalid if m_jobs was modified; use findJob
        RenderJobNet *m = findJob(masterJobIdCopy);
        if (m) {
            if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                m->status   = NetworkJobStatus::Complete;
                m->progress = 100.0;
                emit jobComplete(masterJobIdCopy, outputPath);
            } else {
                m->status = NetworkJobStatus::Failed;
            }
        }
        QFile::remove(concatFile);
        checkAllComplete();
        proc->deleteLater();
    });

    QStringList args;
    args << "-y"
         << "-f"  << "concat"
         << "-safe" << "0"
         << "-i"  << concatFile
         << "-c"  << "copy"
         << master->outputPath;

    master->status = NetworkJobStatus::Rendering;
    proc->start("ffmpeg", args);
}

QString NetworkRenderServer::nodeIdForSocket(QTcpSocket *socket) const
{
    return m_socketToNode.value(socket);
}

RenderNode *NetworkRenderServer::findNode(const QString &nodeId)
{
    auto it = m_nodes.find(nodeId);
    return it != m_nodes.end() ? &it.value() : nullptr;
}

RenderJobNet *NetworkRenderServer::findJob(const QString &jobId)
{
    for (RenderJobNet &job : m_jobs) {
        if (job.jobId == jobId)
            return &job;
    }
    return nullptr;
}

// ═══════════════════════════════════════════════
// NetworkRenderClient
// ═══════════════════════════════════════════════

NetworkRenderClient::NetworkRenderClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_heartbeat(new QTimer(this))
{
    m_nodeId   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_hostname = QHostInfo::localHostName();

    connect(m_socket, &QTcpSocket::connected,
            this, &NetworkRenderClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &NetworkRenderClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &NetworkRenderClient::onReadyRead);

    m_heartbeat->setInterval(5000);
    connect(m_heartbeat, &QTimer::timeout,
            this, &NetworkRenderClient::onHeartbeat);
}

NetworkRenderClient::~NetworkRenderClient()
{
    disconnectFromServer();
}

void NetworkRenderClient::connectToServer(const QString &host, quint16 port)
{
    m_socket->connectToHost(host, port);
}

void NetworkRenderClient::disconnectFromServer()
{
    m_heartbeat->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
        m_socket->waitForDisconnected(3000);
    }
}

bool NetworkRenderClient::isConnected() const
{
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void NetworkRenderClient::reportProgress(const QString &jobId, double progress)
{
    if (!isConnected())
        return;
    QJsonObject msg;
    msg["type"]     = MSG_JOB_PROGRESS;
    msg["jobId"]    = jobId;
    msg["progress"] = progress;
    sendMessage(msg);
}

void NetworkRenderClient::reportComplete(const QString &jobId, const QString &outputPath)
{
    if (!isConnected())
        return;
    QJsonObject msg;
    msg["type"]       = MSG_JOB_COMPLETE;
    msg["jobId"]      = jobId;
    msg["outputPath"] = outputPath;
    sendMessage(msg);
}

void NetworkRenderClient::reportFailed(const QString &jobId, const QString &error)
{
    if (!isConnected())
        return;
    QJsonObject msg;
    msg["type"]  = MSG_JOB_FAILED;
    msg["jobId"] = jobId;
    msg["error"] = error;
    sendMessage(msg);
}

// ── Private ──────────────────────────────────

void NetworkRenderClient::onConnected()
{
    m_heartbeat->start();
    sendHello();
    emit connected();
}

void NetworkRenderClient::onDisconnected()
{
    m_heartbeat->stop();
    emit disconnected();
}

void NetworkRenderClient::onReadyRead()
{
    m_readBuffer += m_socket->readAll();

    int newlineIdx;
    while ((newlineIdx = m_readBuffer.indexOf('\n')) != -1) {
        QByteArray line = m_readBuffer.left(newlineIdx).trimmed();
        m_readBuffer.remove(0, newlineIdx + 1);

        if (line.isEmpty())
            continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;

        handleMessage(doc.object());
    }
}

void NetworkRenderClient::onHeartbeat()
{
    if (!isConnected())
        return;
    QJsonObject msg;
    msg["type"]   = MSG_HEARTBEAT;
    msg["nodeId"] = m_nodeId;
    sendMessage(msg);
}

void NetworkRenderClient::handleMessage(const QJsonObject &msg)
{
    QString type = msg["type"].toString();

    if (type == MSG_ASSIGN_JOB) {
        RenderJobNet job = jobFromJson(msg["job"].toObject());
        job.status = NetworkJobStatus::Rendering;
        emit jobReceived(job);
    }
}

void NetworkRenderClient::sendMessage(const QJsonObject &msg)
{
    QByteArray data = QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n';
    m_socket->write(data);
}

void NetworkRenderClient::sendHello()
{
    QJsonObject msg;
    msg["type"]     = MSG_HELLO;
    msg["nodeId"]   = m_nodeId;
    msg["hostname"] = m_hostname;
    msg["cpuCores"] = m_cpuCores;
    msg["gpuName"]  = m_gpuName;
    sendMessage(msg);
}

// ═══════════════════════════════════════════════
// NetworkRenderDialog
// ═══════════════════════════════════════════════

NetworkRenderDialog::NetworkRenderDialog(NetworkRenderServer *server, QWidget *parent)
    : QDialog(parent)
    , m_server(server)
{
    setWindowTitle("Network Render Manager");
    setMinimumSize(800, 550);
    setupUI();

    connect(server, &NetworkRenderServer::nodeConnected,
            this, &NetworkRenderDialog::onNodeConnected);
    connect(server, &NetworkRenderServer::nodeDisconnected,
            this, &NetworkRenderDialog::onNodeDisconnected);
    connect(server, &NetworkRenderServer::jobProgress,
            this, &NetworkRenderDialog::onJobProgress);
    connect(server, &NetworkRenderServer::jobComplete,
            this, &NetworkRenderDialog::onJobComplete);
    connect(server, &NetworkRenderServer::allJobsComplete,
            this, &NetworkRenderDialog::onAllJobsComplete);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(2000);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        refreshNodesTable();
        refreshJobsTable();
        updateThroughput();
    });
    m_refreshTimer->start();

    // Initialise button states
    m_stopBtn->setEnabled(false);
}

void NetworkRenderDialog::setupUI()
{
    QVBoxLayout *root = new QVBoxLayout(this);

    // ── Tab widget ──────────────────────────────
    m_tabs = new QTabWidget(this);
    root->addWidget(m_tabs);

    // ── Tab 1: Nodes ────────────────────────────
    QWidget *nodesTab = new QWidget;
    QVBoxLayout *nodesLayout = new QVBoxLayout(nodesTab);

    m_nodesTable = new QTableWidget(0, 6, nodesTab);
    m_nodesTable->setHorizontalHeaderLabels({
        "Hostname", "IP Address", "Status", "CPU Cores", "GPU", "Current Job"
    });
    m_nodesTable->horizontalHeader()->setStretchLastSection(true);
    m_nodesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_nodesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    nodesLayout->addWidget(m_nodesTable);
    m_tabs->addTab(nodesTab, "Nodes");

    // ── Tab 2: Jobs ─────────────────────────────
    QWidget *jobsTab = new QWidget;
    QVBoxLayout *jobsLayout = new QVBoxLayout(jobsTab);

    m_jobsTable = new QTableWidget(0, 6, jobsTab);
    m_jobsTable->setHorizontalHeaderLabels({
        "Job ID", "Frames", "Status", "Progress", "Assigned Node", "Output"
    });
    m_jobsTable->horizontalHeader()->setStretchLastSection(true);
    m_jobsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_jobsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobsLayout->addWidget(m_jobsTable);
    m_tabs->addTab(jobsTab, "Jobs");

    // ── Tab 3: Settings ─────────────────────────
    QWidget *settingsTab = new QWidget;
    QFormLayout *settingsLayout = new QFormLayout(settingsTab);
    settingsLayout->setContentsMargins(12, 12, 12, 12);
    settingsLayout->setVerticalSpacing(10);

    m_portSpin = new QSpinBox(settingsTab);
    m_portSpin->setRange(1024, 65535);
    m_portSpin->setValue(9920);
    settingsLayout->addRow("Server Port:", m_portSpin);

    m_autoDistributeCheck = new QCheckBox("Auto-distribute jobs to idle nodes", settingsTab);
    m_autoDistributeCheck->setChecked(true);
    settingsLayout->addRow("Auto Distribute:", m_autoDistributeCheck);

    m_segmentSizeSpin = new QSpinBox(settingsTab);
    m_segmentSizeSpin->setRange(1, 10000);
    m_segmentSizeSpin->setValue(300);
    m_segmentSizeSpin->setSuffix(" frames per chunk");
    settingsLayout->addRow("Segment Size:", m_segmentSizeSpin);

    m_tabs->addTab(settingsTab, "Settings");

    // ── Button bar ──────────────────────────────
    QGroupBox *btnGroup = new QGroupBox("Server Control", this);
    QHBoxLayout *btnLayout = new QHBoxLayout(btnGroup);

    m_startBtn     = new QPushButton("Start Server", btnGroup);
    m_stopBtn      = new QPushButton("Stop Server",  btnGroup);
    m_addJobBtn    = new QPushButton("Add Job",       btnGroup);
    m_cancelJobBtn = new QPushButton("Cancel Job",    btnGroup);

    btnLayout->addWidget(m_startBtn);
    btnLayout->addWidget(m_stopBtn);
    btnLayout->addSpacing(16);
    btnLayout->addWidget(m_addJobBtn);
    btnLayout->addWidget(m_cancelJobBtn);
    btnLayout->addStretch();

    root->addWidget(btnGroup);

    // ── Status / progress bar ───────────────────
    QHBoxLayout *statusLayout = new QHBoxLayout;

    m_statusLabel    = new QLabel("Server not running", this);
    m_throughputLabel = new QLabel("Cluster throughput: —", this);
    m_overallProgress = new QProgressBar(this);
    m_overallProgress->setRange(0, 100);
    m_overallProgress->setValue(0);
    m_overallProgress->setFixedHeight(18);

    statusLayout->addWidget(m_statusLabel);
    statusLayout->addStretch();
    statusLayout->addWidget(m_throughputLabel);
    root->addLayout(statusLayout);
    root->addWidget(m_overallProgress);

    // ── Connections ─────────────────────────────
    connect(m_startBtn,     &QPushButton::clicked, this, &NetworkRenderDialog::onStartServer);
    connect(m_stopBtn,      &QPushButton::clicked, this, &NetworkRenderDialog::onStopServer);
    connect(m_addJobBtn,    &QPushButton::clicked, this, &NetworkRenderDialog::onAddJob);
    connect(m_cancelJobBtn, &QPushButton::clicked, this, &NetworkRenderDialog::onCancelJob);
}

void NetworkRenderDialog::onStartServer()
{
    applySettings();
    quint16 port = static_cast<quint16>(m_portSpin->value());
    if (m_server->startServer(port)) {
        m_statusLabel->setText(QString("Server listening on port %1").arg(port));
        m_startBtn->setEnabled(false);
        m_stopBtn->setEnabled(true);
        m_portSpin->setEnabled(false);
    } else {
        QMessageBox::warning(this, "Network Render",
            QString("Failed to start server on port %1.\nPort may already be in use.").arg(port));
    }
}

void NetworkRenderDialog::onStopServer()
{
    m_server->stopServer();
    m_statusLabel->setText("Server stopped");
    m_startBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    m_portSpin->setEnabled(true);
    m_nodesTable->setRowCount(0);
    refreshJobsTable();
}

void NetworkRenderDialog::onAddJob()
{
    if (!m_server->isListening()) {
        QMessageBox::information(this, "Network Render",
            "Please start the server before adding jobs.");
        return;
    }

    QString projectFile = QFileDialog::getOpenFileName(
        this, "Select Project / Source File", QString(),
        "Video Files (*.mp4 *.mkv *.mov *.avi);;All Files (*)");
    if (projectFile.isEmpty())
        return;

    QString outputFile = QFileDialog::getSaveFileName(
        this, "Select Output File", QString(),
        "MP4 Video (*.mp4);;MKV Video (*.mkv);;All Files (*)");
    if (outputFile.isEmpty())
        return;

    RenderJobNet job;
    job.projectFilePath = projectFile;
    job.outputPath      = outputFile;
    job.startFrame      = 0;
    job.endFrame        = 999;  // caller should set actual frame range
    job.exportPreset    = "default";

    m_server->addRenderJob(job);
    refreshJobsTable();
}

void NetworkRenderDialog::onCancelJob()
{
    QList<QTableWidgetItem *> sel = m_jobsTable->selectedItems();
    if (sel.isEmpty())
        return;

    int row = m_jobsTable->row(sel.first());
    QString jobId = m_jobsTable->item(row, 0)->text();
    m_server->cancelJob(jobId);
    refreshJobsTable();
}

void NetworkRenderDialog::onNodeConnected(const RenderNode &node)
{
    int row = m_nodesTable->rowCount();
    m_nodesTable->insertRow(row);
    m_nodesTable->setItem(row, 0, new QTableWidgetItem(node.hostname));
    m_nodesTable->setItem(row, 1, new QTableWidgetItem(node.ipAddress));
    m_nodesTable->setItem(row, 2, new QTableWidgetItem("Idle"));
    m_nodesTable->setItem(row, 3, new QTableWidgetItem(QString::number(node.cpuCores)));
    m_nodesTable->setItem(row, 4, new QTableWidgetItem(node.gpuName.isEmpty() ? "—" : node.gpuName));
    m_nodesTable->setItem(row, 5, new QTableWidgetItem("—"));

    // Store nodeId in UserRole for lookup
    m_nodesTable->item(row, 0)->setData(Qt::UserRole, node.nodeId);
}

void NetworkRenderDialog::onNodeDisconnected(const RenderNode &node)
{
    int row = findNodeRow(node.nodeId);
    if (row >= 0)
        m_nodesTable->removeRow(row);
}

void NetworkRenderDialog::onJobProgress(const QString &jobId, double progress)
{
    int row = findJobRow(jobId);
    if (row < 0)
        return;

    QProgressBar *bar = qobject_cast<QProgressBar *>(
        m_jobsTable->cellWidget(row, 3));
    if (bar)
        bar->setValue(static_cast<int>(progress));

    // Update overall progress
    const auto &jobs = m_server->jobs();
    if (jobs.isEmpty())
        return;
    double total = 0.0;
    for (const RenderJobNet &j : jobs)
        total += j.progress;
    m_overallProgress->setValue(static_cast<int>(total / jobs.size()));
}

void NetworkRenderDialog::onJobComplete(const QString &jobId, const QString &outputPath)
{
    int row = findJobRow(jobId);
    if (row >= 0) {
        m_jobsTable->setItem(row, 2, new QTableWidgetItem("Complete"));
        m_jobsTable->setItem(row, 5, new QTableWidgetItem(outputPath));
        QProgressBar *bar = qobject_cast<QProgressBar *>(
            m_jobsTable->cellWidget(row, 3));
        if (bar)
            bar->setValue(100);
    }
}

void NetworkRenderDialog::onAllJobsComplete()
{
    m_statusLabel->setText("All jobs complete!");
    m_overallProgress->setValue(100);
    QMessageBox::information(this, "Network Render", "All render jobs have completed.");
}

void NetworkRenderDialog::refreshNodesTable()
{
    const QVector<RenderNode> nodes = m_server->nodes();
    for (const RenderNode &node : nodes) {
        int row = findNodeRow(node.nodeId);
        if (row < 0)
            continue;
        m_nodesTable->item(row, 2)->setText(nodeStatusText(node.status));
        m_nodesTable->item(row, 5)->setText(node.currentJob.isEmpty() ? "—" : node.currentJob);
    }
}

void NetworkRenderDialog::refreshJobsTable()
{
    const QVector<RenderJobNet> jobs = m_server->jobs();
    m_jobsTable->setRowCount(0);

    for (const RenderJobNet &job : jobs) {
        int row = m_jobsTable->rowCount();
        m_jobsTable->insertRow(row);

        m_jobsTable->setItem(row, 0, new QTableWidgetItem(job.jobId));
        m_jobsTable->item(row, 0)->setData(Qt::UserRole, job.jobId);

        QString frames = QString("%1–%2").arg(job.startFrame).arg(job.endFrame);
        m_jobsTable->setItem(row, 1, new QTableWidgetItem(frames));
        m_jobsTable->setItem(row, 2, new QTableWidgetItem(statusText(job.status)));

        QProgressBar *bar = new QProgressBar;
        bar->setRange(0, 100);
        bar->setValue(static_cast<int>(job.progress));
        m_jobsTable->setCellWidget(row, 3, bar);

        m_jobsTable->setItem(row, 4,
            new QTableWidgetItem(job.assignedNode.isEmpty() ? "—" : job.assignedNode));
        m_jobsTable->setItem(row, 5, new QTableWidgetItem(job.outputPath));
    }
}

void NetworkRenderDialog::updateThroughput()
{
    const QVector<RenderNode> nodes = m_server->nodes();
    int busyNodes = 0;
    int totalCores = 0;
    for (const RenderNode &n : nodes) {
        if (n.status == NodeStatus::Busy)
            busyNodes++;
        totalCores += n.cpuCores;
    }
    m_throughputLabel->setText(
        QString("Cluster: %1 node(s) active, %2 total core(s)")
            .arg(busyNodes).arg(totalCores));
}

void NetworkRenderDialog::applySettings()
{
    m_server->setAutoDistribute(m_autoDistributeCheck->isChecked());
    m_server->setSegmentSize(m_segmentSizeSpin->value());
}

int NetworkRenderDialog::findNodeRow(const QString &nodeId) const
{
    for (int i = 0; i < m_nodesTable->rowCount(); ++i) {
        QTableWidgetItem *item = m_nodesTable->item(i, 0);
        if (item && item->data(Qt::UserRole).toString() == nodeId)
            return i;
    }
    return -1;
}

int NetworkRenderDialog::findJobRow(const QString &jobId) const
{
    for (int i = 0; i < m_jobsTable->rowCount(); ++i) {
        QTableWidgetItem *item = m_jobsTable->item(i, 0);
        if (item && item->data(Qt::UserRole).toString() == jobId)
            return i;
    }
    return -1;
}

QString NetworkRenderDialog::statusText(NetworkJobStatus s) const
{
    switch (s) {
    case NetworkJobStatus::Pending:   return "Pending";
    case NetworkJobStatus::Assigned:  return "Assigned";
    case NetworkJobStatus::Rendering: return "Rendering";
    case NetworkJobStatus::Complete:  return "Complete";
    case NetworkJobStatus::Failed:    return "Failed";
    }
    return "Unknown";
}

QString NetworkRenderDialog::nodeStatusText(NodeStatus s) const
{
    switch (s) {
    case NodeStatus::Idle:    return "Idle";
    case NodeStatus::Busy:    return "Busy";
    case NodeStatus::Offline: return "Offline";
    }
    return "Unknown";
}
