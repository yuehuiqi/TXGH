#include "devicetopologyview.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/StyleCollection>
#include <QtNodes/ConnectionStyle>

#include "ConnectionIdHash.hpp"   // std::hash<ConnectionId>，供 unordered_set 使用

#include <QVBoxLayout>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QtMath>
#include <algorithm>

using QtNodes::BasicGraphicsScene;
using QtNodes::ConnectionPolicy;
using QtNodes::GraphicsView;
using QtNodes::InvalidNodeId;
using QtNodes::StyleCollection;

// 规范化为无向 key 对（小在前），用于矩阵<->画板去重比较
static QPair<QString, QString> norm(const QString &a, const QString &b)
{
    return (a <= b) ? qMakePair(a, b) : qMakePair(b, a);
}

// 依据设备流向决定连线方向：input 设备必为源(OUT→)，output 设备必为汇(→IN)，
// 从而保证 input 的输入口、output 的输出口保持空置。返回 a 是否作为源端。
static bool flowAIsSource(const QString &ioA, const QString &ioB)
{
    if (ioA == QLatin1String("input") || ioB == QLatin1String("output")) return true;
    if (ioB == QLatin1String("input") || ioA == QLatin1String("output")) return false;
    return true;
}

// =====================  DeviceGraphModel  ===================================

DeviceGraphModel::DeviceGraphModel() = default;
DeviceGraphModel::~DeviceGraphModel() = default;

DeviceGraphModel::NodeId DeviceGraphModel::addDeviceNode(const QString &key,
                                                         const QString &caption,
                                                         int maxConn,
                                                         const QString &ioRole)
{
    NodeId id = newNodeId();
    m_nodeIds.insert(id);
    m_keyOf.insert(id, key);
    m_nodeOf.insert(key, id);
    m_caption.insert(id, caption);
    m_maxConn.insert(id, maxConn);
    m_ioRole.insert(id, ioRole);
    m_geom.insert(id, NodeGeom{QPointF(0, 0), QSize()});

    Q_EMIT nodeCreated(id);
    return id;
}

// 统计某设备节点当前已连接的连线数（无向，端口策略为 Many）
int DeviceGraphModel::connectionCountOf(NodeId id) const
{
    int n = 0;
    for (const ConnectionId &cid : m_connectivity)
        if (cid.inNodeId == id || cid.outNodeId == id) ++n;
    return n;
}

std::unordered_set<DeviceGraphModel::NodeId> DeviceGraphModel::allNodeIds() const
{
    return m_nodeIds;
}

std::unordered_set<DeviceGraphModel::ConnectionId>
DeviceGraphModel::allConnectionIds(NodeId const nodeId) const
{
    std::unordered_set<ConnectionId> result;
    std::copy_if(m_connectivity.begin(), m_connectivity.end(),
                 std::inserter(result, result.end()),
                 [&nodeId](ConnectionId const &cid) {
                     return cid.inNodeId == nodeId || cid.outNodeId == nodeId;
                 });
    return result;
}

std::unordered_set<DeviceGraphModel::ConnectionId>
DeviceGraphModel::connections(NodeId nodeId, PortType portType, PortIndex portIndex) const
{
    std::unordered_set<ConnectionId> result;
    std::copy_if(m_connectivity.begin(), m_connectivity.end(),
                 std::inserter(result, result.end()),
                 [&](ConnectionId const &cid) {
                     return (QtNodes::getNodeId(portType, cid) == nodeId
                             && QtNodes::getPortIndex(portType, cid) == portIndex);
                 });
    return result;
}

bool DeviceGraphModel::connectionExists(ConnectionId const connectionId) const
{
    return m_connectivity.find(connectionId) != m_connectivity.end();
}

DeviceGraphModel::NodeId DeviceGraphModel::addNode(QString const /*nodeType*/)
{
    // 设备节点只能通过 addDeviceNode() 创建，禁止画板右键新建空节点
    return InvalidNodeId;
}

bool DeviceGraphModel::connectionPossible(ConnectionId const connectionId) const
{
    if (connectionId.outNodeId == connectionId.inNodeId)
        return false;  // 禁止自连
    // 无向去重：任一方向已存在即不可再连
    ConnectionId rev{connectionId.inNodeId, connectionId.inPortIndex,
                     connectionId.outNodeId, connectionId.outPortIndex};
    if (m_connectivity.find(connectionId) != m_connectivity.end()
        || m_connectivity.find(rev) != m_connectivity.end())
        return false;

    // 以下规则仅交互式拖拽时校验；程序化还原已有连线时跳过
    if (m_enforceLimits) {
        // ① 最大连接数限制
        int maxOut = m_maxConn.value(connectionId.outNodeId, 4);
        int maxIn  = m_maxConn.value(connectionId.inNodeId, 4);
        if (connectionCountOf(connectionId.outNodeId) >= maxOut
            || connectionCountOf(connectionId.inNodeId) >= maxIn)
            return false;

        // ② 输入/输出端口空置规则：
        //    - 输入设备的“输入口”(左/IN) 须空着，其他设备不能连入 → 拒绝 inNode 为 input
        //    - 输出设备的“输出口”(右/OUT) 须空着，不能从其引出 → 拒绝 outNode 为 output
        if (ioRoleOf(connectionId.inNodeId) == QStringLiteral("input"))
            return false;
        if (ioRoleOf(connectionId.outNodeId) == QStringLiteral("output"))
            return false;
    }
    return true;
}

