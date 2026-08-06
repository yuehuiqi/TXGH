#ifndef THGH_SERVER_NET_TCP_CONNECTION_H
#define THGH_SERVER_NET_TCP_CONNECTION_H

// ─────────────────────────────────────────────────────────────────────────────
// TcpConnection —— 一条已建立的连接
//
// 持有连接 fd、读写缓冲、协议分帧器，负责：
//   * ET 模式下循环读到 EAGAIN
//   * 写不完时挂到应用层缓冲 + 注册 EPOLLOUT，绝不阻塞事件循环
//   * 心跳超时判定所需的"最后活跃时间"
//
// ── 为什么用 shared_ptr 管理 ─────────────────────────────────────────────
// 连接的销毁时机很微妙：可能在 handleEvent 执行到一半时，回调里决定关闭它。
// 若此时直接 delete，回调返回后 EventLoop 继续访问 Channel 就是 use-after-free。
// 用 shared_ptr + Channel::tie，让对象在事件处理期间被临时续命，
// 真正的释放推迟到所有引用消失之后。
//
// 线程归属：一条连接只属于一个 EventLoop（从 Reactor）线程，
// 所有读写与状态变更都在该线程内完成，因此内部无锁。
// 跨线程发送数据要经 send()，它内部会 runInLoop 投递。
// ─────────────────────────────────────────────────────────────────────────────

#include "net/buffer.h"
#include "net/channel.h"
#include "net/socket.h"
#include "net/timer_queue.h"
#include "thgh/line_framer.h"

#include <atomic>
#include <memory>
#include <string>

namespace thgh {

class EventLoop;
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    // 收到一条**完整协议消息**时回调（分帧已由 LineFramer 完成）
    using MessageCallback =
        std::function<void(const TcpConnectionPtr&, const std::string&)>;
    using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
    using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;

    TcpConnection(EventLoop* loop, std::string name, int sockfd,
                  const InetAddress& localAddr, const InetAddress& peerAddr);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    EventLoop* getLoop() const { return m_loop; }
    const std::string& name() const { return m_name; }
    const InetAddress& localAddress() const { return m_localAddr; }
    const InetAddress& peerAddress() const { return m_peerAddr; }
    bool connected() const { return m_state == State::Connected; }

    // ── 发送 ────────────────────────────────────────────────────────────
    // 发送一条协议消息（内部自动追加分隔符）。
    // 可从任意线程调用：不在本 loop 线程时会投递过去执行。
    void send(const std::string& message);
    // 发送裸字节，不做协议封装
    void sendRaw(const std::string& data);

    // 优雅关闭：先关本端写方向（发 FIN），等对端读完剩余数据并关闭后
    // 再真正销毁。直接 close 会丢掉发送缓冲里还没发出去的数据。
    void shutdown();
    // 立即断开，不等待缓冲区排空
    void forceClose();

    void setTcpNoDelay(bool on);

    // ── 回调注册（由 TcpServer 设置）────────────────────────────────────
    void setConnectionCallback(ConnectionCallback cb) {
        m_connectionCallback = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb) {
        m_messageCallback = std::move(cb);
    }
    void setCloseCallback(CloseCallback cb) { m_closeCallback = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) {
        m_writeCompleteCallback = std::move(cb);
    }

    // ── 生命周期（由 TcpServer 调用）────────────────────────────────────
    void connectEstablished();
    void connectDestroyed();

    // ── 心跳 ────────────────────────────────────────────────────────────
    // 最后一次收到数据的时刻，供 TcpServer 的心跳巡检判定超时
    TimerQueue::TimePoint lastActiveTime() const { return m_lastActive; }

    // ── 诊断 ────────────────────────────────────────────────────────────
    std::size_t outputBufferSize() const { return m_outputBuffer.readableBytes(); }
    std::uint64_t messagesReceived() const { return m_messagesReceived; }
    std::uint64_t bytesSent() const { return m_bytesSent; }

private:
    enum class State { Connecting, Connected, Disconnecting, Disconnected };

    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    void sendInLoop(const std::string& data);
    void shutdownInLoop();
    void forceCloseInLoop();

    EventLoop* m_loop;
    std::string m_name;
    std::atomic<State> m_state{State::Connecting};

    Socket m_socket;
    Channel m_channel;
    InetAddress m_localAddr;
    InetAddress m_peerAddr;

    Buffer m_inputBuffer;
    Buffer m_outputBuffer;
    LineFramer m_framer;  // 协议分帧，与客户端共用同一份实现

    ConnectionCallback m_connectionCallback;
    MessageCallback m_messageCallback;
    CloseCallback m_closeCallback;
    WriteCompleteCallback m_writeCompleteCallback;

    TimerQueue::TimePoint m_lastActive;
    std::uint64_t m_messagesReceived = 0;
    std::uint64_t m_bytesSent = 0;
};

}  // namespace thgh

#endif  // THGH_SERVER_NET_TCP_CONNECTION_H
