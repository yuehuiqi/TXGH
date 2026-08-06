#ifndef DEVICETOPOLOGYVIEW_H
#define DEVICETOPOLOGYVIEW_H

// ============================================================================
//  DeviceTopologyView —— 基于 QtNodes(nodeeditor) 的设备拓扑可视化控件
//
//  对应 device_node_connections_design.md「方案 B：集成开源库 QtNodes」。
//  - 将节点内的每个设备实例渲染为一个图元节点（含 In/Out 端口）。
//  - 设备间连线与「设备连接矩阵」共享同一份数据，实现双向联动：
//      * 矩阵勾选  → updateConnections() 在画板补画/擦除连线；
//      * 画板拖拽连线/删除连线 → 发出 connectionsChanged() 通知外部刷新矩阵。
//  - 设备节点不可被用户删除（由设备列表统一管理），但可自由拖动与连线。
// ============================================================================

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QList>
#include <QPair>
#include <QHash>

#include <QtNodes/AbstractGraphModel>
#include <QtNodes/ConnectionIdUtils>

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
} // namespace QtNodes

// ─── 设备拓扑图数据模型（自定义最小 AbstractGraphModel）──────────────────────
class DeviceGraphModel : public QtNodes::AbstractGraphModel
{
    Q_OBJECT
public:
    using ConnectionId = QtNodes::ConnectionId;
    using NodeId       = QtNodes::NodeId;
    using NodeRole     = QtNodes::NodeRole;
    using NodeFlags    = QtNodes::NodeFlags;
    using PortIndex    = QtNodes::PortIndex;
    using PortRole     = QtNodes::PortRole;
    using PortType     = QtNodes::PortType;

    DeviceGraphModel();
    ~DeviceGraphModel() override;

    // —— 业务辅助 ——
    NodeId addDeviceNode(const QString &key, const QString &caption,
                         int maxConn = 4, const QString &ioRole = QStringLiteral("normal"));
    QString keyOf(NodeId id) const { return m_keyOf.value(id); }
    NodeId  nodeOf(const QString &key) const { return m_nodeOf.value(key, QtNodes::InvalidNodeId); }
    QString ioRoleOf(NodeId id) const { return m_ioRole.value(id, QStringLiteral("normal")); }
    std::unordered_set<ConnectionId> allConnections() const { return m_connectivity; }
    // 程序化批量建连时关闭校验（最大连接数 / 输入输出口），避免还原已有连线被拦截
    void setEnforceLimits(bool on) { m_enforceLimits = on; }
    int  connectionCountOf(NodeId id) const;

    // —— AbstractGraphModel 接口 ——
    std::unordered_set<NodeId> allNodeIds() const override;
    std::unordered_set<ConnectionId> allConnectionIds(NodeId const nodeId) const override;
    std::unordered_set<ConnectionId> connections(NodeId nodeId, PortType portType,
                                                 PortIndex portIndex) const override;
    bool connectionExists(ConnectionId const connectionId) const override;
    NodeId addNode(QString const nodeType = QString()) override;
    bool connectionPossible(ConnectionId const connectionId) const override;
    void addConnection(ConnectionId const connectionId) override;
    bool nodeExists(NodeId const nodeId) const override;
    QVariant nodeData(NodeId nodeId, NodeRole role) const override;
    NodeFlags nodeFlags(NodeId nodeId) const override;
    bool setNodeData(NodeId nodeId, NodeRole role, QVariant value) override;
    QVariant portData(NodeId nodeId, PortType portType, PortIndex portIndex,
                      PortRole role) const override;
    bool setPortData(NodeId nodeId, PortType portType, PortIndex portIndex,
                     QVariant const &value, PortRole role = PortRole::Data) override;
    bool deleteConnection(ConnectionId const connectionId) override;
    bool deleteNode(NodeId const nodeId) override;
    NodeId newNodeId() override { return m_nextNodeId++; }

private:
    struct NodeGeom { QPointF pos; QSize size; };

    std::unordered_set<NodeId>       m_nodeIds;
    std::unordered_set<ConnectionId> m_connectivity;
    QHash<NodeId, NodeGeom>          m_geom;
    QHash<NodeId, QString>           m_caption;
    QHash<NodeId, QString>           m_keyOf;
    QHash<QString, NodeId>           m_nodeOf;
    QHash<NodeId, int>               m_maxConn;          // 各设备最大连接数（端口数字 + 限制）
    QHash<NodeId, QString>           m_ioRole;           // 各设备流向 normal/input/output
    bool                             m_enforceLimits = true;
    NodeId                           m_nextNodeId = 0;
};

// ─── 可视化控件 ──────────────────────────────────────────────────────────────
class DeviceTopologyView : public QWidget
{
    Q_OBJECT
public:
    explicit DeviceTopologyView(QWidget *parent = nullptr);
    ~DeviceTopologyView() override;

    // 整体重建：设备实例 key 列表 + 显示名 + 已有连接（设备增删/打开对话框时调用）
    // ioRoles：各设备流向（normal/input/output），决定初始摆放（输入靠左、输出靠右）
    // maxConns：各设备最大连接数，显示在端口处并用于连线数限制
    void rebuild(const QStringList &keys,
                 const QStringList &captions,
                 const QList<QPair<QString, QString>> &conns,
                 const QStringList &ioRoles = {},
                 const QList<int> &maxConns = {});

    // 仅同步连线（矩阵勾选触发，不改变节点位置）
    void updateConnections(const QList<QPair<QString, QString>> &conns);

    // 当前画板中的连线（无向去重）
    QList<QPair<QString, QString>> connections() const;

signals:
    // 用户在画板上增删连线后发出，携带最新连线列表
    void connectionsChanged(const QList<QPair<QString, QString>> &conns);

private slots:
    void onModelConnectionChanged();

private:
    void teardown();
    void hookModelSignals();

    DeviceGraphModel          *m_model = nullptr;
    QtNodes::BasicGraphicsScene *m_scene = nullptr;
    QtNodes::GraphicsView       *m_view = nullptr;
    bool                        m_applying = false;  // 程序化修改时屏蔽信号回传
};

#endif // DEVICETOPOLOGYVIEW_H
