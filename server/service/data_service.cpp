#include "service/data_service.h"

#include "db/scene_dao.h"
#include "service/json_codec.h"
#include "thgh/message.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace thgh {
namespace {

using nlohmann::json;

// ── 不抛异常的取值（与 json_codec.cpp 同样的理由）──────────────────────────
// 参数来自网络。nlohmann 的 get<T>() 类型不匹配就抛，异常穿过回调栈
// 就能把服务端打死。全部走 find() + 显式类型检查。

std::int64_t getI64(const json& j, std::string_view key, std::int64_t def = 0) {
    auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_number()) {
        return def;
    }
    return static_cast<std::int64_t>(it->get<double>());
}

double getD(const json& j, std::string_view key, double def = 0.0) {
    auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_number()) {
        return def;
    }
    return it->get<double>();
}

std::string getS(const json& j, std::string_view key,
                 const std::string& def = std::string()) {
    auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_string()) {
        return def;
    }
    return it->get<std::string>();
}

// ── 记录 ↔ JSON ─────────────────────────────────────────────────────────────
// 字段名与客户端 datamodel 一致。这些字符串目前只在本文件与客户端的
// RemoteDataStore 里出现，是两端必须对齐的一组约定。

json toJson(const SceneRecord& s) {
    json o;
    o["id"] = s.id;
    o["name"] = s.name;
    o["description"] = s.description;
    o["scene_type"] = s.sceneType;
    o["create_time"] = s.createTime;
    return o;
}

SceneRecord sceneFromJson(const json& j) {
    SceneRecord s;
    s.id = getI64(j, "id");
    s.name = getS(j, "name");
    s.description = getS(j, "description");
    s.sceneType = getS(j, "scene_type");
    s.createTime = getS(j, "create_time");
    return s;
}

json toJson(const SceneNodeRecord& n) {
    json o;
    o["id"] = n.id;
    o["scene_id"] = n.sceneId;
    o["node_id"] = n.nodeId;
    o["from_template"] = n.fromTemplate;
    o["name"] = n.name;
    o["node_type"] = n.nodeType;
    o["status"] = n.status;
    o["longitude"] = n.longitude;
    o["latitude"] = n.latitude;
    o["altitude"] = n.altitude;
    o["interference_db"] = n.interferenceDb;
    o["comm_methods"] = n.commMethods;
    o["device_params"] = n.deviceParams;
    o["device_connections"] = n.deviceConnections;
    return o;
}

SceneNodeRecord nodeFromJson(const json& j) {
    SceneNodeRecord n;
    n.id = getI64(j, "id");
    n.sceneId = getI64(j, "scene_id");
    n.nodeId = static_cast<int>(getI64(j, "node_id"));
    n.fromTemplate = getI64(j, "from_template");
    n.name = getS(j, "name");
    n.nodeType = getS(j, "node_type");
    n.status = getS(j, "status", "在线");
    n.longitude = getD(j, "longitude");
    n.latitude = getD(j, "latitude");
    n.altitude = getD(j, "altitude");
    n.interferenceDb = getD(j, "interference_db");
    n.commMethods = getS(j, "comm_methods");
    n.deviceParams = getS(j, "device_params");
    n.deviceConnections = getS(j, "device_connections");
    return n;
}

json toJson(const SceneLinkRecord& l) {
    json o;
    o["id"] = l.id;
    o["scene_id"] = l.sceneId;
    o["src"] = l.src;
    o["dst"] = l.dst;
    o["from_template"] = l.fromTemplate;
    o["link_type"] = l.linkType;
    o["wireless_type"] = l.wirelessType;
    o["bandwidth_bps"] = l.bandwidthBps;
    o["prop_delay_s"] = l.propDelayS;
    o["comm_protocol"] = l.commProtocol;
    o["device_type"] = l.deviceType;
    o["flows"] = l.flows;
    return o;
}

