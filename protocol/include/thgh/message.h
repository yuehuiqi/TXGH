#ifndef THGH_PROTOCOL_MESSAGE_H
#define THGH_PROTOCOL_MESSAGE_H

// ─────────────────────────────────────────────────────────────────────────────
// 协议消息类型与字段名定义
//
// ── 这个文件存在的唯一理由 ────────────────────────────────────────────────
// 客户端和服务端必须对同一套字符串达成一致。如果两边各写各的字面量：
//
//     客户端：obj["type"] == "plan_result"
//     服务端：j["type"] = "plan_results";      // ← 多了个 s
//
// 编译器不会报错、单测各自也都能过，**只有联调时才会发现**，
// 而且症状是"消息发出去了但对面没反应"，极难定位。
// 把字符串收口到一处，再配合 MessageType 枚举，这类问题在编译期就没了。
//
// ── 为什么不在这里做 JSON 序列化 ──────────────────────────────────────────
// 客户端是 Qt 程序、已经在用 QJsonDocument；服务端用 nlohmann/json。
// 把 JSON 库塞进共享层等于强迫客户端再引入一个 JSON 依赖。
// 两端各用各的库，但字段名一律取自这里，一致性照样有保证。
// 代价是本层不校验 JSON 结构 —— 那是各端自己的事。
// ─────────────────────────────────────────────────────────────────────────────

#include <string_view>

namespace thgh {

// ── 消息类型 ────────────────────────────────────────────────────────────────
enum class MessageType {
    Unknown,

    // ── 规划任务（改造前已有）──────────────────────────────────────────
    StartPlan,   // 客户端 → 服务端：发起规划，载荷含场景/节点/设备/业务流
    PlanResult,  // 服务端 → 客户端：规划结果（链路、路径、告警）
    Error,       // 服务端 → 客户端：错误

    // ── 分阶段推送（P5 新增）──────────────────────────────────────────
    // 改造前是"一次请求一次响应"，规划耗时长时客户端界面完全没有反馈。
    // 升级为 Ack → Progress×N → PlanResult 三阶段。
    Ack,       // 服务端 → 客户端：已收到请求并开始处理（附任务 id）
    Progress,  // 服务端 → 客户端：阶段性进度

    // ── 数据访问（P6 新增）────────────────────────────────────────────
    // 改造前客户端直连数据库。多客户端同时开着时，各自读到的是自己那一刻的
    // 快照，谁后保存谁覆盖，而且数据库连接串和账号密码要发给每一台客户机 ——
    // 既没法保证一致性，也没法做权限控制。
    // 改造后数据访问全部收归服务端，客户端只发协议请求。
    DataRequest,  // 客户端 → 服务端：一次 CRUD 操作
    DataReply,    // 服务端 → 客户端：操作结果

