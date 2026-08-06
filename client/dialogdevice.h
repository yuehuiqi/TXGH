#ifndef DIALOGDEVICE_H
#define DIALOGDEVICE_H

#include "datamodel.h"
#include <QDialog>
#include "framelessdialog.h"

namespace Ui {
class DialogDevice;
}

// ─── 设备配置对话框 ────────────────────────────────────────────────────────────
// 用于配置某个节点上某个通信设备的详细参数。
// 使用方式：
//   DialogDevice *dlg = new DialogDevice(db, nodeDbId, deviceKey, nodeInfo, this);
//   dlg->exec();
// ─────────────────────────────────────────────────────────────────────────────
#include "idatastore.h"

class DialogDevice : public FramelessDialog
{
    Q_OBJECT

public:
    // 构造函数：nodeInfo 为当前节点数据，deviceKey 为要配置的设备名（如"fiber","adhoc"）
    explicit DialogDevice(IDataStore *db, const NodeInfo &nodeInfo,
                          const QString &deviceKey, QWidget *parent = nullptr);
    ~DialogDevice();

signals:
    void deviceParamsSaved(int nodeDbId, const QString &deviceKey, const DeviceParams &params);

private slots:
    void on_btnCancel_clicked();
    void on_btnSaveDevice_clicked();
    void on_cmbNodeName_currentIndexChanged(int index);
    void on_cmbDeviceName_currentIndexChanged(int index);

private:
    void loadParams();   // 将 nodeInfo.deviceParams[deviceKey] 加载到 UI 控件
    void saveParams();   // 从 UI 控件读取并发出信号
    void saveParamsToMemory(); // 保存当前UI参数到内存缓存

    // 判断是否是无线设备
    static bool isWirelessDevice(const QString &deviceKey);

    Ui::DialogDevice   *ui;
    IDataStore *m_db;
    NodeInfo            m_nodeInfo;
    QString             m_deviceKey;
    QList<NodeInfo>     m_nodes;
    QMap<int, NodeInfo> m_modifiedNodes;
    bool                m_loading = false;
};

#endif // DIALOGDEVICE_H
