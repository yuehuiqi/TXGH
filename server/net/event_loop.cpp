#include "net/event_loop.h"

#include "net/channel.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace thgh {
namespace {

constexpr int kInitialEventListSize = 16;

// 每个线程最多一个 EventLoop。用线程局部变量做校验：
// "one loop per thread" 一旦被破坏，两个 loop 会互相抢同一线程的执行权，
// 症状是任务莫名其妙不执行 —— 与其让它变成难查的问题，不如启动时就报错。
thread_local EventLoop* t_loopInThisThread = nullptr;

int createEventFd() {
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        std::fprintf(stderr, "[EventLoop] eventfd 创建失败: %s\n",
                     ::strerror(errno));
        std::abort();
    }
    return fd;
}

}  // namespace

EventLoop::EventLoop()
    : m_threadId(std::this_thread::get_id()),
      m_events(kInitialEventListSize) {
    if (t_loopInThisThread != nullptr) {
        std::fprintf(stderr,
                     "[EventLoop] 本线程已存在 EventLoop，违反 one loop per thread\n");
        std::abort();
    }
    t_loopInThisThread = this;

    m_epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (m_epollFd < 0) {
        std::fprintf(stderr, "[EventLoop] epoll_create1 失败: %s\n",
                     ::strerror(errno));
        std::abort();
    }

    m_wakeupFd = createEventFd();
    m_wakeupChannel.reset(new Channel(this, m_wakeupFd));
    m_wakeupChannel->setReadCallback([this] { handleWakeup(); });
    m_wakeupChannel->enableReading();
}

EventLoop::~EventLoop() {
    if (m_wakeupChannel) {
        m_wakeupChannel->disableAll();
        removeChannel(m_wakeupChannel.get());
    }
    if (m_wakeupFd >= 0) {
        ::close(m_wakeupFd);
    }
    if (m_epollFd >= 0) {
        ::close(m_epollFd);
    }
    t_loopInThisThread = nullptr;
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        std::fprintf(stderr,
                     "[EventLoop] 跨线程调用了只允许在 loop 线程执行的接口\n");
        std::abort();
    }
}

void EventLoop::loop() {
    assertInLoopThread();
    m_looping = true;
    m_quit = false;

    std::vector<Channel*> active;

    while (!m_quit) {
        // 超时值来自小根堆定时器：没有定时任务时传 -1，
        // 线程就一直阻塞在 epoll_wait 上，**零 CPU 占用**。
        // 这正是小根堆定时器相比 sleep 轮询的核心优势。
        const int timeoutMs = m_timers.nextTimeoutMs(TimerQueue::Clock::now());

        const int n = ::epoll_wait(m_epollFd, m_events.data(),
                                   static_cast<int>(m_events.size()), timeoutMs);
        const int savedErrno = errno;

        {
            std::lock_guard<std::mutex> lk(m_statsMutex);
            ++m_stats.loopIterations;
        }

        if (n < 0) {
            // EINTR：被信号打断，不是错误，重来一轮即可。
            // 不处理它会导致进程收到任何信号（比如 gdb attach）就退出。
            if (savedErrno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "[EventLoop] epoll_wait 出错: %s\n",
                         ::strerror(savedErrno));
            continue;
        }

        active.clear();
        for (int i = 0; i < n; ++i) {
            auto* ch = static_cast<Channel*>(m_events[i].data.ptr);
            ch->setRevents(m_events[i].events);
            active.push_back(ch);
        }

        {
            std::lock_guard<std::mutex> lk(m_statsMutex);
            m_stats.eventsHandled += static_cast<std::uint64_t>(n);
        }

        for (Channel* ch : active) {
            ch->handleEvent();
        }

        // 就绪事件填满了数组，说明可能还有更多事件没取回来 → 扩容。
        // 不扩容的话高并发下每轮只能处理固定条数，剩下的要等下一轮，
        // 延迟会明显变差。
        if (n == static_cast<int>(m_events.size())) {
            m_events.resize(m_events.size() * 2);
        }

        doExpiredTimers();
        doPendingTasks();
    }

    m_looping = false;
}

void EventLoop::quit() {
    m_quit = true;
    // 若 quit 来自别的线程，本线程可能正阻塞在 epoll_wait 上，
    // 必须唤醒它，否则要等到下一次 IO 事件或定时器到期才会退出。
    if (!isInLoopThread()) {
        wakeup();
    }
}

void EventLoop::runInLoop(Task task) {
    if (isInLoopThread()) {
        task();
    } else {
        queueInLoop(std::move(task));
    }
}

void EventLoop::queueInLoop(Task task) {
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        m_pendingTasks.push_back(std::move(task));
    }
    // 两种情况必须唤醒：
    //   1. 投递方在别的线程 —— 本线程可能正阻塞在 epoll_wait
    //   2. 投递方就在本线程，但当前正在执行 doPendingTasks ——
    //      本轮的队列已被 swap 走，新任务不会被本轮执行到，
    //      不唤醒的话要等下一次 IO 事件，延迟不可控
    if (!isInLoopThread() || m_callingPendingTasks.load()) {
        wakeup();
    }
}

