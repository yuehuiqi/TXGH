#include "dialognode.h"
#include "ui_dialognode.h"
#include "devicetopologyview.h"

#include <QGridLayout>
#include <QTabBar>
#include <QHeaderView>
#include <QMessageBox>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidgetItem>
#include <QBrush>
#include <QColor>

// ─── 静态映射表：设备类型 中文名 ↔ 英文 key 前缀 ──────────────────────────────
// 格式：中文显示名 → 英文 key 前缀（用于生成 instance_key 如 "fiber_00"）
const QMap<QString, QString> DialogNode::s_typeKeyMap = {
    {"光缆",       "fiber"},
    {"野战线",     "fieldwire"},
    {"自组网",     "adhoc"},
    {"短波",       "shortwave"},
    {"微波接力",   "microwave"},
    {"散射",       "scatter"},
    {"蜂窝",       "cellular"},
    {"卫星通信",   "satellite"},
    {"交换机",     "switch"},
};

const QMap<QString, QString> DialogNode::s_typeNameMap = {
    {"fiber",     "光缆"},
    {"fieldwire", "野战线"},
    {"adhoc",     "自组网"},
    {"shortwave", "短波"},
    {"microwave", "微波接力"},
    {"scatter",   "散射"},
    {"cellular",  "蜂窝"},
    {"satellite", "卫星通信"},
    {"switch",    "交换机"},
};

// ─── 辅助函数 ─────────────────────────────────────────────────────────────────

QString DialogNode::generateInstanceKey(const QString &type, int id) const
{
    // 英文前缀 + 两位序号，如 "fiber_00"、"switch_01"
    return QString("%1_%2").arg(type).arg(id, 2, 10, QChar('0'));
}

QString DialogNode::generateInstanceName(const QString &type, int id) const
{
    // 中文名 + 两位序号，如 "光缆_01"（从 01 开始显示）
    QString cnName = s_typeNameMap.value(type, type);
    return QString("%1_%2").arg(cnName).arg(id + 1, 2, 10, QChar('0'));
}

int DialogNode::nextInstanceId() const
{
    // 在当前所有实例 key 中找到最大 id + 1
    int maxId = -1;
    for (const QString &key : m_deviceKeys) {
        int idx = key.lastIndexOf('_');
        if (idx >= 0) {
            bool ok;
            int id = key.mid(idx + 1).toInt(&ok);
            if (ok && id > maxId) maxId = id;
        }
    }
    return maxId + 1;
}

QStringList DialogNode::activeDeviceKeys() const
{
    return m_deviceKeys;
}

// ─── 新建 ─────────────────────────────────────────────────────────────────────
DialogNode::DialogNode(IDataStore *db, int sceneId, QWidget *parent)
    : FramelessDialog(parent)
    , ui(new Ui::DialogNode)
    , m_db(db)
    , m_sceneId(sceneId)
    , m_editDbId(-1)
    , m_loadingParams(false)
{
    ui->setupUi(this);
    setWindowTitle("新建节点");

    initDeviceTypeCombo();
    initTopologyView();
    loadTemplates();

    connect(ui->tableDeviceMatrix, &QTableWidget::cellChanged,
            this, &DialogNode::onDeviceMatrixCellChanged);
}

// ─── 拖放预填（新建，带初始经纬度和类型）────────────────────────────────────
DialogNode::DialogNode(IDataStore *db, int sceneId, double lon, double lat,
                       const QString &nodeType, QWidget *parent)
    : DialogNode(db, sceneId, parent)
{
    ui->dsbNodeLongitude->setValue(lon);
    ui->dsbNodeLatitude->setValue(lat);
    int idx = ui->cmbNodeType->findText(nodeType, Qt::MatchContains);
    if (idx >= 0) ui->cmbNodeType->setCurrentIndex(idx);
}

