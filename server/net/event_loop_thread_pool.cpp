#include "net/event_loop_thread_pool.h"

#include "net/event_loop.h"

#include <algorithm>
#include <cstdio>

namespace thgh {

// ── EventLoopThread ─────────────────────────────────────────────────────────

EventLoopThread::EventLoopThread(std::string name) : m_name(std::move(name)) {}

EventLoopThread::~EventLoopThread() { stop(); }

EventLoop* EventLoopThread::startLoop() {
    m_thread = std::thread([this] { threadFunc(); });

    std::unique_lock<std::mutex> lk(m_mutex);
    // 必须等 EventLoop 在新线程里构造完成。
    // 用 while 而不是 if：条件变量存在虚假唤醒，且要防止"通知先于等待"。
    m_cond.wait(lk, [this] { return m_started; });
    return m_loop;
}

void EventLoopThread::threadFunc() {
    // EventLoop 必须在**它将要运行的那个线程**里构造：
    // 它在构造函数里记录 threadId 并做 one-loop-per-thread 校验。
    EventLoop loop;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_loop = &loop;
        m_started = true;
    }
    m_cond.notify_one();

    loop.loop();

    // loop 退出后 loop 对象即将析构，把指针置空避免外部拿到悬垂指针
    std::lock_guard<std::mutex> lk(m_mutex);
    m_loop = nullptr;
}

void EventLoopThread::stop() {
    EventLoop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        loop = m_loop;
    }
    if (loop != nullptr) {
        loop->quit();  // 内部会唤醒阻塞中的 epoll_wait
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

// ── EventLoopThreadPool ─────────────────────────────────────────────────────

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string name)
    : m_baseLoop(baseLoop), m_name(std::move(name)) {}

EventLoopThreadPool::~EventLoopThreadPool() { stop(); }

std::size_t EventLoopThreadPool::suggestedThreadNum() {
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 1) {
        return 1;
    }
    // 减 1 是给主 Reactor（accept 线程）留一个核。
    // 不留的话，从 Reactor 满负荷时会和 accept 线程抢核，建连延迟明显变差。
    return static_cast<std::size_t>(hw - 1);
}

void EventLoopThreadPool::start() {
    if (m_started) {
        return;
    }
    m_baseLoop->assertInLoopThread();
    m_started = true;

    m_threads.reserve(m_numThreads);
    m_loops.reserve(m_numThreads);
    for (std::size_t i = 0; i < m_numThreads; ++i) {
        auto t = std::unique_ptr<EventLoopThread>(
            new EventLoopThread(m_name + "-sub" + std::to_string(i)));
        m_loops.push_back(t->startLoop());
        m_threads.push_back(std::move(t));
    }
}

void EventLoopThreadPool::stop() {
    for (auto& t : m_threads) {
        t->stop();
    }
    m_threads.clear();
    m_loops.clear();
    m_started = false;
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    m_baseLoop->assertInLoopThread();
    if (m_loops.empty()) {
        // 单线程模式：连接跑在主 loop 上
        return m_baseLoop;
    }
    EventLoop* loop = m_loops[m_next];
    m_next = (m_next + 1) % m_loops.size();
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::allLoops() const {
    if (m_loops.empty()) {
        return {m_baseLoop};
    }
    return m_loops;
}

}  // namespace thgh
