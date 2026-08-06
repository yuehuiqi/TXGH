// 连接池单元测试
//
// 需要一个可用的 MySQL。环境变量未配置时**跳过而不是失败** ——
// CI 上未必有数据库，让整条流水线因此变红是没有意义的。
//   THGH_TEST_DB_HOST / _USER / _PASSWORD / _DATABASE
//
// 重点覆盖：RAII 归还、上限与超时、并发安全、脏连接（残留事务）的清理。

#include "db/conn_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

using thgh::ConnectionPool;
using thgh::MySqlConfig;
using thgh::PooledConnection;

namespace {

const char* envOr(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? v : fallback;
}

bool dbConfigured() { return std::getenv("THGH_TEST_DB_HOST") != nullptr; }

MySqlConfig testConfig() {
    MySqlConfig cfg;
    cfg.host = envOr("THGH_TEST_DB_HOST", "127.0.0.1");
    cfg.user = envOr("THGH_TEST_DB_USER", "thgh");
    cfg.password = envOr("THGH_TEST_DB_PASSWORD", "");
    cfg.database = envOr("THGH_TEST_DB_DATABASE", "thgh");
    return cfg;
}

// 所有用例共用的跳过判断
#define SKIP_IF_NO_DB()                                                     \
    do {                                                                    \
        if (!dbConfigured()) {                                              \
            GTEST_SKIP() << "未设置 THGH_TEST_DB_HOST，跳过需要数据库的用例"; \
        }                                                                   \
    } while (0)

}  // namespace

TEST(ConnectionPool, InitCreatesInitialConnections) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 3;
    opt.maxSize = 5;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    const auto s = pool.stats();
    // 预建是刻意的：把建连开销放在启动阶段，而不是让第一批请求承担
    EXPECT_EQ(s.created, 3u);
    EXPECT_EQ(s.idle, 3u);
    EXPECT_EQ(s.inUse, 0u);
}

TEST(ConnectionPool, AcquireReturnsUsableConnection) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 2;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    auto conn = pool.acquire();
    ASSERT_TRUE(conn);
    EXPECT_TRUE(conn->ping());

    thgh::ResultSet rs;
    ASSERT_TRUE(conn->query("SELECT 1", rs));
    ASSERT_EQ(rs.rowCount(), 1u);
    EXPECT_EQ(rs.at(0, 0), "1");
}

TEST(ConnectionPool, ConnectionIsReturnedOnScopeExit) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 2;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    EXPECT_EQ(pool.stats().idle, 2u);
    {
        auto conn = pool.acquire();
        ASSERT_TRUE(conn);
        EXPECT_EQ(pool.stats().inUse, 1u);
        EXPECT_EQ(pool.stats().idle, 1u);
    }
    // ★ RAII 归还：任何一条错误返回路径忘了归还，池里的连接就少一条，
    //   反复几次之后所有请求都会卡在等待获取上，表现为服务整体挂死
    EXPECT_EQ(pool.stats().inUse, 0u);
    EXPECT_EQ(pool.stats().idle, 2u);
}

TEST(ConnectionPool, ReusesConnectionsInsteadOfCreatingNew) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 2;
    opt.maxSize = 8;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    for (int i = 0; i < 100; ++i) {
        auto conn = pool.acquire();
        ASSERT_TRUE(conn);
    }
    const auto s = pool.stats();
    EXPECT_EQ(s.acquired, 100u);
    // 100 次获取只应创建最初那 2 条物理连接 —— 这正是连接池的意义
    EXPECT_EQ(s.created, 2u);
}

TEST(ConnectionPool, GrowsUpToMaxSize) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 1;
    opt.maxSize = 4;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    std::vector<PooledConnection> held;
    for (int i = 0; i < 4; ++i) {
        auto c = pool.acquire();
        ASSERT_TRUE(c) << "第 " << i + 1 << " 条获取失败";
        held.push_back(std::move(c));
    }
    EXPECT_EQ(pool.stats().inUse, 4u);
    EXPECT_EQ(pool.stats().created, 4u);
}

