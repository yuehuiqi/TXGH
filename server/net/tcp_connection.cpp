#include "net/tcp_connection.h"

#include "net/event_loop.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>

namespace thgh {

TcpConnection::TcpConnection(EventLoop* loop, std::string name, int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr)
    : m_loop(loop),
      m_name(std::move(name)),
      m_socket(sockfd),
      m_channel(loop, sockfd),
      m_localAddr(localAddr),
      m_peerAddr(peerAddr),
      m_lastActive(TimerQueue::Clock::now()) {
    m_channel.setReadCallback([this] { handleRead(); });
    m_channel.setWriteCallback([this] { handleWrite(); });
    m_channel.setCloseCallback([this] { handleClose(); });
    m_channel.setErrorCallback([this] { handleError(); });

    // 本协议全是小 JSON 消息（心跳、ack、progress），
    // Nagle 攒批毫无收益却平白增加最多一个 RTT 的延迟，直接关掉
    m_socket.setTcpNoDelay(true);
    m_socket.setKeepAlive(true);
}

TcpConnection::~TcpConnection() = default;

void TcpConnection::connectEstablished() {
    m_loop->assertInLoopThread();
    m_state = State::Connected;
    // 把 Channel 的生命周期绑到本对象上：事件处理期间本对象不会被销毁
    m_channel.tie(shared_from_this());
    m_channel.enableReading();
    if (m_connectionCallback) {
        m_connectionCallback(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    m_loop->assertInLoopThread();
    if (m_state == State::Connected || m_state == State::Disconnecting) {
        m_state = State::Disconnected;
        m_channel.disableAll();
    }
    m_loop->removeChannel(&m_channel);
}

void TcpConnection::setTcpNoDelay(bool on) { m_socket.setTcpNoDelay(on); }

// ── 读 ──────────────────────────────────────────────────────────────────────

void TcpConnection::handleRead() {
    m_loop->assertInLoopThread();

    // ET 模式：必须循环读到 EAGAIN。
    // 只读一次的话，内核缓冲里剩下的数据不会再次触发通知，连接就"卡死"了。
    for (;;) {
        int savedErrno = 0;
        const ssize_t n = m_inputBuffer.readFd(m_socket.fd(), &savedErrno);

        if (n > 0) {
            m_lastActive = TimerQueue::Clock::now();

            // 把新读到的字节喂给分帧器，取出所有完整消息。
            // 分帧器与客户端共用同一份实现，行为严格一致。
            m_framer.append(m_inputBuffer.peek(), m_inputBuffer.readableBytes());
            m_inputBuffer.retrieveAll();

            std::string message;
            while (m_framer.next(message)) {
                ++m_messagesReceived;
                if (m_messageCallback) {
                    m_messageCallback(shared_from_this(), message);
                }
            }
            continue;
        }

        if (n == 0) {
            // 对端关闭
            handleClose();
            return;
        }

        // n < 0
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            break;  // 读干净了，正常退出
        }
        if (savedErrno == EINTR) {
            continue;
        }
        errno = savedErrno;
        std::fprintf(stderr, "[TcpConnection %s] read 出错: %s\n",
                     m_name.c_str(), ::strerror(savedErrno));
        handleError();
        return;
    }
}

// ── 写 ──────────────────────────────────────────────────────────────────────

void TcpConnection::send(const std::string& message) {
    std::string framed;
    if (!LineFramer::encode(message, framed)) {
        std::fprintf(stderr,
                     "[TcpConnection %s] 消息含分隔符，拒绝发送（会破坏分帧）\n",
                     m_name.c_str());
        return;
    }
    sendRaw(framed);
}

void TcpConnection::sendRaw(const std::string& data) {
    if (m_state != State::Connected) {
        return;
    }
    if (m_loop->isInLoopThread()) {
        sendInLoop(data);
    } else {
        // 跨线程发送：投递到连接所属的 loop 线程执行。
        // 这样连接的缓冲区始终只被一个线程访问，不需要加锁。
        m_loop->runInLoop(
            [self = shared_from_this(), data] { self->sendInLoop(data); });
    }
}

void TcpConnection::sendInLoop(const std::string& data) {
    m_loop->assertInLoopThread();
    if (m_state != State::Connected) {
        return;
    }

    ssize_t written = 0;
    std::size_t remaining = data.size();
    bool faultError = false;

    // 输出缓冲为空时先尝试直接写。多数情况一次就能写完，
    // 这样可以省掉一次拷贝到缓冲区、以及一轮 EPOLLOUT 事件。
    if (!m_channel.isWriting() && m_outputBuffer.readableBytes() == 0) {
        written = ::write(m_socket.fd(), data.data(), data.size());
        if (written >= 0) {
            m_bytesSent += static_cast<std::uint64_t>(written);
            remaining = data.size() - static_cast<std::size_t>(written);
            if (remaining == 0 && m_writeCompleteCallback) {
                m_loop->queueInLoop(
                    [self = shared_from_this()] {
                        self->m_writeCompleteCallback(self);
                    });
            }
        } else {
            written = 0;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::fprintf(stderr, "[TcpConnection %s] write 出错: %s\n",
                             m_name.c_str(), ::strerror(errno));
                if (errno == EPIPE || errno == ECONNRESET) {
                    faultError = true;
                }
            }
        }
    }

    if (faultError) {
        return;
    }

    if (remaining > 0) {
        // 内核发送缓冲满了，剩余数据存进应用层缓冲并关注 EPOLLOUT。
        // **绝不能在这里阻塞等待**——那会让整个 EventLoop 被这一条连接卡住，
        // 该线程上的其它连接全部饿死。
        m_outputBuffer.append(data.data() + written, remaining);
        if (!m_channel.isWriting()) {
            m_channel.enableWriting();
        }
    }
}

void TcpConnection::handleWrite() {
    m_loop->assertInLoopThread();
    if (!m_channel.isWriting()) {
        return;
    }

    // ET 模式下同样要循环写到 EAGAIN 或写完
    for (;;) {
        const std::size_t readable = m_outputBuffer.readableBytes();
        if (readable == 0) {
            break;
        }
        const ssize_t n = ::write(m_socket.fd(), m_outputBuffer.peek(), readable);
        if (n > 0) {
            m_outputBuffer.retrieve(static_cast<std::size_t>(n));
            m_bytesSent += static_cast<std::uint64_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // 内核缓冲又满了，等下次 EPOLLOUT
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        std::fprintf(stderr, "[TcpConnection %s] handleWrite 出错: %s\n",
                     m_name.c_str(), ::strerror(errno));
        break;
    }

    if (m_outputBuffer.readableBytes() == 0) {
        // 写完了就取消关注 EPOLLOUT。
        // 不取消的话，只要内核缓冲有空间就会持续触发，事件循环空转烧 CPU。
        m_channel.disableWriting();
        if (m_writeCompleteCallback) {
            m_loop->queueInLoop([self = shared_from_this()] {
                self->m_writeCompleteCallback(self);
            });
        }
        // 之前请求过优雅关闭，现在数据发完了，可以真正关写端
        if (m_state == State::Disconnecting) {
            shutdownInLoop();
        }
    }
}

// ── 关闭 ────────────────────────────────────────────────────────────────────

void TcpConnection::shutdown() {
    if (m_state == State::Connected) {
        m_state = State::Disconnecting;
        m_loop->runInLoop([self = shared_from_this()] { self->shutdownInLoop(); });
    }
}

void TcpConnection::shutdownInLoop() {
    m_loop->assertInLoopThread();
    // 还有数据没发完就先不关：等 handleWrite 把缓冲排空后再调回这里。
    // 直接 shutdown 会丢掉缓冲区里未发送的数据。
    if (!m_channel.isWriting()) {
        if (::shutdown(m_socket.fd(), SHUT_WR) < 0) {
            std::fprintf(stderr, "[TcpConnection %s] shutdown(SHUT_WR) 失败: %s\n",
                         m_name.c_str(), ::strerror(errno));
        }
    }
}

void TcpConnection::forceClose() {
    if (m_state == State::Connected || m_state == State::Disconnecting) {
        m_state = State::Disconnecting;
        m_loop->queueInLoop(
            [self = shared_from_this()] { self->forceCloseInLoop(); });
    }
}

void TcpConnection::forceCloseInLoop() {
    m_loop->assertInLoopThread();
    if (m_state == State::Connected || m_state == State::Disconnecting) {
        handleClose();
    }
}

void TcpConnection::handleClose() {
    m_loop->assertInLoopThread();
    if (m_state == State::Disconnected) {
        return;
    }
    m_state = State::Disconnected;
    m_channel.disableAll();

    // 先自持一份 shared_ptr：回调里通常会把连接从 TcpServer 的表里移除，
    // 那可能是最后一个引用。不自持的话本函数返回前对象就被销毁了。
    const TcpConnectionPtr guard = shared_from_this();
    if (m_connectionCallback) {
        m_connectionCallback(guard);
    }
    if (m_closeCallback) {
        m_closeCallback(guard);
    }
}

void TcpConnection::handleError() {
    const int err = m_socket.takeError();
    std::fprintf(stderr, "[TcpConnection %s] socket 错误: %s\n", m_name.c_str(),
                 ::strerror(err));
    handleClose();
}

}  // namespace thgh
