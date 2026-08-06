#ifndef IDATASTORE_H
#define IDATASTORE_H

// ─────────────────────────────────────────────────────────────────────────────
// IDataStore —— 数据访问抽象（P6）
//
// 两个实现：
//   DbManager        直连数据库（改造前的做法，保留作为单机/离线模式）
//   RemoteDataStore  经协议请求服务端（默认）
//
// ── 为什么保留本地实现而不是直接删掉 ──────────────────────────────────────
// 演示和调试时经常没有服务端环境。留一个开关能直接退回单机模式，
// 比"改代码重新编译"实用得多。而且有两个实现在，接口才会被逼着定干净 ——
// 只有一个实现的"抽象"通常会不知不觉泄漏出实现细节。
//
// ── 关于同步 API ★ ────────────────────────────────────────────────────────
// 这些方法是**同步**的：调用即阻塞，直到拿到结果或超时。
// 网络操作做成同步在很多场景下是坏味道，这里是有意为之，理由：
//
//   1. 改造前这些调用就是同步的 —— `QSqlQuery::exec()` 一样阻塞 GUI 线程。
//      换成网络不是新引入阻塞，只是把阻塞源从本地 IO 换成网络 IO。
//   2. 全部改成异步意味着重写 70 处调用点的 UI 逻辑
//      （`for (const SceneInfo &s : m_db->listScenes())` 这种内联写法到处都是），
//      回归风险远大于收益。这些又都是用户点一下才触发的低频操作。
//   3. 真正的风险是"网络挂了导致界面永久冻结"，这一条用**硬超时**解决，
//      而不是靠异步。超时后返回失败，界面报错并恢复可用。
//
// 代价写清楚：服务端不可达时，第一次调用会卡住一个超时时长（默认 5 秒）。
// 如果以后单次操作的数据量涨到需要秒级传输，就该重新考虑异步化了。
//
// ── 批量接口 ★ ────────────────────────────────────────────────────────────
// `addNodes` / `addLinks` 不是可有可无的便利方法，是**必需品**：
// 导入场景原来写的是 `for (n : nodes) addNode(n)`，直连数据库时只是慢，
// 走网络就是 N 次往返 —— 500 个节点直接把导入变成不可用。
// ─────────────────────────────────────────────────────────────────────────────

#include "datamodel.h"

#include <QList>
#include <QString>

class IDataStore
{
public:
    virtual ~IDataStore() = default;

    // 就绪状态。false 表示后续调用都会失败（未连接/未初始化）。
    virtual bool isReady() const = 0;
    // 数据源描述，显示在状态栏上（本地是文件路径，远端是 host:port）
    virtual QString sourceDescription() const = 0;
    // 最近一次失败的原因，供界面提示
    virtual QString lastError() const = 0;

    // ── 场景 ────────────────────────────────────────────────────────────
    virtual int              createScene(const SceneInfo &s) = 0;
    virtual QList<SceneInfo> listScenes() = 0;
    virtual bool             updateScene(const SceneInfo &s) = 0;
    virtual bool             deleteScene(int sceneId) = 0;

    // ── 节点 ────────────────────────────────────────────────────────────
    virtual int             addNode(const NodeInfo &n) = 0;
    virtual bool            addNodes(int sceneId, const QList<NodeInfo> &nodes) = 0;
    virtual QList<NodeInfo> getNodesByScene(int sceneId) = 0;
    virtual bool            updateNode(const NodeInfo &n) = 0;
    virtual bool            deleteNode(int nodeDbId) = 0;

    // ── 链路 ────────────────────────────────────────────────────────────
    virtual int             addLink(const LinkInfo &l) = 0;
    virtual bool            addLinks(int sceneId, const QList<LinkInfo> &links) = 0;
    virtual QList<LinkInfo> getLinksByScene(int sceneId) = 0;
    virtual bool            updateLink(const LinkInfo &l) = 0;
    virtual bool            deleteLink(int linkDbId) = 0;
    virtual bool            clearLinksByScene(int sceneId) = 0;

    // ── 模板 ────────────────────────────────────────────────────────────
    virtual int                 saveNodeTemplate(const NodeTemplate &t) = 0;
    virtual QList<NodeTemplate> listNodeTemplates() = 0;
    virtual int                 saveLinkTemplate(const LinkTemplate &t) = 0;
    virtual QList<LinkTemplate> listLinkTemplates() = 0;
};

#endif // IDATASTORE_H
