#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "datamodel.h"
#include "dbmanager.h"
#include "topoview.h"
#include "dialognode.h"
#include "dialogplanning.h"
#include "dialogscene.h"
#include "dialoglink.h"
#include "simbridge.h"
#include "idatastore.h"
#include "remotedatastore.h"

#include <QMainWindow>
#include <QMouseEvent>
#include <QPoint>
#include <QToolButton>
#include <QString>
#include <QTimer>
#include <QTime>
#include <QDateTime>
#include <QFile>
#include <QLabel>
#include <QProgressBar>
#include <QTableView>
#include <QStandardItemModel>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QDebug>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QtMath>
#include <QMenu>
#include <QList>
#include <QJsonObject>
#include <functional>

#include <QtCharts>
QT_CHARTS_USE_NAMESPACE

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// ─── 节点卡片代理 ─────────────────────────────────────────────────────────────
class NodeCardDelegate : public QStyledItemDelegate
{
public:
    NodeCardDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    std::function<void(int dbId)> onLocate;
    std::function<void(int dbId)> onConfigure;
    std::function<void(int dbId)> onDelete;

    static void calcBtnRects(const QRect &rect,
                              QRect &btn1, QRect &btn2, QRect &btn3);
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;
};

// ─── 邻接矩阵热力图代理 ───────────────────────────────────────────────────────
class MatrixDelegate : public QStyledItemDelegate
{
public:
    MatrixDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};

// ─── 实时告警卡片代理 ─────────────────────────────────────────────────────────
// UserRole   = level string ("info" / "warning" / "error")
// UserRole+1 = time string  ("hh:mm:ss")
class WarnCardDelegate : public QStyledItemDelegate
{
public:
    WarnCardDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};

// ─── 主窗口 ───────────────────────────────────────────────────────────────────
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    // ── 初始化 ──────────────────────────────────────────────────────────────
    void initDatabase();
    void clearAndSeedDatabase();
    void initTopoView();
    void initStatusBar();
    void initSimBridge();
    void loadScenesIntoCmb();

    // ── 全场景刷新（切换场景或数据变更后调用） ────────────────────────────
    void onSceneChanged(int sceneId);
    void refreshSceneTree(const QList<NodeInfo> &nodes);
    void refreshNodeListView(const QList<NodeInfo> &nodes, const QList<LinkInfo> &links);
    void refreshNodeStats(const QList<NodeInfo> &nodes);
    void refreshLinkListWidget(const QList<LinkInfo> &links,
                               const QList<NodeInfo> &nodes);
    void refreshLinkTable(const QList<LinkInfo> &links,
                          const QList<NodeInfo> &nodes);
    void refreshNodeTable(const QList<NodeInfo> &nodes);
    void refreshAdjMatrix(const QList<NodeInfo> &nodes,
                          const QList<LinkInfo> &links);
    void refreshDeviceStats(const QList<NodeInfo> &nodes);
    void refreshTopoView(const QList<NodeInfo> &nodes,
                         const QList<LinkInfo> &links);

    // ── 工具 ────────────────────────────────────────────────────────────────
    QString nodeIdToName(int nodeId, const QList<NodeInfo> &nodes) const;
    QToolButton* createToolButton(const QString &icon, const QString &tooltip);
    void buildAndSetLinkTableModel(const QList<LinkInfo> &links, const QList<NodeInfo> &nodes);
    void buildAndSetNodeTableModel(const QList<NodeInfo> &nodes);
    void exportTableToCsv(QTableView *table, const QString &title);

    bool isLinkValid(const LinkInfo &l, const QList<NodeInfo> &nodes) const;
    void cleanupInvalidLinks(int sceneId, int editedNodeId = -1);

    void initCharts();
    void initWarnList();
    QJsonObject buildPlanPayload() const;
    void postStatusLog(const QString &msg, bool isError = false);

