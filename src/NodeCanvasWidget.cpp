#include "NodeCanvasWidget.h"

#include "NodeLibrary.h"

#include <QApplication>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionGraphicsItem>
#include <QVBoxLayout>
#include <QWheelEvent>

// ---------------------------------------------------------------------------
// NodeItem
// ---------------------------------------------------------------------------

NodeItem::NodeItem(GraphNode *node, QGraphicsItem *parent)
    : QGraphicsItem(parent), m_node(node)
{
    setFlag(QGraphicsItem::ItemIsSelectable, true);
    setFlag(QGraphicsItem::ItemIsMovable, true);
    setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
    updateGeometry();
}

void NodeItem::updateGeometry()
{
    if (!m_node) return;

    m_maxPorts = qMax(m_node->inputs.size(), m_node->outputs.size());
    int contentHeight = qMax(1, m_maxPorts) * kPortSpacing + kBodyPad * 2;
    int totalHeight = kTitleHeight + contentHeight;

    m_bodyRect = QRectF(0, 0, kNodeWidth, totalHeight);
    prepareGeometryChange();
}

QRectF NodeItem::boundingRect() const
{
    return m_bodyRect.adjusted(-2, -2, 2, 2);
}

void NodeItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *)
{
    if (!m_node) return;

    painter->setRenderHint(QPainter::Antialiasing);

    bool selected = option && (option->state & QStyle::State_Selected);

    // Body
    QRectF body = m_bodyRect;
    QRectF titleRect = body.adjusted(0, 0, 0, -(body.height() - kTitleHeight));

    painter->fillRect(body, selected ? QColor(55, 55, 65) : QColor(45, 45, 55));
    painter->fillRect(titleRect, selected ? QColor(70, 90, 120) : QColor(55, 65, 80));

    // Border
    painter->setPen(selected ? QPen(QColor(100, 140, 200), 2) : QPen(QColor(80, 80, 90), 1));
    painter->drawRoundedRect(body, 6, 6);

    // Title
    painter->setPen(QColor(220, 220, 220));
    QFont font = painter->font();
    font.setBold(true);
    font.setPointSize(9);
    painter->setFont(font);
    painter->drawText(titleRect.adjusted(kHeaderPad, 0, -kHeaderPad, 0),
                      Qt::AlignLeft | Qt::AlignVCenter, m_node->displayName);

    // Input ports (left side)
    font.setBold(false);
    font.setPointSize(7);
    painter->setFont(font);

    int startY = kTitleHeight + kBodyPad + kPortSpacing / 2;
    for (int i = 0; i < m_node->inputs.size(); ++i) {
        QPointF pos(kPortPadX, startY + i * kPortSpacing);

        // Port circle
        painter->setPen(QPen(QColor(120, 180, 255), 1.5));
        painter->setBrush(QColor(120, 180, 255));
        painter->drawEllipse(pos, kPortRadius, kPortRadius);

        // Label
        painter->setPen(QColor(180, 180, 190));
        painter->drawText(pos.x() + kPortRadius + 3, pos.y() + 3, m_node->inputs[i].name);
    }

    // Output ports (right side)
    for (int i = 0; i < m_node->outputs.size(); ++i) {
        QPointF pos(kNodeWidth - kPortPadX, startY + i * kPortSpacing);

        painter->setPen(QPen(QColor(180, 255, 140), 1.5));
        painter->setBrush(QColor(180, 255, 140));
        painter->drawEllipse(pos, kPortRadius, kPortRadius);

        painter->setPen(QColor(180, 180, 190));
        painter->drawText(QRectF(0, pos.y() - kPortSpacing / 2.0,
                                 pos.x() - kPortRadius - 3, kPortSpacing),
                          Qt::AlignRight | Qt::AlignVCenter, m_node->outputs[i].name);
    }
}

QPointF NodeItem::portScenePosition(int portIndex, bool isInput) const
{
    QPointF localPos;
    if (isInput) {
        localPos.setX(kPortPadX);
    } else {
        localPos.setX(kNodeWidth - kPortPadX);
    }
    localPos.setY(kTitleHeight + kBodyPad + kPortSpacing / 2 + portIndex * kPortSpacing);
    return mapToScene(localPos);
}