// ─── 编辑 ─────────────────────────────────────────────────────────────────────
DialogNode::DialogNode(IDataStore *db, int sceneId, const NodeInfo &node,
                       QWidget *parent)
    : FramelessDialog(parent)
    , ui(new Ui::DialogNode)
    , m_db(db)
    , m_sceneId(sceneId)
    , m_editDbId(node.id)
    , m_loadingParams(false)
{
    ui->setupUi(this);
    setWindowTitle("编辑节点");

    initDeviceTypeCombo();
    initTopologyView();
    loadTemplates();

    connect(ui->tableDeviceMatrix, &QTableWidget::cellChanged,
            this, &DialogNode::onDeviceMatrixCellChanged);

    // ── 预填基础属性 ──
    ui->leNodeId->setText(QString::number(node.nodeId));
    ui->leNodeName->setText(node.name);

    int typeIdx = ui->cmbNodeType->findText(node.nodeType);
    if (typeIdx >= 0) ui->cmbNodeType->setCurrentIndex(typeIdx);

    int statusIdx = ui->cmbNodeStatus->findText(node.status);
    if (statusIdx >= 0) ui->cmbNodeStatus->setCurrentIndex(statusIdx);

    ui->dsbNodeLongitude->setValue(node.longitude);
    ui->dsbNodeLatitude->setValue(node.latitude);
    ui->dsbNodeAltitude->setValue(node.altitude);
    ui->dsbInterferenceDb->setValue(node.interferenceDb);

    // ── 还原设备实例 ──
    m_deviceKeys   = node.commMethods;
    m_deviceParams = node.deviceParams;
    m_deviceConns  = node.deviceConnections;

    refreshDeviceList();

    if (!m_deviceKeys.isEmpty()) {
        ui->listDeviceInstances->setCurrentRow(0);
    }

    // ── 还原连接矩阵与拓扑 ──
    initDeviceMatrix();
    refreshTopology();
}

DialogNode::~DialogNode()
{
    delete ui;
}

// ─── 标签页外观：标题加宽、左右均匀铺满 ───────────────────────────────────────
void DialogNode::initTabBarStyle()
{
    // setExpanding(true)：四个标签按等分宽度铺满整个标签栏，实现左右均匀分布
    ui->tabWidget->tabBar()->setExpanding(true);
    ui->tabWidget->tabBar()->setUsesScrollButtons(false);
    ui->tabWidget->setStyleSheet(
        "QTabBar::tab{ min-width:120px; padding:10px 18px; margin-right:2px;"
        " font-size:16px; font-weight:600; }"
        "QTabWidget::tab-bar{ alignment:center; }");
}

// ─── 初始化设备类型下拉框 ─────────────────────────────────────────────────────
void DialogNode::initDeviceTypeCombo()
{
    initTabBarStyle();

    ui->cmbDeviceType->clear();
    for (auto it = s_typeKeyMap.constBegin(); it != s_typeKeyMap.constEnd(); ++it) {
        ui->cmbDeviceType->addItem(it.key(), it.value());  // text=中文, userData=英文key
    }

    // 初始化 DeviceRole 和 DeviceCategory 下拉选项
    if (ui->cmbDeviceRole->count() == 0) {
        ui->cmbDeviceRole->addItem("通信设备", "communication");
        ui->cmbDeviceRole->addItem("交换设备", "switch");
    }
    if (ui->cmbDeviceCategory->count() == 0) {
        ui->cmbDeviceCategory->addItem("无线 (wireless)", "wireless");
        ui->cmbDeviceCategory->addItem("有线 (wired)", "wired");
    }

    // 初始化 输入输出（设备流向）下拉：普通 / 输入端 / 输出端
    {
        QSignalBlocker b(ui->cmbInputOutput);
        ui->cmbInputOutput->clear();
        ui->cmbInputOutput->addItem("普通", "normal");
        ui->cmbInputOutput->addItem("输入", "input");
        ui->cmbInputOutput->addItem("输出", "output");
    }

    // 最大连接数：0~99，默认 4（限制节点内设备间的连线数量）
    ui->sbMaxDevice->setRange(0, 99);
    ui->sbMaxDevice->setValue(4);

    // 初始化 stackedWidget 的速率/背板单位
    ui->dsbMaxBandwidth->setSuffix(" Mbps");
    ui->dsbMaxBandwidth->setRange(0.1, 100000.0);
    ui->dsbMaxBandwidth->setValue(100.0);
    ui->dsbBackplaneBandwidth->setSuffix(" Mbps");
    ui->dsbBackplaneBandwidth->setRange(0.1, 1000000.0);
    ui->dsbBackplaneBandwidth->setValue(1000.0);
    ui->dsbProcessingDelay->setSuffix(" μs");
    ui->dsbProcessingDelay->setRange(0.0, 10000.0);
    ui->dsbProcessingDelay->setValue(50.0);

    // 设备高度（所有设备通用）
    ui->dsbDeviceHeight->setSuffix(" m");
    ui->dsbDeviceHeight->setDecimals(2);
    ui->dsbDeviceHeight->setRange(0.0, 10000.0);
    ui->dsbDeviceHeight->setValue(0.0);

    // 有线设备专用参数的单位与范围
    ui->dsbWiredTxPower->setSuffix(" dBm");
    ui->dsbWiredTxPower->setDecimals(1);
    ui->dsbWiredTxPower->setRange(-50.0, 60.0);
    ui->dsbWiredTxPower->setValue(0.0);
    ui->dsbWiredRxSensitivity->setSuffix(" dBm");
    ui->dsbWiredRxSensitivity->setDecimals(1);
    ui->dsbWiredRxSensitivity->setRange(-150.0, 0.0);
    ui->dsbWiredRxSensitivity->setValue(-20.0);
    ui->dsbFiberAttenuation->setSuffix(" dB/km");
    ui->dsbFiberAttenuation->setDecimals(2);
    ui->dsbFiberAttenuation->setRange(0.0, 100.0);
    ui->dsbFiberAttenuation->setValue(0.2);
    ui->dsbConnectorLoss->setSuffix(" dB");
    ui->dsbConnectorLoss->setDecimals(2);
    ui->dsbConnectorLoss->setRange(0.0, 100.0);
    ui->dsbConnectorLoss->setValue(1.0);
}

