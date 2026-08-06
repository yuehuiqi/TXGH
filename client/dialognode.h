#ifndef DIALOGNODE_H
#define DIALOGNODE_H

#include "datamodel.h"
#include "dbmanager.h"
#include "framelessdialog.h"

#include <QList>
#include <QMap>

namespace Ui {
class DialogNode;
}

class DeviceTopologyView;

class DialogNode : public FramelessDialog
{
    Q_OBJECT

public:
    // 新建节点
    explicit DialogNode(IDataStore *db, int sceneId, QWidget *parent = nullptr);
    // 新建节点（从拓扑图拖拽，预填经纬度和类型）
    explicit DialogNode(IDataStore *db, int sceneId, double lon, double lat,
                        const QString &nodeType, QWidget *parent = nullptr);
    // 编辑节点
    explicit DialogNode(IDataStore *db, int sceneId, const NodeInfo &node,
                        QWidget *parent = nullptr);
    ~DialogNode();

signals:
    void nodeSaved();

private slots:
    void on_btnCancel_clicked();
    void on_btnSaveNode_clicked();
    void on_btnSaveNodeTemplate_clicked();
    void on_cmbNodeTemplate_currentIndexChanged(int index);

    // Tab 2：设备多实例管理
    void on_btnAddDevice_clicked();
    void on_btnDeleteDevice_clicked();
    void on_listDeviceInstances_currentRowChanged(int row);
    void on_cmbDeviceRole_currentIndexChanged(int index);
    void on_cmbDeviceCategory_currentIndexChanged(int index);
    void on_cmbInputOutput_currentIndexChanged(int index);
    void on_sbMaxDevice_valueChanged(int value);
    void on_leDeviceName_editingFinished();

    // Tab 3：连接矩阵对称同步
    void onDeviceMatrixCellChanged(int row, int col);

    // Tab 4：拓扑画板连线变化回传
    void onTopologyConnectionsChanged(const QList<QPair<QString, QString>> &conns);

private:
    // ── 模板 ──
    void loadTemplates();
    void fillFromTemplate(const NodeTemplate &t);

    // ── 设备管理 ──
    void initTabBarStyle();
    void initDeviceTypeCombo();
    void refreshDeviceList();
    void loadDeviceParamsToUI(const QString &key);
    void saveCurrentDeviceParamsFromUI();
    void updateParamPanelVisibility();
    QString generateInstanceKey(const QString &type, int id) const;
    QString generateInstanceName(const QString &type, int id) const;
    int     nextInstanceId() const;
    QStringList activeDeviceKeys() const;

    // ── 连接矩阵 ──
    void initDeviceMatrix();
    void syncMatrixFromConns();

    // ── 拓扑可视化（QtNodes）──
    void initTopologyView();
    void refreshTopology();
    QStringList deviceCaptions() const;
    QStringList deviceIoRoles() const;    // 各设备 ioRole（与 m_deviceKeys 同序）
    QList<int>  deviceMaxConns() const;    // 各设备 maxConnections（与 m_deviceKeys 同序）

    // ── 连接数限制 ──
    int connectionCountOf(const QString &key) const;  // 统计某设备当前连接数

    // ── 数据收集 ──
    NodeInfo collectNodeData() const;

    Ui::DialogNode     *ui;
    IDataStore *m_db;
    int                 m_sceneId;
    int                 m_editDbId = -1;            // -1 = 新建
    QList<NodeTemplate> m_templates;                // 缓存模板列表

    // 设备实例数据（内存缓存）
    QStringList                    m_deviceKeys;    // 有序实例 key 列表（如 "fiber_00"）
    QMap<QString, DeviceParams>    m_deviceParams;  // key → 参数
    QString                        m_currentKey;    // 当前正在编辑的 key
    bool                           m_loadingParams; // 防止循环保存的标志

    // 设备间连接对
    QList<QPair<QString, QString>> m_deviceConns;

    // 设备拓扑可视化控件（替换 ui->gvDeviceTopology 占位）
    DeviceTopologyView            *m_topoView = nullptr;

    // 设备类型中文名 ↔ 英文 key 的映射
    static const QMap<QString, QString> s_typeKeyMap;   // 中文名 → 英文前缀
    static const QMap<QString, QString> s_typeNameMap;  // 英文前缀 → 中文名
};

#endif // DIALOGNODE_H
