#ifndef THGH_SERVER_DB_SCENE_DAO_H
#define THGH_SERVER_DB_SCENE_DAO_H

// ─────────────────────────────────────────────────────────────────────────────
// SceneDao —— 场景相关的数据访问
//
// ── 这一层要解决的核心问题：多语句写入的原子性 ──────────────────────────
// "保存一个场景"实际是三步：
//     INSERT scenes → 批量 INSERT scene_nodes → 批量 INSERT scene_links
// 改造前客户端是逐条裸执行的（`grep -c "transaction|commit|rollback"` = **0**），
// 中途任何一步失败，数据库里就留下一个**只有一半节点、没有链路的残缺场景**，
// 而且没有任何标记能识别它 —— 用户看到的是一个打开就报错的场景。
//
// 用事务包起来之后：要么整个场景完整落库，要么什么都不留。
//
// ── 批量插入为什么要合并成一条 SQL ────────────────────────────────────────
// 一个场景动辄上百个节点。逐条 INSERT 意味着上百次网络往返 ——
// 即使在同一台机器上，每次往返也有几十微秒的固定开销。
// 合并成 `INSERT ... VALUES (...),(...),(...)` 之后只有一次往返，
// 实测差距见 apps/db_bench.cpp。
//
// 线程安全：无状态，方法都接收连接作为参数，由调用方从连接池借。
// ─────────────────────────────────────────────────────────────────────────────

#include "db/mysql_conn.h"

#include <cstdint>
#include <string>
#include <vector>

namespace thgh {

struct SceneRecord {
    std::int64_t id = 0;
    std::string name;
    std::string description;
    std::string sceneType;
    std::string createTime;
};

struct SceneNodeRecord {
    std::int64_t id = 0;
    std::int64_t sceneId = 0;
    int nodeId = 0;
    std::int64_t fromTemplate = 0;
    std::string name;
    std::string nodeType;
    std::string status = "在线";
    double longitude = 0.0;
    double latitude = 0.0;
    double altitude = 0.0;
    double interferenceDb = 0.0;
    std::string commMethods;
    std::string deviceParams;
    std::string deviceConnections;
};

struct SceneLinkRecord {
    std::int64_t id = 0;
    std::int64_t sceneId = 0;
    int src = 0;
    int dst = 0;
    std::int64_t fromTemplate = 0;
    std::string linkType;
    std::string wirelessType;
    double bandwidthBps = 0.0;
    double propDelayS = 0.0;
    std::string commProtocol;
    std::string deviceType;
    std::string flows;
};

// ── 模板（P6 新增）──────────────────────────────────────────────────────────
struct NodeTemplateRecord {
    std::int64_t id = 0;
    std::string templateName;
    std::string nodeType;
    std::string commMethods;
    double interferenceDb = 0.0;
    std::string description;
    std::string deviceParams;
};

struct LinkTemplateRecord {
    std::int64_t id = 0;
    std::string templateName;
    std::string linkType;
    std::string wirelessType;
    double bandwidthBps = 50e6;
    std::string description;
};

class SceneDao {
public:
    // ── 写 ──────────────────────────────────────────────────────────────
    // 原子地保存整个场景（场景 + 全部节点 + 全部链路）。
    // 任何一步失败即整体回滚，不会留下残缺数据。
    // 成功时把生成的场景 id 写回 outSceneId。
    static bool saveScene(MySqlConnection& conn, const SceneRecord& scene,
                          const std::vector<SceneNodeRecord>& nodes,
                          const std::vector<SceneLinkRecord>& links,
                          std::int64_t* outSceneId);

    // 删除场景。子表数据由外键 ON DELETE CASCADE 自动清除 ——
    // 这是数据库层面保证一致性，比应用层"先删子表再删主表"可靠：
    // 后者一旦中途崩溃就会留下孤儿数据。
    static bool deleteScene(MySqlConnection& conn, std::int64_t sceneId);

