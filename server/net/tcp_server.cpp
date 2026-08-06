#include "net/tcp_server.h"

#include "net/event_loop.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace thgh {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr,
                     std::string name)
    : m_loop(loop), m_listenAddr(listenAddr), m_name(std::move(name)) {}

TcpServer::~TcpServer() { stop(); }

bool TcpServer::start() {
    m_loop->assertInLoopThread();
    if (m_started) {
        return true;
    }

    m_acceptor.reset(new Acceptor(m_loop, m_listenAddr, m_options.reusePort));
    m_acceptor->setNewConnectionCallback(
        [this](int sockfd, const InetAddress& peer) {
            onNewConnection(sockfd, peer);
        });

    m_threadPool.reset(new EventLoopThreadPool(m_loop, m_name));
    m_threadPool->setThreadNum(m_options.numThreads);
    m_threadPool->start();

    if (!m_acceptor->listen()) {
        return false;
    }

    // 心跳巡检：一个周期定时器扫全表，而不是每条连接各挂一个定时器
    if (m_options.idleTimeoutSec > 0 && m_options.checkIntervalSec > 0) {
        m_loop->runEvery(
            std::chrono::milliseconds(m_options.checkIntervalSec * 1000),
            [this] { checkIdleConnections(); });
    }

    m_started = true;
    std::printf("[TcpServer %s] 监听 %s，从 Reactor 线程数 %zu\n",
                m_name.c_str(), m_listenAddr.toIpPort().c_str(),
                m_threadPool->threadNum());
    return true;
}

void TcpServer::stop() {
    if (!m_started) {
        return;
    }
    m_started = false;

    // 先关掉所有连接。每条连接要在**它自己的 loop 线程**里销毁，
    // 所以用 runInLoop 投递过去，不能在主线程直接动。
    for (auto& kv : m_connections) {
        TcpConnectionPtr conn = kv.second;
        conn->getLoop()->runInLoop([conn] { conn->connectDestroyed(); });
    }
    m_connections.clear();

    if (m_threadPool) {
        m_threadPool->stop();
    }
    m_acceptor.reset();
}

void TcpServer::onNewConnection(int sockfd, const InetAddress& peer) {
    m_loop->assertInLoopThread();

    // 自连接检测：客户端连本机时内核可能分配到与目标端口相同的源端口，
    // 形成自己连自己的畸形连接，行为异常且难以察觉
    if (sockets::isSelfConnect(sockfd)) {
        std::fprintf(stderr, "[TcpServer %s] 检测到自连接，已丢弃\n",
                     m_name.c_str());
        ::close(sockfd);
        return;
    }

    // 轮询选一个从 Reactor 承载这条连接
    EventLoop* ioLoop = m_threadPool->getNextLoop();

    const std::string connName =
        m_name + "-" + peer.toIpPort() + "#" + std::to_string(m_nextConnId++);

    auto conn = std::make_shared<TcpConnection>(
        ioLoop, connName, sockfd, sockets::localAddress(sockfd), peer);

    conn->setConnectionCallback(m_connectionCallback);
    conn->setMessageCallback(m_messageCallback);
    conn->setCloseCallback([this](const TcpConnectionPtr& c) {
        removeConnection(c);
    });

    m_connections[connName] = conn;

    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        ++m_stats.connectionsAccepted;
        m_stats.currentConnections = m_connections.size();
        m_stats.peakConnections =
            std::max(m_stats.peakConnections, m_stats.currentConnections);
    }

    // 连接的建立动作要在它所属的 loop 线程里完成
    ioLoop->runInLoop([conn] { conn->connectEstablished(); });
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    // 本函数由连接所在的**从 Reactor 线程**调用，
    // 但连接表属于主 Reactor 线程，必须投递过去操作
    m_loop->runInLoop([this, conn] { removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    m_loop->assertInLoopThread();
    m_connections.erase(conn->name());

    {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        ++m_stats.connectionsClosed;
        m_stats.currentConnections = m_connections.size();
    }

    // 销毁动作回到连接自己的 loop 线程执行。
    // 这里必须用 queueInLoop 而不是直接调用：当前可能正处在该连接的
    // 事件处理调用栈中，立刻销毁会导致栈上层继续访问已释放对象。
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop([conn] { conn->connectDestroyed(); });
}

void TcpServer::checkIdleConnections() {
    m_loop->assertInLoopThread();

    const auto now = TimerQueue::Clock::now();
    const auto timeout = std::chrono::seconds(m_options.idleTimeoutSec);

    // 先收集再踢除：forceClose 会触发 removeConnection，
    // 那会修改 m_connections，在遍历过程中修改容器是未定义行为
    std::vector<TcpConnectionPtr> idle;
    for (const auto& kv : m_connections) {
        if (now - kv.second->lastActiveTime() > timeout) {
            idle.push_back(kv.second);
        }
    }

    for (const TcpConnectionPtr& conn : idle) {
        std::printf("[TcpServer %s] 连接 %s 超过 %d 秒无数据，判定半开并踢除\n",
                    m_name.c_str(), conn->name().c_str(),
                    m_options.idleTimeoutSec);
        conn->forceClose();
    }

    if (!idle.empty()) {
        std::lock_guard<std::mutex> lk(m_statsMutex);
        m_stats.idleKicked += idle.size();
    }
}

EventLoop::Stats TcpServer::aggregatedLoopStats() const {
    EventLoop::Stats total;
    // 主 Reactor
    const auto base = m_loop->stats();
    total.loopIterations += base.loopIterations;
    total.eventsHandled += base.eventsHandled;
    total.tasksExecuted += base.tasksExecuted;
    total.timersExpired += base.timersExpired;
    total.wakeups += base.wakeups;
    // 各从 Reactor
    if (m_threadPool) {
        for (EventLoop* loop : m_threadPool->allLoops()) {
            if (loop == m_loop) {
                continue;  // 单线程模式下 allLoops 返回的就是主 loop，避免重复计入
            }
            const auto s = loop->stats();
            total.loopIterations += s.loopIterations;
            total.eventsHandled += s.eventsHandled;
            total.tasksExecuted += s.tasksExecuted;
            total.timersExpired += s.timersExpired;
            total.wakeups += s.wakeups;
        }
    }
    return total;
}

TcpServer::Stats TcpServer::stats() const {
    std::lock_guard<std::mutex> lk(m_statsMutex);
    return m_stats;
}

}  // namespace thgh