int NodeItem::hitTestPort(QPointF scenePos, bool isInput) const
{
    QVector<NodePort> *ports = isInput ? &m_node->inputs : &m_node->outputs;
    for (int i = 0; i < ports->size(); ++i) {
        QPointF portPos = portScenePosition(i, isInput);
        if (QPointF(scenePos - portPos).manhattanLength() < kPortRadius + 4) {
            return i;
        }
    }
    return -1;
}

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if (change == QGraphicsItem::ItemPositionChange && m_node) {
        m_node->canvasPos = value.toPointF();
    }
    return QGraphicsItem::itemChange(change, value);
}

// ---------------------------------------------------------------------------
// ConnectionItem
// ---------------------------------------------------------------------------

ConnectionItem::ConnectionItem(QGraphicsItem *parent)
    : QGraphicsPathItem(parent)
{
    setPen(QPen(QColor(160, 160, 170), 2));
    setZValue(-1);
}

void ConnectionItem::setEndpoints(QPointF outputPos, QPointF inputPos)
{
    QPointF c1(outputPos.x() + 60, outputPos.y());
    QPointF c2(inputPos.x() - 60, inputPos.y());

    QPainterPath path(outputPos);
    path.cubicTo(c1, c2, inputPos);
    setPath(path);
}

void ConnectionItem::setConnectionInfo(int fromNode, int fromPort, int toNode, int toPort)
{
    m_fromNodeId = fromNode;
    m_fromPortIndex = fromPort;
    m_toNodeId = toNode;
    m_toPortIndex = toPort;
}

// ---------------------------------------------------------------------------
// NodeCanvasView
// ---------------------------------------------------------------------------

NodeCanvasView::NodeCanvasView(QWidget *parent)
    : QGraphicsView(parent)
{
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::NoDrag);
    setMouseTracking(true);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorUnderMouse);
    setFocusPolicy(Qt::ClickFocus);
}

void NodeCanvasView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        QPointF scenePos = mapToScene(event->pos());
        auto itemsAtPos = scene()->items(scenePos);

        for (auto *item : itemsAtPos) {
            if (auto *nodeItem = qgraphicsitem_cast<NodeItem *>(item)) {
                int outPort = nodeItem->hitTestPort(scenePos, false);
                if (outPort >= 0) {
                    m_portDragging = true;
                    m_dragStartPortPos = nodeItem->portScenePosition(outPort, false);
                    emit portDragStarted(m_dragStartPortPos);
                    return;
                }
            }
        }

        bool onNode = false;
        for (auto *item : itemsAtPos) {
            if (qgraphicsitem_cast<NodeItem *>(item)) {
                onNode = true;
                break;
            }
        }
        if (!onNode) {
            emit canvasClicked();
        }
    }

    QGraphicsView::mousePressEvent(event);
}

void NodeCanvasView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_portDragging) {
        emit portDragMoved(mapToScene(event->pos()));
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void NodeCanvasView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_portDragging && event->button() == Qt::LeftButton) {
        m_portDragging = false;
        emit portDragEnded(mapToScene(event->pos()));
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void NodeCanvasView::contextMenuEvent(QContextMenuEvent *event)
{
    QPointF scenePos = mapToScene(event->pos());
    auto itemsAtPos = scene()->items(scenePos);

    bool onNode = false;
    for (auto *item : itemsAtPos) {
        if (qgraphicsitem_cast<NodeItem *>(item)) {
            onNode = true;
            break;
        }
    }

    if (!onNode) {
        emit canvasContextMenuRequested(scenePos, event->globalPos());
    } else {
        QGraphicsView::contextMenuEvent(event);
    }
}

void NodeCanvasView::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() > 0) {
        scale(1.15, 1.15);
    } else {
        scale(1.0 / 1.15, 1.0 / 1.15);
    }
}

void NodeCanvasView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        emit deleteRequested();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// NodeCanvasWidget
// ---------------------------------------------------------------------------

