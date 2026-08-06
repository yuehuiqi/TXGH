#include "topoview.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QToolButton>
#include <QListWidgetItem>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QContextMenuEvent>
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QFont>
#include <QIcon>
#include <QScrollBar>
#include <QtMath>
#include <QPolygonF>
#include <QVariant>
#include <QHelpEvent>
#include <QToolTip>
#include <QGraphicsView>
#include <QCoreApplication>
#include <cmath>
#include <algorithm>
#include <limits>

static QString translateLinkType(const QString &engType) {
    static const QMap<QString, QString> map = {
        {"fiber",       "光缆"},
        {"fieldwire",   "野战电缆"},
        {"fieldWire",   "野战电缆"},
        {"microwave",   "微波接力"},
        {"scatter",     "超视距微波"},
        {"adhoc",       "自组网电台"},
        {"satellite",   "卫星通信"},
        {"cellular",    "移动公网"},
        {"shortwave",   "短波电台"},
        {"narrowband",  "窄带战术电台"},
        {"wired",       "有线"},
        {"wireless",    "无线"},
        {"switch",      "交换机"}
    };
    return map.value(engType.trimmed().toLower(), engType);
}

// 节点基准半径（屏幕像素，因 ItemIgnoresTransformations）
static const int NODE_BASE_R = 18;
// 节点最小可见半径（密集时收缩下限，保证仍可点击）
static const int NODE_MIN_R  = 5;
// 场景属性键：当前自适应图标缩放
static const char *kIconScaleProp = "iconScale";

// 从场景属性读取当前图标缩放
static double readIconScale(QGraphicsScene *sc) {
    if (!sc) return 1.0;
    QVariant v = sc->property(kIconScaleProp);
    if (!v.isValid()) return 1.0;
    double s = v.toDouble();
    if (s <= 0.0) return 1.0;
    return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
// NodeGfxItem — 自定义节点图元
// 渲染方案：
//   1. 设置 ItemIgnoresTransformations，使节点坐标系不随视图缩放，避免在低缩放比下
//      因场景坐标过小而像素化、消失。
//   2. paint() 时读取场景属性 "iconScale"（由 TopoView::updateIconScale() 写入），
//      对绘制结果做整体缩放。该缩放因子由视图当前缩放下、屏幕坐标系内"最近邻节点
//      距离"自适应推导：节点越密集（屏幕距离越小），图标越小，从而避免重叠成团。
//   3. 单节点最小直径限制为 NODE_MIN_R*2，保证用户始终可点击。
// ═══════════════════════════════════════════════════════════════════════════════
class NodeGfxItem : public QGraphicsItem
{
public:
    enum { Type = UserType + 1 };
    int type() const override { return Type; }

    NodeGfxItem(int nodeId, const QString &name, const QString &nodeType,
                const QString &status, const QPixmap &icon)
        : m_nodeId(nodeId), m_name(name), m_nodeType(nodeType)
        , m_status(status), m_icon(icon)
    {
        setFlag(ItemIsSelectable);
        setFlag(ItemIgnoresTransformations);
        setAcceptHoverEvents(true);
    }

    int     nodeId()   const { return m_nodeId;   }
    QString nodeType() const { return m_nodeType; }
    QString nodeName() const { return m_name;     }
    QString status()   const { return m_status;   }
    double  lon()      const { return m_lon;      }
    double  lat()      const { return m_lat;      }

    void setLonLat(double lon, double lat) { m_lon = lon; m_lat = lat; setToolTip(buildTooltip()); }
    void setLonLatAltDevices(double lon, double lat, double alt, const QStringList &devices) {
        m_lon = lon;
        m_lat = lat;
        m_alt = alt;
        m_devices = devices;
        setToolTip(buildTooltip());
    }
    void setStatus(const QString &s)       { m_status = s; update(); setToolTip(buildTooltip()); }
    void setHighlighted(bool on)           { m_highlighted = on; update(); }

    // bounding rect 以屏幕像素为单位，用 base 大小保证缩放后任意尺寸都覆盖到
    QRectF boundingRect() const override
    {
        const int R = NODE_BASE_R;
        return QRectF(-R - 8, -R - 8, (R + 8) * 2, (R + 8) * 2);
    }

    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override
    {
        // 自适应缩放因子（由 TopoView 计算并通过 scene property 共享）
        const double s = qBound(0.25, readIconScale(scene()), 1.0);

        p->save();
        p->scale(s, s);
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::SmoothPixmapTransform);

        const int R = NODE_BASE_R;

        // 选中 / 高亮光晕
        if (isSelected() || m_highlighted) {
            QColor glow = m_highlighted ? QColor("#F44336") : QColor("#1565C0");
            glow.setAlpha(60);
            p->setPen(Qt::NoPen);
            p->setBrush(glow);
            p->drawEllipse(QRectF(-R - 8, -R - 8, (R + 8) * 2, (R + 8) * 2));
        } else if (m_hovered) {
            QColor glow(21, 101, 192, 40);
            p->setPen(Qt::NoPen);
            p->setBrush(glow);
            p->drawEllipse(QRectF(-R - 5, -R - 5, (R + 5) * 2, (R + 5) * 2));
        }

        // 白色底盘
        p->setPen(QPen(QColor("#C8D8F0"), 1.5));
        p->setBrush(QColor("#FFFFFF"));
        p->drawEllipse(QRectF(-R, -R, R * 2, R * 2));

        // 图标或 fallback 彩色圆
        int inner = R - 3;
        if (!m_icon.isNull()) {
            p->drawPixmap(QRect(-inner, -inner, inner * 2, inner * 2), m_icon);
        } else {
            p->setPen(Qt::NoPen);
            p->setBrush(fallbackColor());
            p->drawEllipse(QRectF(-inner + 2, -inner + 2,
                                  (inner - 2) * 2, (inner - 2) * 2));
        }

        // 状态圆点（右上角）
        p->setPen(QPen(QColor("#FFFFFF"), 1.5));
        p->setBrush(statusColor());
        p->drawEllipse(QRectF(R - 8, -R, 9, 9));

        p->restore();
    }

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override { m_hovered = true;  update(); }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override { m_hovered = false; update(); }

private:
    QString buildTooltip() const {
        QStringList cnDevices;
        for (const QString &dev : m_devices) {
            cnDevices << translateLinkType(dev);
        }
        QString devicesStr = cnDevices.isEmpty() ? "无" : cnDevices.join(", ");

        QString nameStr = m_name.isEmpty() ? QString("N%1").arg(m_nodeId) : m_name;
        QString typeStr = m_nodeType;

        QString lonStr = QString("%1°%2").arg(qAbs(m_lon), 0, 'f', 4).arg(m_lon >= 0 ? "E" : "W");
        QString latStr = QString("%1°%2").arg(qAbs(m_lat), 0, 'f', 4).arg(m_lat >= 0 ? "N" : "S");
        QString altStr = QString("%1m").arg(m_alt, 0, 'f', 1);

        QString html = QString(
            "<html>"
            "<body style='font-family: sans-serif; font-size: 11px; margin: 0; padding: 2px;'>"
            "<table width='100%' style='border-bottom: 1px solid #CCCCCC; padding-bottom: 2px; margin-bottom: 4px;'>"
            "  <tr>"
            "    <td align='left' style='font-weight: bold; font-size: 12px; color: #1565C0;'>%1</td>"
            "    <td align='right' style='font-weight: bold; color: #E65100; padding-left: 20px;'>%2</td>"
            "  </tr>"
            "</table>"
            "<div style='margin-bottom: 3px;'><b>配备设备:</b> %3</div>"
            "<div><b>位置：</b>（%4, %5, %6）</div>"
            "</body>"
            "</html>"
        ).arg(nameStr, typeStr, devicesStr, lonStr, latStr, altStr);

        return html;
    }

    QColor fallbackColor() const {
        if (m_nodeType.contains("干线", Qt::CaseInsensitive)) return QColor("#E65100");
        if (m_nodeType.contains("支线", Qt::CaseInsensitive)) return QColor("#1565C0");
        if (m_nodeType.contains("一级ZK", Qt::CaseInsensitive)) return QColor("#1565C0");
        if (m_nodeType.contains("二级ZK", Qt::CaseInsensitive)) return QColor("#0288D1");
        if (m_nodeType.contains("RD",     Qt::CaseInsensitive)) return QColor("#E65100");
        if (m_nodeType.contains("FK",     Qt::CaseInsensitive)) return QColor("#558B2F");
        return QColor("#607D8B");
    }

    QColor statusColor() const {
        if (m_status == "离线") return QColor("#9E9E9E");
        if (m_status == "告警") return QColor("#FF9800");
        return QColor("#4CAF50");
    }

    int     m_nodeId;
    QString m_name, m_nodeType, m_status;
    QPixmap m_icon;
    double  m_lon = 0, m_lat = 0, m_alt = 0;
    QStringList m_devices;
    bool    m_hovered     = false;
    bool    m_highlighted = false;
};