// ─── 刷新设备实例列表 ──────────────────────────────────────────────────────────
void DialogNode::refreshDeviceList()
{
    QSignalBlocker b(ui->listDeviceInstances);
    ui->listDeviceInstances->clear();
    for (const QString &key : m_deviceKeys) {
        const DeviceParams &dp = m_deviceParams.value(key);
        QString displayName = dp.instanceName.isEmpty()
                              ? generateInstanceName(key.left(key.lastIndexOf('_')), dp.instanceId)
                              : dp.instanceName;
        ui->listDeviceInstances->addItem(displayName);
    }
}

// ─── 将选中设备的参数填入右侧 UI ──────────────────────────────────────────────
void DialogNode::loadDeviceParamsToUI(const QString &key)
{
    if (!m_deviceParams.contains(key)) return;

    m_loadingParams = true;
    const DeviceParams &dp = m_deviceParams[key];

    // 公共参数
    ui->leDeviceName->setText(dp.instanceName);

    int roleIdx = ui->cmbDeviceRole->findData(dp.deviceRole);
    if (roleIdx >= 0) ui->cmbDeviceRole->setCurrentIndex(roleIdx);

    int catIdx = ui->cmbDeviceCategory->findData(dp.deviceCategory);
    if (catIdx >= 0) ui->cmbDeviceCategory->setCurrentIndex(catIdx);

    int ioIdx = ui->cmbInputOutput->findData(dp.ioRole);
    if (ioIdx >= 0) ui->cmbInputOutput->setCurrentIndex(ioIdx);

    ui->sbMaxDevice->setValue(dp.maxConnections);

    ui->dsbDeviceHeight->setValue(dp.deviceHeightM);
    ui->dsbMaxBandwidth->setValue(dp.maxBandwidthBps / 1e6);
    ui->dsbBackplaneBandwidth->setValue(dp.backplaneBandwidthBps / 1e6);
    ui->dsbProcessingDelay->setValue(dp.processingDelayUs);

    // 无线参数
    ui->dsbFreqHz->setValue(dp.freqHz);
    ui->dsbTxPowerDbm->setValue(dp.txPowerDbm);
    ui->dsbRxSensitivity->setValue(dp.rxSensitivityDbm);
    ui->dsbTxAntennaGain->setValue(dp.txAntennaGainDbi);
    ui->dsbRxAntennaGain->setValue(dp.rxAntennaGainDbi);
    ui->dsbNoiseFigure->setValue(dp.noiseFigureDb);
    ui->dsbSnrThreshold->setValue(dp.snrThresholdDb);
    ui->dsbPathLossExp->setValue(dp.pathLossExponent);
    ui->dsbExtraLoss->setValue(dp.additionalLossDb);

    // 有线参数
    ui->dsbWiredTxPower->setValue(dp.txPowerDbm);
    ui->dsbWiredRxSensitivity->setValue(dp.rxSensitivityDbm);
    ui->dsbFiberAttenuation->setValue(dp.fiberAttenuationDbPerKm);
    ui->dsbConnectorLoss->setValue(dp.connectorLossDb);
    ui->leCommProtocol->setText(dp.commProtocol);
    ui->leDeviceType->setText(dp.deviceType);

    // 更新 StackedWidget 页面可见性
    updateParamPanelVisibility();

    m_loadingParams = false;
}