TimerId EventLoop::runAt(TimePoint when, Task cb) {
    if (isInLoopThread()) {
        return m_timers.addTimer(when, std::move(cb));
    }
    // 定时器队列不是线程安全的，跨线程添加必须投递到 loop 线程。
    // 代价是拿不到 TimerId（异步执行时还没生成），
    // 需要 id 的场景应确保在 loop 线程内调用。
    queueInLoop([this, when, cb = std::move(cb)]() mutable {
        m_timers.addTimer(when, std::move(cb));
    });
    return TimerQueue::kInvalidTimerId;
}

TimerId EventLoop::runAfter(Millis delay, Task cb) {
    return runAt(TimerQueue::Clock::now() + delay, std::move(cb));
}

TimerId EventLoop::runEvery(Millis interval, Task cb) {
    const auto first = TimerQueue::Clock::now() + interval;
    if (isInLoopThread()) {
        return m_timers.addRepeating(first, interval, std::move(cb));
    }
    queueInLoop([this, first, interval, cb = std::move(cb)]() mutable {
        m_timers.addRepeating(first, interval, std::move(cb));
    });
    return TimerQueue::kInvalidTimerId;
}

void EventLoop::cancelTimer(TimerId id) {
    if (id == TimerQueue::kInvalidTimerId) {
        return;
    }
    runInLoop([this, id] { m_timers.cancel(id); });
}

void EventLoop::updateChannel(Channel* channel) {
    assertInLoopThread();

    epoll_event ev;
    ::memset(&ev, 0, sizeof(ev));
    ev.events = static_cast<uint32_t>(channel->events());
    ev.data.ptr = channel;

    const int fd = channel->fd();

    if (channel->state() == Channel::State::New ||
        channel->state() == Channel::State::Deleted) {
        if (::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::fprintf(stderr, "[EventLoop] epoll_ctl ADD fd=%d 失败: %s\n",
                         fd, ::strerror(errno));
            return;
        }
        channel->setState(Channel::State::Added);
    } else {
        if (channel->isNoneEvent()) {
            // 不再关注任何事件：从 epoll 摘除，但保留 Channel 对象。
            // 留在 epoll 里会让已关闭的 fd 产生无意义的事件。
            if (::epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
                std::fprintf(stderr, "[EventLoop] epoll_ctl DEL fd=%d 失败: %s\n",
                             fd, ::strerror(errno));
            }
            channel->setState(Channel::State::Deleted);
        } else {
            if (::epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) < 0) {
                std::fprintf(stderr, "[EventLoop] epoll_ctl MOD fd=%d 失败: %s\n",
                             fd, ::strerror(errno));
            }
        }
    }
}

void EventLoop::removeChannel(Channel* channel) {
    assertInLoopThread();
    if (channel->state() == Channel::State::Added) {
        ::epoll_ctl(m_epollFd, EPOLL_CTL_DEL, channel->fd(), nullptr);
    }
    channel->setState(Channel::State::New);
}

void EventLoop::wakeup() {
    const std::uint64_t one = 1;
    const ssize_t n = ::write(m_wakeupFd, &one, sizeof(one));
    if (n != sizeof(one)) {
        // eventfd 是 8 字节计数器，写入要么全成功要么失败。
        // 唯一的失败场景是计数器溢出（2^64-1），实践中不会发生。
        std::fprintf(stderr, "[EventLoop] wakeup 写入 %zd 字节（应为 8）\n", n);
    }
}

void EventLoop::handleWakeup() {
    std::uint64_t one = 0;
    // 必须把计数读走，否则 eventfd 一直可读，epoll 会反复通知，
    // 循环空转烧 CPU（即使用了 ET，下次 write 又会再次触发）。
    const ssize_t n = ::read(m_wakeupFd, &one, sizeof(one));
    if (n != sizeof(one)) {
        std::fprintf(stderr, "[EventLoop] wakeup 读出 %zd 字节（应为 8）\n", n);
    }
    std::lock_guard<std::mutex> lk(m_statsMutex);
    ++m_stats.wakeups;
}

void EventLoop::doPendingTasks() {
    std::vector<Task> tasks;
    m_callingPendingTasks = true;
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        // swap 而不是逐个取：
        //   1. 临界区缩到最小 —— 执行任务时不持锁，任务里可以安全地再次投递
        //   2. 避免"任务里又投递任务"导致本轮无限循环
        tasks.swap(m_pendingTasks);
    }
    for (const Task& t : tasks) {
        t();
    }
    m_callingPendingTasks = false;

    std::lock_guard<std::mutex> lk(m_statsMutex);
    m_stats.tasksExecuted += tasks.size();
}

void EventLoop::doExpiredTimers() {
    std::vector<Task> expired;
    m_timers.expire(TimerQueue::Clock::now(), expired);
    for (const Task& t : expired) {
        t();
    }
    if (!expired.empty()) {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        m_stats.timersExpired += expired.size();
    }
}

EventLoop::Stats EventLoop::stats() const {
    std::lock_guard<std::mutex> lk(m_statsMutex);
    return m_stats;
}

}  // namespace thgh
