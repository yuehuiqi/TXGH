// LineFramer 单元测试
//
// 重点是 TCP 的两个固有问题：
//   * 半包：一条消息在**任意字节**处被切断
//   * 粘包：一次 read 拿到多条消息
// 这两类边界用真实网络极难稳定构造 —— 分帧层抽出来之后可以精确到字节地验证。
//
// 另外覆盖了原客户端实现缺失的长度上限保护（DoS 面）与重新同步。

#include "thgh/line_framer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using thgh::LineFramer;

namespace {

// 把 framer 里当前能取出的消息全部取出来
std::vector<std::string> drain(LineFramer& f) {
    std::vector<std::string> out;
    std::string line;
    while (f.next(line)) {
        out.push_back(line);
    }
    return out;
}

// 一次性喂入并取出
std::vector<std::string> feed(LineFramer& f, const std::string& bytes) {
    f.append(bytes);
    return drain(f);
}

}  // namespace

// ── 基本分帧 ────────────────────────────────────────────────────────────────

TEST(LineFramer, SplitsSingleMessage) {
    LineFramer f;
    EXPECT_EQ(feed(f, "hello\n"), (std::vector<std::string>{"hello"}));
    EXPECT_EQ(f.buffered(), 0u);
}

TEST(LineFramer, IncompleteMessageYieldsNothing) {
    LineFramer f;
    EXPECT_TRUE(feed(f, "hello").empty());
    EXPECT_EQ(f.buffered(), 5u);  // 字节留在缓冲里等后续
}