// ═══════════════════════════════════════════════════════════════════════════════
// LinkGfxItem — 自定义链路图元（实线/虚线 + 中间箭头）
// ═══════════════════════════════════════════════════════════════════════════════
class LinkGfxItem : public QGraphicsItem
{
public:
    enum { Type = UserType + 2 };
    int type() const override { return Type; }

    LinkGfxItem(int src, int dst, const QString &linkType,
                const QString &wirelessType, const QPointF &p1, const QPointF &p2)
        : m_src(src), m_dst(dst), m_linkType(linkType)
        , m_wirelessType(wirelessType), m_p1(p1), m_p2(p2)
    {
        setFlag(ItemIsSelectable);
        setAcceptHoverEvents(true);
        setZValue(1);
        setToolTip(buildTooltip());
    }

    int     srcId()        const { return m_src;          }
    int     dstId()        const { return m_dst;          }
    QString linkType()     const { return m_linkType;     }
    QString wirelessType() const { return m_wirelessType; }
    QPointF p1()           const { return m_p1;           }
    QPointF p2()           const { return m_p2;           }
    int     linkCount()    const { return m_linkCount;    }

    void setPoints(const QPointF &p1, const QPointF &p2) {
        prepareGeometryChange();
        m_p1 = p1; m_p2 = p2;
    }
    void setHighlighted(bool on) { m_highlighted = on; update(); }

    void setLinkCount(int count)               { m_linkCount = count; setToolTip(buildTooltip()); update(); }
    void setLinkInfo(const QStringList &lines) { m_linkInfoLines = lines; setToolTip(buildTooltip()); update(); }

    // 给定足够大的固定 bounding rect，让 shape() 可以基于当前视图缩放
    // 自适应展开成"屏幕像素级"窄命中带，而无需在缩放变化时重算 boundingRect。
    QRectF boundingRect() const override {
        const double pad = 32.0;       // 场景单位，覆盖低缩放比下的徽章+命中带
        double lx = qMin(m_p1.x(), m_p2.x()) - pad;
        double ty = qMin(m_p1.y(), m_p2.y()) - pad;
        double w  = qAbs(m_p2.x() - m_p1.x()) + pad * 2;
        double h  = qAbs(m_p2.y() - m_p1.y()) + pad * 2;
        return QRectF(lx, ty, w, h);
    }

    // shape 用作精细命中：
    //  - 沿线段的 ~6px 屏幕宽度的命中带（独立于视图缩放）
    //  - 多链路徽章区域（半径 ~14px 屏幕）
    // 这样：放大时命中带依旧窄 → 不会误中相邻链路；缩小时命中带保持可点击宽度。
    QPainterPath shape() const override {
        QPainterPath path;
        double vs = 1.0;
        if (scene() && !scene()->views().isEmpty())
            vs = scene()->views().first()->transform().m11();
        vs = qMax(0.01, vs);

        QPointF dir = m_p2 - m_p1;
        double len = std::sqrt(dir.x()*dir.x() + dir.y()*dir.y());
        if (len >= 1.0) {
            const double HALF_BAND_PX = 6.0;        // 屏幕像素命中半宽
            const double halfScene = HALF_BAND_PX / vs;
            QPointF u = dir / len;
            QPointF n(-u.y() * halfScene, u.x() * halfScene);
            QPolygonF poly;
            poly << m_p1 + n << m_p2 + n << m_p2 - n << m_p1 - n;
            path.addPolygon(poly);
        } else {
            path.moveTo(m_p1);
            path.lineTo(m_p2);
        }

        if (m_linkCount > 1) {
            const double BADGE_R_PX = 14.0;
            const double badgeRScene = BADGE_R_PX / vs;
            QPointF mid = (m_p1 + m_p2) * 0.5;
            path.addEllipse(mid, badgeRScene, badgeRScene);
        }
        return path;
    }

    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        p->setRenderHint(QPainter::Antialiasing);
        // 与节点共用同一份自适应缩放，密集时徽章/箭头一起缩小
        const double s = qBound(0.25, readIconScale(scene()), 1.0);

