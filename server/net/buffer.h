#ifndef THGH_SERVER_NET_BUFFER_H
#define THGH_SERVER_NET_BUFFER_H

// ─────────────────────────────────────────────────────────────────────────────
// Buffer —— 连接的读写缓冲区
//
// ── 为什么非阻塞 IO 必须有应用层缓冲 ──────────────────────────────────────
//
// **读**：ET 模式下必须循环 read 到 EAGAIN，否则剩余数据不会再次触发通知。
//   一次事件里可能读出好几个 TCP 段，凑不成完整消息的部分要存起来等下次。
//
// **写**：write 可能只写进去一部分（内核发送缓冲满了）。这时**绝不能阻塞等待**，
//   否则整个 EventLoop 就被这一条连接卡住、其他连接全部饿死。
//   正确做法是把没写完的存进应用层缓冲，注册 EPOLLOUT，
//   等内核缓冲有空间了再继续写完。
//
// ── 内部布局 ──────────────────────────────────────────────────────────────
//
//   +-------------------+------------------+------------------+
//   |   已读走(可回收)   |     待处理数据     |     可写空间      |
//   +-------------------+------------------+------------------+
//   0      <=       readerIndex   <=   writerIndex    <=     size
//
// 用两个游标而不是每次 memmove：读走数据只推进 readerIndex，
// 空间在真正需要时才整理。这和 P1 的 LineFramer 是同一个思路 ——
// 那边修掉的正是"每取一条消息就搬一次剩余数据"的 O(n²)。
//
// 线程安全：**无**。每条连接的缓冲只被它所属的那个 EventLoop 线程访问。
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace thgh {

class Buffer {
public:
    // 前置预留空间：用于极少数需要在数据头部插入的场景（如长度前缀协议
    // 事后回填长度）。本协议是行分隔不需要，但保留这个设计不增加成本，
    // 且让 prepend 成为 O(1) 操作。
    static constexpr std::size_t kPrepend = 8;
    static constexpr std::size_t kInitialSize = 1024;

    explicit Buffer(std::size_t initialSize = kInitialSize)
        : m_data(kPrepend + initialSize),
          m_readerIndex(kPrepend),
          m_writerIndex(kPrepend) {}

    // ── 查询 ────────────────────────────────────────────────────────────
    std::size_t readableBytes() const { return m_writerIndex - m_readerIndex; }
    std::size_t writableBytes() const { return m_data.size() - m_writerIndex; }
    std::size_t prependableBytes() const { return m_readerIndex; }

    const char* peek() const { return begin() + m_readerIndex; }
    char* beginWrite() { return begin() + m_writerIndex; }
    const char* beginWrite() const { return begin() + m_writerIndex; }

    // ── 读取 ────────────────────────────────────────────────────────────
    void retrieve(std::size_t len) {
        if (len >= readableBytes()) {
            retrieveAll();
            return;
        }
        m_readerIndex += len;
    }

    void retrieveAll() {
        // 全部读完时把游标复位到起点，这是最常见的路径，O(1)，
        // 且能让后续写入总是从缓冲区头部开始、缓存局部性最好
        m_readerIndex = kPrepend;
        m_writerIndex = kPrepend;
    }

    std::string retrieveAsString(std::size_t len) {
        len = std::min(len, readableBytes());
        std::string s(peek(), len);
        retrieve(len);
        return s;
    }

    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes());
    }

    // ── 写入 ────────────────────────────────────────────────────────────
    void append(const char* data, std::size_t len) {
        if (len == 0 || data == nullptr) {
            return;
        }
        ensureWritable(len);
        std::memcpy(beginWrite(), data, len);
        m_writerIndex += len;
    }

    void append(const std::string& s) { append(s.data(), s.size()); }

    void ensureWritable(std::size_t len) {
        if (writableBytes() < len) {
            makeSpace(len);
        }
    }

    void hasWritten(std::size_t len) {
        // 由 readFd 在直接写入 beginWrite() 之后调用
        m_writerIndex += len;
    }

    // ── 与 fd 交互 ──────────────────────────────────────────────────────
    // 从 fd 读数据。返回实际读到的字节数；返回 -1 时 savedErrno 带出错误码。
    //
    // 用 readv + 栈上临时区的技巧：一次系统调用同时填"缓冲区剩余空间"和
    // 一块 64KB 的栈上空间。这样即使缓冲区当前很小，一次 read 也能收下
    // 大量数据，**减少系统调用次数**；同时缓冲区不必一开始就分配得很大，
    // 连接数多时能省下可观的内存（每条连接省几十 KB，上万连接就是几百 MB）。
    ssize_t readFd(int fd, int* savedErrno);

    // ── 调试 / 诊断 ─────────────────────────────────────────────────────
    std::size_t capacity() const { return m_data.size(); }
    void shrink(std::size_t reserve = kInitialSize);

private:
    char* begin() { return m_data.data(); }
    const char* begin() const { return m_data.data(); }

    void makeSpace(std::size_t len);

    std::vector<char> m_data;
    std::size_t m_readerIndex;
    std::size_t m_writerIndex;
};

}  // namespace thgh

#endif  // THGH_SERVER_NET_BUFFER_H
