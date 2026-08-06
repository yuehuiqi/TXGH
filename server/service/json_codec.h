#ifndef THGH_SERVER_SERVICE_JSON_CODEC_H
#define THGH_SERVER_SERVICE_JSON_CODEC_H

// ─────────────────────────────────────────────────────────────────────────────
// 协议消息的 JSON 编解码（服务端侧）
//
// ── 为什么编解码在服务端而不在 protocol/ 共享层 ──────────────────────────
// 客户端是 Qt 程序、已经在用 QJsonDocument；服务端用 nlohmann/json。
// 把某一个 JSON 库塞进共享层，等于强迫另一端也引入它。
// 共享层只收口**字段名字符串与消息类型枚举**（见 protocol/thgh/message.h），
// 序列化各端自己做 —— 字段名一致性照样有保证，依赖却不用统一。
//
// ── 解析失败必须是"返回错误"而不是"抛异常穿到 IO 线程" ────────────────
// 报文来自网络，是不可信输入：畸形 JSON、类型不对、字段缺失都会发生，
// 而且是攻击者可以随意构造的。nlohmann 默认在类型不匹配时抛异常，
// 若让它穿过 IO 线程的回调栈，一条畸形报文就能让整个服务端进程退出 ——
// 这是个真实的可远程触发的 DoS。
// 所以本层一律用不抛异常的取值方式，把错误变成返回值。
// ─────────────────────────────────────────────────────────────────────────────

#include "planner/static_planner.h"
#include "thgh/message.h"

#include <cstdint>
#include <string>
#include <vector>

namespace thgh {

// 一次规划请求的解析结果
struct PlanRequest {
    std::vector<PlanNode> nodes;
    std::vector<PlanFlow> flows;
    PlanConfig config;
    int sceneId = -1;
};

// 一条消息的外层信封
struct Envelope {
    MessageType type = MessageType::Unknown;
    std::string payload;      // payload 子对象的序列化文本，没有则为空串
    std::uint64_t taskId = 0; // 关联 id，没有则为 0
    bool wellFormed = false;  // JSON 本身是否可解析
};

// 解析消息外层。畸形 JSON 时 wellFormed=false、type=Unknown，不抛异常。
Envelope parseEnvelope(const std::string& line);

// 解析 start_plan 报文的 payload。
// 失败时返回 false 并把原因写进 errorOut（原因会原样回给客户端，
// 所以措辞要能帮上排查，不能只说"参数错误"）。
bool parsePlanRequest(const std::string& payloadJson, PlanRequest& out,
                      std::string& errorOut);

// ── 编码 ────────────────────────────────────────────────────────────────────
std::string encodeAck(std::uint64_t taskId, int nodeCount, int flowCount);
std::string encodeProgress(std::uint64_t taskId, int percent,
                           const std::string& stage);
std::string encodePlanResult(std::uint64_t taskId, const PlanResult& r);
std::string encodeError(const std::string& message,
                        std::uint64_t taskId = 0);
std::string encodeHeartbeatAck();

}  // namespace thgh

#endif  // THGH_SERVER_SERVICE_JSON_CODEC_H
