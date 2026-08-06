#ifndef DIALOGLINK_H
#define DIALOGLINK_H

#include "datamodel.h"
#include "dbmanager.h"
#include "framelessdialog.h"

#include <QList>

namespace Ui {
class DialogLink;
}

class DialogLink : public FramelessDialog
{
    Q_OBJECT

public:
    // 新建链路
    explicit DialogLink(IDataStore *db, int sceneId, QWidget *parent = nullptr);
    // 预选源/目节点（拓扑图连线后调用）
    void preselectNodes(int srcNodeId, int dstNodeId);
    // 编辑链路
    explicit DialogLink(IDataStore *db, int sceneId, const LinkInfo &link,
                        QWidget *parent = nullptr);
    ~DialogLink();

signals:
    void linkSaved();

private slots:
    void on_btnCancel_clicked();
    void on_btnSaveLink_clicked();
    void on_btnSaveLinkTemplate_clicked();
    void on_cmbLinkTemplate_currentIndexChanged(int index);
    void on_cmbLinkType_currentIndexChanged(int index);

private:
    void loadTemplates();
    void loadNodes();
    void fillFromTemplate(const LinkTemplate &t);
    void applyLinkTypeUI(int index);          // 0=有线，1=无线
    void updateAvailableSubtypes();           // 根据两节点共同能力过滤子类型
    QStringList commonCapabilities() const;   // 两节点通信能力交集

    Ui::DialogLink      *ui;
    IDataStore *m_db;
    int                  m_sceneId;
    int                  m_editDbId = -1;
    QList<NodeInfo>      m_nodes;          // 当前场景节点（用于查坐标和下拉框）
    QList<LinkTemplate>  m_templates;
};

#endif // DIALOGLINK_H
