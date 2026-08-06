// TimerQueue 单元测试
//
// 时间全部由测试注入，不使用 sleep：
// 测"1 小时后到期"也是瞬间完成，且不会因为 CI 机器负载高而 flaky。

#include "net/timer_queue.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using thgh::TimerId;
using thgh::TimerQueue;

namespace {

using TimePoint = TimerQueue::TimePoint;
using Millis = TimerQueue::Millis;

TimePoint base() { return TimePoint(Millis(1'000'000)); }

// 推进到 now，执行所有到期回调，返回执行了几个
std::size_t fire(TimerQueue& q, TimePoint now) {
    std::vector<TimerQueue::Callback> cbs;
    q.expire(now, cbs);
    for (auto& cb : cbs) {
        cb();
    }
    return cbs.size();
}

}  // namespace

// ── 基本行为 ────────────────────────────────────────────────────────────────

TEST(TimerQueue, EmptyQueueHasNoTimeout) {
    TimerQueue q;
    EXPECT_EQ(q.nextTimeoutMs(base()), -1);  // -1 → epoll_wait 无限阻塞
    EXPECT_TRUE(q.empty());
}

TEST(TimerQueue, FiresOneShotTimerExactlyOnce) {
    TimerQueue q;
    int count = 0;
    q.addTimer(base() + Millis(100), [&] { ++count; });

    EXPECT_EQ(fire(q, base() + Millis(99)), 0u);  // 差 1ms 不触发
    EXPECT_EQ(count, 0);

    EXPECT_EQ(fire(q, base() + Millis(100)), 1u);  // 到点触发
    EXPECT_EQ(count, 1);

    EXPECT_EQ(fire(q, base() + Millis(1000)), 0u);  // 不会再触发
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(q.empty());
}

