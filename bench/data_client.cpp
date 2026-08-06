// ─────────────────────────────────────────────────────────────────────────────
// data_client —— 数据访问协议的端到端验证与压测（P6）
//
// 单测能证明 DAO 对、编解码对，证明不了**整条链路**对：
//   协议 → 路由 → 计算线程 → 连接池 → SQL → 回程
// 这个程序走真实 socket，把每个操作跑一遍并校验读回来的内容。
//
// 模式：
//   crud     全部 CRUD 操作走一遍，逐项校验（默认）
//   batch    ★ 批量 vs 逐条的往返次数对照 —— 这是 P6 最重要的一个量化实验
//   concurrent  多客户端并发读写，验证服务端不串数据
//   garbage  畸形请求，验证服务端不被打死
//
// 用法：data_client [host] [port] [mode] [n]

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

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  %-52s %s\n", what, ok ? "通过" : "失败 ✗");
    if (!ok) {
        ++g_failures;
    }
}

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

// 一条同步请求-应答的连接
class Session {
public:
    bool open(const char* host, uint16_t port) {
        m_fd = dialServer(host, port);
        return m_fd >= 0;
    }
    ~Session() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }

    // 发一个 data_request，等 data_reply。ok 写回 okOut，data 写回 dataOut。
    bool call(const std::string& op, const json& args, json& dataOut,
              std::string& errOut, int timeoutMs = 20000) {
        const std::uint64_t id = ++m_nextId;
        json payload;
        payload["op"] = op;
        payload["args"] = args;
        json msg;
        msg["type"] = "data_request";
        msg["task_id"] = id;
        msg["payload"] = std::move(payload);

        if (!sendAll(m_fd, msg.dump() + "\n")) {
            errOut = "发送失败";
            return false;
        }

        const auto deadline =
            Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            auto pos = m_buf.find('\n');
            while (pos != std::string::npos) {
                const std::string line = m_buf.substr(0, pos);
                m_buf.erase(0, pos + 1);
                const json j = json::parse(line, nullptr, false);
                if (!j.is_discarded() && j.is_object()) {
                    const std::string type = j.value("type", "");
                    if (type == "data_reply" &&
                        j.value("task_id", 0ULL) == id) {
                        const auto& pl = j.at("payload");
                        if (!pl.value("ok", false)) {
                            errOut = pl.value("error", "(未说明)");
                            return false;
                        }
                        dataOut = pl.value("data", json::object());
                        return true;
                    }
                    if (type == "error") {
                        errOut = j.value("message", "(未说明)");
                        return false;
                    }
                }
                pos = m_buf.find('\n');
            }

            const auto left = deadline - Clock::now();
            if (left <= std::chrono::milliseconds(0)) {
                errOut = "等待响应超时";
                return false;
            }
            timeval tv{
                static_cast<time_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(left).count()),
                static_cast<suseconds_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(left)
                        .count() % 1000000)};
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(m_fd, &rs);
            if (::select(m_fd + 1, &rs, nullptr, nullptr, &tv) <= 0) {
                errOut = "等待响应超时";
                return false;
            }
            char tmp[65536];
            const ssize_t n = ::recv(m_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                errOut = "连接被对端关闭";
                return false;
            }
            m_buf.append(tmp, static_cast<std::size_t>(n));
        }
    }

    // 发裸字节（畸形报文测试用）
    bool raw(const std::string& s) { return sendAll(m_fd, s); }
    int fd() const { return m_fd; }

private:
    int m_fd = -1;
    std::string m_buf;
    std::uint64_t m_nextId = 0;
};

json makeNode(int sceneId, int nodeId) {
    json n;
    n["scene_id"] = sceneId;
    n["node_id"] = nodeId;
    n["name"] = "节点" + std::to_string(nodeId);
    n["node_type"] = (nodeId % 3 == 0) ? "干线" : "支线";
    n["status"] = "在线";
    n["longitude"] = 116.0 + 0.001 * nodeId;
    n["latitude"] = 39.0 + 0.001 * nodeId;
    n["altitude"] = 10.0 * (nodeId % 30);
    n["interference_db"] = 0.0;
    n["comm_methods"] = R"(["fiber","microwave"])";
    n["device_params"] = "[]";
    n["device_connections"] = "[]";
    return n;
}