TEST(ConnectionPool, TimesOutWhenExhausted) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 1;
    opt.maxSize = 1;
    opt.acquireTimeout = std::chrono::milliseconds(200);
    ASSERT_TRUE(pool.init(testConfig(), opt));

    auto held = pool.acquire();
    ASSERT_TRUE(held);

    // ★ 必须有超时上限：无限等待会让"数据库变慢"升级成"整个服务卡死"，
    //   而且故障现场根本看不出是数据库的问题
    const auto t0 = std::chrono::steady_clock::now();
    auto second = pool.acquire();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_FALSE(second) << "池已耗尽却仍返回了连接";
    EXPECT_GE(elapsed, std::chrono::milliseconds(180));
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
    EXPECT_EQ(pool.stats().acquireTimeouts, 1u);
}

TEST(ConnectionPool, WaiterIsServedWhenConnectionReturned) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 1;
    opt.maxSize = 1;
    opt.acquireTimeout = std::chrono::milliseconds(3000);
    ASSERT_TRUE(pool.init(testConfig(), opt));

    auto held = pool.acquire();
    ASSERT_TRUE(held);

    std::atomic<bool> got{false};
    std::thread waiter([&] {
        auto c = pool.acquire();
        got = c.valid();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(got.load()) << "池里没有空闲连接却立刻拿到了";

    held.release();  // 归还，应唤醒等待者
    waiter.join();
    EXPECT_TRUE(got.load());
}

TEST(ConnectionPool, ConcurrentAcquireIsSafe) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 2;
    opt.maxSize = 6;
    opt.acquireTimeout = std::chrono::milliseconds(5000);
    ASSERT_TRUE(pool.init(testConfig(), opt));

    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;
    std::atomic<int> ok{0};

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                auto c = pool.acquire();
                if (c) {
                    thgh::ResultSet rs;
                    if (c->query("SELECT 1", rs) && rs.rowCount() == 1) {
                        ++ok;
                    }
                }
            }
        });
    }
    for (auto& t : ts) t.join();

    EXPECT_EQ(ok.load(), kThreads * kPerThread);
    // 物理连接数不得超过上限
    EXPECT_LE(pool.stats().created, opt.maxSize);
    // 全部归还
    EXPECT_EQ(pool.stats().inUse, 0u);
}

TEST(ConnectionPool, RollsBackLeftoverTransactionOnReturn) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 1;
    opt.maxSize = 1;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    {
        auto conn = pool.acquire();
        ASSERT_TRUE(conn);
        ASSERT_TRUE(conn->begin());
        // 故意不提交就归还 —— 模拟使用方因异常或提前 return 而漏掉 commit
    }

    // ★ 归还前必须把连接恢复到干净状态。
    //   直接放回池里的话，下一个借到它的人会莫名其妙地处在别人的事务里，
    //   而且那个事务还占着行锁。这是连接池最容易出的一类 bug。
    auto next = pool.acquire();
    ASSERT_TRUE(next);
    EXPECT_FALSE(next->inTransaction());
    // 能正常开启新事务，说明上一个确实被清理了
    EXPECT_TRUE(next->begin());
    EXPECT_TRUE(next->rollback());
}

TEST(ConnectionPool, ShutdownWakesBlockedWaiters) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 1;
    opt.maxSize = 1;
    opt.acquireTimeout = std::chrono::milliseconds(10000);
    ASSERT_TRUE(pool.init(testConfig(), opt));

    auto held = pool.acquire();
    ASSERT_TRUE(held);

    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        auto c = pool.acquire();
        returned = true;
        EXPECT_FALSE(c) << "关闭后不应再发放连接";
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(returned.load());

    // 不唤醒等待者的话，它们会一直阻塞到 10 秒超时
    pool.shutdown();
    waiter.join();
    EXPECT_TRUE(returned.load());
}