NodeCanvasWidget::NodeCanvasWidget(QWidget *parent)
    : QWidget(parent)
{
    m_scene = new QGraphicsScene(this);
    m_view = new NodeCanvasView(this);
    m_view->setScene(m_scene);

    connect(m_view, &NodeCanvasView::canvasContextMenuRequested,
            this, &NodeCanvasWidget::onCanvasContextMenuRequested);
    connect(m_view, &NodeCanvasView::canvasClicked,
            this, &NodeCanvasWidget::onCanvasClicked);
    connect(m_view, &NodeCanvasView::portDragStarted,
            this, &NodeCanvasWidget::onPortDragStarted);
    connect(m_view, &NodeCanvasView::portDragMoved,
            this, &NodeCanvasWidget::onPortDragMoved);
    connect(m_view, &NodeCanvasView::portDragEnded,
            this, &NodeCanvasWidget::onPortDragEnded);
    connect(m_scene, &QGraphicsScene::selectionChanged,
            this, &NodeCanvasWidget::onSelectionChanged);
    connect(m_view, &NodeCanvasView::deleteRequested,
            this, &NodeCanvasWidget::onDeleteSelected);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view);

    m_view->setAcceptDrops(true);
}

void NodeCanvasWidget::setGraph(NodeGraph *graph)
{
    m_graph = graph;
    rebuildScene();
}

void NodeCanvasWidget::rebuildScene()
{
    m_scene->clear();
    m_nodeItems.clear();
    m_tempConnection = nullptr;

    if (!m_graph) return;

    for (auto &node : m_graph->nodes) {
        auto *item = new NodeItem(const_cast<GraphNode *>(&node));
        item->setPos(node.canvasPos);
        m_scene->addItem(item);
        m_nodeItems.insert(node.id, item);
    }

    for (const auto &conn : m_graph->connections) {
        auto *connItem = new ConnectionItem();
        connItem->setConnectionInfo(conn.fromNodeId, conn.fromPortIndex,
                                     conn.toNodeId, conn.toPortIndex);

        auto *fromItem = m_nodeItems.value(conn.fromNodeId);
        auto *toItem = m_nodeItems.value(conn.toNodeId);
        if (fromItem && toItem) {
            QPointF outPos = fromItem->portScenePosition(conn.fromPortIndex, false);
            QPointF inPos = toItem->portScenePosition(conn.toPortIndex, true);
            connItem->setEndpoints(outPos, inPos);
        }
        m_scene->addItem(connItem);
    }
}

void NodeCanvasWidget::onAddNode(const QString &typeName)
{
    if (!m_graph) return;

    int id = m_graph->addNode(typeName);
    GraphNode *node = m_graph->node(id);
    if (!node) return;

    rebuildScene();
    emit graphChanged();
}

void NodeCanvasWidget::onDeleteSelected()
{
    if (!m_graph) return;

    auto selectedItems = m_scene->selectedItems();
    QVector<NodeConnection> connectionsToRemove;
    QVector<int> nodesToRemove;

    for (auto *item : selectedItems) {
        if (auto *connItem = qgraphicsitem_cast<ConnectionItem *>(item)) {
            connectionsToRemove.append(NodeConnection{-1, -1, connItem->toNodeId(),
                                                      connItem->toPortIndex()});
        } else if (auto *nodeItem = qgraphicsitem_cast<NodeItem *>(item)) {
            GraphNode *node = nodeItem->node();
            if (node) {
                nodesToRemove.append(node->id);
            }
        }
    }

    bool changed = false;
    for (const NodeConnection &connection : connectionsToRemove) {
        m_graph->disconnect(connection.toNodeId, connection.toPortIndex);
        changed = true;
    }
    for (int nodeId : nodesToRemove) {
        m_graph->removeNode(nodeId);
        changed = true;
    }

    rebuildScene();
    if (changed) {
        emit graphChanged();
    }
}

void NodeCanvasWidget::onPortDragStarted(QPointF outputPortScenePos)
{
    if (!m_graph) return;

    m_dragStartPortPos = outputPortScenePos;
    m_tempConnection = new ConnectionItem();
    m_tempConnection->setPen(QPen(QColor(100, 200, 255), 2, Qt::DashLine));
    m_tempConnection->setEndpoints(outputPortScenePos, outputPortScenePos);
    m_scene->addItem(m_tempConnection);
}

void NodeCanvasWidget::onPortDragMoved(QPointF scenePos)
{
    if (m_tempConnection) {
        m_tempConnection->setEndpoints(m_dragStartPortPos, scenePos);
    }
}

