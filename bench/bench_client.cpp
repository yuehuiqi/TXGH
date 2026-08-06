// 服务端压测客户端
//
// ⚠️ 必须与服务端**同机运行**。
//    开发机与服务器跨地域时 RTT 可达数十毫秒，压测客户端若跑在远端，
//    测出的 P99 全是网络往返延迟而非服务端性能。
//
// ── 测量方法 ──────────────────────────────────────────────────────────────
// 每条连接做请求-应答（ping-pong）：发一条消息、等回显、记录往返耗时。
// 这样测出的延迟是**完整的服务端处理链路**：
//   epoll 唤醒 → read → 分帧 → 业务回调 → write → 客户端收到
//
// 刻意不用"只发不收"的单向打流：那样测出来的是内核发送缓冲的吞吐，
// 服务端可能根本没处理完，数字虚高且没有延迟指标。
//
// 延迟统计保留全部样本再排序取分位数，而不是维护近似直方图 ——
// 压测时长有限（样本量百万级），内存完全够，换来的是精确的 P99/P999。
//
// 用法：
//   bench_client [host] [port] [连接数] [线程数] [时长秒] [消息字节数]

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    int connections = 100;
    int threads = 4;
    int durationSec = 10;
    int messageBytes = 128;
};

struct ThreadResult {
    std::uint64_t requests = 0;
    std::uint64_t errors = 0;
    std::vector<double> latenciesUs;  // 每次请求-应答的往返微秒数
};

std::atomic<bool> g_stop{false};

int connectTo(const Config& cfg) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    // 客户端也关 Nagle：否则小消息会被攒批，测出来的是 Nagle 的延迟不是服务端的
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(cfg.port);
    ::inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// 阻塞收满一整行（以 '\n' 结尾）
bool recvLine(int fd, std::string& buf) {
    char tmp[8192];
    for (;;) {
        const std::size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            buf.erase(0, nl + 1);
            return true;
        }
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return false;
        }
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

void workerThread(const Config& cfg, int connCount, ThreadResult* result) {
    std::vector<int> fds;
    std::vector<std::string> bufs;
    fds.reserve(connCount);

    for (int i = 0; i < connCount; ++i) {
        const int fd = connectTo(cfg);
        if (fd < 0) {
            ++result->errors;
            continue;
        }
        fds.push_back(fd);
    }
    bufs.resize(fds.size());

    const std::string payload(static_cast<std::size_t>(cfg.messageBytes), 'x');
    const std::string request = payload + "\n";

    // 预留容量，避免测量过程中 vector 扩容影响时延数据
    result->latenciesUs.reserve(
        static_cast<std::size_t>(cfg.durationSec) * 50000);

    while (!g_stop.load(std::memory_order_relaxed)) {
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if (g_stop.load(std::memory_order_relaxed)) {
                break;
            }
            const auto t0 = Clock::now();
            if (::send(fds[i], request.data(), request.size(), 0) < 0) {
                ++result->errors;
                continue;
            }
            if (!recvLine(fds[i], bufs[i])) {
                ++result->errors;
                continue;
            }
            const auto t1 = Clock::now();
            result->latenciesUs.push_back(
                std::chrono::duration<double, std::micro>(t1 - t0).count());
            ++result->requests;
        }
    }

    for (int fd : fds) {
        ::close(fd);
    }
}

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto idx = static_cast<std::size_t>(p / 100.0 * (sorted.size() - 1));
    return sorted[std::min(idx, sorted.size() - 1)];
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (argc > 1) cfg.host = argv[1];
    if (argc > 2) cfg.port = static_cast<uint16_t>(std::atoi(argv[2]));
    if (argc > 3) cfg.connections = std::atoi(argv[3]);
    if (argc > 4) cfg.threads = std::atoi(argv[4]);
    if (argc > 5) cfg.durationSec = std::atoi(argv[5]);
    if (argc > 6) cfg.messageBytes = std::atoi(argv[6]);

    if (cfg.threads < 1) cfg.threads = 1;
    if (cfg.connections < cfg.threads) cfg.connections = cfg.threads;

    std::printf("压测目标 %s:%u  连接数 %d  客户端线程 %d  时长 %ds  消息 %d 字节\n",
                cfg.host.c_str(), cfg.port, cfg.connections, cfg.threads,
                cfg.durationSec, cfg.messageBytes);

    std::vector<ThreadResult> results(static_cast<std::size_t>(cfg.threads));
    std::vector<std::thread> workers;

    const int perThread = cfg.connections / cfg.threads;
    const int remainder = cfg.connections % cfg.threads;

    const auto start = Clock::now();
    for (int i = 0; i < cfg.threads; ++i) {
        const int n = perThread + (i < remainder ? 1 : 0);
        workers.emplace_back(workerThread, std::cref(cfg), n,
                             &results[static_cast<std::size_t>(i)]);
    }

    std::this_thread::sleep_for(std::chrono::seconds(cfg.durationSec));
    g_stop.store(true);

    for (auto& t : workers) {
        t.join();
    }
    const double elapsed =
        std::chrono::duration<double>(Clock::now() - start).count();

    // 汇总
    std::uint64_t totalReq = 0, totalErr = 0;
    std::vector<double> allLat;
    for (const auto& r : results) {
        totalReq += r.requests;
        totalErr += r.errors;
        allLat.insert(allLat.end(), r.latenciesUs.begin(), r.latenciesUs.end());
    }
    std::sort(allLat.begin(), allLat.end());

    double sum = 0.0;
    for (double v : allLat) sum += v;

    std::printf("\n%-16s %s\n", "实际时长", (std::to_string(elapsed) + " s").c_str());
    std::printf("%-16s %llu\n", "总请求数", (unsigned long long)totalReq);
    std::printf("%-16s %llu\n", "错误数", (unsigned long long)totalErr);
    std::printf("%-16s %.0f\n", "QPS", totalReq / elapsed);
    std::printf("\n往返延迟（微秒）\n");
    std::printf("%-10s %10.1f\n", "平均", allLat.empty() ? 0.0 : sum / allLat.size());
    std::printf("%-10s %10.1f\n", "P50", percentile(allLat, 50));
    std::printf("%-10s %10.1f\n", "P90", percentile(allLat, 90));
    std::printf("%-10s %10.1f\n", "P99", percentile(allLat, 99));
    std::printf("%-10s %10.1f\n", "P99.9", percentile(allLat, 99.9));
    std::printf("%-10s %10.1f\n", "最大", allLat.empty() ? 0.0 : allLat.back());
    return 0;
}
