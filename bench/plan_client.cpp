// ─────────────────────────────────────────────────────────────────────────────
// plan_client —— 分阶段推送的端到端验证客户端
//
// 单测能证明每个零件对，但证明不了**装起来之后消息顺序是对的**：
//   * Ack 是否真的先于 Progress 到达
//   * 进度百分比是否单调不减
//   * 计算期间断开连接，服务端会不会崩
//   * 并发压满计算队列时，是否正确回"服务繁忙"而不是卡死
// 这些只能在真实 socket 上验证。
//
// 用法：
//   plan_client [host] [port] [nodes] [flows] [mode]
//   mode: normal   正常跑一次，打印完整消息时序（默认）
//         abort    发完请求立刻断开，验证服务端不会因此崩溃
//         flood    并发灌满计算队列，验证过载时的拒绝行为
//         garbage  发各种畸形报文，验证服务端不被一条烂报文打死

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int dialServer(const char* host, uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

// 造一个规模可控的规划请求
std::string buildRequest(int nodeCount, int flowCount) {
    json nodes = json::array();
    for (int i = 0; i < nodeCount; ++i) {
        json n;
        n["id"] = i;
        n["node_type"] = (i % 10 < 3) ? "干线" : "支线";
        n["longitude"] = 116.0 + 0.9 * (i % 17) / 17.0;
        n["latitude"] = 39.0 + 0.9 * (i % 13) / 13.0;
        n["altitude"] = 10.0 * (i % 50);
        n["comm_methods"] = json::array({"fiber", "microwave", "manet"});
        nodes.push_back(std::move(n));
    }
    json flows = json::array();
    for (int i = 0; i < flowCount; ++i) {
        json f;
        f["fid"] = i;
        f["src_node_id"] = i % nodeCount;
        f["dst_node_id"] = (i * 7 + 1) % nodeCount;
        f["rate_bps"] = 1e6 * (1 + i % 20);
        f["qos_level"] = i % 3;
        flows.push_back(std::move(f));
    }
    json payload;
    payload["scene_id"] = 1;
    payload["nodes"] = std::move(nodes);
    payload["flows"] = std::move(flows);

    json msg;
    msg["type"] = "start_plan";
    msg["payload"] = std::move(payload);
    return msg.dump() + "\n";
}

bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// 读一行（以 '\n' 分隔）。超时或对端关闭返回 false。
bool recvLine(int fd, std::string& buf, std::string& lineOut, int timeoutMs) {
    for (;;) {
        const auto pos = buf.find('\n');
        if (pos != std::string::npos) {
            lineOut = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            return true;
        }
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        const int r = ::select(fd + 1, &rs, nullptr, nullptr, &tv);
        if (r <= 0) {
            return false;  // 超时或出错
        }
        char tmp[65536];
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return false;  // 对端关闭
        }
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

// ── 模式一：正常跑一次，检查消息时序 ────────────────────────────────────────
int runNormal(const char* host, uint16_t port, int nodes, int flows) {
    const int fd = dialServer(host, port);
    if (fd < 0) {
        std::fprintf(stderr, "连接失败\n");
        return 1;
    }

    const std::string req = buildRequest(nodes, flows);
    std::printf("请求：%d 节点 / %d 流，报文 %zu 字节\n", nodes, flows,
                req.size());

    const auto t0 = Clock::now();
    if (!sendAll(fd, req)) {
        std::fprintf(stderr, "发送失败\n");
        ::close(fd);
        return 1;
    }

    std::string buf, line;
    bool sawAck = false;
    int progressCount = 0;
    int lastPercent = -1;
    bool ordered = true;
    bool monotonic = true;
    double ackMs = -1, firstProgressMs = -1, resultMs = -1;
    int failures = 0;

    while (recvLine(fd, buf, line, 30000)) {
        const json j = json::parse(line, nullptr, false);
        if (j.is_discarded()) {
            std::printf("  [%.1fms] 收到无法解析的行\n", msSince(t0));
            ++failures;
            continue;
        }
        const std::string type = j.value("type", "");

        if (type == "ack") {
            ackMs = msSince(t0);
            sawAck = true;
            std::printf("  [%7.1fms] ack       task_id=%llu 节点=%d 流=%d\n",
                        ackMs,
                        static_cast<unsigned long long>(j.value("task_id", 0ULL)),
                        j["payload"].value("node_count", 0),
                        j["payload"].value("flow_count", 0));

        } else if (type == "progress") {
            const int pct = j["payload"].value("percent", -1);
            if (!sawAck) {
                ordered = false;   // ★ progress 跑到了 ack 前面
            }
            if (pct < lastPercent) {
                monotonic = false; // ★ 进度倒退
            }
            lastPercent = pct;
            if (progressCount == 0) {
                firstProgressMs = msSince(t0);
            }
            ++progressCount;
            std::printf("  [%7.1fms] progress  %3d%%  %s\n", msSince(t0), pct,
                        j["payload"].value("stage", "").c_str());

        } else if (type == "plan_result") {
            resultMs = msSince(t0);
            const auto& pl = j.at("payload");
            int satisfied = 0;
            for (const auto& p : pl.at("plan_results")) {
                if (p.value("is_satisfied", false)) {
                    ++satisfied;
                }
            }
            std::printf("  [%7.1fms] result    链路=%zu 路径=%zu 告警=%zu "
                        "满足=%d 服务端耗时=%.1fms\n",
                        resultMs, pl.at("links").size(),
                        pl.at("plan_results").size(), pl.at("warns").size(),
                        satisfied, pl.value("elapsed_s", 0.0) * 1000.0);
            break;

        } else if (type == "error") {
            std::printf("  [%7.1fms] error     %s\n", msSince(t0),
                        j.value("message", "").c_str());
            ++failures;
            break;
        }
    }
    ::close(fd);

    std::printf("\n── 断言 ──\n");
    auto check = [&failures](const char* what, bool ok) {
        std::printf("  %-40s %s\n", what, ok ? "通过" : "失败 ✗");
        if (!ok) {
            ++failures;
        }
    };
    check("收到 ack", sawAck);
    check("ack 先于所有 progress 到达", ordered);
    check("进度百分比单调不减", monotonic);
    check("收到至少一条 progress", progressCount > 0);
    check("最终收到 plan_result", resultMs > 0);
    check("ack 明显早于 result（这才叫分阶段）",
          ackMs >= 0 && resultMs > 0 && ackMs < resultMs);

    std::printf("\n  progress 条数=%d  ack=%.1fms  首条 progress=%.1fms  "
                "result=%.1fms\n",
                progressCount, ackMs, firstProgressMs, resultMs);
    return failures == 0 ? 0 : 1;
}

// ── 模式二：计算途中断开 ────────────────────────────────────────────────────
int runAbort(const char* host, uint16_t port, int nodes, int flows) {
    std::printf("发送 %d 节点 / %d 流的请求后立刻断开，重复 20 次\n", nodes, flows);
    const std::string req = buildRequest(nodes, flows);
    for (int i = 0; i < 20; ++i) {
        const int fd = dialServer(host, port);
        if (fd < 0) {
            std::fprintf(stderr, "第 %d 次连接失败\n", i);
            return 1;
        }
        sendAll(fd, req);
        // 不等任何响应，直接关。服务端此时大概率正在算。
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("全部断开完成。服务端若仍存活并能正常服务，说明连接生命周期处理正确。\n");

    // 再发一次正常请求确认服务端还活着
    std::printf("\n验证服务端仍可用：\n");
    return runNormal(host, port, 20, 20);
}

// ── 模式三：压满计算队列 ────────────────────────────────────────────────────
int runFlood(const char* host, uint16_t port, int nodes, int flows) {
    constexpr int kClients = 40;
    std::printf("并发 %d 个客户端同时发起 %d 节点 / %d 流的规划\n", kClients,
                nodes, flows);

    std::atomic<int> acked{0}, busy{0}, results{0}, failed{0};
    std::vector<std::thread> threads;
    const std::string req = buildRequest(nodes, flows);

    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&] {
            const int fd = dialServer(host, port);
            if (fd < 0) {
                ++failed;
                return;
            }
            if (!sendAll(fd, req)) {
                ++failed;
                ::close(fd);
                return;
            }
            std::string buf, line;
            while (recvLine(fd, buf, line, 30000)) {
                const json j = json::parse(line, nullptr, false);
                if (j.is_discarded()) {
                    continue;
                }
                const std::string t = j.value("type", "");
                if (t == "ack") {
                    ++acked;
                } else if (t == "plan_result") {
                    ++results;
                    break;
                } else if (t == "error") {
                    ++busy;
                    break;
                }
            }
            ::close(fd);
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    std::printf("\n  受理(ack)=%d  拒绝(busy)=%d  拿到结果=%d  连接失败=%d\n",
                acked.load(), busy.load(), results.load(), failed.load());
    std::printf("  ★ 关键：受理的请求必须都拿到结果，被拒的必须明确收到错误 ——\n"
                "     既不能静默丢弃，也不能让客户端一直干等。\n");

    const bool ok = failed.load() == 0 &&
                    results.load() == acked.load() &&
                    (acked.load() + busy.load()) == kClients;
    std::printf("  判定：%s\n", ok ? "通过" : "失败 ✗");
    return ok ? 0 : 1;
}

// ── 模式四：畸形报文 ────────────────────────────────────────────────────────
int runGarbage(const char* host, uint16_t port) {
    // ★ 每一条都曾经是能打死服务端的候选：JSON 解析抛异常、
    //   类型转换抛异常、超大数组撑爆内存、循环上界来自不可信输入。
    const std::vector<std::string> payloads = {
        "{\n",
        "not json at all\n",
        "\n",
        "[1,2,3]\n",
        "null\n",
        R"({"type":"start_plan"})" "\n",
        R"({"type":"start_plan","payload":null})" "\n",
        R"({"type":"start_plan","payload":{"nodes":"not an array"}})" "\n",
        R"({"type":{"nested":true}})" "\n",
        R"({"type":"start_plan","payload":{"nodes":[{"id":"x"}],"flows":[]}})" "\n",
        R"({"type":"start_plan","payload":{"nodes":[{"id":1}],"flows":[{"fid":1}],)"
        R"("config":{"paths_per_flow":999999}}})" "\n",
        R"({"type":"完全不存在的类型","payload":{}})" "\n",
        std::string(4096, 'A') + "\n",
    };

    std::printf("逐条发送 %zu 种畸形报文\n", payloads.size());
    int i = 0;
    for (const std::string& p : payloads) {
        const int fd = dialServer(host, port);
        if (fd < 0) {
            std::fprintf(stderr, "第 %d 条：连接失败 —— 服务端可能已经挂了\n", i);
            return 1;
        }
        sendAll(fd, p);
        std::string buf, line;
        const bool got = recvLine(fd, buf, line, 3000);
        std::printf("  [%2d] %-46s → %s\n", i,
                    p.size() > 44 ? (p.substr(0, 41) + "...").c_str()
                                  : p.substr(0, p.size() - 1).c_str(),
                    got ? line.substr(0, 60).c_str() : "(无响应)");
        ::close(fd);
        ++i;
    }

    std::printf("\n验证服务端仍可用：\n");
    return runNormal(host, port, 20, 20);
}

}  // namespace

int main(int argc, char** argv) {
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const uint16_t port =
        argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : 9000;
    const int nodes = argc > 3 ? std::atoi(argv[3]) : 100;
    const int flows = argc > 4 ? std::atoi(argv[4]) : 100;
    const std::string mode = argc > 5 ? argv[5] : "normal";

    std::printf("═══ plan_client [%s] %s:%u ═══\n", mode.c_str(), host, port);

    if (mode == "normal") {
        return runNormal(host, port, nodes, flows);
    }
    if (mode == "abort") {
        return runAbort(host, port, nodes, flows);
    }
    if (mode == "flood") {
        return runFlood(host, port, nodes, flows);
    }
    if (mode == "garbage") {
        return runGarbage(host, port);
    }
    std::fprintf(stderr, "未知模式：%s\n", mode.c_str());
    return 2;
}