        QPen pen = makePen();
        if (isSelected() || m_highlighted) {
            pen.setColor(m_highlighted ? QColor("#F44336") : QColor("#1565C0"));
            pen.setWidthF(2.5);
            pen.setStyle(Qt::SolidLine);
            pen.setCosmetic(true);
        } else if (m_hovered) {
            pen.setWidthF(pen.widthF() + 0.8);
        }
        p->setPen(pen);
        p->drawLine(m_p1, m_p2);

        drawBiArrows(p, pen.color(), s);

        if (m_linkCount > 1) {
            QTransform wt = p->worldTransform();
            QPointF midS = wt.map((m_p1 + m_p2) * 0.5);
            p->save();
            p->resetTransform();
            const double br = 10.0 * s;            // 徽章半径随密度缩放
            QRectF badge(midS.x() - br, midS.y() - br, br * 2, br * 2);
            p->setBrush(QColor("#7B1FA2"));
            p->setPen(Qt::NoPen);
            p->drawEllipse(badge);
            p->setPen(Qt::white);
            QFont f;
            f.setPixelSize(qMax(6, int(10 * s + 0.5)));
            f.setBold(true);
            p->setFont(f);
            p->drawText(badge, Qt::AlignCenter, QString::number(m_linkCount));
            p->restore();
        }
    }

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override { m_hovered = true;  update(); }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override { m_hovered = false; update(); }

private:
    QString buildTooltip() const {
        QString base = QString("链路 %1 ↔ %2").arg(m_src).arg(m_dst);
        if (!m_linkInfoLines.isEmpty()) {
            base += "\n" + m_linkInfoLines.join("\n");
        } else {
            QString cnLinkType = (m_linkType == "wireless" || m_linkType == "无线") ? "无线" : "有线";
            QString cnWirelessType = translateLinkType(m_wirelessType);
            QString wt = cnWirelessType.isEmpty() ? "" : " · " + cnWirelessType;
            base += QString("\n类型: %1%2").arg(cnLinkType).arg(wt);
        }
        return base;
    }

    QPen makePen() const {
        QPen pen;
        pen.setWidthF(1.5);
        pen.setCosmetic(true);
        if (m_linkType == "wired" || m_linkType == "有线") {
            pen.setStyle(Qt::SolidLine);
            pen.setColor(QColor("#37474F"));
        } else {
            pen.setStyle(Qt::DashLine);
            const QString &wt = m_wirelessType;
            if      (wt.contains("microwave", Qt::CaseInsensitive) || wt.contains("微波"))
                pen.setColor(QColor("#7B1FA2"));
            else if (wt.contains("scatter",   Qt::CaseInsensitive) || wt.contains("散射"))
                pen.setColor(QColor("#00838F"));
            else if (wt.contains("satellite", Qt::CaseInsensitive) || wt.contains("卫星"))
                pen.setColor(QColor("#F57F17"));
            else if (wt.contains("adhoc",     Qt::CaseInsensitive) || wt.contains("自组网"))
                pen.setColor(QColor("#1565C0"));
            else if (wt.contains("cellular",  Qt::CaseInsensitive) || wt.contains("4G") || wt.contains("5G"))
                pen.setColor(QColor("#00897B"));
            else if (wt.contains("shortwave", Qt::CaseInsensitive) || wt.contains("短波"))
                pen.setColor(QColor("#6D4C41"));
            else
                pen.setColor(QColor("#0277BD"));
        }
        return pen;
    }

    void drawBiArrows(QPainter *p, const QColor &color, double iconScale) const {
        QTransform wt = p->worldTransform();
        QPointF p1s = wt.map(m_p1);
        QPointF p2s = wt.map(m_p2);
        QPointF dir = p2s - p1s;
        double len  = std::sqrt(dir.x()*dir.x() + dir.y()*dir.y());
        if (len < 30.0 * iconScale) return;
        QPointF u = dir / len;
        QPointF n(-u.y(), u.x());
        const double s = 8.0 * iconScale;          // 箭头大小同步缩放

        p->save();
        p->resetTransform();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(color);

        QPointF a1 = p1s + dir * 0.85;
        QPolygonF arr1;
        arr1 << a1 + u * s
             << a1 - u * s + n * s * 0.55
             << a1 - u * s - n * s * 0.55;
        p->drawPolygon(arr1);

        QPointF a2 = p1s + dir * 0.15;
        QPolygonF arr2;
        arr2 << a2 - u * s
             << a2 + u * s + n * s * 0.55
             << a2 + u * s - n * s * 0.55;
        p->drawPolygon(arr2);

        p->restore();
    }

    int     m_src, m_dst;
    QString m_linkType, m_wirelessType;
    QPointF m_p1, m_p2;
    bool    m_hovered     = false;
    bool    m_highlighted = false;
    int     m_linkCount   = 1;
    QStringList m_linkInfoLines;
};

// ═══════════════════════════════════════════════════════════════════════════════
// TopoView 实现
// ═══════════════════════════════════════════════════════════════════════════════

QMap<QString, QPixmap> TopoView::s_iconCache;

