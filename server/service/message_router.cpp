#include "service/message_router.h"

#include "service/data_service.h"
#include "service/json_codec.h"
#include "service/plan_service.h"

namespace thgh {

MessageRouter::MessageRouter(PlanService* planService, DataService* dataService)
    : m_plan(planService), m_data(dataService) {}

void MessageRouter::onMessage(const TcpConnectionPtr& conn,
                              const std::string& line) {
    const Envelope env = parseEnvelope(line);

    if (!env.wellFormed) {
        m_malformed.fetch_add(1, std::memory_order_relaxed);
        conn->send(encodeError("报文不是合法 JSON"));
        return;
    }

    switch (env.type) {
        case MessageType::Heartbeat:
            // 直接在 IO 线程应答，不经过任何队列
            m_heartbeats.fetch_add(1, std::memory_order_relaxed);
            conn->send(encodeHeartbeatAck());
            return;

        case MessageType::HeartbeatAck:
            // 对端应答了我们的心跳。刷新活跃时间已由 TcpConnection 在
            // 收到任何数据时做掉了，这里无事可做。
            return;

        case MessageType::StartPlan:
            if (m_plan != nullptr) {
                m_plan->handleStartPlan(conn, env.payload);
            } else {
                m_unsupported.fetch_add(1, std::memory_order_relaxed);
                conn->send(encodeError("本服务端未启用规划功能"));
            }
            return;

        case MessageType::DataRequest:
            if (m_data != nullptr) {
                m_data->handle(conn, env.taskId, env.payload);
            } else {
                m_unsupported.fetch_add(1, std::memory_order_relaxed);
                // 说清楚是"没配数据库"而不是"你请求错了"，
                // 否则客户端会以为是自己的报文有问题，白查半天。
                conn->send(encodeError(
                    "本服务端未启用数据访问功能（未配置数据库）", env.taskId));
            }
            return;

        default:
            m_unsupported.fetch_add(1, std::memory_order_relaxed);
            conn->send(encodeError("不支持的消息类型"));
            return;
    }
}

MessageRouter::Stats MessageRouter::stats() const {
    Stats s;
    s.malformed = m_malformed.load(std::memory_order_relaxed);
    s.unsupported = m_unsupported.load(std::memory_order_relaxed);
    s.heartbeats = m_heartbeats.load(std::memory_order_relaxed);
    return s;
}

}  // namespace thgh
