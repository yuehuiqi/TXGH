#ifndef THGH_SERVER_SERVICE_COMPUTE_POOL_H
#define THGH_SERVER_SERVICE_COMPUTE_POOL_H

// ─────────────────────────────────────────────────────────────────────────────
// ComputePool —— 计算线程池
//
// ── 为什么必须有它 ────────────────────────────────────────────────────────
// 规划计算是纯 CPU 密集任务：200 节点 / 200 条流实测要 71ms。
// 如果直接在从 Reactor（IO 线程）里同步跑，后果是：
//
//   1. 该 Reactor 上**所有**连接被卡住 71ms —— 不只是发起规划的那一条。
//      4 个 IO 线程分摊 1000 条连接，一次规划就冻结其中 250 条。
//   2. 心跳应答发不出去，对端可能反过来判定服务端超时并断连。
//   3. ★ 分阶段推送直接失去意义 —— progress 消息也发不出去，
//      因为发它的线程正被计算本身占着。
//
// 换句话说：**没有计算线程池，P5 的 ack/progress/result 三阶段推送根本不成立。**
// 这两件事是绑在一起的，不是两个独立的改动。
//
// ── 为什么队列是有界的 ────────────────────────────────────────────────────
// 无界队列在过载时的表现是"看起来还活着，但内存一直涨，延迟一直恶化，
// 最后被 OOM killer 干掉"。有界队列满了就明确拒绝，客户端立刻收到
// "服务繁忙"而不是等 30 秒后超时 —— 快速失败比慢速崩溃好。
//
// ── 与 IO 线程池（EventLoopThreadPool）的区别 ────────────────────────────
//   EventLoopThreadPool：每线程一个 epoll 事件循环，处理**IO 事件**，
//                        线程数按核数定，任务是"等事件 → 读写"
//   ComputePool        ：无事件循环，从队列取**计算任务**跑，
//                        线程数按核数定，任务是"算完 → 回调"
// 两者线程数相加会超过核数，但 IO 线程绝大部分时间阻塞在 epoll_wait 上
// 不占 CPU，所以不会真的抢满。
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace thgh {

class ComputePool {
public:
    using Task = std::function<void()>;

    struct Options {
        // 工作线程数。0 表示按硬件并发数推断。
        std::size_t numThreads = 0;
        // 队列容量上限。超过即拒绝新任务。
        std::size_t maxQueueSize = 64;
    };

    // ⚠️ 拆成两个构造函数，而不是写 `ComputePool(Options opt = Options{})`。
    //    嵌套类的默认成员初始化器（NSDMI）要等**外层类**定义结束才被解析，
    //    所以在外层类内部把 `Options{}` 用作默认实参会报
    //    "default member initializer required before the end of its enclosing class"。
    //    这个坑在 P1 的 LineFramer 上踩过一次，这里又踩了一次 ——
    //    只要"嵌套 Options + 默认实参"这个组合出现就会中招。
    ComputePool();
    explicit ComputePool(Options opt);
    ~ComputePool();

    ComputePool(const ComputePool&) = delete;
    ComputePool& operator=(const ComputePool&) = delete;

    void start();
    // 停止并等待所有线程退出。已在队列里但未开始的任务被丢弃，
    // 正在执行的任务会跑完 —— 强行中断一个跑到一半的计算没有安全的做法。
    void stop();

    // 提交任务。队列已满或池已停止时返回 false，**调用方必须处理这个返回值**。
    bool submit(Task task);

    std::size_t threadCount() const { return m_threads.size(); }

    struct Stats {
        std::uint64_t submitted = 0;   // 累计提交成功
        std::uint64_t rejected = 0;    // 累计因队列满被拒
        std::uint64_t completed = 0;   // 累计执行完成
        std::size_t queued = 0;        // 当前排队中
        std::size_t running = 0;       // 当前执行中
        std::size_t peakQueued = 0;    // 排队长度峰值
    };
    Stats stats() const;

    // 按核数给出建议线程数
    static std::size_t suggestedThreadNum();

private:
    void workerLoop();

    mutable std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::deque<Task> m_queue;
    std::vector<std::thread> m_threads;

    Options m_options;
    bool m_running = false;

    std::uint64_t m_submitted = 0;
    std::uint64_t m_rejected = 0;
    std::uint64_t m_completed = 0;
    std::size_t m_running_count = 0;
    std::size_t m_peakQueued = 0;
};

}  // namespace thgh

#endif  // THGH_SERVER_SERVICE_COMPUTE_POOL_H