json makeLink(int sceneId, int src, int dst) {
    json l;
    l["scene_id"] = sceneId;
    l["src"] = src;
    l["dst"] = dst;
    l["link_type"] = "wireless";
    l["wireless_type"] = "microwave";
    l["bandwidth_bps"] = 50e6;
    l["prop_delay_s"] = 1e-5;
    l["comm_protocol"] = "";
    l["device_type"] = "";
    l["flows"] = "[]";
    return l;
}

// ── 模式一：完整 CRUD ───────────────────────────────────────────────────────
int runCrud(const char* host, uint16_t port) {
    Session s;
    if (!s.open(host, port)) {
        std::fprintf(stderr, "连接失败\n");
        return 1;
    }
    json data;
    std::string err;

    // 建场景
    json scene;
    scene["name"] = "P6端到端测试场景";
    scene["description"] = "由 data_client 创建";
    scene["scene_type"] = "测试";
    scene["create_time"] = "2026-08-04 00:00:00";
    if (!s.call("scene.create", scene, data, err)) {
        std::fprintf(stderr, "创建场景失败：%s\n", err.c_str());
        return 1;
    }
    const int sceneId = data.value("id", -1);
    check("scene.create 返回了有效 id", sceneId > 0);
    std::printf("  （场景 id = %d）\n", sceneId);

    // 列场景，确认能查到
    bool found = false;
    if (s.call("scene.list", json::object(), data, err)) {
        for (const auto& it : data.at("items")) {
            if (it.value("id", -1) == sceneId) {
                found = true;
                check("scene.list 读回的名称一致",
                      it.value("name", "") == "P6端到端测试场景");
            }
        }
    }
    check("scene.list 能查到刚建的场景", found);

    // 改场景
    scene["id"] = sceneId;
    scene["name"] = "P6端到端测试场景-已改名";
    check("scene.update 成功", s.call("scene.update", scene, data, err));
    found = false;
    if (s.call("scene.list", json::object(), data, err)) {
        for (const auto& it : data.at("items")) {
            if (it.value("id", -1) == sceneId &&
                it.value("name", "") == "P6端到端测试场景-已改名") {
                found = true;
            }
        }
    }
    check("scene.update 的改动确实落库了", found);

    // 加节点
    if (!s.call("node.add", makeNode(sceneId, 1), data, err)) {
        std::fprintf(stderr, "node.add 失败：%s\n", err.c_str());
        return 1;
    }
    const int nodeDbId = data.value("id", -1);
    check("node.add 返回了有效 id", nodeDbId > 0);

    // 批量加节点
    json batch;
    batch["scene_id"] = sceneId;
    json items = json::array();
    for (int i = 2; i <= 20; ++i) {
        items.push_back(makeNode(sceneId, i));
    }
    batch["items"] = items;
    check("node.addBatch 成功", s.call("node.addBatch", batch, data, err));
    check("node.addBatch 报告写入 19 条", data.value("affected", 0) == 19);

    // 列节点
    json listArgs;
    listArgs["scene_id"] = sceneId;
    std::size_t nodeCount = 0;
    if (s.call("node.list", listArgs, data, err)) {
        nodeCount = data.at("items").size();
    }
    check("node.list 读回 20 个节点", nodeCount == 20);

    // 校验字段往返：中文名和复合字段不能丢
    bool fieldsOk = false;
    if (s.call("node.list", listArgs, data, err)) {
        for (const auto& it : data.at("items")) {
            if (it.value("node_id", 0) == 1) {
                fieldsOk = it.value("name", "") == "节点1" &&
                           it.value("comm_methods", "") ==
                               R"(["fiber","microwave"])" &&
                           it.value("node_type", "") == "支线";
            }
        }
    }
    check("节点字段（含中文、复合 JSON）原样往返", fieldsOk);

    // 改节点
    json upd = makeNode(sceneId, 1);
    upd["id"] = nodeDbId;
    upd["name"] = "改过的节点";
    upd["longitude"] = 120.5;
    check("node.update 成功", s.call("node.update", upd, data, err));
    bool updOk = false;
    if (s.call("node.list", listArgs, data, err)) {
        for (const auto& it : data.at("items")) {
            if (it.value("id", -1) == nodeDbId) {
                updOk = it.value("name", "") == "改过的节点" &&
                        std::abs(it.value("longitude", 0.0) - 120.5) < 1e-9;
            }
        }
    }
    check("node.update 的改动确实落库了", updOk);

    // 链路
    json lbatch;
    lbatch["scene_id"] = sceneId;
    json litems = json::array();
    for (int i = 1; i < 20; ++i) {
        litems.push_back(makeLink(sceneId, i, i + 1));
    }
    lbatch["items"] = litems;
    check("link.addBatch 成功", s.call("link.addBatch", lbatch, data, err));
    std::size_t linkCount = 0;
    if (s.call("link.list", listArgs, data, err)) {
        linkCount = data.at("items").size();
    }
    check("link.list 读回 19 条链路", linkCount == 19);

    // 清链路
    check("link.clearByScene 成功",
          s.call("link.clearByScene", listArgs, data, err));
    if (s.call("link.list", listArgs, data, err)) {
        linkCount = data.at("items").size();
    }
    check("清空后链路数为 0", linkCount == 0);

    // 删节点
    json delArgs;
    delArgs["id"] = nodeDbId;
    check("node.delete 成功", s.call("node.delete", delArgs, data, err));
    if (s.call("node.list", listArgs, data, err)) {
        nodeCount = data.at("items").size();
    }
    check("删除后节点数为 19", nodeCount == 19);

    // 模板
    json nt;
    nt["template_name"] = "测试节点模板";
    nt["node_type"] = "干线";
    nt["comm_methods"] = R"(["fiber"])";
    nt["interference_db"] = 1.5;
    nt["description"] = "端到端测试";
    nt["device_params"] = "[]";
    check("nodeTemplate.save 成功",
          s.call("nodeTemplate.save", nt, data, err));
    bool tplOk = false;
    if (s.call("nodeTemplate.list", json::object(), data, err)) {
        for (const auto& it : data.at("items")) {
            if (it.value("template_name", "") == "测试节点模板") {
                tplOk = std::abs(it.value("interference_db", 0.0) - 1.5) < 1e-9;
            }
        }
    }
    check("nodeTemplate 往返一致", tplOk);

    json lt;
    lt["template_name"] = "测试链路模板";
    lt["link_type"] = "wireless";
    lt["wireless_type"] = "satellite";
    lt["bandwidth_bps"] = 2e6;
    lt["description"] = "端到端测试";
    check("linkTemplate.save 成功",
          s.call("linkTemplate.save", lt, data, err));

    // ★ 删场景：外键 ON DELETE CASCADE 应把节点一起清掉
    delArgs["id"] = sceneId;
    check("scene.delete 成功", s.call("scene.delete", delArgs, data, err));
    if (s.call("node.list", listArgs, data, err)) {
        nodeCount = data.at("items").size();
    }
    check("删场景后其节点被级联清除（外键 CASCADE 生效）", nodeCount == 0);

    return g_failures == 0 ? 0 : 1;
}