// ─── 将右侧 UI 的参数保存回内存 ───────────────────────────────────────────────
void DialogNode::saveCurrentDeviceParamsFromUI()
{
    if (m_loadingParams || m_currentKey.isEmpty()) return;
    if (!m_deviceParams.contains(m_currentKey)) return;

    DeviceParams &dp = m_deviceParams[m_currentKey];

    dp.instanceName         = ui->leDeviceName->text().trimmed();
    dp.deviceRole           = ui->cmbDeviceRole->currentData().toString();
    dp.deviceCategory       = ui->cmbDeviceCategory->currentData().toString();
    dp.ioRole               = ui->cmbInputOutput->currentData().toString();
    dp.maxConnections       = ui->sbMaxDevice->value();
    dp.deviceHeightM        = ui->dsbDeviceHeight->value();
    dp.maxBandwidthBps      = ui->dsbMaxBandwidth->value() * 1e6;
    dp.backplaneBandwidthBps = ui->dsbBackplaneBandwidth->value() * 1e6;
    dp.processingDelayUs    = ui->dsbProcessingDelay->value();

    if (dp.deviceCategory == "wireless") {
        dp.freqHz             = ui->dsbFreqHz->value();
        dp.txPowerDbm         = ui->dsbTxPowerDbm->value();
        dp.rxSensitivityDbm   = ui->dsbRxSensitivity->value();
        dp.txAntennaGainDbi   = ui->dsbTxAntennaGain->value();
        dp.rxAntennaGainDbi   = ui->dsbRxAntennaGain->value();
        dp.noiseFigureDb      = ui->dsbNoiseFigure->value();
        dp.snrThresholdDb     = ui->dsbSnrThreshold->value();
        dp.pathLossExponent   = ui->dsbPathLossExp->value();
        dp.additionalLossDb   = ui->dsbExtraLoss->value();
    } else {
        dp.txPowerDbm           = ui->dsbWiredTxPower->value();
        dp.rxSensitivityDbm     = ui->dsbWiredRxSensitivity->value();
        dp.fiberAttenuationDbPerKm = ui->dsbFiberAttenuation->value();
        dp.connectorLossDb      = ui->dsbConnectorLoss->value();
        dp.commProtocol         = ui->leCommProtocol->text().trimmed();
        dp.deviceType           = ui->leDeviceType->text().trimmed();
    }

    // 同步更新列表显示名称
    int row = m_deviceKeys.indexOf(m_currentKey);
    if (row >= 0) {
        QString displayName = dp.instanceName.isEmpty()
                              ? generateInstanceName(
                                    m_currentKey.left(m_currentKey.lastIndexOf('_')),
                                    dp.instanceId)
                              : dp.instanceName;
        QSignalBlocker b(ui->listDeviceInstances);
        if (ui->listDeviceInstances->item(row))
            ui->listDeviceInstances->item(row)->setText(displayName);
    }
}

// ─── 更新参数面板显隐（角色 + 类别联动）──────────────────────────────────────
void DialogNode::updateParamPanelVisibility()
{
    QString role     = ui->cmbDeviceRole->currentData().toString();
    QString category = ui->cmbDeviceCategory->currentData().toString();

    bool isSwitch = (role == "switch");
    bool isWired  = (category == "wired");

    // 交换机 / 交换设备：右侧参数面板（无线 & 有线两页）一律不显示。
    // 触发条件：cmbDeviceType 当前类型为交换机(switch) 或 cmbDeviceRole 为交换设备(switch)。
    bool isSwitchDevice = isSwitch
                          || (deviceTypeOfKey(m_currentKey) == "switch");

    if (isSwitchDevice) {
        // 交换机/交换设备不配置无线/有线参数，整张 stackedWidget 隐藏
        ui->stackedWidget->hide();
    } else {
        // 非交换设备：根据 cmbDeviceCategory 有线/无线决定显示哪一页
        ui->stackedWidget->show();
        if (isWired) {
            ui->stackedWidget->setCurrentWidget(ui->pageWired);
        } else {
            ui->stackedWidget->setCurrentWidget(ui->pageWireless);
        }
    }

    // cmbDeviceRole 控制公共参数行的显隐：
    //   通信设备（communication）→ 显示 dsbMaxBandwidth
    //   交换设备（switch）        → 显示 dsbBackplaneBandwidth + dsbProcessingDelay
    ui->dsbMaxBandwidth->setVisible(!isSwitch);
    ui->lblMaxBandwidth->setVisible(!isSwitch);
    ui->dsbBackplaneBandwidth->setVisible(isSwitch);
    ui->lblBackplaneBandwidth->setVisible(isSwitch);
    ui->dsbProcessingDelay->setVisible(isSwitch);
    ui->lblProcessingDelay->setVisible(isSwitch);
}

// ─── 加载模板列表 ─────────────────────────────────────────────────────────────
void DialogNode::loadTemplates()
{
    m_templates = m_db->listNodeTemplates();

    QSignalBlocker blocker(ui->cmbNodeTemplate);
    ui->cmbNodeTemplate->clear();
    ui->cmbNodeTemplate->addItem("（无模板）", -1);
    for (const NodeTemplate &t : m_templates)
        ui->cmbNodeTemplate->addItem(t.name, t.id);
}

