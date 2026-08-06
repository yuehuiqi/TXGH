#ifndef DIALOGSCENE_H
#define DIALOGSCENE_H

#include "datamodel.h"
#include "dbmanager.h"
#include "framelessdialog.h"

namespace Ui {
class DialogScene;
}

class DialogScene : public FramelessDialog
{
    Q_OBJECT

public:
    // 新建场景
    explicit DialogScene(IDataStore *db, QWidget *parent = nullptr);
    // 编辑场景（预填现有数据）
    explicit DialogScene(IDataStore *db, const SceneInfo &existing,
                         QWidget *parent = nullptr);
    ~DialogScene();

signals:
    void sceneSaved(int sceneId);

private slots:
    void on_btnCancel_clicked();
    void on_btnSaveScene_clicked();

private:
    Ui::DialogScene *ui;
    IDataStore *m_db;
    int        m_editId = -1;    // -1 = 新建
};

#endif // DIALOGSCENE_H
