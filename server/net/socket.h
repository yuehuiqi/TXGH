#ifndef THGH_SERVER_NET_SOCKET_H
#define THGH_SERVER_NET_SOCKET_H

// ─────────────────────────────────────────────────────────────────────────────
// Socket —— 文件描述符的 RAII 封装 + 常用 socket 选项
//
// 为什么值得单独封一层：fd 是需要显式 close 的资源，一旦某条错误路径漏了 close
// 就是 fd 泄漏。服务端跑久了 fd 耗尽会导致 accept 失败、新连接全部被拒 ——
// 这类问题在压测跑几小时后才暴露，极难排查。用 RAII 把它变成不可能。
// ─────────────────────────────────────────────────────────────────────────────

#include <netinet/in.h>

#include <string>

namespace thgh {

// ── IPv4 地址 ───────────────────────────────────────────────────────────────
class InetAddress {
public:
    InetAddress();
    explicit InetAddress(uint16_t port, const std::string& ip = "0.0.0.0");
    explicit InetAddress(const sockaddr_in& addr) : m_addr(addr) {}

    std::string ip() const;
    uint16_t port() const;
    std::string toIpPort() const;

    const sockaddr* sockAddr() const {
        return reinterpret_cast<const sockaddr*>(&m_addr);
    }
    sockaddr* sockAddr() { return reinterpret_cast<sockaddr*>(&m_addr); }
    socklen_t sockAddrLen() const { return sizeof(m_addr); }

private:
    sockaddr_in m_addr;
};

// ── fd 的 RAII 持有者 ───────────────────────────────────────────────────────
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : m_fd(fd) {}
    ~Socket();

    // 只可移动不可拷贝：fd 的所有权必须唯一，
    // 拷贝会导致同一个 fd 被 close 两次（第二次可能误关别人刚分配到的 fd，
    // 这是最难查的一类 bug）
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : m_fd(other.m_fd) { other.m_fd = -1; }
    Socket& operator=(Socket&& other) noexcept;

    int fd() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }

    // 交出所有权，调用方负责 close
    int release() {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }
    void reset(int fd = -1);

    // ── 服务端用 ────────────────────────────────────────────────────────
    bool bindAddress(const InetAddress& addr);
    bool listen(int backlog = 1024);
    // 接受一条连接。成功返回新 fd（已设为非阻塞 + close-on-exec），
    // 并把对端地址写入 peer；失败返回 -1，savedErrno 带出错误码。
    int accept(InetAddress* peer, int* savedErrno);

    // ── socket 选项 ─────────────────────────────────────────────────────
    // 允许 bind 处于 TIME_WAIT 的端口。没有它的话，服务端重启会因为上次连接
    // 残留的 TIME_WAIT 状态而 bind 失败（Address already in use），
    // 得等 2MSL（通常 60 秒）才能重启 —— 对开发和线上滚动发布都不可接受。
    bool setReuseAddr(bool on);
    // 允许多个进程/线程 bind 同一端口，内核做负载均衡。
    // 本项目主从 Reactor 只有一个 accept 线程，用不到，但保留接口。
    bool setReusePort(bool on);
    bool setKeepAlive(bool on);

    // 关闭 Nagle 算法。
    // Nagle 会把小包攒批发送以提高带宽利用率，代价是最多等一个 RTT。
    // 本协议的心跳、ack、progress 都是几十到几百字节的小 JSON，
    // 攒批毫无收益却平白增加延迟，所以必须关掉。
    // 成本就是一次 setsockopt，是典型的"一行代码换实打实的延迟改善"。
    bool setTcpNoDelay(bool on);

    // 关闭时是否等待未发送数据（SO_LINGER）
    bool setLinger(bool on, int seconds);

    // 取出并清除 socket 上的挂起错误（SO_ERROR）。
    // 非阻塞 connect 完成、以及 EPOLLERR 时用它拿真正的错误码。
    int takeError();

private:
    int m_fd = -1;
};

// ── 与具体 fd 无关的工具函数 ────────────────────────────────────────────────
namespace sockets {

// 创建非阻塞 + close-on-exec 的 TCP socket。
// **非阻塞是 epoll ET 模式的硬性前提**：ET 只在状态变化时通知一次，
// 必须循环读到 EAGAIN 才算读完；阻塞 fd 在读空之后会永久阻塞在 read 上，
// 整个 EventLoop 就死在这里了。
int createNonblockingTcp();

bool setNonBlocking(int fd);

// 本端 / 对端地址
InetAddress localAddress(int fd);
InetAddress peerAddress(int fd);

// 判断是否自连接：本端地址与对端地址完全相同。
// 客户端连本机时内核可能分配到与目标端口相同的源端口，形成自己连自己的
// 畸形连接，行为异常且难以察觉，检测到应直接关闭。
bool isSelfConnect(int fd);

}  // namespace sockets

}  // namespace thgh

#endif  // THGH_SERVER_NET_SOCKET_H