SceneLinkRecord linkFromJson(const json& j) {
    SceneLinkRecord l;
    l.id = getI64(j, "id");
    l.sceneId = getI64(j, "scene_id");
    l.src = static_cast<int>(getI64(j, "src"));
    l.dst = static_cast<int>(getI64(j, "dst"));
    l.fromTemplate = getI64(j, "from_template");
    l.linkType = getS(j, "link_type");
    l.wirelessType = getS(j, "wireless_type");
    l.bandwidthBps = getD(j, "bandwidth_bps");
    l.propDelayS = getD(j, "prop_delay_s");
    l.commProtocol = getS(j, "comm_protocol");
    l.deviceType = getS(j, "device_type");
    l.flows = getS(j, "flows");
    return l;
}

json toJson(const NodeTemplateRecord& t) {
    json o;
    o["id"] = t.id;
    o["template_name"] = t.templateName;
    o["node_type"] = t.nodeType;
    o["comm_methods"] = t.commMethods;
    o["interference_db"] = t.interferenceDb;
    o["description"] = t.description;
    o["device_params"] = t.deviceParams;
    return o;
}

json toJson(const LinkTemplateRecord& t) {
    json o;
    o["id"] = t.id;
    o["template_name"] = t.templateName;
    o["link_type"] = t.linkType;
    o["wireless_type"] = t.wirelessType;
    o["bandwidth_bps"] = t.bandwidthBps;
    o["description"] = t.description;
    return o;
}

// ── 应答封装 ────────────────────────────────────────────────────────────────

std::string makeReply(std::uint64_t taskId, bool ok, json data,
                      const std::string& error) {
    json payload;
    payload[std::string(field::data::kOk)] = ok;
    if (ok) {
        payload[std::string(field::data::kData)] = std::move(data);
    } else {
        payload[std::string(field::data::kError)] = error;
    }
    json j;
    j[std::string(field::kType)] =
        std::string(toString(MessageType::DataReply));
    j[std::string(field::kTaskId)] = taskId;
    j[std::string(field::kPayload)] = std::move(payload);
    return j.dump();
}

std::string okReply(std::uint64_t taskId, json data) {
    return makeReply(taskId, true, std::move(data), std::string());
}

std::string errReply(std::uint64_t taskId, const std::string& msg) {
    return makeReply(taskId, false, json::object(), msg);
}

// 批量参数解析：args.items 必须是数组
template <typename T, typename Fn>
bool parseItems(const json& args, Fn&& fromJson, std::vector<T>& out,
                std::string& err) {
    auto it = args.find(std::string(field::data::kItems));
    if (it == args.end() || !it->is_array()) {
        err = "批量操作缺少 items 数组";
        return false;
    }
    out.reserve(it->size());
    for (const json& e : *it) {
        if (!e.is_object()) {
            continue;
        }
        out.push_back(fromJson(e));
    }
    return true;
}

}  // namespace

DataService::DataService(ComputePool* pool, ConnectionPool* dbPool)
    : m_pool(pool), m_dbPool(dbPool) {}

// ── IO 线程：解析并派发 ─────────────────────────────────────────────────────

bool DataService::handle(const TcpConnectionPtr& conn, std::uint64_t taskId,
                         const std::string& payloadJson) {
    m_requests.fetch_add(1, std::memory_order_relaxed);

    const json p = json::parse(payloadJson, nullptr, false);
    if (p.is_discarded() || !p.is_object()) {
        m_failed.fetch_add(1, std::memory_order_relaxed);
        conn->send(errReply(taskId, "data_request 的 payload 不是合法 JSON 对象"));
        return true;
    }

    const std::string op = getS(p, field::data::kOp);
    if (op.empty()) {
        m_failed.fetch_add(1, std::memory_order_relaxed);
        conn->send(errReply(taskId, "缺少 op 字段"));
        return true;
    }

    // args 允许缺省（scene.list 这类不需要参数）
    std::string argsJson = "{}";
    auto ait = p.find(std::string(field::data::kArgs));
    if (ait != p.end() && ait->is_object()) {
        argsJson = ait->dump();
    }

    auto weakConn = std::weak_ptr<TcpConnection>(conn);
    const bool submitted = m_pool->submit(
        [this, weakConn, taskId, op, argsJson = std::move(argsJson)]() mutable {
            execute(weakConn, taskId, std::move(op), std::move(argsJson));
        });

    if (!submitted) {
        m_rejected.fetch_add(1, std::memory_order_relaxed);
        conn->send(errReply(taskId, "服务端繁忙，请稍后重试"));
    }
    return true;
}