void DeviceGraphModel::addConnection(ConnectionId const connectionId)
{
    m_connectivity.insert(connectionId);
    Q_EMIT connectionCreated(connectionId);
}

bool DeviceGraphModel::nodeExists(NodeId const nodeId) const
{
    return m_nodeIds.find(nodeId) != m_nodeIds.end();
}

QVariant DeviceGraphModel::nodeData(NodeId nodeId, NodeRole role) const
{
    switch (role) {
    case NodeRole::Type:           return QStringLiteral("device");
    case NodeRole::Position:       return m_geom.value(nodeId).pos;
    case NodeRole::Size:           return m_geom.value(nodeId).size;
    case NodeRole::CaptionVisible: return true;
    case NodeRole::Caption:        return m_caption.value(nodeId, QStringLiteral("设备"));
    case NodeRole::Style: {
        auto style = StyleCollection::nodeStyle();
        return style.toJson().toVariantMap();
    }
    case NodeRole::InternalData:   return QVariant();
    case NodeRole::InPortCount:    return 1u;
    case NodeRole::OutPortCount:   return 1u;
    case NodeRole::Widget:         return QVariant();
    default:                       return QVariant();
    }
}

DeviceGraphModel::NodeFlags DeviceGraphModel::nodeFlags(NodeId /*nodeId*/) const
{
    return QtNodes::NodeFlag::NoFlags;  // 可拖动，但不可调整大小
}

bool DeviceGraphModel::setNodeData(NodeId nodeId, NodeRole role, QVariant value)
{
    switch (role) {
    case NodeRole::Position:
        m_geom[nodeId].pos = value.value<QPointF>();
        Q_EMIT nodePositionUpdated(nodeId);
        return true;
    case NodeRole::Size:
        m_geom[nodeId].size = value.value<QSize>();
        return true;
    default:
        return false;
    }
}

QVariant DeviceGraphModel::portData(NodeId nodeId, PortType portType,
                                    PortIndex /*portIndex*/, PortRole role) const
{
    switch (role) {
    case PortRole::Data:                 return QVariant();
    case PortRole::DataType:             return QVariant();
    case PortRole::ConnectionPolicyRole: return QVariant::fromValue(ConnectionPolicy::Many);
    case PortRole::CaptionVisible:       return true;
    case PortRole::Caption: {
        // 端口处仅显示该设备的最大连接数（左端口=输入，右端口=输出），不显示汉字
        Q_UNUSED(portType);
        return QString::number(m_maxConn.value(nodeId, 4));
    }
    default:                             return QVariant();
    }
}

bool DeviceGraphModel::setPortData(NodeId, PortType, PortIndex, QVariant const &, PortRole)
{
    return false;
}

bool DeviceGraphModel::deleteConnection(ConnectionId const connectionId)
{
    auto it = m_connectivity.find(connectionId);
    if (it == m_connectivity.end())
        return false;
    m_connectivity.erase(it);
    Q_EMIT connectionDeleted(connectionId);
    return true;
}

bool DeviceGraphModel::deleteNode(NodeId const /*nodeId*/)
{
    // 设备节点由设备列表统一增删，禁止从画板删除
    return false;
}

// =====================  DeviceTopologyView  =================================

