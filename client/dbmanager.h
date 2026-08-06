#ifndef DBMANAGER_H
#define DBMANAGER_H

#include "datamodel.h"
#include "idatastore.h"

#include <QObject>
#include <QSqlDatabase>
#include <QList>

// 直连数据库的实现。改造前是唯一的实现，P6 之后退居为
// 单机/离线模式，默认走 RemoteDataStore。
class DbManager : public QObject, public IDataStore
{
    Q_OBJECT
public:
    explicit DbManager(QObject *parent = nullptr);
    ~DbManager();

    // 初始化：连接数据库并建表，返回是否成功；path 为空则使用上次路径或默认值
    bool    initDB(const QString &path = QString());
    QString dbPath() const { return m_dbPath; }

    // ── IDataStore ─────────────────────────────────────────────────────────
    bool    isReady() const override { return m_db.isOpen(); }
    QString sourceDescription() const override { return m_dbPath; }
    QString lastError() const override { return m_lastError; }

    // ── 场景 CRUD ──────────────────────────────────────────────────────────
    int             createScene(const SceneInfo &s) override;
    QList<SceneInfo> listScenes() override;
    bool            updateScene(const SceneInfo &s) override;
    bool            deleteScene(int sceneId) override;

    // ── 节点 CRUD ──────────────────────────────────────────────────────────
    int              addNode(const NodeInfo &n) override;
    bool             addNodes(int sceneId, const QList<NodeInfo> &nodes) override;
    QList<NodeInfo>  getNodesByScene(int sceneId) override;
    bool             updateNode(const NodeInfo &n) override;
    bool             deleteNode(int nodeDbId) override;

    // ── 链路 CRUD ──────────────────────────────────────────────────────────
    int              addLink(const LinkInfo &l) override;
    bool             addLinks(int sceneId, const QList<LinkInfo> &links) override;
    QList<LinkInfo>  getLinksByScene(int sceneId) override;
    bool             updateLink(const LinkInfo &l) override;
    bool             deleteLink(int linkDbId) override;
    bool             clearLinksByScene(int sceneId) override;

    // ── 节点模板 ───────────────────────────────────────────────────────────
    int                   saveNodeTemplate(const NodeTemplate &t) override;
    QList<NodeTemplate>   listNodeTemplates() override;

    // ── 链路模板 ───────────────────────────────────────────────────────────
    int                   saveLinkTemplate(const LinkTemplate &t) override;
    QList<LinkTemplate>   listLinkTemplates() override;

    // ── 工具函数 ───────────────────────────────────────────────────────────
    // GEO(经纬高) → ECEF → 三维距离 / 光速 = 传播时延(秒)
    static double calcPropDelay(double lon1, double lat1, double alt1,
                                double lon2, double lat2, double alt2);

    bool execSQL(const QString &sql);

private:
    int  lastInsertedId();   // SELECT SCOPE_IDENTITY() after INSERT

    // 复合字段的 JSON 编解码已移到 modelcodec.h ——
    // RemoteDataStore 也要用，不该藏在某一个实现的私有区里。

    QSqlDatabase m_db;
    QString      m_dbPath;
    QString      m_lastError;
    static int   s_instanceCount;  // 用于生成唯一连接名
};

#endif // DBMANAGER_H