void DialogNode::fillFromTemplate(const NodeTemplate &t)
{
    int typeIdx = ui->cmbNodeType->findText(t.nodeType);
    if (typeIdx >= 0) ui->cmbNodeType->setCurrentIndex(typeIdx);
    ui->dsbInterferenceDb->setValue(t.defaultInterferenceDb);

    // 还原设备实例
    m_deviceKeys   = t.commMethods;
    m_deviceParams = t.deviceParams;
    m_deviceConns.clear();

    refreshDeviceList();
    if (!m_deviceKeys.isEmpty())
        ui->listDeviceInstances->setCurrentRow(0);
    initDeviceMatrix();
    refreshTopology();
}

// ─── 初始化设备连接矩阵 ───────────────────────────────────────────────────────
void DialogNode::initDeviceMatrix()
{
    QSignalBlocker blocker(ui->tableDeviceMatrix);

    int N = m_deviceKeys.size();
    ui->tableDeviceMatrix->clear();
    ui->tableDeviceMatrix->setRowCount(N);
    ui->tableDeviceMatrix->setColumnCount(N);

    // 生成表头（使用 instanceName 或自动名称）
    QStringList horHeaders;
    QStringList verHeaders;
    for (const QString &key : m_deviceKeys) {
        const DeviceParams &dp = m_deviceParams.value(key);
        QString name = dp.instanceName.isEmpty()
                       ? generateInstanceName(key.left(key.lastIndexOf('_')), dp.instanceId)
                       : dp.instanceName;
        verHeaders << name;

        // 对水平列标题进行换行处理，以适配正方形窄列宽
        QString horName = name;
        int lastUnderline = horName.lastIndexOf('_');
        if (lastUnderline > 0) {
            horName[lastUnderline] = '\n';
        } else {
            int lastSpace = horName.lastIndexOf(' ');
            if (lastSpace > 0) {
                horName[lastSpace] = '\n';
            } else if (horName.length() > 4) {
                horName.insert(horName.length() / 2, '\n');
            }
        }
        horHeaders << horName;
    }
    ui->tableDeviceMatrix->setHorizontalHeaderLabels(horHeaders);
    ui->tableDeviceMatrix->setVerticalHeaderLabels(verHeaders);

    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            QTableWidgetItem *item = new QTableWidgetItem();
            if (r == c) {
                // 对角线：置灰禁用
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
                item->setBackground(QBrush(QColor("#E2E8F0")));
            } else {
                item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
                bool connected = m_deviceConns.contains({m_deviceKeys[r], m_deviceKeys[c]}) ||
                                 m_deviceConns.contains({m_deviceKeys[c], m_deviceKeys[r]});
                item->setCheckState(connected ? Qt::Checked : Qt::Unchecked);
            }
            item->setTextAlignment(Qt::AlignCenter);
            ui->tableDeviceMatrix->setItem(r, c, item);
        }
    }

    // 浅色主题样式 + 勾选框适当大小
    ui->tableDeviceMatrix->setStyleSheet(
        "QTableWidget { background:#FFFFFF; border:1px solid #D0DCE8; }"
        "QTableWidget::indicator { width:24px; height:24px; }"
        "QHeaderView::section { background:#EBF4FF; color:#1565C0; font-size:12px;"
        "  font-weight:600; padding:4px; border:1px solid #D0DCE8; }"
        "QTableWidget::item { background:#FAFAFA; }"
        "QTableWidget::item:disabled { background:#E2E8F0; }");

    const int cell = 52;
    QHeaderView *hh = ui->tableDeviceMatrix->horizontalHeader();
    QHeaderView *vh = ui->tableDeviceMatrix->verticalHeader();
    hh->setFixedHeight(46); // 增加表头高度以完整显示换行文字
    hh->setMinimumSectionSize(cell);
    for (int c = 0; c < N; ++c) {
        hh->setSectionResizeMode(c, QHeaderView::Fixed);
        ui->tableDeviceMatrix->setColumnWidth(c, cell);
    }
    vh->setDefaultSectionSize(cell);
    for (int r = 0; r < N; ++r)
        vh->setSectionResizeMode(r, QHeaderView::Fixed);
    hh->setStretchLastSection(false);

    // 按实际内容收缩表格，从左上角开始显示
    int vhW = (N > 0) ? vh->sizeHint().width() : 0;
    int fw  = ui->tableDeviceMatrix->frameWidth() * 2;
    int tableW = qMax(vhW + N * cell + fw + 2, 10);
    int tableH = qMax((N > 0 ? hh->height() : 0) + N * cell + fw + 2, 10);
    ui->tableDeviceMatrix->setFixedSize(tableW, tableH);
    ui->tableDeviceMatrix->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    // 父布局左上对齐
    if (QLayout *parentLay = ui->tableDeviceMatrix->parentWidget()
                             ? ui->tableDeviceMatrix->parentWidget()->layout() : nullptr) {
        if (auto *gridLay = qobject_cast<QGridLayout*>(parentLay))
            gridLay->setAlignment(ui->tableDeviceMatrix, Qt::AlignTop | Qt::AlignLeft);
    }
}

