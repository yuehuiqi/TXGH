// Buffer 单元测试
//
// 重点：读写游标的正确性、空间回收策略（避免长连接下缓冲区无限膨胀）、
// 以及 readFd 的 readv 双缓冲行为。

#include "net/buffer.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <vector>

using thgh::Buffer;

namespace {

// 建一对管道，用于测试 readFd
struct Pipe {
    int fds[2] = {-1, -1};
    Pipe() { EXPECT_EQ(::pipe(fds), 0); }
    ~Pipe() {
        if (fds[0] >= 0) ::close(fds[0]);
        if (fds[1] >= 0) ::close(fds[1]);
    }
    void writeAll(const std::string& s) {
        ssize_t n = ::write(fds[1], s.data(), s.size());
        EXPECT_EQ(n, static_cast<ssize_t>(s.size()));
    }
    void closeWrite() {
        if (fds[1] >= 0) { ::close(fds[1]); fds[1] = -1; }
    }
};

}  // namespace

// ── 基本读写 ────────────────────────────────────────────────────────────────

TEST(Buffer, StartsEmpty) {
    Buffer b;
    EXPECT_EQ(b.readableBytes(), 0u);
    EXPECT_EQ(b.prependableBytes(), Buffer::kPrepend);
    EXPECT_GT(b.writableBytes(), 0u);
}

TEST(Buffer, AppendThenRetrieve) {
    Buffer b;
    b.append("hello");
    EXPECT_EQ(b.readableBytes(), 5u);
    EXPECT_EQ(std::string(b.peek(), 5), "hello");

    EXPECT_EQ(b.retrieveAsString(5), "hello");
    EXPECT_EQ(b.readableBytes(), 0u);
}

TEST(Buffer, PartialRetrieveAdvancesCursor) {
    Buffer b;
    b.append("hello world");
    EXPECT_EQ(b.retrieveAsString(6), "hello ");
    EXPECT_EQ(b.readableBytes(), 5u);
    EXPECT_EQ(b.retrieveAllAsString(), "world");
}

TEST(Buffer, RetrieveMoreThanAvailableIsClamped) {
    Buffer b;
    b.append("abc");
    b.retrieve(100);  // 不应越界
    EXPECT_EQ(b.readableBytes(), 0u);
}

TEST(Buffer, AppendNullOrZeroLengthIsSafe) {
    Buffer b;
    b.append(nullptr, 5);
    b.append("x", 0);
    EXPECT_EQ(b.readableBytes(), 0u);
}

TEST(Buffer, MultipleAppendsConcatenate) {
    Buffer b;
    b.append("aaa");
    b.append("bbb");
    b.append("ccc");
    EXPECT_EQ(b.retrieveAllAsString(), "aaabbbccc");
}

TEST(Buffer, HandlesBinaryDataWithNulls) {
    // 缓冲区是字节容器，不能按 C 字符串处理
    Buffer b;
    const std::string data("a\0b\0c", 5);
    b.append(data);
    EXPECT_EQ(b.readableBytes(), 5u);
    EXPECT_EQ(b.retrieveAllAsString(), data);
}

// ── 空间管理 ★ ─────────────────────────────────────────────────────────────

TEST(Buffer, RetrieveAllResetsCursorsToStart) {
    // 全部读完时游标复位，让后续写入总是从头开始，缓存局部性最好
    Buffer b;
    b.append(std::string(500, 'x'));
    b.retrieveAll();
    EXPECT_EQ(b.prependableBytes(), Buffer::kPrepend);
    EXPECT_EQ(b.readableBytes(), 0u);
}

TEST(Buffer, ReusesFrontSpaceInsteadOfGrowingForever) {
    // ★ 长连接的关键场景：持续小块收发，绝不能让缓冲区随时间无限膨胀。
    //   读走的数据在前部留下空洞，应该被回收复用而不是一路扩容。
    Buffer b(1024);
    const std::size_t initialCap = b.capacity();

    for (int i = 0; i < 10'000; ++i) {
        b.append(std::string(100, 'x'));
        EXPECT_EQ(b.retrieveAsString(100).size(), 100u);
    }

    EXPECT_EQ(b.capacity(), initialCap)
        << "缓冲区从 " << initialCap << " 膨胀到 " << b.capacity();
}

