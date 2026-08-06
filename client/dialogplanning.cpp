#include "dialogplanning.h"
#include "ui_dialogplanning.h"
#include "tableutils.h"

#include <QMessageBox>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QPushButton>
#include <QWidget>
#include <QHBoxLayout>

// ─── constructor ──────────────────────────────────────────────────────────────

DialogPlanning::DialogPlanning(IDataStore *db, int currentSceneId, QWidget *parent)
    : FramelessDialog(parent)
    , ui(new Ui::DialogPlanning)
    , m_db(db)
{
    ui->setupUi(this);
    setWindowTitle("通信规划配置");

    // ── tableFlows 列头（采用统一的 styleDataTable 样式和自适应逻辑）──
    ui->tableFlows->setColumnCount(6);
    ui->tableFlows->setHorizontalHeaderLabels(
        {"流ID", "源节点", "目的节点", "带宽需求(Mbps)", "到达时间(s)", "持续时长(s)"});
    styleDataTable(ui->tableFlows);

    // ── tableExclusionList 列头 ──
    ui->tableExclusionList->setColumnCount(3);
    ui->tableExclusionList->setHorizontalHeaderLabels({"节点 A", "节点 B", "操作"});
    // 前两列等宽自适应，操作列固定宽度
    ui->tableExclusionList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->tableExclusionList->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableExclusionList->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->tableExclusionList->setColumnWidth(2, 80);
    ui->tableExclusionList->horizontalHeader()->setHighlightSections(false);
    ui->tableExclusionList->horizontalHeader()->setFixedHeight(28);
    ui->tableExclusionList->verticalHeader()->setVisible(false);
    ui->tableExclusionList->verticalHeader()->setDefaultSectionSize(36);  // 行高足够显示按钮
    ui->tableExclusionList->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableExclusionList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tableExclusionList->setShowGrid(true);
    ui->tableExclusionList->setStyleSheet(kDataTableStyleSheet);

    // ── 其他控件初始化 ──
    ui->sbKPaths->setMinimum(1);
    ui->sbKPaths->setMaximum(10);
    ui->sbKPaths->setValue(3);

    if (ui->cmbOpt->count() == 0) {
        ui->cmbOpt->addItem("性能优先（最小化传输时延）",  "performance");
        ui->cmbOpt->addItem("资源均衡（最小化链路负载）",  "resource");
        ui->cmbOpt->addItem("最小变更（减少现有链路影响）", "min_change");
    }
    ui->cmbOpt->setCurrentIndex(0);

    // ── 加载当前场景节点（场景已锁定，不再提供切换入口）──
    m_sceneId = currentSceneId;
    loadNodesForScene(m_sceneId);

    // ── 连接信号 ──
    connect(ui->tableFlows, &QTableWidget::itemSelectionChanged,
            this, &DialogPlanning::onTableFlowsSelectionChanged);
    connect(ui->cmbSrcNode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DialogPlanning::onNodesChanged);
    connect(ui->cmbDstNode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DialogPlanning::onNodesChanged);
}

DialogPlanning::~DialogPlanning()
{
    delete ui;
}

// ─── private helpers ──────────────────────────────────────────────────────────

void DialogPlanning::loadNodesForScene(int sceneId)
{
    QSignalBlocker b1(ui->cmbSrcNode), b2(ui->cmbDstNode);
    QSignalBlocker b3(ui->cmbExcludeNodeA), b4(ui->cmbExcludeNodeB);

    ui->cmbSrcNode->clear();
    ui->cmbDstNode->clear();
    ui->cmbExcludeNodeA->clear();
    ui->cmbExcludeNodeB->clear();
    m_nodes.clear();

    if (sceneId < 0) return;

    m_nodes = m_db->getNodesByScene(sceneId);
    for (const NodeInfo &n : m_nodes) {
        if (n.status == "离线") continue;
        QString label = QString("[%1] %2").arg(n.nodeId).arg(n.name);
        ui->cmbSrcNode->addItem(label, n.nodeId);
        ui->cmbDstNode->addItem(label, n.nodeId);
        ui->cmbExcludeNodeA->addItem(label, n.nodeId);
        ui->cmbExcludeNodeB->addItem(label, n.nodeId);
    }

    if (ui->cmbDstNode->count() > 1)
        ui->cmbDstNode->setCurrentIndex(1);

    autoFillFlowParams();
}

QString DialogPlanning::nodeIdToName(int nodeId) const
{
    for (const NodeInfo &n : m_nodes)
        if (n.nodeId == nodeId)
            return n.name.isEmpty() ? QString::number(nodeId) : n.name;
    return QString::number(nodeId);
}

// ─── 自动推算合理默认参数 ─────────────────────────────────────────────────────

