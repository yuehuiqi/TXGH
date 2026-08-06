#ifndef THGH_SERVER_NET_EVENT_LOOP_THREAD_POOL_H
#define THGH_SERVER_NET_EVENT_LOOP_THREAD_POOL_H

// ─────────────────────────────────────────────────────────────────────────────
// EventLoopThreadPool —— 从 Reactor 线程池
//
// 每个线程跑一个独立的 EventLoop（各自持有独立的 epoll 实例）。
// 主 Reactor accept 到新连接后，按轮询把它分配给其中一个从 Reactor。
//
// ── 线程数怎么定 ──────────────────────────────────────────────────────────
// 这些线程做的是 IO 多路复用 + 协议解析，属于 CPU 密集偏 IO 的混合型。
// 经验值取硬件并发数，但要**给主 Reactor 线程留出余量** ——
// 本项目部署在 4 核机器上，取 3 个从 Reactor + 1 个主 Reactor 刚好占满，
// 再多就是过度订阅，线程互相抢核反而更慢。
// （在 TOPO 项目上实测过这个现象：线程数拉满到逻辑核数时吞吐会崩塌。）
//
// ── 为什么轮询分配就够了 ──────────────────────────────────────────────────
// 更"聪明"的做法是按各 loop 的当前连接数选最闲的。但那需要跨线程读取
// 各 loop 的状态（要么加锁、要么用原子量近似），复杂度上去了收益却有限——
// 长连接场景下连接数量本身就均匀，轮询已经足够。
// 真正需要负载感知的是"连接活跃度差异极大"的场景，本项目不属于。
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace thgh {

class EventLoop;

// 一个线程 + 它独占的 EventLoop
class EventLoopThread {
public:
    explicit EventLoopThread(std::string name);
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    // 启动线程并**等待 EventLoop 真正创建完成**后返回其指针。
    // 必须等待：EventLoop 是在新线程里构造的，不等就可能拿到空指针。
    EventLoop* startLoop();
    void stop();

    const std::string& name() const { return m_name; }

private:
    void threadFunc();

    std::string m_name;
    std::thread m_thread;
    EventLoop* m_loop = nullptr;

    // 用于等待新线程里的 EventLoop 构造完成
    std::mutex m_mutex;
    std::condition_variable m_cond;
    bool m_started = false;
};

class EventLoopThreadPool {
public:
    // baseLoop 是主 Reactor 所在的 loop。
    // numThreads == 0 时不创建任何从 Reactor，所有连接都跑在主 loop 上
    // （单线程模式，便于调试与对比测试）。
    EventLoopThreadPool(EventLoop* baseLoop, std::string name);
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void setThreadNum(std::size_t n) { m_numThreads = n; }
    void start();
    void stop();

    // 轮询取下一个 loop 用于承载新连接
    EventLoop* getNextLoop();
    std::vector<EventLoop*> allLoops() const;

    std::size_t threadNum() const { return m_numThreads; }
    bool started() const { return m_started; }

    // 按当前机器规格给出建议的从 Reactor 数量：
    // 硬件并发数减 1（给主 Reactor 留一个核），至少 1。
    static std::size_t suggestedThreadNum();

private:
    EventLoop* m_baseLoop;
    std::string m_name;
    bool m_started = false;
    std::size_t m_numThreads = 0;
    std::size_t m_next = 0;  // 轮询游标，只在主 loop 线程访问，无需原子
    std::vector<std::unique_ptr<EventLoopThread>> m_threads;
    std::vector<EventLoop*> m_loops;
};

}  // namespace thgh

#endif  // THGH_SERVER_NET_EVENT_LOOP_THREAD_POOL_H
