#include "thgh/message.h"

namespace thgh {
namespace {

// 枚举 ↔ 字符串的唯一映射表。
// 两个转换函数都从这里取，保证不会出现"枚举加了但字符串忘了加"的半边情况。
struct Entry {
    MessageType type;
    std::string_view name;
};

constexpr Entry kTable[] = {
    {MessageType::StartPlan, "start_plan"},
    {MessageType::PlanResult, "plan_result"},
    {MessageType::Error, "error"},
    {MessageType::Ack, "ack"},
    {MessageType::Progress, "progress"},
    {MessageType::DataRequest, "data_request"},
    {MessageType::DataReply, "data_reply"},
    {MessageType::Heartbeat, "heartbeat"},
    {MessageType::HeartbeatAck, "heartbeat_ack"},
};

}  // namespace

MessageType messageTypeFromString(std::string_view s) noexcept {
    // 消息类型只有个位数个，线性查找比建哈希表更快也更简单：
    // 省掉了静态初始化和一次哈希计算，而且分支预测友好。
    for (const Entry& e : kTable) {
        if (e.name == s) {
            return e.type;
        }
    }
    return MessageType::Unknown;
}

std::string_view toString(MessageType t) noexcept {
    for (const Entry& e : kTable) {
        if (e.type == t) {
            return e.name;
        }
    }
    return "unknown";
}

}  // namespace thgh
