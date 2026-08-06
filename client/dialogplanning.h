#ifndef DIALOGPLANNING_H
#define DIALOGPLANNING_H

#include "datamodel.h"
#include "dbmanager.h"
#include "framelessdialog.h"
#include <QList>

namespace Ui {
class DialogPlanning;
}

class DialogPlanning : public FramelessDialog
{
    Q_OBJECT

public:
    // currentSceneId：主窗口当前场景，打开时自动预选（场景已锁定，不支持切换）
    explicit DialogPlanning(IDataStore *db, int currentSceneId = -1,
                            QWidget *parent = nullptr);
    ~DialogPlanning();

    QList<FlowInfo>       flows()      const { return m_flows;   }
    int                   sceneId()    const { return m_sceneId; }
    QList<QPair<int,int>> exclusions() const { return m_exclusions; }

signals:
    void planSaved(int sceneId, QList<FlowInfo> flows, QList<QPair<int,int>> exclusions);

private slots:
    void on_btnCancel_clicked();
    void on_btnAddFlow_clicked();
    void on_btnSavePlan_clicked();
    void on_btnAddExclusion_clicked();
    void onTableFlowsSelectionChanged();
    void onNodesChanged();     // 源/目节点变化时自动填充其他参数

private:
    void     loadNodesForScene(int sceneId);
    void     rebuildFlowTable();
    void     rebuildExclusionTable();
    void     fillFormFromFlow(const FlowInfo &f);
    FlowInfo collectFormData() const;
    QString  nodeIdToName(int nodeId) const;
    void     autoFillFlowParams();

    Ui::DialogPlanning *ui;
    IDataStore *m_db;
    QList<FlowInfo>     m_flows;
    QList<NodeInfo>     m_nodes;
    int                 m_sceneId    = -1;
    int                 m_editingFid = -1;

    // 禁连规则存储：[nodeA_id, nodeB_id]
    QList<QPair<int,int>> m_exclusions;
};

#endif // DIALOGPLANNING_H
