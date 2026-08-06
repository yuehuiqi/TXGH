#include "service/plan_service.h"

#include "service/json_codec.h"

#include <cstdio>
#include <utility>

namespace thgh {

PlanService::PlanService(ComputePool* pool) : PlanService(pool, Options{}) {}

PlanService::PlanService(ComputePool* pool, Options opt)
    : m_pool(pool), m_options(opt) {}

// ── 连接建立 / 断开 ─────────────────────────────────────────────────────────

void PlanService::onConnection(const TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (conn->connected()) {
        m_sessions[conn->name()] = Session{};
    } else {
        // 连接断开就删会话。此时可能还有计算在跑 ——
        // 那个任务算完后会发现 weak_ptr 已失效（或会话已不存在），
        // 自行丢弃结果，不需要在这里做额外的同步。
        m_sessions.erase(conn->name());
    }
}

bool PlanService::isCurrentTask(const std::string& connName,
                                std::uint64_t taskId) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_sessions.find(connName);
    return it != m_sessions.end() && it->second.currentTask == taskId;
}

// ── 收到消息（IO 线程）─────────────────────────────────────────────────────

void PlanService::handleStartPlan(const TcpConnectionPtr& conn,
                                  const std::string& payloadJson) {
    // ── 解析规划请求 ────────────────────────────────────────────────────
    PlanRequest req;
    std::string err;
    if (!parsePlanRequest(payloadJson, req, err)) {
        m_badRequests.fetch_add(1, std::memory_order_relaxed);
        conn->send(encodeError("规划请求无效：" + err));
        return;
    }

    const std::uint64_t taskId = nextTaskId();

    // 记为当前任务。同一连接上再来一次规划时，旧任务的结果就会被判定为过期。
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_sessions.find(conn->name());
        if (it == m_sessions.end()) {
            return;  // 连接已在处理途中断开
        }
        it->second.currentTask = taskId;
    }

    const int nodeCount = static_cast<int>(req.nodes.size());
    const int flowCount = static_cast<int>(req.flows.size());

    // ★ 顺序问题：先发 Ack 再派发，还是先派发再发 Ack？
    //
    //   先发 Ack：队列满时 Ack 已经出去了，客户端收到"已受理"之后
    //             却永远等不到结果，只能靠超时兜底 —— 体验和排查都很差。
    //   先派发  ：能确认受理成功再承诺。选这个。
    //
    //   但先派发带来一个疑问：任务可能立刻被工作线程取走并开始发 Progress，
    //   会不会跑到 Ack 前面？**不会**，这由 EventLoop 的结构保证：
    //     - 我们此刻在 IO 线程内（onMessage 由 handleEvent 调用），
    //       conn->send(Ack) 判断"已在本 loop 线程"，走同步路径直接写；
    //     - 计算线程的 conn->send(Progress) 不在 loop 线程，走 queueInLoop
    //       进入待办队列，而待办队列是在 handleEvent() **之后**
    //       由 doPendingTasks() 执行的（见 event_loop.cpp 的 loop()）。
    //   所以 Ack 必然先落到输出缓冲。这是设计上的保证，不是时序上的侥幸。
    auto weakConn = std::weak_ptr<TcpConnection>(conn);
    const bool ok = m_pool->submit([this, weakConn, taskId,
                                    req = std::move(req)]() mutable {
        runPlan(weakConn, taskId, std::move(req));
    });

    if (!ok) {
        m_plansRejected.fetch_add(1, std::memory_order_relaxed);
        conn->send(encodeError(
            "服务端计算队列已满，请稍后重试", taskId));
        return;
    }

    m_plansAccepted.fetch_add(1, std::memory_order_relaxed);
    conn->send(encodeAck(taskId, nodeCount, flowCount));
}

// ── 执行规划（计算线程）────────────────────────────────────────────────────

void PlanService::runPlan(const std::weak_ptr<TcpConnection>& weakConn,
                          std::uint64_t taskId, PlanRequest req) {
    // ★ 每次用连接前都要 lock()，不能只在开头 lock 一次然后一直持有 ——
    //   那等于把 weak_ptr 退化成 shared_ptr，连接在整个计算期间无法释放。
    auto aliveConn = [&weakConn]() -> TcpConnectionPtr {
        auto c = weakConn.lock();
        return (c && c->connected()) ? c : nullptr;
    };

    {
        auto conn = aliveConn();
        if (!conn) {
            m_plansDropped.fetch_add(1, std::memory_order_relaxed);
            return;  // 还没开算连接就断了
        }
    }

    StaticPlanner planner(std::move(req.nodes), std::move(req.flows),
                          req.config);

    int lastSentPercent = -100;
    planner.setProgressCallback([&](int percent, const std::string& stage) {
        // 节流：百分比变化不够大就不发。100% 例外，它是阶段收尾的标志。
        if (percent < 100 &&
            percent - lastSentPercent < m_options.progressStepPercent) {
            m_progressSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto conn = aliveConn();
        if (!conn) {
            return;  // 连接没了，进度自然不用发；结果那边会统一计入 dropped
        }

        // 背压：客户端读得慢时输出缓冲会涨。进度是可丢的，先保结果。
        if (conn->outputBufferSize() > m_options.progressDropThreshold) {
            m_progressSkipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        lastSentPercent = percent;
        m_progressSent.fetch_add(1, std::memory_order_relaxed);
        // send() 可从任意线程调用，内部会 runInLoop 投递到该连接所属的
        // IO 线程执行 —— 计算线程绝不直接碰 fd 和输出缓冲。
        conn->send(encodeProgress(taskId, percent, stage));
    });

    const PlanResult result = planner.plan();

    // ── 回传结果 ────────────────────────────────────────────────────────
    auto conn = aliveConn();
    if (!conn) {
        m_plansDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // 过期检查：客户端在这次计算期间又发了一次规划，旧结果就不能再送出去，
    // 否则界面会先显示旧方案、再被新方案覆盖，中间那一下是错的。
    if (!isCurrentTask(conn->name(), taskId)) {
        m_plansDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    m_plansCompleted.fetch_add(1, std::memory_order_relaxed);
    conn->send(encodePlanResult(taskId, result));
}

PlanService::Stats PlanService::stats() const {
    Stats s;
    s.plansAccepted = m_plansAccepted.load(std::memory_order_relaxed);
    s.plansRejected = m_plansRejected.load(std::memory_order_relaxed);
    s.plansCompleted = m_plansCompleted.load(std::memory_order_relaxed);
    s.plansDropped = m_plansDropped.load(std::memory_order_relaxed);
    s.badRequests = m_badRequests.load(std::memory_order_relaxed);
    s.progressSent = m_progressSent.load(std::memory_order_relaxed);
    s.progressSkipped = m_progressSkipped.load(std::memory_order_relaxed);
    return s;
}

}  // namespace thgh
