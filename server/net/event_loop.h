#ifndef THGH_SERVER_NET_EVENT_LOOP_H
#define THGH_SERVER_NET_EVENT_LOOP_H

// ─────────────────────────────────────────────────────────────────────────────
// EventLoop —— 一个线程一个事件循环（one loop per thread）
//
// 循环体做三件事：
//   1. epoll_wait 等待 IO 事件，超时值由 TimerQueue 算出
//   2. 分发就绪事件给各自的 Channel
//   3. 执行跨线程投递过来的任务、以及到期的定时器回调
//
// ── 为什么是 Reactor 而不是 Proactor ──────────────────────────────────────
// Reactor：内核通知"可以读了"，用户态自己完成 read/write。
// Proactor：内核完成实际读写后再通知结果（Windows IOCP 是典型）。
// Linux 在 io_uring 成熟之前，异步 IO（aio）对 socket 支持一直不完善，
// 所以主流高性能网络库（muduo、libevent、Netty 的 epoll 后端）几乎都是 Reactor。
//
// ── one loop per thread 的意义 ────────────────────────────────────────────
// 每个 EventLoop 绑定一个线程，它管辖的所有 Channel、Buffer、定时器都只被
// 这一个线程访问 —— **整个数据面不需要任何锁**。
// 跨线程操作一律通过 runInLoop 投递，把并发问题收敛成"一个队列 + 一次唤醒"。
//
// ── 为什么需要 eventfd 唤醒 ───────────────────────────────────────────────
// 线程可能正阻塞在 epoll_wait 上（比如超时 -1，无限等待）。
// 此时别的线程投递了任务，如果不唤醒它，任务要等到下一次 IO 事件才会被执行 ——
// 延迟不可控。eventfd 注册进同一个 epoll，往里写 8 字节即可立刻唤醒。
// 相比传统的自管道(self-pipe)技巧，eventfd 只占 1 个 fd 而不是 2 个，
// 且内核实现更轻量。
// ─────────────────────────────────────────────────────────────────────────────

#include "net/timer_queue.h"

// epoll_event 出现在成员变量里，本头文件必须自己包含它，
// 不能依赖使用方恰好先包含了 channel.h 而"碰巧能编过"
#include <sys/epoll.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace thgh {

class Channel;

class EventLoop {
public:
    using Task = std::function<void()>;
    using TimePoint = TimerQueue::TimePoint;
    using Millis = TimerQueue::Millis;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 启动事件循环，阻塞直到 quit() 被调用
    void loop();
    // 退出循环。可从任意线程调用（会自动唤醒阻塞中的 epoll_wait）。
    void quit();

    // ── 跨线程任务投递 ──────────────────────────────────────────────────
    // 若调用方就在本 loop 线程，直接同步执行；否则入队并唤醒该线程。
    // 这是把"多线程并发访问"转化为"单线程串行执行"的关键入口。
    void runInLoop(Task task);
    // 无论在哪个线程，一律入队（不直接执行）。
    // 用于避免在事件处理过程中递归调用而导致的重入问题。
    void queueInLoop(Task task);

    // ── 定时器 ──────────────────────────────────────────────────────────
    TimerId runAt(TimePoint when, Task cb);
    TimerId runAfter(Millis delay, Task cb);
    TimerId runEvery(Millis interval, Task cb);
    void cancelTimer(TimerId id);

    // ── Channel 注册（仅本 loop 线程调用）───────────────────────────────
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    // ── 线程归属 ────────────────────────────────────────────────────────
    bool isInLoopThread() const {
        return m_threadId == std::this_thread::get_id();
    }
    // 断言当前在 loop 线程。跨线程误用是这类框架最常见的 bug 来源，
    // 与其让它变成难查的数据竞争，不如在开发期就直接 abort。
    void assertInLoopThread() const;

    // ── 诊断 ────────────────────────────────────────────────────────────
    struct Stats {
        std::uint64_t loopIterations = 0;   // 循环轮次
        std::uint64_t eventsHandled = 0;    // 处理的 IO 事件数
        std::uint64_t tasksExecuted = 0;    // 执行的跨线程任务数
        std::uint64_t timersExpired = 0;    // 触发的定时器数
        std::uint64_t wakeups = 0;          // 被 eventfd 唤醒次数
    };
    Stats stats() const;

private:
    void wakeup();          // 往 eventfd 写 8 字节
    void handleWakeup();    // 读走 eventfd 的计数，否则 LT 下会反复触发
    void doPendingTasks();
    void doExpiredTimers();

    int m_epollFd = -1;
    int m_wakeupFd = -1;
    std::unique_ptr<Channel> m_wakeupChannel;

    std::atomic<bool> m_looping{false};
    std::atomic<bool> m_quit{false};
    const std::thread::id m_threadId;

    std::vector<epoll_event> m_events;  // epoll_wait 的输出数组，会动态扩容

    // 跨线程任务队列。只有这里需要锁 —— 数据面完全无锁。
    mutable std::mutex m_taskMutex;
    std::vector<Task> m_pendingTasks;
    // 标记"正在执行 pending 任务"。此时若有新任务投递进来，
    // 即使投递方就在本线程也必须唤醒，否则本轮 doPendingTasks 已经
    // swap 走了队列，新任务要等到下一次 IO 事件才被执行。
    std::atomic<bool> m_callingPendingTasks{false};

    TimerQueue m_timers;

    mutable std::mutex m_statsMutex;
    Stats m_stats;
};

}  // namespace thgh

#endif  // THGH_SERVER_NET_EVENT_LOOP_H