// ── 模式二：★ 批量 vs 逐条 ─────────────────────────────────────────────────
int runBatch(const char* host, uint16_t port, int n) {
    Session s;
    if (!s.open(host, port)) {
        std::fprintf(stderr, "连接失败\n");
        return 1;
    }
    json data;
    std::string err;

    auto newScene = [&](const char* name) -> int {
        json sc;
        sc["name"] = name;
        sc["description"] = "";
        sc["scene_type"] = "bench";
        sc["create_time"] = "2026-08-04 00:00:00";
        if (!s.call("scene.create", sc, data, err)) {
            return -1;
        }
        return data.value("id", -1);
    };

    std::printf("导入 %d 个节点，两种写法对照\n\n", n);

    // ── 逐条 ────────────────────────────────────────────────────────────
    const int sceneA = newScene("bench-逐条");
    if (sceneA < 0) {
        std::fprintf(stderr, "建场景失败：%s\n", err.c_str());
        return 1;
    }
    const auto t0 = Clock::now();
    for (int i = 1; i <= n; ++i) {
        if (!s.call("node.add", makeNode(sceneA, i), data, err)) {
            std::fprintf(stderr, "第 %d 条失败：%s\n", i, err.c_str());
            return 1;
        }
    }
    const double perItemMs = msSince(t0);

    // ── 批量 ────────────────────────────────────────────────────────────
    const int sceneB = newScene("bench-批量");
    json batch;
    batch["scene_id"] = sceneB;
    json items = json::array();
    for (int i = 1; i <= n; ++i) {
        items.push_back(makeNode(sceneB, i));
    }
    batch["items"] = items;

    const auto t1 = Clock::now();
    if (!s.call("node.addBatch", batch, data, err)) {
        std::fprintf(stderr, "批量失败：%s\n", err.c_str());
        return 1;
    }
    const double batchMs = msSince(t1);

    // ── 校验两边真的都写进去了 ─────────────────────────────────────────
    json args;
    std::size_t cntA = 0, cntB = 0;
    args["scene_id"] = sceneA;
    if (s.call("node.list", args, data, err)) {
        cntA = data.at("items").size();
    }
    args["scene_id"] = sceneB;
    if (s.call("node.list", args, data, err)) {
        cntB = data.at("items").size();
    }

    std::printf("%-14s %12s %12s %12s\n", "写法", "网络往返", "总耗时(ms)",
                "每节点(us)");
    std::printf("%-14s %12d %12.1f %12.1f\n", "逐条 node.add", n, perItemMs,
                perItemMs * 1000.0 / n);
    std::printf("%-14s %12d %12.1f %12.1f\n", "批量 addBatch", 1, batchMs,
                batchMs * 1000.0 / n);
    std::printf("\n  加速比 %.1fx，往返次数从 %d 降到 1\n", perItemMs / batchMs, n);

    check("逐条写入的条数正确", cntA == static_cast<std::size_t>(n));
    check("批量写入的条数正确", cntB == static_cast<std::size_t>(n));

    // 清理
    json del;
    del["id"] = sceneA;
    s.call("scene.delete", del, data, err);
    del["id"] = sceneB;
    s.call("scene.delete", del, data, err);

    std::printf("\n  说明：客户端导入场景原来就是 for 循环逐条写。直连数据库时只是慢，\n"
                "        改走协议后每条都是一次网络往返 —— 这正是必须提供批量接口的原因。\n");
    return g_failures == 0 ? 0 : 1;
}