DeviceTopologyView::DeviceTopologyView(QWidget *parent)
    : QWidget(parent)
{
    // ── 浅色主题：白色背景 + 深色节点 ────────────────────────────────────
    QtNodes::GraphicsViewStyle::setStyle(R"(
    {
      "GraphicsViewStyle": {
        "BackgroundColor": "#F5F7FA",
        "FineGridColor":   "#DDEEFF",
        "CoarseGridColor": "#C8DCF0"
      }
    }
    )");

    QtNodes::NodeStyle::setNodeStyle(R"(
    {
      "NodeStyle": {
        "NormalBoundaryColor":        "#6EA8D4",
        "SelectedBoundaryColor":      "#1565C0",
        "GradientColor0":             "#FFFFFF",
        "GradientColor1":             "#EBF4FF",
        "GradientColor2":             "#D6EAFF",
        "GradientColor3":             "#C2DFFF",
        "ShadowColor":                "#AAAAAA",
        "ShadowEnabled":              true,
        "FontColor":                  "#1C2B3A",
        "FontColorFaded":             "#6B7A8D",
        "ConnectionPointColor":       "#2980B9",
        "FilledConnectionPointColor": "#1565C0",
        "WarningColor":               "#F39C12",
        "ErrorColor":                 "#E74C3C",
        "PenWidth":                   1.5,
        "HoveredPenWidth":            2.5,
        "ConnectionPointDiameter":    8.0,
        "Opacity":                    1.0
      }
    }
    )");

    QtNodes::ConnectionStyle::setConnectionStyle(R"(
    {
      "ConnectionStyle": {
        "ConstructionColor":      "#999999",
        "NormalColor":            "#4A90D9",
        "SelectedColor":          "#1565C0",
        "SelectedHaloColor":      "#90CAF9",
        "HoveredColor":           "#1976D2",
        "LineWidth":              2.0,
        "ConstructionLineWidth":  1.5,
        "PointDiameter":          8.0,
        "UseDataDefinedColors":   false
      }
    }
    )");

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_view = new GraphicsView(this);
    m_view->setStyleSheet("QGraphicsView { background-color: #F5F7FA; border: 1px solid #D0DCE8; border-radius: 4px; }");
    m_view->setContextMenuPolicy(Qt::NoContextMenu);  // 禁用右键"新建节点"
    lay->addWidget(m_view);

    rebuild({}, {}, {});  // 初始空模型
}

DeviceTopologyView::~DeviceTopologyView()
{
    teardown();
}

void DeviceTopologyView::teardown()
{
    if (m_view)
        m_view->setScene(nullptr);
    delete m_scene;
    m_scene = nullptr;
    delete m_model;
    m_model = nullptr;
}

void DeviceTopologyView::hookModelSignals()
{
    connect(m_model, &DeviceGraphModel::connectionCreated,
            this, &DeviceTopologyView::onModelConnectionChanged);
    connect(m_model, &DeviceGraphModel::connectionDeleted,
            this, &DeviceTopologyView::onModelConnectionChanged);
}