TEST(Buffer, GrowsWhenDataGenuinelyExceedsCapacity) {
    // 真的装不下时该扩就扩
    Buffer b(64);
    b.append(std::string(10'000, 'y'));
    EXPECT_GE(b.capacity(), 10'000u);
    EXPECT_EQ(b.readableBytes(), 10'000u);
}

TEST(Buffer, CompactionPreservesUnreadData) {
    // 回收前部空间时，未读数据必须原样保留且顺序不变
    Buffer b(128);
    b.append(std::string(100, 'a'));
    b.retrieve(90);                    // 前部留下 90 字节空洞
    b.append(std::string(100, 'b'));   // 触发整理

    const std::string s = b.retrieveAllAsString();
    EXPECT_EQ(s, std::string(10, 'a') + std::string(100, 'b'));
}

TEST(Buffer, ShrinkReleasesMemoryButKeepsData) {
    Buffer b(1024);
    b.append(std::string(100'000, 'z'));
    EXPECT_GE(b.capacity(), 100'000u);

    const std::string kept = b.retrieveAsString(50);
    b.shrink(1024);

    EXPECT_LT(b.capacity(), 100'000u);
    EXPECT_EQ(b.readableBytes(), 100'000u - 50);
    EXPECT_EQ(kept, std::string(50, 'z'));
}

TEST(Buffer, EnsureWritableGuaranteesSpace) {
    Buffer b(16);
    b.ensureWritable(5000);
    EXPECT_GE(b.writableBytes(), 5000u);
}

// ── readFd ──────────────────────────────────────────────────────────────────

TEST(Buffer, ReadFdReadsAvailableData) {
    Pipe p;
    p.writeAll("hello from pipe");

    Buffer b;
    int err = 0;
    const ssize_t n = b.readFd(p.fds[0], &err);

    EXPECT_EQ(n, 15);
    EXPECT_EQ(b.retrieveAllAsString(), "hello from pipe");
}

TEST(Buffer, ReadFdHandlesDataLargerThanBuffer) {
    // ★ readv 双缓冲的意义：即使缓冲区很小，一次系统调用也能收下大量数据。
    //   这样缓冲区不必一开始就分配得很大 —— 上万连接时能省下可观内存。
    Pipe p;
    // 管道容量通常 64KB，写满一点但别阻塞
    const std::string payload(60'000, 'q');
    p.writeAll(payload);

    Buffer b(64);   // 故意只给 64 字节初始容量
    int err = 0;
    const ssize_t n = b.readFd(p.fds[0], &err);

    EXPECT_EQ(n, static_cast<ssize_t>(payload.size()));
    EXPECT_EQ(b.readableBytes(), payload.size());
    EXPECT_EQ(b.retrieveAllAsString(), payload);
}

TEST(Buffer, ReadFdReturnsZeroOnEof) {
    // 对端关闭写端 → read 返回 0，服务端据此判定连接关闭
    Pipe p;
    p.closeWrite();

    Buffer b;
    int err = 0;
    EXPECT_EQ(b.readFd(p.fds[0], &err), 0);
}

TEST(Buffer, ReadFdReportsErrnoOnFailure) {
    Buffer b;
    int err = 0;
    // 无效 fd
    EXPECT_EQ(b.readFd(-1, &err), -1);
    EXPECT_EQ(err, EBADF);
}

TEST(Buffer, ReadFdOnNonBlockingEmptyFdReturnsEagain) {
    // ET 模式下循环读到 EAGAIN 才算读完，这是必经路径
    Pipe p;
    ::fcntl(p.fds[0], F_SETFL, O_NONBLOCK);

    Buffer b;
    int err = 0;
    EXPECT_EQ(b.readFd(p.fds[0], &err), -1);
    EXPECT_TRUE(err == EAGAIN || err == EWOULDBLOCK);
}

TEST(Buffer, MultipleReadFdCallsAccumulate) {
    Pipe p;
    Buffer b;
    int err = 0;

    p.writeAll("part1-");
    b.readFd(p.fds[0], &err);
    p.writeAll("part2");
    b.readFd(p.fds[0], &err);

    EXPECT_EQ(b.retrieveAllAsString(), "part1-part2");
}
