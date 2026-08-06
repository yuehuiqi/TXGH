#ifndef THGH_SERVER_NET_ACCEPTOR_H
#define THGH_SERVER_NET_ACCEPTOR_H

// ─────────────────────────────────────────────────────────────────────────────
// Acceptor —— 主 Reactor 的连接接入器
//
// 只做一件事：在**主线程**的 EventLoop 里监听 listen fd，接受新连接后
// 把 fd 交给 TcpServer 分发给某个从 Reactor。它自己不碰任何连接数据。
//
// ── 主从 Reactor vs 单 Reactor 多线程 ─────────────────────────────────────
//
//   单 Reactor 多线程：一个事件循环负责 accept 和所有连接的 IO，
//     业务处理丢给线程池。问题是**事件分发本身是单点**——
//     连接数上去后 accept 和大量 IO 事件挤在同一个线程里，它先成为瓶颈。
//
//   主从 Reactor：主 Reactor 只负责 accept，N 个从 Reactor 各自持有独立的
//     epoll 实例处理分给自己的连接。好处：
//       * 每个 epoll 实例的 fd 集合更小，内核红黑树操作更快
//       * 各线程操作各自的 epoll，**没有共享 epoll 的锁竞争**
//       * accept 不会被 IO 处理拖慢，建连延迟稳定
//     这是 muduo 等主流库的做法。
//
// ── 惊群问题 ──────────────────────────────────────────────────────────────
// 若多个线程同时监听同一个 listen fd，新连接到来时内核可能唤醒所有等待者，
// 但只有一个能真正 accept 成功，其余白白被唤醒又睡回去，浪费 CPU。
// 规避方式有 EPOLLEXCLUSIVE、SO_REUSEPORT，或者最简单的——
// **让 accept 只发生在一个线程里**，也就是本类的做法。
// ─────────────────────────────────────────────────────────────────────────────

#include "net/channel.h"
#include "net/socket.h"

#include <functional>
#include <memory>

namespace thgh {

class EventLoop;

class Acceptor {
public:
    // 新连接回调：参数是已就绪的连接 fd 与对端地址
    using NewConnectionCallback =
        std::function<void(int sockfd, const InetAddress& peer)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void setNewConnectionCallback(NewConnectionCallback cb) {
        m_newConnectionCallback = std::move(cb);
    }

    bool listening() const { return m_listening; }
    // 开始监听。返回 false 表示 bind/listen 失败（端口被占用等）。
    bool listen();

    const InetAddress& listenAddress() const { return m_listenAddr; }

private:
    void handleRead();

    EventLoop* m_loop;
    Socket m_acceptSocket;
    Channel m_acceptChannel;
    InetAddress m_listenAddr;
    NewConnectionCallback m_newConnectionCallback;
    bool m_listening = false;

    // ── fd 耗尽的兜底 ───────────────────────────────────────────────────
    // accept 返回 EMFILE（进程 fd 用尽）时，连接仍然挂在内核的已完成队列里。
    // ET 模式下这次事件已经消费掉了，若什么都不做，那条连接会一直躺在队列里
    // 不再触发通知，客户端表现为"连上了但永远没响应"，而且队列会被逐渐占满。
    //
    // 经典解法：**预先占住一个空闲 fd**。遇到 EMFILE 时先 close 它腾出名额，
    // 用这个名额 accept 出连接再立刻 close 掉（明确拒绝而不是让客户端干等），
    // 最后重新占回那个 fd 备用。
    int m_idleFd = -1;
};

}  // namespace thgh

#endif  // THGH_SERVER_NET_ACCEPTOR_H