void DeviceTopologyView::rebuild(const QStringList &keys,
                                 const QStringList &captions,
                                 const QList<QPair<QString, QString>> &conns,
                                 const QStringList &ioRoles,
                                 const QList<int> &maxConns)
{
    // 先销毁旧的 scene/model
    DeviceGraphModel          *oldModel = m_model;
    QtNodes::BasicGraphicsScene *oldScene = m_scene;

    m_model = new DeviceGraphModel();
    m_scene = new BasicGraphicsScene(*m_model, this);
    m_view->setScene(m_scene);

    if (oldScene) { delete oldScene; }
    if (oldModel) { delete oldModel; }

    hookModelSignals();

    m_applying = true;
    m_model->setEnforceLimits(false);  // 还原已有连线时不校验上限

    const int N = keys.size();

    // ── 按设备流向分组：输入靠左、输出靠右、普通居中 ──
    QList<int> leftIdx, rightIdx, midIdx;
    for (int i = 0; i < N; ++i) {
        QString io = (i < ioRoles.size()) ? ioRoles[i] : QString("normal");
        if (io == "input")       leftIdx  << i;
        else if (io == "output") rightIdx << i;
        else                     midIdx   << i;
    }

    // ── 节点大框尺寸（基本占据页面，四周留边距）──
    const double nodeW = 150.0, nodeH = 80.0, vGap = 40.0, margin = 60.0;
    int rows = qMax(qMax(leftIdx.size(), rightIdx.size()), qMax(midIdx.size(), 1));
    const double frameW = 780.0;
    const double frameH = qMax(440.0, 2 * margin + rows * nodeH + (rows - 1) * vGap);

    const double leftX  = margin;
    const double rightX = frameW - margin - nodeW;
    const double midX   = (frameW - nodeW) / 2.0;

    // 在指定列垂直居中摆放一组设备
    auto placeColumn = [&](const QList<int> &group, double x) {
        const int cnt = group.size();
        if (cnt == 0) return;
        double totalH = cnt * nodeH + (cnt - 1) * vGap;
        double startY = (frameH - totalH) / 2.0;
        for (int k = 0; k < cnt; ++k) {
            int i = group[k];
            QString cap = (i < captions.size() && !captions[i].isEmpty())
                              ? captions[i] : keys[i];
            int mc = (i < maxConns.size()) ? maxConns[i] : 4;
            QString io = (i < ioRoles.size()) ? ioRoles[i] : QStringLiteral("normal");
            DeviceGraphModel::NodeId id = m_model->addDeviceNode(keys[i], cap, mc, io);
            QPointF pos(x, startY + k * (nodeH + vGap));
            m_model->setNodeData(id, DeviceGraphModel::NodeRole::Position, pos);
        }
    };
    placeColumn(leftIdx,  leftX);
    placeColumn(midIdx,   midX);
    placeColumn(rightIdx, rightX);

    // ── 绘制代表“节点”的大框（位于所有设备之下）──
    QGraphicsRectItem *frame = new QGraphicsRectItem(0, 0, frameW, frameH);
    frame->setPen(QPen(QColor("#6EA8D4"), 2, Qt::DashLine));
    frame->setBrush(QBrush(QColor(110, 168, 212, 18)));
    frame->setZValue(-100);  // 置于设备节点之下
    frame->setFlag(QGraphicsItem::ItemIsSelectable, false);
    frame->setFlag(QGraphicsItem::ItemIsMovable, false);
    m_scene->addItem(frame);

    QGraphicsSimpleTextItem *frameLabel = new QGraphicsSimpleTextItem("节点", frame);
    QFont lf; lf.setPointSize(11); lf.setBold(true);
    frameLabel->setFont(lf);
    frameLabel->setBrush(QBrush(QColor("#1565C0")));
    frameLabel->setPos(10, 6);
    frameLabel->setZValue(-99);

    // 适当扩展场景范围，保证大框与边距完整可见
    m_scene->setSceneRect(QRectF(-40, -40, frameW + 80, frameH + 80));

    // 应用已有连线（按设备流向定向：input 引出、output 接入）
    for (const auto &c : conns) {
        DeviceGraphModel::NodeId a = m_model->nodeOf(c.first);
        DeviceGraphModel::NodeId b = m_model->nodeOf(c.second);
        if (a == InvalidNodeId || b == InvalidNodeId || a == b) continue;
        bool aSrc = flowAIsSource(m_model->ioRoleOf(a), m_model->ioRoleOf(b));
        DeviceGraphModel::NodeId src = aSrc ? a : b;
        DeviceGraphModel::NodeId snk = aSrc ? b : a;
        DeviceGraphModel::ConnectionId cid{src, 0, snk, 0};
        if (m_model->connectionPossible(cid))
            m_model->addConnection(cid);
    }

    m_model->setEnforceLimits(true);   // 还原完成，恢复交互式上限校验
    m_applying = false;
}

void DeviceTopologyView::updateConnections(const QList<QPair<QString, QString>> &conns)
{
    if (!m_model) return;

    m_applying = true;
    m_model->setEnforceLimits(false);  // 矩阵已做上限校验，此处程序化同步不再拦截

    // 期望的无向连线集合
    QList<QPair<QString, QString>> desired;
    for (const auto &c : conns)
        desired.append(norm(c.first, c.second));

    // 删除画板中多余的连线
    const auto current = m_model->allConnections();
    for (const auto &cid : current) {
        QPair<QString, QString> p = norm(m_model->keyOf(cid.outNodeId),
                                         m_model->keyOf(cid.inNodeId));
        if (!desired.contains(p))
            m_model->deleteConnection(cid);
    }

    // 补画缺失的连线（按设备流向定向）
    for (const auto &c : conns) {
        DeviceGraphModel::NodeId a = m_model->nodeOf(c.first);
        DeviceGraphModel::NodeId b = m_model->nodeOf(c.second);
        if (a == InvalidNodeId || b == InvalidNodeId || a == b) continue;
        bool aSrc = flowAIsSource(m_model->ioRoleOf(a), m_model->ioRoleOf(b));
        DeviceGraphModel::NodeId src = aSrc ? a : b;
        DeviceGraphModel::NodeId snk = aSrc ? b : a;
        DeviceGraphModel::ConnectionId cid{src, 0, snk, 0};
        if (m_model->connectionPossible(cid))
            m_model->addConnection(cid);
    }

    m_model->setEnforceLimits(true);
    m_applying = false;
}

QList<QPair<QString, QString>> DeviceTopologyView::connections() const
{
    QList<QPair<QString, QString>> out;
    if (!m_model) return out;
    for (const auto &cid : m_model->allConnections()) {
        QString a = m_model->keyOf(cid.outNodeId);
        QString b = m_model->keyOf(cid.inNodeId);
        if (a.isEmpty() || b.isEmpty()) continue;
        QPair<QString, QString> p = norm(a, b);
        if (!out.contains(p))
            out.append(p);
    }
    return out;
}

void DeviceTopologyView::onModelConnectionChanged()
{
    if (m_applying) return;  // 程序化修改，避免回环
    emit connectionsChanged(connections());
}
