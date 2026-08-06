#ifndef THGH_SERVER_NET_TIMER_QUEUE_H
#define THGH_SERVER_NET_TIMER_QUEUE_H

// ─────────────────────────────────────────────────────────────────────────────
// TimerQueue —— 小根堆定时器
//
// 服务端需要定时做两件事：心跳超时踢除、周期性统计。
//
// ── 为什么不能用 sleep 轮询 ────────────────────────────────────────────────
// 最朴素的做法是开个线程 sleep(1s) 然后遍历所有连接检查超时。三个问题：
//   1. **精度差**：超时判定的误差就是轮询周期，缩短周期又会加剧下面两点
//   2. **浪费 CPU**：绝大多数轮询什么都不做，纯空转
//   3. **O(n) 扫描**：每轮都要遍历全部连接，连接数上去后越来越慢
// 小根堆把它变成：只看堆顶是否到期，插入/删除 O(log n)，而且能直接算出
// "距离最近一次到期还有多久"，把这个值交给 epoll_wait 当超时参数 ——
// **没有定时任务到期时线程就一直阻塞在 epoll_wait 上，零 CPU 占用**。
//
// ── 为什么选小根堆而不是时间轮 ────────────────────────────────────────────
//   小根堆：插入/删除 O(log n)，精度不受限，实现简单
//   时间轮：插入/删除 O(1)，但精度受槽位粒度限制，且要处理跨轮溢出
// 本项目的定时任务是心跳（秒级、数量与连接数同阶），不是海量短定时任务，
// O(log n) 完全够用，没必要引入时间轮的复杂度。
// 时间轮的优势要到"每秒百万级定时任务增删"那个量级才体现出来。
//
// ── 可测试性：时间由调用方注入 ────────────────────────────────────────────
// 本类**不读时钟**，所有接口都接收 now 参数。于是单测可以精确控制时间推进，
// 不需要任何 sleep —— 测"1 小时后到期"也是瞬间完成，且不会因为 CI 机器
// 负载高而 flaky。这是在 TOPO 的连接状态机上验证过的做法。
//
// 线程安全：**无**。每个 EventLoop 独占一个实例，跨线程操作要经由
// EventLoop::runInLoop 投递到所属线程执行。
// ─────────────────────────────────────────────────────────────────────────────

#include <chrono>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace thgh {

using TimerId = std::uint64_t;

class TimerQueue {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Millis = std::chrono::milliseconds;
    using Callback = std::function<void()>;

    // 无效的定时器 id，addTimer 失败或表示"未持有定时器"时使用
    static constexpr TimerId kInvalidTimerId = 0;

    TimerQueue() = default;

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    // 一次性定时器：在 when 时刻触发一次
    TimerId addTimer(TimePoint when, Callback cb);

    // 周期定时器：首次在 first 触发，之后每隔 interval 触发一次。
    // interval <= 0 会被当作一次性定时器。
    TimerId addRepeating(TimePoint first, Millis interval, Callback cb);

    // 取消定时器。返回 false 表示该 id 不存在（已触发完或已取消）。
    //
    // 实现用**惰性删除**：只从数据表里抹掉，堆里的那条记录不动。
    // 因为二叉堆不支持 O(log n) 的任意位置删除（要先 O(n) 找到它），
    // 而惰性删除只需 O(1)，代价是堆里可能残留失效记录 ——
    // 这些记录在 pop 到时会被跳过，且不会无限累积（每条最多残留到它的到期时刻）。
    bool cancel(TimerId id);

    // 距离最近一次到期还有多少毫秒，**直接用作 epoll_wait 的超时参数**。
    //   * 没有任何定时器 → 返回 -1（epoll_wait 无限阻塞）
    //   * 已有到期的     → 返回 0（epoll_wait 立即返回）
    int nextTimeoutMs(TimePoint now) const;

    // 取出所有已到期的回调追加进 out。
    //
    // 刻意**返回回调而不是就地执行**：
    //   1. 回调里可能又增删定时器，就地执行会在遍历过程中修改容器
    //   2. 返回出来才能让 EventLoop 决定执行时机与异常处理
    //   3. 单测可以只验证"哪些该到期"，不必真的触发副作用
    // 周期定时器的重新入堆在本函数内部完成。
    void expire(TimePoint now, std::vector<Callback>& out);

    // 有效定时器数量（不含堆中残留的失效记录）
    std::size_t size() const { return m_timers.size(); }
    bool empty() const { return m_timers.empty(); }

    // 堆中记录数，含惰性删除后残留的失效记录。仅用于测试与诊断。
    std::size_t heapSize() const { return m_heap.size(); }

    void clear();

private:
    struct Entry {
        TimePoint when;
        TimerId id;
    };

    // 小根堆：到期时间早的优先。时间相同时按 id 排，保证顺序确定、测试可复现。
    struct Later {
        bool operator()(const Entry& a, const Entry& b) const {
            if (a.when != b.when) {
                return a.when > b.when;
            }
            return a.id > b.id;
        }
    };

    struct TimerData {
        Callback cb;
        Millis interval{0};  // 0 表示一次性
    };

    TimerId nextId();

    std::priority_queue<Entry, std::vector<Entry>, Later> m_heap;
    std::unordered_map<TimerId, TimerData> m_timers;
    TimerId m_nextId = 1;  // 0 保留给 kInvalidTimerId
};

}  // namespace thgh

#endif  // THGH_SERVER_NET_TIMER_QUEUE_H