void DialogPlanning::autoFillFlowParams()
{
    if (m_sceneId < 0) return;

    int srcId = ui->cmbSrcNode->currentData().toInt();
    int dstId = ui->cmbDstNode->currentData().toInt();
    if (srcId == dstId || ui->cmbSrcNode->count() == 0) return;

    NodeInfo srcNode, dstNode;
    bool foundSrc = false, foundDst = false;
    for (const NodeInfo &n : m_nodes) {
        if (n.nodeId == srcId) { srcNode = n; foundSrc = true; }
        if (n.nodeId == dstId) { dstNode = n; foundDst = true; }
    }
    if (!foundSrc || !foundDst) return;

    // 查找源/目之间的直连链路，取最大带宽
    const QList<LinkInfo> links = m_db->getLinksByScene(m_sceneId);
    double maxBw = 0.0;
    bool   directLink = false;
    for (const LinkInfo &l : links) {
        if ((l.src == srcId && l.dst == dstId) ||
            (l.src == dstId && l.dst == srcId)) {
            if (l.bandwidthBps > maxBw) maxBw = l.bandwidthBps;
            directLink = true;
        }
    }

    double suggestBps;
    if (directLink) {
        suggestBps = maxBw * 0.30;
    } else {
        double total = 0.0; int cnt = 0;
        for (const LinkInfo &l : links) { total += l.bandwidthBps; cnt++; }
        suggestBps = cnt > 0 ? total / cnt * 0.20 : 8e6;
    }
    suggestBps = qMax(suggestBps, 1e6);
    ui->dsbBandwidth->setValue(suggestBps / 1e6);

    auto typePriority = [](const QString &t) -> int {
        if (t == "干线" || t.contains("干线")) return 0;
        if (t == "支线" || t.contains("支线")) return 1;
        if (t == "一级ZK" || t == "二级ZK")   return 0;
        if (t == "RD节点")                      return 1;
        return 2;
    };
    int qos = qMin(typePriority(srcNode.nodeType), typePriority(dstNode.nodeType));
    ui->cmbQosLevel->setCurrentIndex(qos);
    ui->cmbOpt->setCurrentIndex(qos);

    ui->dsbFlowArrivalTime->setValue(m_flows.size() * 5.0);
    ui->dsbFlowDuration->setValue(30.0);
}

FlowInfo DialogPlanning::collectFormData() const
{
    FlowInfo f;
    f.src      = ui->cmbSrcNode->currentData().toInt();
    f.dst      = ui->cmbDstNode->currentData().toInt();
    f.rateBps  = ui->dsbBandwidth->value() * 1e6;
    f.arrivalT = ui->dsbFlowArrivalTime->value();
    f.durT     = ui->dsbFlowDuration->value();
    f.qosLevel = ui->cmbQosLevel->currentIndex();
    f.kPaths   = ui->sbKPaths->value();
    static const QStringList OPT_KEYS = {"performance", "resource", "min_change"};
    f.optTarget = OPT_KEYS.value(ui->cmbOpt->currentIndex(), "performance");
    return f;
}

void DialogPlanning::rebuildFlowTable()
{
    ui->tableFlows->setRowCount(0);
    for (const FlowInfo &f : m_flows) {
        int row = ui->tableFlows->rowCount();
        ui->tableFlows->insertRow(row);
        ui->tableFlows->setItem(row, 0, new QTableWidgetItem(QString::number(f.fid)));
        ui->tableFlows->setItem(row, 1, new QTableWidgetItem(nodeIdToName(f.src)));
        ui->tableFlows->setItem(row, 2, new QTableWidgetItem(nodeIdToName(f.dst)));
        ui->tableFlows->setItem(row, 3, new QTableWidgetItem(
            QString::number(f.rateBps / 1e6, 'f', 1)));
        ui->tableFlows->setItem(row, 4, new QTableWidgetItem(
            QString::number(f.arrivalT, 'f', 1)));
        ui->tableFlows->setItem(row, 5, new QTableWidgetItem(
            QString::number(f.durT, 'f', 1)));
    }
}

void DialogPlanning::rebuildExclusionTable()
{
    ui->tableExclusionList->setRowCount(0);
    for (int i = 0; i < m_exclusions.size(); ++i) {
        const QPair<int,int> &ex = m_exclusions[i];
        int row = ui->tableExclusionList->rowCount();
        ui->tableExclusionList->insertRow(row);
        ui->tableExclusionList->setItem(row, 0,
            new QTableWidgetItem(nodeIdToName(ex.first)));
        ui->tableExclusionList->setItem(row, 1,
            new QTableWidgetItem(nodeIdToName(ex.second)));

        // 删除按钮（在单元格内居中，尺寸适配行高）
        QPushButton *btnDel = new QPushButton("删除");
        btnDel->setProperty("exclusionIndex", i);
        btnDel->setFixedSize(60, 26);
        btnDel->setCursor(Qt::PointingHandCursor);
        btnDel->setStyleSheet(
            "QPushButton { background:#FDECEA; color:#C62828; border:1px solid #EF9A9A;"
            " border-radius:4px; font-size:12px; padding:0px; min-height:0px; min-width:0px; }"
            "QPushButton:hover { background:#FFCDD2; border-color:#E53935; color:#C62828; font-size:12px; }");
        connect(btnDel, &QPushButton::clicked, this, [this, btnDel]() {
            int idx = btnDel->property("exclusionIndex").toInt();
            if (idx >= 0 && idx < m_exclusions.size()) {
                m_exclusions.removeAt(idx);
                rebuildExclusionTable();
            }
        });
        QWidget *cell = new QWidget;
        QHBoxLayout *hl = new QHBoxLayout(cell);
        hl->setContentsMargins(2, 2, 2, 2);
        hl->setAlignment(Qt::AlignCenter);
        hl->addWidget(btnDel);
        ui->tableExclusionList->setCellWidget(row, 2, cell);
    }
}