    // ── 连接保活（P2 新增）────────────────────────────────────────────
    // 服务端需要主动踢除半开连接，否则死连接会一直占着 fd 和内存。
    Heartbeat,     // 双向：心跳探测
    HeartbeatAck,  // 双向：心跳应答
};

// 字符串 ↔ 枚举。两个方向都提供，避免各处散落 if-else 比较字面量。
MessageType messageTypeFromString(std::string_view s) noexcept;
std::string_view toString(MessageType t) noexcept;

// ── 顶层字段 ────────────────────────────────────────────────────────────────
// 每条消息的外层结构固定为：
//     {"type": "...", "payload": {...}}
// 错误消息额外带 "message"，分阶段推送额外带 "task_id"。
namespace field {

inline constexpr std::string_view kType = "type";
inline constexpr std::string_view kPayload = "payload";
inline constexpr std::string_view kMessage = "message";
// 关联 id。名字沿用 task_id（P5 已在用），语义就是"把请求和它的若干条响应
// 对应起来"。规划任务和数据请求共用这一个概念，不再另起一个字段 ——
// 同一个概念在协议里有两个名字，只会让两端都写两套判断。
inline constexpr std::string_view kTaskId = "task_id";

// ── Ack 载荷 ────────────────────────────────────────────────────────────
// 回显服务端**实际解析到**的规模，而不只是回一个空的"收到了"。
// 客户端可以据此立刻发现"我发了 150 个节点，服务端只认出 3 个"这类问题 ——
// 否则要等到最终结果出来看着不对，才回头怀疑是不是报文没组对。
namespace ack {
inline constexpr std::string_view kNodeCount = "node_count";
inline constexpr std::string_view kFlowCount = "flow_count";
}  // namespace ack

// ── Progress 载荷 ───────────────────────────────────────────────────────
namespace progress {
inline constexpr std::string_view kStage = "stage";        // 阶段名
inline constexpr std::string_view kPercent = "percent";    // 0~100
inline constexpr std::string_view kDetail = "detail";      // 可选描述
}  // namespace progress

// ── 数据访问（P6）───────────────────────────────────────────────────────
//
// 请求：{"type":"data_request","task_id":N,
//        "payload":{"op":"node.list","args":{...}}}
// 响应：{"type":"data_reply","task_id":N,
//        "payload":{"ok":true,"data":{...}}}
//   失败时 ok=false，"error" 给出可排查的原因（会原样显示给用户）。
namespace data {
inline constexpr std::string_view kOp = "op";
inline constexpr std::string_view kArgs = "args";
inline constexpr std::string_view kOk = "ok";
inline constexpr std::string_view kError = "error";
inline constexpr std::string_view kData = "data";
// 列表类操作统一把结果放在 data.items 里，写入类放 data.id / data.affected
inline constexpr std::string_view kItems = "items";
inline constexpr std::string_view kId = "id";
inline constexpr std::string_view kAffected = "affected";
}  // namespace data

// 操作名。两端共用同一份定义 —— 这类字符串一旦两边各写各的，
// 症状就是"客户端发出去了、服务端回不支持的操作"，只有联调才发现。
namespace op {
inline constexpr std::string_view kSceneList = "scene.list";
inline constexpr std::string_view kSceneCreate = "scene.create";
inline constexpr std::string_view kSceneUpdate = "scene.update";
inline constexpr std::string_view kSceneDelete = "scene.delete";

inline constexpr std::string_view kNodeList = "node.list";
inline constexpr std::string_view kNodeAdd = "node.add";
// ★ 批量新增。客户端导入场景时原来是 for 循环逐个 addNode，
//   走网络就是 N 次往返 —— 500 个节点直接把导入变成不可用。
//   批量接口让它变回一次往返，服务端侧也能合并成一条多值 INSERT。
inline constexpr std::string_view kNodeAddBatch = "node.addBatch";
inline constexpr std::string_view kNodeUpdate = "node.update";
inline constexpr std::string_view kNodeDelete = "node.delete";

inline constexpr std::string_view kLinkList = "link.list";
inline constexpr std::string_view kLinkAdd = "link.add";
inline constexpr std::string_view kLinkAddBatch = "link.addBatch";
inline constexpr std::string_view kLinkUpdate = "link.update";
inline constexpr std::string_view kLinkDelete = "link.delete";
inline constexpr std::string_view kLinkClearByScene = "link.clearByScene";

inline constexpr std::string_view kNodeTemplateList = "nodeTemplate.list";
inline constexpr std::string_view kNodeTemplateSave = "nodeTemplate.save";
inline constexpr std::string_view kLinkTemplateList = "linkTemplate.list";
inline constexpr std::string_view kLinkTemplateSave = "linkTemplate.save";
}  // namespace op

// ── PlanResult 载荷 ─────────────────────────────────────────────────────
// 字段名与改造前 Python 服务端的输出保持完全一致，
// 这样客户端的解析代码不用改，改造可以分阶段推进而不是一次性大爆炸。
namespace result {
inline constexpr std::string_view kLinks = "links";
inline constexpr std::string_view kPlanResults = "plan_results";
inline constexpr std::string_view kWarns = "warns";
}  // namespace result

// links[] 元素
namespace link {
inline constexpr std::string_view kLinkId = "link_id";
inline constexpr std::string_view kSrcNodeId = "src_node_id";
inline constexpr std::string_view kDstNodeId = "dst_node_id";
inline constexpr std::string_view kLinkType = "link_type";
inline constexpr std::string_view kWirelessType = "wireless_type";
inline constexpr std::string_view kBandwidthBps = "bandwidth_bps";
inline constexpr std::string_view kPropDelayS = "prop_delay_s";
inline constexpr std::string_view kFlows = "flows";
}  // namespace link

// links[].flows[] 元素
namespace linkflow {
inline constexpr std::string_view kFid = "fid";
inline constexpr std::string_view kBandwidthBps = "bandwidth_bps";
inline constexpr std::string_view kDescription = "description";
}  // namespace linkflow

// plan_results[] 元素
namespace planresult {
inline constexpr std::string_view kFid = "fid";
inline constexpr std::string_view kSrcNodeId = "src_node_id";
inline constexpr std::string_view kDstNodeId = "dst_node_id";
inline constexpr std::string_view kIsSatisfied = "is_satisfied";
inline constexpr std::string_view kHops = "hops";
inline constexpr std::string_view kActualBandwidthBps = "actual_bandwidth_bps";
inline constexpr std::string_view kUnsatisfiedReason = "unsatisfied_reason";
inline constexpr std::string_view kPathNodes = "path_nodes";
inline constexpr std::string_view kPathLinks = "path_links";
}  // namespace planresult

// warns[] 元素
namespace warn {
inline constexpr std::string_view kLevel = "level";
inline constexpr std::string_view kMessage = "message";
inline constexpr std::string_view kWarnType = "warn_type";
inline constexpr std::string_view kRelatedFid = "related_fid";
}  // namespace warn

}  // namespace field

// ── 协议约束 ────────────────────────────────────────────────────────────────
namespace limits {

// 单条消息最大字节数，与 LineFramer::Options::maxLineBytes 默认值保持一致。
// 规划请求要带整个场景的节点与链路，报文可以很大，但不该到兆级以上。
inline constexpr std::size_t kMaxMessageBytes = 1u << 20;

// 心跳间隔与超时。超时取间隔的 3 倍：允许偶发丢包或调度抖动导致的
// 一两次心跳延迟，不会因为一次抖动就误踢正常连接。
inline constexpr int kHeartbeatIntervalSec = 10;
inline constexpr int kHeartbeatTimeoutSec = 30;

}  // namespace limits

}  // namespace thgh

#endif  // THGH_PROTOCOL_MESSAGE_H