    // ── 读 ──────────────────────────────────────────────────────────────
    static bool listScenes(MySqlConnection& conn, std::vector<SceneRecord>& out);
    static bool loadNodes(MySqlConnection& conn, std::int64_t sceneId,
                          std::vector<SceneNodeRecord>& out);
    static bool loadLinks(MySqlConnection& conn, std::int64_t sceneId,
                          std::vector<SceneLinkRecord>& out);

    // 按名称查场景。这是索引优化实验的目标查询之一 ——
    // name 上没有索引时是全表扫描。
    static bool findSceneByName(MySqlConnection& conn, const std::string& name,
                                std::vector<SceneRecord>& out);

    // ── 单实体 CRUD（P6 新增）───────────────────────────────────────────
    //
    // P3 只做了"整场景原子保存/加载"，因为那时客户端还直连数据库、
    // 服务端只需要承载规划任务。P6 客户端改走协议之后，
    // 界面上每一次增删改都要能单独发过来，所以补齐这一层。
    //
    // 全部走预处理语句。这里的参数直接来自网络，是不可信输入 ——
    // 拼接 SQL 的话，一个场景名写成 `'; DROP TABLE scenes; --` 就完了。
    // 预处理语句把值和语句结构分开传给服务器，值永远不会被当作 SQL 解析。

    static bool createScene(MySqlConnection& conn, const SceneRecord& s,
                            std::int64_t* outId);
    static bool updateScene(MySqlConnection& conn, const SceneRecord& s);

    static bool addNode(MySqlConnection& conn, const SceneNodeRecord& n,
                        std::int64_t* outId);
    // 批量新增。客户端导入场景时一次几百个节点，逐条发就是几百次往返。
    // 内部复用 insertNodesBatch 的多值 INSERT，并用事务包住。
    static bool addNodesBatch(MySqlConnection& conn, std::int64_t sceneId,
                              const std::vector<SceneNodeRecord>& nodes,
                              std::int64_t* outAffected);
    static bool updateNode(MySqlConnection& conn, const SceneNodeRecord& n);
    static bool deleteNode(MySqlConnection& conn, std::int64_t nodeDbId);

    static bool addLink(MySqlConnection& conn, const SceneLinkRecord& l,
                        std::int64_t* outId);
    static bool addLinksBatch(MySqlConnection& conn, std::int64_t sceneId,
                              const std::vector<SceneLinkRecord>& links,
                              std::int64_t* outAffected);
    static bool updateLink(MySqlConnection& conn, const SceneLinkRecord& l);
    static bool deleteLink(MySqlConnection& conn, std::int64_t linkDbId);
    static bool clearLinksByScene(MySqlConnection& conn, std::int64_t sceneId,
                                  std::int64_t* outAffected);

    // ── 模板 ────────────────────────────────────────────────────────────
    static bool listNodeTemplates(MySqlConnection& conn,
                                  std::vector<NodeTemplateRecord>& out);
    static bool saveNodeTemplate(MySqlConnection& conn,
                                 const NodeTemplateRecord& t,
                                 std::int64_t* outId);
    static bool listLinkTemplates(MySqlConnection& conn,
                                  std::vector<LinkTemplateRecord>& out);
    static bool saveLinkTemplate(MySqlConnection& conn,
                                 const LinkTemplateRecord& t,
                                 std::int64_t* outId);

    // ── 测试与压测辅助 ──────────────────────────────────────────────────
    static bool countNodes(MySqlConnection& conn, std::int64_t sceneId,
                           std::int64_t* out);
    static bool countLinks(MySqlConnection& conn, std::int64_t sceneId,
                           std::int64_t* out);

private:
    static bool insertNodesBatch(MySqlConnection& conn, std::int64_t sceneId,
                                 const std::vector<SceneNodeRecord>& nodes);
    static bool insertLinksBatch(MySqlConnection& conn, std::int64_t sceneId,
                                 const std::vector<SceneLinkRecord>& links);
};

}  // namespace thgh

#endif  // THGH_SERVER_DB_SCENE_DAO_H