void DialogPlanning::fillFormFromFlow(const FlowInfo &f)
{
    for (int i = 0; i < ui->cmbSrcNode->count(); ++i)
        if (ui->cmbSrcNode->itemData(i).toInt() == f.src) {
            ui->cmbSrcNode->setCurrentIndex(i); break;
        }
    for (int i = 0; i < ui->cmbDstNode->count(); ++i)
        if (ui->cmbDstNode->itemData(i).toInt() == f.dst) {
            ui->cmbDstNode->setCurrentIndex(i); break;
        }
    ui->dsbBandwidth->setValue(f.rateBps / 1e6);
    ui->dsbFlowArrivalTime->setValue(f.arrivalT);
    ui->dsbFlowDuration->setValue(f.durT);
    ui->cmbQosLevel->setCurrentIndex(f.qosLevel);
    ui->sbKPaths->setValue(f.kPaths);
    static const QStringList OPT_KEYS = {"performance", "resource", "min_change"};
    int optIdx = OPT_KEYS.indexOf(f.optTarget);
    ui->cmbOpt->setCurrentIndex(optIdx >= 0 ? optIdx : 0);
}

// ─── slots ────────────────────────────────────────────────────────────────────

void DialogPlanning::onNodesChanged()
{
    if (m_editingFid < 0)
        autoFillFlowParams();
}

void DialogPlanning::on_btnAddFlow_clicked()
{
    if (m_sceneId < 0) {
        QMessageBox::warning(this, "提示", "当前无规划场景");
        return;
    }
    if (ui->cmbSrcNode->count() == 0 || ui->cmbDstNode->count() == 0) {
        QMessageBox::warning(this, "提示", "当前场景无可用节点");
        return;
    }
    int src = ui->cmbSrcNode->currentData().toInt();
    int dst = ui->cmbDstNode->currentData().toInt();
    if (src == dst) {
        QMessageBox::warning(this, "提示", "源节点和目的节点不能相同");
        return;
    }

    FlowInfo f = collectFormData();

    if (m_editingFid >= 0) {
        for (FlowInfo &existing : m_flows) {
            if (existing.fid == m_editingFid) {
                f.fid    = m_editingFid;
                existing = f;
                break;
            }
        }
        m_editingFid = -1;
    } else {
        f.fid = m_flows.isEmpty() ? 0 : m_flows.last().fid + 1;
        m_flows.append(f);
    }

    rebuildFlowTable();

    if (ui->cmbSrcNode->count() > 0) ui->cmbSrcNode->setCurrentIndex(0);
    if (ui->cmbDstNode->count() > 1) ui->cmbDstNode->setCurrentIndex(1);
}

void DialogPlanning::onTableFlowsSelectionChanged()
{
    QList<QTableWidgetItem*> sel = ui->tableFlows->selectedItems();
    if (sel.isEmpty()) { m_editingFid = -1; return; }

    int row = ui->tableFlows->row(sel.first());
    if (row >= 0 && row < m_flows.size()) {
        m_editingFid = m_flows[row].fid;
        fillFormFromFlow(m_flows[row]);
    }
}

void DialogPlanning::on_btnAddExclusion_clicked()
{
    int nodeA = ui->cmbExcludeNodeA->currentData().toInt();
    int nodeB = ui->cmbExcludeNodeB->currentData().toInt();

    if (nodeA == nodeB) {
        QMessageBox::warning(this, "提示", "禁连规则的两个节点不能相同");
        return;
    }

    // 检查是否重复
    for (const QPair<int,int> &ex : m_exclusions) {
        if ((ex.first == nodeA && ex.second == nodeB) ||
            (ex.first == nodeB && ex.second == nodeA)) {
            QMessageBox::information(this, "提示", "该禁连规则已存在");
            return;
        }
    }

    m_exclusions.append({nodeA, nodeB});
    rebuildExclusionTable();
}

void DialogPlanning::on_btnSavePlan_clicked()
{
    if (m_flows.isEmpty()) {
        QMessageBox::warning(this, "提示", "请至少添加一条通信流");
        return;
    }
    if (m_sceneId < 0) {
        QMessageBox::warning(this, "提示", "无效的规划场景");
        return;
    }
    emit planSaved(m_sceneId, m_flows, m_exclusions);
    accept();
}

void DialogPlanning::on_btnCancel_clicked()
{
    reject();
}