// ── 事务 RAII ───────────────────────────────────────────────────────────────

TEST(Transaction, AutoRollsBackWhenNotCommitted) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 1;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    auto conn = pool.acquire();
    ASSERT_TRUE(conn);
    conn->execute("DROP TABLE IF EXISTS tx_raii_test");
    ASSERT_TRUE(conn->execute(
        "CREATE TABLE tx_raii_test (id INT PRIMARY KEY) ENGINE=InnoDB"));

    {
        thgh::Transaction tx(*conn);
        ASSERT_TRUE(tx.begun());
        conn->execute("INSERT INTO tx_raii_test (id) VALUES (1)");
        // 不调用 commit，析构时应自动回滚
    }

    thgh::ResultSet rs;
    ASSERT_TRUE(conn->query("SELECT COUNT(*) FROM tx_raii_test", rs));
    EXPECT_EQ(rs.at(0, 0), "0") << "未提交的事务没有被自动回滚";

    conn->execute("DROP TABLE tx_raii_test");
}

TEST(Transaction, CommitPersistsChanges) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 1;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    auto conn = pool.acquire();
    ASSERT_TRUE(conn);
    conn->execute("DROP TABLE IF EXISTS tx_commit_test");
    ASSERT_TRUE(conn->execute(
        "CREATE TABLE tx_commit_test (id INT PRIMARY KEY) ENGINE=InnoDB"));

    {
        thgh::Transaction tx(*conn);
        conn->execute("INSERT INTO tx_commit_test (id) VALUES (1),(2)");
        EXPECT_TRUE(tx.commit());
    }

    thgh::ResultSet rs;
    ASSERT_TRUE(conn->query("SELECT COUNT(*) FROM tx_commit_test", rs));
    EXPECT_EQ(rs.at(0, 0), "2");

    conn->execute("DROP TABLE tx_commit_test");
}

TEST(MySqlConnection, RejectsNestedTransaction) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 1;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    auto conn = pool.acquire();
    ASSERT_TRUE(conn);
    ASSERT_TRUE(conn->begin());
    // MySQL 不支持嵌套事务，BEGIN 会隐式提交上一个 —— 那是静默的数据风险，
    // 这里直接拒绝而不是放行
    EXPECT_FALSE(conn->begin());
    EXPECT_TRUE(conn->rollback());
}

TEST(MySqlConnection, PreparedStatementBlocksSqlInjection) {
    SKIP_IF_NO_DB();
    ConnectionPool pool;
    ConnectionPool::Options opt;
    opt.initialSize = 1;
    ASSERT_TRUE(pool.init(testConfig(), opt));

    auto conn = pool.acquire();
    ASSERT_TRUE(conn);
    conn->execute("DROP TABLE IF EXISTS inject_test");
    ASSERT_TRUE(conn->execute(
        "CREATE TABLE inject_test (id INT PRIMARY KEY, name VARCHAR(100)) "
        "ENGINE=InnoDB"));
    ASSERT_TRUE(conn->execute("INSERT INTO inject_test VALUES (1,'alice')"));

    // 经典注入载荷。走预处理协议时它只是一个普通的字符串值，
    // 不参与 SQL 文本解析，因此从原理上不可能改变语句结构
    const std::string evil = "x' OR '1'='1";
    thgh::ResultSet rs;
    ASSERT_TRUE(conn->queryPrepared("SELECT id FROM inject_test WHERE name = ?",
                                    {thgh::Param::ofString(evil)}, rs));
    EXPECT_EQ(rs.rowCount(), 0u) << "注入载荷被当作 SQL 执行了";

    // 表必须完好无损
    thgh::ResultSet chk;
    ASSERT_TRUE(conn->query("SELECT COUNT(*) FROM inject_test", chk));
    EXPECT_EQ(chk.at(0, 0), "1");

    conn->execute("DROP TABLE inject_test");
}
