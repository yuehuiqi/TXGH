#include "service/json_codec.h"

#include <nlohmann/json.hpp>

#include <string>

namespace thgh {
namespace {

using nlohmann::json;

// ── 不抛异常的取值辅助 ──────────────────────────────────────────────────────
// 报文来自网络，字段缺失或类型不对是常态而非异常。
// nlohmann 的 j["k"].get<T>() 在类型不匹配时会抛，这里一律走
// value() + 显式类型检查，把"取不到"变成"用默认值"。

double getNum(const json& j, std::string_view key, double def) {
    auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_number()) {
        return def;
    }
    return it->get<double>();
}

int getInt(const json& j, std::string_view key, int def) {
    auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_number()) {
        return def;
    }
    // 先取 double 再截断：客户端可能把整数序列化成 12.0，
    // 直接 get<int>() 在 nlohmann 里对浮点会抛
    return static_cast<int>(it->get<double>());
}

std::string getStr(const json& j, std::string_view key,
                   const std::string& def = std::string()) {
    auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_string()) {
        return def;
    }
    return it->get<std::string>();
}

const json* getArray(const json& j, std::string_view key) {
    auto it = j.find(std::string(key));
    if (it == j.end() || !it->is_array()) {
        return nullptr;
    }
    return &(*it);
}

// 通信方式字段兼容两种形态：字符串数组，或逗号分隔的单字符串。
// 老版本客户端发的是后者 —— 协议演进时不能要求两端同时升级。
std::vector<std::string> parseCommMethods(const json& node) {
    std::vector<std::string> out;
    auto it = node.find("comm_methods");
    if (it == node.end()) {
        return out;
    }
    if (it->is_array()) {
        for (const json& m : *it) {
            if (m.is_string()) {
                out.push_back(m.get<std::string>());
            }
        }
    } else if (it->is_string()) {
        const std::string s = it->get<std::string>();
        std::string cur;
        for (char c : s) {
            if (c == ',' || c == ';') {
                if (!cur.empty()) {
                    out.push_back(cur);
                    cur.clear();
                }
            } else if (c != ' ') {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            out.push_back(cur);
        }
    }
    return out;
}

}  // namespace

// ── 解析 ────────────────────────────────────────────────────────────────────

Envelope parseEnvelope(const std::string& line) {
    Envelope env;
    // accept=false, allow_exceptions=false：畸形输入返回 discarded 而不是抛。
    // 一条畸形报文本来可以直接打死进程，这个参数是那道防线。
    const json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return env;
    }
    env.wellFormed = true;
    env.type = messageTypeFromString(getStr(j, field::kType));
    // 关联 id 取 double 再转：JSON 数值统一是 double，
    // 直接 get<uint64_t>() 遇到 12.0 这种写法会抛
    env.taskId = static_cast<std::uint64_t>(
        getNum(j, field::kTaskId, 0.0));

    auto it = j.find(std::string(field::kPayload));
    if (it != j.end()) {
        env.payload = it->dump();
    }
    return env;
}

bool parsePlanRequest(const std::string& payloadJson, PlanRequest& out,
                      std::string& errorOut) {
    const json j = json::parse(payloadJson, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        errorOut = "payload 不是合法的 JSON 对象";
        return false;
    }

    out.sceneId = getInt(j, "scene_id", -1);

    // ── 节点 ────────────────────────────────────────────────────────────
    const json* nodes = getArray(j, "nodes");
    if (nodes == nullptr) {
        errorOut = "缺少 nodes 数组";
        return false;
    }
    out.nodes.reserve(nodes->size());
    for (const json& n : *nodes) {
        if (!n.is_object()) {
            continue;
        }
        PlanNode pn;
        pn.id = getInt(n, "id", -1);
        if (pn.id < 0) {
            errorOut = "存在缺少 id 的节点";
            return false;
        }
        pn.nodeType = getStr(n, "node_type", "干线");
        pn.longitude = getNum(n, "longitude", 0.0);
        pn.latitude = getNum(n, "latitude", 0.0);
        pn.altitude = getNum(n, "altitude", 0.0);
        pn.commMethods = parseCommMethods(n);
        out.nodes.push_back(std::move(pn));
    }
    if (out.nodes.empty()) {
        errorOut = "nodes 为空，没有可规划的节点";
        return false;
    }

    // ── 业务流 ──────────────────────────────────────────────────────────
    const json* flows = getArray(j, "flows");
    if (flows == nullptr) {
        errorOut = "缺少 flows 数组";
        return false;
    }
    out.flows.reserve(flows->size());
    for (const json& f : *flows) {
        if (!f.is_object()) {
            continue;
        }
        PlanFlow pf;
        pf.fid = getInt(f, "fid", -1);
        if (pf.fid < 0) {
            errorOut = "存在缺少 fid 的业务流";
            return false;
        }
        pf.src = getInt(f, "src_node_id", -1);
        pf.dst = getInt(f, "dst_node_id", -1);
        pf.rateBps = getNum(f, "rate_bps", 0.0);
        pf.qosLevel = getInt(f, "qos_level", 2);
        out.flows.push_back(pf);
    }
    if (out.flows.empty()) {
        errorOut = "flows 为空，没有需要规划的业务流";
        return false;
    }

    // ── 配置（可选，缺省用默认值）────────────────────────────────────
    auto cit = j.find("config");
    if (cit != j.end() && cit->is_object()) {
        out.config.commRangeKm = getNum(*cit, "comm_range_km", out.config.commRangeKm);
        out.config.pathsPerFlow = getInt(*cit, "paths_per_flow", out.config.pathsPerFlow);
    }
    // 防御：这两个值直接决定计算量，客户端传个负数或超大值会让服务端空转。
    // 用不可信输入做循环上界，是很典型的远程资源耗尽面。
    if (out.config.commRangeKm <= 0.0 || out.config.commRangeKm > 40000.0) {
        out.config.commRangeKm = PlanConfig{}.commRangeKm;
    }
    if (out.config.pathsPerFlow < 1 || out.config.pathsPerFlow > 32) {
        out.config.pathsPerFlow = PlanConfig{}.pathsPerFlow;
    }

    return true;
}