private slots:
    // 窗口控制
    void on_btnMinimize_clicked();
    void on_btnMaximize_clicked();
    void on_btnClose_clicked();

    // 顶部导航
    void on_btnNavScene_clicked();
    void on_btnNavNode_clicked();
    void on_btnNavLink_clicked();
    void on_btnNavPlanning_clicked();

    // 场景 tab 操作
    void on_cmbCurrentScene_currentIndexChanged(int index);
    void on_btnSceneEdit_clicked();
    void on_btnSceneDelete_clicked();
    void on_btnSceneImport_clicked();
    void on_btnSceneOutput_clicked();

    // 链路 tab 操作
    void on_btnLinkDelete_clicked();
    void on_btnLinkDetail_clicked();

    // 中央视图切换
    void on_btnMap_clicked();
    void on_btnSimResult_clicked();
    void on_btnDataManage_clicked();

    // 右侧面板
    void on_btnDeviceDetails_clicked();

    // 拓扑视图右键菜单
    void showNodeContextMenu(int nodeId, const QPoint &globalPos);

    // 拓扑视图交互信号处理
    void onTopoRequestAddNode(double lon, double lat, const QString &nodeType);
    void onTopoRequestAddLink(int srcNodeId, int dstNodeId);
    void onTopoRequestMoveNode(int nodeId, double lon, double lat);
    void onTopoRequestEditNode(int nodeId);
    void onTopoRequestEditLink(int srcNodeId, int dstNodeId);
    void onTopoRequestDeleteNode(int nodeId);
    void onTopoRequestDeleteLink(int srcNodeId, int dstNodeId);

    // 节点/链路检索过滤
    void onNodeFilterChanged();
    void onLinkFilterChanged();

    // 数据视图过滤 + 导出
    void onDataViewLinkFilterChanged();
    void onDataViewNodeFilterChanged();
    void on_btnExportLinkCsv_clicked();
    void on_btnExportFlowCsv_clicked();

    // 仿真控制
    void on_btnSimToggle_clicked();
    // void on_btnSimStop_clicked();

    // SimBridge — 连接状态
    void onSimBridgeConnected();
    void onSimBridgeDisconnected();
    void onSimBridgeConnectionError(QString msg);

    // SimBridge — v3.0 分阶段推送
    void onPlanAcknowledged(quint64 taskId, int nodeCount, int flowCount);
    void onPlanProgress(int percent, QString stage);

    // SimBridge — v2.0 综合规划结果
    void onPlanResult(PlanResultPacket packet);
    void onSimError(QString msg);
    void onSimFinished();

    void onSceneTreeItemDoubleClicked(class QTreeWidgetItem *item, int column);
    void onSceneTreeItemClicked(class QTreeWidgetItem *item, int column);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    bool nativeEvent(const QByteArray &eventType,
                     void *message, long *result) override;

private:
    Ui::MainWindow *ui;
    // ★ 数据访问改为经接口访问：m_db 指向当前生效的实现。
    //   默认是 m_remoteDb（经协议请求服务端）；服务端连不上时
    //   回退到 m_localDb（直连数据库），保证演示和离线调试仍可用。
    // 服务端地址。数据通道与规划通道连的是同一个服务端，
    // 但各用一条连接（理由见 remotedatastore.h）。
    QString          m_serverHost = "127.0.0.1";
    quint16          m_serverPort = 9000;

    IDataStore      *m_db       = nullptr;   // 不拥有，指向下面两个之一
    DbManager       *m_localDb  = nullptr;
    RemoteDataStore *m_remoteDb = nullptr;
    bool             m_remoteMode = false;
    TopoView       *m_topoView = nullptr;
    int             m_currentSceneId = -1;
    bool            m_dbOk     = false;

    bool   m_isDragging   = false;
    QPoint m_dragPosition;

    QList<NodeInfo> m_currentNodes;
    QList<LinkInfo> m_currentLinks;
    QList<int>      m_adjNodeIds;

    // ── 状态栏标签 ──────────────────────────────────────────────────────────
    QLabel *m_lblStatusDb  = nullptr;   // 数据库连接状态（可点击修改路径）
    QLabel *m_lblStatusPy  = nullptr;   // Python通信状态（可点击修改地址）
    QLabel *m_lblStatusLog = nullptr;   // 关键日志/报错滚动展示

    // ── 仿真相关 ────────────────────────────────────────────────────────────
    SimBridge          *m_simBridge    = nullptr;
    QList<FlowInfo>     m_planFlows;
    QList<QPair<int,int>> m_planExclusions;   // 节点级禁连规则 [nodeA_id, nodeB_id]
    int                 m_planSceneId  = -1;
    bool                m_simRunning   = false;

    // 规划进度条。在代码里创建并插入到「启动」按钮所在的那条工具栏上，
    // 不改 .ui —— .ui 是 XML，手工改层级容易把 Designer 里的布局搞乱。
    QProgressBar       *m_planProgress = nullptr;

    QLineSeries        *m_seriesDelay  = nullptr;
    QLineSeries        *m_seriesLoss   = nullptr;

    QStandardItemModel *m_warnModel    = nullptr;

    QDateTime           m_simStartTime;   // 仿真启动时刻，用于计算耗时
};

#endif // MAINWINDOW_H
