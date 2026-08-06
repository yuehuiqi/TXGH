#ifndef THGH_SERVER_DB_CONN_POOL_H
#define THGH_SERVER_DB_CONN_POOL_H

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionPool —— MySQL 连接池
//
// ── 为什么需要连接池 ──────────────────────────────────────────────────────
// 建立一条 MySQL 连接的代价包括：
//     TCP 三次握手 → MySQL 协议握手 → 认证（口令校验、权限加载）→ 选库
// 这套流程的耗时**远大于一次简单查询本身**。
// 请求量一上来，绝大部分时间会花在反复建连上，而不是干活。
// 连接池把连接复用起来，把这块开销从"每请求一次"摊薄成"启动时若干次"。
// 本项目实测的量化对比见 apps/db_bench.cpp。
//
// ── 初始 / 最大连接数怎么定 ───────────────────────────────────────────────
// 上限不是越大越好：每条连接在 MySQL 服务端都要占一个线程和若干缓冲区，
// 连接数超过服务端处理能力后，请求只会在数据库内部排队，
// 徒增上下文切换而吞吐不再上升，延迟反而恶化。
// 经验起点是**数据库机器的核数量级**，再按压测结果调整。
// 本项目服务端是 4 核，初始 4、上限 16。
//
// ── 空闲连接为什么要健康检查 ──────────────────────────────────────────────
// MySQL 服务端有 wait_timeout（默认 8 小时），会主动断开长时间空闲的连接。
// 连接池若不检查就把一条已死的连接交给业务，表现为**随机的查询失败** ——
// 而且只在低峰期之后出现，极难复现。
// 所以取出连接时若已超过探活间隔就先 ping 一次，失败则丢弃重建。
//
// 线程安全：**是**。这是它与 MySqlConnection 的分工——
// 单连接不保证线程安全，由池保证"一条连接同一时刻只被一个线程持有"。
// ─────────────────────────────────────────────────────────────────────────────

#include "db/mysql_conn.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace thgh {

class ConnectionPool;

// 从池里借出的连接。析构时自动归还 ——
// 这是必须的：任何一条错误返回路径忘了归还，池里的连接就少一条，
// 反复几次之后所有请求都会卡在等待获取上，表现为服务整体挂死。
class PooledConnection {
public:
    PooledConnection() = default;
    PooledConnection(ConnectionPool* pool, std::unique_ptr<MySqlConnection> conn);
    ~PooledConnection();

    // 只可移动不可拷贝：连接的持有权必须唯一
    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    PooledConnection(PooledConnection&& other) noexcept;
    PooledConnection& operator=(PooledConnection&& other) noexcept;

    bool valid() const { return m_conn != nullptr; }
    explicit operator bool() const { return valid(); }

    MySqlConnection* operator->() { return m_conn.get(); }
    MySqlConnection& operator*() { return *m_conn; }
    MySqlConnection* get() { return m_conn.get(); }

    // 提前归还（否则等析构）
    void release();

private:
    ConnectionPool* m_pool = nullptr;
    std::unique_ptr<MySqlConnection> m_conn;
};

class ConnectionPool {
public:
    struct Options {
        std::size_t initialSize = 4;
        std::size_t maxSize = 16;
        // 获取连接的最长等待时间。**必须有上限**：
        // 无限等待会让"数据库变慢"升级成"整个服务卡死"，
        // 而且故障现场看不出是数据库的问题。
        std::chrono::milliseconds acquireTimeout{3000};
        // 空闲连接超过这个时长后，取出时先 ping 一次再交付
        std::chrono::seconds healthCheckIdle{60};
    };

    struct Stats {
        std::uint64_t acquired = 0;        // 累计成功获取次数
        std::uint64_t acquireTimeouts = 0; // 获取超时次数
        std::uint64_t created = 0;         // 累计创建的物理连接数
        std::uint64_t destroyed = 0;       // 累计销毁的物理连接数
        std::uint64_t healthCheckFailed = 0; // 探活失败而被丢弃的连接数
        std::uint64_t waited = 0;          // 因无空闲连接而等待过的次数
        std::size_t idle = 0;              // 当前空闲连接数
        std::size_t inUse = 0;             // 当前借出连接数
        std::size_t peakInUse = 0;         // 借出数峰值，用于校准 maxSize
    };

    ConnectionPool();
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // 初始化并预建 initialSize 条连接。
    // 预建是刻意的：把建连开销放在启动阶段，而不是让第一批用户承担。
    bool init(const MySqlConfig& dbCfg, const Options& opt);
    void shutdown();

    // 借一条连接。池空且已达上限时阻塞等待，超时返回无效对象。
    PooledConnection acquire();

    Stats stats() const;
    const Options& options() const { return m_opt; }

private:
    friend class PooledConnection;
    // 由 PooledConnection 析构时调用
    void giveBack(std::unique_ptr<MySqlConnection> conn);

    std::unique_ptr<MySqlConnection> createConnection();

    struct Idle {
        std::unique_ptr<MySqlConnection> conn;
        std::chrono::steady_clock::time_point since;
    };

    MySqlConfig m_dbCfg;
    Options m_opt;

    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    std::deque<Idle> m_idle;
    std::size_t m_total = 0;   // 已创建且未销毁的物理连接总数
    std::size_t m_inUse = 0;
    bool m_shutdown = false;
    bool m_inited = false;

    mutable std::mutex m_statsMutex;
    Stats m_stats;
};

}  // namespace thgh

#endif  // THGH_SERVER_DB_CONN_POOL_H