TEST(TimerQueue, FiresInChronologicalOrder) {
    TimerQueue q;
    std::vector<int> order;
    // 故意乱序添加
    q.addTimer(base() + Millis(300), [&] { order.push_back(3); });
    q.addTimer(base() + Millis(100), [&] { order.push_back(1); });
    q.addTimer(base() + Millis(200), [&] { order.push_back(2); });

    fire(q, base() + Millis(500));
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(TimerQueue, SameDeadlineFiresInInsertionOrder) {
    // 到期时间相同时按 id 排序，保证顺序确定、测试可复现
    TimerQueue q;
    std::vector<int> order;
    for (int i = 0; i < 5; ++i) {
        q.addTimer(base() + Millis(10), [&order, i] { order.push_back(i); });
    }
    fire(q, base() + Millis(10));
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(TimerQueue, FiresAllOverdueTimersAtOnce) {
    // 线程被阻塞很久之后，所有错过的一次性定时器应该一次性全部触发
    TimerQueue q;
    int count = 0;
    for (int i = 1; i <= 10; ++i) {
        q.addTimer(base() + Millis(i * 10), [&] { ++count; });
    }
    EXPECT_EQ(fire(q, base() + Millis(10'000)), 10u);
    EXPECT_EQ(count, 10);
}

TEST(TimerQueue, RejectsNullCallback) {
    TimerQueue q;
    EXPECT_EQ(q.addTimer(base(), nullptr), TimerQueue::kInvalidTimerId);
    EXPECT_TRUE(q.empty());
}

// ── nextTimeoutMs：直接喂给 epoll_wait 的值 ─────────────────────────────────

TEST(TimerQueue, ReportsTimeUntilNextExpiry) {
    TimerQueue q;
    q.addTimer(base() + Millis(250), [] {});
    EXPECT_EQ(q.nextTimeoutMs(base()), 250);
    EXPECT_EQ(q.nextTimeoutMs(base() + Millis(100)), 150);
}

TEST(TimerQueue, ReportsZeroWhenAlreadyDue) {
    // 已到期返回 0，让 epoll_wait 立即返回去处理定时任务
    TimerQueue q;
    q.addTimer(base() + Millis(100), [] {});
    EXPECT_EQ(q.nextTimeoutMs(base() + Millis(100)), 0);
    EXPECT_EQ(q.nextTimeoutMs(base() + Millis(999)), 0);
}

TEST(TimerQueue, SubMillisecondRemainderRoundsUpToOne) {
    // duration_cast 向零截断，0.4ms 会变成 0，会让 epoll_wait 空转。
    // 必须向上取整到至少 1ms。
    TimerQueue q;
    q.addTimer(base() + std::chrono::microseconds(400), [] {});
    EXPECT_EQ(q.nextTimeoutMs(base()), 1);
}

TEST(TimerQueue, UsesEarliestAmongMany) {
    TimerQueue q;
    q.addTimer(base() + Millis(500), [] {});
    q.addTimer(base() + Millis(50), [] {});
    q.addTimer(base() + Millis(5000), [] {});
    EXPECT_EQ(q.nextTimeoutMs(base()), 50);
}

// ── 取消（惰性删除）────────────────────────────────────────────────────────

TEST(TimerQueue, CancelledTimerDoesNotFire) {
    TimerQueue q;
    int count = 0;
    const TimerId id = q.addTimer(base() + Millis(100), [&] { ++count; });

    EXPECT_TRUE(q.cancel(id));
    EXPECT_EQ(fire(q, base() + Millis(1000)), 0u);
    EXPECT_EQ(count, 0);
}

TEST(TimerQueue, CancelUnknownIdReturnsFalse) {
    TimerQueue q;
    EXPECT_FALSE(q.cancel(12345));
    EXPECT_FALSE(q.cancel(TimerQueue::kInvalidTimerId));
}

TEST(TimerQueue, CancelTwiceIsSafe) {
    TimerQueue q;
    const TimerId id = q.addTimer(base() + Millis(100), [] {});
    EXPECT_TRUE(q.cancel(id));
    EXPECT_FALSE(q.cancel(id));  // 第二次应返回 false 而不是崩溃
}

TEST(TimerQueue, CancellingOneDoesNotAffectOthers) {
    TimerQueue q;
    std::vector<int> fired;
    q.addTimer(base() + Millis(100), [&] { fired.push_back(1); });
    const TimerId id2 = q.addTimer(base() + Millis(200), [&] { fired.push_back(2); });
    q.addTimer(base() + Millis(300), [&] { fired.push_back(3); });

    q.cancel(id2);
    fire(q, base() + Millis(1000));
    EXPECT_EQ(fired, (std::vector<int>{1, 3}));
}

TEST(TimerQueue, StaleHeapEntriesAreSkippedByNextTimeout) {
    // 惰性删除会在堆里留下失效记录，nextTimeoutMs 必须跳过它们，
    // 否则会返回一个永远不会触发的时间点，导致 epoll_wait 提前醒来空转
    TimerQueue q;
    const TimerId early = q.addTimer(base() + Millis(10), [] {});
    q.addTimer(base() + Millis(500), [] {});

    q.cancel(early);
    // 堆里还留着 early 那条，但下次超时应报 500 而不是 10
    EXPECT_EQ(q.nextTimeoutMs(base()), 500);
    EXPECT_EQ(q.size(), 1u);
    EXPECT_EQ(q.heapSize(), 2u);  // 失效记录仍在堆中
}

TEST(TimerQueue, StaleEntriesAreReclaimedWhenPopped) {
    // 失效记录不会无限累积：到期被 pop 时清掉
    TimerQueue q;
    for (int i = 0; i < 100; ++i) {
        const TimerId id = q.addTimer(base() + Millis(10), [] {});
        q.cancel(id);
    }
    EXPECT_EQ(q.heapSize(), 100u);
    EXPECT_EQ(q.size(), 0u);

    fire(q, base() + Millis(20));
    EXPECT_EQ(q.heapSize(), 0u);  // 全部回收
}

TEST(TimerQueue, TimerCanCancelItselfFromCallback) {
    // 回调里操作定时器队列不能破坏遍历 ——
    // 这正是 expire() 返回回调而不是就地执行的原因之一
    TimerQueue q;
    TimerId id = TimerQueue::kInvalidTimerId;
    int count = 0;
    id = q.addRepeating(base() + Millis(10), Millis(10), [&] {
        ++count;
        q.cancel(id);
    });

    fire(q, base() + Millis(10));
    EXPECT_EQ(count, 1);
    fire(q, base() + Millis(1000));
    EXPECT_EQ(count, 1);  // 已自我取消，不再触发
}

TEST(TimerQueue, CallbackCanAddNewTimer) {
    TimerQueue q;
    int outer = 0, inner = 0;
    q.addTimer(base() + Millis(10), [&] {
        ++outer;
        q.addTimer(base() + Millis(20), [&] { ++inner; });
    });

    fire(q, base() + Millis(10));
    EXPECT_EQ(outer, 1);
    EXPECT_EQ(inner, 0);

    fire(q, base() + Millis(20));
    EXPECT_EQ(inner, 1);
}

// ── 周期定时器 ──────────────────────────────────────────────────────────────

TEST(TimerQueue, RepeatingTimerFiresEveryInterval) {
    TimerQueue q;
    int count = 0;
    q.addRepeating(base() + Millis(100), Millis(100), [&] { ++count; });

    for (int i = 1; i <= 5; ++i) {
        fire(q, base() + Millis(100 * i));
    }
    EXPECT_EQ(count, 5);
    EXPECT_EQ(q.size(), 1u);  // 周期定时器一直在
}

TEST(TimerQueue, RepeatingScheduleDoesNotDrift) {
    // ★ 从"本次预定到期时刻"递推，而不是从 now 递推。
    //   从 now 递推的话每次触发的延迟会累积，长时间运行后节奏越漂越偏。
    TimerQueue q;
    std::vector<long long> fireTimes;
    q.addRepeating(base() + Millis(100), Millis(100), [&] {
        fireTimes.push_back(0);  // 占位，下面看 nextTimeoutMs
    });

    // 每次都晚 30ms 才处理
    fire(q, base() + Millis(130));
    // 下一次仍应排在 200ms（而不是 130+100=230ms）
    EXPECT_EQ(q.nextTimeoutMs(base() + Millis(130)), 70);

    fire(q, base() + Millis(230));
    EXPECT_EQ(q.nextTimeoutMs(base() + Millis(230)), 70);  // 排在 300ms
}

TEST(TimerQueue, MissedPeriodsDoNotCauseBurstCatchUp) {
    // 若线程阻塞很久错过了很多个周期，不该补偿式地连续触发一大堆，
    // 而应直接跳到当前时间之后的下一个周期点
    TimerQueue q;
    int count = 0;
    q.addRepeating(base() + Millis(10), Millis(10), [&] { ++count; });

    // 阻塞了 1 秒 = 错过约 100 个周期
    fire(q, base() + Millis(1010));
    EXPECT_EQ(count, 1) << "补偿式触发了 " << count << " 次";

    // 下一次应排在 1020ms
    EXPECT_EQ(q.nextTimeoutMs(base() + Millis(1010)), 10);
}

TEST(TimerQueue, NonPositiveIntervalDegradesToOneShot) {
    // 周期非正会导致 expire 里无限自我重排
    TimerQueue q;
    int count = 0;
    q.addRepeating(base() + Millis(10), Millis(0), [&] { ++count; });

    fire(q, base() + Millis(1000));
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(q.empty());
}

TEST(TimerQueue, MixesOneShotAndRepeating) {
    TimerQueue q;
    int once = 0, repeat = 0;
    q.addTimer(base() + Millis(50), [&] { ++once; });
    q.addRepeating(base() + Millis(30), Millis(30), [&] { ++repeat; });

    fire(q, base() + Millis(30));   // repeat
    fire(q, base() + Millis(50));   // once
    fire(q, base() + Millis(60));   // repeat
    fire(q, base() + Millis(90));   // repeat

    EXPECT_EQ(once, 1);
    EXPECT_EQ(repeat, 3);
    EXPECT_EQ(q.size(), 1u);
}

// ── 心跳超时的实际用法 ──────────────────────────────────────────────────────

TEST(TimerQueue, ModelsHeartbeatTimeoutRefresh) {
    // 服务端踢除半开连接的实际模式：
    // 每次收到数据就取消旧的超时定时器、重新排一个
    TimerQueue q;
    bool kicked = false;
    TimerId timeoutId = TimerQueue::kInvalidTimerId;

    auto refresh = [&](TimePoint now) {
        if (timeoutId != TimerQueue::kInvalidTimerId) {
            q.cancel(timeoutId);
        }
        timeoutId = q.addTimer(now + Millis(30'000), [&] { kicked = true; });
    };

    refresh(base());
    // 每 10 秒来一次数据，始终不该被踢
    for (int i = 1; i <= 10; ++i) {
        const auto now = base() + Millis(10'000 * i);
        fire(q, now);
        EXPECT_FALSE(kicked) << "第 " << i << " 次刷新后被误踢";
        refresh(now);
    }

    // 停止发送，30 秒后应被踢除
    fire(q, base() + Millis(100'000 + 30'000));
    EXPECT_TRUE(kicked);
}

TEST(TimerQueue, ClearRemovesEverything) {
    TimerQueue q;
    q.addTimer(base() + Millis(10), [] {});
    q.addRepeating(base() + Millis(20), Millis(20), [] {});
    EXPECT_EQ(q.size(), 2u);

    q.clear();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.heapSize(), 0u);
    EXPECT_EQ(q.nextTimeoutMs(base()), -1);
}
