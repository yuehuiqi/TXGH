// 消息类型定义的单元测试
//
// 这一层是纯常量，看起来没什么可测的，但有两件事必须钉住：
//   1. 枚举 ↔ 字符串的**双向映射自洽**——防止加了枚举却忘了加字符串
//   2. 与改造前 Python 服务端的字段名**逐字一致**——这是分阶段改造的前提，
//      客户端解析代码不用改，改造才能一步步推进而不是一次性大爆炸

#include "thgh/message.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using thgh::MessageType;
using thgh::messageTypeFromString;
using thgh::toString;

namespace {

// 所有有效消息类型（不含 Unknown）
const std::vector<MessageType> kAllTypes = {
    MessageType::StartPlan, MessageType::PlanResult, MessageType::Error,
    MessageType::Ack,       MessageType::Progress,   MessageType::Heartbeat,
    MessageType::HeartbeatAck,
};

}  // namespace

// ── 双向映射自洽 ────────────────────────────────────────────────────────────

TEST(Message, RoundTripsEveryType) {
    for (MessageType t : kAllTypes) {
        const auto name = toString(t);
        EXPECT_NE(name, "unknown")
            << "枚举值缺少对应字符串，映射表漏了一项";
        EXPECT_EQ(messageTypeFromString(name), t)
            << "字符串 [" << name << "] 反查不回原枚举";
    }
}

TEST(Message, TypeNamesAreUnique) {
    // 两个枚举映射到同一个字符串会导致反查结果不确定
    std::set<std::string> seen;
    for (MessageType t : kAllTypes) {
        const std::string name(toString(t));
        EXPECT_TRUE(seen.insert(name).second) << "字符串重复: " << name;
    }
}

TEST(Message, UnknownStringsMapToUnknown) {
    EXPECT_EQ(messageTypeFromString("plan_results"), MessageType::Unknown);
    EXPECT_EQ(messageTypeFromString(""), MessageType::Unknown);
    EXPECT_EQ(messageTypeFromString("START_PLAN"), MessageType::Unknown);
    EXPECT_EQ(messageTypeFromString("start_plan "), MessageType::Unknown);
}

TEST(Message, UnknownEnumMapsToUnknownString) {
    EXPECT_EQ(toString(MessageType::Unknown), "unknown");
}

TEST(Message, MatchIsExactNotPrefix) {
    // 前缀匹配会让 "ack" 误匹配 "ack_something"，必须是全等
    EXPECT_EQ(messageTypeFromString("ac"), MessageType::Unknown);
    EXPECT_EQ(messageTypeFromString("ackk"), MessageType::Unknown);
    EXPECT_EQ(messageTypeFromString("heartbeat"), MessageType::Heartbeat);
    EXPECT_EQ(messageTypeFromString("heartbeat_ack"), MessageType::HeartbeatAck);
}

// ── 与改造前协议的兼容性 ★ ──────────────────────────────────────────────────

TEST(Message, LegacyTypeNamesUnchanged) {
    // 改造前 Python 服务端与 Qt 客户端已在用的三种消息，
    // 字符串必须逐字不变，否则客户端立刻不认识服务端
    EXPECT_EQ(toString(MessageType::StartPlan), "start_plan");
    EXPECT_EQ(toString(MessageType::PlanResult), "plan_result");
    EXPECT_EQ(toString(MessageType::Error), "error");
}

TEST(Message, LegacyFieldNamesUnchanged) {
    namespace f = thgh::field;

    // 顶层字段（对应 simbridge.cpp 里的 obj.value("type") / value("payload")）
    EXPECT_EQ(f::kType, "type");
    EXPECT_EQ(f::kPayload, "payload");
    EXPECT_EQ(f::kMessage, "message");

    // plan_result 载荷的三个数组
    EXPECT_EQ(f::result::kLinks, "links");
    EXPECT_EQ(f::result::kPlanResults, "plan_results");
    EXPECT_EQ(f::result::kWarns, "warns");

    // links[] 元素字段
    EXPECT_EQ(f::link::kLinkId, "link_id");
    EXPECT_EQ(f::link::kSrcNodeId, "src_node_id");
    EXPECT_EQ(f::link::kDstNodeId, "dst_node_id");
    EXPECT_EQ(f::link::kLinkType, "link_type");
    EXPECT_EQ(f::link::kWirelessType, "wireless_type");
    EXPECT_EQ(f::link::kBandwidthBps, "bandwidth_bps");
    EXPECT_EQ(f::link::kPropDelayS, "prop_delay_s");
    EXPECT_EQ(f::link::kFlows, "flows");

    // links[].flows[] 元素字段
    EXPECT_EQ(f::linkflow::kFid, "fid");
    EXPECT_EQ(f::linkflow::kBandwidthBps, "bandwidth_bps");
    EXPECT_EQ(f::linkflow::kDescription, "description");

    // plan_results[] 元素字段
    EXPECT_EQ(f::planresult::kFid, "fid");
    EXPECT_EQ(f::planresult::kSrcNodeId, "src_node_id");
    EXPECT_EQ(f::planresult::kDstNodeId, "dst_node_id");
    EXPECT_EQ(f::planresult::kIsSatisfied, "is_satisfied");
    EXPECT_EQ(f::planresult::kHops, "hops");
    EXPECT_EQ(f::planresult::kActualBandwidthBps, "actual_bandwidth_bps");
    EXPECT_EQ(f::planresult::kUnsatisfiedReason, "unsatisfied_reason");
    EXPECT_EQ(f::planresult::kPathNodes, "path_nodes");
    EXPECT_EQ(f::planresult::kPathLinks, "path_links");

    // warns[] 元素字段
    EXPECT_EQ(f::warn::kLevel, "level");
    EXPECT_EQ(f::warn::kMessage, "message");
    EXPECT_EQ(f::warn::kWarnType, "warn_type");
    EXPECT_EQ(f::warn::kRelatedFid, "related_fid");
}

// ── 协议约束 ────────────────────────────────────────────────────────────────

TEST(Message, HeartbeatTimeoutIsAMultipleOfInterval) {
    namespace L = thgh::limits;
    // 超时必须显著大于间隔，否则一次调度抖动或偶发丢包就会误踢正常连接。
    // 取 3 倍：容忍连续两次心跳丢失。
    EXPECT_GE(L::kHeartbeatTimeoutSec, L::kHeartbeatIntervalSec * 3);
}

TEST(Message, MaxMessageBytesIsSane) {
    namespace L = thgh::limits;
    // 规划请求要带整个场景的节点与链路，太小会截断正常业务；
    // 太大则失去 DoS 防护意义
    EXPECT_GE(L::kMaxMessageBytes, 64u * 1024u);
    EXPECT_LE(L::kMaxMessageBytes, 16u * 1024u * 1024u);
}