TopoView::TopoView(QWidget *parent)
    : QGraphicsView(parent)
{
    m_scene = new QGraphicsScene(this);
    m_scene->setSceneRect(0, 0, SCENE_W, SCENE_H);
    m_scene->setProperty(kIconScaleProp, m_iconScale);   // 初始化共享缩放
    setScene(m_scene);

    setRenderHint(QPainter::Antialiasing);
    setRenderHint(QPainter::SmoothPixmapTransform);
    setDragMode(QGraphicsView::NoDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::ArrowCursor);

    // ── 加载世界地图底图（按候选路径依次尝试，跨平台兼容）─────────────────────
    {
        // 候选 1: 可执行文件同目录 HYP_HR/HYP_HR.jpg（典型部署布局）
        // 候选 2: 父目录 HYP_HR/HYP_HR.jpg（开发期 build-XX 目录与项目根并列）
        // 候选 3: 资源系统 :/resources/HYP_HR.jpg（如已嵌入 .qrc 时）
        // 候选 4: TXGHSTUDIO_MAP 环境变量指向的文件
        const QString appDir = QCoreApplication::applicationDirPath();
        QStringList candidates = {
            appDir + "/HYP_HR/HYP_HR.jpg",
            appDir + "/../HYP_HR/HYP_HR.jpg",
            appDir + "/../../HYP_HR/HYP_HR.jpg",
            ":/resources/HYP_HR.jpg",
            QString::fromLocal8Bit(qgetenv("TXGHSTUDIO_MAP"))
        };
        for (const QString &p : candidates) {
            if (p.isEmpty()) continue;
            if (m_mapPixmap.load(p)) {
                qInfo("[TopoView] 已加载地图图片：%s", qPrintable(p));
                break;
            }
        }
        if (m_mapPixmap.isNull())
            qWarning("[TopoView] 未能加载地图图片 HYP_HR.jpg，将使用纯色背景；"
                     "请将 HYP_HR/HYP_HR.jpg 放到可执行文件同级或父级目录");
    }

    // ── 左下角节点调色板（横向排列）──────────────────────────────────────────
    m_palette = new QListWidget(this);
    m_palette->setObjectName("topoPalette");
    m_palette->setViewMode(QListView::IconMode);
    m_palette->setFlow(QListView::LeftToRight);
    m_palette->setWrapping(false);
    m_palette->setMovement(QListView::Static);
    m_palette->setIconSize(QSize(20, 20));
    m_palette->setDragEnabled(true);
    m_palette->setDefaultDropAction(Qt::CopyAction);
    m_palette->setDragDropMode(QAbstractItemView::DragOnly);
    m_palette->setSelectionMode(QAbstractItemView::SingleSelection);
    m_palette->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_palette->setFocusPolicy(Qt::NoFocus);
    m_palette->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_palette->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_palette->setSpacing(2);
    m_palette->setStyleSheet(
        "QListWidget {"
        "  background: rgba(255,255,255,0.82);"
        "  border: 1px solid rgba(180,200,230,0.7);"
        "  border-radius: 10px;"
        "  padding: 3px 5px;"
        "}"
        "QListWidget::item {"
        "  border-radius: 6px;"
        "  padding: 2px 1px;"
        "  color: #2C3E50;"
        "  font-size: 9px;"
        "}"
        "QListWidget::item:hover { background: rgba(227,240,252,0.9); color: #1565C0; }"
        "QListWidget::item:selected { background: rgba(187,222,251,0.9); color: #0D47A1; }");

    auto addPaletteItem = [&](const QString &resPath, const QString &label,
                               const QString &nodeType) {
        QListWidgetItem *it = new QListWidgetItem(QIcon(resPath), QString(), m_palette);
        it->setData(Qt::UserRole, nodeType);
        it->setSizeHint(QSize(36, 36));
        it->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        it->setToolTip(QString("拖动添加「%1」节点").arg(label));
    };
    addPaletteItem(":/resources/icon/node_2zk.svg", "支线", "支线");
    addPaletteItem(":/resources/icon/node_rd_ruduan.svg",  "干线", "干线");

    // ── 右下角浮动工具栏 ─────────────────────────────────────────────────────
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName("topoToolbar");
    m_toolbar->setStyleSheet(
        "#topoToolbar {"
        "  background: rgba(255,255,255,0.82);"
        "  border: 1px solid rgba(180,200,230,0.7);"
        "  border-radius: 10px;"
        "}"
        "#topoToolbar QToolButton {"
        "  min-width: 0px;"
        "  min-height: 0px;"
        "  padding: 0px;"
        "  width: 28px;"
        "  height: 28px;"
        "}");

    auto makeBtn = [](const QString &text, const QString &tip) -> QToolButton* {
        QToolButton *b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setFixedSize(28, 28);
        b->setStyleSheet(
            "QToolButton { background:transparent; border:none; font-size:14px; color:#2C3E50; font-weight:600; min-width:0px; min-height:0px; width:28px; height:28px; padding:0px; }"
            "QToolButton:hover { background:rgba(227,240,252,0.9); border-radius:5px; color:#1565C0; }"
            "QToolButton:pressed { background:rgba(187,222,251,0.9); }");
        return b;
    };

    QToolButton *btnZoomIn  = makeBtn("+", "放大 (滚轮上)");
    QToolButton *btnZoomOut = makeBtn("−", "缩小 (滚轮下)");
    QToolButton *btnFit     = makeBtn("⊡", "适应全部节点");
    QToolButton *btnMap     = makeBtn("🗺", "切换地图显示");

    connect(btnZoomIn,  &QToolButton::clicked, this, [this]{ scale(1.25, 1.25); clampZoom(); updateIconScale(); });
    connect(btnZoomOut, &QToolButton::clicked, this, [this]{ scale(0.8,  0.8);  clampZoom(); updateIconScale(); });
    connect(btnFit,     &QToolButton::clicked, this, &TopoView::fitAll);
    connect(btnMap,     &QToolButton::clicked, this, &TopoView::toggleMapVisibility);

    QHBoxLayout *tLay = new QHBoxLayout(m_toolbar);
    tLay->setContentsMargins(5, 3, 5, 3);
    tLay->setSpacing(3);
    tLay->addWidget(btnZoomIn);
    tLay->addWidget(btnZoomOut);
    tLay->addWidget(btnFit);
    tLay->addWidget(btnMap);
    m_toolbar->adjustSize();
    m_toolbar->raise();
    m_palette->raise();
}

