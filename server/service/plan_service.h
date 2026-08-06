#ifndef THGH_SERVER_SERVICE_PLAN_SERVICE_H
#define THGH_SERVER_SERVICE_PLAN_SERVICE_H

// ─────────────────────────────────────────────────────────────────────────────
// PlanService —— 规划业务编排
//
// 把"收到 start_plan"变成 Ack → Progress×N → PlanResult 三阶段推送。
//
//   IO 线程（从 Reactor）              计算线程（ComputePool）
//   ─────────────────────             ──────────────────────
//   onMessage(start_plan)
//     ├ 解析报文
//     ├ 校验失败 → 回 error，结束
//     ├ 分配 taskId
//     ├ 发 Ack ──────────────────→（客户端立刻知道请求已受理）
//     └ submit(计算任务) ─────────┐
//        队列满 → 回 error         │
//                                  ↓
//                            plan() 跑 71ms
//                              ├ 进度回调 → conn->send(Progress)
//                              │    （send 内部 runInLoop 投递回 IO 线程）
//                              └ 算完 → conn->send(PlanResult)
//
// ── 三个必须处理对的问题 ──────────────────────────────────────────────────
//
// 1. **连接可能在计算期间断开**（71ms 足够用户点关闭）。
//    计算任务持 weak_ptr 而不是 shared_ptr：
//      * 持 shared_ptr → 连接对象被计算线程续命，fd 迟迟不释放；
//        高并发下这就是 fd 泄漏，最终 accept 返回 EMFILE
//      * 持 weak_ptr → 用之前 lock()，连接已死就直接丢弃结果
//
// 2. **进度消息必须能丢，结果消息不能丢**。
//    客户端读得慢时 progress 会在输出缓冲里堆积。
//    做法：发 progress 前看 outputBufferSize()，超阈值就跳过这一条。
//    丢几条进度只是进度条跳一下，堆爆内存是事故。
//
// 3. **同一连接上的过期任务**。客户端可能连发两次规划。
//    每条连接记住"当前任务 id"，计算完成时若 taskId 已不是当前的，
//    说明结果过期，直接丢弃 —— 否则客户端会先收到旧结果再收到新结果，
//    界面显示的是旧方案。
//
// ── 为什么不做"计算中途取消" ──────────────────────────────────────────────
// 真正的取消要在算法热循环里插检查点，会污染 planner 的实现、也拖慢它。
// 规划计算是**有界时间**的纯 CPU 任务（最大规模也就百毫秒级），
// 让它跑完再丢弃结果，代价可接受。如果单次计算是秒级甚至分钟级，
// 这个取舍就要重新算了。
// ─────────────────────────────────────────────────────────────────────────────

#include "net/tcp_connection.h"
#include "service/compute_pool.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace thgh {

class PlanService {
public:
    struct Options {
        // 输出缓冲超过这个字节数时跳过进度推送（结果消息不受此限）
        std::size_t progressDropThreshold = 256 * 1024;
        // 进度节流：百分比至少变化这么多才推一条。
        // 200 条流会产生 200 次回调，全推等于用进度消息淹没客户端。
        int progressStepPercent = 2;
    };

    // 同 ComputePool：嵌套 Options 不能拿来做默认实参，见 compute_pool.h 的说明
    explicit PlanService(ComputePool* pool);
    PlanService(ComputePool* pool, Options opt);

    // 处理一条 start_plan。由 MessageRouter 在 IO 线程调用，
    // 信封已经解析好 —— 这样 PlanService 和 DataService 是对等的两个处理器，
    // 而不是其中一个兼职做路由。
    void handleStartPlan(const TcpConnectionPtr& conn,
                         const std::string& payloadJson);
    // 挂到 TcpServer::setConnectionCallback
    void onConnection(const TcpConnectionPtr& conn);

    struct Stats {
        std::uint64_t plansAccepted = 0;   // 受理的规划请求
        std::uint64_t plansRejected = 0;   // 因队列满被拒
        std::uint64_t plansCompleted = 0;  // 算完并成功回传
        std::uint64_t plansDropped = 0;    // 算完但连接已断或任务过期
        std::uint64_t badRequests = 0;     // 报文非法
        std::uint64_t progressSent = 0;    // 实际推出的进度条数
        std::uint64_t progressSkipped = 0; // 因背压/节流跳过的进度条数
    };
    Stats stats() const;

private:
    // 每条连接的会话状态
    struct Session {
        std::uint64_t currentTask = 0;
    };

    std::uint64_t nextTaskId() { return ++m_nextTaskId; }
    void runPlan(const std::weak_ptr<TcpConnection>& weakConn,
                 std::uint64_t taskId, class PlanRequest req);
    bool isCurrentTask(const std::string& connName, std::uint64_t taskId) const;

    ComputePool* m_pool;
    Options m_options;

    // 会话表被 IO 线程（建/删/改）与计算线程（查）同时访问，必须加锁。
    // 用一把普通互斥量而不是读写锁：临界区只是一次哈希查找，
    // 读写锁自身的开销比它保护的操作还大。
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Session> m_sessions;

    std::atomic<std::uint64_t> m_nextTaskId{0};

    // 指标用原子量：计算线程与 IO 线程都会更新，
    // 但彼此之间没有一致性要求（不需要"同时读到一组自洽的值"）
    std::atomic<std::uint64_t> m_plansAccepted{0};
    std::atomic<std::uint64_t> m_plansRejected{0};
    std::atomic<std::uint64_t> m_plansCompleted{0};
    std::atomic<std::uint64_t> m_plansDropped{0};
    std::atomic<std::uint64_t> m_badRequests{0};
    std::atomic<std::uint64_t> m_progressSent{0};
    std::atomic<std::uint64_t> m_progressSkipped{0};
};

}  // namespace thgh

#endif  // THGH_SERVER_SERVICE_PLAN_SERVICE_H