void NodeCanvasWidget::onPortDragEnded(QPointF scenePos)
{
    if (!m_graph || !m_tempConnection) return;

    auto itemsAtPos = m_scene->items(scenePos);
    NodeItem *targetNodeItem = nullptr;
    for (auto *item : itemsAtPos) {
        if (auto *ni = qgraphicsitem_cast<NodeItem *>(item)) {
            targetNodeItem = ni;
            break;
        }
    }

    if (targetNodeItem) {
        int inPort = targetNodeItem->hitTestPort(scenePos, true);
        if (inPort >= 0) {
            NodeItem *sourceNodeItem = nullptr;
            int outPort = -1;
            for (auto it = m_nodeItems.begin(); it != m_nodeItems.end(); ++it) {
                int p = it.value()->hitTestPort(m_dragStartPortPos, false);
                if (p >= 0) {
                    sourceNodeItem = it.value();
                    outPort = p;
                    break;
                }
            }

            if (sourceNodeItem && sourceNodeItem != targetNodeItem) {
                bool ok = m_graph->connect(sourceNodeItem->node()->id, outPort,
                                           targetNodeItem->node()->id, inPort);
                if (ok) {
                    emit graphChanged();
                }
            }
        }
    }

    m_scene->removeItem(m_tempConnection);
    delete m_tempConnection;
    m_tempConnection = nullptr;

    rebuildScene();
}

void NodeCanvasWidget::onCanvasContextMenuRequested(QPointF scenePos, QPoint globalPos)
{
    if (!m_graph) return;

    QMenu menu(this);
    buildContextMenu(&menu, scenePos);

    if (!menu.isEmpty()) {
        QAction *action = menu.exec(globalPos);
        if (action) {
            QString typeName = action->data().toString();
            if (!typeName.isEmpty()) {
                int id = m_graph->addNode(typeName);
                GraphNode *node = m_graph->node(id);
                if (node) {
                    node->canvasPos = scenePos;
                    rebuildScene();
                    emit graphChanged();
                }
            }
        }
    }
}

void NodeCanvasWidget::onCanvasClicked()
{
    emit nodeSelected(-1);
}

void NodeCanvasWidget::onSelectionChanged()
{
    auto selected = m_scene->selectedItems();
    if (selected.isEmpty()) {
        emit nodeSelected(-1);
        return;
    }

    for (auto *item : selected) {
        if (auto *nodeItem = qgraphicsitem_cast<NodeItem *>(item)) {
            emit nodeSelected(nodeItem->node()->id);
            return;
        }
    }
    emit nodeSelected(-1);
}

NodeItem *NodeCanvasWidget::findNodeItem(int nodeId) const
{
    return m_nodeItems.value(nodeId, nullptr);
}

ConnectionItem *NodeCanvasWidget::findConnectionItem(int fromNode, int fromPort, int toNode, int toPort) const
{
    for (auto *item : m_scene->items()) {
        if (auto *ci = qgraphicsitem_cast<ConnectionItem *>(item)) {
            if (ci->fromNodeId() == fromNode && ci->fromPortIndex() == fromPort &&
                ci->toNodeId() == toNode && ci->toPortIndex() == toPort) {
                return ci;
            }
        }
    }
    return nullptr;
}

void NodeCanvasWidget::removeConnectionItem(ConnectionItem *item)
{
    m_scene->removeItem(item);
    delete item;
}

void NodeCanvasWidget::buildContextMenu(QMenu *menu, QPointF)
{
    if (!m_graph) return;

    nodelib::NodeRegistry &reg = nodelib::NodeRegistry::instance();
    QStringList typeNames = reg.allTypeNames();

    QHash<QString, QStringList> grouped;
    for (const QString &typeName : typeNames) {
        const nodelib::NodeTypeDescriptor *desc = reg.descriptor(typeName);
        QString category = desc ? desc->category : QStringLiteral("Other");
        grouped[category].append(typeName);
    }

    QStringList categories = grouped.keys();
    categories.sort();

    for (const QString &category : categories) {
        QMenu *subMenu = menu->addMenu(category);
        for (const QString &typeName : grouped[category]) {
            const nodelib::NodeTypeDescriptor *desc = reg.descriptor(typeName);
            QString displayName = desc ? desc->displayName : typeName;
            QAction *action = subMenu->addAction(displayName);
            action->setData(typeName);
        }
    }
}