TopoView::~TopoView()
{
}

// ─── 坐标转换 ─────────────────────────────────────────────────────────────────
QPointF TopoView::lonLatToScene(double lon, double lat) const
{
    double x = (lon + 180.0) / 360.0 * SCENE_W;
    double y = (90.0 - lat)  / 180.0 * SCENE_H;
    return QPointF(x, y);
}

void TopoView::scenePosToLonLat(const QPointF &pos, double &lon, double &lat)
{
    lon = pos.x() / SCENE_W * 360.0 - 180.0;
    lat = 90.0 - pos.y() / SCENE_H * 180.0;
    lon = qBound(-180.0, lon, 180.0);
    lat = qBound( -90.0, lat,  90.0);
}

// ─── 缩放限制 ─────────────────────────────────────────────────────────────────
double TopoView::minScale() const
{
    int vw = viewport()->width();
    if (vw <= 0) vw = 1;
    return (double)vw / (double)SCENE_W;
}

// 无最大缩放上限：节点使用 ItemIgnoresTransformations，放再大也保持固定屏幕尺寸，
// 不会撑满屏幕。背景地图可被无限放大（像素化由用户接受）。
void TopoView::clampZoom()
{
    double ms = minScale();
    double cs = transform().m11();

    if (cs < ms) {
        QTransform t;
        t.scale(ms, ms);
        setTransform(t);
        if (!m_nodes.isEmpty()) {
            double cx = 0, cy = 0;
            for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
                cx += it.value()->pos().x();
                cy += it.value()->pos().y();
            }
            cx /= m_nodes.size();
            cy /= m_nodes.size();
            centerOn(cx, cy);
        } else {
            centerOn(SCENE_W / 2.0, SCENE_H / 2.0);
        }
    }
}

// ─── 自适应图标缩放 ─────────────────────────────────────────────────────────
// 算法：
//   1. 把所有节点 pos 通过当前视图缩放映射到"屏幕单位"坐标。
//   2. 对每个节点计算到其他节点的最小距离（最近邻），收集成数组并排序。
//   3. 取 30% 分位（即偏密的那一端，但避开极端的最小重叠对），乘以一个目标占空比
//      系数（0.85），得到目标图标直径。
//   4. 将目标直径 clamp 到 [NODE_MIN_R*2, NODE_BASE_R*2]，对应缩放比 [0.28, 1.0]。
// 该方案在视图缩放或节点位置变化时被调用，保持图标既不挤成一团也不过小不可见。
void TopoView::updateIconScale()
{
    double newScale;

    if (m_nodes.size() < 2) {
        newScale = 1.0;
    } else {
        const double vs = transform().m11();
        QVector<QPointF> sp;
        sp.reserve(m_nodes.size());
        for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
            QPointF p = it.value()->pos();
            sp.append(QPointF(p.x() * vs, p.y() * vs));
        }

        QVector<double> nn;
        nn.reserve(sp.size());
        for (int i = 0; i < sp.size(); ++i) {
            double minD2 = std::numeric_limits<double>::max();
            for (int j = 0; j < sp.size(); ++j) {
                if (i == j) continue;
                double dx = sp[i].x() - sp[j].x();
                double dy = sp[i].y() - sp[j].y();
                double d2 = dx*dx + dy*dy;
                if (d2 < minD2) minD2 = d2;
            }
            nn.append(std::sqrt(minD2));
        }
        std::sort(nn.begin(), nn.end());

        int idx = qBound(0, int(nn.size() * 0.30), nn.size() - 1);
        double p30 = nn[idx];

        const double baseDiam = NODE_BASE_R * 2.0;
        const double minDiam  = NODE_MIN_R  * 2.0;
        double targetDiam = qBound(minDiam, p30 * 0.85, baseDiam);
        newScale = targetDiam / baseDiam;
    }

    if (qAbs(newScale - m_iconScale) > 0.01) {
        m_iconScale = newScale;
        m_scene->setProperty(kIconScaleProp, m_iconScale);
        // 触发节点重绘：标记所有节点为 update（只重绘节点边界，不影响链路）
        for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it)
            it.value()->update();
    }
}

// ─── 图标缓存 ─────────────────────────────────────────────────────────────────
QPixmap TopoView::getNodeIcon(const QString &type)
{
    QString key;
    if      (type.contains("干线", Qt::CaseInsensitive))
        key = ":/resources/icon/node_rd_ruduan.svg";
    else if (type.contains("支线", Qt::CaseInsensitive))
        key = ":/resources/icon/node_2zk.svg";
    else if (type.contains("一级ZK", Qt::CaseInsensitive) || type.contains("主控"))
        key = ":/resources/icon/node_zk_zhukong.svg";
    else if (type.contains("二级ZK", Qt::CaseInsensitive))
        key = ":/resources/icon/node_2zk.svg";
    else if (type.contains("RD", Qt::CaseInsensitive) || type.contains("入端"))
        key = ":/resources/icon/node_rd_ruduan.svg";
    else if (type.contains("FK", Qt::CaseInsensitive) || type.contains("分控"))
        key = ":/resources/icon/node_rk_fenkong.svg";
    else
        return QPixmap();

    if (s_iconCache.contains(key)) return s_iconCache[key];

    QIcon icon(key);
    QPixmap pm = icon.isNull() ? QPixmap() : icon.pixmap(QSize(ICON_R * 4, ICON_R * 4));
    s_iconCache.insert(key, pm);
    return pm;
}

// ─── 命中检测 ────────────────────────────────────────────────────────────────
NodeGfxItem *TopoView::nodeAt(const QPoint &viewPos) const
{
    // 先尝试 Qt 内置命中（在低密度/正常缩放时可行）
    for (QGraphicsItem *it : items(viewPos)) {
        if (it->type() == NodeGfxItem::Type)
            return static_cast<NodeGfxItem*>(it);
    }
    // 备用：屏幕坐标邻近检测，半径随当前图标缩放调整
    int hitR = qMax(8, int(NODE_BASE_R * m_iconScale + 4));
    NodeGfxItem *best = nullptr;
    int bestDist = hitR + 1;
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        QPoint nodeViewPos = mapFromScene(it.value()->pos());
        int dx = viewPos.x() - nodeViewPos.x();
        int dy = viewPos.y() - nodeViewPos.y();
        int dist = (int)std::sqrt((double)(dx*dx + dy*dy));
        if (dist <= hitR && dist < bestDist) {
            bestDist = dist;
            best = it.value();
        }
    }
    return best;
}

