#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QMessageBox>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QAbstractButton>
#include "framelessdialog.h"
#include <QTextStream>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTableView>
#include <QHeaderView>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "tableutils.h"

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

// ═══════════════════════════════════════════════════════════════════════════════
// NodeCardDelegate
// ═══════════════════════════════════════════════════════════════════════════════

// UserRole 映射：
//   UserRole+0  : nodeId (int，场景内仿真ID)
//   UserRole+1  : name   (QString)
//   UserRole+2  : type   (QString)
//   UserRole+3  : location (QString，"经度 纬度")
//   UserRole+4  : linkCount (int)
//   UserRole+5  : status (QString: 在线/离线/告警)
//   UserRole+6  : dbId   (int，数据库主键)

void NodeCardDelegate::calcBtnRects(const QRect &rect,
                                     QRect &btn1, QRect &btn2, QRect &btn3)
{
    const int hMargin = 10;
    const int btnH    = 22;
    const int gap     = 5;
    const int btnY    = rect.bottom() - btnH - 5;
    const int btnW    = (rect.width() - 2*hMargin - 2*gap) / 3;
    btn1 = QRect(rect.left() + hMargin,                  btnY, btnW, btnH);
    btn2 = QRect(rect.left() + hMargin + (btnW + gap),   btnY, btnW, btnH);
    btn3 = QRect(rect.left() + hMargin + 2*(btnW + gap), btnY, btnW, btnH);
}

void NodeCardDelegate::paint(QPainter *painter,
                              const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    int     nodeId         = index.data(Qt::UserRole).toInt();
    QString name           = index.data(Qt::UserRole + 1).toString();
    QString type           = index.data(Qt::UserRole + 2).toString();
    QString location       = index.data(Qt::UserRole + 3).toString();
    int     linkCount      = index.data(Qt::UserRole + 4).toInt();
    QString status         = index.data(Qt::UserRole + 5).toString();
    double  altitude       = index.data(Qt::UserRole + 7).toDouble();
    double  interferenceDb = index.data(Qt::UserRole + 8).toDouble();

    bool isOnline  = (status == "在线");
    bool isWarning = (status == "告警");
    bool isOffline = !isOnline && !isWarning;

    QColor dotColor = isOnline  ? QColor("#27AE60")
                    : isWarning ? QColor("#F39C12")
                                : QColor("#E74C3C");
    QColor cardBg   = isOffline ? QColor("#FAFAFA") : QColor("#FFFFFF");
    QColor borderClr = isOffline ? QColor("#D5D5D5") : QColor("#D4E6F1");

    QRect rect = option.rect.adjusted(3, 4, -3, -4);
    const int hM = 10;

    bool hovered = option.state & QStyle::State_MouseOver;
    if (hovered) { cardBg = QColor("#EBF4FF"); borderClr = QColor("#2980B9"); }

    // ── 卡片背景
    painter->setPen(QPen(borderClr, 1));
    painter->setBrush(cardBg);
    painter->drawRoundedRect(rect, 7, 7);

    // ── 左侧状态色竖条（3px）
    painter->setPen(Qt::NoPen);
    painter->setBrush(dotColor);
    painter->drawRoundedRect(QRect(rect.left() + 1, rect.top() + 6, 3, rect.height() - 12), 2, 2);

    // ── 第一行：状态圆点 + 节点名称 + 类型标签
    const int contentL = rect.left() + hM + 6;   // 左边内容起始（竖条右侧留空）
    int y = rect.top() + 13;

    // 状态圆点（9px）
    painter->setPen(Qt::NoPen);
    painter->setBrush(dotColor);
    painter->drawEllipse(contentL, y, 9, 9);

    // 节点名称
    QFont fName = painter->font();
    fName.setPointSize(10); fName.setBold(true);
    painter->setFont(fName);
    painter->setPen(isOffline ? QColor("#9E9E9E") : QColor("#1C2B3A"));
    QString displayName = name.isEmpty() ? QString("节点%1").arg(nodeId) : name;
    painter->drawText(contentL + 14, y + 10, displayName);

    // 节点类型（右对齐小标签）
    QFont fTag = painter->font();
    fTag.setPointSize(8); fTag.setBold(false);
    painter->setFont(fTag);
    QFontMetrics fmTag(fTag);
    int tagW = fmTag.horizontalAdvance(type) + 10;
    int tagH = 17;
    int tagX = rect.right() - hM - tagW;
    int tagY = rect.top() + 10;
    QColor tagBg = isOffline ? QColor("#BDBDBD") : QColor("#5B8DB8");
    painter->setBrush(tagBg);
    painter->setPen(Qt::NoPen);
    painter->drawRoundedRect(QRect(tagX, tagY, tagW, tagH), 3, 3);
    painter->setPen(QColor("#FFFFFF"));
    painter->drawText(QRect(tagX, tagY, tagW, tagH), Qt::AlignCenter, type);

    // ── 分隔线
    y = rect.top() + 33;
    painter->setPen(QColor("#EDF2F7"));
    painter->drawLine(rect.left() + hM, y, rect.right() - hM, y);

    // ── 位置（所有节点都显示，离线用灰色）
    QFont fInfo = painter->font();
    fInfo.setPointSize(8); fInfo.setBold(false);
    painter->setFont(fInfo);
    QFontMetrics fmInfo(fInfo);

    y += 13;
    painter->setPen(isOffline ? QColor("#AAAAAA") : QColor("#4A5568"));
    painter->drawText(contentL - 6, y,
        QString("位置：(%1, %2m)").arg(location).arg(altitude, 0, 'f', 0));

    // ── 链路数（左）+ 干扰（右），同一行
    y += 16;
    painter->setPen(isOffline ? QColor("#BBBBBB") : QColor("#2471A3"));
    painter->drawText(contentL - 6, y, QString("链路：%1条").arg(linkCount));

    QString interStr = QString("干扰：%1dBm").arg(interferenceDb, 0, 'f', 1);
    painter->setPen(isOffline ? QColor("#BBBBBB") : QColor("#7F8C8D"));
    painter->drawText(rect.right() - hM - fmInfo.horizontalAdvance(interStr), y, interStr);

    // 按钮行（统一：定位/配置/删除）
    QRect btn1, btn2, btn3;
    calcBtnRects(rect, btn1, btn2, btn3);

    auto drawBtn = [&](const QRect &r, const QString &text,
                       QColor bg, QColor border, QColor fg) {
        painter->setPen(QPen(border, 1));
        painter->setBrush(bg);
        painter->drawRoundedRect(r, 4, 4);
        painter->setPen(fg);
        QFont f = painter->font();
        f.setPointSize(8); f.setBold(false);
        painter->setFont(f);
        painter->drawText(r, Qt::AlignCenter, text);
    };

    drawBtn(btn1, "定位", QColor("#F0F4FA"), QColor("#CBD5E0"), QColor("#2C3E50"));
    drawBtn(btn2, "配置", QColor("#F0F4FA"), QColor("#CBD5E0"), QColor("#2C3E50"));
    drawBtn(btn3, "删除", QColor("#FDECEA"), QColor("#EF9A9A"), QColor("#C62828"));

    painter->restore();
}

QSize NodeCardDelegate::sizeHint(const QStyleOptionViewItem &option,
                                  const QModelIndex &) const
{
    return QSize(option.rect.width(), 112);
}

bool NodeCardDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                    const QStyleOptionViewItem &option,
                                    const QModelIndex &index)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        QMouseEvent *me = static_cast<QMouseEvent*>(event);
        QRect rect = option.rect.adjusted(6, 6, -6, -6);
        QRect btn1, btn2, btn3;
        calcBtnRects(rect, btn1, btn2, btn3);

        int dbId = index.data(Qt::UserRole + 6).toInt();
        if (btn1.contains(me->pos())) {
            if (onLocate) onLocate(dbId);
            return true;
        }
        if (btn2.contains(me->pos())) {
            if (onConfigure) onConfigure(dbId);
            return true;
        }
        if (btn3.contains(me->pos())) {
            if (onDelete) onDelete(dbId);
            return true;
        }
    }
    Q_UNUSED(model)
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MatrixDelegate
// ═══════════════════════════════════════════════════════════════════════════════
void MatrixDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    double value = index.data(Qt::DisplayRole).toDouble();
    value = qBound(0.0, value, 1.0);

    QColor minColor(126, 255, 255);
    QColor maxColor(11, 118, 159);
    int r = minColor.red()   + int((maxColor.red()   - minColor.red())   * value);
    int g = minColor.green() + int((maxColor.green() - minColor.green()) * value);
    int b = minColor.blue()  + int((maxColor.blue()  - minColor.blue())  * value);
    painter->fillRect(option.rect.adjusted(1,1,-1,-1), QColor(r,g,b));

    painter->setPen(value > 0.5 ? QColor("#FFFFFF") : QColor("#333333"));
    painter->setFont(option.font);
    if (value > 0.001)
        painter->drawText(option.rect, Qt::AlignCenter,
                          QString::number(value, 'f', 2));
    painter->restore();
}

// ═══════════════════════════════════════════════════════════════════════════════
// WarnCardDelegate  — warnListView 告警卡片（浅色主题，两行布局）
// ═══════════════════════════════════════════════════════════════════════════════
QSize WarnCardDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(0, 62);
}

void WarnCardDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    const QString level   = index.data(Qt::UserRole).toString();
    const QString timeStr = index.data(Qt::UserRole + 1).toString();
    const QString msg     = index.data(Qt::DisplayRole).toString();

    // ── 浅色主题配色 ─────────────────────────────────────────────────────────
    QColor stripColor, iconColor, badgeBg, badgeFg;
    QString iconChar, badgeText;
    if (level == "error") {
        stripColor = QColor("#E53935"); iconColor = QColor("#E53935");
        badgeBg    = QColor("#FFEBEE"); badgeFg   = QColor("#C62828");
        iconChar   = "✕";              badgeText  = "ERROR";
    } else if (level == "warning") {
        stripColor = QColor("#FB8C00"); iconColor = QColor("#EF6C00");
        badgeBg    = QColor("#FFF8E1"); badgeFg   = QColor("#E65100");
        iconChar   = "▲";              badgeText  = "WARN";
    } else {
        stripColor = QColor("#43A047"); iconColor = QColor("#2E7D32");
        badgeBg    = QColor("#E8F5E9"); badgeFg   = QColor("#1B5E20");
        iconChar   = "●";              badgeText  = "INFO";
    }

    const QRect r = option.rect.adjusted(4, 3, -4, -3);

    // ── 卡片背景 ─────────────────────────────────────────────────────────────
    QColor bgColor     = (option.state & QStyle::State_Selected)
                         ? QColor("#E3F2FD") : QColor("#FFFFFF");
    QColor borderColor = (option.state & QStyle::State_Selected)
                         ? QColor("#90CAF9") : QColor("#E0E0E0");
    painter->setBrush(bgColor);
    painter->setPen(QPen(borderColor, 1));
    painter->drawRoundedRect(r, 5, 5);

    // ── 左侧色条 ─────────────────────────────────────────────────────────────
    QRect strip(r.left(), r.top() + 5, 4, r.height() - 10);
    painter->setBrush(stripColor);
    painter->setPen(Qt::NoPen);
    painter->drawRoundedRect(strip, 2, 2);

    // ── 布局：第一行 = 图标 + 徽标 + 时间，第二行 = 消息文字 ────────────────
    const int padL   = 14;
    const int row1H  = 26;   // 第一行高度
    const int row2Y  = r.top() + row1H;
    const int row2H  = r.height() - row1H;

    // 图标（第一行垂直居中）
    QFont iconFont = option.font;
    iconFont.setPointSize(8);
    painter->setFont(iconFont);
    painter->setPen(iconColor);
    QRect iconRect(r.left() + padL, r.top(), 14, row1H);
    painter->drawText(iconRect, Qt::AlignVCenter | Qt::AlignHCenter, iconChar);

    // 徽标
    QFont badgeFont = option.font;
    badgeFont.setPointSize(7);
    badgeFont.setBold(true);
    painter->setFont(badgeFont);
    QFontMetrics badgeFm(badgeFont);
    int badgeW = badgeFm.horizontalAdvance(badgeText) + 10;
    QRect badgeRect(r.left() + padL + 16, r.top() + (row1H - 15) / 2, badgeW, 15);
    painter->setBrush(badgeBg);
    painter->setPen(Qt::NoPen);
    painter->drawRoundedRect(badgeRect, 3, 3);
    painter->setPen(badgeFg);
    painter->drawText(badgeRect, Qt::AlignCenter, badgeText);

    // 时间戳（第一行，右对齐）
    QFont timeFont = option.font;
    timeFont.setPointSize(8);
    painter->setFont(timeFont);
    painter->setPen(QColor("#9E9E9E"));
    int timeW = 60;
    QRect timeRect(r.right() - timeW - 2, r.top(), timeW, row1H);
    painter->drawText(timeRect, Qt::AlignVCenter | Qt::AlignRight, timeStr);

    // 消息文字（第二行，完整显示，只在极长时裁断）
    QFont msgFont = option.font;
    msgFont.setPointSize(9);
    painter->setFont(msgFont);
    painter->setPen(QColor("#424242"));
    QRect msgRect(r.left() + padL, row2Y, r.width() - padL - 6, row2H);
    QFontMetrics msgFm(msgFont);
    QString elidedMsg = msgFm.elidedText(msg, Qt::ElideRight, msgRect.width());
    painter->drawText(msgRect, Qt::AlignVCenter | Qt::AlignLeft, elidedMsg);

    painter->restore();
}

// ═══════════════════════════════════════════════════════════════════════════════
// LinkCardDelegate  — lwLinkList 双行条目
// ═══════════════════════════════════════════════════════════════════════════════
// UserRole+1: 第一行文字 (src→dst  类型)
// UserRole+2: 第二行文字 (带宽  时延)
class LinkCardDelegate : public QStyledItemDelegate
{
public:
    LinkCardDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        bool selected  = option.state & QStyle::State_Selected;
        bool hovered   = option.state & QStyle::State_MouseOver;
        QColor bg = (index.row() % 2 == 0) ? QColor("#FFFFFF") : QColor("#F7FAFC");
        if (selected) bg = QColor("#DBEAFE");
        else if (hovered) bg = QColor("#EBF4FF");
        painter->fillRect(option.rect, bg);

        // 底部细分隔线
        painter->setPen(QPen(QColor("#E2E8F0"), 1));
        painter->drawLine(option.rect.left() + 12, option.rect.bottom(),
                          option.rect.right() - 12, option.rect.bottom());

        int x = option.rect.left() + 14;
        int w = option.rect.width() - 28;
        int y1 = option.rect.top() + 6;
        int y2 = option.rect.top() + 28;

        QString line1 = index.data(Qt::UserRole + 1).toString();
        QString line2 = index.data(Qt::UserRole + 2).toString();
        QString line3 = index.data(Qt::UserRole + 3).toString();
        QString typeStr = index.data(Qt::UserRole + 4).toString();
        QString delayStr = index.data(Qt::UserRole + 5).toString();

        QFont f1 = option.font;
        f1.setPointSize(9); f1.setBold(true);
        painter->setFont(f1);
        painter->setPen(selected ? QColor("#1565C0") : QColor("#2C3E50"));
        int typeWidth = painter->fontMetrics().horizontalAdvance(typeStr);
        QString elidedLine1 = painter->fontMetrics().elidedText(line1, Qt::ElideRight, w - typeWidth - 8);
        painter->drawText(QRect(x, y1, w - typeWidth - 8, 20), Qt::AlignLeft | Qt::AlignVCenter, elidedLine1);
        painter->setPen(selected ? QColor("#1976D2") : QColor("#7F8C8D"));
        painter->drawText(QRect(x, y1, w, 20), Qt::AlignRight | Qt::AlignVCenter, typeStr);

        QFont f2 = option.font;
        f2.setPointSize(9);
        painter->setFont(f2);
        painter->setPen(selected ? QColor("#1976D2") : QColor("#718096"));
        int delayWidth = painter->fontMetrics().horizontalAdvance(delayStr);
        QString elidedLine2 = painter->fontMetrics().elidedText(line2, Qt::ElideRight, w - delayWidth - 8);
        painter->drawText(QRect(x, y2, w - delayWidth - 8, 18), Qt::AlignLeft | Qt::AlignVCenter, elidedLine2);
        painter->drawText(QRect(x, y2, w, 18), Qt::AlignRight | Qt::AlignVCenter, delayStr);

