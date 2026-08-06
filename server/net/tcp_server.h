#ifndef THGH_SERVER_NET_TCP_SERVER_H
#define THGH_SERVER_NET_TCP_SERVER_H

// ─────────────────────────────────────────────────────────────────────────────
// TcpServer —— 把主从 Reactor 串起来
//
//        ┌──────────────── 主 Reactor（主线程）────────────────┐
//        │  Acceptor: epoll 监听 listen fd，循环 accept 到 EAGAIN │
//        │  心跳巡检定时器：踢除超时连接                          │
//        └───────────────────────┬────────────────────────────┘
//                                │ 轮询分配 connfd
//        ┌───────────────────────┴────────────────────────────┐
//        │  从 Reactor × N（每个一线程、一个独立 epoll 实例）      │
//        │  TcpConnection: ET 循环读 → 分帧 → 业务回调 → 写      │
//        └────────────────────────────────────────────────────┘
//
// ── 心跳超时踢除 ──────────────────────────────────────────────────────────
// 半开连接（对端进程被 kill、网线被拔、NAT 超时）在 TCP 层可能长时间不被发现，
// 服务端这边连接还"活着"，白白占着 fd 和内存。
// 做法：每条连接记录最后活跃时间，主 Reactor 上挂一个周期定时器巡检，
// 超过阈值的直接 forceClose。
//
// 用**一个周期定时器巡检全部连接**，而不是每条连接各挂一个超时定时器：
//   * 每连接一个定时器：定时器数量与连接数同阶，每次收到数据都要
//     取消旧的、排一个新的，堆操作频繁
//   * 单个巡检定时器：定时器只有一个，代价是遍历连接表 O(n)，
//     但巡检周期是秒级，n 在万级以内完全可以接受
// 本项目连接数是百级，后者明显更划算。
// ─────────────────────────────────────────────────────────────────────────────

#include "net/acceptor.h"
#include "net/event_loop_thread_pool.h"
#include "net/event_loop.h"
#include "net/tcp_connection.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace thgh {

class EventLoop;

class TcpServer {
public:
    using ConnectionCallback = TcpConnection::ConnectionCallback;
    using MessageCallback = TcpConnection::MessageCallback;

    struct Options {
        std::size_t numThreads = 0;   // 从 Reactor 数量，0 = 单线程模式
        bool reusePort = false;
        // 心跳超时秒数。超过这个时长没收到任何数据即判定为半开连接并踢除。
        int idleTimeoutSec = 30;
        // 巡检周期秒数。取超时的 1/3，保证超时判定的误差不超过一个巡检周期。
        int checkIntervalSec = 10;
    };

    TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void setOptions(const Options& opt) { m_options = opt; }
    const Options& options() const { return m_options; }

    void setConnectionCallback(ConnectionCallback cb) {
        m_connectionCallback = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb) {
        m_messageCallback = std::move(cb);
    }

    // 启动监听与从 Reactor 线程。返回 false 表示 bind/listen 失败。
    bool start();
    void stop();

    const std::string& name() const { return m_name; }
    const InetAddress& listenAddress() const { return m_listenAddr; }

    // ── 运行指标（压测要用）─────────────────────────────────────────────
    struct Stats {
        std::uint64_t connectionsAccepted = 0;  // 累计接受的连接数
        std::uint64_t connectionsClosed = 0;    // 累计关闭的连接数
        std::uint64_t idleKicked = 0;           // 因心跳超时被踢除的连接数
        std::size_t currentConnections = 0;     // 当前在线连接数
        std::size_t peakConnections = 0;        // 在线连接数峰值
    };
    Stats stats() const;

    // 汇总全部 EventLoop（主 + 从）的运行指标。
    // 只看主 Reactor 会严重误导：它只负责 accept，真正的 IO 事件都在从 Reactor 上，
    // 压测时主 Reactor 的事件数几乎为 0，看着像"服务端没干活"。
    EventLoop::Stats aggregatedLoopStats() const;

private:
    void onNewConnection(int sockfd, const InetAddress& peer);
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn);
    void checkIdleConnections();

    EventLoop* m_loop;  // 主 Reactor
    InetAddress m_listenAddr;
    std::string m_name;
    Options m_options;

    std::unique_ptr<Acceptor> m_acceptor;
    std::unique_ptr<EventLoopThreadPool> m_threadPool;

    ConnectionCallback m_connectionCallback;
    MessageCallback m_messageCallback;

    // 连接表只在主 Reactor 线程访问（新增在 onNewConnection，
    // 移除经 removeConnection 投递回主 loop），因此无需加锁
    std::map<std::string, TcpConnectionPtr> m_connections;
    std::uint64_t m_nextConnId = 1;
    bool m_started = false;

    mutable std::mutex m_statsMutex;
    Stats m_stats;
};

}  // namespace thgh

#endif  // THGH_SERVER_NET_TCP_SERVER_H