LinkGfxItem *TopoView::linkAt(const QPoint &viewPos) const
{
    // 优先用屏幕坐标的精准最近邻匹配（≤10px），避免在密集图里多个链路 shape
    // 重叠时取错；找不到再退回 Qt 的内置命中
    if (LinkGfxItem *l = closestLinkAt(viewPos, 10.0))
        return l;
    for (QGraphicsItem *it : items(viewPos)) {
        if (it->type() == LinkGfxItem::Type)
            return static_cast<LinkGfxItem*>(it);
    }
    return nullptr;
}

// 点到线段的最短距离（2D）
static double distPointToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    QPointF ab = b - a;
    double abLen2 = ab.x()*ab.x() + ab.y()*ab.y();
    if (abLen2 < 1e-6) {
        double dx = p.x() - a.x(), dy = p.y() - a.y();
        return std::sqrt(dx*dx + dy*dy);
    }
    QPointF ap = p - a;
    double t = (ap.x()*ab.x() + ap.y()*ab.y()) / abLen2;
    t = qBound(0.0, t, 1.0);
    QPointF proj = a + ab * t;
    double dx = p.x() - proj.x(), dy = p.y() - proj.y();
    return std::sqrt(dx*dx + dy*dy);
}

LinkGfxItem *TopoView::closestLinkAt(const QPoint &viewPos, double maxPx) const
{
    LinkGfxItem *best = nullptr;
    double bestDist = maxPx + 1.0;
    QPointF vp(viewPos);
    const double BADGE_R = 14.0;   // 与 shape() 中保持一致

    for (auto it = m_links.constBegin(); it != m_links.constEnd(); ++it) {
        LinkGfxItem *lnk = it.value();
        if (!lnk) continue;
        QPointF a = mapFromScene(lnk->p1());
        QPointF b = mapFromScene(lnk->p2());

        double d = distPointToSegment(vp, a, b);

        // 徽章命中：cursor 落在多链路数字圈内时，视作命中该链路
        if (lnk->linkCount() > 1) {
            QPointF mid = (a + b) * 0.5;
            double dx = vp.x() - mid.x(), dy = vp.y() - mid.y();
            double dBadge = std::sqrt(dx*dx + dy*dy);
            if (dBadge <= BADGE_R) d = 0.0;
        }

        if (d < bestDist) {
            bestDist = d;
            best = lnk;
        }
    }
    return best;
}

// ─── 节点操作 ─────────────────────────────────────────────────────────────────
void TopoView::addNode(int nodeId, const QString &name, const QString &type,
                       double lon, double lat, double alt, const QStringList &devices,
                       const QString &status)
{
    if (m_nodes.contains(nodeId)) removeNode(nodeId);

    QPixmap icon = getNodeIcon(type);
    NodeGfxItem *item = new NodeGfxItem(nodeId, name, type, status, icon);
    item->setLonLatAltDevices(lon, lat, alt, devices);
    item->setPos(lonLatToScene(lon, lat));
    item->setZValue(2);
    m_scene->addItem(item);
    m_nodes.insert(nodeId, item);

    updateIconScale();
}

void TopoView::removeNode(int nodeId)
{
    if (!m_nodes.contains(nodeId)) return;
    NodeGfxItem *item = m_nodes.take(nodeId);
    m_scene->removeItem(item);
    delete item;

    QList<QPair<int,int>> toRemove;
    for (auto it = m_links.begin(); it != m_links.end(); ++it) {
        if (it.key().first == nodeId || it.key().second == nodeId)
            toRemove << it.key();
    }
    for (auto &k : toRemove) removeLink(k.first, k.second);

    updateIconScale();
}

void TopoView::updateNodeStatus(int nodeId, const QString &status)
{
    if (m_nodes.contains(nodeId))
        m_nodes[nodeId]->setStatus(status);
}

// ─── 链路操作 ─────────────────────────────────────────────────────────────────
void TopoView::addLink(int src, int dst, const QString &linkType,
                       const QString &wirelessType)
{
    QPair<int,int> key(src, dst);
    if (m_links.contains(key)) removeLink(src, dst);
    if (!m_nodes.contains(src) || !m_nodes.contains(dst)) return;

    QPointF p1 = m_nodes[src]->pos();
    QPointF p2 = m_nodes[dst]->pos();

    LinkGfxItem *item = new LinkGfxItem(src, dst, linkType, wirelessType, p1, p2);
    m_scene->addItem(item);
    m_links.insert(key, item);
}

void TopoView::removeLink(int src, int dst)
{
    QPair<int,int> key(src, dst);
    if (!m_links.contains(key)) return;
    LinkGfxItem *item = m_links.take(key);
    m_scene->removeItem(item);
    delete item;
}

// ─── 节点移动时同步更新关联链路端点 ─────────────────────────────────────────
void TopoView::updateLinksForNode(int nodeId)
{
    if (!m_nodes.contains(nodeId)) return;
    QPointF nodePos = m_nodes[nodeId]->pos();
    for (auto it = m_links.begin(); it != m_links.end(); ++it) {
        if (it.key().first == nodeId) {
            QPointF other = m_nodes.contains(it.key().second)
                            ? m_nodes[it.key().second]->pos() : QPointF();
            it.value()->setPoints(nodePos, other);
        } else if (it.key().second == nodeId) {
            QPointF other = m_nodes.contains(it.key().first)
                            ? m_nodes[it.key().first]->pos() : QPointF();
            it.value()->setPoints(other, nodePos);
        }
    }
}

// ─── 整体操作 ─────────────────────────────────────────────────────────────────
void TopoView::clearAll()
{
    m_scene->clear();
    m_nodes.clear();
    m_links.clear();
    m_tempLine           = nullptr;
    m_drawingLink        = false;
    m_linkSrcId          = -1;
    m_movingNodeId       = -1;
    m_movingNode         = false;
    m_panning            = false;
    m_suppressContextMenu = false;
    m_iconScale          = 1.0;
    m_scene->setProperty(kIconScaleProp, m_iconScale);
}