// ── 编码 ────────────────────────────────────────────────────────────────────

namespace {

// 统一的信封封装。字段名一律取自 protocol/thgh/message.h，
// 不在这里写字面量 —— 那正是这个共享头文件存在的意义。
std::string wrap(MessageType type, json payload, std::uint64_t taskId) {
    json j;
    j[std::string(field::kType)] = std::string(toString(type));
    if (taskId != 0) {
        j[std::string(field::kTaskId)] = taskId;
    }
    j[std::string(field::kPayload)] = std::move(payload);
    return j.dump();
}

}  // namespace

std::string encodeAck(std::uint64_t taskId, int nodeCount, int flowCount) {
    json p;
    p[std::string(field::ack::kNodeCount)] = nodeCount;
    p[std::string(field::ack::kFlowCount)] = flowCount;
    return wrap(MessageType::Ack, std::move(p), taskId);
}

std::string encodeProgress(std::uint64_t taskId, int percent,
                           const std::string& stage) {
    json p;
    p[std::string(field::progress::kPercent)] = percent;
    p[std::string(field::progress::kStage)] = stage;
    return wrap(MessageType::Progress, std::move(p), taskId);
}

std::string encodePlanResult(std::uint64_t taskId, const PlanResult& r) {
    json links = json::array();
    for (const LinkResult& l : r.links) {
        json o;
        o[std::string(field::link::kLinkId)] = l.linkId;
        o[std::string(field::link::kSrcNodeId)] = l.srcNodeId;
        o[std::string(field::link::kDstNodeId)] = l.dstNodeId;
        o[std::string(field::link::kLinkType)] = l.linkType;
        o[std::string(field::link::kWirelessType)] = l.wirelessType;
        o[std::string(field::link::kBandwidthBps)] = l.bandwidthBps;
        o[std::string(field::link::kPropDelayS)] = l.propDelayS;
        json flows = json::array();
        for (const LinkFlowResult& f : l.flows) {
            json fo;
            fo[std::string(field::linkflow::kFid)] = f.fid;
            fo[std::string(field::linkflow::kBandwidthBps)] = f.bandwidthBps;
            fo[std::string(field::linkflow::kDescription)] = f.description;
            flows.push_back(std::move(fo));
        }
        o[std::string(field::link::kFlows)] = std::move(flows);
        links.push_back(std::move(o));
    }

    json results = json::array();
    for (const PlanPathResult& p : r.planResults) {
        json o;
        o[std::string(field::planresult::kFid)] = p.fid;
        o[std::string(field::planresult::kSrcNodeId)] = p.srcNodeId;
        o[std::string(field::planresult::kDstNodeId)] = p.dstNodeId;
        o[std::string(field::planresult::kIsSatisfied)] = p.isSatisfied;
        o[std::string(field::planresult::kHops)] = p.hops;
        o[std::string(field::planresult::kActualBandwidthBps)] =
            p.actualBandwidthBps;
        o[std::string(field::planresult::kUnsatisfiedReason)] =
            p.unsatisfiedReason;
        o[std::string(field::planresult::kPathNodes)] = p.pathNodes;
        o[std::string(field::planresult::kPathLinks)] = p.pathLinks;
        results.push_back(std::move(o));
    }

    json warns = json::array();
    for (const WarnItem& w : r.warns) {
        json o;
        o[std::string(field::warn::kLevel)] = w.level;
        o[std::string(field::warn::kMessage)] = w.message;
        o[std::string(field::warn::kWarnType)] = w.warnType;
        o[std::string(field::warn::kRelatedFid)] = w.relatedFid;
        warns.push_back(std::move(o));
    }

    json p;
    p[std::string(field::result::kLinks)] = std::move(links);
    p[std::string(field::result::kPlanResults)] = std::move(results);
    p[std::string(field::result::kWarns)] = std::move(warns);
    p["elapsed_s"] = r.elapsedSec;
    return wrap(MessageType::PlanResult, std::move(p), taskId);
}

std::string encodeError(const std::string& message, std::uint64_t taskId) {
    json j;
    j[std::string(field::kType)] = std::string(toString(MessageType::Error));
    if (taskId != 0) {
        j[std::string(field::kTaskId)] = taskId;
    }
    // 错误消息的文本放在顶层 message，不在 payload 里 ——
    // 与改造前的协议保持一致，客户端解析代码不用改
    j[std::string(field::kMessage)] = message;
    return j.dump();
}

std::string encodeHeartbeatAck() {
    json j;
    j[std::string(field::kType)] =
        std::string(toString(MessageType::HeartbeatAck));
    return j.dump();
}

}  // namespace thgh