// ─── 矩阵对称同步 ─────────────────────────────────────────────────────────────
void DialogNode::onDeviceMatrixCellChanged(int row, int col)
{
    if (row == col) return;
    if (row >= m_deviceKeys.size() || col >= m_deviceKeys.size()) return;

    QTableWidgetItem *changedItem = ui->tableDeviceMatrix->item(row, col);
    if (!changedItem) return;

    Qt::CheckState state = changedItem->checkState();

    // 阻塞信号防止死循环
    QSignalBlocker blocker(ui->tableDeviceMatrix);

    const QString &keyA = m_deviceKeys[row];
    const QString &keyB = m_deviceKeys[col];

    // ── 新增连接前校验最大连接数限制 ──
    if (state == Qt::Checked) {
        int maxA = m_deviceParams.value(keyA).maxConnections;
        int maxB = m_deviceParams.value(keyB).maxConnections;
        // 当前计数（此格尚未写入 m_deviceConns，故不含本连接）
        if (connectionCountOf(keyA) >= maxA || connectionCountOf(keyB) >= maxB) {
            const DeviceParams &dpA = m_deviceParams.value(keyA);
            const DeviceParams &dpB = m_deviceParams.value(keyB);
            QString nameA = dpA.instanceName.isEmpty() ? keyA : dpA.instanceName;
            QString nameB = dpB.instanceName.isEmpty() ? keyB : dpB.instanceName;
            QString full  = (connectionCountOf(keyA) >= maxA)
                            ? QString("%1（上限 %2）").arg(nameA).arg(maxA)
                            : QString("%1（上限 %2）").arg(nameB).arg(maxB);
            QMessageBox::warning(this, "连接数已达上限",
                QString("设备 %1 已达到最大连接数，无法再添加连接。").arg(full));
            changedItem->setCheckState(Qt::Unchecked);
            return;
        }
    }

    // 同步对称格
    QTableWidgetItem *symItem = ui->tableDeviceMatrix->item(col, row);
    if (symItem) symItem->setCheckState(state);

    // 更新内存中的连接对
    QPair<QString, QString> pair = {keyA, keyB};
    QPair<QString, QString> pairRev = {keyB, keyA};

    m_deviceConns.removeAll(pair);
    m_deviceConns.removeAll(pairRev);

    if (state == Qt::Checked)
        m_deviceConns.append(pair);

    // 同步到拓扑画板（程序化更新，不触发回环）
    if (m_topoView) m_topoView->updateConnections(m_deviceConns);
}

// ─── 拓扑可视化（QtNodes 方案 B）──────────────────────────────────────────────
void DialogNode::initTopologyView()
{
    QWidget *holder = ui->gvDeviceTopology->parentWidget();
    m_topoView = new DeviceTopologyView(holder);

    if (auto *grid = qobject_cast<QGridLayout *>(holder->layout()))
        grid->replaceWidget(ui->gvDeviceTopology, m_topoView);

    ui->gvDeviceTopology->hide();
    ui->gvDeviceTopology->deleteLater();

    connect(m_topoView, &DeviceTopologyView::connectionsChanged,
            this, &DialogNode::onTopologyConnectionsChanged);
}

// 设备实例显示名列表（与矩阵表头一致）
QStringList DialogNode::deviceCaptions() const
{
    QStringList caps;
    for (const QString &key : m_deviceKeys) {
        const DeviceParams &dp = m_deviceParams.value(key);
        caps << (dp.instanceName.isEmpty()
                     ? generateInstanceName(key.left(key.lastIndexOf('_')), dp.instanceId)
                     : dp.instanceName);
    }
    return caps;
}

// 各设备 ioRole（normal/input/output），与 m_deviceKeys 同序
QStringList DialogNode::deviceIoRoles() const
{
    QStringList roles;
    for (const QString &key : m_deviceKeys)
        roles << m_deviceParams.value(key).ioRole;
    return roles;
}

// 各设备最大连接数，与 m_deviceKeys 同序
QList<int> DialogNode::deviceMaxConns() const
{
    QList<int> conns;
    for (const QString &key : m_deviceKeys)
        conns << m_deviceParams.value(key).maxConnections;
    return conns;
}

// 统计某设备当前在连接矩阵中的连接数
int DialogNode::connectionCountOf(const QString &key) const
{
    int n = 0;
    for (const QPair<QString, QString> &p : m_deviceConns)
        if (p.first == key || p.second == key) ++n;
    return n;
}

