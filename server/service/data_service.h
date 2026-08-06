#ifndef THGH_SERVER_SERVICE_DATA_SERVICE_H
#define THGH_SERVER_SERVICE_DATA_SERVICE_H

// ─────────────────────────────────────────────────────────────────────────────
// DataService —— 数据访问业务编排（P6）
//
// ── 为什么要把数据访问收归服务端 ──────────────────────────────────────────
// 改造前每个客户端各自直连数据库，问题有三条，都不是理论上的：
//
//   1. **没有一致性可言**。两个客户端同时打开同一个场景，各自读到的是
//      自己那一刻的快照，谁后保存谁覆盖 —— 先保存的人的改动直接消失，
//      而且没有任何提示。
//   2. **凭据要发给每一台客户机**。数据库地址、账号、密码写在客户端配置里，
//      等于把库的完整权限交给了每一个装了客户端的人。
//      改造后客户端只认服务端地址，库的凭据只存在于服务端。
//   3. **没法做权限与审计**。谁在什么时候改了什么，数据库层面看到的
//      全是同一个账号。
//
// 收归服务端之后，数据访问是单一入口：可以加权限、加审计、加缓存，
// 也可以换数据库而客户端完全无感。
//
// ── 为什么跑在计算线程池里 ────────────────────────────────────────────────
// 数据库操作是**阻塞 IO**：一次查询几百微秒到几毫秒，慢查询更久。
// 直接在 IO 线程（从 Reactor）里执行，会卡住该 Reactor 上的全部连接 ——
// 和 P5 里规划计算的问题完全一样。
// 所以复用同一个 ComputePool：IO 线程只解析请求并派发，
// 数据库操作在计算线程里做，做完经 conn->send() 回推。
//
// 连接池同理不能少：每次请求新建 MySQL 连接要 15.9ms（含 TCP 握手 +
// caching_sha2_password 认证），池化后是 0.1us（P3 实测）。
// ─────────────────────────────────────────────────────────────────────────────

#include "db/conn_pool.h"
#include "net/tcp_connection.h"
#include "service/compute_pool.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace thgh {

class DataService {
public:
    DataService(ComputePool* pool, ConnectionPool* dbPool);

    // 处理一条 data_request。在 IO 线程调用，内部派发到计算线程。
    // 返回 false 表示这条消息不该由本服务处理（类型不匹配）。
    bool handle(const TcpConnectionPtr& conn, std::uint64_t taskId,
                const std::string& payloadJson);

    struct Stats {
        std::uint64_t requests = 0;    // 收到的数据请求
        std::uint64_t succeeded = 0;   // 执行成功
        std::uint64_t failed = 0;      // 执行失败（含非法请求）
        std::uint64_t rejected = 0;    // 计算队列满被拒
        std::uint64_t dropped = 0;     // 执行完但连接已断
    };
    Stats stats() const;

private:
    // 在计算线程里执行。conn 用 weak_ptr —— 数据库操作期间客户端可能断开，
    // 持 shared_ptr 会让连接对象被续命、fd 迟迟不释放（同 P5 的处理）。
    void execute(const std::weak_ptr<TcpConnection>& weakConn,
                 std::uint64_t taskId, std::string op, std::string argsJson);

    ComputePool* m_pool;
    ConnectionPool* m_dbPool;

    std::atomic<std::uint64_t> m_requests{0};
    std::atomic<std::uint64_t> m_succeeded{0};
    std::atomic<std::uint64_t> m_failed{0};
    std::atomic<std::uint64_t> m_rejected{0};
    std::atomic<std::uint64_t> m_dropped{0};
};

}  // namespace thgh

#endif  // THGH_SERVER_SERVICE_DATA_SERVICE_H
