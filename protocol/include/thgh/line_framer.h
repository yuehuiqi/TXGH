#ifndef THGH_PROTOCOL_LINE_FRAMER_H
#define THGH_PROTOCOL_LINE_FRAMER_H

// ─────────────────────────────────────────────────────────────────────────────
// LineFramer —— 行缓冲分帧器（TCP 粘包 / 半包处理）
//
// TCP 是面向字节流的协议，**没有消息边界**。一次 read 可能：
//   * 只读到半条消息（半包）
//   * 一次读到好几条消息（粘包）
//   * 甚至在任意一个字节处被切断
// 所以应用层必须自己定义分帧方式。本协议用 '\n' 作为消息分隔符。
//
// ── 为什么单独抽一层，而不是像客户端那样内联在 readyRead 里 ────────────────
//   1. 服务端和客户端要用同一套分帧逻辑，行为必须一致
//   2. 分帧是纯字节操作，不碰 socket、不碰 JSON，因此能脱离网络单测 ——
//      粘包/半包这类边界用真实网络极难稳定构造，抽出来之后可以**逐字节**验证
//
// ── 相比客户端 SimBridge 原有实现修掉的三个问题 ────────────────────────────
//
//   原实现（simbridge.cpp:101-113）：
//       m_lineBuf += chunk;
//       while (true) {
//           int nl = m_lineBuf.indexOf('\n');       // ① 每次从头搜
//           if (nl < 0) break;
//           QByteArray line = m_lineBuf.left(nl).trimmed();
//           m_lineBuf.remove(0, nl + 1);            // ② 每条一次 O(n) memmove
//           if (!line.isEmpty()) parseLine(line);
//       }
//                                                   // ③ 缓冲区无上限
//
//   ① **重复扫描**：没找到 '\n' 时下次 append 后又从头搜一遍已扫过的字节，
//      长半包场景退化成 O(n²)。→ 本实现用 m_searchPos 记住扫描进度。
//
//   ② **反复 memmove**：一个 chunk 里有 N 条消息就搬 N 次剩余缓冲，
//      同样是 O(n²)。→ 本实现用 m_readPos 游标标记已消费位置，
//      全部消费完时直接 clear（O(1)，最常见情况），
//      只有"部分消费且积压超过阈值"才做一次压缩。
//
//   ③ **无长度上限**：对端只要一直发不含 '\n' 的字节，缓冲区就无限增长直到
//      进程 OOM —— 这是一个真实可利用的拒绝服务面。
//      → 本实现有 maxLineBytes 上限，超限后进入丢弃模式，
//        扫到下一个 '\n' 自动重新同步，不会因为一条坏消息就废掉整条连接。
//
// 线程安全：**无**。一个实例对应一条连接，由该连接所属的那一个线程独占使用。
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <string>

namespace thgh {

class LineFramer {
public:
    struct Options {
        // 单条消息的最大字节数（不含分隔符）。超限即判定为异常流量，
        // 丢弃并重新同步。默认 1 MiB：规划请求会带整个场景的节点与链路，
        // 报文可以很大，但不该到兆级以上。
        std::size_t maxLineBytes = 1u << 20;

        // 缓冲区压缩阈值：已消费字节数超过它才真正做一次 memmove。
        // 设得太小会退化成"每条消息搬一次"，设得太大会让缓冲区长期占着内存。
        std::size_t compactThreshold = 8192;

        // 跳过空行。与客户端原有行为一致（原实现里 line.isEmpty() 就不解析）。
        bool skipEmptyLines = true;

        // 容忍 CRLF：去掉行尾的 '\r'。
        // 对端如果是 Windows 上的工具或 telnet 手工调试，会带 \r。
        bool trimTrailingCr = true;
    };

    struct Stats {
        std::uint64_t bytesAppended = 0;      // 累计写入的字节数
        std::uint64_t linesEmitted = 0;       // 成功切出的完整消息数
        std::uint64_t emptyLinesSkipped = 0;  // 被跳过的空行数
        std::uint64_t oversizedDropped = 0;   // 因超长被丢弃的消息数
        std::uint64_t resyncs = 0;            // 超长后重新同步成功的次数
        std::uint64_t compactions = 0;        // 缓冲区压缩（memmove）次数
        std::size_t maxBufferedBytes = 0;     // 缓冲区占用峰值
    };

    // 注意不写成 `explicit LineFramer(Options opt = {})`：
    // 客户端用的是 Qt 5.12 自带的 MinGW GCC 7.3，它无法解析
    // "默认实参里构造一个带默认成员初始化器(NSDMI)的嵌套类"，
    // 会报 "constructor required before non-static data member ... has been parsed"。
    // 拆成两个构造函数即可绕开，这是在 TOPO 项目上踩过的同一个坑。
    LineFramer();
    explicit LineFramer(Options opt);

    // 追加从 socket 读到的原始字节。可以在任意字节边界处切分调用。
    void append(const char* data, std::size_t len);
    void append(const std::string& data);

    // 取出下一条完整消息。
    // 返回 true 表示 out 里是一条完整消息（已去掉分隔符与行尾 '\r'）；
    // 返回 false 表示当前缓冲里还没有完整的一行，需要继续 append。
    //
    // 典型用法：
    //     framer.append(buf, n);
    //     std::string line;
    //     while (framer.next(line)) { handle(line); }
    bool next(std::string& out);

    // 把一条消息编码成可直接写入 socket 的字节（追加分隔符）。
    // 若 payload 自身含 '\n' 会破坏分帧，这里直接拒绝并返回 false ——
    // 这是行分隔协议的固有局限：**不能承载含分隔符的二进制数据**，
    // 那种场景应该改用长度前缀。本协议载荷是紧凑 JSON，天然不含裸 '\n'。
    static bool encode(const std::string& payload, std::string& out);

    // ── 观察接口 ────────────────────────────────────────────────────────────
    std::size_t buffered() const;      // 当前缓冲区里尚未成帧的字节数
    bool resyncing() const;            // 是否处于超长丢弃 / 重新同步状态
    const Stats& stats() const { return m_stats; }
    void resetStats();

    // 清空缓冲与状态。连接断开重连时调用，避免把上一条连接的残留字节
    // 与新连接的数据拼接成一条畸形消息。
    void reset();

    const Options& options() const { return m_opt; }

private:
    void compactIfNeeded();
    void enterResync();

    Options m_opt;
    std::string m_buf;
    std::size_t m_readPos = 0;    // 已消费到的位置（m_buf 的绝对下标）
    std::size_t m_searchPos = 0;  // 已扫描 '\n' 到的位置，避免重复扫描
    bool m_resyncing = false;     // 超长丢弃模式：一直丢到遇见下一个 '\n'
    Stats m_stats;
};

}  // namespace thgh

#endif  // THGH_PROTOCOL_LINE_FRAMER_H