// 整体重建拓扑画板（设备增删 / 打开对话框）
void DialogNode::refreshTopology()
{
    if (m_topoView)
        m_topoView->rebuild(m_deviceKeys, deviceCaptions(), m_deviceConns,
                            deviceIoRoles(), deviceMaxConns());
}

// 用户在画板上增删连线 → 更新内存并刷新矩阵
void DialogNode::onTopologyConnectionsChanged(const QList<QPair<QString, QString>> &conns)
{
    m_deviceConns = conns;
    initDeviceMatrix();   // 矩阵重建会屏蔽自身信号，不会回环
}

// ─── 槽：添加设备实例 ─────────────────────────────────────────────────────────
void DialogNode::on_btnAddDevice_clicked()
{
    // 先保存当前编辑中的参数
    saveCurrentDeviceParamsFromUI();

    QString typeKey  = ui->cmbDeviceType->currentData().toString();
    QString typeName = ui->cmbDeviceType->currentText();

    int id = nextInstanceId();
    QString key  = generateInstanceKey(typeKey, id);
    QString name = generateInstanceName(typeKey, id);

    // 构造新设备参数
    DeviceParams dp;
    dp.instanceId   = id;
    dp.instanceName = name;
    dp.deviceRole   = (typeKey == "switch") ? "switch" : "communication";
    dp.deviceCategory = (typeKey == "fiber" || typeKey == "fieldwire") ? "wired" : "wireless";

    m_deviceKeys.append(key);
    m_deviceParams.insert(key, dp);

    refreshDeviceList();
    ui->listDeviceInstances->setCurrentRow(m_deviceKeys.size() - 1);

    // 刷新矩阵与拓扑
    initDeviceMatrix();
    refreshTopology();
}

// ─── 槽：删除设备实例 ─────────────────────────────────────────────────────────
void DialogNode::on_btnDeleteDevice_clicked()
{
    int row = ui->listDeviceInstances->currentRow();
    if (row < 0 || row >= m_deviceKeys.size()) return;

    QString key = m_deviceKeys[row];

    // 移除相关连接对
    m_deviceConns.erase(
        std::remove_if(m_deviceConns.begin(), m_deviceConns.end(),
                       [&key](const QPair<QString,QString> &p) {
                           return p.first == key || p.second == key;
                       }),
        m_deviceConns.end()
    );

    m_deviceKeys.removeAt(row);
    m_deviceParams.remove(key);

    if (m_currentKey == key)
        m_currentKey.clear();

    refreshDeviceList();
    initDeviceMatrix();
    refreshTopology();

    int newRow = qMin(row, m_deviceKeys.size() - 1);
    if (newRow >= 0)
        ui->listDeviceInstances->setCurrentRow(newRow);
}

// ─── 槽：切换设备实例（左侧列表选中变化）─────────────────────────────────────
void DialogNode::on_listDeviceInstances_currentRowChanged(int row)
{
    // 先保存旧的参数
    saveCurrentDeviceParamsFromUI();

    if (row < 0 || row >= m_deviceKeys.size()) {
        m_currentKey.clear();
        return;
    }

    m_currentKey = m_deviceKeys[row];
    loadDeviceParamsToUI(m_currentKey);
}

// ─── 槽：角色变化 ─────────────────────────────────────────────────────────────
void DialogNode::on_cmbDeviceRole_currentIndexChanged(int /*index*/)
{
    if (m_loadingParams) return;
    saveCurrentDeviceParamsFromUI();
    updateParamPanelVisibility();
}

// ─── 槽：传输类型变化 ─────────────────────────────────────────────────────────
void DialogNode::on_cmbDeviceCategory_currentIndexChanged(int /*index*/)
{
    if (m_loadingParams) return;
    saveCurrentDeviceParamsFromUI();
    updateParamPanelVisibility();
}

// ─── 槽：输入输出（设备流向）变化 ────────────────────────────────────────────
void DialogNode::on_cmbInputOutput_currentIndexChanged(int /*index*/)
{
    if (m_loadingParams) return;
    saveCurrentDeviceParamsFromUI();
    // 流向变化后重建拓扑：输入设备靠左、输出设备靠右
    refreshTopology();
}

// ─── 槽：最大连接数变化 ───────────────────────────────────────────────────────
void DialogNode::on_sbMaxDevice_valueChanged(int /*value*/)
{
    if (m_loadingParams) return;
    saveCurrentDeviceParamsFromUI();
    // 端口处显示的数字即最大连接数，刷新拓扑以更新显示
    refreshTopology();
}

// ─── 槽：设备名称编辑完成 ─────────────────────────────────────────────────────
void DialogNode::on_leDeviceName_editingFinished()
{
    saveCurrentDeviceParamsFromUI();
    // 名称变化后同步矩阵表头与拓扑节点标题
    initDeviceMatrix();
    refreshTopology();
}