// ── 模式三：并发读写 ───────────────────────────────────────────────────────
int runConcurrent(const char* host, uint16_t port, int clients) {
    std::printf("%d 个客户端各自建场景、写 50 个节点、读回校验\n", clients);

    std::atomic<int> okCount{0}, badCount{0};
    std::vector<std::thread> threads;

    for (int c = 0; c < clients; ++c) {
        threads.emplace_back([&, c] {
            Session s;
            if (!s.open(host, port)) {
                ++badCount;
                return;
            }
            json data;
            std::string err;

            json sc;
            sc["name"] = "并发场景" + std::to_string(c);
            sc["description"] = "";
            sc["scene_type"] = "concurrent";
            sc["create_time"] = "2026-08-04 00:00:00";
            if (!s.call("scene.create", sc, data, err)) {
                ++badCount;
                return;
            }
            const int sid = data.value("id", -1);

            json batch;
            batch["scene_id"] = sid;
            json items = json::array();
            for (int i = 1; i <= 50; ++i) {
                items.push_back(makeNode(sid, i));
            }
            batch["items"] = items;
            if (!s.call("node.addBatch", batch, data, err)) {
                ++badCount;
                return;
            }

            // ★ 关键校验：读回来的必须**只有自己那 50 个**。
            //   如果服务端把不同连接的请求串了，这里就会读到别人的数据。
            json args;
            args["scene_id"] = sid;
            if (!s.call("node.list", args, data, err)) {
                ++badCount;
                return;
            }
            bool allMine = data.at("items").size() == 50;
            for (const auto& it : data.at("items")) {
                if (it.value("scene_id", -1) != sid) {
                    allMine = false;
                }
            }
            if (allMine) {
                ++okCount;
            } else {
                ++badCount;
            }

            json del;
            del["id"] = sid;
            s.call("scene.delete", del, data, err);
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    std::printf("\n  成功=%d  失败=%d\n", okCount.load(), badCount.load());
    check("全部客户端只读到自己的数据（无串数据）",
          okCount.load() == clients && badCount.load() == 0);
    return g_failures == 0 ? 0 : 1;
}

// ── 模式四：畸形请求 ───────────────────────────────────────────────────────
int runGarbage(const char* host, uint16_t port) {
    // ★ 每条都曾是能打死服务端或造成注入的候选
    const std::vector<std::string> lines = {
        R"({"type":"data_request","task_id":1,"payload":{}})",
        R"({"type":"data_request","task_id":2,"payload":{"op":"不存在的操作"}})",
        R"({"type":"data_request","task_id":3,"payload":"不是对象"})",
        R"({"type":"data_request","task_id":4,"payload":{"op":"node.list"}})",
        R"({"type":"data_request","task_id":5,"payload":{"op":"node.addBatch","args":{"scene_id":1}}})",
        R"({"type":"data_request","task_id":6,"payload":{"op":"scene.create","args":{"name":123}}})",
        // ★ SQL 注入尝试。预处理语句应该把它当成普通字符串存进去，
        //   而不是当 SQL 执行。服务端存活 + 表还在 = 防住了。
        R"({"type":"data_request","task_id":7,"payload":{"op":"scene.create","args":{"name":"'); DROP TABLE scenes; --","description":"注入测试","scene_type":"x","create_time":"now"}}})",
        R"({"type":"data_request","task_id":8,"payload":{"op":"node.delete","args":{"id":"不是数字"}}})",
        "{ 这不是 JSON",
    };

    Session s;
    if (!s.open(host, port)) {
        std::fprintf(stderr, "连接失败\n");
        return 1;
    }

    std::printf("逐条发送 %zu 种畸形/恶意请求\n", lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        s.raw(lines[i] + "\n");
    }
    // 给服务端一点时间处理完
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 服务端还活着吗？还能正常服务吗？
    Session s2;
    if (!s2.open(host, port)) {
        std::fprintf(stderr, "服务端已不可连接 —— 被打死了\n");
        return 1;
    }
    json data;
    std::string err;
    const bool alive = s2.call("scene.list", json::object(), data, err);
    check("发完全部畸形请求后服务端仍可正常服务", alive);

    // ★ 表还在吗？（注入是否被防住）
    bool tableAlive = false;
    if (alive) {
        tableAlive = data.contains("items");
        // ★ 清理本轮制造的残留。
        //
        //   两类都要清，第一版只清了注入那条，结果每跑一次 verify
        //   就在库里留下一个空名字的场景 —— 演示时打开客户端会看到
        //   一串莫名其妙的空场景，很难解释。**测试不该留垃圾。**
        //
        //   ① 注入串被当作普通场景名存下（说明预处理语句防住了）
        //   ② name 传了非字符串（123），服务端按"取不到就用默认值"存成空串
        int cleaned = 0;
        for (const auto& it : data.at("items")) {
            const std::string nm = it.value("name", "");
            const bool isInjection = nm.find("DROP TABLE") != std::string::npos;
            const bool isEmptyName = nm.empty();
            if (!isInjection && !isEmptyName) {
                continue;
            }
            if (isInjection) {
                std::printf("  （注入串被当作普通场景名存下了：%s）\n", nm.c_str());
            }
            json del;
            del["id"] = it.value("id", -1);
            json d2;
            std::string e2;
            if (s2.call("scene.delete", del, d2, e2)) {
                ++cleaned;
            }
        }
        std::printf("  （已清理本轮制造的 %d 个残留场景）\n", cleaned);
    }
    check("scenes 表未被 DROP（预处理语句防住注入）", tableAlive);

    return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const uint16_t port =
        argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : 9000;
    const std::string mode = argc > 3 ? argv[3] : "crud";
    const int n = argc > 4 ? std::atoi(argv[4]) : 500;

    std::printf("═══ data_client [%s] %s:%u ═══\n", mode.c_str(), host, port);

    int rc = 2;
    if (mode == "crud") {
        rc = runCrud(host, port);
    } else if (mode == "batch") {
        rc = runBatch(host, port, n);
    } else if (mode == "concurrent") {
        rc = runConcurrent(host, port, n > 0 && n <= 64 ? n : 8);
    } else if (mode == "garbage") {
        rc = runGarbage(host, port);
    } else {
        std::fprintf(stderr, "未知模式：%s\n", mode.c_str());
        return 2;
    }

    std::printf("\n%s\n", g_failures == 0 ? "全部通过" : "存在失败项 ✗");
    return rc;
}