TEST(LineFramer, SkipsEmptyLines) {
    LineFramer f;
    EXPECT_EQ(feed(f, "\n\na\n\nb\n"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(f.stats().emptyLinesSkipped, 3u);
}

TEST(LineFramer, TrimsTrailingCarriageReturn) {
    // 对端是 Windows 工具或 telnet 手工调试时会带 \r
    LineFramer f;
    EXPECT_EQ(feed(f, "hello\r\n"), (std::vector<std::string>{"hello"}));
}

TEST(LineFramer, KeepsCarriageReturnInsideMessage) {
    // 只去行尾的 \r，消息中间的必须原样保留
    LineFramer f;
    EXPECT_EQ(feed(f, "a\rb\n"), (std::vector<std::string>{"a\rb"}));
}

TEST(LineFramer, CanDisableCrTrimming) {
    LineFramer::Options opt;
    opt.trimTrailingCr = false;
    LineFramer f(opt);
    EXPECT_EQ(feed(f, "hello\r\n"), (std::vector<std::string>{"hello\r"}));
}

// ── 粘包：一次读到多条 ──────────────────────────────────────────────────────

TEST(LineFramer, SplitsMultipleMessagesFromOneChunk) {
    LineFramer f;
    EXPECT_EQ(feed(f, "a\nbb\nccc\n"),
              (std::vector<std::string>{"a", "bb", "ccc"}));
    EXPECT_EQ(f.buffered(), 0u);
}

TEST(LineFramer, KeepsTrailingPartialAfterCompleteOnes) {
    // 两条完整 + 一条残缺：完整的取出，残缺的原样留着
    LineFramer f;
    EXPECT_EQ(feed(f, "a\nb\nccc"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(f.buffered(), 3u);

    // 后续字节到达后应能拼出第三条："ccc" + "cc" = "ccccc"
    EXPECT_EQ(feed(f, "cc\n"), (std::vector<std::string>{"ccccc"}));
    EXPECT_EQ(f.buffered(), 0u);
}

TEST(LineFramer, HandlesManyMessagesInOneChunk) {
    LineFramer f;
    std::string chunk;
    std::vector<std::string> expected;
    for (int i = 0; i < 1000; ++i) {
        const std::string msg = "msg-" + std::to_string(i);
        chunk += msg;
        chunk += '\n';
        expected.push_back(msg);
    }
    EXPECT_EQ(feed(f, chunk), expected);
    EXPECT_EQ(f.buffered(), 0u);
    // 一个 chunk 全部取完时走的是 O(1) 的 clear 路径，不该有任何压缩(memmove)
    EXPECT_EQ(f.stats().compactions, 0u);
}

// ── 半包：任意字节处切断 ★核心用例 ─────────────────────────────────────────

TEST(LineFramer, ReassemblesAcrossEveryPossibleSplitPoint) {
    // 对一批消息，在**每一个字节位置**切成两段分别喂入，
    // 结果都必须与一次性喂入完全一致。
    // 这是"半包安全"的完整定义，比抽查几个切点靠谱得多。
    const std::vector<std::string> payloads = {
        "a\nbb\nccc\n",
        "{\"type\":\"ack\"}\n{\"type\":\"progress\"}\n",
        "single\n",
        "x\n\ny\n",
    };

    for (const std::string& full : payloads) {
        LineFramer whole;
        const auto expected = feed(whole, full);

        for (std::size_t cut = 0; cut <= full.size(); ++cut) {
            LineFramer f;
            std::vector<std::string> got;

            f.append(full.data(), cut);
            for (auto& s : drain(f)) got.push_back(s);

            f.append(full.data() + cut, full.size() - cut);
            for (auto& s : drain(f)) got.push_back(s);

            EXPECT_EQ(got, expected)
                << "载荷 [" << full << "] 在第 " << cut << " 字节处切断";
        }
    }
}

TEST(LineFramer, ReassemblesWhenFedOneByteAtATime) {
    // 最极端的半包：每次只喂 1 个字节
    const std::string full = "{\"type\":\"plan_result\"}\n{\"type\":\"error\"}\n";
    LineFramer f;
    std::vector<std::string> got;
    std::string line;

    for (char c : full) {
        f.append(&c, 1);
        while (f.next(line)) {
            got.push_back(line);
        }
    }

    EXPECT_EQ(got, (std::vector<std::string>{R"({"type":"plan_result"})",
                                             R"({"type":"error"})"}));
    EXPECT_EQ(f.buffered(), 0u);
}

TEST(LineFramer, SplitPointInsideCrLfIsHandled) {
    // \r 和 \n 之间被切开，是个容易写错的边界
    LineFramer f;
    EXPECT_TRUE(feed(f, "hello\r").empty());
    EXPECT_EQ(feed(f, "\n"), (std::vector<std::string>{"hello"}));
}

// ── 长度上限与重新同步（原实现完全没有）────────────────────────────────────

TEST(LineFramer, DropsOversizedCompleteMessage) {
    LineFramer::Options opt;
    opt.maxLineBytes = 16;
    LineFramer f(opt);

    const std::string big(100, 'x');
    // 超长的那条被丢弃，但后面正常的消息不受影响
    EXPECT_EQ(feed(f, big + "\nok\n"), (std::vector<std::string>{"ok"}));
    EXPECT_EQ(f.stats().oversizedDropped, 1u);
}

TEST(LineFramer, BoundedMemoryWhenPeerNeverSendsDelimiter) {
    // ★ 这条钉的是原实现里真实存在的 DoS 面：
    //   对端只要一直发不含 '\n' 的字节，原实现的缓冲区就无限增长直到 OOM。
    LineFramer::Options opt;
    opt.maxLineBytes = 1024;
    LineFramer f(opt);

    const std::string junk(4096, 'x');
    for (int i = 0; i < 100; ++i) {  // 共喂 400KB 无分隔符垃圾
        f.append(junk);
        std::string line;
        EXPECT_FALSE(f.next(line));
        // 缓冲区必须被限制住，不能随投喂量线性增长
        EXPECT_LE(f.buffered(), opt.maxLineBytes + junk.size())
            << "第 " << i << " 轮，缓冲区 " << f.buffered() << " 字节";
    }
    EXPECT_TRUE(f.resyncing());
}

TEST(LineFramer, ResynchronizesAfterOversizedGarbage) {
    // 丢弃超长垃圾之后，遇到下一个分隔符要能恢复正常工作 ——
    // 一条坏消息不该废掉整条连接
    LineFramer::Options opt;
    opt.maxLineBytes = 64;
    LineFramer f(opt);

    f.append(std::string(500, 'x'));   // 超长垃圾，无分隔符
    std::string line;
    EXPECT_FALSE(f.next(line));
    EXPECT_TRUE(f.resyncing());

    // 垃圾结束，后面跟两条正常消息
    EXPECT_EQ(feed(f, "\ngood1\ngood2\n"),
              (std::vector<std::string>{"good1", "good2"}));
    EXPECT_FALSE(f.resyncing());
    EXPECT_EQ(f.stats().resyncs, 1u);
}

TEST(LineFramer, MessageExactlyAtLimitIsAccepted) {
    // 边界：恰好等于上限应当放行，超过一个字节才拒绝
    LineFramer::Options opt;
    opt.maxLineBytes = 32;
    LineFramer f(opt);

    const std::string exact(32, 'a');
    EXPECT_EQ(feed(f, exact + "\n"), (std::vector<std::string>{exact}));

    const std::string over(33, 'b');
    EXPECT_TRUE(feed(f, over + "\n").empty());
    EXPECT_EQ(f.stats().oversizedDropped, 1u);
}

// ── 缓冲区管理（相对原实现的性能改进）──────────────────────────────────────

TEST(LineFramer, FullyConsumedChunkNeedsNoCompaction) {
    // 全部消费完时走 O(1) 的 clear，不做 memmove
    LineFramer f;
    for (int i = 0; i < 500; ++i) {
        feed(f, "message-" + std::to_string(i) + "\n");
    }
    EXPECT_EQ(f.stats().compactions, 0u);
    EXPECT_EQ(f.buffered(), 0u);
}

TEST(LineFramer, CompactionIsAmortizedNotPerMessage) {
    // 部分消费的场景：压缩次数应该远小于消息条数，
    // 而不是原实现那样"每取一条搬一次"
    LineFramer::Options opt;
    opt.compactThreshold = 4096;
    LineFramer f(opt);

    const std::string tail = "incomplete-tail-without-newline";
    std::string chunk;
    for (int i = 0; i < 2000; ++i) {
        chunk += "m" + std::to_string(i) + "\n";
    }
    chunk += tail;  // 末尾留一段残缺，逼迫缓冲区不能整体 clear

    f.append(chunk);
    const auto got = drain(f);

    EXPECT_EQ(got.size(), 2000u);
    EXPECT_EQ(f.buffered(), tail.size());
    // 2000 条消息，压缩次数应该是个位数
    EXPECT_LT(f.stats().compactions, 10u)
        << "压缩了 " << f.stats().compactions << " 次，退化成按条搬运了";
}

TEST(LineFramer, TracksPeakBufferedBytes) {
    LineFramer f;
    f.append(std::string(5000, 'x'));
    EXPECT_GE(f.stats().maxBufferedBytes, 5000u);
    feed(f, "\n");  // 取走后峰值不回落
    EXPECT_GE(f.stats().maxBufferedBytes, 5000u);
}

// ── 状态管理 ────────────────────────────────────────────────────────────────

TEST(LineFramer, ResetDiscardsPartialData) {
    // 连接断开重连时必须 reset，否则上条连接的残留字节会和新连接的数据
    // 拼成一条畸形消息
    LineFramer f;
    f.append("partial-data-no-newline");
    EXPECT_GT(f.buffered(), 0u);

    f.reset();
    EXPECT_EQ(f.buffered(), 0u);
    EXPECT_FALSE(f.resyncing());

    EXPECT_EQ(feed(f, "fresh\n"), (std::vector<std::string>{"fresh"}));
}

TEST(LineFramer, StatsCountEmittedLines) {
    LineFramer f;
    feed(f, "a\nb\nc\n");
    EXPECT_EQ(f.stats().linesEmitted, 3u);
    EXPECT_EQ(f.stats().bytesAppended, 6u);

    f.resetStats();
    EXPECT_EQ(f.stats().linesEmitted, 0u);
}

TEST(LineFramer, ZeroMaxLineBytesIsClampedToOne) {
    LineFramer::Options opt;
    opt.maxLineBytes = 0;
    LineFramer f(opt);
    EXPECT_EQ(f.options().maxLineBytes, 1u);
}

TEST(LineFramer, AppendNullOrZeroLengthIsSafe) {
    LineFramer f;
    f.append(nullptr, 0);
    f.append(nullptr, 10);
    f.append("x", 0);
    EXPECT_EQ(f.buffered(), 0u);
    EXPECT_EQ(f.stats().bytesAppended, 0u);
}

// ── 编码 ────────────────────────────────────────────────────────────────────

TEST(LineFramer, EncodeAppendsDelimiter) {
    std::string out;
    ASSERT_TRUE(LineFramer::encode(R"({"type":"ack"})", out));
    EXPECT_EQ(out, "{\"type\":\"ack\"}\n");
}

TEST(LineFramer, EncodeRejectsPayloadContainingDelimiter) {
    // 行分隔协议的固有局限：载荷不能含分隔符，必须在发送端就拦住，
    // 否则接收端会把一条消息拆成两条畸形消息
    std::string out;
    EXPECT_FALSE(LineFramer::encode("has\nnewline", out));
}

TEST(LineFramer, EncodeDecodeRoundTrip) {
    const std::vector<std::string> messages = {
        R"({"type":"start_plan","payload":{}})",
        R"({"type":"progress","task_id":"t-1","payload":{"percent":50}})",
        R"({"type":"error","message":"boom"})",
    };

    std::string wire;
    for (const auto& m : messages) {
        std::string encoded;
        ASSERT_TRUE(LineFramer::encode(m, encoded));
        wire += encoded;
    }

    LineFramer f;
    EXPECT_EQ(feed(f, wire), messages);
}
