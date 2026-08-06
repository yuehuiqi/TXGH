#include "dialogdevice.h"
#include "ui_dialogdevice.h"
#include "dbmanager.h"

#include <QMessageBox>
#include <QMap>

// ─── 设备类型分类表 ───────────────────────────────────────────────────────────
// 无线设备：microwave、scatter、adhoc、satellite、cellular、shortwave、narrowband
// 有线设备：fiber、fieldwire（fieldWire）

static bool isWirelessKey(const QString &key) {
    static const QSet<QString> wirelessKeys = {
        "microwave", "scatter", "adhoc", "satellite",
        "cellular", "shortwave", "narrowband"
    };
    return wirelessKeys.contains(key.toLower());
}

// 设备key → 中文名称映射
static QString deviceKeyToName(const QString &key) {
    static const QMap<QString, QString> nameMap = {
        {"fiber",       "光缆"},
        {"fieldwire",   "野战电缆"},
        {"fieldWire",   "野战电缆"},
        {"microwave",   "微波接力"},
        {"scatter",     "超视距微波"},
        {"adhoc",       "自组网电台"},
        {"satellite",   "卫星通信"},
        {"cellular",    "移动公网"},
        {"shortwave",   "短波电台"},
        {"narrowband",  "窄带战术电台"}
    };
    return nameMap.value(key, key);
}

// ─── 构造函数 ─────────────────────────────────────────────────────────────────
DialogDevice::DialogDevice(IDataStore *db, const NodeInfo &nodeInfo,
                           const QString &deviceKey, QWidget *parent)
    : FramelessDialog(parent)
    , ui(new Ui::DialogDevice)
    , m_db(db)
    , m_nodeInfo(nodeInfo)
    , m_deviceKey(deviceKey)
{
    ui->setupUi(this);

    m_loading = true;

    if (m_db) {
        m_nodes = m_db->getNodesByScene(m_nodeInfo.sceneId);
    }
    if (m_nodes.isEmpty()) {
        m_nodes.append(m_nodeInfo);
    }

    // Populate cmbNodeName
    ui->cmbNodeName->clear();
    int nodeIndex = -1;
    for (int i = 0; i < m_nodes.size(); ++i) {
        const NodeInfo &node = m_nodes[i];
        QString name = node.name.isEmpty()
                           ? QString("节点%1").arg(node.nodeId)
                           : node.name;
        ui->cmbNodeName->addItem(name, node.id);
        if (node.id == m_nodeInfo.id) {
            nodeIndex = i;
        }
    }
    if (nodeIndex != -1) {
        ui->cmbNodeName->setCurrentIndex(nodeIndex);
    }

    // Populate cmbDeviceName
    ui->cmbDeviceName->clear();
    int devIndex = -1;
    for (int i = 0; i < m_nodeInfo.commMethods.size(); ++i) {
        const QString &method = m_nodeInfo.commMethods[i];
        ui->cmbDeviceName->addItem(deviceKeyToName(method), method);
        if (method == m_deviceKey) {
            devIndex = i;
        }
    }
    if (devIndex != -1) {
        ui->cmbDeviceName->setCurrentIndex(devIndex);
    }

    m_loading = false;

    // 根据设备类型切换页面
    if (isWirelessKey(m_deviceKey)) {
        ui->stackedWidget->setCurrentIndex(0);  // 无线参数页
    } else {
        ui->stackedWidget->setCurrentIndex(1);  // 有线参数页
    }

    // 加载已有参数
    loadParams();
}

DialogDevice::~DialogDevice()
{
    delete ui;
}

// ─── 加载参数到 UI ────────────────────────────────────────────────────────────
void DialogDevice::loadParams()
{
    DeviceParams dp;
    if (m_nodeInfo.deviceParams.contains(m_deviceKey)) {
        dp = m_nodeInfo.deviceParams[m_deviceKey];
    }
    // 默认值已在 DeviceParams 结构体中设置，直接使用

    if (isWirelessKey(m_deviceKey)) {
        ui->dsbFreqHz->setValue(dp.freqHz);
        ui->dsbTxPowerDbm->setValue(dp.txPowerDbm);
        ui->dsbRxSensitivity->setValue(dp.rxSensitivityDbm);
        ui->dsbTxAntennaGain->setValue(dp.txAntennaGainDbi);
        ui->dsbRxAntennaGain->setValue(dp.rxAntennaGainDbi);
        ui->dsbNoiseFigure->setValue(dp.noiseFigureDb);
        ui->dsbSnrThreshold->setValue(dp.snrThresholdDb);
        ui->dsbPathLossExp->setValue(dp.pathLossExponent);
        ui->dsbExtraLoss->setValue(dp.additionalLossDb);
    } else {
        ui->leCommProtocol->setText(dp.commProtocol);
        ui->leDeviceType->setText(dp.deviceType);
    }
}

