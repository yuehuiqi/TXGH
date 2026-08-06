#include "net/buffer.h"

#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

namespace thgh {

void Buffer::makeSpace(std::size_t len) {
    // 关键判断：先看**回收前部已读空间**够不够，不够才真正扩容。
    //
    // 长连接持续收发时，readerIndex 会不断向后推进，前面积累出大片
    // 已读走的空洞。整理一次就能腾出空间，避免缓冲区随连接存活时间无限膨胀。
    if (writableBytes() + prependableBytes() < len + kPrepend) {
        // 空间确实不够，扩容
        m_data.resize(m_writerIndex + len);
    } else {
        // 空间够，把待处理数据搬到前面复用已读空间
        const std::size_t readable = readableBytes();
        std::memmove(begin() + kPrepend, begin() + m_readerIndex, readable);
        m_readerIndex = kPrepend;
        m_writerIndex = m_readerIndex + readable;
    }
}

void Buffer::shrink(std::size_t reserve) {
    const std::size_t readable = readableBytes();
    std::vector<char> other(kPrepend + std::max(readable, reserve));
    if (readable > 0) {
        std::memcpy(other.data() + kPrepend, peek(), readable);
    }
    m_data.swap(other);
    m_readerIndex = kPrepend;
    m_writerIndex = m_readerIndex + readable;
}

ssize_t Buffer::readFd(int fd, int* savedErrno) {
    // 栈上临时区。放栈上是有意的：它随函数返回自动释放，
    // 不占用每条连接的常驻内存 —— 上万连接时这个差别非常可观。
    char extrabuf[65536];

    struct iovec vec[2];
    const std::size_t writable = writableBytes();
    vec[0].iov_base = beginWrite();
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    // 缓冲区本身够大时就不必带上临时区，少一次内核的 iovec 处理。
    // 阈值取 extrabuf 大小：缓冲区已有 64KB 以上可写空间时单独用它就够了。
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<std::size_t>(n) <= writable) {
        // 全部落进了缓冲区
        m_writerIndex += static_cast<std::size_t>(n);
    } else {
        // 缓冲区被填满，剩下的在临时区里，追加进来（这一步会触发扩容）
        m_writerIndex = m_data.size();
        append(extrabuf, static_cast<std::size_t>(n) - writable);
    }
    return n;
}

}  // namespace thgh