void TopoView::precomputeBounds(const QList<QPair<double,double>> &)
{
    // 世界地图坐标系固定，无需动态边界
}

void TopoView::fitAll()
{
    int vw = viewport()->width();
    int vh = viewport()->height();

    if (m_nodes.isEmpty() || vw <= 0 || vh <= 0) {
        double ms = minScale();
        QTransform t;
        t.scale(ms, ms);
        setTransform(t);
        centerOn(SCENE_W / 2.0, SCENE_H / 2.0);
        updateIconScale();
        return;
    }

    // 计算当前所有节点的完整轴对齐边界框（不排除离群点 —— 必须保证每个节点可见）
    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    for (auto it = m_nodes.constBegin(); it != m_nodes.constEnd(); ++it) {
        QPointF p = it.value()->pos();
        if (p.x() < minX) minX = p.x();
        if (p.x() > maxX) maxX = p.x();
        if (p.y() < minY) minY = p.y();
        if (p.y() > maxY) maxY = p.y();
    }

    // 中心取边界框几何中心（而非质心），保证最远的节点距视口中心对称分布
    double cx = (minX + maxX) * 0.5;
    double cy = (minY + maxY) * 0.5;

    double rangeX = maxX - minX;
    double rangeY = maxY - minY;

    // 单节点 / 完全重叠：保留一个合理的默认可视范围（约 0.5° 经度）
    const double MIN_RANGE = 2.5;       // 场景单位
    rangeX = qMax(rangeX, MIN_RANGE);
    rangeY = qMax(rangeY, MIN_RANGE);

    // 节点图标使用 ItemIgnoresTransformations，占据固定屏幕像素。
    // 必须在视口里预留 (图标半径 + 视觉留白) 像素，否则边缘节点的图标会被截断。
    const int EDGE_PX = NODE_BASE_R + 16;
    int availW = qMax(1, vw - EDGE_PX * 2);
    int availH = qMax(1, vh - EDGE_PX * 2);

    double scaleX = (double)availW / rangeX;
    double scaleY = (double)availH / rangeY;
    double newScale = qMin(scaleX, scaleY);

    // 仅设下限；无上限（地图可无限放大）
    double ms = minScale();
    newScale = qMax(newScale, ms);

    QTransform t;
    t.scale(newScale, newScale);
    setTransform(t);
    centerOn(cx, cy);

    updateIconScale();
}

// ─── 路径高亮 ─────────────────────────────────────────────────────────────────
void TopoView::highlightPaths(const QList<QList<int>> &paths)
{
    resetHighlight();
    for (const QList<int> &path : paths) {
        for (int i = 0; i < path.size() - 1; ++i) {
            QPair<int,int> k(path[i], path[i+1]);
            QPair<int,int> rk(path[i+1], path[i]);
            if      (m_links.contains(k))  m_links[k]->setHighlighted(true);
            else if (m_links.contains(rk)) m_links[rk]->setHighlighted(true);
            if (m_nodes.contains(path[i])) m_nodes[path[i]]->setHighlighted(true);
        }
        if (!path.isEmpty() && m_nodes.contains(path.last()))
            m_nodes[path.last()]->setHighlighted(true);
    }
}

void TopoView::resetHighlight()
{
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it)
        it.value()->setHighlighted(false);
    for (auto it = m_links.begin(); it != m_links.end(); ++it)
        it.value()->setHighlighted(false);
}

// ─── 背景绘制（世界地图）─────────────────────────────────────────────────────
void TopoView::drawBackground(QPainter *painter, const QRectF &rect)
{
    Q_UNUSED(rect)
    if (m_mapVisible && !m_mapPixmap.isNull()) {
        painter->drawPixmap(QRectF(0, 0, SCENE_W, SCENE_H), m_mapPixmap,
                            QRectF(m_mapPixmap.rect()));
    } else {
        painter->fillRect(QRectF(0, 0, SCENE_W, SCENE_H), QColor("#FFFFFF"));
    }
}

// ─── 控件定位（调色板左下，工具栏右下）────────────────────────────────────────
void TopoView::repositionWidgets()
{
    const int margin = 6;
    if (m_palette) {
        m_palette->setFixedWidth(84);
        m_palette->setFixedHeight(42);
        int py = height() - m_palette->height() - margin;
        m_palette->move(margin, qMax(margin, py));
        m_palette->raise();
    }
    if (m_toolbar) {
        m_toolbar->setFixedWidth(132);
        m_toolbar->setFixedHeight(34);
        int ty = height() - m_toolbar->height() - margin;
        m_toolbar->move(width() - m_toolbar->width() - margin, qMax(margin, ty));
        m_toolbar->raise();
    }
}

void TopoView::resizeEvent(QResizeEvent *e)
{
    QGraphicsView::resizeEvent(e);
    repositionWidgets();
    clampZoom();
    updateIconScale();
}

// ─── 鼠标事件 ────────────────────────────────────────────────────────────────
void TopoView::mousePressEvent(QMouseEvent *e)
{
    m_pressPos = e->pos();

    if (e->button() == Qt::LeftButton) {
        NodeGfxItem *node = nodeAt(e->pos());
        if (node) {
            m_movingNodeId = node->nodeId();
            m_movingNode   = false;
            m_scene->clearSelection();
            node->setSelected(true);
            e->accept();
            return;
        }
        LinkGfxItem *link = linkAt(e->pos());
        if (link) {
            m_scene->clearSelection();
            link->setSelected(true);
            m_movingNodeId = -1;
            e->accept();
            return;
        }
        m_movingNodeId = -1;
        m_panning = true;
        m_panLast = e->pos();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }

    QGraphicsView::mousePressEvent(e);
}

