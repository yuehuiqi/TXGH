#ifndef THGH_SERVER_SERVICE_MESSAGE_ROUTER_H
#define THGH_SERVER_SERVICE_MESSAGE_ROUTER_H

// ─────────────────────────────────────────────────────────────────────────────
// MessageRouter —— 按消息类型分发到各业务处理器
//
// 信封在这里**只解析一次**，然后把 payload 交给对应的处理器。
// P5 时只有规划一种业务，解析和处理都塞在 PlanService::onMessage 里；
// P6 加了数据访问之后，如果让 PlanService 继续兼职路由，
// 它就得认识 DataService —— 两个平级的业务处理器互相依赖，
// 以后再加一种业务还得改它。抽出路由层之后两者互不知道对方存在。
//
// 心跳在这一层直接应答，不下发给任何业务处理器，也**绝不进计算队列**：
// 队列排满时心跳要是答不出去，对端会误判服务端已死并断连 ——
// 那正是过载时最不该发生的事（雪上加霜）。
// ─────────────────────────────────────────────────────────────────────────────

#include "net/tcp_connection.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace thgh {

class PlanService;
class DataService;

class MessageRouter {
public:
    // dataService 允许为 nullptr —— 没有配数据库时服务端仍能跑规划，
    // 只是数据访问请求会被明确拒绝。这样部署更灵活，也便于单独压测网络层。
    MessageRouter(PlanService* planService, DataService* dataService);

    // 挂到 TcpServer::setMessageCallback
    void onMessage(const TcpConnectionPtr& conn, const std::string& line);

    struct Stats {
        std::uint64_t malformed = 0;    // 报文不是合法 JSON
        std::uint64_t unsupported = 0;  // 类型不认识或对应服务未启用
        std::uint64_t heartbeats = 0;   // 应答过的心跳数
    };
    Stats stats() const;

private:
    PlanService* m_plan;
    DataService* m_data;

    std::atomic<std::uint64_t> m_malformed{0};
    std::atomic<std::uint64_t> m_unsupported{0};
    std::atomic<std::uint64_t> m_heartbeats{0};
};

}  // namespace thgh

#endif  // THGH_SERVER_SERVICE_MESSAGE_ROUTER_H
