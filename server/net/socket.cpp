#include "net/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>

namespace thgh {

// ── InetAddress ─────────────────────────────────────────────────────────────

InetAddress::InetAddress() {
    ::memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin_family = AF_INET;
}

InetAddress::InetAddress(uint16_t port, const std::string& ip) {
    ::memset(&m_addr, 0, sizeof(m_addr));
    m_addr.sin_family = AF_INET;
    m_addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &m_addr.sin_addr) != 1) {
        m_addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
    }
}

std::string InetAddress::ip() const {
    char buf[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &m_addr.sin_addr, buf, sizeof(buf));
    return buf;
}

uint16_t InetAddress::port() const { return ::ntohs(m_addr.sin_port); }

std::string InetAddress::toIpPort() const {
    return ip() + ":" + std::to_string(port());
}

// ── Socket ──────────────────────────────────────────────────────────────────

Socket::~Socket() { reset(); }

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        reset(other.m_fd);
        other.m_fd = -1;
    }
    return *this;
}

void Socket::reset(int fd) {
    if (m_fd >= 0 && m_fd != fd) {
        ::close(m_fd);
    }
    m_fd = fd;
}

bool Socket::bindAddress(const InetAddress& addr) {
    return ::bind(m_fd, addr.sockAddr(), addr.sockAddrLen()) == 0;
}

bool Socket::listen(int backlog) { return ::listen(m_fd, backlog) == 0; }

int Socket::accept(InetAddress* peer, int* savedErrno) {
    sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);

    // accept4 相比 accept + fcntl 的好处：一次系统调用同时完成接受连接和
    // 设置标志，且**没有竞态窗口** —— 用 accept 的话，从返回 fd 到 fcntl
    // 设置 CLOEXEC 之间若发生 fork/exec，fd 会泄漏到子进程。
    const int connfd = ::accept4(m_fd, reinterpret_cast<sockaddr*>(&addr), &len,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd < 0) {
        *savedErrno = errno;
        return -1;
    }
    if (peer != nullptr) {
        *peer = InetAddress(addr);
    }
    return connfd;
}

bool Socket::setReuseAddr(bool on) {
    const int optval = on ? 1 : 0;
    return ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
                        static_cast<socklen_t>(sizeof(optval))) == 0;
}

bool Socket::setReusePort(bool on) {
    const int optval = on ? 1 : 0;
    return ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEPORT, &optval,
                        static_cast<socklen_t>(sizeof(optval))) == 0;
}

bool Socket::setKeepAlive(bool on) {
    const int optval = on ? 1 : 0;
    return ::setsockopt(m_fd, SOL_SOCKET, SO_KEEPALIVE, &optval,
                        static_cast<socklen_t>(sizeof(optval))) == 0;
}

bool Socket::setTcpNoDelay(bool on) {
    const int optval = on ? 1 : 0;
    return ::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &optval,
                        static_cast<socklen_t>(sizeof(optval))) == 0;
}

bool Socket::setLinger(bool on, int seconds) {
    struct linger l;
    l.l_onoff = on ? 1 : 0;
    l.l_linger = seconds;
    return ::setsockopt(m_fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) == 0;
}

int Socket::takeError() {
    int optval = 0;
    socklen_t optlen = sizeof(optval);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
        return errno;
    }
    return optval;
}

// ── sockets 工具函数 ────────────────────────────────────────────────────────

namespace sockets {

int createNonblockingTcp() {
    // SOCK_NONBLOCK 直接在创建时指定，省掉一次 fcntl，也没有中间竞态窗口
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                            IPPROTO_TCP);
    return fd;
}

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

InetAddress localAddress(int fd) {
    sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return InetAddress(addr);
}

InetAddress peerAddress(int fd) {
    sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return InetAddress(addr);
}

bool isSelfConnect(int fd) {
    const InetAddress local = localAddress(fd);
    const InetAddress peer = peerAddress(fd);
    return local.port() == peer.port() && local.ip() == peer.ip();
}

}  // namespace sockets

}  // namespace thgh
