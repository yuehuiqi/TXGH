#include "net/acceptor.h"

#include "net/event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <cstdio>

namespace thgh {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr,
                   bool reusePort)
    : m_loop(loop),
      m_acceptSocket(sockets::createNonblockingTcp()),
      m_acceptChannel(loop, m_acceptSocket.fd()),
      m_listenAddr(listenAddr),
      m_idleFd(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
    if (!m_acceptSocket.valid()) {
        std::fprintf(stderr, "[Acceptor] 创建监听 socket 失败: %s\n",
                     ::strerror(errno));
        return;
    }
    m_acceptSocket.setReuseAddr(true);
    if (reusePort) {
        m_acceptSocket.setReusePort(true);
    }
    m_acceptChannel.setReadCallback([this] { handleRead(); });
}

Acceptor::~Acceptor() {
    m_acceptChannel.disableAll();
    m_loop->removeChannel(&m_acceptChannel);
    if (m_idleFd >= 0) {
        ::close(m_idleFd);
    }
}

bool Acceptor::listen() {
    m_loop->assertInLoopThread();

    if (!m_acceptSocket.bindAddress(m_listenAddr)) {
        std::fprintf(stderr, "[Acceptor] bind %s 失败: %s\n",
                     m_listenAddr.toIpPort().c_str(), ::strerror(errno));
        return false;
    }
    if (!m_acceptSocket.listen()) {
        std::fprintf(stderr, "[Acceptor] listen 失败: %s\n", ::strerror(errno));
        return false;
    }
    m_listening = true;
    m_acceptChannel.enableReading();
    return true;
}

void Acceptor::handleRead() {
    m_loop->assertInLoopThread();

    // ET 模式下**必须循环 accept 到 EAGAIN**。
    // 若一次事件里到达了多条连接却只 accept 一条，剩下的不会再触发通知，
    // 那些客户端会一直卡在"已连上但服务端无响应"的状态。
    for (;;) {
        InetAddress peer;
        int savedErrno = 0;
        const int connfd = m_acceptSocket.accept(&peer, &savedErrno);

        if (connfd >= 0) {
            if (m_newConnectionCallback) {
                m_newConnectionCallback(connfd, peer);
            } else {
                ::close(connfd);
            }
            continue;
        }

        // 已排空
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            break;
        }

        // 连接在 accept 之前就被对端重置了。不是服务端的错误，跳过继续。
        if (savedErrno == ECONNABORTED || savedErrno == EPROTO ||
            savedErrno == EINTR) {
            continue;
        }

        // fd 耗尽：用预留的 idleFd 腾名额，接出来立刻关掉。
        // 这样做的意义是**给客户端一个明确的拒绝**，而不是让它挂在
        // 内核队列里干等；同时防止已完成队列被占满影响其它连接。
        if (savedErrno == EMFILE || savedErrno == ENFILE) {
            std::fprintf(stderr,
                         "[Acceptor] fd 耗尽(%s)，用预留 fd 腾名额后拒绝连接\n",
                         ::strerror(savedErrno));
            if (m_idleFd >= 0) {
                ::close(m_idleFd);
                m_idleFd = ::accept(m_acceptSocket.fd(), nullptr, nullptr);
                if (m_idleFd >= 0) {
                    ::close(m_idleFd);
                }
                m_idleFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            }
            break;
        }

        std::fprintf(stderr, "[Acceptor] accept 失败: %s\n",
                     ::strerror(savedErrno));
        break;
    }
}

}  // namespace thgh