// ── 计算线程：执行数据库操作 ────────────────────────────────────────────────

void DataService::execute(const std::weak_ptr<TcpConnection>& weakConn,
                          std::uint64_t taskId, std::string op,
                          std::string argsJson) {
    auto alive = [&weakConn]() -> TcpConnectionPtr {
        auto c = weakConn.lock();
        return (c && c->connected()) ? c : nullptr;
    };
    if (!alive()) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const json args = json::parse(argsJson, nullptr, false);
    const json emptyArgs = json::object();
    const json& a = (args.is_discarded() || !args.is_object()) ? emptyArgs : args;

    // 从池里借连接。借不到就是数据库真的出问题了或者压力过大，
    // 直接把原因告诉客户端，别让它干等。
    PooledConnection pc = m_dbPool->acquire();
    if (!pc.valid()) {
        m_failed.fetch_add(1, std::memory_order_relaxed);
        if (auto c = alive()) {
            c->send(errReply(taskId, "获取数据库连接超时，服务端可能过载"));
        }
        return;
    }
    MySqlConnection& db = *pc;

    json data = json::object();
    std::string err;
    bool ok = false;

    // ── 场景 ────────────────────────────────────────────────────────────
    if (op == field::op::kSceneList) {
        std::vector<SceneRecord> rows;
        ok = SceneDao::listScenes(db, rows);
        if (ok) {
            json items = json::array();
            for (const auto& r : rows) {
                items.push_back(toJson(r));
            }
            data[std::string(field::data::kItems)] = std::move(items);
        }

    } else if (op == field::op::kSceneCreate) {
        std::int64_t id = 0;
        ok = SceneDao::createScene(db, sceneFromJson(a), &id);
        data[std::string(field::data::kId)] = id;

    } else if (op == field::op::kSceneUpdate) {
        ok = SceneDao::updateScene(db, sceneFromJson(a));

    } else if (op == field::op::kSceneDelete) {
        ok = SceneDao::deleteScene(db, getI64(a, "id"));

    // ── 节点 ────────────────────────────────────────────────────────────
    } else if (op == field::op::kNodeList) {
        std::vector<SceneNodeRecord> rows;
        ok = SceneDao::loadNodes(db, getI64(a, "scene_id"), rows);
        if (ok) {
            json items = json::array();
            for (const auto& r : rows) {
                items.push_back(toJson(r));
            }
            data[std::string(field::data::kItems)] = std::move(items);
        }

    } else if (op == field::op::kNodeAdd) {
        std::int64_t id = 0;
        ok = SceneDao::addNode(db, nodeFromJson(a), &id);
        data[std::string(field::data::kId)] = id;

    } else if (op == field::op::kNodeAddBatch) {
        std::vector<SceneNodeRecord> rows;
        if (parseItems(a, nodeFromJson, rows, err)) {
            std::int64_t n = 0;
            ok = SceneDao::addNodesBatch(db, getI64(a, "scene_id"), rows, &n);
            data[std::string(field::data::kAffected)] = n;
        }

    } else if (op == field::op::kNodeUpdate) {
        ok = SceneDao::updateNode(db, nodeFromJson(a));

    } else if (op == field::op::kNodeDelete) {
        ok = SceneDao::deleteNode(db, getI64(a, "id"));

    // ── 链路 ────────────────────────────────────────────────────────────
    } else if (op == field::op::kLinkList) {
        std::vector<SceneLinkRecord> rows;
        ok = SceneDao::loadLinks(db, getI64(a, "scene_id"), rows);
        if (ok) {
            json items = json::array();
            for (const auto& r : rows) {
                items.push_back(toJson(r));
            }
            data[std::string(field::data::kItems)] = std::move(items);
        }

    } else if (op == field::op::kLinkAdd) {
        std::int64_t id = 0;
        ok = SceneDao::addLink(db, linkFromJson(a), &id);
        data[std::string(field::data::kId)] = id;

    } else if (op == field::op::kLinkAddBatch) {
        std::vector<SceneLinkRecord> rows;
        if (parseItems(a, linkFromJson, rows, err)) {
            std::int64_t n = 0;
            ok = SceneDao::addLinksBatch(db, getI64(a, "scene_id"), rows, &n);
            data[std::string(field::data::kAffected)] = n;
        }

    } else if (op == field::op::kLinkUpdate) {
        ok = SceneDao::updateLink(db, linkFromJson(a));

    } else if (op == field::op::kLinkDelete) {
        ok = SceneDao::deleteLink(db, getI64(a, "id"));

    } else if (op == field::op::kLinkClearByScene) {
        std::int64_t n = 0;
        ok = SceneDao::clearLinksByScene(db, getI64(a, "scene_id"), &n);
        data[std::string(field::data::kAffected)] = n;

    // ── 模板 ────────────────────────────────────────────────────────────
    } else if (op == field::op::kNodeTemplateList) {
        std::vector<NodeTemplateRecord> rows;
        ok = SceneDao::listNodeTemplates(db, rows);
        if (ok) {
            json items = json::array();
            for (const auto& r : rows) {
                items.push_back(toJson(r));
            }
            data[std::string(field::data::kItems)] = std::move(items);
        }

    } else if (op == field::op::kNodeTemplateSave) {
        NodeTemplateRecord t;
        t.templateName = getS(a, "template_name");
        t.nodeType = getS(a, "node_type");
        t.commMethods = getS(a, "comm_methods");
        t.interferenceDb = getD(a, "interference_db");
        t.description = getS(a, "description");
        t.deviceParams = getS(a, "device_params");
        std::int64_t id = 0;
        ok = SceneDao::saveNodeTemplate(db, t, &id);
        data[std::string(field::data::kId)] = id;

    } else if (op == field::op::kLinkTemplateList) {
        std::vector<LinkTemplateRecord> rows;
        ok = SceneDao::listLinkTemplates(db, rows);
        if (ok) {
            json items = json::array();
            for (const auto& r : rows) {
                items.push_back(toJson(r));
            }
            data[std::string(field::data::kItems)] = std::move(items);
        }

    } else if (op == field::op::kLinkTemplateSave) {
        LinkTemplateRecord t;
        t.templateName = getS(a, "template_name");
        t.linkType = getS(a, "link_type");
        t.wirelessType = getS(a, "wireless_type");
        t.bandwidthBps = getD(a, "bandwidth_bps", 50e6);
        t.description = getS(a, "description");
        std::int64_t id = 0;
        ok = SceneDao::saveLinkTemplate(db, t, &id);
        data[std::string(field::data::kId)] = id;

    } else {
        err = "不支持的操作：" + op;
    }

    if (err.empty() && !ok) {
        // 把数据库的原始错误带上。这是客户端排查问题的唯一线索 ——
        // 只回一句"操作失败"等于什么也没说。
        err = "数据库操作失败：" + db.lastError();
    }

    auto conn = alive();
    if (!conn) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ok) {
        m_succeeded.fetch_add(1, std::memory_order_relaxed);
        conn->send(okReply(taskId, std::move(data)));
    } else {
        m_failed.fetch_add(1, std::memory_order_relaxed);
        conn->send(errReply(taskId, err));
    }
}

DataService::Stats DataService::stats() const {
    Stats s;
    s.requests = m_requests.load(std::memory_order_relaxed);
    s.succeeded = m_succeeded.load(std::memory_order_relaxed);
    s.failed = m_failed.load(std::memory_order_relaxed);
    s.rejected = m_rejected.load(std::memory_order_relaxed);
    s.dropped = m_dropped.load(std::memory_order_relaxed);
    return s;
}

}  // namespace thgh
