// ─────────────────────────────────────────────────────────────────────────────
// 规划算法性能实验
//
// 三组对照：
//   实验一  Dijkstra 三个版本的逐步优化（这是本次移植的主要性能工作）
//   实验二  Yen K 短路：K 与规模对耗时的影响
//   实验三  端到端 plan()：不同节点规模下的完整规划耗时
//
// 实验一之所以做成**三路**而不是两路：优化分两步走，
// 如果只对比首尾两版，就说不清收益到底来自哪一步。
// 事实上第一步（parent 回溯）的收益远小于预期，第二步（去哈希表）才是大头 ——
// 这个结论是量出来的，不是猜出来的。
// ─────────────────────────────────────────────────────────────────────────────

#include "planner/graph.h"
#include "planner/static_planner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <queue>
#include <random>
#include <string>
#include <vector>

using namespace thgh;

namespace {

double nowSec() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ── 版本 A：路径存堆元素 + 哈希表（原型写法）────────────────────────────────
// 逻辑上没错，两个问题：
//   * 堆元素是变长 vector，每次 push 要拷贝整条路径，堆内每次交换也要搬运它
//   * 每个节点的状态存在 unordered_map 里，每条边松弛都要做哈希查找
std::vector<EdgeId> dijkstraPathInHeap(const Graph& g, NodeId src, NodeId dst) {
    if (src == dst || !g.hasNode(src) || !g.hasNode(dst)) {
        return {};
    }
    using Item = std::pair<double, std::vector<EdgeId>>;
    struct Cmp {
        bool operator()(const Item& a, const Item& b) const {
            return a.first > b.first;  // 小根堆
        }
    };
    std::priority_queue<Item, std::vector<Item>, Cmp> pq;
    std::unordered_map<NodeId, double> best;

    pq.push({0.0, {}});
    best[src] = 0.0;

    while (!pq.empty()) {
        Item cur = pq.top();
        pq.pop();
        const std::vector<NodeId> nodes = g.edgesToNodes(cur.second, src);
        if (nodes.empty()) {
            continue;
        }
        const NodeId u = nodes.back();
        if (u == dst) {
            return cur.second;
        }
        auto it = best.find(u);
        if (it != best.end() && cur.first > it->second) {
            continue;
        }
        for (const Adj& a : g.adjacency(u)) {
            const double nd = cur.first + g.edge(a.edge).weight;
            auto vit = best.find(a.node);
            if (vit == best.end() || nd < vit->second) {
                best[a.node] = nd;
                std::vector<EdgeId> next = cur.second;  // ← 整条路径拷贝
                next.push_back(a.edge);
                pq.push({nd, std::move(next)});
            }
        }
    }
    return {};
}

// ── 版本 B：parent 回溯 + 哈希表（第一步优化后的中间版本）──────────────────
// 堆元素缩成 (cost, node) 定长对，路径靠 parent 回溯重建。
// 但 dist/parent/settled 仍是 unordered_map —— 这一版是本次改造的第一稿，
// 保留在这里是为了把两步优化的收益分开量。
std::vector<EdgeId> dijkstraParentHashmap(const Graph& g, NodeId src,
                                          NodeId dst) {
    if (src == dst || !g.hasNode(src) || !g.hasNode(dst)) {
        return {};
    }
    std::unordered_map<NodeId, double> dist;
    std::unordered_map<NodeId, std::pair<NodeId, EdgeId>> parent;
    std::unordered_set<NodeId> settled;

    using Item = std::pair<double, NodeId>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;

    dist[src] = 0.0;
    pq.emplace(0.0, src);

    while (!pq.empty()) {
        const auto [cost, u] = pq.top();
        pq.pop();
        if (settled.count(u) > 0) {
            continue;
        }
        auto dit = dist.find(u);
        if (dit == dist.end() || cost > dit->second) {
            continue;
        }
        settled.insert(u);
        if (u == dst) {
            break;
        }
        for (const Adj& a : g.adjacency(u)) {
            if (settled.count(a.node) > 0) {
                continue;
            }
            const double nd = cost + g.edge(a.edge).weight;
            auto vit = dist.find(a.node);
            if (vit == dist.end() || nd < vit->second) {
                dist[a.node] = nd;
                parent[a.node] = {u, a.edge};
                pq.emplace(nd, a.node);
            }
        }
    }

    if (settled.count(dst) == 0) {
        return {};
    }
    std::vector<EdgeId> path;
    NodeId cur = dst;
    while (cur != src) {
        auto it = parent.find(cur);
        if (it == parent.end()) {
            return {};
        }
        path.push_back(it->second.second);
        cur = it->second.first;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

// 随机连通图：先连成环保证连通，再按密度加边
Graph makeGraph(int n, double density, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> w(1e-5, 1e-3);
    std::uniform_real_distribution<double> p(0.0, 1.0);

    Graph g;
    for (int i = 0; i < n; ++i) {
        g.addEdge(i, (i + 1) % n, w(rng), 1e9);
    }
    for (int i = 0; i < n; ++i) {
        for (int j = i + 2; j < n; ++j) {
            if (p(rng) < density) {
                g.addEdge(i, j, w(rng), 1e9);
            }
        }
    }
    return g;
}

// 造一批分布在一片区域内的节点
std::vector<PlanNode> makeNodes(int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> lon(116.0, 117.0);
    std::uniform_real_distribution<double> lat(39.0, 40.0);
    std::uniform_real_distribution<double> alt(0.0, 500.0);

    const std::vector<std::vector<std::string>> kMethodSets = {
        {"fiber", "microwave"},
        {"microwave", "manet"},
        {"fiber", "microwave", "satellite"},
        {"microwave", "scatter", "manet"},
    };

    std::vector<PlanNode> nodes;
    nodes.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        PlanNode nd;
        nd.id = i;
        // 三成干线、七成支线，接近真实组网的比例
        nd.nodeType = (i % 10 < 3) ? "干线" : "支线";
        nd.longitude = lon(rng);
        nd.latitude = lat(rng);
        nd.altitude = alt(rng);
        nd.commMethods = kMethodSets[static_cast<std::size_t>(i) % kMethodSets.size()];
        nodes.push_back(std::move(nd));
    }
    return nodes;
}

std::vector<PlanFlow> makeFlows(int count, int nodeCount, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> node(0, nodeCount - 1);
    std::uniform_real_distribution<double> rate(1e6, 20e6);
    std::uniform_int_distribution<int> qos(0, 2);

    std::vector<PlanFlow> flows;
    flows.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        PlanFlow f;
        f.fid = i;
        f.src = node(rng);
        do {
            f.dst = node(rng);
        } while (f.dst == f.src);
        f.rateBps = rate(rng);
        f.qosLevel = qos(rng);
        flows.push_back(f);
    }
    return flows;
}

// ── 实验一 ──────────────────────────────────────────────────────────────────
void benchDijkstra() {
    std::printf("\n【实验一】Dijkstra 逐步优化（每行 200 次查询的总耗时）\n");
    std::printf("  A = 路径存堆 + 哈希表（原型写法）\n");
    std::printf("  B = parent 回溯 + 哈希表（第一步优化）\n");
    std::printf("  C = parent 回溯 + 紧凑索引数组（当前实现）\n");
    std::printf("────────────────────────────────────────────────────────────"
                "────────────────\n");
    std::printf("%7s %7s %9s %9s %9s %8s %8s %8s %s\n", "节点数", "边数",
                "A(ms)", "B(ms)", "C(ms)", "A→B", "B→C", "A→C", "一致");

    for (int n : {50, 100, 200, 400, 800}) {
        const Graph g = makeGraph(n, 0.05, 20250804u);

        // 固定跑同样的 (src,dst) 集合，三种实现完全同输入
        std::vector<std::pair<NodeId, NodeId>> queries;
        for (int i = 0; i < 200; ++i) {
            queries.emplace_back(i % n, (i * 37 + n / 2) % n);
        }

        auto run = [&](std::vector<EdgeId> (*fn)(const Graph&, NodeId, NodeId),
                       std::vector<std::vector<EdgeId>>& out) {
            out.clear();
            const double t = nowSec();
            for (const auto& [s, d] : queries) {
                out.push_back(fn(g, s, d));
            }
            return (nowSec() - t) * 1000.0;
        };

        std::vector<std::vector<EdgeId>> outA, outB, outC;
        const double msA = run(dijkstraPathInHeap, outA);
        const double msB = run(dijkstraParentHashmap, outB);

        const double t2 = nowSec();
        for (const auto& [s, d] : queries) {
            outC.push_back(dijkstra(g, s, d));
        }
        const double msC = (nowSec() - t2) * 1000.0;

        // 只比总权重：等权路径可能有多条，边序列不必逐一相同
        bool allSame = true;
        for (std::size_t i = 0; i < queries.size(); ++i) {
            const double wa = g.pathWeight(outA[i]);
            const double wb = g.pathWeight(outB[i]);
            const double wc = g.pathWeight(outC[i]);
            if (outA[i].empty() != outC[i].empty() ||
                outB[i].empty() != outC[i].empty() ||
                std::abs(wa - wc) > 1e-12 || std::abs(wb - wc) > 1e-12) {
                allSame = false;
            }
        }

        std::printf("%7d %7zu %9.2f %9.2f %9.2f %7.2fx %7.2fx %7.2fx %s\n", n,
                    g.edgeCount(), msA, msB, msC, msA / msB, msB / msC,
                    msA / msC, allSame ? "是" : "否 ✗");
    }
    std::printf("说明：三个版本输出的最短路总权重完全一致，差异只在实现开销上。\n"
                "      A→B 是把变长堆元素换成定长对的收益；\n"
                "      B→C 是把三份节点状态从 unordered_map 换成 vector 的收益。\n");
}

// ── 实验二 ──────────────────────────────────────────────────────────────────
void benchYen() {
    std::printf("\n【实验二】Yen K 最短路：K 与图规模的影响\n");
    std::printf("──────────────────────────────────────────────────────────"
                "──────────────\n");
    std::printf("%8s %8s %6s %14s %12s\n", "节点数", "边数", "K", "单次耗时(ms)",
                "实际路径数");

    for (int n : {50, 100, 200}) {
        const Graph g = makeGraph(n, 0.05, 20250804u);
        for (int K : {2, 4, 8}) {
            const int rounds = 20;
            std::size_t got = 0;
            const double t0 = nowSec();
            for (int i = 0; i < rounds; ++i) {
                const auto paths =
                    yenKShortestPaths(g, i % n, (i * 37 + n / 2) % n, K);
                got += paths.size();
            }
            const double ms = (nowSec() - t0) * 1000.0 / rounds;
            std::printf("%8d %8zu %6d %14.3f %12.1f\n", n, g.edgeCount(), K, ms,
                        static_cast<double>(got) / rounds);
        }
    }
    std::printf("说明：Yen 每找一条新路径要跑 O(路径长度) 次 Dijkstra，\n"
                "      所以耗时大致随 K 线性增长 —— 这也是实验一的收益会被放大的原因。\n");
}

// ── 实验三 ──────────────────────────────────────────────────────────────────
void benchPlan() {
    std::printf("\n【实验三】端到端 plan()：建拓扑 + 选路 + 分配带宽\n");
    std::printf("──────────────────────────────────────────────────────────"
                "──────────────\n");
    std::printf("%8s %8s %8s %12s %10s %10s\n", "节点数", "流数", "链路数",
                "耗时(ms)", "满足流数", "满足率");

    for (int n : {20, 50, 100, 200}) {
        const int flowCount = n;
        const auto nodes = makeNodes(n, 20250804u);
        const auto flows = makeFlows(flowCount, n, 20250804u);

        // 先跑一次预热，把首次分配内存的开销排除掉
        {
            StaticPlanner warm(nodes, flows);
            warm.plan();
        }

        const int rounds = 5;
        double totalMs = 0.0;
        PlanResult last;
        for (int i = 0; i < rounds; ++i) {
            StaticPlanner p(nodes, flows);
            const double t0 = nowSec();
            last = p.plan();
            totalMs += (nowSec() - t0) * 1000.0;
        }

        int satisfied = 0;
        std::vector<bool> seen(static_cast<std::size_t>(flowCount), false);
        for (const PlanPathResult& pr : last.planResults) {
            if (pr.isSatisfied && !seen[static_cast<std::size_t>(pr.fid)]) {
                seen[static_cast<std::size_t>(pr.fid)] = true;
                ++satisfied;
            }
        }

        std::printf("%8d %8d %8zu %12.2f %10d %9.1f%%\n", n, flowCount,
                    last.links.size(), totalMs / rounds, satisfied,
                    100.0 * satisfied / flowCount);
    }
    std::printf("说明：节点数翻倍时链路数约按平方增长（干线两两互联），\n"
                "      所以耗时增长明显快于线性。\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("规划算法性能实验\n");
    std::printf("编译器 %s，", __VERSION__);
#ifdef NDEBUG
    std::printf("构建类型 Release/RelWithDebInfo\n");
#else
    std::printf("构建类型 Debug（★ 数据仅供参考，应在 Release 下测量）\n");
#endif

    benchDijkstra();
    benchYen();
    benchPlan();
    return 0;
}