// ─── 槽：模板切换 ─────────────────────────────────────────────────────────────
void DialogNode::on_cmbNodeTemplate_currentIndexChanged(int index)
{
    if (index <= 0) return;
    int templateId = ui->cmbNodeTemplate->itemData(index).toInt();
    for (const NodeTemplate &t : m_templates) {
        if (t.id == templateId) {
            fillFromTemplate(t);
            break;
        }
    }
}

// ─── 槽：保存为模板 ───────────────────────────────────────────────────────────
void DialogNode::on_btnSaveNodeTemplate_clicked()
{
    // 保存当前正在编辑的设备参数
    saveCurrentDeviceParamsFromUI();

    QDialog dlg(this, Qt::Dialog | Qt::FramelessWindowHint);
    QVBoxLayout *lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(10);
    QLabel *title = new QLabel("保存为节点模板", &dlg);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:14px; font-weight:700; color:#1565C0;");
    QLineEdit *le = new QLineEdit(&dlg);
    le->setPlaceholderText("请输入模板名称");
    QDialogButtonBox *btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    if (btns->button(QDialogButtonBox::Ok))     btns->button(QDialogButtonBox::Ok)->setText("确定");
    if (btns->button(QDialogButtonBox::Cancel)) btns->button(QDialogButtonBox::Cancel)->setText("取消");
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(title);
    lay->addWidget(le);
    lay->addWidget(btns);
    dlg.setMinimumWidth(280);
    if (dlg.exec() != QDialog::Accepted) return;
    QString tplName = le->text().trimmed();
    if (tplName.isEmpty()) return;

    NodeTemplate t;
    t.name                  = tplName;
    t.nodeType              = ui->cmbNodeType->currentText();
    t.commMethods           = m_deviceKeys;
    t.deviceParams          = m_deviceParams;
    t.defaultInterferenceDb = ui->dsbInterferenceDb->value();

    int newId = m_db->saveNodeTemplate(t);
    if (newId < 0) {
        QMessageBox::warning(this, "错误", "保存模板失败");
        return;
    }
    loadTemplates();
    int idx = ui->cmbNodeTemplate->findData(newId);
    if (idx >= 0) ui->cmbNodeTemplate->setCurrentIndex(idx);
}

// ─── 槽：取消 ─────────────────────────────────────────────────────────────────
void DialogNode::on_btnCancel_clicked()
{
    reject();
}

// ─── 槽：保存节点 ─────────────────────────────────────────────────────────────
void DialogNode::on_btnSaveNode_clicked()
{
    // 先保存当前正在编辑的参数
    saveCurrentDeviceParamsFromUI();

    // ── 输入校验 ──
    QString idStr = ui->leNodeId->text().trimmed();
    if (idStr.isEmpty()) {
        QMessageBox::warning(this, "提示", "节点ID不能为空");
        return;
    }
    bool idOk;
    int nodeId = idStr.toInt(&idOk);
    if (!idOk || nodeId < 0) {
        QMessageBox::warning(this, "提示", "节点ID必须为非负整数");
        return;
    }

    double lon = ui->dsbNodeLongitude->value();
    double lat = ui->dsbNodeLatitude->value();
    if (lon == 0.0 && lat == 0.0) {
        int ret = QMessageBox::question(this, "确认",
            "经纬度为 0°，确认保存？（可能是未填写）");
        if (ret != QMessageBox::Yes) return;
    }

    // ── 构造 NodeInfo ──
    NodeInfo n;
    n.id               = m_editDbId;
    n.sceneId          = m_sceneId;
    n.nodeId           = nodeId;
    n.name             = ui->leNodeName->text().trimmed();
    n.nodeType         = ui->cmbNodeType->currentText();
    n.status           = ui->cmbNodeStatus->currentText();
    n.longitude        = lon;
    n.latitude         = lat;
    n.altitude         = ui->dsbNodeAltitude->value();
    n.interferenceDb   = ui->dsbInterferenceDb->value();
    n.commMethods      = m_deviceKeys;
    n.deviceParams     = m_deviceParams;
    n.deviceConnections = m_deviceConns;

    int tplIdx = ui->cmbNodeTemplate->currentIndex();
    if (tplIdx > 0)
        n.fromTemplate = ui->cmbNodeTemplate->itemData(tplIdx).toInt();

    // ── 写库 ──
    if (m_editDbId < 0) {
        int newId = m_db->addNode(n);
        if (newId < 0) {
            QMessageBox::critical(this, "错误", "保存节点失败");
            return;
        }
    } else {
        if (!m_db->updateNode(n)) {
            QMessageBox::critical(this, "错误", "更新节点失败");
            return;
        }
    }

    emit nodeSaved();
    accept();
}
