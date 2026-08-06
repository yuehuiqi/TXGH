#include "dialoglink.h"
#include "ui_dialoglink.h"

#include <QMessageBox>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QDialogButtonBox>

// ─────────────────────────────────────────────────────────────────────────────
// 新建
// ─────────────────────────────────────────────────────────────────────────────
DialogLink::DialogLink(IDataStore *db, int sceneId, QWidget *parent)
    : FramelessDialog(parent)
    , ui(new Ui::DialogLink)
    , m_db(db)
    , m_sceneId(sceneId)
    , m_editDbId(-1)
{
    ui->setupUi(this);
    setWindowTitle("新建链路");

    loadNodes();
    loadTemplates();

    // 节点选择变化时实时更新可用子类型
    connect(ui->cmbSrcNode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DialogLink::updateAvailableSubtypes);
    connect(ui->cmbDstNode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DialogLink::updateAvailableSubtypes);

    // 初始状态：有线（cmbLinkType index=0）
    applyLinkTypeUI(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 编辑
// ─────────────────────────────────────────────────────────────────────────────
DialogLink::DialogLink(IDataStore *db, int sceneId, const LinkInfo &link,
                       QWidget *parent)
    : FramelessDialog(parent)
    , ui(new Ui::DialogLink)
    , m_db(db)
    , m_sceneId(sceneId)
    , m_editDbId(link.id)
{
    ui->setupUi(this);
    setWindowTitle("编辑链路");

    loadNodes();
    loadTemplates();

    connect(ui->cmbSrcNode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DialogLink::updateAvailableSubtypes);
    connect(ui->cmbDstNode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DialogLink::updateAvailableSubtypes);

    // 预填链路类型
    bool isWireless = (link.linkType == "wireless" || link.linkType == "无线");
    {
        QSignalBlocker b(ui->cmbLinkType);
        ui->cmbLinkType->setCurrentIndex(isWireless ? 1 : 0);
    }
    applyLinkTypeUI(isWireless ? 1 : 0);

    // 选中通信方式子类型
    {
        int wIdx = ui->cmbWiredSubType->findText(link.wirelessType);
        if (wIdx >= 0) ui->cmbWiredSubType->setCurrentIndex(wIdx);
    }

    // 选中源/目节点
    for (int i = 0; i < ui->cmbSrcNode->count(); ++i) {
        if (ui->cmbSrcNode->itemData(i).toInt() == link.src) {
            ui->cmbSrcNode->setCurrentIndex(i); break;
        }
    }
    for (int i = 0; i < ui->cmbDstNode->count(); ++i) {
        if (ui->cmbDstNode->itemData(i).toInt() == link.dst) {
            ui->cmbDstNode->setCurrentIndex(i); break;
        }
    }

    ui->dsbBandwidthBps->setValue(link.bandwidthBps);
    ui->dsbPropDelayS->setValue(link.propDelayS);

    // ui->dsbFreqHz->setValue(link.freqHz);
    // ui->dsbTxPowerDbm->setValue(link.txPowerDbm);
    // ui->dsbRxSensitivity->setValue(link.rxSensitivityDbm);
    // ui->dsbTxAntennaGain->setValue(link.txAntennaGainDbi);
    // ui->dsbRxAntennaGain->setValue(link.rxAntennaGainDbi);
    // ui->dsbNoiseFigure->setValue(link.noiseFigureDb);
    // ui->dsbSnrThreshold->setValue(link.snrThresholdDb);
    // ui->dsbPathLossExp->setValue(link.pathLossExponent);
    // ui->dsbExtraLoss->setValue(link.additionalLossDb);

    // ui->leCommProtocol->setText(link.commProtocol);
    // ui->leDeviceType->setText(link.deviceType);
}

DialogLink::~DialogLink()
{
    delete ui;
}

// ─── 预选源/目节点（拓扑图拖拽连线后调用）──────────────────────────────────
void DialogLink::preselectNodes(int srcNodeId, int dstNodeId)
{
    for (int i = 0; i < ui->cmbSrcNode->count(); ++i) {
        if (ui->cmbSrcNode->itemData(i).toInt() == srcNodeId) {
            ui->cmbSrcNode->setCurrentIndex(i); break;
        }
    }
    for (int i = 0; i < ui->cmbDstNode->count(); ++i) {
        if (ui->cmbDstNode->itemData(i).toInt() == dstNodeId) {
            ui->cmbDstNode->setCurrentIndex(i); break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 私有辅助
// ─────────────────────────────────────────────────────────────────────────────
void DialogLink::loadNodes()
{
    m_nodes = m_db->getNodesByScene(m_sceneId);
    ui->cmbSrcNode->clear();
    ui->cmbDstNode->clear();
    for (const NodeInfo &n : m_nodes) {
        if (n.status == "离线") continue;   // 离线节点不可建链路
        QString display = QString("[%1] %2").arg(n.nodeId).arg(n.name.isEmpty()
                                                               ? n.nodeType : n.name);
        ui->cmbSrcNode->addItem(display, n.nodeId);
        ui->cmbDstNode->addItem(display, n.nodeId);
    }
    if (ui->cmbDstNode->count() > 1)
        ui->cmbDstNode->setCurrentIndex(1);
}

void DialogLink::loadTemplates()
{
    m_templates = m_db->listLinkTemplates();
    QSignalBlocker b(ui->cmbLinkTemplate);
    ui->cmbLinkTemplate->clear();
    ui->cmbLinkTemplate->addItem("（无模板）", -1);
    for (const LinkTemplate &t : m_templates)
        ui->cmbLinkTemplate->addItem(t.name, t.id);
}

void DialogLink::fillFromTemplate(const LinkTemplate &t)
{
    bool isWireless = (t.linkType == "wireless" || t.linkType == "无线");
    {
        QSignalBlocker b(ui->cmbLinkType);
        ui->cmbLinkType->setCurrentIndex(isWireless ? 1 : 0);
    }
    applyLinkTypeUI(isWireless ? 1 : 0);

    ui->dsbBandwidthBps->setValue(t.bandwidthBps);
    // ui->dsbFreqHz->setValue(t.freqHz);
    // ui->dsbTxPowerDbm->setValue(t.txPowerDbm);
    // ui->dsbRxSensitivity->setValue(t.rxSensitivityDbm);
    // ui->dsbTxAntennaGain->setValue(t.txAntennaGainDbi);
    // ui->dsbRxAntennaGain->setValue(t.rxAntennaGainDbi);
    // ui->dsbNoiseFigure->setValue(t.noiseFigureDb);
    // ui->dsbSnrThreshold->setValue(t.snrThresholdDb);
    // ui->dsbPathLossExp->setValue(t.pathLossExponent);
    // ui->dsbExtraLoss->setValue(t.additionalLossDb);
}

// ─── 核心：有线/无线切换 ───────────────────────────────────────────────────────
void DialogLink::applyLinkTypeUI(int index)
{
    if (index == 0) {
        ui->lblWiredType->setText("有线类型");
    } else {
        ui->lblWiredType->setText("无线类型");
    }
    updateAvailableSubtypes();
}

// ─────────────────────────────────────────────────────────────────────────────
// 通信能力交集
// ─────────────────────────────────────────────────────────────────────────────
QStringList DialogLink::commonCapabilities() const
{
    int id1 = ui->cmbSrcNode->currentData().toInt();
    int id2 = ui->cmbDstNode->currentData().toInt();
    QStringList caps1, caps2;
    for (const NodeInfo &n : m_nodes) {
        if (n.nodeId == id1) caps1 = n.commMethods;
        if (n.nodeId == id2) caps2 = n.commMethods;
    }
    QStringList common;
    for (const QString &c : caps1)
        if (caps2.contains(c)) common << c;
    return common;
}

// 能力键 → (是否无线, 子类型显示名)，名称与节点能力勾选项保持一致
static void capToLinkType(const QString &cap, bool &wireless, QString &subtype)
{
    wireless = true;
    if      (cap == "fiber")      { wireless = false; subtype = "光缆"; }
    else if (cap == "fieldwire")  { wireless = false; subtype = "野战电缆"; }
    else if (cap == "adhoc")      { subtype = "自组网电台"; }
    else if (cap == "narrowband") { subtype = "窄带战术电台"; }
    else if (cap == "microwave")  { subtype = "微波接力"; }
    else if (cap == "scatter")    { subtype = "超视距微波"; }
    else if (cap == "cellular")   { subtype = "移动公网"; }
    else if (cap == "shortwave")  { subtype = "短波电台"; }
    else if (cap == "satellite")  { subtype = "卫星通信"; }
    else                          { subtype = QString(); }
}

// ─────────────────────────────────────────────────────────────────────────────
// 根据节点共同能力动态过滤子类型下拉框
// ─────────────────────────────────────────────────────────────────────────────
void DialogLink::updateAvailableSubtypes()
{
    bool isWireless = (ui->cmbLinkType->currentIndex() == 1);
    QStringList common = commonCapabilities();

    // 计算当前链路类型下可用子类型
    QStringList available;
    for (const QString &cap : common) {
        bool w; QString sub;
        capToLinkType(cap, w, sub);
        if (w == isWireless && !sub.isEmpty())
            available << sub;
    }

    // 若无交集或交集不含当前类型，退回默认列表
    const QStringList fallbackWired    = {"光缆", "野战电缆"};
    const QStringList fallbackWireless = {"自组网电台", "窄带战术电台", "微波接力", "超视距微波", "短波电台", "卫星通信", "移动公网"};
    const QStringList &toShow = available.isEmpty()
                                ? (isWireless ? fallbackWireless : fallbackWired)
                                : available;

    QSignalBlocker b(ui->cmbWiredSubType);
    QString prev = ui->cmbWiredSubType->currentText();
    ui->cmbWiredSubType->clear();
    ui->cmbWiredSubType->addItems(toShow);
    int idx = ui->cmbWiredSubType->findText(prev);
    if (idx >= 0) ui->cmbWiredSubType->setCurrentIndex(idx);
}

// ─────────────────────────────────────────────────────────────────────────────
// 槽函数
// ─────────────────────────────────────────────────────────────────────────────
void DialogLink::on_btnCancel_clicked()
{
    reject();
}

void DialogLink::on_cmbLinkType_currentIndexChanged(int index)
{
    applyLinkTypeUI(index);
}

void DialogLink::on_cmbLinkTemplate_currentIndexChanged(int index)
{
    if (index <= 0) return;
    int tplId = ui->cmbLinkTemplate->itemData(index).toInt();
    for (const LinkTemplate &t : m_templates) {
        if (t.id == tplId) { fillFromTemplate(t); break; }
    }
}

void DialogLink::on_btnSaveLinkTemplate_clicked()
{
    QDialog dlg(this, Qt::Dialog | Qt::FramelessWindowHint);
    QVBoxLayout *lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(10);
    QLabel *title = new QLabel("保存为链路模板", &dlg);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:14px; font-weight:700; color:#1565C0;");
    QLineEdit *le = new QLineEdit(&dlg);
    le->setPlaceholderText("请输入模板名称");
    QDialogButtonBox *btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(title);
    lay->addWidget(le);
    lay->addWidget(btns);
    dlg.setMinimumWidth(280);
    if (dlg.exec() != QDialog::Accepted) return;
    QString tplName = le->text().trimmed();
    if (tplName.isEmpty()) return;

    bool isWireless = (ui->cmbLinkType->currentIndex() == 1);

    LinkTemplate t;
    t.name             = tplName.trimmed();
    t.linkType         = isWireless ? "wireless" : "wired";
    t.wirelessType     = isWireless ? ui->cmbWiredSubType->currentText() : QString();
    t.bandwidthBps     = ui->dsbBandwidthBps->value();
    // t.freqHz           = ui->dsbFreqHz->value();
    // t.txPowerDbm       = ui->dsbTxPowerDbm->value();
    // t.rxSensitivityDbm = ui->dsbRxSensitivity->value();
    // t.txAntennaGainDbi = ui->dsbTxAntennaGain->value();
    // t.rxAntennaGainDbi = ui->dsbRxAntennaGain->value();
    // t.noiseFigureDb    = ui->dsbNoiseFigure->value();
    // t.snrThresholdDb   = ui->dsbSnrThreshold->value();
    // t.pathLossExponent = ui->dsbPathLossExp->value();
    // t.additionalLossDb = ui->dsbExtraLoss->value();

    int newId = m_db->saveLinkTemplate(t);
    if (newId < 0) { QMessageBox::warning(this, "错误", "保存模板失败"); return; }
    loadTemplates();
    int idx = ui->cmbLinkTemplate->findData(newId);
    if (idx >= 0) ui->cmbLinkTemplate->setCurrentIndex(idx);
}

void DialogLink::on_btnSaveLink_clicked()
{
    if (ui->cmbSrcNode->count() == 0 || ui->cmbDstNode->count() == 0) {
        QMessageBox::warning(this, "提示", "当前场景没有可用节点（离线节点除外），请先添加节点");
        return;
    }

    int srcNodeId = ui->cmbSrcNode->currentData().toInt();
    int dstNodeId = ui->cmbDstNode->currentData().toInt();
    if (srcNodeId == dstNodeId) {
        QMessageBox::warning(this, "提示", "节点一和节点二不能相同");
        return;
    }

    bool isWireless = (ui->cmbLinkType->currentIndex() == 1);
    QString subtype  = ui->cmbWiredSubType->currentText();

    // ── 传播时延自动计算 ──
    double propDelay = ui->dsbPropDelayS->value();
    if (propDelay <= 0.0) {
        const NodeInfo *srcNode = nullptr, *dstNode = nullptr;
        for (const NodeInfo &n : m_nodes) {
            if (n.nodeId == srcNodeId) srcNode = &n;
            if (n.nodeId == dstNodeId) dstNode = &n;
        }
        if (srcNode && dstNode) {
            propDelay = DbManager::calcPropDelay(
                srcNode->longitude, srcNode->latitude, srcNode->altitude,
                dstNode->longitude, dstNode->latitude, dstNode->altitude
            );
            ui->dsbPropDelayS->setValue(propDelay);
        }
    }

    // ── 构造 LinkInfo ──
    LinkInfo l;
    l.id         = m_editDbId;
    l.sceneId    = m_sceneId;
    l.src        = srcNodeId;
    l.dst        = dstNodeId;
    l.linkType   = isWireless ? "wireless" : "wired";
    l.wirelessType = ui->cmbWiredSubType->currentText();
    l.bandwidthBps     = ui->dsbBandwidthBps->value();
    l.propDelayS       = propDelay;
    // l.freqHz           = ui->dsbFreqHz->value();
    // l.txPowerDbm       = ui->dsbTxPowerDbm->value();
    // l.rxSensitivityDbm = ui->dsbRxSensitivity->value();
    // l.txAntennaGainDbi = ui->dsbTxAntennaGain->value();
    // l.rxAntennaGainDbi = ui->dsbRxAntennaGain->value();
    // l.noiseFigureDb    = ui->dsbNoiseFigure->value();
    // l.snrThresholdDb   = ui->dsbSnrThreshold->value();
    // l.pathLossExponent = ui->dsbPathLossExp->value();
    // l.additionalLossDb = ui->dsbExtraLoss->value();
    // l.commProtocol     = ui->leCommProtocol->text();
    // l.deviceType       = ui->leDeviceType->text();

    // 记录模板来源
    int tplIdx = ui->cmbLinkTemplate->currentIndex();
    if (tplIdx > 0)
        l.fromTemplate = ui->cmbLinkTemplate->itemData(tplIdx).toInt();

    // ── 写库 ──
    if (m_editDbId < 0) {
        int newId = m_db->addLink(l);
        if (newId < 0) { QMessageBox::critical(this, "错误", "保存链路失败"); return; }
    } else {
        if (!m_db->updateLink(l)) { QMessageBox::critical(this, "错误", "更新链路失败"); return; }
    }

    emit linkSaved();
    accept();
}