        if (!line3.isEmpty()) {
            int y3 = option.rect.top() + 48;
            QFont f3 = option.font;
            f3.setPointSize(8.5);
            painter->setFont(f3);
            painter->setPen(selected ? QColor("#1E88E5") : QColor("#E53935"));
            QString elidedLine3 = painter->fontMetrics().elidedText(line3, Qt::ElideRight, w);
            painter->drawText(QRect(x, y3, w, 18), Qt::AlignLeft | Qt::AlignVCenter, elidedLine3);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        Q_UNUSED(option)
        QString line3 = index.data(Qt::UserRole + 3).toString();
        if (line3.isEmpty()) {
            return QSize(200, 52);
        } else {
            return QSize(200, 72);
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// MainWindow
// ═══════════════════════════════════════════════════════════════════════════════
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    this->setWindowFlags(Qt::FramelessWindowHint);
    ui->setupUi(this);
    ui->tabLeft->tabBar()->setExpanding(true);
    ui->tabLeft->tabBar()->setDocumentMode(true);

    // 场景树不显示列头，缩小行高
    ui->twSceneTree->setHeaderHidden(true);
    ui->twSceneTree->setStyleSheet("QTreeView::item { height: 20px; padding: 0px; }");
    ui->twSceneTree->setExpandsOnDoubleClick(false);
    connect(ui->twSceneTree, &QTreeWidget::itemDoubleClicked,
            this, &MainWindow::onSceneTreeItemDoubleClicked);
    connect(ui->twSceneTree, &QTreeWidget::itemClicked,
            this, &MainWindow::onSceneTreeItemClicked);

    // 链路列表使用双行代理
    ui->lwLinkList->setItemDelegate(new LinkCardDelegate(ui->lwLinkList));
    ui->lwLinkList->setMouseTracking(true);

    initDatabase();
    initStatusBar();
    initTopoView();
    initCharts();
    initWarnList();
    loadScenesIntoCmb();
    initSimBridge();

    // 链路Tab过滤标签改为"节点一/节点二"
    ui->lblLinkSrcFilter->setText("节点一");
    ui->lblLinkDstFilter->setText("节点二");

    // 节点检索过滤连接
    connect(ui->leSearchNode, &QLineEdit::textChanged, this, &MainWindow::onNodeFilterChanged);
    connect(ui->cmbNodeType,   QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onNodeFilterChanged);
    connect(ui->cmbNodeStatus, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onNodeFilterChanged);
    connect(ui->cmbCommMode,   QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onNodeFilterChanged);

    // 链路检索过滤连接
    connect(ui->leSearchLink, &QLineEdit::textChanged, this, &MainWindow::onLinkFilterChanged);
    connect(ui->cmbLinkSrcFilter,  QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onLinkFilterChanged);
    connect(ui->cmbLinkDstFilter,  QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onLinkFilterChanged);
    connect(ui->cmbLinkTypeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onLinkFilterChanged);

    // 数据视图过滤器连接
    connect(ui->cmbLinkNodeFilter,  QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDataViewLinkFilterChanged);
    connect(ui->cmbLinkTypeFilter_2, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDataViewLinkFilterChanged);
    connect(ui->cmbNodeTypeFilter,  QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDataViewNodeFilterChanged);
    connect(ui->cmbNodeStatusFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDataViewNodeFilterChanged);

    // 场景下拉框：activated 可捕捉到「选中同一项」的操作，强制刷新
    connect(ui->cmbCurrentScene, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
        if (index < 0) return;
        int sceneId = ui->cmbCurrentScene->itemData(index).toInt();
        if (sceneId >= 0) onSceneChanged(sceneId);
    });

    // 节点Tab：窗口大小变化时重置 listview
    connect(ui->tabLeft, &QTabWidget::currentChanged, this, [this](int) {
        if (ui->tabLeft->currentWidget() == ui->tabNodeList)
            QTimer::singleShot(0, this, [this]{ ui->lvNodeList->reset(); });
    });

    // 邻接矩阵基础配置
    ui->tableAdjMatrix->setItemDelegate(new MatrixDelegate(ui->tableAdjMatrix));
    ui->tableAdjMatrix->horizontalHeader()->setVisible(false);
    ui->tableAdjMatrix->verticalHeader()->setVisible(false);
    ui->tableAdjMatrix->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->tableAdjMatrix->verticalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->tableAdjMatrix->setSelectionMode(QAbstractItemView::NoSelection);
    ui->tableAdjMatrix->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tableAdjMatrix->setShowGrid(false);
    ui->tableAdjMatrix->setStyleSheet(
        "QTableView { background:transparent; border:none; outline:none; }"
    );

    // tableLink / tableNode 基础配置（统一样式）
    for (QTableView *tv : {ui->tableLink, ui->tableNode})
        styleDataTable(tv);

    // 禁用并隐藏链路增删改按钮
    ui->btnNavLink->setEnabled(false);
    ui->btnNavLink->setVisible(false);
    ui->btnLinkDelete->setEnabled(false);
    ui->btnLinkDelete->setVisible(false);
    ui->btnLinkDetail->setEnabled(false);
    ui->btnLinkDetail->setVisible(false);
}

MainWindow::~MainWindow()
{
    delete ui;
}

// ─────────────────────────────────────────────────────────────────────────────
// 初始化
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// 数据访问初始化（P6 改造）
//
// 改造前：客户端直连数据库，库的地址账号密码写在客户端里。
// 改造后：默认经协议请求服务端，客户端只认服务端地址。
//
// 为什么保留"连不上就回退本地"这条路径：
//   演示、离线调试、以及服务端还没起来的时候，程序仍然要能打开。
//   直接报错退出对使用者太不友好，而单机模式本来就是它改造前的形态。
//   状态栏会明确显示当前是哪一种模式，不会让人误以为改动已经同步到服务端。
// ─────────────────────────────────────────────────────────────────────────────
// 从环境变量解析服务端地址，未设置时保留成员的默认值（127.0.0.1:9000）。
// 客户端跑在本地、服务端跑在云主机时，只需：
//   set THGH_SERVER_HOST=1.2.3.4        （PowerShell: $env:THGH_SERVER_HOST="1.2.3.4"）
// 不用改代码、不用重新编译。
void MainWindow::resolveServerEndpoint()
{
    const QByteArray host = qgetenv("THGH_SERVER_HOST");
    if (!host.isEmpty()) {
        m_serverHost = QString::fromLocal8Bit(host).trimmed();
    }
    const QByteArray port = qgetenv("THGH_SERVER_PORT");
    if (!port.isEmpty()) {
        bool ok = false;
        const uint v = QString::fromLatin1(port).trimmed().toUInt(&ok);
        // 端口 0 是"由系统分配"的语义，作为连接目标没有意义，一并挡掉
        if (ok && v > 0 && v < 65536) {
            m_serverPort = static_cast<quint16>(v);
        } else {
            qWarning() << "[Data] THGH_SERVER_PORT 非法，忽略:" << port;
        }
    }
    qDebug() << "[Data] 服务端地址:" << m_serverHost << m_serverPort;
}

void MainWindow::initDatabase()
{
    resolveServerEndpoint();

    m_localDb = new DbManager(this);

    // 先试远端
    m_remoteDb = new RemoteDataStore(this);
    m_remoteDb->setServer(m_serverHost, m_serverPort);
    if (m_remoteDb->connectToServer(3000)) {
        m_db = m_remoteDb;
        m_remoteMode = true;
        m_dbOk = true;
        qDebug() << "[Data] 经服务端访问数据:" << m_remoteDb->sourceDescription();
        return;
    }

    qWarning() << "[Data] 服务端不可用，回退单机模式:" << m_remoteDb->lastError();

    // 回退本地直连
    m_db = m_localDb;
    m_remoteMode = false;
    m_dbOk = m_localDb->initDB();
    if (!m_dbOk) {
        QMessageBox::critical(this, "数据源不可用",
                              QString("服务端连不上（%1），本地数据库也无法初始化。\n"
                                      "请检查服务端是否已启动，或程序目录是否可写。")
                                  .arg(m_remoteDb->lastError()));
        return;
    }

    // 种子数据只在单机模式做：远端的库由服务端管理，
    // 客户端擅自清空别人的数据是灾难性的。
    QString flagPath = QCoreApplication::applicationDirPath() + "/.db_seeded_0526";
    if (!QFile::exists(flagPath)) {
        clearAndSeedDatabase();
        QFile flagFile(flagPath);
        if (flagFile.open(QIODevice::WriteOnly)) {
            flagFile.write("seeded");
            flagFile.close();
        }
    }
}

void MainWindow::initTopoView()
{
    m_topoView = new TopoView(ui->pageMapView);
    // 替换 gvTopology
    QLayout *lay = ui->pageMapView->layout();
    if (lay) {
        lay->replaceWidget(ui->gvTopology, m_topoView);
        ui->gvTopology->hide();
    } else {
        m_topoView->setGeometry(ui->gvTopology->geometry());
        ui->gvTopology->hide();
    }
    m_topoView->show();

    connect(m_topoView, &TopoView::nodeRightClicked,
            this, &MainWindow::showNodeContextMenu);
    connect(m_topoView, &TopoView::requestAddNode,
            this, &MainWindow::onTopoRequestAddNode);
    connect(m_topoView, &TopoView::requestAddLink,
            this, &MainWindow::onTopoRequestAddLink);
    connect(m_topoView, &TopoView::requestMoveNode,
            this, &MainWindow::onTopoRequestMoveNode);
    connect(m_topoView, &TopoView::requestEditNode,
            this, &MainWindow::onTopoRequestEditNode);
    connect(m_topoView, &TopoView::requestEditLink,
            this, &MainWindow::onTopoRequestEditLink);
    connect(m_topoView, &TopoView::requestDeleteNode,
            this, &MainWindow::onTopoRequestDeleteNode);
    connect(m_topoView, &TopoView::requestDeleteLink,
            this, &MainWindow::onTopoRequestDeleteLink);
}

void MainWindow::initStatusBar()
{
    // ── 数据库连接状态（可点击修改路径） ────────────────────────────────────
    m_lblStatusDb = new QLabel(this);
    m_lblStatusDb->setToolTip("点击可修改数据库路径");
    m_lblStatusDb->setCursor(Qt::PointingHandCursor);
    m_lblStatusDb->installEventFilter(this);
    if (m_dbOk) {
        m_lblStatusDb->setText("DB: 已连接");
        m_lblStatusDb->setStyleSheet("color:#27AE60; padding:0 10px;");
    } else {
        m_lblStatusDb->setText("DB: 连接失败");
        m_lblStatusDb->setStyleSheet("color:#E74C3C; padding:0 10px;");
    }

    // ── Python 通信状态（可点击修改地址） ───────────────────────────────────
    m_lblStatusPy = new QLabel("Python: 未连接", this);
    m_lblStatusPy->setStyleSheet("color:#E74C3C; padding:0 10px;");
    m_lblStatusPy->setToolTip("点击可修改连接地址");
    m_lblStatusPy->setCursor(Qt::PointingHandCursor);
    m_lblStatusPy->installEventFilter(this);

    // ── 日志/报错滚动展示 ────────────────────────────────────────────────────
    m_lblStatusLog = new QLabel(this);
    m_lblStatusLog->setStyleSheet("color:#888888; padding:0 10px;");
    m_lblStatusLog->setMinimumWidth(260);
    m_lblStatusLog->setMaximumWidth(500);

    // ── 时间（右侧固定） ─────────────────────────────────────────────────────
    QLabel *timeLabel = new QLabel(this);
    timeLabel->setStyleSheet("padding:0 10px;");

    auto *sep1 = new QLabel("|", this); sep1->setStyleSheet("color:#BDBDBD;");
    auto *sep2 = new QLabel("|", this); sep2->setStyleSheet("color:#BDBDBD;");
    auto *sep3 = new QLabel("|", this); sep3->setStyleSheet("color:#BDBDBD;");

    statusBar()->addWidget(m_lblStatusDb);
    statusBar()->addWidget(sep1);
    statusBar()->addWidget(m_lblStatusPy);
    statusBar()->addWidget(sep2);
    statusBar()->addWidget(m_lblStatusLog);
    statusBar()->addWidget(sep3);
    statusBar()->addPermanentWidget(timeLabel);

    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, [timeLabel]() {
        timeLabel->setText(QTime::currentTime().toString("hh:mm:ss"));
    });
    timer->start(1000);
}

void MainWindow::postStatusLog(const QString &msg, bool isError)
{
    if (!m_lblStatusLog) return;
    m_lblStatusLog->setText(msg);
    m_lblStatusLog->setStyleSheet(
        isError ? "color:#E74C3C; padding:0 10px;"
                : "color:#888888; padding:0 10px;");
    // 5 秒后自动淡出（恢复灰色空白）
    QTimer::singleShot(5000, m_lblStatusLog, [this]() {
        if (m_lblStatusLog) {
            m_lblStatusLog->setText("");
            m_lblStatusLog->setStyleSheet("color:#888888; padding:0 10px;");
        }
    });
}

