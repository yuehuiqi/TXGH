#include "db/scene_dao.h"

#include <cstdio>
#include <cstdlib>

namespace thgh {
namespace {

std::int64_t toI64(const std::string& s) {
    return s.empty() ? 0 : std::strtoll(s.c_str(), nullptr, 10);
}
double toD(const std::string& s) {
    return s.empty() ? 0.0 : std::strtod(s.c_str(), nullptr);
}

// 批量插入的分批大小。
// 不是越大越好：MySQL 有 max_allowed_packet 限制（默认 64MB，但很多部署调小到 4MB），
// 单条 SQL 过大会直接被服务端拒绝，而且报错信息不直观。
// 500 行/批在本项目的行宽下约几百 KB，安全且已经把往返次数摊得足够薄。
constexpr std::size_t kBatchRows = 500;

}  // namespace

// ── 写 ──────────────────────────────────────────────────────────────────────

bool SceneDao::saveScene(MySqlConnection& conn, const SceneRecord& scene,
                         const std::vector<SceneNodeRecord>& nodes,
                         const std::vector<SceneLinkRecord>& links,
                         std::int64_t* outSceneId) {
    // RAII 事务守卫：任何一条 return 路径没有 commit 就自动 rollback。
    // 这是"零残缺数据"的保证 —— 靠人记得写 rollback 是不可靠的。
    Transaction tx(conn);
    if (!tx.begun()) {
        std::fprintf(stderr, "[SceneDao] 开启事务失败: %s\n",
                     conn.lastError().c_str());
        return false;
    }

    if (!conn.executePrepared(
            "INSERT INTO scenes (name, description, scene_type, create_time) "
            "VALUES (?, ?, ?, ?)",
            {Param::ofString(scene.name), Param::ofString(scene.description),
             Param::ofString(scene.sceneType),
             Param::ofString(scene.createTime)})) {
        std::fprintf(stderr, "[SceneDao] 插入场景失败: %s\n",
                     conn.lastError().c_str());
        return false;  // tx 析构自动回滚
    }

    const std::int64_t sceneId = static_cast<std::int64_t>(conn.lastInsertId());

    if (!insertNodesBatch(conn, sceneId, nodes)) {
        return false;
    }
    if (!insertLinksBatch(conn, sceneId, links)) {
        return false;
    }

    if (!tx.commit()) {
        std::fprintf(stderr, "[SceneDao] 提交事务失败: %s\n",
                     conn.lastError().c_str());
        return false;
    }

    if (outSceneId != nullptr) {
        *outSceneId = sceneId;
    }
    return true;
}

bool SceneDao::insertNodesBatch(MySqlConnection& conn, std::int64_t sceneId,
                                const std::vector<SceneNodeRecord>& nodes) {
    if (nodes.empty()) {
        return true;
    }
    // 合并成多值 INSERT：一个场景上百个节点，逐条插就是上百次网络往返。
    // 这里按 kBatchRows 分批，避免单条 SQL 超过 max_allowed_packet。
    for (std::size_t start = 0; start < nodes.size(); start += kBatchRows) {
        const std::size_t end = std::min(start + kBatchRows, nodes.size());

        std::string sql =
            "INSERT INTO scene_nodes (scene_id, node_id, from_template, name, "
            "node_type, status, longitude, latitude, altitude, "
            "interference_db, comm_methods, device_params, device_connections) "
            "VALUES ";
        std::vector<Param> params;
        params.reserve((end - start) * 13);

        for (std::size_t i = start; i < end; ++i) {
            if (i > start) {
                sql += ",";
            }
            sql += "(?,?,?,?,?,?,?,?,?,?,?,?,?)";
            const SceneNodeRecord& n = nodes[i];
            params.push_back(Param::ofInt(sceneId));
            params.push_back(Param::ofInt(n.nodeId));
            params.push_back(Param::ofInt(n.fromTemplate));
            params.push_back(Param::ofString(n.name));
            params.push_back(Param::ofString(n.nodeType));
            params.push_back(Param::ofString(n.status));
            params.push_back(Param::ofDouble(n.longitude));
            params.push_back(Param::ofDouble(n.latitude));
            params.push_back(Param::ofDouble(n.altitude));
            params.push_back(Param::ofDouble(n.interferenceDb));
            params.push_back(Param::ofString(n.commMethods));
            params.push_back(Param::ofString(n.deviceParams));
            params.push_back(Param::ofString(n.deviceConnections));
        }

        if (!conn.executePrepared(sql, params)) {
            std::fprintf(stderr, "[SceneDao] 批量插入节点失败: %s\n",
                         conn.lastError().c_str());
            return false;
        }
    }
    return true;
}

bool SceneDao::insertLinksBatch(MySqlConnection& conn, std::int64_t sceneId,
                                const std::vector<SceneLinkRecord>& links) {
    if (links.empty()) {
        return true;
    }
    for (std::size_t start = 0; start < links.size(); start += kBatchRows) {
        const std::size_t end = std::min(start + kBatchRows, links.size());

        std::string sql =
            "INSERT INTO scene_links (scene_id, src, dst, from_template, "
            "link_type, wireless_type, bandwidth_bps, prop_delay_s, "
            "comm_protocol, device_type, flows) VALUES ";
        std::vector<Param> params;
        params.reserve((end - start) * 11);

        for (std::size_t i = start; i < end; ++i) {
            if (i > start) {
                sql += ",";
            }
            sql += "(?,?,?,?,?,?,?,?,?,?,?)";
            const SceneLinkRecord& l = links[i];
            params.push_back(Param::ofInt(sceneId));
            params.push_back(Param::ofInt(l.src));
            params.push_back(Param::ofInt(l.dst));
            params.push_back(Param::ofInt(l.fromTemplate));
            params.push_back(Param::ofString(l.linkType));
            params.push_back(Param::ofString(l.wirelessType));
            params.push_back(Param::ofDouble(l.bandwidthBps));
            params.push_back(Param::ofDouble(l.propDelayS));
            params.push_back(Param::ofString(l.commProtocol));
            params.push_back(Param::ofString(l.deviceType));
            params.push_back(Param::ofString(l.flows));
        }

        if (!conn.executePrepared(sql, params)) {
            std::fprintf(stderr, "[SceneDao] 批量插入链路失败: %s\n",
                         conn.lastError().c_str());
            return false;
        }
    }
    return true;
}

bool SceneDao::deleteScene(MySqlConnection& conn, std::int64_t sceneId) {
    // 子表由 ON DELETE CASCADE 自动清除，这里只删主表。
    // 外键约束保证了不可能出现"主表删了子表还在"的孤儿数据。
    return conn.executePrepared("DELETE FROM scenes WHERE id = ?",
                                {Param::ofInt(sceneId)});
}

// ── 读 ──────────────────────────────────────────────────────────────────────

bool SceneDao::listScenes(MySqlConnection& conn, std::vector<SceneRecord>& out) {
    out.clear();
    ResultSet rs;
    if (!conn.query(
            "SELECT id, name, description, scene_type, create_time FROM scenes "
            "ORDER BY id",
            rs)) {
        return false;
    }
    out.reserve(rs.rowCount());
    for (std::size_t i = 0; i < rs.rowCount(); ++i) {
        SceneRecord s;
        s.id = toI64(rs.at(i, 0));
        s.name = rs.at(i, 1);
        s.description = rs.at(i, 2);
        s.sceneType = rs.at(i, 3);
        s.createTime = rs.at(i, 4);
        out.push_back(std::move(s));
    }
    return true;
}

bool SceneDao::findSceneByName(MySqlConnection& conn, const std::string& name,
                               std::vector<SceneRecord>& out) {
    out.clear();
    ResultSet rs;
    if (!conn.queryPrepared(
            "SELECT id, name, description, scene_type, create_time FROM scenes "
            "WHERE name = ?",
            {Param::ofString(name)}, rs)) {
        return false;
    }
    for (std::size_t i = 0; i < rs.rowCount(); ++i) {
        SceneRecord s;
        s.id = toI64(rs.at(i, 0));
        s.name = rs.at(i, 1);
        s.description = rs.at(i, 2);
        s.sceneType = rs.at(i, 3);
        s.createTime = rs.at(i, 4);
        out.push_back(std::move(s));
    }
    return true;
}

bool SceneDao::loadNodes(MySqlConnection& conn, std::int64_t sceneId,
                         std::vector<SceneNodeRecord>& out) {
    out.clear();
    ResultSet rs;
    if (!conn.queryPrepared(
            "SELECT id, scene_id, node_id, from_template, name, node_type, "
            "status, longitude, latitude, altitude, interference_db, "
            "comm_methods, device_params, device_connections "
            "FROM scene_nodes WHERE scene_id = ? ORDER BY node_id",
            {Param::ofInt(sceneId)}, rs)) {
        return false;
    }
    out.reserve(rs.rowCount());
    for (std::size_t i = 0; i < rs.rowCount(); ++i) {
        SceneNodeRecord n;
        n.id = toI64(rs.at(i, 0));
        n.sceneId = toI64(rs.at(i, 1));
        n.nodeId = static_cast<int>(toI64(rs.at(i, 2)));
        n.fromTemplate = toI64(rs.at(i, 3));
        n.name = rs.at(i, 4);
        n.nodeType = rs.at(i, 5);
        n.status = rs.at(i, 6);
        n.longitude = toD(rs.at(i, 7));
        n.latitude = toD(rs.at(i, 8));
        n.altitude = toD(rs.at(i, 9));
        n.interferenceDb = toD(rs.at(i, 10));
        n.commMethods = rs.at(i, 11);
        n.deviceParams = rs.at(i, 12);
        n.deviceConnections = rs.at(i, 13);
        out.push_back(std::move(n));
    }
    return true;
}

bool SceneDao::loadLinks(MySqlConnection& conn, std::int64_t sceneId,
                         std::vector<SceneLinkRecord>& out) {
    out.clear();
    ResultSet rs;
    if (!conn.queryPrepared(
            "SELECT id, scene_id, src, dst, from_template, link_type, "
            "wireless_type, bandwidth_bps, prop_delay_s, comm_protocol, "
            "device_type, flows FROM scene_links WHERE scene_id = ? ORDER BY id",
            {Param::ofInt(sceneId)}, rs)) {
        return false;
    }
    out.reserve(rs.rowCount());
    for (std::size_t i = 0; i < rs.rowCount(); ++i) {
        SceneLinkRecord l;
        l.id = toI64(rs.at(i, 0));
        l.sceneId = toI64(rs.at(i, 1));
        l.src = static_cast<int>(toI64(rs.at(i, 2)));
        l.dst = static_cast<int>(toI64(rs.at(i, 3)));
        l.fromTemplate = toI64(rs.at(i, 4));
        l.linkType = rs.at(i, 5);
        l.wirelessType = rs.at(i, 6);
        l.bandwidthBps = toD(rs.at(i, 7));
        l.propDelayS = toD(rs.at(i, 8));
        l.commProtocol = rs.at(i, 9);
        l.deviceType = rs.at(i, 10);
        l.flows = rs.at(i, 11);
        out.push_back(std::move(l));
    }
    return true;
}

bool SceneDao::countNodes(MySqlConnection& conn, std::int64_t sceneId,
                          std::int64_t* out) {
    ResultSet rs;
    if (!conn.queryPrepared(
            "SELECT COUNT(*) FROM scene_nodes WHERE scene_id = ?",
            {Param::ofInt(sceneId)}, rs)) {
        return false;
    }
    *out = rs.rowCount() > 0 ? toI64(rs.at(0, 0)) : 0;
    return true;
}

bool SceneDao::countLinks(MySqlConnection& conn, std::int64_t sceneId,
                          std::int64_t* out) {
    ResultSet rs;
    if (!conn.queryPrepared(
            "SELECT COUNT(*) FROM scene_links WHERE scene_id = ?",
            {Param::ofInt(sceneId)}, rs)) {
        return false;
    }
    *out = rs.rowCount() > 0 ? toI64(rs.at(0, 0)) : 0;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// 单实体 CRUD（P6）
//
// P3 只做了"整场景原子保存/加载"，因为那时客户端还直连数据库、
// 服务端只需要承载规划任务。P6 客户端改走协议之后，
// 界面上每一次增删改都要能单独发过来，所以补齐这一层。
//
// 全部走预处理语句。参数直接来自网络、是不可信输入 ——
// 拼接 SQL 的话一个场景名写成 `'; DROP TABLE scenes; --` 就完了。
// 预处理语句把值和语句结构分开传给服务器，值永远不会被当作 SQL 解析。
// ═══════════════════════════════════════════════════════════════════════════

// ── 场景 ────────────────────────────────────────────────────────────────────

bool SceneDao::createScene(MySqlConnection& conn, const SceneRecord& s,
                           std::int64_t* outId) {
    if (!conn.executePrepared(
            "INSERT INTO scenes (name, description, scene_type, create_time) "
            "VALUES (?, ?, ?, ?)",
            {Param::ofString(s.name), Param::ofString(s.description),
             Param::ofString(s.sceneType), Param::ofString(s.createTime)})) {
        return false;
    }
    if (outId != nullptr) {
        *outId = static_cast<std::int64_t>(conn.lastInsertId());
    }
    return true;
}

bool SceneDao::updateScene(MySqlConnection& conn, const SceneRecord& s) {
    return conn.executePrepared(
        "UPDATE scenes SET name = ?, description = ?, scene_type = ? "
        "WHERE id = ?",
        {Param::ofString(s.name), Param::ofString(s.description),
         Param::ofString(s.sceneType), Param::ofInt(s.id)});
}

// ── 节点 ────────────────────────────────────────────────────────────────────

bool SceneDao::addNode(MySqlConnection& conn, const SceneNodeRecord& n,
                       std::int64_t* outId) {
    if (!conn.executePrepared(
            "INSERT INTO scene_nodes (scene_id, node_id, from_template, name, "
            "node_type, status, longitude, latitude, altitude, "
            "interference_db, comm_methods, device_params, device_connections) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {Param::ofInt(n.sceneId), Param::ofInt(n.nodeId),
             Param::ofInt(n.fromTemplate), Param::ofString(n.name),
             Param::ofString(n.nodeType), Param::ofString(n.status),
             Param::ofDouble(n.longitude), Param::ofDouble(n.latitude),
             Param::ofDouble(n.altitude), Param::ofDouble(n.interferenceDb),
             Param::ofString(n.commMethods), Param::ofString(n.deviceParams),
             Param::ofString(n.deviceConnections)})) {
        return false;
    }
    if (outId != nullptr) {
        *outId = static_cast<std::int64_t>(conn.lastInsertId());
    }
    return true;
}

bool SceneDao::addNodesBatch(MySqlConnection& conn, std::int64_t sceneId,
                             const std::vector<SceneNodeRecord>& nodes,
                             std::int64_t* outAffected) {
    if (nodes.empty()) {
        if (outAffected != nullptr) {
            *outAffected = 0;
        }
        return true;
    }
    // 事务包住：批量导入要么全进要么全不进。
    // 否则中途失败会留下"只导了一半节点"的场景，而且没有任何标记能识别它。
    Transaction tx(conn);
    if (!tx.begun()) {
        return false;
    }
    if (!insertNodesBatch(conn, sceneId, nodes)) {
        return false;  // Transaction 析构时自动回滚
    }
    if (!tx.commit()) {
        return false;
    }
    if (outAffected != nullptr) {
        *outAffected = static_cast<std::int64_t>(nodes.size());
    }
    return true;
}

bool SceneDao::updateNode(MySqlConnection& conn, const SceneNodeRecord& n) {
    return conn.executePrepared(
        "UPDATE scene_nodes SET node_id = ?, name = ?, node_type = ?, "
        "status = ?, longitude = ?, latitude = ?, altitude = ?, "
        "interference_db = ?, comm_methods = ?, device_params = ?, "
        "device_connections = ? WHERE id = ?",
        {Param::ofInt(n.nodeId), Param::ofString(n.name),
         Param::ofString(n.nodeType), Param::ofString(n.status),
         Param::ofDouble(n.longitude), Param::ofDouble(n.latitude),
         Param::ofDouble(n.altitude), Param::ofDouble(n.interferenceDb),
         Param::ofString(n.commMethods), Param::ofString(n.deviceParams),
         Param::ofString(n.deviceConnections), Param::ofInt(n.id)});
}

bool SceneDao::deleteNode(MySqlConnection& conn, std::int64_t nodeDbId) {
    return conn.executePrepared("DELETE FROM scene_nodes WHERE id = ?",
                                {Param::ofInt(nodeDbId)});
}

// ── 链路 ────────────────────────────────────────────────────────────────────

bool SceneDao::addLink(MySqlConnection& conn, const SceneLinkRecord& l,
                       std::int64_t* outId) {
    if (!conn.executePrepared(
            "INSERT INTO scene_links (scene_id, src, dst, from_template, "
            "link_type, wireless_type, bandwidth_bps, prop_delay_s, "
            "comm_protocol, device_type, flows) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
            {Param::ofInt(l.sceneId), Param::ofInt(l.src), Param::ofInt(l.dst),
             Param::ofInt(l.fromTemplate), Param::ofString(l.linkType),
             Param::ofString(l.wirelessType), Param::ofDouble(l.bandwidthBps),
             Param::ofDouble(l.propDelayS), Param::ofString(l.commProtocol),
             Param::ofString(l.deviceType), Param::ofString(l.flows)})) {
        return false;
    }
    if (outId != nullptr) {
        *outId = static_cast<std::int64_t>(conn.lastInsertId());
    }
    return true;
}

bool SceneDao::addLinksBatch(MySqlConnection& conn, std::int64_t sceneId,
                             const std::vector<SceneLinkRecord>& links,
                             std::int64_t* outAffected) {
    if (links.empty()) {
        if (outAffected != nullptr) {
            *outAffected = 0;
        }
        return true;
    }
    Transaction tx(conn);
    if (!tx.begun()) {
        return false;
    }
    if (!insertLinksBatch(conn, sceneId, links)) {
        return false;
    }
    if (!tx.commit()) {
        return false;
    }
    if (outAffected != nullptr) {
        *outAffected = static_cast<std::int64_t>(links.size());
    }
    return true;
}

bool SceneDao::updateLink(MySqlConnection& conn, const SceneLinkRecord& l) {
    return conn.executePrepared(
        "UPDATE scene_links SET src = ?, dst = ?, link_type = ?, "
        "wireless_type = ?, bandwidth_bps = ?, prop_delay_s = ?, "
        "comm_protocol = ?, device_type = ?, flows = ? WHERE id = ?",
        {Param::ofInt(l.src), Param::ofInt(l.dst), Param::ofString(l.linkType),
         Param::ofString(l.wirelessType), Param::ofDouble(l.bandwidthBps),
         Param::ofDouble(l.propDelayS), Param::ofString(l.commProtocol),
         Param::ofString(l.deviceType), Param::ofString(l.flows),
         Param::ofInt(l.id)});
}

bool SceneDao::deleteLink(MySqlConnection& conn, std::int64_t linkDbId) {
    return conn.executePrepared("DELETE FROM scene_links WHERE id = ?",
                                {Param::ofInt(linkDbId)});
}

bool SceneDao::clearLinksByScene(MySqlConnection& conn, std::int64_t sceneId,
                                 std::int64_t* outAffected) {
    if (!conn.executePrepared("DELETE FROM scene_links WHERE scene_id = ?",
                              {Param::ofInt(sceneId)})) {
        return false;
    }
    if (outAffected != nullptr) {
        *outAffected = static_cast<std::int64_t>(conn.affectedRows());
    }
    return true;
}

// ── 模板 ────────────────────────────────────────────────────────────────────

bool SceneDao::listNodeTemplates(MySqlConnection& conn,
                                 std::vector<NodeTemplateRecord>& out) {
    out.clear();
    ResultSet rs;
    if (!conn.query("SELECT id, template_name, node_type, comm_methods, "
                    "interference_db, description, device_params "
                    "FROM node_templates ORDER BY id",
                    rs)) {
        return false;
    }
    out.reserve(rs.rowCount());
    for (std::size_t i = 0; i < rs.rowCount(); ++i) {
        NodeTemplateRecord t;
        t.id = toI64(rs.at(i, 0));
        t.templateName = rs.at(i, 1);
        t.nodeType = rs.at(i, 2);
        t.commMethods = rs.at(i, 3);
        t.interferenceDb = toD(rs.at(i, 4));
        t.description = rs.at(i, 5);
        t.deviceParams = rs.at(i, 6);
        out.push_back(std::move(t));
    }
    return true;
}

bool SceneDao::saveNodeTemplate(MySqlConnection& conn,
                                const NodeTemplateRecord& t,
                                std::int64_t* outId) {
    if (!conn.executePrepared(
            "INSERT INTO node_templates (template_name, node_type, "
            "comm_methods, interference_db, description, device_params) "
            "VALUES (?,?,?,?,?,?)",
            {Param::ofString(t.templateName), Param::ofString(t.nodeType),
             Param::ofString(t.commMethods), Param::ofDouble(t.interferenceDb),
             Param::ofString(t.description),
             Param::ofString(t.deviceParams)})) {
        return false;
    }
    if (outId != nullptr) {
        *outId = static_cast<std::int64_t>(conn.lastInsertId());
    }
    return true;
}

bool SceneDao::listLinkTemplates(MySqlConnection& conn,
                                 std::vector<LinkTemplateRecord>& out) {
    out.clear();
    ResultSet rs;
    if (!conn.query("SELECT id, template_name, link_type, wireless_type, "
                    "bandwidth_bps, description FROM link_templates ORDER BY id",
                    rs)) {
        return false;
    }
    out.reserve(rs.rowCount());
    for (std::size_t i = 0; i < rs.rowCount(); ++i) {
        LinkTemplateRecord t;
        t.id = toI64(rs.at(i, 0));
        t.templateName = rs.at(i, 1);
        t.linkType = rs.at(i, 2);
        t.wirelessType = rs.at(i, 3);
        t.bandwidthBps = toD(rs.at(i, 4));
        t.description = rs.at(i, 5);
        out.push_back(std::move(t));
    }
    return true;
}

bool SceneDao::saveLinkTemplate(MySqlConnection& conn,
                                const LinkTemplateRecord& t,
                                std::int64_t* outId) {
    if (!conn.executePrepared(
            "INSERT INTO link_templates (template_name, link_type, "
            "wireless_type, bandwidth_bps, description) VALUES (?,?,?,?,?)",
            {Param::ofString(t.templateName), Param::ofString(t.linkType),
             Param::ofString(t.wirelessType), Param::ofDouble(t.bandwidthBps),
             Param::ofString(t.description)})) {
        return false;
    }
    if (outId != nullptr) {
        *outId = static_cast<std::int64_t>(conn.lastInsertId());
    }
    return true;
}

}  // namespace thgh
