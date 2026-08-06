#include "thgh/line_framer.h"

#include <algorithm>

namespace thgh {

LineFramer::LineFramer() : LineFramer(Options()) {}

LineFramer::LineFramer(Options opt) : m_opt(opt) {
    // 上限为 0 没有意义（任何消息都会被判超长），抬到 1
    if (m_opt.maxLineBytes == 0) {
        m_opt.maxLineBytes = 1;
    }
}

void LineFramer::append(const char* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    m_stats.bytesAppended += len;
    m_buf.append(data, len);
    m_stats.maxBufferedBytes =
        std::max(m_stats.maxBufferedBytes, m_buf.size() - m_readPos);
}

void LineFramer::append(const std::string& data) {
    append(data.data(), data.size());
}

bool LineFramer::next(std::string& out) {
    // 循环而不是递归：跳过空行、以及从超长状态重新同步之后，
    // 都需要继续找下一条，用循环避免深度不可控的递归。
    for (;;) {
        const std::size_t nl = m_buf.find('\n', m_searchPos);

        if (nl == std::string::npos) {
            // 没有完整的一行。把扫描进度推到缓冲区末尾，
            // 下次 append 之后从这里继续，不再重复扫描已看过的字节。
            m_searchPos = m_buf.size();

            if (m_resyncing) {
                // 丢弃模式下这些字节全都不要，直接扔掉以免继续吃内存
                m_buf.clear();
                m_readPos = 0;
                m_searchPos = 0;
            } else if (m_buf.size() - m_readPos > m_opt.maxLineBytes) {
                // 累积了超过上限的字节仍未见到分隔符 → 判定异常流量
                enterResync();
            }
            return false;
        }

        if (m_resyncing) {
            // 找到分隔符，说明坏消息到此为止，从下一字节起重新同步
            m_resyncing = false;
            ++m_stats.resyncs;
            m_readPos = nl + 1;
            m_searchPos = m_readPos;
            compactIfNeeded();
            continue;
        }

        std::size_t lineLen = nl - m_readPos;

        // 单条消息本身超长：即使它是完整的一行也不接受。
        // 这里必须判断，否则"恰好在超限那一刻分隔符到了"就会绕过上限检查。
        if (lineLen > m_opt.maxLineBytes) {
            ++m_stats.oversizedDropped;
            m_readPos = nl + 1;
            m_searchPos = m_readPos;
            compactIfNeeded();
            continue;
        }

        const char* begin = m_buf.data() + m_readPos;

        // 容忍 CRLF：对端是 Windows 工具或 telnet 手工调试时会带 '\r'
        if (m_opt.trimTrailingCr && lineLen > 0 && begin[lineLen - 1] == '\r') {
            --lineLen;
        }

        m_readPos = nl + 1;
        m_searchPos = m_readPos;

        if (lineLen == 0 && m_opt.skipEmptyLines) {
            ++m_stats.emptyLinesSkipped;
            compactIfNeeded();
            continue;
        }

        out.assign(begin, lineLen);
        ++m_stats.linesEmitted;
        compactIfNeeded();
        return true;
    }
}

bool LineFramer::encode(const std::string& payload, std::string& out) {
    // 载荷含分隔符会直接破坏分帧，必须在发送端就拦住。
    // 这是行分隔协议的固有代价，换来的是实现简单、可读、便于手工调试。
    if (payload.find('\n') != std::string::npos) {
        return false;
    }
    out.reserve(payload.size() + 1);
    out.assign(payload);
    out.push_back('\n');
    return true;
}

std::size_t LineFramer::buffered() const {
    return m_buf.size() - m_readPos;
}

bool LineFramer::resyncing() const {
    return m_resyncing;
}

void LineFramer::resetStats() {
    m_stats = Stats();
}

void LineFramer::reset() {
    m_buf.clear();
    m_readPos = 0;
    m_searchPos = 0;
    m_resyncing = false;
}

// ── 私有 ────────────────────────────────────────────────────────────────────

void LineFramer::compactIfNeeded() {
    if (m_readPos == 0) {
        return;
    }

    // 最常见的情况：一个 chunk 里的消息全部取完。
    // 此时直接 clear，O(1)，不需要任何 memmove。
    // 原客户端实现每取一条就 remove(0, n) 搬一次剩余数据，
    // 一个 chunk 里 N 条消息就是 N 次搬运，这里被彻底消掉。
    if (m_readPos >= m_buf.size()) {
        m_buf.clear();
        m_readPos = 0;
        m_searchPos = 0;
        return;
    }

    // 部分消费：只有已消费字节积累到阈值才真正搬一次，
    // 把 O(每条消息一次 memmove) 摊薄成 O(每阈值字节一次)。
    if (m_readPos >= m_opt.compactThreshold) {
        m_buf.erase(0, m_readPos);
        m_searchPos -= m_readPos;
        m_readPos = 0;
        ++m_stats.compactions;
    }
}

void LineFramer::enterResync() {
    ++m_stats.oversizedDropped;
    m_resyncing = true;
    // 已缓冲的这些字节确定属于那条超长消息，直接丢掉。
    // 保留它们没有意义，而且会继续占内存。
    m_buf.clear();
    m_readPos = 0;
    m_searchPos = 0;
}

}  // namespace thgh
