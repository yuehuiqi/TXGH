#include "dialogscene.h"
#include "ui_dialogscene.h"

#include <QMessageBox>

// 新建
DialogScene::DialogScene(IDataStore *db, QWidget *parent)
    : FramelessDialog(parent)
    , ui(new Ui::DialogScene)
    , m_db(db)
    , m_editId(-1)
{
    ui->setupUi(this);
    setWindowTitle("新建场景");
}

// 编辑
DialogScene::DialogScene(IDataStore *db, const SceneInfo &existing, QWidget *parent)
    : FramelessDialog(parent)
    , ui(new Ui::DialogScene)
    , m_db(db)
    , m_editId(existing.id)
{
    ui->setupUi(this);
    setWindowTitle("编辑场景");

    ui->leSceneName->setText(existing.name);
    ui->teSceneDesc->setPlainText(existing.description);

    int idx = ui->cmbSceneType->findText(existing.sceneType);
    if (idx >= 0) ui->cmbSceneType->setCurrentIndex(idx);
}

DialogScene::~DialogScene()
{
    delete ui;
}

void DialogScene::on_btnCancel_clicked()
{
    reject();
}

void DialogScene::on_btnSaveScene_clicked()
{
    QString name = ui->leSceneName->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, "", "场景名称不能为空");
        return;
    }

    SceneInfo s;
    s.name        = name;
    s.description = ui->teSceneDesc->toPlainText();
    s.sceneType   = ui->cmbSceneType->currentText();
    s.id          = m_editId;

    int savedId = -1;
    if (m_editId < 0) {
        savedId = m_db->createScene(s);
        if (savedId < 0) {
            QMessageBox::critical(this, "", "保存场景失败，请检查数据库");
            return;
        }
    } else {
        if (!m_db->updateScene(s)) {
            QMessageBox::critical(this, "", "更新场景失败");
            return;
        }
        savedId = m_editId;
    }

    emit sceneSaved(savedId);
    accept();
}
