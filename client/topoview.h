#ifndef TOPOVIEW_H
#define TOPOVIEW_H

#include "datamodel.h"

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QListWidget>
#include <QMap>
#include <QPair>
#include <QList>
#include <QPixmap>

// Forward declarations for custom item classes (defined in topoview.cpp)
class NodeGfxItem;
class LinkGfxItem;

class TopoView : public QGraphicsView
{
    Q_OBJECT
public:
    explicit TopoView(QWidget *parent = nullptr);
    ~TopoView() override;

    // ── 节点操作 ──────────────────────────────────────────────────────────────
    void addNode(int nodeId, const QString &name, const QString &type,
                 double lon, double lat, double alt, const QStringList &devices,
                 const QString &status = "在线");
    void removeNode(int nodeId);
    void updateNodeStatus(int nodeId, const QString &status);

    // ── 链路操作 ──────────────────────────────────────────────────────────────
    void addLink(int src, int dst, const QString &linkType,
                 const QString &wirelessType = QString());
    void removeLink(int src, int dst);

    // ── 整体操作 ──────────────────────────────────────────────────────────────
    void clearAll();
    void fitAll();
    void precomputeBounds(const QList<QPair<double,double>> &lonLats); // no-op with world map

    // ── 地图显示控制 ────────────────────────────────────────────
    void toggleMapVisibility();           // 切换地图显示/隐藏
    bool isMapVisible() const { return m_mapVisible; }

    // ── 路径高亮 ────────────────────────────────────────────
    void highlightPaths(const QList<QList<int>> &paths);
    void resetHighlight();

    // ── 链路多条信息更新 ──────────────────────────────────────
    // 设置两节点间的链路数量和详情（拓扑图显示数字并展示 tooltip）
    void updateLinkCountInfo(int src, int dst, int count, const QStringList &infoLines);

    // ── 坐标转换（公开，供 MainWindow 使用） ─────────────────────────────────
    QPointF lonLatToScene(double lon, double lat) const;
    static void scenePosToLonLat(const QPointF &scenePos, double &lon, double &lat);

signals:
    void nodeClicked(int nodeId);
    void nodeRightClicked(int nodeId, const QPoint &globalPos);

    // 拖放/交互请求（由 MainWindow 处理）
    void requestAddNode(double lon, double lat, const QString &nodeType);
    void requestAddLink(int srcNodeId, int dstNodeId);
    void requestMoveNode(int nodeId, double lon, double lat);  // 左键拖拽节点
    void requestEditNode(int nodeId);
    void requestEditLink(int srcNodeId, int dstNodeId);
    void requestDeleteNode(int nodeId);
    void requestDeleteLink(int srcNodeId, int dstNodeId);

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void resizeEvent(QResizeEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    bool viewportEvent(QEvent *e) override;          // 拦截 QEvent::ToolTip，做精准链路命中

private:
    NodeGfxItem *nodeAt(const QPoint &viewPos) const;
    LinkGfxItem *linkAt(const QPoint &viewPos) const;
    // 在屏幕坐标下找到离 viewPos 最近的链路（含徽章命中），返回 nullptr 表示无命中
    LinkGfxItem *closestLinkAt(const QPoint &viewPos, double maxPx = 10.0) const;
    void repositionWidgets();
    void updateLinksForNode(int nodeId);  // 节点移动后更新关联链路端点
    double minScale() const;              // 最小允许缩放比 = 视口宽 / SCENE_W
    void   clampZoom();                   // 强制不低于最小缩放
    // 根据屏幕坐标下的最近邻间距，自适应计算节点图标缩放因子（防止重叠）
    void   updateIconScale();

    // 图标缓存（按类型字符串→Pixmap）
    static QMap<QString, QPixmap> s_iconCache;
    static QPixmap getNodeIcon(const QString &type);

    QGraphicsScene   *m_scene;
    QPixmap           m_mapPixmap;     // 世界地图背景图

    QMap<int,              NodeGfxItem *>  m_nodes;   // nodeId → item
    QMap<QPair<int,int>,   LinkGfxItem *>  m_links;   // (src,dst) → item

    QListWidget      *m_palette;       // 左下角拖拽面板
    QWidget          *m_toolbar;       // 右下角工具栏

    // 左键拖拽移动节点状态
    int               m_movingNodeId  = -1;
    bool              m_movingNode    = false;

    // 右键拖拽创建链路状态
    int               m_linkSrcId     = -1;
    bool              m_drawingLink   = false;
    QGraphicsLineItem *m_tempLine     = nullptr;

    // 平移状态
    bool              m_panning       = false;
    QPoint            m_panLast;

    // 抑制右键菜单（拖拽结束后）
    bool              m_suppressContextMenu = false;

    // 鼠标按下位置（判断阈值）
    QPoint            m_pressPos;

    bool              m_mapVisible    = true;  // 地图背景可见性

    double            m_iconScale     = 1.0;   // 自适应图标缩放（0.25~1.0），由 updateIconScale() 维护

    static const int SCENE_W = 1800;
    static const int SCENE_H = 900;
    static const int ICON_R  = 18;    // 节点图标半径（屏幕像素，因 ItemIgnoresTransformations）
};

#endif // TOPOVIEW_H
