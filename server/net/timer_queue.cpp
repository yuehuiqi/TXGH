#include "net/timer_queue.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace thgh {

TimerId TimerQueue::nextId() {
    // 溢出保护：跳过 0（它是 kInvalidTimerId）。
    // 64 位 id 每纳秒分配一个也要几百年才绕回来，这里只是不留隐患。
    if (m_nextId == std::numeric_limits<TimerId>::max()) {
        m_nextId = 1;
    }
    return m_nextId++;
}

TimerId TimerQueue::addTimer(TimePoint when, Callback cb) {
    if (!cb) {
        return kInvalidTimerId;
    }
    const TimerId id = nextId();
    m_timers.emplace(id, TimerData{std::move(cb), Millis(0)});
    m_heap.push(Entry{when, id});
    return id;
}

TimerId TimerQueue::addRepeating(TimePoint first, Millis interval, Callback cb) {
    if (!cb) {
        return kInvalidTimerId;
    }
    if (interval <= Millis(0)) {
        // 周期非正会导致 expire 里无限自我重排，退化成一次性更安全
        return addTimer(first, std::move(cb));
    }
    const TimerId id = nextId();
    m_timers.emplace(id, TimerData{std::move(cb), interval});
    m_heap.push(Entry{first, id});
    return id;
}

bool TimerQueue::cancel(TimerId id) {
    // 惰性删除：只抹数据表，堆里那条留着，pop 到时发现查不到就跳过
    return m_timers.erase(id) > 0;
}

int TimerQueue::nextTimeoutMs(TimePoint now) const {
    // 堆顶可能是已被取消的失效记录，要往下找到第一条有效的。
    // 这里不能修改 m_heap（const 方法），所以用一份拷贝逐个看 ——
    // 实践中失效记录很少，循环几乎不会真的执行。
    auto heap = m_heap;
    while (!heap.empty()) {
        const Entry& top = heap.top();
        if (m_timers.find(top.id) == m_timers.end()) {
            heap.pop();  // 失效记录，跳过
            continue;
        }
        if (top.when <= now) {
            return 0;  // 已到期，让 epoll_wait 立即返回
        }
        const auto ms =
            std::chrono::duration_cast<Millis>(top.when - now).count();
        // 向上取整到至少 1ms：返回 0 会让 epoll_wait 空转，
        // 而 duration_cast 是向零截断，0.4ms 会变成 0
        return static_cast<int>(std::max<std::int64_t>(ms, 1));
    }
    return -1;  // 无定时器，epoll_wait 无限阻塞，零 CPU 占用
}

void TimerQueue::expire(TimePoint now, std::vector<Callback>& out) {
    // 周期定时器要重新入堆。如果直接在循环里 push，可能把刚排好的
    // 下一次触发又立刻弹出来（interval 很小时会死循环），
    // 所以先收集、循环结束后统一入堆。
    std::vector<Entry> reschedule;

    while (!m_heap.empty()) {
        const Entry top = m_heap.top();
        if (top.when > now) {
            break;  // 堆顶未到期，后面的更晚，可以停
        }
        m_heap.pop();

        auto it = m_timers.find(top.id);
        if (it == m_timers.end()) {
            continue;  // 已取消的残留记录
        }

        out.push_back(it->second.cb);

        if (it->second.interval > Millis(0)) {
            // 周期定时器：从**本次预定到期时刻**递推，而不是从 now 递推。
            // 从 now 递推会让每次触发的延迟累积，长时间运行后节奏越漂越偏。
            TimePoint next = top.when + it->second.interval;
            // 若因阻塞错过了多个周期，直接跳到 now 之后的下一个整点，
            // 避免补偿式地连续触发一大堆（惊群式补偿）
            if (next <= now) {
                const auto behind = now - top.when;
                const auto periods =
                    behind / it->second.interval + 1;
                next = top.when + it->second.interval * periods;
            }
            reschedule.push_back(Entry{next, top.id});
        } else {
            m_timers.erase(it);
        }
    }

    for (const Entry& e : reschedule) {
        m_heap.push(e);
    }
}

void TimerQueue::clear() {
    m_timers.clear();
    while (!m_heap.empty()) {
        m_heap.pop();
    }
}

}  // namespace thgh