// ─── 保存参数 ─────────────────────────────────────────────────────────────────
void DialogDevice::saveParams()
{
    DeviceParams dp;

    if (isWirelessKey(m_deviceKey)) {
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
        dp.commProtocol = ui->leCommProtocol->text().trimmed();
        dp.deviceType   = ui->leDeviceType->text().trimmed();
    }

    // 更新节点内存中的设备参数并保存到数据库
    m_nodeInfo.deviceParams[m_deviceKey] = dp;
    if (m_db) {
        m_db->updateNode(m_nodeInfo);
    }

    // 发出信号通知 MainWindow
    emit deviceParamsSaved(m_nodeInfo.id, m_deviceKey, dp);
}

// ─── 静态辅助函数 ─────────────────────────────────────────────────────────────
bool DialogDevice::isWirelessDevice(const QString &deviceKey)
{
    return isWirelessKey(deviceKey);
}

void DialogDevice::on_btnCancel_clicked()
{
    reject();
}

void DialogDevice::saveParamsToMemory()
{
    if (m_deviceKey.isEmpty()) return;

    DeviceParams dp;
    if (isWirelessKey(m_deviceKey)) {
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
        dp.commProtocol = ui->leCommProtocol->text().trimmed();
        dp.deviceType   = ui->leDeviceType->text().trimmed();
    }

    m_nodeInfo.deviceParams[m_deviceKey] = dp;
    m_modifiedNodes[m_nodeInfo.id] = m_nodeInfo;
}

void DialogDevice::on_cmbNodeName_currentIndexChanged(int index)
{
    if (m_loading || index < 0) return;

    // 1. 保存当前编辑的节点参数到内存
    saveParamsToMemory();

    // 2. 加载选中的节点
    int nextNodeDbId = ui->cmbNodeName->itemData(index).toInt();
    NodeInfo nextNode;
    if (m_modifiedNodes.contains(nextNodeDbId)) {
        nextNode = m_modifiedNodes[nextNodeDbId];
    } else {
        for (const auto &node : m_nodes) {
            if (node.id == nextNodeDbId) {
                nextNode = node;
                break;
            }
        }
    }

    m_nodeInfo = nextNode;

    // 3. 更新设备下拉框
    m_loading = true;
    ui->cmbDeviceName->clear();
    for (const QString &method : m_nodeInfo.commMethods) {
        ui->cmbDeviceName->addItem(deviceKeyToName(method), method);
    }

    // 寻找匹配的设备Key，如果没有则选第一个
    int devIndex = m_nodeInfo.commMethods.indexOf(m_deviceKey);
    if (devIndex == -1 && !m_nodeInfo.commMethods.isEmpty()) {
        devIndex = 0;
    }

    if (devIndex != -1) {
        ui->cmbDeviceName->setCurrentIndex(devIndex);
        m_deviceKey = m_nodeInfo.commMethods[devIndex];
    } else {
        m_deviceKey = "";
    }
    m_loading = false;

    // 4. 根据当前设备类型切换 UI 页并加载参数
    if (!m_deviceKey.isEmpty()) {
        if (isWirelessKey(m_deviceKey)) {
            ui->stackedWidget->setCurrentIndex(0);
        } else {
            ui->stackedWidget->setCurrentIndex(1);
        }
        loadParams();
    }
}

void DialogDevice::on_cmbDeviceName_currentIndexChanged(int index)
{
    if (m_loading || index < 0) return;

    // 1. 保存当前编辑的参数到内存
    saveParamsToMemory();

    // 2. 切换到新设备
    m_deviceKey = ui->cmbDeviceName->itemData(index).toString();

    // 3. 根据当前设备类型切换 UI 页并加载参数
    if (!m_deviceKey.isEmpty()) {
        if (isWirelessKey(m_deviceKey)) {
            ui->stackedWidget->setCurrentIndex(0);
        } else {
            ui->stackedWidget->setCurrentIndex(1);
        }
        loadParams();
    }
}

void DialogDevice::on_btnSaveDevice_clicked()
{
    saveParamsToMemory();

    if (m_db) {
        for (auto it = m_modifiedNodes.begin(); it != m_modifiedNodes.end(); ++it) {
            m_db->updateNode(it.value());
        }
    }

    // 通知界面更新
    if (!m_deviceKey.isEmpty()) {
        emit deviceParamsSaved(m_nodeInfo.id, m_deviceKey, m_nodeInfo.deviceParams[m_deviceKey]);
    }

    accept();
}