void TopoView::mouseMoveEvent(QMouseEvent *e)
{
    if ((e->buttons() & Qt::LeftButton) && m_movingNodeId != -1) {
        QPoint delta = e->pos() - m_pressPos;
        if (!m_movingNode && delta.manhattanLength() > 6) {
            m_movingNode = true;
            setCursor(Qt::SizeAllCursor);
        }
        if (m_movingNode && m_nodes.contains(m_movingNodeId)) {
            QPointF newScenePos = mapToScene(e->pos());
            m_nodes[m_movingNodeId]->setPos(newScenePos);
            double lon, lat;
            scenePosToLonLat(newScenePos, lon, lat);
            m_nodes[m_movingNodeId]->setLonLat(lon, lat);
            updateLinksForNode(m_movingNodeId);
        }
        e->accept();
        return;
    }

    if (m_panning && (e->buttons() & Qt::LeftButton)) {
        QPoint delta = e->pos() - m_panLast;
        m_panLast = e->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        e->accept();
        return;
    }

    QGraphicsView::mouseMoveEvent(e);
}

void TopoView::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        if (m_movingNode && m_movingNodeId != -1 && m_nodes.contains(m_movingNodeId)) {
            double lon, lat;
            scenePosToLonLat(m_nodes[m_movingNodeId]->pos(), lon, lat);
            emit requestMoveNode(m_movingNodeId, lon, lat);
            updateIconScale();   // 节点位置变化后重算
        } else if (m_movingNodeId != -1 && !m_movingNode) {
            emit nodeClicked(m_movingNodeId);
        }
        m_movingNodeId = -1;
        m_movingNode   = false;

        if (m_panning) {
            m_panning = false;
        }
        setCursor(Qt::ArrowCursor);
        e->accept();
        return;
    }

    QGraphicsView::mouseReleaseEvent(e);
}

void TopoView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        NodeGfxItem *node = nodeAt(e->pos());
        if (node) { emit requestEditNode(node->nodeId()); e->accept(); return; }
        LinkGfxItem *link = linkAt(e->pos());
        if (link) { emit requestEditLink(link->srcId(), link->dstId()); e->accept(); return; }
    }
    QGraphicsView::mouseDoubleClickEvent(e);
}

void TopoView::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Delete) {
        for (QGraphicsItem *it : m_scene->selectedItems()) {
            if (it->type() == NodeGfxItem::Type)
                emit requestDeleteNode(static_cast<NodeGfxItem*>(it)->nodeId());
            else if (it->type() == LinkGfxItem::Type) {
                auto *lnk = static_cast<LinkGfxItem*>(it);
                emit requestDeleteLink(lnk->srcId(), lnk->dstId());
            }
        }
        e->accept();
        return;
    }
    QGraphicsView::keyPressEvent(e);
}

void TopoView::contextMenuEvent(QContextMenuEvent *e)
{
    if (m_suppressContextMenu) {
        m_suppressContextMenu = false;
        e->accept();
        return;
    }
    NodeGfxItem *node = nodeAt(e->pos());
    if (node) { emit nodeRightClicked(node->nodeId(), e->globalPos()); return; }
    QGraphicsView::contextMenuEvent(e);
}

// ─── 工具提示：精准选最近的链路 ─────────────────────────────────────────────
// Qt 默认会把工具提示交给 shape() 命中的"顶层"图元，但在密集场景里多个链路的
// shape 可能同时覆盖光标 → 选错。此处接管 QEvent::ToolTip：
//   1. 节点 hit 最高优先（半径按 NODE_BASE_R * iconScale 估算）
//   2. 否则在屏幕坐标下选离光标最近的链路（含徽章命中）
bool TopoView::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const QPoint vp = he->pos();

        // 节点优先
        if (NodeGfxItem *n = nodeAt(vp)) {
            const QString tip = n->toolTip();
            if (!tip.isEmpty()) {
                QToolTip::showText(he->globalPos(), tip, viewport());
                return true;
            }
        }

        // 链路：屏幕坐标下最近的（含徽章），命中阈值放宽到 14px 易触发
        if (LinkGfxItem *l = closestLinkAt(vp, 14.0)) {
            const QString tip = l->toolTip();
            if (!tip.isEmpty()) {
                QToolTip::showText(he->globalPos(), tip, viewport());
                return true;
            }
        }

        QToolTip::hideText();
        e->ignore();
        return true;
    }
    return QGraphicsView::viewportEvent(e);
}

// ─── 拖放事件（从调色板拖入节点）─────────────────────────────────────────────
void TopoView::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->source() == m_palette) e->acceptProposedAction();
    else QGraphicsView::dragEnterEvent(e);
}

void TopoView::dragMoveEvent(QDragMoveEvent *e)
{
    if (e->source() == m_palette) e->acceptProposedAction();
    else QGraphicsView::dragMoveEvent(e);
}

void TopoView::dropEvent(QDropEvent *e)
{
    if (e->source() == m_palette) {
        QListWidgetItem *it = m_palette->currentItem();
        if (!it) { e->ignore(); return; }
        QString nodeType = it->data(Qt::UserRole).toString();
        QPointF scenePos = mapToScene(e->pos());
        double lon, lat;
        scenePosToLonLat(scenePos, lon, lat);
        emit requestAddNode(lon, lat, nodeType);
        e->acceptProposedAction();
        return;
    }
    QGraphicsView::dropEvent(e);
}

// ─── 滚轮缩放 ───────────────────────────────────────────────────────────────
void TopoView::wheelEvent(QWheelEvent *e)
{
    double factor = (e->delta() > 0) ? 1.15 : (1.0 / 1.15);
    scale(factor, factor);
    clampZoom();
    updateIconScale();
    e->accept();
}

// ─── 切换地图显示/隐藏 ───────────────────────────────────────────────────────
void TopoView::toggleMapVisibility()
{
    m_mapVisible = !m_mapVisible;
    scene()->update();
}

void TopoView::updateLinkCountInfo(int src, int dst, int count, const QStringList &infoLines)
{
    for (auto it = m_links.begin(); it != m_links.end(); ++it) {
        LinkGfxItem *item = it.value();
        if (!item) continue;
        bool match = (item->srcId() == src && item->dstId() == dst)
                  || (item->srcId() == dst && item->dstId() == src);
        if (match) {
            item->setLinkCount(count);
            item->setLinkInfo(infoLines);
        }
    }
}