void MainWindow::loadScenesIntoCmb()
{
    int savedId = m_currentSceneId;

    // 全程阻塞信号，避免 setCurrentIndex 触发 currentIndexChanged 导致双重刷新
    QSignalBlocker blocker(ui->cmbCurrentScene);
    ui->cmbCurrentScene->clear();
    for (const SceneInfo &s : m_db->listScenes())
        ui->cmbCurrentScene->addItem(s.name, s.id);

    if (ui->cmbCurrentScene->count() > 0) {
        int restoreIdx = 0;
        if (savedId >= 0) {
            for (int i = 0; i < ui->cmbCurrentScene->count(); ++i) {
                if (ui->cmbCurrentScene->itemData(i).toInt() == savedId) {
                    restoreIdx = i; break;
                }
            }
        }
        ui->cmbCurrentScene->setCurrentIndex(restoreIdx);
        // 直接调用刷新，不依赖信号（解决启动时画面空白、新建场景不自动切换的问题）
        int sceneId = ui->cmbCurrentScene->itemData(restoreIdx).toInt();
        if (sceneId >= 0) onSceneChanged(sceneId);
    } else {
        m_currentSceneId = -1;
        m_currentNodes.clear();
        m_currentLinks.clear();
        ui->twSceneTree->clear();
        m_topoView->clearAll();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 场景切换 → 全刷新
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onSceneChanged(int sceneId)
{
    m_currentSceneId = sceneId;

    m_currentNodes = m_db->getNodesByScene(sceneId);
    m_currentLinks = m_db->getLinksByScene(sceneId);

    refreshSceneTree(m_currentNodes);
    refreshNodeListView(m_currentNodes, m_currentLinks);
    refreshNodeStats(m_currentNodes);
    refreshLinkListWidget(m_currentLinks, m_currentNodes);
    refreshLinkTable(m_currentLinks, m_currentNodes);
    refreshNodeTable(m_currentNodes);
    refreshAdjMatrix(m_currentNodes, m_currentLinks);
    refreshDeviceStats(m_currentNodes);
    refreshTopoView(m_currentNodes, m_currentLinks);
}

// ─────────────────────────────────────────────────────────────────────────────
// 场景树刷新
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshSceneTree(const QList<NodeInfo> &nodes)
{
    ui->twSceneTree->clear();

    // 查找当前场景信息（含类型描述）
    QString sceneName = ui->cmbCurrentScene->currentText();
    QString sceneType;
    for (const SceneInfo &s : m_db->listScenes()) {
        if (s.id == m_currentSceneId) { sceneType = s.sceneType; break; }
    }

    QTreeWidgetItem *root = new QTreeWidgetItem(ui->twSceneTree);
    if (sceneType.isEmpty())
        root->setText(0, sceneName);
    else
        root->setText(0, QString("%1    %2").arg(sceneName).arg(sceneType));
    QFont fRoot = root->font(0);
    fRoot.setBold(true);
    root->setFont(0, fRoot);
    root->setForeground(0, QBrush(QColor("#1565C0")));
    root->setExpanded(true);

    static const QMap<QString, QString> devNameMap = {
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
        {"switch",      "交换机"}
    };

    for (const NodeInfo &n : nodes) {
        QTreeWidgetItem *nodeItem = new QTreeWidgetItem(root);
        QString statusIcon = (n.status == "在线") ? "🟢" :
                       (n.status == "告警") ? "🟡" : "🔴";
        nodeItem->setText(0, QString("%1 [%2] %3")
                      .arg(statusIcon).arg(n.nodeId)
                      .arg(n.name.isEmpty() ? n.nodeType : n.name));
        
        QVariantMap nodeData;
        nodeData["type"] = "node";
        nodeData["dbId"] = n.id;
        nodeItem->setData(0, Qt::UserRole, nodeData);
        nodeItem->setExpanded(true);

        if (n.status == "离线")
            nodeItem->setForeground(0, QBrush(QColor("#999999")));
        else if (n.status == "告警")
            nodeItem->setForeground(0, QBrush(QColor("#FF8C00")));

        // 添加每个通信设备实例作为子节点（优先显示用户自定义的实例名称）
        for (const QString &method : n.commMethods) {
            QTreeWidgetItem *devItem = new QTreeWidgetItem(nodeItem);
            const DeviceParams dp = n.deviceParams.value(method);
            QString devCN = !dp.instanceName.isEmpty()
                            ? dp.instanceName
                            : devNameMap.value(deviceTypeOfKey(method), deviceTypeOfKey(method));
            devItem->setText(0, QString("📡 %1").arg(devCN));

            QVariantMap devData;
            devData["type"] = "device";
            devData["nodeDbId"] = n.id;
            devData["deviceKey"] = method;
            devItem->setData(0, Qt::UserRole, devData);
        }
    }

    if (nodes.isEmpty()) {
        QTreeWidgetItem *empty = new QTreeWidgetItem(root);
        empty->setText(0, "暂无节点");
        empty->setForeground(0, QBrush(QColor("#999999")));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 节点列表卡片刷新
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshNodeListView(const QList<NodeInfo> &nodes,
                                      const QList<LinkInfo> &links)
{
    // 统计每个节点的链路数
    QMap<int, int> linkCounts;
    for (const LinkInfo &l : links) {
        linkCounts[l.src]++;
        linkCounts[l.dst]++;
    }

    QStandardItemModel *model = new QStandardItemModel(this);
    for (const NodeInfo &n : nodes) {
        QStandardItem *item = new QStandardItem();
        item->setData(n.nodeId,    Qt::UserRole);
        item->setData(n.name,      Qt::UserRole + 1);
        item->setData(n.nodeType,  Qt::UserRole + 2);
        // 位置字段存储格式化经纬（高度单独存 UserRole+7）
        item->setData(QString("%1°E, %2°N")
                      .arg(n.longitude, 0, 'f', 4)
                      .arg(n.latitude,  0, 'f', 4),
                      Qt::UserRole + 3);
        item->setData(linkCounts.value(n.nodeId, 0), Qt::UserRole + 4);
        item->setData(n.status,        Qt::UserRole + 5);
        item->setData(n.id,            Qt::UserRole + 6);
        item->setData(n.altitude,      Qt::UserRole + 7);
        item->setData(n.interferenceDb, Qt::UserRole + 8);
        model->appendRow(item);
    }

    // 填充节点类型过滤下拉框（保持当前选项）
    {
        QSignalBlocker b1(ui->cmbNodeType), b2(ui->cmbNodeStatus), b3(ui->cmbCommMode);
        QString curType   = ui->cmbNodeType->currentText();
        QString curStatus = ui->cmbNodeStatus->currentText();
        QString curComm   = ui->cmbCommMode->currentText();

        ui->cmbNodeType->clear();
        ui->cmbNodeType->addItem("全部");
        QSet<QString> types;
        QSet<QString> commsCn;
        
        // 通信能力键 → 显示名
        auto keyToCn = [](const QString &key) -> QString {
            if (key == "fiber")     return "光缆";
            if (key == "fieldwire" || key == "fieldWire") return "野战电缆";
            if (key == "adhoc")     return "自组网电台";
            if (key == "narrowband") return "窄带战术电台";
            if (key == "microwave") return "微波接力";
            if (key == "scatter")   return "超视距微波";
            if (key == "cellular")  return "移动公网";
            if (key == "shortwave") return "短波电台";
            if (key == "satellite") return "卫星通信";
            if (key == "switch")    return "交换机";
            return key;
        };

        for (const NodeInfo &n : m_currentNodes) {
            types.insert(n.nodeType);
            for (const QString &m : n.commMethods) commsCn.insert(keyToCn(deviceTypeOfKey(m)));
        }
        for (const QString &t : types)  ui->cmbNodeType->addItem(t);

        ui->cmbNodeStatus->clear();
        ui->cmbNodeStatus->addItems({"全部", "在线", "告警", "离线"});

        ui->cmbCommMode->clear();
        ui->cmbCommMode->addItem("全部");
        for (const QString &c : commsCn) ui->cmbCommMode->addItem(c);

        // 恢复之前选项
        int ti = ui->cmbNodeType->findText(curType);
        if (ti >= 0) ui->cmbNodeType->setCurrentIndex(ti);
        int si = ui->cmbNodeStatus->findText(curStatus);
        if (si >= 0) ui->cmbNodeStatus->setCurrentIndex(si);
        int ci = ui->cmbCommMode->findText(curComm);
        if (ci >= 0) ui->cmbCommMode->setCurrentIndex(ci);
    }

    // 创建带回调的 delegate
    NodeCardDelegate *delegate = new NodeCardDelegate(this);

    delegate->onLocate = [this](int dbId) {
        // 定位：让拓扑视图居中显示该节点
        auto nodes = m_db->getNodesByScene(m_currentSceneId);
        for (const NodeInfo &n : nodes) {
            if (n.id == dbId) {
                m_topoView->fitAll();
                break;
            }
        }
    };

    delegate->onConfigure = [this](int dbId) {
        auto nodes = m_db->getNodesByScene(m_currentSceneId);
        for (const NodeInfo &n : nodes) {
            if (n.id == dbId) {
                int editedNodeId = n.nodeId;
                DialogNode *dlg = new DialogNode(m_db, m_currentSceneId, n, this);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                connect(dlg, &DialogNode::nodeSaved, this,
                        [this, editedNodeId]{
                    cleanupInvalidLinks(m_currentSceneId, editedNodeId);
                    onSceneChanged(m_currentSceneId);
                });
                dlg->exec();
                break;
            }
        }
    };

    delegate->onDelete = [this](int dbId) {
        int ret = QMessageBox::question(this, "确认删除",
            "删除节点将同时删除该节点相关的所有链路，确认？");
        if (ret == QMessageBox::Yes) {
            m_db->deleteNode(dbId);
            onSceneChanged(m_currentSceneId);
        }
    };

    // 替换旧 model 和 delegate
    QAbstractItemModel *oldModel = ui->lvNodeList->model();
    ui->lvNodeList->setModel(model);
    ui->lvNodeList->setItemDelegate(delegate);
    if (oldModel && oldModel->parent() == this) delete oldModel;

    ui->lvNodeList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->lvNodeList->setSelectionMode(QAbstractItemView::NoSelection);
    ui->lvNodeList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->lvNodeList->setSpacing(8);
    ui->lvNodeList->setMouseTracking(true);
    ui->lvNodeList->setResizeMode(QListView::Adjust);
}

// ─────────────────────────────────────────────────────────────────────────────
// 节点统计标签刷新
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshNodeStats(const QList<NodeInfo> &nodes)
{
    int total = nodes.size();
    int online  = 0, offline = 0;
    for (const NodeInfo &n : nodes) {
        if (n.status == "在线" || n.status == "告警") online++;
        else offline++;
    }
    ui->lblTotalNodeCount->setText(QString::number(total));
    ui->lblOnlineCount->setText(QString::number(online));
    ui->lblOfflineCount->setText(QString::number(offline));
}

// ─────────────────────────────────────────────────────────────────────────────
// 链路列表（左侧 Tab 链路页）
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshLinkListWidget(const QList<LinkInfo> &links,
                                       const QList<NodeInfo> &nodes)
{
    ui->lwLinkList->clear();

    // 更新链路过滤下拉框（源节点、目标节点、链路类型）
    {
        QSignalBlocker b1(ui->cmbLinkSrcFilter), b2(ui->cmbLinkDstFilter),
                       b3(ui->cmbLinkTypeFilter);
        ui->cmbLinkSrcFilter->clear();
        ui->cmbLinkDstFilter->clear();
        ui->cmbLinkSrcFilter->addItem("全部");
        ui->cmbLinkDstFilter->addItem("全部");
        for (const NodeInfo &n : nodes) {
            QString s = QString("[%1] %2").arg(n.nodeId)
                        .arg(n.name.isEmpty() ? n.nodeType : n.name);
            ui->cmbLinkSrcFilter->addItem(s, n.nodeId);
            ui->cmbLinkDstFilter->addItem(s, n.nodeId);
        }

        // 链路类型过滤：收集当前场景存在的类型
        QString curTypeFilter = ui->cmbLinkTypeFilter->currentText();
        ui->cmbLinkTypeFilter->clear();
        ui->cmbLinkTypeFilter->addItem("全部");
        QSet<QString> typeSet;
        for (const LinkInfo &l : links) {
            bool iw = (l.linkType == "wireless" || l.linkType == "无线");
            typeSet.insert(iw ? "无线" : "有线");
            if (!l.wirelessType.isEmpty())
                typeSet.insert(translateLinkType(l.wirelessType));  // Translated to Chinese
        }
        for (const QString &t : typeSet) ui->cmbLinkTypeFilter->addItem(t);
        int ti = ui->cmbLinkTypeFilter->findText(curTypeFilter);
        if (ti >= 0) ui->cmbLinkTypeFilter->setCurrentIndex(ti);
    }

    for (const LinkInfo &l : links) {
        QString wtStr = translateLinkType(l.wirelessType);
        if (wtStr.isEmpty()) {
            wtStr = (l.linkType == "wireless" || l.linkType == "无线") ? "无线" : "有线";
        }
        QString srcName = nodeIdToName(l.src, nodes);
        QString dstName = nodeIdToName(l.dst, nodes);

        QStringList flowStrings;
        for (const LinkFlow &f : l.flows) {
            double rateMbps = f.bandwidthBps / 1e6;
            flowStrings << QString("流%1: %2M").arg(f.flowId).arg(rateMbps, 0, 'f', 1);
        }

        QString line1 = QString("链路%1  %2↔%3").arg(l.id).arg(srcName).arg(dstName);
        QString line2 = QString("带宽: %1 Mbps").arg(l.bandwidthBps / 1e6, 0, 'f', 1);
        QString delayStr = QString("时延: %1 ms").arg(l.propDelayS * 1000, 0, 'f', 3);
        QString line3 = flowStrings.isEmpty() ? "" : QString("流: %1").arg(flowStrings.join(" / "));

        QListWidgetItem *item = new QListWidgetItem();
        item->setData(Qt::UserRole,     l.id);
        item->setData(Qt::UserRole + 1, line1);
        item->setData(Qt::UserRole + 2, line2);
        if (!line3.isEmpty()) {
            item->setData(Qt::UserRole + 3, line3);
        }
        item->setData(Qt::UserRole + 4, wtStr);
        item->setData(Qt::UserRole + 5, delayStr);

        // 鼠标悬停显示完整、未截断的链路详情
        QString toolTipText = QString(
            "链路ID: %1\n"
            "节点: %2 ↔ %3\n"
            "类型: %4\n"
            "带宽: %5 Mbps\n"
            "时延: %6 ms"
        ).arg(l.id).arg(srcName).arg(dstName).arg(wtStr)
         .arg(l.bandwidthBps / 1e6, 0, 'f', 1)
         .arg(l.propDelayS * 1000, 0, 'f', 3);
        if (!line3.isEmpty()) {
            toolTipText += "\n" + line3;
        }
        item->setToolTip(toolTipText);

        ui->lwLinkList->addItem(item);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 链路表格（数据视图）：填充过滤下拉框 + 建表
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshLinkTable(const QList<LinkInfo> &links,
                                   const QList<NodeInfo> &nodes)
{
    // 填充 cmbLinkNodeFilter
    {
        QSignalBlocker b1(ui->cmbLinkNodeFilter), b2(ui->cmbLinkTypeFilter_2);
        ui->cmbLinkNodeFilter->clear();
        ui->cmbLinkNodeFilter->addItem("全部节点");
        for (const NodeInfo &n : nodes)
            ui->cmbLinkNodeFilter->addItem(
                QString("[%1] %2").arg(n.nodeId).arg(n.name.isEmpty() ? n.nodeType : n.name),
                n.nodeId);

        // 收集当前链路类型（wirelessType 已存新中文名，直接作为独立选项）
        ui->cmbLinkTypeFilter_2->clear();
        ui->cmbLinkTypeFilter_2->addItem("全部类型");
        QStringList types;
        for (const LinkInfo &l : links) {
            bool iw = (l.linkType == "wireless" || l.linkType == "无线");
            QString t = iw ? "无线" : "有线";
            if (!types.contains(t)) types << t;
            if (!l.wirelessType.isEmpty()) {
                QString wt = translateLinkType(l.wirelessType);
                if (!types.contains(wt)) types << wt;
            }
        }
        for (const QString &t : types) ui->cmbLinkTypeFilter_2->addItem(t);
    }
    buildAndSetLinkTableModel(links, nodes);
}

// ─────────────────────────────────────────────────────────────────────────────
// 节点表格（数据视图）：填充过滤下拉框 + 建表
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshNodeTable(const QList<NodeInfo> &nodes)
{
    {
        QSignalBlocker b1(ui->cmbNodeTypeFilter), b2(ui->cmbNodeStatusFilter);
        QString prevType   = ui->cmbNodeTypeFilter->currentText();
        QString prevStatus = ui->cmbNodeStatusFilter->currentText();

        ui->cmbNodeTypeFilter->clear();
        ui->cmbNodeTypeFilter->addItem("全部类型");
        QStringList types;
        for (const NodeInfo &n : nodes)
            if (!n.nodeType.isEmpty() && !types.contains(n.nodeType))
                types << n.nodeType;
        for (const QString &t : types) ui->cmbNodeTypeFilter->addItem(t);

        ui->cmbNodeStatusFilter->clear();
        // 用 QStringList 而不是 {const char*...}：后者会为每个元素临时构造一个
        // QString，再把 const QString& 绑到这个临时量上（-Wrange-loop-construct）。
        for (const QString &s : QStringList{"全部状态", "在线", "告警", "离线"})
            ui->cmbNodeStatusFilter->addItem(s);

        int ti = ui->cmbNodeTypeFilter->findText(prevType);
        if (ti >= 0) ui->cmbNodeTypeFilter->setCurrentIndex(ti);
        int si = ui->cmbNodeStatusFilter->findText(prevStatus);
        if (si >= 0) ui->cmbNodeStatusFilter->setCurrentIndex(si);
    }
    buildAndSetNodeTableModel(nodes);
}

// ─────────────────────────────────────────────────────────────────────────────
// 内部：建链路表 model
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::buildAndSetLinkTableModel(const QList<LinkInfo> &links,
                                           const QList<NodeInfo> &nodes)
{
    auto *model = new QStandardItemModel(this);
    model->setHorizontalHeaderLabels({"链路ID", "节点一", "节点二", "类型", "子类型", "带宽(Mbps)", "时延(ms)", "流"});

    auto mk = [](const QString &text) {
        QStandardItem *it = new QStandardItem(text);
        it->setTextAlignment(Qt::AlignCenter);
        return it;
    };
    for (const LinkInfo &l : links) {
        bool isWireless = (l.linkType == "wireless" || l.linkType == "无线");
        QStringList flowStrings;
        for (const LinkFlow &f : l.flows) {
            double rateMbps = f.bandwidthBps / 1e6;
            flowStrings << QString("流%1:%2M").arg(f.flowId).arg(rateMbps, 0, 'f', 1);
        }
        QString flowStr = flowStrings.isEmpty() ? "--" : flowStrings.join("/");

        model->appendRow({
            mk(QString::number(l.id)),
            mk(nodeIdToName(l.src, nodes)),
            mk(nodeIdToName(l.dst, nodes)),
            mk(isWireless ? "无线" : "有线"),
            mk(translateLinkType(l.wirelessType)),
            mk(QString::number(l.bandwidthBps / 1e6, 'f', 1)),
            mk(QString::number(l.propDelayS * 1000, 'f', 3)),
            mk(flowStr)
        });
    }
    QAbstractItemModel *old = ui->tableLink->model();
    ui->tableLink->setModel(model);
    if (old && old->parent() == this) delete old;
    ui->tableLink->resizeColumnsToContents();
}

// ─────────────────────────────────────────────────────────────────────────────
// 内部：建节点表 model
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::buildAndSetNodeTableModel(const QList<NodeInfo> &nodes)
{
    auto *model = new QStandardItemModel(this);
    model->setHorizontalHeaderLabels({"节点ID", "名称", "类型", "状态",
                                      "经度", "纬度", "高度(m)", "干扰(dBm)", "通信能力"});
    auto mk = [](const QString &text) {
        QStandardItem *it = new QStandardItem(text);
        it->setTextAlignment(Qt::AlignCenter);
        return it;
    };
    // 通信能力键 → 显示名（与节点能力勾选项保持一致）
    auto capDisplay = [](const QString &key) -> QString {
        if (key == "fiber")     return "光缆";
        if (key == "fieldwire") return "野战电缆";
        if (key == "adhoc")     return "自组网电台";
        if (key == "narrowband") return "窄带战术电台";
        if (key == "microwave") return "微波接力";
        if (key == "scatter")   return "超视距微波";
        if (key == "cellular")  return "移动公网";
        if (key == "shortwave") return "短波电台";
        if (key == "satellite") return "卫星通信";
        return key;
    };
    for (const NodeInfo &n : nodes) {
        QStringList capNames;
        for (const QString &c : deviceTypesOf(n.commMethods)) capNames << capDisplay(c);
        model->appendRow({
            mk(QString::number(n.nodeId)),
            mk(n.name),
            mk(n.nodeType),
            mk(n.status),
            mk(QString::number(n.longitude, 'f', 6)),
            mk(QString::number(n.latitude,  'f', 6)),
            mk(QString::number(n.altitude,  'f', 1)),
            mk(QString::number(n.interferenceDb, 'f', 1)),
            mk(capNames.join("，"))
        });
    }
    QAbstractItemModel *old = ui->tableNode->model();
    ui->tableNode->setModel(model);
    if (old && old->parent() == this) delete old;
    ui->tableNode->resizeColumnsToContents();
}

// ─────────────────────────────────────────────────────────────────────────────
// 邻接矩阵刷新
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshAdjMatrix(const QList<NodeInfo> &nodes,
                                   const QList<LinkInfo> &links)
{
    int n = nodes.size();
    if (n == 0) {
        ui->tableAdjMatrix->setModel(new QStandardItemModel(0, 0, this));
        return;
    }

    // 最多显示 12×12，避免太大
    int display = qMin(n, 12);
    m_adjNodeIds.clear();
    for (int i = 0; i < display; ++i)
        m_adjNodeIds << nodes[i].nodeId;

    auto *model = new QStandardItemModel(display, display, this);

    // 初始化 0
    for (int r = 0; r < display; ++r)
        for (int c = 0; c < display; ++c) {
            auto *item = new QStandardItem();
            item->setData(0.0, Qt::DisplayRole);
            model->setItem(r, c, item);
        }

    // 有链路的格子设为带宽利用率代理（暂用1.0表示有链路）
    for (const LinkInfo &l : links) {
        int ri = m_adjNodeIds.indexOf(l.src);
        int ci = m_adjNodeIds.indexOf(l.dst);
        if (ri >= 0 && ci >= 0) {
            double val = 0.6;  // 有链路时默认显示0.6（蓝色）
            model->item(ri, ci)->setData(val, Qt::DisplayRole);
            model->item(ci, ri)->setData(val, Qt::DisplayRole);
        }
    }

    QAbstractItemModel *old = ui->tableAdjMatrix->model();
    ui->tableAdjMatrix->setModel(model);
    if (old && old->parent() == this) delete old;
}

// ─────────────────────────────────────────────────────────────────────────────
// 右侧设备统计
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshDeviceStats(const QList<NodeInfo> &nodes)
{
    int total = nodes.size(), online = 0, offline = 0, warn = 0;
    for (const NodeInfo &n : nodes) {
        if      (n.status == "在线") online++;
        else if (n.status == "告警") warn++;
        else                         offline++;
    }
    ui->lblTotalDevCount->setText(QString::number(total));
    ui->lblOnlineDevCount->setText(QString::number(online));
    ui->lblOfflineDevCount->setText(QString::number(offline));
    ui->lblWarnDevCount->setText(QString::number(warn));
}

// ─────────────────────────────────────────────────────────────────────────────
// 拓扑视图刷新
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshTopoView(const QList<NodeInfo> &nodes,
                                  const QList<LinkInfo> &links)
{
    m_topoView->clearAll();

    if (!nodes.isEmpty()) {
        for (const NodeInfo &n : nodes)
            m_topoView->addNode(n.nodeId,
                                n.name.isEmpty() ? QString("N%1").arg(n.nodeId) : n.name,
                                n.nodeType, n.longitude, n.latitude, n.altitude,
                                deviceTypesOf(n.commMethods), n.status);
        
        QMap<QPair<int,int>, QList<LinkInfo>> pairLinks;
        for (const LinkInfo &l : links) {
            int u = qMin(l.src, l.dst);
            int v = qMax(l.src, l.dst);
            pairLinks[qMakePair(u, v)].append(l);
        }

        for (auto it = pairLinks.begin(); it != pairLinks.end(); ++it) {
            int u = it.key().first;
            int v = it.key().second;
            const QList<LinkInfo> &list = it.value();
            const LinkInfo &firstLink = list.first();
            m_topoView->addLink(u, v, firstLink.linkType, firstLink.wirelessType);

            QStringList tooltipLines;
            for (const LinkInfo &l : list) {
                QString cnLinkType = (l.linkType == "wireless" || l.linkType == "无线") ? "无线" : "有线";
                QString cnWirelessType = translateLinkType(l.wirelessType);
                QString subT = cnWirelessType.isEmpty() ? "" : "·" + cnWirelessType;
                QStringList flowStrings;
                for (const LinkFlow &f : l.flows) {
                    double rateMbps = f.bandwidthBps / 1e6;
                    flowStrings << QString("流%1: %2M").arg(f.flowId).arg(rateMbps, 0, 'f', 1);
                }
                QString flowPart = flowStrings.isEmpty() ? "" : QString(" | 流: %1").arg(flowStrings.join("/"));
                tooltipLines << QString("ID: %1 | 类型: %2%3%4")
                               .arg(l.id).arg(cnLinkType).arg(subT).arg(flowPart);
            }
            m_topoView->updateLinkCountInfo(u, v, list.size(), tooltipLines);
        }
    }

    // 延迟 fitAll，确保视口已完成布局（首次加载时视口尺寸为 0）
    QTimer::singleShot(0, m_topoView, &TopoView::fitAll);
}

// ─────────────────────────────────────────────────────────────────────────────
// 工具函数
// ─────────────────────────────────────────────────────────────────────────────
QString MainWindow::nodeIdToName(int nodeId, const QList<NodeInfo> &nodes) const
{
    for (const NodeInfo &n : nodes) {
        if (n.nodeId == nodeId)
            return n.name.isEmpty() ? QString("节点%1").arg(nodeId) : n.name;
    }
    return QString::number(nodeId);
}

QToolButton* MainWindow::createToolButton(const QString &icon, const QString &tooltip)
{
    QToolButton *btn = new QToolButton();
    btn->setText(icon);
    btn->setToolTip(tooltip);
    btn->setFixedSize(40, 40);
    btn->setStyleSheet(
        "QToolButton { background:#F0F4FA; border:1px solid #CBD5E0; border-radius:8px; font-size:18px; }"
        "QToolButton:hover { background:#E3F0FC; border-color:#1565C0; }"
        "QToolButton:pressed { background:#BBDEFB; }"
    );
    return btn;
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口控制
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::on_btnMinimize_clicked() { this->showMinimized(); }

void MainWindow::on_btnMaximize_clicked()
{
    isMaximized() ? showNormal() : showMaximized();
}

void MainWindow::on_btnClose_clicked() { this->close(); }

// ─────────────────────────────────────────────────────────────────────────────
// 顶部导航按钮
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::on_btnNavScene_clicked()
{
    DialogScene *dlg = new DialogScene(m_db, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &DialogScene::sceneSaved, this, [this](int sceneId) {
        m_currentSceneId = sceneId;
        loadScenesIntoCmb();
    });
    dlg->exec();
}

void MainWindow::on_btnNavNode_clicked()
{
    if (m_currentSceneId < 0) {
        QMessageBox::information(this, "提示", "请先创建或选择一个场景");
        return;
    }
    DialogNode *dlg = new DialogNode(m_db, m_currentSceneId, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &DialogNode::nodeSaved,
            this, [this]{ onSceneChanged(m_currentSceneId); });
    dlg->exec();
}

void MainWindow::on_btnNavLink_clicked()
{
    // 禁用手动创建链路
}

void MainWindow::on_btnNavPlanning_clicked()
{
    if (m_currentSceneId < 0) {
        QMessageBox::warning(this, "提示", "请先创建或选择一个场景");
        return;
    }
    DialogPlanning *dlg = new DialogPlanning(m_db, m_currentSceneId, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &DialogPlanning::planSaved,
            this, [this](int sceneId, QList<FlowInfo> flows, QList<QPair<int,int>> exclusions) {
        m_planSceneId    = sceneId;
        m_planFlows      = flows;
        m_planExclusions = exclusions;
        ui->lblPlanPathValue->setText(QString("已配置 %1 条流").arg(flows.size()));
    });
    dlg->exec();
}

// ─────────────────────────────────────────────────────────────────────────────
// 场景 Tab 操作
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::on_cmbCurrentScene_currentIndexChanged(int index)
{
    // 仅响应用户实际切换到不同场景（activated 信号负责处理「重选同一项」）
    if (index < 0) return;
    int sceneId = ui->cmbCurrentScene->itemData(index).toInt();
    if (sceneId == m_currentSceneId) return;  // activated 已处理，这里避免重复刷新
    onSceneChanged(sceneId);
}

void MainWindow::on_btnSceneEdit_clicked()
{
    if (m_currentSceneId < 0) return;
    for (const SceneInfo &s : m_db->listScenes()) {
        if (s.id == m_currentSceneId) {
            DialogScene *dlg = new DialogScene(m_db, s, this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            connect(dlg, &DialogScene::sceneSaved, this, [this](int) {
                loadScenesIntoCmb();
            });
            dlg->exec();
            return;
        }
    }
}

void MainWindow::on_btnSceneDelete_clicked()
{
    if (m_currentSceneId < 0) return;
    int ret = QMessageBox::question(this, "确认删除",
        "删除场景将同时删除其所有节点和链路，此操作不可撤销，确认？",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        m_db->deleteScene(m_currentSceneId);
        m_currentSceneId = -1;
        loadScenesIntoCmb();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 链路合法性校验（两端非离线 + 通信能力匹配）
// ─────────────────────────────────────────────────────────────────────────────
bool MainWindow::isLinkValid(const LinkInfo &l, const QList<NodeInfo> &nodes) const
{
    const NodeInfo *n1 = nullptr, *n2 = nullptr;
    for (const NodeInfo &n : nodes) {
        if (n.nodeId == l.src) n1 = &n;
        if (n.nodeId == l.dst) n2 = &n;
    }
    if (!n1 || !n2)                          return false;  // 节点不存在
    if (n1->status == "离线" || n2->status == "离线") return false;

    bool isWireless = (l.linkType == "wireless" || l.linkType == "无线");
    const QString &sub = l.wirelessType;
    QString requiredCap;
    if (!isWireless) {
        if      (sub == "光缆" || sub == "光纤")     requiredCap = "fiber";
        else if (sub == "野战电缆" || sub == "被服线") requiredCap = "fieldwire";
    } else {
        if      (sub == "微波接力"   || sub == "微波")  requiredCap = "microwave";
        else if (sub == "超视距微波" || sub == "散射")  requiredCap = "scatter";
        else if (sub == "自组网电台" || sub == "自组网") requiredCap = "adhoc";
        else if (sub == "卫星通信"   || sub == "卫星")  requiredCap = "satellite";
        else if (sub == "移动公网"   || sub == "4G/5G") requiredCap = "cellular";
        else if (sub == "短波电台"   || sub == "短波")  requiredCap = "shortwave";
        else if (sub == "窄带战术电台")                  requiredCap = "narrowband";
    }
    if (requiredCap.isEmpty()) return true;  // 未知子类型，只校验状态
    // 按设备类型前缀匹配（commMethods 现存实例 key，如 "fiber_00"）；fieldwire 兼容大小写
    return nodeHasCapability(n1->commMethods, requiredCap)
        && nodeHasCapability(n2->commMethods, requiredCap);
}

// ─────────────────────────────────────────────────────────────────────────────
// 删除场景中违反约束的链路（editedNodeId=-1 检查全部，否则只检查涉及该节点的链路）
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::cleanupInvalidLinks(int sceneId, int editedNodeId)
{
    auto nodes = m_db->getNodesByScene(sceneId);
    auto links = m_db->getLinksByScene(sceneId);
    for (const LinkInfo &l : links) {
        if (editedNodeId >= 0 && l.src != editedNodeId && l.dst != editedNodeId)
            continue;
        if (!isLinkValid(l, nodes))
            m_db->deleteLink(l.id);
    }
}

void MainWindow::on_btnSceneImport_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(this, "导入场景",
                                                    QString(), "JSON文件(*.json)");
    if (filePath.isEmpty()) return;

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "错误", "无法打开文件");
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) { QMessageBox::warning(this, "错误", "JSON格式错误"); return; }

    QJsonObject obj = doc.object();

    // 创建场景
    SceneInfo s;
    s.name        = obj["name"].toString("导入场景");
    s.description = obj["description"].toString();
    s.sceneType   = obj["scene_type"].toString();
    int sceneId = m_db->createScene(s);
    if (sceneId < 0) { QMessageBox::critical(this, "错误", "创建场景失败"); return; }

    // 导入节点
    QList<NodeInfo> pendingNodes;
    for (const QJsonValue &v : obj["nodes"].toArray()) {
        QJsonObject no = v.toObject();
        NodeInfo n;
        n.sceneId     = sceneId;
        n.nodeId      = no["node_id"].toInt();
        n.name        = no["name"].toString();
        n.nodeType    = no["node_type"].toString();
        n.status      = no["status"].toString("在线");
        n.longitude   = no["longitude"].toDouble();
        n.latitude    = no["latitude"].toDouble();
        n.altitude    = no["altitude"].toDouble();
        n.interferenceDb = no["interference_db"].toDouble();

        // 优先解析新版 devices[]（含实例 key 与完整参数），回退到旧版 comm_methods
        if (no.contains("devices") && no["devices"].isArray()) {
            for (const QJsonValue &dv : no["devices"].toArray()) {
                QJsonObject d = dv.toObject();
                QString key = d["instance_key"].toString();
                if (key.isEmpty())
                    key = QString("%1_%2").arg(d["device_name"].toString())
                                          .arg(d["instance_id"].toInt(), 2, 10, QChar('0'));
                if (key.isEmpty()) continue;
                DeviceParams dp;
                dp.instanceId            = d["instance_id"].toInt(0);
                dp.instanceName          = d["instance_name"].toString();
                dp.deviceRole            = d["device_role"].toString("communication");
                dp.deviceCategory        = d["device_category"].toString("wireless");
                dp.ioRole                = d["io_role"].toString("normal");
                dp.maxConnections        = d["max_connections"].toInt(4);
                dp.deviceHeightM         = d["device_height_m"].toDouble(0.0);
                dp.maxBandwidthBps       = d["max_bandwidth_bps"].toDouble(100e6);
                dp.freqHz                = d["freq_hz"].toDouble(3e9);
                dp.txPowerDbm            = d["tx_power_dbm"].toDouble(30.0);
                dp.rxSensitivityDbm      = d["rx_sensitivity_dbm"].toDouble(-90.0);
                dp.txAntennaGainDbi      = d["tx_antenna_gain_dbi"].toDouble(0.0);
                dp.rxAntennaGainDbi      = d["rx_antenna_gain_dbi"].toDouble(0.0);
                dp.noiseFigureDb         = d["noise_figure_db"].toDouble(7.0);
                dp.snrThresholdDb        = d["snr_threshold_db"].toDouble(10.0);
                dp.pathLossExponent      = d["path_loss_exponent"].toDouble(2.0);
                dp.additionalLossDb      = d["additional_loss_db"].toDouble(0.0);
                dp.fiberAttenuationDbPerKm = d["fiber_attenuation_db_per_km"].toDouble(0.2);
                dp.connectorLossDb       = d["connector_loss_db"].toDouble(1.0);
                dp.commProtocol          = d["comm_protocol"].toString();
                dp.deviceType            = d["device_type"].toString();
                dp.backplaneBandwidthBps = d["backplane_bandwidth_bps"].toDouble(1000e6);
                dp.processingDelayUs     = d["processing_delay_us"].toDouble(50.0);
                n.commMethods << key;
                n.deviceParams.insert(key, dp);
            }
            for (const QJsonValue &cv : no["device_connections"].toArray()) {
                QJsonArray pair = cv.toArray();
                if (pair.size() >= 2)
                    n.deviceConnections.append({pair[0].toString(), pair[1].toString()});
            }
        } else {
            for (const QJsonValue &m : no["comm_methods"].toArray())
                n.commMethods << m.toString();
        }
        // ★ 先攒着，循环结束一次性提交。
        //   原来这里是逐条 addNode —— 直连数据库时只是慢，
        //   走协议就是每个节点一次网络往返，几百个节点直接不可用。
        pendingNodes.append(n);
    }
    if (!m_db->addNodes(sceneId, pendingNodes)) {
        QMessageBox::warning(this, "导入失败",
                             "写入节点失败：" + m_db->lastError());
        return;
    }

    // 导入链路（跳过不符合状态/能力约束的链路）
    auto importedNodes = m_db->getNodesByScene(sceneId);
    QList<LinkInfo> pendingLinks;
    for (const QJsonValue &v : obj["links"].toArray()) {
        QJsonObject lo = v.toObject();
        LinkInfo l;
        l.sceneId      = sceneId;
        l.src          = lo["src"].toInt();
        l.dst          = lo["dst"].toInt();
        l.linkType     = lo["link_type"].toString();
        l.wirelessType = lo["wireless_type"].toString();
        l.bandwidthBps = lo["bandwidth_bps"].toDouble(50e6);
        l.propDelayS   = lo["prop_delay_s"].toDouble();
        l.freqHz           = lo["freq_hz"].toDouble(3e9);
        l.txPowerDbm       = lo["tx_power_dbm"].toDouble(30);
        l.rxSensitivityDbm = lo["rx_sensitivity_dbm"].toDouble(-90);
        l.txAntennaGainDbi = lo["tx_antenna_gain_dbi"].toDouble();
        l.rxAntennaGainDbi = lo["rx_antenna_gain_dbi"].toDouble();
        l.noiseFigureDb    = lo["noise_figure_db"].toDouble(7);
        l.snrThresholdDb   = lo["snr_threshold_db"].toDouble(10);
        l.pathLossExponent = lo["path_loss_exponent"].toDouble(2.0);
        l.additionalLossDb = lo["additional_loss_db"].toDouble();

        if (!isLinkValid(l, importedNodes)) continue;  // 静默跳过无效链路
        pendingLinks.append(l);
    }
    if (!m_db->addLinks(sceneId, pendingLinks)) {
        QMessageBox::warning(this, "导入失败",
                             "写入链路失败：" + m_db->lastError());
        return;
    }

    m_currentSceneId = sceneId;
    loadScenesIntoCmb();
    QMessageBox::information(this, "导入成功",
                             QString("场景导入完成：%1 个节点 / %2 条链路")
                                 .arg(pendingNodes.size()).arg(pendingLinks.size()));
}

void MainWindow::on_btnSceneOutput_clicked()
{
    if (m_currentSceneId < 0) return;
    QString filePath = QFileDialog::getSaveFileName(this, "导出场景",
                                                    "scene_export.json",
                                                    "JSON文件(*.json)");
    if (filePath.isEmpty()) return;

    QJsonObject obj;
    for (const SceneInfo &s : m_db->listScenes()) {
        if (s.id == m_currentSceneId) {
            obj["name"]        = s.name;
            obj["description"] = s.description;
            obj["scene_type"]  = s.sceneType;
            obj["create_time"] = s.createTime;
            break;
        }
    }

    QJsonArray nodesArr;
    for (const NodeInfo &n : m_db->getNodesByScene(m_currentSceneId)) {
        QJsonObject no;
        no["node_id"]       = n.nodeId;
        no["name"]          = n.name;
        no["node_type"]     = n.nodeType;
        no["status"]        = n.status;
        no["longitude"]     = n.longitude;
        no["latitude"]      = n.latitude;
        no["altitude"]      = n.altitude;
        no["interference_db"] = n.interferenceDb;

        // comm_methods：去重类型名（便于阅读）
        QJsonArray cm;
        for (const QString &t : deviceTypesOf(n.commMethods)) cm.append(t);
        no["comm_methods"]  = cm;

        // devices[]：完整多实例参数（无损往返）
        QJsonArray devArr;
        for (const QString &key : n.commMethods) {
            const DeviceParams dp = n.deviceParams.value(key);
            QJsonObject d;
            d["instance_key"]               = key;
            d["instance_id"]                = dp.instanceId;
            d["instance_name"]              = dp.instanceName;
            d["device_name"]                = deviceTypeOfKey(key);
            d["device_role"]                = dp.deviceRole;
            d["device_category"]            = dp.deviceCategory;
            d["io_role"]                    = dp.ioRole;
            d["max_connections"]            = dp.maxConnections;
            d["device_height_m"]            = dp.deviceHeightM;
            d["max_bandwidth_bps"]          = dp.maxBandwidthBps;
            d["freq_hz"]                    = dp.freqHz;
            d["tx_power_dbm"]               = dp.txPowerDbm;
            d["rx_sensitivity_dbm"]         = dp.rxSensitivityDbm;
            d["tx_antenna_gain_dbi"]        = dp.txAntennaGainDbi;
            d["rx_antenna_gain_dbi"]        = dp.rxAntennaGainDbi;
            d["noise_figure_db"]            = dp.noiseFigureDb;
            d["snr_threshold_db"]           = dp.snrThresholdDb;
            d["path_loss_exponent"]         = dp.pathLossExponent;
            d["additional_loss_db"]         = dp.additionalLossDb;
            d["fiber_attenuation_db_per_km"]= dp.fiberAttenuationDbPerKm;
            d["connector_loss_db"]          = dp.connectorLossDb;
            d["comm_protocol"]              = dp.commProtocol;
            d["device_type"]                = dp.deviceType;
            d["backplane_bandwidth_bps"]    = dp.backplaneBandwidthBps;
            d["processing_delay_us"]        = dp.processingDelayUs;
            devArr.append(d);
        }
        no["devices"] = devArr;

        QJsonArray connArr;
        for (const QPair<QString,QString> &c : n.deviceConnections) {
            QJsonArray pair; pair.append(c.first); pair.append(c.second);
            connArr.append(pair);
        }
        no["device_connections"] = connArr;

        nodesArr.append(no);
    }
    obj["nodes"] = nodesArr;

    QJsonArray linksArr;
    for (const LinkInfo &l : m_db->getLinksByScene(m_currentSceneId)) {
        QJsonObject lo;
        lo["src"]              = l.src;
        lo["dst"]              = l.dst;
        lo["link_type"]        = l.linkType;
        lo["wireless_type"]    = l.wirelessType;
        lo["bandwidth_bps"]    = l.bandwidthBps;
        lo["prop_delay_s"]     = l.propDelayS;
        lo["freq_hz"]              = l.freqHz;
        lo["tx_power_dbm"]         = l.txPowerDbm;
        lo["rx_sensitivity_dbm"]   = l.rxSensitivityDbm;
        lo["tx_antenna_gain_dbi"]  = l.txAntennaGainDbi;
        lo["rx_antenna_gain_dbi"]  = l.rxAntennaGainDbi;
        lo["noise_figure_db"]      = l.noiseFigureDb;
        lo["snr_threshold_db"]     = l.snrThresholdDb;
        lo["path_loss_exponent"]   = l.pathLossExponent;
        lo["additional_loss_db"]   = l.additionalLossDb;
        linksArr.append(lo);
    }
    obj["links"] = linksArr;

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "错误", "无法写入文件");
        return;
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    f.close();
    QMessageBox::information(this, "导出成功", "场景已导出到:\n" + filePath);
}

// ─────────────────────────────────────────────────────────────────────────────
// 链路 Tab 操作
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::on_btnLinkDelete_clicked()
{
    // 禁用链路删除
}

void MainWindow::on_btnLinkDetail_clicked()
{
    // 禁用链路详情编辑
}

// ─────────────────────────────────────────────────────────────────────────────
// 中央视图切换
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::on_btnMap_clicked()         { ui->stkMainView->setCurrentIndex(0); }
void MainWindow::on_btnSimResult_clicked()   { ui->stkMainView->setCurrentIndex(1); }
void MainWindow::on_btnDataManage_clicked()  { ui->stkMainView->setCurrentIndex(2); }

// ─────────────────────────────────────────────────────────────────────────────
// 右侧面板
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::on_btnDeviceDetails_clicked()
{
//    DialogPlanDetails *dlg = new DialogPlanDetails(this);
//    dlg->setAttribute(Qt::WA_DeleteOnClose);
//    dlg->show();
}

// ─────────────────────────────────────────────────────────────────────────────
// 拓扑视图交互处理
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::onTopoRequestAddNode(double lon, double lat, const QString &nodeType)
{
    if (m_currentSceneId < 0) return;
    DialogNode *dlg = new DialogNode(m_db, m_currentSceneId, lon, lat, nodeType, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &DialogNode::nodeSaved, this, [this]{
        onSceneChanged(m_currentSceneId);
    });
    dlg->exec();
}

void MainWindow::onTopoRequestAddLink(int srcNodeId, int dstNodeId)
{
    Q_UNUSED(srcNodeId);
    Q_UNUSED(dstNodeId);
    return; // 禁用连线功能，全部由 Python 返回结构构建
    /*
    if (m_currentSceneId < 0) return;
    DialogLink *dlg = new DialogLink(m_db, m_currentSceneId, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->preselectNodes(srcNodeId, dstNodeId);
    connect(dlg, &DialogLink::linkSaved, this, [this]{
        onSceneChanged(m_currentSceneId);
    });
    dlg->exec();
    */
}

void MainWindow::onTopoRequestMoveNode(int nodeId, double lon, double lat)
{
    if (m_currentSceneId < 0) return;
    for (const NodeInfo &n : m_currentNodes) {
        if (n.nodeId == nodeId) {
            NodeInfo updated = n;
            updated.longitude = lon;
            updated.latitude  = lat;
            m_db->updateNode(updated);
            onSceneChanged(m_currentSceneId);
            return;
        }
    }
}

void MainWindow::onTopoRequestEditNode(int nodeId)
{
    if (m_currentSceneId < 0) return;
    NodeInfo target;
    bool found = false;
    for (const NodeInfo &n : m_db->getNodesByScene(m_currentSceneId)) {
        if (n.nodeId == nodeId) { target = n; found = true; break; }
    }
    if (!found) return;
    DialogNode *dlg = new DialogNode(m_db, m_currentSceneId, target, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &DialogNode::nodeSaved, this, [this, nodeId]{
        cleanupInvalidLinks(m_currentSceneId, nodeId);
        onSceneChanged(m_currentSceneId);
    });
    dlg->exec();
}

void MainWindow::onTopoRequestEditLink(int srcNodeId, int dstNodeId)
{
    Q_UNUSED(srcNodeId);
    Q_UNUSED(dstNodeId);
    return; // 禁用修改链路功能，全部由 Python 端维护
    /*
    if (m_currentSceneId < 0) return;
    LinkInfo target;
    bool found = false;
    for (const LinkInfo &l : m_db->getLinksByScene(m_currentSceneId)) {
        if ((l.src == srcNodeId && l.dst == dstNodeId) ||
            (l.src == dstNodeId && l.dst == srcNodeId)) {
            target = l; found = true; break;
        }
    }
    if (!found) return;
    DialogLink *dlg = new DialogLink(m_db, m_currentSceneId, target, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &DialogLink::linkSaved, this, [this]{
        onSceneChanged(m_currentSceneId);
    });
    dlg->exec();
    */
}

void MainWindow::onTopoRequestDeleteNode(int nodeId)
{
    if (m_currentSceneId < 0) return;
    NodeInfo target;
    for (const NodeInfo &n : m_db->getNodesByScene(m_currentSceneId)) {
        if (n.nodeId == nodeId) { target = n; break; }
    }
    if (target.id < 0) return;
    int ret = QMessageBox::question(this, "确认删除",
        QString("删除节点「%1」将同时删除相关链路，确认？")
            .arg(target.name.isEmpty() ? QString::number(nodeId) : target.name));
    if (ret == QMessageBox::Yes) {
        m_db->deleteNode(target.id);
        onSceneChanged(m_currentSceneId);
    }
}

void MainWindow::onTopoRequestDeleteLink(int srcNodeId, int dstNodeId)
{
    Q_UNUSED(srcNodeId);
    Q_UNUSED(dstNodeId);
    return; // 禁用删除链路，全部由 Python 维护
    /*
    if (m_currentSceneId < 0) return;
    for (const LinkInfo &l : m_db->getLinksByScene(m_currentSceneId)) {
        if ((l.src == srcNodeId && l.dst == dstNodeId) ||
            (l.src == dstNodeId && l.dst == srcNodeId)) {
            int ret = QMessageBox::question(this, "确认删除",
                QString("确认删除节点 %1 ↔ %2 之间的链路？")
                    .arg(srcNodeId).arg(dstNodeId));
            if (ret == QMessageBox::Yes) {
                m_db->deleteLink(l.id);
                onSceneChanged(m_currentSceneId);
            }
            return;
        }
    }
    */
}

// ─────────────────────────────────────────────────────────────────────────────
// 拓扑视图右键菜单
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::showNodeContextMenu(int nodeId, const QPoint &globalPos)
{
    if (m_currentSceneId < 0) return;

    // 找到对应的节点数据库记录
    NodeInfo targetNode;
    bool found = false;
    for (const NodeInfo &n : m_db->getNodesByScene(m_currentSceneId)) {
        if (n.nodeId == nodeId) { targetNode = n; found = true; break; }
    }
    if (!found) return;

    QMenu menu(this);
    QAction *actEdit   = menu.addAction("配置节点");
    QAction *actDelete = menu.addAction("删除节点");

    QAction *chosen = menu.exec(globalPos);
    if (chosen == actEdit) {
        int editedNodeId = targetNode.nodeId;
        DialogNode *dlg = new DialogNode(m_db, m_currentSceneId, targetNode, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &DialogNode::nodeSaved, this,
                [this, editedNodeId]{
            cleanupInvalidLinks(m_currentSceneId, editedNodeId);
            onSceneChanged(m_currentSceneId);
        });
        dlg->exec();
    } else if (chosen == actDelete) {
        int ret = QMessageBox::question(this, "确认删除",
            "删除节点将同时删除该节点相关的所有链路，确认？");
        if (ret == QMessageBox::Yes) {
            m_db->deleteNode(targetNode.id);
            onSceneChanged(m_currentSceneId);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 鼠标事件（无边框窗口拖拽）
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton &&
        ui->wgtHeader->geometry().contains(event->pos())) {
        m_isDragging   = true;
        m_dragPosition = event->globalPosition().toPoint() - this->frameGeometry().topLeft();
        event->accept();
    }
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_isDragging && (event->buttons() & Qt::LeftButton)) {
        this->move(event->globalPosition().toPoint() - m_dragPosition);
        event->accept();
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    m_isDragging = false;
    event->accept();
}

bool MainWindow::nativeEvent(const QByteArray &eventType,
                              void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        MSG *msg = static_cast<MSG *>(message);
        if (msg->message == WM_NCHITTEST) {
            const int BW   = 5;
            const int CAPH = ui->wgtHeader->height();
            RECT wr;
            GetWindowRect(msg->hwnd, &wr);
            int x = (int)(short)LOWORD(msg->lParam) - (int)wr.left;
            int y = (int)(short)HIWORD(msg->lParam) - (int)wr.top;
            int w = wr.right  - wr.left;
            int h = wr.bottom - wr.top;
            bool L = x < BW, R = x >= w - BW, T = y < BW, B = y >= h - BW;
            if (T && L) { *result = HTTOPLEFT;     return true; }
            if (T && R) { *result = HTTOPRIGHT;    return true; }
            if (B && L) { *result = HTBOTTOMLEFT;  return true; }
            if (B && R) { *result = HTBOTTOMRIGHT; return true; }
            if (T)      { *result = HTTOP;         return true; }
            if (B)      { *result = HTBOTTOM;      return true; }
            if (L)      { *result = HTLEFT;        return true; }
            if (R)      { *result = HTRIGHT;       return true; }
            // 标题栏命中测试：x/y 为物理像素，而 CAPH 为 Qt 逻辑像素，需按设备像素比换算，
            // 否则在 100% 缩放下整条 header 都被判为标题栏(HTCAPTION)，导致按钮收不到点击。
            qreal dpr = this->devicePixelRatioF();
            if (dpr <= 0.0) dpr = 1.0;
            if (y < CAPH * dpr) {
                // 标题栏波段内：若落在可点击按钮上，交还客户区(HTCLIENT)以保证按钮可点；
                // 否则视为标题栏空白区，返回 HTCAPTION 支持拖拽移动窗口。
                QWidget *child = this->childAt(qRound(x / dpr), qRound(y / dpr));
                for (QWidget *wgt = child; wgt && wgt != this; wgt = wgt->parentWidget()) {
                    if (qobject_cast<QAbstractButton *>(wgt)) { *result = HTCLIENT; return true; }
                }
                *result = HTCAPTION; return true;
            }
            *result = HTCLIENT; return true;
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    ui->lvNodeList->reset();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() != QEvent::MouseButtonPress)
        return QMainWindow::eventFilter(obj, event);

    // 点击 Python 状态标签 → 弹出地址配置（无标题栏）
    if (obj == m_lblStatusPy) {
        FramelessDialog dlg(this);
        QVBoxLayout *lay = new QVBoxLayout(&dlg);
        lay->setContentsMargins(24, 20, 24, 20);
        lay->setSpacing(10);
        QLabel *title = new QLabel("Python 通信连接服务", &dlg);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:15px; font-weight:700; color:#1565C0; padding-bottom:4px;");
        QLabel *hint = new QLabel("请输入 host:port（例如 127.0.0.1:9000）", &dlg);
        hint->setStyleSheet("font-size:12px; color:#546E7A;");
        QLineEdit *le = new QLineEdit(&dlg);
        le->setText(QString("%1:%2").arg(m_simBridge->host()).arg(m_simBridge->port()));
        QDialogButtonBox *btns = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        if (btns->button(QDialogButtonBox::Ok)) btns->button(QDialogButtonBox::Ok)->setText("确定");
        if (btns->button(QDialogButtonBox::Cancel)) btns->button(QDialogButtonBox::Cancel)->setText("取消");
        connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        lay->addWidget(title);
        lay->addWidget(hint);
        lay->addWidget(le);
        lay->addWidget(btns);
        dlg.setMinimumWidth(340);
        if (dlg.exec() == QDialog::Accepted) {
            QString input = le->text().trimmed();
            if (!input.isEmpty()) {
                QStringList parts = input.split(":");
                QString host = parts.value(0).trimmed();
                quint16 port = static_cast<quint16>(parts.value(1).trimmed().toUShort());
                if (!host.isEmpty() && port > 0) {
                    m_simBridge->disconnectFromServer();
                    m_simBridge->setServer(host, port);
                    m_simBridge->connectToServer();
                    postStatusLog(QString("正在连接 %1:%2 …").arg(host).arg(port));
                }
            }
        }
        return true;
    }

    // 点击 DB 状态标签 → 弹出数据库服务端配置（无标题栏）
    if (obj == m_lblStatusDb) {
        /*
        // ======= 达梦数据库连接配置 (只注释不删除) =======
        FramelessDialog dlg(this);
        QVBoxLayout *lay = new QVBoxLayout(&dlg);
        lay->setContentsMargins(24, 20, 24, 20);
        lay->setSpacing(10);
        QLabel *title = new QLabel("达梦数据库连接配置", &dlg);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:15px; font-weight:700; color:#1565C0; padding-bottom:4px;");
        QLabel *hint = new QLabel("请输入 DM8 服务端地址（格式：host:port，例如 127.0.0.1:5236）", &dlg);
        hint->setWordWrap(true);
        hint->setStyleSheet("font-size:12px; color:#546E7A;");
        QLineEdit *le = new QLineEdit(&dlg);
        le->setText(m_localDb ? m_localDb->dbPath() : "127.0.0.1:5236");
        QDialogButtonBox *btns = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        if (btns->button(QDialogButtonBox::Ok)) btns->button(QDialogButtonBox::Ok)->setText("确定");
        if (btns->button(QDialogButtonBox::Cancel)) btns->button(QDialogButtonBox::Cancel)->setText("取消");
        connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        lay->addWidget(title);
        lay->addWidget(hint);
        lay->addWidget(le);
        lay->addWidget(btns);
        dlg.setMinimumWidth(360);
        if (dlg.exec() == QDialog::Accepted) {
            QString path = le->text().trimmed();
            if (!path.isEmpty()) {
                // 这个对话框配置的是**本地**数据库。手工配了本地库就意味着
                // 用户想用单机模式，所以顺带把生效实现切过去，
                // 否则会出现"配了半天但读写还是走服务端"的困惑。
                bool success = m_localDb && m_localDb->initDB(path);
                if (success) { m_db = m_localDb; m_remoteMode = false; }
                if (success) {
                    m_dbOk = true;
                    m_lblStatusDb->setText("DB: 已连接");
                    m_lblStatusDb->setStyleSheet("color:#27AE60; padding:0 10px;");
                    postStatusLog("数据库已重新连接: " + path);
                    onSceneChanged(m_currentSceneId);
                    loadScenesIntoCmb();
                } else {
                    m_dbOk = false;
                    m_lblStatusDb->setText("DB: 连接失败");
                    m_lblStatusDb->setStyleSheet("color:#E74C3C; padding:0 10px;");
                    postStatusLog("数据库连接失败: " + path, true);
                }
            }
        }
        // ===============================================
        */

        // ======= SQLite 数据库配置 =======
        FramelessDialog dlg(this);
        QVBoxLayout *lay = new QVBoxLayout(&dlg);
        lay->setContentsMargins(24, 20, 24, 20);
        lay->setSpacing(10);
        QLabel *title = new QLabel("SQLite 数据库配置", &dlg);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:15px; font-weight:700; color:#1565C0; padding-bottom:4px;");
        QLabel *hint = new QLabel("请输入 SQLite 数据库文件路径（例如 sim_data.db）", &dlg);
        hint->setWordWrap(true);
        hint->setStyleSheet("font-size:12px; color:#546E7A;");
        
        QHBoxLayout *hlay = new QHBoxLayout();
        QLineEdit *le = new QLineEdit(&dlg);
        QString defaultDbPath = m_localDb ? m_localDb->dbPath() : "";
        if (defaultDbPath.isEmpty()) {
            defaultDbPath = QCoreApplication::applicationDirPath() + "/sim_data.db";
        }
        le->setText(defaultDbPath);
        QPushButton *btnBrowse = new QPushButton("浏览...", &dlg);
        hlay->addWidget(le);
        hlay->addWidget(btnBrowse);

        connect(btnBrowse, &QPushButton::clicked, this, [le, this]() {
            QString filePath = QFileDialog::getSaveFileName(
                this, "选择 SQLite 数据库文件", le->text(), "SQLite Database Files (*.db);;All Files (*)");
            if (!filePath.isEmpty()) {
                le->setText(filePath);
            }
        });

        QDialogButtonBox *btns = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        if (btns->button(QDialogButtonBox::Ok)) btns->button(QDialogButtonBox::Ok)->setText("确定");
        if (btns->button(QDialogButtonBox::Cancel)) btns->button(QDialogButtonBox::Cancel)->setText("取消");
        connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        
        lay->addWidget(title);
        lay->addWidget(hint);
        lay->addLayout(hlay);
        lay->addWidget(btns);
        dlg.setMinimumWidth(400);
        
        if (dlg.exec() == QDialog::Accepted) {
            QString path = le->text().trimmed();
            if (!path.isEmpty()) {
                // 这个对话框配置的是**本地**数据库。手工配了本地库就意味着
                // 用户想用单机模式，所以顺带把生效实现切过去，
                // 否则会出现"配了半天但读写还是走服务端"的困惑。
                bool success = m_localDb && m_localDb->initDB(path);
                if (success) { m_db = m_localDb; m_remoteMode = false; }
                if (success) {
                    m_dbOk = true;
                    m_lblStatusDb->setText("DB: 已连接");
                    m_lblStatusDb->setStyleSheet("color:#27AE60; padding:0 10px;");
                    postStatusLog("数据库已重新连接: " + path);
                    onSceneChanged(m_currentSceneId);
                    loadScenesIntoCmb();
                } else {
                    m_dbOk = false;
                    m_lblStatusDb->setText("DB: 连接失败");
                    m_lblStatusDb->setStyleSheet("color:#E74C3C; padding:0 10px;");
                    postStatusLog("数据库连接失败: " + path, true);
                }
            }
        }
        return true;
    }

    return QMainWindow::eventFilter(obj, event);
}

// ─────────────────────────────────────────────────────────────────────────────
// 检索过滤槽函数
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onNodeFilterChanged()
{
    if (m_currentSceneId < 0) return;

    QString searchText  = ui->leSearchNode->text().trimmed().toLower();
    QString typeFilter  = ui->cmbNodeType->currentText();
    QString statFilter  = ui->cmbNodeStatus->currentText();
    QString commFilter  = ui->cmbCommMode->currentText();

    // 通信方式过滤映射：cmbCommMode 显示中文名，commMethods 存的是英文key
    static const QMap<QString, QString> s_cnToKey = {
        {"光缆",       "fiber"},
        {"野战电缆",   "fieldwire"},
        {"自组网电台", "adhoc"},
        {"窄带战术电台","narrowband"},
        {"微波接力",   "microwave"},
        {"超视距微波", "scatter"},
        {"移动公网",   "cellular"},
        {"短波电台",   "shortwave"},
        {"卫星通信",   "satellite"}
    };

    QList<NodeInfo> filtered;
    for (const NodeInfo &n : m_currentNodes) {
        if (!searchText.isEmpty()) {
            bool hit = n.name.toLower().contains(searchText)
                    || QString::number(n.nodeId).contains(searchText);
            if (!hit) continue;
        }
        if (typeFilter != "全部" && !typeFilter.isEmpty() && n.nodeType != typeFilter)
            continue;
        if (statFilter != "全部" && !statFilter.isEmpty() && n.status != statFilter)
            continue;
        if (commFilter != "全部" && !commFilter.isEmpty()) {
            QString commKey = s_cnToKey.value(commFilter, commFilter);
            if (!nodeHasCapability(n.commMethods, commKey)) continue;
        }
        filtered << n;
    }

    // 只重建列表模型，不重填过滤下拉框
    QMap<int, int> linkCounts;
    for (const LinkInfo &l : m_currentLinks) {
        linkCounts[l.src]++;
        linkCounts[l.dst]++;
    }
    QStandardItemModel *model = new QStandardItemModel(this);
    for (const NodeInfo &n : filtered) {
        QStandardItem *item = new QStandardItem();
        item->setData(n.nodeId,    Qt::UserRole);
        item->setData(n.name,      Qt::UserRole + 1);
        item->setData(n.nodeType,  Qt::UserRole + 2);
        item->setData(QString("%1°E %2°N").arg(n.longitude,0,'f',4).arg(n.latitude,0,'f',4),
                      Qt::UserRole + 3);
        item->setData(linkCounts.value(n.nodeId, 0), Qt::UserRole + 4);
        item->setData(n.status,        Qt::UserRole + 5);
        item->setData(n.id,            Qt::UserRole + 6);
        item->setData(n.altitude,      Qt::UserRole + 7);
        item->setData(n.interferenceDb, Qt::UserRole + 8);
        model->appendRow(item);
    }
    QAbstractItemModel *old = ui->lvNodeList->model();
    ui->lvNodeList->setModel(model);
    if (old && old->parent() == this) delete old;
}

void MainWindow::onLinkFilterChanged()
{
    if (m_currentSceneId < 0) return;

    QString searchText  = ui->leSearchLink->text().trimmed().toLower();
    int srcFilter = ui->cmbLinkSrcFilter->currentIndex() > 0
                    ? ui->cmbLinkSrcFilter->currentData().toInt() : -1;
    int dstFilter = ui->cmbLinkDstFilter->currentIndex() > 0
                    ? ui->cmbLinkDstFilter->currentData().toInt() : -1;
    QString typeFilter  = ui->cmbLinkTypeFilter->currentText();   // "全部"/"有线"/"无线"/"无线·微波"...

    ui->lwLinkList->clear();
    for (const LinkInfo &l : m_currentLinks) {
        if (srcFilter >= 0 && l.src != srcFilter) continue;
        if (dstFilter >= 0 && l.dst != dstFilter) continue;

        bool isWireless = (l.linkType == "wireless" || l.linkType == "无线");

        // 链路类型过滤：过滤项为"有线"/"无线"或具体通信方式名称
        if (typeFilter != "全部" && !typeFilter.isEmpty()) {
            if (typeFilter == "有线"  &&  isWireless)  continue;
            if (typeFilter == "无线"  && !isWireless)  continue;
            // 具体通信方式：直接匹配 wirelessType 的中文翻译
            if (typeFilter != "有线" && typeFilter != "无线" && translateLinkType(l.wirelessType) != typeFilter) continue;
        }

        QString srcName = nodeIdToName(l.src, m_currentNodes);
        QString dstName = nodeIdToName(l.dst, m_currentNodes);

        if (!searchText.isEmpty()) {
            bool hit = srcName.toLower().contains(searchText)
                    || dstName.toLower().contains(searchText);
            if (!hit) continue;
        }

        QString wtStr = translateLinkType(l.wirelessType);
        if (wtStr.isEmpty()) {
            wtStr = (l.linkType == "wireless" || l.linkType == "无线") ? "无线" : "有线";
        }

        QStringList flowStrings;
        for (const LinkFlow &f : l.flows) {
            double rateMbps = f.bandwidthBps / 1e6;
            flowStrings << QString("流%1: %2M").arg(f.flowId).arg(rateMbps, 0, 'f', 1);
        }

        QString line1 = QString("链路%1  %2↔%3").arg(l.id).arg(srcName).arg(dstName);
        QString line2 = QString("带宽: %1 Mbps").arg(l.bandwidthBps / 1e6, 0, 'f', 1);
        QString delayStr = QString("时延: %1 ms").arg(l.propDelayS * 1000, 0, 'f', 3);
        QString line3 = flowStrings.isEmpty() ? "" : QString("流: %1").arg(flowStrings.join(" / "));

        QListWidgetItem *item = new QListWidgetItem();
        item->setData(Qt::UserRole,     l.id);
        item->setData(Qt::UserRole + 1, line1);
        item->setData(Qt::UserRole + 2, line2);
        if (!line3.isEmpty()) {
            item->setData(Qt::UserRole + 3, line3);
        }
        item->setData(Qt::UserRole + 4, wtStr);
        item->setData(Qt::UserRole + 5, delayStr);

        // 鼠标悬停显示完整、未截断的链路详情
        QString toolTipText = QString(
            "链路ID: %1\n"
            "节点: %2 ↔ %3\n"
            "类型: %4\n"
            "带宽: %5 Mbps\n"
            "时延: %6 ms"
        ).arg(l.id).arg(srcName).arg(dstName).arg(wtStr)
         .arg(l.bandwidthBps / 1e6, 0, 'f', 1)
         .arg(l.propDelayS * 1000, 0, 'f', 3);
        if (!line3.isEmpty()) {
            toolTipText += "\n" + line3;
        }
        item->setToolTip(toolTipText);

        ui->lwLinkList->addItem(item);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 数据视图：链路过滤器变化
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onDataViewLinkFilterChanged()
{
    if (m_currentSceneId < 0) return;

    int nodeFilter = ui->cmbLinkNodeFilter->currentIndex() > 0
                     ? ui->cmbLinkNodeFilter->currentData().toInt() : -1;
    QString typeFilter = ui->cmbLinkTypeFilter_2->currentText();

    QList<LinkInfo> filtered;
    for (const LinkInfo &l : m_currentLinks) {
        if (nodeFilter >= 0 && l.src != nodeFilter && l.dst != nodeFilter) continue;
        bool isWireless = (l.linkType == "wireless" || l.linkType == "无线");
        if (typeFilter != "全部类型" && !typeFilter.isEmpty()) {
            if (typeFilter == "有线"  &&  isWireless) continue;
            if (typeFilter == "无线"  && !isWireless) continue;
            // 具体通信方式：直接匹配 wirelessType 的中文翻译
            if (typeFilter != "有线" && typeFilter != "无线" && translateLinkType(l.wirelessType) != typeFilter) continue;
        }
        filtered << l;
    }
    buildAndSetLinkTableModel(filtered, m_currentNodes);
}

// ─────────────────────────────────────────────────────────────────────────────
// 数据视图：节点过滤器变化
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onDataViewNodeFilterChanged()
{
    if (m_currentSceneId < 0) return;

    QString typeFilter   = ui->cmbNodeTypeFilter->currentText();
    QString statusFilter = ui->cmbNodeStatusFilter->currentText();

    QList<NodeInfo> filtered;
    for (const NodeInfo &n : m_currentNodes) {
        if (typeFilter   != "全部类型"  && !typeFilter.isEmpty()   && n.nodeType != typeFilter)   continue;
        if (statusFilter != "全部状态"  && !statusFilter.isEmpty() && n.status   != statusFilter) continue;
        filtered << n;
    }
    buildAndSetNodeTableModel(filtered);
}

// ─────────────────────────────────────────────────────────────────────────────
// 导出 CSV
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::exportTableToCsv(QTableView *table, const QString &title)
{
    if (!table || !table->model()) return;
    QString path = QFileDialog::getSaveFileName(this, "导出CSV", title + ".csv",
                                                "CSV文件 (*.csv)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "错误", "无法写入文件：" + path);
        return;
    }
    QTextStream out(&f);
    // Qt6 移除了 setCodec（QTextCodec 已挪出 Core），改用 QStringConverter。
    // Qt6 默认就是 UTF-8，显式写出来是为了把编码意图留在代码里。
    out.setEncoding(QStringConverter::Utf8);
    out << "\xEF\xBB\xBF";  // UTF-8 BOM（Excel兼容）

    QAbstractItemModel *model = table->model();
    int cols = model->columnCount();
    int rows = model->rowCount();

    QStringList header;
    for (int c = 0; c < cols; ++c)
        header << model->headerData(c, Qt::Horizontal).toString();
    out << header.join(",") << "\n";

    for (int r = 0; r < rows; ++r) {
        QStringList row;
        for (int c = 0; c < cols; ++c) {
            QString val = model->data(model->index(r, c)).toString();
            if (val.contains(',') || val.contains('"') || val.contains('\n'))
                val = '"' + val.replace('"', "\"\"") + '"';
            row << val;
        }
        out << row.join(",") << "\n";
    }
    f.close();
    QMessageBox::information(this, "导出完成", QString("已导出 %1 条记录").arg(rows));
}

void MainWindow::on_btnExportLinkCsv_clicked()
{
    exportTableToCsv(ui->tableLink, "链路列表");
}

void MainWindow::on_btnExportFlowCsv_clicked()
{
    exportTableToCsv(ui->tableNode, "节点列表");
}

// ═════════════════════════════════════════════════════════════════════════════
// 仿真相关初始化
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::initCharts()
{
    // 时延折线图
    m_seriesDelay = new QLineSeries(this);
    QChart *chartD = new QChart();
    chartD->addSeries(m_seriesDelay);
    chartD->setTitle("端到端时延 (ms)");
    chartD->createDefaultAxes();
    chartD->legend()->hide();
    chartD->setMargins(QMargins(4, 4, 4, 4));
//    ui->chartDelay->setChart(chartD);
//    ui->chartDelay->setRenderHint(QPainter::Antialiasing);

    // 丢包率折线图
    m_seriesLoss = new QLineSeries(this);
    QChart *chartL = new QChart();
    chartL->addSeries(m_seriesLoss);
    chartL->setTitle("丢包率 (%)");
    chartL->createDefaultAxes();
    chartL->legend()->hide();
    chartL->setMargins(QMargins(4, 4, 4, 4));
//    ui->chartLossRate->setChart(chartL);
//    ui->chartLossRate->setRenderHint(QPainter::Antialiasing);
}

void MainWindow::initWarnList()
{
    m_warnModel = new QStandardItemModel(this);
    ui->warnListView->setModel(m_warnModel);
    ui->warnListView->setItemDelegate(new WarnCardDelegate(ui->warnListView));
    ui->warnListView->setSpacing(2);
    ui->warnListView->setUniformItemSizes(false);
    ui->warnListView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->warnListView->setMouseTracking(true);   // 启用 tooltip 悬浮检测

    // tablePaths 列头配置（v2.0：流 ID / src / dst / 跳数 / 途径节点 / 是否满足 / 未满足原因）
    ui->tablePaths->setColumnCount(7);
    ui->tablePaths->setHorizontalHeaderLabels(
        {"流 ID", "源节点", "目标节点", "跳数", "途径节点序列", "是否满足", "未满足原因"});
    styleDataTable(ui->tablePaths);

    // tableFlows 列头配置（v2.0：流 ID / src / dst / 需求带宽 / 实际带宽 / 途径节点）
    ui->tableFlows->setColumnCount(6);
    ui->tableFlows->setHorizontalHeaderLabels(
        {"流 ID", "源节点", "目标节点", "需求带宽(Mbps)", "实际带宽(Mbps)", "途径节点序列"});
    styleDataTable(ui->tableFlows);
}

// ─────────────────────────────────────────────────────────────────────────────
// SimBridge 初始化：TCP连接 + 信号绑定
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::initSimBridge()
{
    m_simBridge = new SimBridge(this);
    // 规划服务与数据访问是同一个服务端，用同一组地址；
    // 否则会出现"数据走云端、规划还连本机"的割裂状态。
    m_simBridge->setServer(m_serverHost, m_serverPort);
    connect(m_simBridge, &SimBridge::connected,
            this, &MainWindow::onSimBridgeConnected);
    connect(m_simBridge, &SimBridge::disconnected,
            this, &MainWindow::onSimBridgeDisconnected);
    connect(m_simBridge, &SimBridge::connectionError,
            this, &MainWindow::onSimBridgeConnectionError);
    connect(m_simBridge, &SimBridge::planAcknowledged,
            this, &MainWindow::onPlanAcknowledged);
    connect(m_simBridge, &SimBridge::planProgress,
            this, &MainWindow::onPlanProgress);
    connect(m_simBridge, &SimBridge::planResultReceived,
            this, &MainWindow::onPlanResult);
    connect(m_simBridge, &SimBridge::simError,
            this, &MainWindow::onSimError);
    connect(m_simBridge, &SimBridge::simFinished,
            this, &MainWindow::onSimFinished);

    // ── 进度条 ──────────────────────────────────────────────────────────
    // 放在「启动」按钮右边。平时隐藏，规划期间才出现 —— 一个常驻的
    // 空进度条只会占地方，还让人以为有什么东西一直在跑。
    m_planProgress = new QProgressBar(this);
    m_planProgress->setRange(0, 100);
    m_planProgress->setValue(0);
    m_planProgress->setFixedWidth(220);
    m_planProgress->setTextVisible(true);
    m_planProgress->setVisible(false);
    if (auto *bar = ui->btnSimToggle->parentWidget()) {
        if (auto *lay = qobject_cast<QHBoxLayout *>(bar->layout())) {
            // 插到「启动」按钮之后
            lay->insertWidget(lay->indexOf(ui->btnSimToggle) + 1, m_planProgress);
        }
    }

    m_simBridge->connectToServer();   // 尝试连接到规划服务端
}

// ─────────────────────────────────────────────────────────────────────────────
// v3.0 分阶段推送：ack → progress×N → plan_result
//
// 改造前是"一次请求一次响应"，按钮置灰之后界面完全没有反馈，
// 规模大时用户看到的就是一个卡住的程序，分不清在算还是已经死了。
// 实测 150 节点 / 150 流：ack 在 1.4ms 到达、最终结果在 115ms 到达 ——
// 中间这 113ms 原来是纯黑盒，现在有 43 条进度在填。
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::onPlanAcknowledged(quint64 taskId, int nodeCount, int flowCount)
{
    qDebug() << "[Plan] 服务端已受理 task_id=" << taskId;
    // 从"不确定的忙碌动画"切回真实的 0~100 进度
    m_planProgress->setRange(0, 100);
    m_planProgress->setValue(0);
    m_planProgress->setVisible(true);
    m_planProgress->setFormat("已受理 %p%");
    postStatusLog(QString("服务端已受理规划任务 #%1：%2 个节点 / %3 条流")
                      .arg(taskId).arg(nodeCount).arg(flowCount));
}

void MainWindow::onPlanProgress(int percent, QString stage)
{
    if (!m_planProgress->isVisible())
        m_planProgress->setVisible(true);
    m_planProgress->setValue(qBound(0, percent, 100));
    // 阶段名直接显示在进度条上，比单独占一个 label 省地方
    m_planProgress->setFormat(stage + " %p%");
    ui->btnSimToggle->setText(QString("规划中 %1%").arg(percent));
}

// ─────────────────────────────────────────────────────────────────────────────
// v2.0：构建综合规划请求 payload（场景 + 节点 + 设备 + 规划流）
// 节点编号全程使用用户设定的 node_id，不再做 0..N-1 重映射
// ─────────────────────────────────────────────────────────────────────────────
QJsonObject MainWindow::buildPlanPayload() const
{
    QJsonObject payload;

    auto nodes = m_db->getNodesByScene(m_planSceneId);

    // ── scene ────────────────────────────────────────────────────────────────
    QJsonObject sceneObj;
    sceneObj["id"] = m_planSceneId;
    for (const SceneInfo &s : m_db->listScenes()) {
        if (s.id == m_planSceneId) {
            sceneObj["name"]        = s.name;
            sceneObj["description"] = s.description;
            sceneObj["scene_type"]  = s.sceneType;
            break;
        }
    }
    payload["scene"] = sceneObj;

    // ── nodes[] ──────────────────────────────────────────────────────────────
    QJsonArray nodesArr;
    for (const NodeInfo &n : nodes) {
        QJsonObject no;
        no["node_id"]         = n.nodeId;
        no["name"]            = n.name;
        no["node_type"]       = n.nodeType;
        no["status"]          = n.status;
        no["longitude"]       = n.longitude;
        no["latitude"]        = n.latitude;
        no["altitude"]        = n.altitude;
        no["interference_db"] = n.interferenceDb;

        // comm_methods：去重后的设备类型名列表（["microwave","satellite","switch"]）
        QJsonArray cmArr;
        for (const QString &t : deviceTypesOf(n.commMethods)) cmArr.append(t);
        no["comm_methods"] = cmArr;

        // devices[]：多实例设备，按 instance_id 区分同类型设备（v3 协议）
        QJsonArray devArr;
        for (const QString &key : n.commMethods) {
            const DeviceParams dp = n.deviceParams.value(key);
            QJsonObject dev;
            // 公共字段
            dev["instance_id"]     = dp.instanceId;
            dev["instance_key"]    = key;                       // 与 device_connections 中引用一致
            dev["device_name"]     = deviceTypeOfKey(key);      // 设备类型英文名
            dev["instance_name"]   = dp.instanceName;           // 用户可读名称（如"光缆_01"）
            dev["device_role"]     = dp.deviceRole;             // communication / switch
            dev["device_category"] = dp.deviceCategory;         // wireless / wired
            dev["io_role"]         = dp.ioRole;                 // normal / input / output：节点内设备流向
            dev["max_connections"] = dp.maxConnections;         // 该设备最大连接数（限制设备间连线数量）
            dev["device_height_m"] = dp.deviceHeightM;          // 设备架设高度(m)，所有设备通用

            if (dp.deviceRole == "switch") {
                // 交换设备
                dev["backplane_bandwidth_bps"] = dp.backplaneBandwidthBps;
                dev["processing_delay_us"]     = dp.processingDelayUs;
            } else if (dp.deviceCategory == "wired") {
                // 有线通信设备
                dev["max_bandwidth_bps"]            = dp.maxBandwidthBps;
                dev["tx_power_dbm"]                 = dp.txPowerDbm;
                dev["rx_sensitivity_dbm"]           = dp.rxSensitivityDbm;
                dev["fiber_attenuation_db_per_km"]  = dp.fiberAttenuationDbPerKm;
                dev["connector_loss_db"]            = dp.connectorLossDb;
                dev["comm_protocol"]                = dp.commProtocol;
                dev["device_type"]                  = dp.deviceType;
            } else {
                // 无线通信设备
                dev["max_bandwidth_bps"]   = dp.maxBandwidthBps;
                dev["freq_hz"]             = dp.freqHz;
                dev["tx_power_dbm"]        = dp.txPowerDbm;
                dev["rx_sensitivity_dbm"]  = dp.rxSensitivityDbm;
                dev["tx_antenna_gain_dbi"] = dp.txAntennaGainDbi;
                dev["rx_antenna_gain_dbi"] = dp.rxAntennaGainDbi;
                dev["noise_figure_db"]     = dp.noiseFigureDb;
                dev["snr_threshold_db"]    = dp.snrThresholdDb;
                dev["path_loss_exponent"]  = dp.pathLossExponent;
                dev["additional_loss_db"]  = dp.additionalLossDb;
            }
            devArr.append(dev);
        }
        no["devices"] = devArr;

        // device_connections：节点内设备间连接对 [["fiber_00","switch_00"], ...]
        QJsonArray connArr;
        for (const QPair<QString,QString> &c : n.deviceConnections) {
            QJsonArray pair;
            pair.append(c.first);
            pair.append(c.second);
            connArr.append(pair);
        }
        no["device_connections"] = connArr;

        nodesArr.append(no);
    }
    payload["nodes"] = nodesArr;

    // ── forbidden_node_links[]：节点级禁连规则 [[nodeA,nodeB], ...] ────────────
    QJsonArray forbiddenArr;
    for (const QPair<int,int> &ex : m_planExclusions) {
        QJsonArray pair;
        pair.append(ex.first);
        pair.append(ex.second);
        forbiddenArr.append(pair);
    }
    payload["forbidden_node_links"] = forbiddenArr;

    // ── plans[]（规划流）：使用原始 node_id，不重映射 ───────────────────────
    QJsonArray plansArr;
    for (const FlowInfo &f : m_planFlows) {
        QJsonObject fo;
        fo["fid"]         = f.fid;
        fo["src_node_id"] = f.src;
        fo["dst_node_id"] = f.dst;
        fo["qos_level"]   = f.qosLevel;
        fo["rate_bps"]    = f.rateBps;
        fo["arrival_t"]   = f.arrivalT;
        fo["dur_t"]       = f.durT;
        fo["k_paths"]     = f.kPaths;
        fo["opt_target"]  = f.optTarget;
        plansArr.append(fo);
    }
    payload["plans"] = plansArr;

    return payload;
}

// ═════════════════════════════════════════════════════════════════════════════
// 仿真控制槽
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::on_btnSimToggle_clicked()
{
    if (m_simRunning) return;

    if (!m_simBridge->isConnected()) {
        QMessageBox::warning(this, "未连接",
            QString("规划服务端未连接（%1:%2）\n"
                    "请先启动 plan_server，或点击状态栏标签修改地址。")
                .arg(m_simBridge->host()).arg(m_simBridge->port()));
        return;
    }
    if (m_planFlows.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先点击「规划」按钮配置通信流");
        return;
    }
    if (m_planSceneId < 0) {
        QMessageBox::warning(this, "提示", "规划场景无效，请重新配置规划");
        return;
    }
    auto planNodes = m_db->getNodesByScene(m_planSceneId);
    if (planNodes.isEmpty()) {
        QMessageBox::warning(this, "提示", "规划场景内没有节点，请先添加节点");
        return;
    }

    // 切换到仿真结果页
    ui->stkMainView->setCurrentIndex(1);

    // 重置图表和结果
    if (m_seriesDelay) m_seriesDelay->clear();
    if (m_seriesLoss)  m_seriesLoss->clear();
    if (m_warnModel)   m_warnModel->clear();
    ui->tablePaths->setRowCount(0);
    ui->tableFlows->setRowCount(0);
//    ui->lblAvgDelayValue->setText("--");
//    ui->lblPktLossValue->setText("--");
//    ui->lblBwUsageValue->setText("--");
    m_topoView->resetHighlight();

    // 记录启动时刻，供计算耗时
    m_simStartTime  = QDateTime::currentDateTime();
    ui->lblPlanTimeValue->setText(m_simStartTime.toString("hh:mm:ss"));
    ui->lblCalcTimeValue->setText("0.0 s");
    ui->lblPlanPathValue->setText("0 条");

    m_simRunning = true;
    ui->btnSimToggle->setText("发送中...");
    ui->btnSimToggle->setEnabled(false);
    // 先显示成"忙碌"样式（range 0,0 是 Qt 的不确定进度动画）：
    // 请求已发出但服务端还没 ack，此时并不知道进度，
    // 显示 0% 会让人误以为卡住了。收到 ack 再切回 0~100。
    if (m_planProgress) {
        m_planProgress->setRange(0, 0);
        m_planProgress->setFormat("等待服务端受理");
        m_planProgress->setVisible(true);
    }

    postStatusLog(QString("正在发送规划请求：%1 个节点 / %2 条流")
                  .arg(planNodes.size()).arg(m_planFlows.size()));
    m_simBridge->startPlan(buildPlanPayload());
}

// ─────────────────────────────────────────────────────────────────────────────
// SimBridge 回调槽
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// v2.0：一次性综合规划结果接收 —— 链路写库 + 路径表 + 告警 + 拓扑高亮
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onPlanResult(PlanResultPacket packet)
{
    qDebug() << "[onPlanResult] links=" << packet.links.size()
             << "plan_results=" << packet.planResults.size()
             << "warns=" << packet.warns.size();

    if (m_planSceneId < 0) {
        postStatusLog("收到规划结果但未确定目标场景，已忽略", true);
        return;
    }

    // ── 1. 链路结果写库（覆盖原有链路） ─────────────────────────────────────
    m_db->clearLinksByScene(m_planSceneId);
    for (const LinkResult &lr : packet.links) {
        LinkInfo l;
        l.sceneId      = m_planSceneId;
        l.src          = lr.srcNodeId;   // 直接使用原始 node_id
        l.dst          = lr.dstNodeId;
        l.linkType     = lr.linkType;
        l.wirelessType = lr.wirelessType;
        l.bandwidthBps = lr.bandwidthBps;
        l.propDelayS   = lr.propDelayS;

        // 流信息 LinkResultFlow → LinkFlow
        for (const LinkResultFlow &lrf : lr.flows) {
            LinkFlow lf;
            lf.flowId       = lrf.fid;
            lf.bandwidthBps = lrf.bandwidthBps;
            lf.description  = lrf.description;
            l.flows.append(lf);
        }
        m_db->addLink(l);
    }

    // ── 2. 路径表 + 流量统计表填充 ──────────────────────────────────────────
    auto makeCell = [](const QString &text, Qt::Alignment a = Qt::AlignCenter) {
        auto *it = new QTableWidgetItem(text);
        it->setTextAlignment(a);
        return it;
    };

    ui->tablePaths->setRowCount(0);
    ui->tableFlows->setRowCount(0);

    // fid → 对应规划输入（用于查需求带宽）
    QMap<int, FlowInfo> planMap;
    for (const FlowInfo &f : m_planFlows) planMap[f.fid] = f;

    QList<QList<int>> highlightPaths;
    int satisfiedCount = 0;

    for (const PlanResult &pr : packet.planResults) {
        // tablePaths（流 ID / src / dst / 跳数 / 途径节点 / 是否满足 / 未满足原因）
        int row = ui->tablePaths->rowCount();
        ui->tablePaths->insertRow(row);
        ui->tablePaths->setItem(row, 0, makeCell(QString::number(pr.fid)));
        ui->tablePaths->setItem(row, 1, makeCell(QString::number(pr.srcNodeId)));
        ui->tablePaths->setItem(row, 2, makeCell(QString::number(pr.dstNodeId)));
        ui->tablePaths->setItem(row, 3, makeCell(
            pr.hops >= 0 ? QString::number(pr.hops) : "--"));

        QStringList nsAll;
        for (int n : pr.pathNodes) nsAll << QString::number(n);
        ui->tablePaths->setItem(row, 4, makeCell(
            nsAll.isEmpty() ? "--" : nsAll.join(" → "),
            Qt::AlignVCenter | Qt::AlignLeft));

        auto *satCell = makeCell(pr.isSatisfied ? "满足" : "未满足");
        satCell->setForeground(QBrush(QColor(pr.isSatisfied ? "#27AE60" : "#E74C3C")));
        ui->tablePaths->setItem(row, 5, satCell);
        ui->tablePaths->setItem(row, 6, makeCell(
            pr.unsatisfiedReason.isEmpty() ? "--" : pr.unsatisfiedReason,
            Qt::AlignVCenter | Qt::AlignLeft));

        // tableFlows（流 ID / src / dst / 需求带宽 / 实际带宽 / 途径节点）
        double reqMbps = planMap.contains(pr.fid)
                            ? planMap[pr.fid].rateBps / 1e6 : 0.0;
        double actMbps = pr.actualBandwidthBps / 1e6;
        int frow = ui->tableFlows->rowCount();
        ui->tableFlows->insertRow(frow);
        ui->tableFlows->setItem(frow, 0, makeCell(QString::number(pr.fid)));
        ui->tableFlows->setItem(frow, 1, makeCell(QString::number(pr.srcNodeId)));
        ui->tableFlows->setItem(frow, 2, makeCell(QString::number(pr.dstNodeId)));
        ui->tableFlows->setItem(frow, 3, makeCell(
            reqMbps > 0 ? QString("%1").arg(reqMbps, 0, 'f', 2) : "--"));
        ui->tableFlows->setItem(frow, 4, makeCell(
            pr.isSatisfied ? QString("%1").arg(actMbps, 0, 'f', 2) : "--"));
        ui->tableFlows->setItem(frow, 5, makeCell(
            nsAll.isEmpty() ? "--" : nsAll.join(" → "),
            Qt::AlignVCenter | Qt::AlignLeft));

        if (pr.isSatisfied && !pr.pathNodes.isEmpty()) {
            highlightPaths.append(pr.pathNodes);
            satisfiedCount++;
        }
    }

    // ── 3. 拓扑视图高亮（仅高亮成功路径） ───────────────────────────────────
    m_topoView->resetHighlight();
    if (!highlightPaths.isEmpty())
        m_topoView->highlightPaths(highlightPaths);

    // ── 4. 告警列表填充 ─────────────────────────────────────────────────────
    if (m_warnModel) m_warnModel->clear();
    if (m_warnModel) {
        for (const WarnItem &w : packet.warns) {
            auto *item = new QStandardItem(w.message);
            item->setData(w.level, Qt::UserRole);
            item->setData(QTime::currentTime().toString("hh:mm:ss"), Qt::UserRole + 1);
            item->setToolTip(QString("[%1] %2%3")
                .arg(w.level)
                .arg(w.warnType.isEmpty() ? "" : (w.warnType + " — "))
                .arg(w.message));
            item->setEditable(false);
            m_warnModel->appendRow(item);
        }
        ui->warnListView->scrollToBottom();
    }

    // ── 5. 顶部统计标签 ─────────────────────────────────────────────────────
    ui->lblPlanPathValue->setText(
        QString("%1/%2 条").arg(satisfiedCount).arg(packet.planResults.size()));
    if (m_simStartTime.isValid()) {
        double elapsed = m_simStartTime.msecsTo(QDateTime::currentDateTime()) / 1000.0;
        ui->lblCalcTimeValue->setText(QString("%1 s").arg(elapsed, 0, 'f', 1));
    }

    // v2.0 不再提供 delay/loss/bw_utilization 字段，相关标签维持 "--"
//    ui->lblAvgDelayValue->setText("--");
//    ui->lblPktLossValue->setText("--");
//    ui->lblBwUsageValue->setText("--");

    // ── 6. 刷新地图、节点/链路列表（链路已更新） ────────────────────────────
    if (m_planSceneId == m_currentSceneId)
        onSceneChanged(m_currentSceneId);

    postStatusLog(QString("规划完成：%1/%2 条流满足，%3 条链路")
                  .arg(satisfiedCount)
                  .arg(packet.planResults.size())
                  .arg(packet.links.size()));
}

void MainWindow::onSimError(QString msg)
{
    qDebug() << "[SimError]" << msg;
    postStatusLog("[规划错误] " + msg, true);
    QMessageBox::warning(this, "规划错误", msg);
    m_simRunning = false;
    ui->btnSimToggle->setText("启动");
    ui->btnSimToggle->setEnabled(true);
    if (m_planProgress) {
        m_planProgress->setVisible(false);
        m_planProgress->setRange(0, 100);
        m_planProgress->setValue(0);
    }
}

void MainWindow::onSimFinished()
{
    m_simRunning = false;
    ui->btnSimToggle->setText("启动");
    ui->btnSimToggle->setEnabled(true);
    if (m_planProgress) {
        m_planProgress->setVisible(false);
        m_planProgress->setRange(0, 100);
        m_planProgress->setValue(0);
    }

    if (m_simStartTime.isValid()) {
        double elapsed = m_simStartTime.msecsTo(QDateTime::currentDateTime()) / 1000.0;
        ui->lblCalcTimeValue->setText(QString("%1 s").arg(elapsed, 0, 'f', 1));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SimBridge 连接状态槽
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::onSimBridgeConnected()
{
    qDebug() << "[SimBridge] 已连接到" << m_simBridge->host() << ":" << m_simBridge->port();
    if (m_lblStatusPy) {
        m_lblStatusPy->setText(
            QString("Python: 已连接 (%1:%2)")
                .arg(m_simBridge->host()).arg(m_simBridge->port()));
        m_lblStatusPy->setStyleSheet("color:#27AE60; padding:0 10px;");
    }
    postStatusLog(QString("已连接到 Python 服务端 %1:%2")
                  .arg(m_simBridge->host()).arg(m_simBridge->port()));
}

void MainWindow::onSimBridgeDisconnected()
{
    if (m_lblStatusPy) {
        m_lblStatusPy->setText("Python: 未连接 (重连中…)");
        m_lblStatusPy->setStyleSheet("color:#E74C3C; padding:0 10px;");
    }
    if (m_simRunning) {
        m_simRunning = false;
        ui->btnSimToggle->setText("启动");
        ui->btnSimToggle->setEnabled(true);
        postStatusLog("Python 连接断开，仿真已中止", true);
    }
}

void MainWindow::onSimBridgeConnectionError(QString msg)
{
    Q_UNUSED(msg)
    if (m_lblStatusPy) {
        m_lblStatusPy->setText(
            QString("Python: 连接失败 (%1:%2)")
                .arg(m_simBridge->host()).arg(m_simBridge->port()));
        m_lblStatusPy->setStyleSheet("color:#E74C3C; padding:0 10px;");
    }
    postStatusLog(QString("无法连接 Python 服务端 %1:%2，等待重试…")
                  .arg(m_simBridge->host()).arg(m_simBridge->port()), true);
}

void MainWindow::onSceneTreeItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item) return;

    QVariant var = item->data(0, Qt::UserRole);
    if (!var.isValid()) return;

    QVariantMap map = var.toMap();
    QString type = map.value("type").toString();

    if (type == "node") {
        int dbId = map.value("dbId").toInt();
        for (const NodeInfo &n : m_currentNodes) {
            if (n.id == dbId) {
                DialogNode *dlg = new DialogNode(m_db, m_currentSceneId, n, this);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                connect(dlg, &DialogNode::nodeSaved, this, [this]{ onSceneChanged(m_currentSceneId); });
                dlg->exec();
                break;
            }
        }
    } else if (type == "device") {
        // 设备已并入节点配置弹窗，双击设备直接打开其所属节点的配置对话框
        int nodeDbId = map.value("nodeDbId").toInt();
        for (const NodeInfo &n : m_currentNodes) {
            if (n.id == nodeDbId) {
                DialogNode *dlg = new DialogNode(m_db, m_currentSceneId, n, this);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                connect(dlg, &DialogNode::nodeSaved, this, [this]{ onSceneChanged(m_currentSceneId); });
                dlg->exec();
                break;
            }
        }
    }
}

void MainWindow::onSceneTreeItemClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (item) {
        item->setExpanded(!item->isExpanded());
    }
}

void MainWindow::clearAndSeedDatabase()
{
    if (!m_dbOk) return;
    // ★ 只在单机模式播种。远端库是服务端管理的共享数据，
    //   客户端启动时把它清空会直接毁掉别人的场景。
    if (m_remoteMode || !m_localDb) {
        qWarning() << "[Data] 远端模式下跳过本地种子数据";
        return;
    }

    // 清空所有表数据
    m_localDb->execSQL("DELETE FROM scene_links");
    m_localDb->execSQL("DELETE FROM scene_nodes");
    m_localDb->execSQL("DELETE FROM scenes");
    m_localDb->execSQL("DELETE FROM node_templates");
    m_localDb->execSQL("DELETE FROM link_templates");

    // 设备类型 → 中文名（用于生成实例显示名）
    static const QMap<QString, QString> seedTypeCn = {
        {"fiber","光缆"}, {"fieldwire","野战电缆"}, {"adhoc","自组网"},
        {"narrowband","窄带电台"}, {"shortwave","短波"}, {"microwave","微波接力"},
        {"scatter","散射"}, {"cellular","蜂窝"}, {"satellite","卫星通信"}, {"switch","交换机"},
    };
    // 由设备类型列表构造多实例 key 列表 + 参数表（instance_id 全局自增、唯一）
    auto buildDevices = [&](const QStringList &types,
                            QStringList &keysOut,
                            QMap<QString, DeviceParams> &paramsOut) {
        keysOut.clear();
        paramsOut.clear();
        int gid = 0;
        for (const QString &t : types) {
            int id = gid++;
            QString key = QString("%1_%2").arg(t).arg(id, 2, 10, QChar('0'));
            DeviceParams dp;
            dp.instanceId     = id;
            dp.instanceName   = QString("%1_%2")
                                    .arg(seedTypeCn.value(t, t)).arg(id + 1, 2, 10, QChar('0'));
            dp.deviceRole     = (t == "switch") ? "switch" : "communication";
            dp.deviceCategory = (t == "fiber" || t == "fieldwire") ? "wired" : "wireless";
            keysOut << key;
            paramsOut.insert(key, dp);
        }
    };

    // 1. 添加节点模板
    NodeTemplate nt1;
    nt1.name = "干线节点模板";
    nt1.nodeType = "干线";
    buildDevices({"fiber", "microwave", "satellite", "switch"},
                 nt1.commMethods, nt1.deviceParams);
    nt1.defaultInterferenceDb = 0.0;
    nt1.description = "干线网络核心节点模板";
    m_db->saveNodeTemplate(nt1);

    NodeTemplate nt2;
    nt2.name = "支线节点模板";
    nt2.nodeType = "支线";
    buildDevices({"adhoc", "narrowband", "shortwave"},
                 nt2.commMethods, nt2.deviceParams);
    nt2.defaultInterferenceDb = 0.0;
    nt2.description = "末端支线接入电台模板";
    m_db->saveNodeTemplate(nt2);

    // 2. 添加链路模板
    LinkTemplate lt1;
    lt1.name = "光缆有线模板";
    lt1.linkType = "wired";
    lt1.bandwidthBps = 1000e6;
    lt1.description = "高速光缆骨干链路";
    m_db->saveLinkTemplate(lt1);

    LinkTemplate lt2;
    lt2.name = "微波无线模板";
    lt2.linkType = "wireless";
    lt2.wirelessType = "微波接力";
    lt2.bandwidthBps = 100e6;
    lt2.description = "中程高速无线接力";
    m_db->saveLinkTemplate(lt2);

    LinkTemplate lt3;
    lt3.name = "自组网无线模板";
    lt3.linkType = "wireless";
    lt3.wirelessType = "自组网电台";
    lt3.bandwidthBps = 50e6;
    lt3.description = "战术宽带自组网";
    m_db->saveLinkTemplate(lt3);

    // 3. 创建默认场景
    SceneInfo sc;
    sc.name = "通信规划演练场景";
    sc.sceneType = "骨干-接入混合网络";
    sc.description = "用于测试支线/干线节点和多链路配置的演示场景";
    int sceneId = m_db->createScene(sc);

    if (sceneId > 0) {
        // 4. 在场景中创建5个节点
        // 节点 1 (干线)
        NodeInfo n1;
        n1.sceneId = sceneId;
        n1.nodeId = 1;
        n1.name = "指挥所干线车1";
        n1.nodeType = "干线";
        n1.status = "在线";
        n1.longitude = 116.397;
        n1.latitude = 39.908;
        n1.altitude = 50.0;
        buildDevices({"fiber", "microwave", "satellite", "switch"},
                     n1.commMethods, n1.deviceParams);
        // 节点内连接：交换机汇聚各通信设备
        n1.deviceConnections = {
            {"switch_03", "fiber_00"},
            {"switch_03", "microwave_01"},
            {"switch_03", "satellite_02"},
        };
        m_db->addNode(n1);

        // 节点 2 (支线)
        NodeInfo n2;
        n2.sceneId = sceneId;
        n2.nodeId = 2;
        n2.name = "前沿支线车2";
        n2.nodeType = "支线";
        n2.status = "在线";
        n2.longitude = 116.417;
        n2.latitude = 39.928;
        n2.altitude = 30.0;
        buildDevices({"adhoc", "narrowband"}, n2.commMethods, n2.deviceParams);
        m_db->addNode(n2);

        // 节点 3 (支线)
        NodeInfo n3;
        n3.sceneId = sceneId;
        n3.nodeId = 3;
        n3.name = "前沿支线车3";
        n3.nodeType = "支线";
        n3.status = "在线";
        n3.longitude = 116.377;
        n3.latitude = 39.888;
        n3.altitude = 35.0;
        buildDevices({"adhoc", "narrowband"}, n3.commMethods, n3.deviceParams);
        m_db->addNode(n3);

        // 节点 4 (干线)
        NodeInfo n4;
        n4.sceneId = sceneId;
        n4.nodeId = 4;
        n4.name = "机动指挥车4";
        n4.nodeType = "干线";
        n4.status = "在线";
        n4.longitude = 116.357;
        n4.latitude = 39.948;
        n4.altitude = 55.0;
        buildDevices({"fiber", "microwave", "satellite", "switch"},
                     n4.commMethods, n4.deviceParams);
        n4.deviceConnections = {
            {"switch_03", "fiber_00"},
            {"switch_03", "microwave_01"},
            {"switch_03", "satellite_02"},
        };
        m_db->addNode(n4);

        // 节点 5 (支线)
        NodeInfo n5;
        n5.sceneId = sceneId;
        n5.nodeId = 5;
        n5.name = "巡逻支线车5";
        n5.nodeType = "支线";
        n5.status = "告警";
        n5.longitude = 116.437;
        n5.latitude = 39.868;
        n5.altitude = 25.0;
        buildDevices({"adhoc", "narrowband", "shortwave"},
                     n5.commMethods, n5.deviceParams);
        m_db->addNode(n5);
    }
}

