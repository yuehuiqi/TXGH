#include "service/compute_pool.h"

#include <algorithm>

namespace thgh {

ComputePool::ComputePool() : ComputePool(Options{}) {}

ComputePool::ComputePool(Options opt) : m_options(opt) {
    if (m_options.numThreads == 0) {
        m_options.numThreads = suggestedThreadNum();
    }
    if (m_options.maxQueueSize == 0) {
        m_options.maxQueueSize = 1;
    }
}

ComputePool::~ComputePool() { stop(); }

std::size_t ComputePool::suggestedThreadNum() {
    const unsigned hc = std::thread::hardware_concurrency();
    // hardware_concurrency 允许返回 0（信息不可得），必须兜底
    return hc == 0 ? 2u : static_cast<std::size_t>(hc);
}

void ComputePool::start() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_running) {
            return;
        }
        m_running = true;
    }
    m_threads.reserve(m_options.numThreads);
    for (std::size_t i = 0; i < m_options.numThreads; ++i) {
        m_threads.emplace_back([this] { workerLoop(); });
    }
}

void ComputePool::stop() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_running) {
            return;
        }
        m_running = false;
        m_queue.clear();  // 未开始的任务直接丢弃
    }
    // ★ notify 放在锁外还是锁内？这里放锁外是安全的，因为等待方
    //   在 wait 的谓词里同时检查了 m_running，不会漏唤醒。
    //   （TOPO 项目里踩过 winpthreads 锁外 notify 丢唤醒的坑，
    //     那次的根因是谓词判断与 notify 之间存在竞争窗口；
    //     这里谓词覆盖了停止标志，不存在那个窗口。）
    m_notEmpty.notify_all();

    for (std::thread& t : m_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_threads.clear();
}

bool ComputePool::submit(Task task) {
    if (!task) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_running) {
            return false;
        }
        // 有界拒绝：队列满时立刻返回 false，让调用方回一个明确的"服务繁忙"。
        // 阻塞等待是更糟的选择 —— 调用方是 IO 线程，阻塞它等于把
        // 整个 Reactor 拖下水，正是我们要避免的事。
        if (m_queue.size() >= m_options.maxQueueSize) {
            ++m_rejected;
            return false;
        }
        m_queue.push_back(std::move(task));
        ++m_submitted;
        m_peakQueued = std::max(m_peakQueued, m_queue.size());
    }
    m_notEmpty.notify_one();
    return true;
}

void ComputePool::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_notEmpty.wait(lk,
                            [this] { return !m_running || !m_queue.empty(); });
            if (!m_running) {
                return;  // 停止时不再消费剩余任务
            }
            task = std::move(m_queue.front());
            m_queue.pop_front();
            ++m_running_count;
        }

        // ★ 在锁外执行。任务动辄几十毫秒，持锁执行等于把线程池退化成单线程。
        task();

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            --m_running_count;
            ++m_completed;
        }
    }
}

ComputePool::Stats ComputePool::stats() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    Stats s;
    s.submitted = m_submitted;
    s.rejected = m_rejected;
    s.completed = m_completed;
    s.queued = m_queue.size();
    s.running = m_running_count;
    s.peakQueued = m_peakQueued;
    return s;
}

}  // namespace thgh
