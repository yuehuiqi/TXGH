// 回显服务端 —— P2 网络核心的端到端验证程序
//
// 业务逻辑刻意做到最简（收到什么回什么），目的是**只验证网络层**：
// 主从 Reactor 分发、ET 循环读、协议分帧、心跳踢除、优雅关闭。
// 业务真正的规划计算在 P4 接入。
//
// 用法：echo_server [port] [numThreads] [idleTimeoutSec]

#include "net/event_loop.h"
#include "net/event_loop_thread_pool.h"
#include "net/tcp_server.h"

#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
thgh::EventLoop* g_loop = nullptr;

void onSignal(int sig) {
    std::printf("\n收到信号 %d，正在退出...\n", sig);
    if (g_loop != nullptr) {
        g_loop->quit();   // quit 内部会唤醒阻塞中的 epoll_wait
    }
}
}  // namespace

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9000;
    const std::size_t threads =
        argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2]))
                 : thgh::EventLoopThreadPool::suggestedThreadNum();
    const int idleSec = argc > 3 ? std::atoi(argv[3]) : 30;

    // stdout 重定向到文件时默认是**全缓冲**（4KB 满了才落盘），
    // 结果是 tail -f 看不到任何输出、进程被 kill -9 时日志直接丢失。
    // 服务端必须改成行缓冲，才能实时观察运行状态。
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    // 必须忽略 SIGPIPE：对端已关闭时继续 write 会收到 SIGPIPE，
    // 默认行为是直接终止进程。服务端必须让 write 返回 EPIPE 由代码处理。
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);

    thgh::EventLoop loop;
    g_loop = &loop;

    thgh::TcpServer server(&loop, thgh::InetAddress(port), "echo");
    thgh::TcpServer::Options opt;
    opt.numThreads = threads;
    opt.idleTimeoutSec = idleSec;
    opt.checkIntervalSec = idleSec / 3 > 0 ? idleSec / 3 : 1;
    server.setOptions(opt);

    server.setConnectionCallback([](const thgh::TcpConnectionPtr& conn) {
        std::printf("[echo] 连接 %s %s\n", conn->name().c_str(),
                    conn->connected() ? "建立" : "断开");
    });

    server.setMessageCallback(
        [](const thgh::TcpConnectionPtr& conn, const std::string& msg) {
            conn->send(msg);   // 原样回显（send 内部会补分隔符）
        });

    if (!server.start()) {
        std::fprintf(stderr, "启动失败\n");
        return 1;
    }

    // 每 10 秒打一次指标，便于压测时观察
    loop.runEvery(std::chrono::milliseconds(10'000), [&server] {
        const auto s = server.stats();
        // 用跨全部 loop 的聚合值：只看主 Reactor 会误以为服务端没干活
        const auto ls = server.aggregatedLoopStats();
        std::printf("[指标] 在线=%zu 峰值=%zu 累计接受=%llu 关闭=%llu 超时踢除=%llu "
                    "| 循环=%llu 事件=%llu 唤醒=%llu\n",
                    s.currentConnections, s.peakConnections,
                    (unsigned long long)s.connectionsAccepted,
                    (unsigned long long)s.connectionsClosed,
                    (unsigned long long)s.idleKicked,
                    (unsigned long long)ls.loopIterations,
                    (unsigned long long)ls.eventsHandled,
                    (unsigned long long)ls.wakeups);
    });

    loop.loop();
    server.stop();
    std::printf("已退出\n");
    return 0;
}
