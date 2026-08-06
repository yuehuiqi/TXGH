// ─────────────────────────────────────────────────────────────────────────────
// plan_server —— 规划服务端主程序
//
// 完整四层：网络（epoll 主从 Reactor）→ 协议（行分帧 + JSON）
//           → 业务计算（规划算法）→ 数据（MySQL，P6 接入）
//
// 线程构成：
//   1 个主 Reactor            只 accept + 心跳巡检
//   N 个从 Reactor            IO 读写与协议分帧
//   M 个计算线程              跑规划算法
//
// 用法：plan_server [port] [ioThreads] [computeThreads] [idleTimeoutSec]

#include "db/conn_pool.h"
#include "net/event_loop.h"
#include "net/event_loop_thread_pool.h"
#include "net/tcp_server.h"
#include "service/compute_pool.h"
#include "service/data_service.h"
#include "service/message_router.h"
#include "service/plan_service.h"

#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {
thgh::EventLoop* g_loop = nullptr;

void onSignal(int sig) {
    std::printf("\n收到信号 %d，正在退出...\n", sig);
    if (g_loop != nullptr) {
        g_loop->quit();
    }
}

const char* envOr(const char* key, const char* def) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? v : def;
}

// 数据库凭据只从环境变量读，不写进代码也不放进命令行参数。
// 命令行参数会出现在 `ps aux` 里，同机的任何用户都能看到密码。
bool readDbConfig(thgh::MySqlConfig& cfg) {
    const char* host = std::getenv("THGH_DB_HOST");
    if (host == nullptr) {
        return false;   // 没配就是不启用数据访问，不是错误
    }
    cfg.host = host;
    cfg.port = static_cast<unsigned>(std::atoi(envOr("THGH_DB_PORT", "3306")));
    cfg.user = envOr("THGH_DB_USER", "thgh");
    cfg.password = envOr("THGH_DB_PASSWORD", "");
    cfg.database = envOr("THGH_DB_NAME", "thgh");
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    const uint16_t port =
        argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9000;
    const std::size_t ioThreads =
        argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2]))
                 : thgh::EventLoopThreadPool::suggestedThreadNum();
    const std::size_t computeThreads =
        argc > 3 ? static_cast<std::size_t>(std::atoi(argv[3]))
                 : thgh::ComputePool::suggestedThreadNum();
    const int idleSec = argc > 4 ? std::atoi(argv[4]) : 60;

    // 重定向到文件时 stdout 默认全缓冲，tail -f 看不到输出、
    // 进程被 kill -9 时日志直接丢失。服务端必须行缓冲。
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    // 对端已关闭时继续 write 会收到 SIGPIPE，默认行为是终止进程。
    // 必须忽略它，让 write 返回 EPIPE 交给代码处理。
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);

    thgh::EventLoop loop;
    g_loop = &loop;

    thgh::ComputePool::Options poolOpt;
    poolOpt.numThreads = computeThreads;
    // 队列容量取线程数的 8 倍：留出突发缓冲，但不至于让排队时间
    // 长到客户端已经超时放弃了才轮到它算。
    poolOpt.maxQueueSize = computeThreads * 8;
    thgh::ComputePool pool(poolOpt);
    pool.start();

    thgh::PlanService service(&pool);

    // ── 数据访问（P6）──────────────────────────────────────────────────
    // 没配数据库就不启用：服务端仍能跑规划，数据请求会收到明确的拒绝。
    // 这样压测网络层时不必先起一个 MySQL。
    std::unique_ptr<thgh::ConnectionPool> dbPool;
    std::unique_ptr<thgh::DataService> dataService;
    thgh::MySqlConfig dbCfg;
    if (readDbConfig(dbCfg)) {
        thgh::ConnectionPool::Options popt;
        // 池上限与计算线程数对齐：真正并发执行 SQL 的就是那几个计算线程，
        // 池开得比它大只是白占数据库的连接数配额。
        popt.initialSize = 2;
        popt.maxSize = computeThreads > 2 ? computeThreads : 4;
        dbPool = std::make_unique<thgh::ConnectionPool>();
        if (!dbPool->init(dbCfg, popt)) {
            std::fprintf(stderr,
                         "[plan] 数据库连接池初始化失败，数据访问功能不可用\n");
            dbPool.reset();
        } else {
            dataService = std::make_unique<thgh::DataService>(&pool, dbPool.get());
            std::printf("[plan] 数据访问已启用：%s:%u/%s（池上限 %zu）\n",
                        dbCfg.host.c_str(), dbCfg.port, dbCfg.database.c_str(),
                        popt.maxSize);
        }
    } else {
        std::printf("[plan] 未设置 THGH_DB_HOST，数据访问功能未启用\n");
    }

    thgh::MessageRouter router(&service, dataService.get());

    thgh::TcpServer server(&loop, thgh::InetAddress(port), "plan");
    thgh::TcpServer::Options opt;
    opt.numThreads = ioThreads;
    opt.idleTimeoutSec = idleSec;
    opt.checkIntervalSec = idleSec / 3 > 0 ? idleSec / 3 : 1;
    server.setOptions(opt);

    server.setConnectionCallback([&service](const thgh::TcpConnectionPtr& conn) {
        std::printf("[plan] 连接 %s %s\n", conn->name().c_str(),
                    conn->connected() ? "建立" : "断开");
        service.onConnection(conn);
    });
    server.setMessageCallback(
        [&router](const thgh::TcpConnectionPtr& conn, const std::string& msg) {
            router.onMessage(conn, msg);
        });

    if (!server.start()) {
        std::fprintf(stderr, "启动失败：端口 %u 可能已被占用\n", port);
        pool.stop();
        return 1;
    }

    std::printf("[plan] 监听 %u | IO 线程 %zu | 计算线程 %zu | 队列上限 %zu\n",
                port, ioThreads, pool.threadCount(), poolOpt.maxQueueSize);

    loop.runEvery(std::chrono::milliseconds(10'000),
                  [&server, &service, &pool, &dataService] {
                      const auto s = server.stats();
                      const auto ps = service.stats();
                      const auto cs = pool.stats();
                      std::printf(
                          "[指标] 在线=%zu 峰值=%zu | 规划 受理=%llu 完成=%llu "
                          "拒绝=%llu 丢弃=%llu 非法=%llu | 进度 发出=%llu "
                          "跳过=%llu | 计算队列 排队=%zu 执行中=%zu 峰值=%zu\n",
                          s.currentConnections, s.peakConnections,
                          static_cast<unsigned long long>(ps.plansAccepted),
                          static_cast<unsigned long long>(ps.plansCompleted),
                          static_cast<unsigned long long>(ps.plansRejected),
                          static_cast<unsigned long long>(ps.plansDropped),
                          static_cast<unsigned long long>(ps.badRequests),
                          static_cast<unsigned long long>(ps.progressSent),
                          static_cast<unsigned long long>(ps.progressSkipped),
                          cs.queued, cs.running, cs.peakQueued);
                      if (dataService) {
                          const auto ds = dataService->stats();
                          std::printf(
                              "[数据] 请求=%llu 成功=%llu 失败=%llu 拒绝=%llu "
                              "丢弃=%llu\n",
                              static_cast<unsigned long long>(ds.requests),
                              static_cast<unsigned long long>(ds.succeeded),
                              static_cast<unsigned long long>(ds.failed),
                              static_cast<unsigned long long>(ds.rejected),
                              static_cast<unsigned long long>(ds.dropped));
                      }
                  });

    loop.loop();

    // ★ 停止顺序：先停网络再停计算池。
    //   反过来的话，计算池已经停了但网络还在收请求，
    //   每条请求都会因 submit 失败而回"服务繁忙"，日志一片红。
    server.stop();
    pool.stop();
    // 连接池最后关：计算线程可能还在用池里的连接，
    // 必须等它们 join 完（pool.stop() 保证了这一点）才能拆池。
    if (dbPool) {
        dbPool->shutdown();
    }
    std::printf("[plan] 已退出\n");
    return 0;
}
