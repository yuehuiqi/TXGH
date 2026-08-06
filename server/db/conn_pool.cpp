#include "db/conn_pool.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace thgh {

// ── PooledConnection ────────────────────────────────────────────────────────

PooledConnection::PooledConnection(ConnectionPool* pool,
                                   std::unique_ptr<MySqlConnection> conn)
    : m_pool(pool), m_conn(std::move(conn)) {}

PooledConnection::~PooledConnection() { release(); }

PooledConnection::PooledConnection(PooledConnection&& other) noexcept
    : m_pool(other.m_pool), m_conn(std::move(other.m_conn)) {
    other.m_pool = nullptr;
}

PooledConnection& PooledConnection::operator=(
    PooledConnection&& other) noexcept {
    if (this != &other) {
        release();
        m_pool = other.m_pool;
        m_conn = std::move(other.m_conn);
        other.m_pool = nullptr;
    }
    return *this;
}

void PooledConnection::release() {
    if (m_pool != nullptr && m_conn != nullptr) {
        m_pool->giveBack(std::move(m_conn));
    }
    m_pool = nullptr;
    m_conn.reset();
}

// ── ConnectionPool ──────────────────────────────────────────────────────────

ConnectionPool::ConnectionPool() = default;

ConnectionPool::~ConnectionPool() { shutdown(); }

bool ConnectionPool::init(const MySqlConfig& dbCfg, const Options& opt) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_inited) {
        return true;
    }
    m_dbCfg = dbCfg;
    m_opt = opt;
    if (m_opt.maxSize < 1) {
        m_opt.maxSize = 1;
    }
    if (m_opt.initialSize > m_opt.maxSize) {
        m_opt.initialSize = m_opt.maxSize;
    }

    // 预建连接：把建连开销放在启动阶段，而不是让第一批请求承担。
    // 这也顺带在启动时就验证了数据库配置是否正确 ——
    // 配置写错的话现在就失败，而不是等到线上第一个请求进来才暴露。
    for (std::size_t i = 0; i < m_opt.initialSize; ++i) {
        auto conn = createConnection();
        if (!conn) {
            std::fprintf(stderr,
                         "[ConnectionPool] 初始化失败：第 %zu 条连接建立不成功\n",
                         i + 1);
            // 已建好的要释放掉，不留半成品
            m_idle.clear();
            m_total = 0;
            return false;
        }
        m_idle.push_back({std::move(conn), std::chrono::steady_clock::now()});
    }

    m_inited = true;
    m_shutdown = false;
    return true;
}

void ConnectionPool::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_shutdown) {
            return;
        }
        m_shutdown = true;
        m_idle.clear();  // unique_ptr 析构会关闭连接
        m_inited = false;
    }
    // 唤醒所有等待者，让它们看到 shutdown 标志后返回，
    // 否则会一直阻塞到超时
    m_cond.notify_all();
}

std::unique_ptr<MySqlConnection> ConnectionPool::createConnection() {
    auto conn = std::unique_ptr<MySqlConnection>(new MySqlConnection());
    if (!conn->connect(m_dbCfg)) {
        std::fprintf(stderr, "[ConnectionPool] 建立连接失败: %s\n",
                     conn->lastError().c_str());
        return nullptr;
    }
    ++m_total;
    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        ++m_stats.created;
    }
    return conn;
}

PooledConnection ConnectionPool::acquire() {
    std::unique_lock<std::mutex> lk(m_mutex);
    const auto deadline = std::chrono::steady_clock::now() + m_opt.acquireTimeout;

    for (;;) {
        if (m_shutdown) {
            return PooledConnection();
        }

        // 1. 优先复用空闲连接
        while (!m_idle.empty()) {
            Idle item = std::move(m_idle.front());
            m_idle.pop_front();

            const auto idleFor = std::chrono::steady_clock::now() - item.since;
            bool healthy = true;
            if (idleFor > m_opt.healthCheckIdle) {
                // 空闲太久，可能已被服务端的 wait_timeout 断掉。
                // ping 要在锁外做（涉及网络往返），但这里为简化仍持锁 ——
                // 探活只在超过 healthCheckIdle 的连接上发生，频率极低，
                // 且 loopback 上的 ping 是微秒级，持锁时间可以接受。
                healthy = item.conn->ping();
                if (!healthy) {
                    --m_total;
                    std::lock_guard<std::mutex> slk(m_statsMutex);
                    ++m_stats.healthCheckFailed;
                    ++m_stats.destroyed;
                }
            }

            if (healthy) {
                ++m_inUse;
                {
                    std::lock_guard<std::mutex> slk(m_statsMutex);
                    ++m_stats.acquired;
                    m_stats.peakInUse = std::max(m_stats.peakInUse, m_inUse);
                }
                return PooledConnection(this, std::move(item.conn));
            }
            // 探活失败，继续看下一条空闲连接
        }

        // 2. 没有空闲连接，但还没到上限 → 新建
        if (m_total < m_opt.maxSize) {
            auto conn = createConnection();
            if (conn) {
                ++m_inUse;
                {
                    std::lock_guard<std::mutex> slk(m_statsMutex);
                    ++m_stats.acquired;
                    m_stats.peakInUse = std::max(m_stats.peakInUse, m_inUse);
                }
                return PooledConnection(this, std::move(conn));
            }
            // 建连失败（数据库不可用），落到下面等待，
            // 让已借出的连接归还后复用，而不是不停地重试建连
        }

        // 3. 已达上限，等待有人归还
        {
            std::lock_guard<std::mutex> slk(m_statsMutex);
            ++m_stats.waited;
        }
        // 必须用 wait_until 而不是 wait：
        // 无限等待会让"数据库变慢"升级成"整个服务卡死"，
        // 而且故障现场根本看不出是数据库的问题
        if (m_cond.wait_until(lk, deadline) == std::cv_status::timeout) {
            if (m_idle.empty() && m_total >= m_opt.maxSize) {
                std::lock_guard<std::mutex> slk(m_statsMutex);
                ++m_stats.acquireTimeouts;
                return PooledConnection();
            }
        }
    }
}

void ConnectionPool::giveBack(std::unique_ptr<MySqlConnection> conn) {
    if (!conn) {
        return;
    }

    // 归还前必须把连接恢复到干净状态。
    // 使用方可能因为异常或提前 return 而留下未提交的事务 ——
    // 直接放回池里的话，下一个借到它的人会莫名其妙地处在别人的事务里，
    // 而且那个事务还占着行锁。这是连接池最容易出的一类 bug。
    if (conn->inTransaction()) {
        std::fprintf(stderr,
                     "[ConnectionPool] 归还的连接仍处于事务中，已自动回滚\n");
        conn->rollback();
    }

    bool destroyed = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_inUse > 0) {
            --m_inUse;
        }
        if (m_shutdown || !conn->connected()) {
            --m_total;
            destroyed = true;
        } else {
            m_idle.push_back({std::move(conn), std::chrono::steady_clock::now()});
        }
    }
    if (destroyed) {
        std::lock_guard<std::mutex> slk(m_statsMutex);
        ++m_stats.destroyed;
    }
    // 在锁外通知，避免被唤醒的线程立刻又因拿不到锁而挂起
    m_cond.notify_one();
}

ConnectionPool::Stats ConnectionPool::stats() const {
    Stats s;
    {
        std::lock_guard<std::mutex> slk(m_statsMutex);
        s = m_stats;
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        s.idle = m_idle.size();
        s.inUse = m_inUse;
    }
    return s;
}

}  // namespace thgh
