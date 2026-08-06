// 图算法单元测试
//
// ── 验证策略：暴力枚举做参考实现 ────────────────────────────────────────────
// 不与算法侧的 Python 原型对拍，而是在测试里写一份**暴力枚举所有简单路径**
// 的参考实现，再和 Dijkstra / Yen 的结果比对。
//
// 这样做更严格：
//   * 暴力解的正确性是显然的（穷举所有可能，不存在算法技巧）
//   * 对拍只能证明"两个实现一致"，如果原型本身有缺陷，一致反而是坏事 ——
//     而原型的 Yen 恰好就有偏离点取法的 off-by-one（见 graph.h 注释）
//   * 暴力解能覆盖随机图，比几个手工样例的覆盖面大得多
//
// 代价是只能在小图上跑（路径数随规模爆炸），所以用 6~8 个节点的随机图。

#include "planner/graph.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

using namespace thgh;

namespace {

// ── 暴力参考实现：枚举 src→dst 的全部简单路径 ──────────────────────────────
void enumerateAll(const Graph& g, NodeId cur, NodeId dst,
                  std::vector<EdgeId>& path, std::set<NodeId>& visited,
                  std::vector<std::vector<EdgeId>>& out) {
    if (cur == dst) {
        out.push_back(path);
        return;
    }
    for (const Adj& a : g.adjacency(cur)) {
        if (visited.count(a.node) > 0) {
            continue;  // 简单路径：不重复经过节点
        }
        visited.insert(a.node);
        path.push_back(a.edge);
        enumerateAll(g, a.node, dst, path, visited, out);
        path.pop_back();
        visited.erase(a.node);
    }
}

// 枚举全部简单路径并按总权重升序排序
std::vector<std::vector<EdgeId>> bruteForceAllPaths(const Graph& g, NodeId src,
                                                    NodeId dst) {
    std::vector<std::vector<EdgeId>> all;
    std::vector<EdgeId> path;
    std::set<NodeId> visited{src};
    enumerateAll(g, src, dst, path, visited, all);

    std::stable_sort(all.begin(), all.end(),
                     [&g](const std::vector<EdgeId>& a,
                          const std::vector<EdgeId>& b) {
                         return g.pathWeight(a) < g.pathWeight(b);
                     });
    return all;
}

// 造一个连通的随机图
Graph makeRandomGraph(unsigned seed, int nodeCount, double extraEdgeProb) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> w(0.001, 1.0);
    std::uniform_real_distribution<double> p(0.0, 1.0);

    Graph g;
    // 先连成一条链，保证连通
    for (int i = 1; i < nodeCount; ++i) {
        g.addEdge(i - 1, i, w(rng), 1e6);
    }
    // 再随机加边（可能产生平行边，这正是边级图要处理的情况）
    for (int i = 0; i < nodeCount; ++i) {
        for (int j = i + 1; j < nodeCount; ++j) {
            if (p(rng) < extraEdgeProb) {
                g.addEdge(i, j, w(rng), 1e6);
            }
        }
    }
    return g;
}

}  // namespace

// ── Graph 基础 ──────────────────────────────────────────────────────────────

TEST(Graph, EdgesToNodesFollowsPath) {
    Graph g;
    const EdgeId e0 = g.addEdge(1, 2, 1.0, 100);
    const EdgeId e1 = g.addEdge(2, 3, 1.0, 100);
    EXPECT_EQ(g.edgesToNodes({e0, e1}, 1), (std::vector<NodeId>{1, 2, 3}));
    // 反向走同样成立（无向图）
    EXPECT_EQ(g.edgesToNodes({e1, e0}, 3), (std::vector<NodeId>{3, 2, 1}));
}

TEST(Graph, EdgesToNodesRejectsDisconnectedSequence) {
    // ★ 这正是原型 Yen 的缺陷会触发的情况：
    //   拼接出来的边序列不连续，还原节点序列时必须能检测出来
    Graph g;
    const EdgeId e0 = g.addEdge(1, 2, 1.0, 100);
    g.addEdge(2, 3, 1.0, 100);
    const EdgeId e2 = g.addEdge(4, 5, 1.0, 100);
    EXPECT_TRUE(g.edgesToNodes({e0, e2}, 1).empty()) << "不连续的边序列应被拒绝";
}

TEST(Graph, SupportsParallelEdges) {
    // 同一对节点之间的多条链路（光缆 + 微波）必须是独立的边
    Graph g;
    const EdgeId fiber = g.addEdge(1, 2, 0.001, 10e9);
    const EdgeId micro = g.addEdge(1, 2, 0.002, 50e6);
    EXPECT_NE(fiber, micro);
    EXPECT_EQ(g.edgeCount(), 2u);
    EXPECT_DOUBLE_EQ(g.edge(fiber).capacity, 10e9);
    EXPECT_DOUBLE_EQ(g.edge(micro).capacity, 50e6);
    // 邻接表里两条都在
    EXPECT_EQ(g.adjacency(1).size(), 2u);
}

TEST(Graph, NodeOrderIsDeterministic) {
    // 节点遍历顺序必须确定，否则规划结果不可复现
    Graph g;
    g.addEdge(5, 3, 1.0, 100);
    g.addEdge(1, 9, 1.0, 100);
    g.addEdge(3, 1, 1.0, 100);
    EXPECT_EQ(g.nodes(), (std::vector<NodeId>{1, 3, 5, 9}));
}

// ── Dijkstra ────────────────────────────────────────────────────────────────

TEST(Dijkstra, FindsDirectEdge) {
    Graph g;
    const EdgeId e = g.addEdge(1, 2, 5.0, 100);
    EXPECT_EQ(dijkstra(g, 1, 2), (std::vector<EdgeId>{e}));
}

TEST(Dijkstra, PrefersLowerDelayOverFewerHops) {
    // ★ 权重是时延不是跳数：
    //   一跳的卫星链路（100ms）应该输给三跳的光缆（各 1ms）
    Graph g;
    const EdgeId sat = g.addEdge(1, 4, 0.100, 2e6);      // 一跳，100ms
    const EdgeId f1 = g.addEdge(1, 2, 0.001, 10e9);      // 三跳，共 3ms
    const EdgeId f2 = g.addEdge(2, 3, 0.001, 10e9);
    const EdgeId f3 = g.addEdge(3, 4, 0.001, 10e9);

    const auto path = dijkstra(g, 1, 4);
    EXPECT_EQ(path, (std::vector<EdgeId>{f1, f2, f3}))
        << "选了跳数少但时延高的路径";
    EXPECT_NE(path, (std::vector<EdgeId>{sat}));
}

TEST(Dijkstra, ReturnsEmptyWhenUnreachable) {
    Graph g;
    g.addEdge(1, 2, 1.0, 100);
    g.addEdge(3, 4, 1.0, 100);
    EXPECT_TRUE(dijkstra(g, 1, 4).empty());
}

TEST(Dijkstra, RespectsBannedEdges) {
    Graph g;
    const EdgeId direct = g.addEdge(1, 2, 1.0, 100);
    const EdgeId a = g.addEdge(1, 3, 2.0, 100);
    const EdgeId b = g.addEdge(3, 2, 2.0, 100);

    EXPECT_EQ(dijkstra(g, 1, 2), (std::vector<EdgeId>{direct}));

    Bans bans;
    bans.edges.insert(direct);
    EXPECT_EQ(dijkstra(g, 1, 2, bans), (std::vector<EdgeId>{a, b}));
}

TEST(Dijkstra, RespectsBannedNodes) {
    Graph g;
    const EdgeId a = g.addEdge(1, 3, 1.0, 100);
    const EdgeId b = g.addEdge(3, 2, 1.0, 100);
    const EdgeId c = g.addEdge(1, 4, 5.0, 100);
    const EdgeId d = g.addEdge(4, 2, 5.0, 100);

    EXPECT_EQ(dijkstra(g, 1, 2), (std::vector<EdgeId>{a, b}));

    Bans bans;
    bans.nodes.insert(3);
    EXPECT_EQ(dijkstra(g, 1, 2, bans), (std::vector<EdgeId>{c, d}));
}

TEST(Dijkstra, PicksCheaperParallelEdge) {
    Graph g;
    g.addEdge(1, 2, 0.010, 50e6);          // 微波，慢
    const EdgeId fiber = g.addEdge(1, 2, 0.001, 10e9);  // 光缆，快
    EXPECT_EQ(dijkstra(g, 1, 2), (std::vector<EdgeId>{fiber}));
}

TEST(Dijkstra, MatchesBruteForceOnRandomGraphs) {
    // ★ 与暴力枚举对照：Dijkstra 的结果必须等于所有简单路径中权重最小的那条
    for (unsigned seed = 1; seed <= 60; ++seed) {
        const Graph g = makeRandomGraph(seed, 7, 0.35);
        for (NodeId s = 0; s < 7; ++s) {
            for (NodeId t = 0; t < 7; ++t) {
                if (s == t) continue;
                const auto got = dijkstra(g, s, t);
                const auto all = bruteForceAllPaths(g, s, t);

                ASSERT_FALSE(all.empty()) << "随机图应连通 seed=" << seed;
                ASSERT_FALSE(got.empty())
                    << "seed=" << seed << " " << s << "->" << t << " 应可达";

                EXPECT_NEAR(g.pathWeight(got), g.pathWeight(all.front()), 1e-12)
                    << "seed=" << seed << " " << s << "->" << t
                    << " Dijkstra 未找到最短路";
                // 路径本身要合法：连续且起止正确
                const auto nodes = g.edgesToNodes(got, s);
                ASSERT_FALSE(nodes.empty());
                EXPECT_EQ(nodes.back(), t);
            }
        }
    }
}

// ── Yen K 最短路 ★ ─────────────────────────────────────────────────────────

TEST(YenKShortestPaths, ReturnsSinglePathWhenOnlyOneExists) {
    Graph g;
    const EdgeId e0 = g.addEdge(1, 2, 1.0, 100);
    const EdgeId e1 = g.addEdge(2, 3, 1.0, 100);
    const auto paths = yenKShortestPaths(g, 1, 3, 4);
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], (std::vector<EdgeId>{e0, e1}));
}

TEST(YenKShortestPaths, ReturnsPathsInAscendingCost) {
    Graph g;
    g.addEdge(1, 2, 1.0, 100);   // 直连，最短
    g.addEdge(1, 3, 2.0, 100);
    g.addEdge(3, 2, 2.0, 100);   // 绕一跳
    g.addEdge(1, 4, 10.0, 100);
    g.addEdge(4, 2, 10.0, 100);  // 绕远路

    const auto paths = yenKShortestPaths(g, 1, 2, 3);
    ASSERT_EQ(paths.size(), 3u);
    for (std::size_t i = 1; i < paths.size(); ++i) {
        EXPECT_LE(g.pathWeight(paths[i - 1]), g.pathWeight(paths[i]))
            << "第 " << i << " 条路径的代价小于前一条，未按升序返回";
    }
}

TEST(YenKShortestPaths, AllReturnedPathsAreLoopFree) {
    for (unsigned seed = 1; seed <= 40; ++seed) {
        const Graph g = makeRandomGraph(seed, 7, 0.4);
        const auto paths = yenKShortestPaths(g, 0, 6, 5);
        for (const auto& p : paths) {
            const auto nodes = g.edgesToNodes(p, 0);
            ASSERT_FALSE(nodes.empty())
                << "seed=" << seed << " 返回了不连续的路径";
            std::set<NodeId> uniq(nodes.begin(), nodes.end());
            EXPECT_EQ(uniq.size(), nodes.size())
                << "seed=" << seed << " 返回了带环的路径";
            EXPECT_EQ(nodes.back(), 6);
        }
    }
}

TEST(YenKShortestPaths, PathsAreDistinct) {
    for (unsigned seed = 1; seed <= 40; ++seed) {
        const Graph g = makeRandomGraph(seed, 7, 0.4);
        const auto paths = yenKShortestPaths(g, 0, 6, 6);
        std::set<std::vector<EdgeId>> uniq(paths.begin(), paths.end());
        EXPECT_EQ(uniq.size(), paths.size())
            << "seed=" << seed << " 返回了重复路径";
    }
}

TEST(YenKShortestPaths, MatchesBruteForceOnRandomGraphs) {
    // ★★ 最强的一条：Yen 返回的前 K 条路径，必须与
    //     "枚举全部简单路径后按权重排序取前 K" 逐条等价（按权重比较）。
    //     这是 K 短路正确性的完整定义。
    int checked = 0;
    for (unsigned seed = 1; seed <= 50; ++seed) {
        const Graph g = makeRandomGraph(seed, 6, 0.45);
        for (NodeId s = 0; s < 6; ++s) {
            for (NodeId t = s + 1; t < 6; ++t) {
                const int K = 5;
                const auto got = yenKShortestPaths(g, s, t, K);
                const auto all = bruteForceAllPaths(g, s, t);

                const std::size_t expectCount =
                    std::min<std::size_t>(all.size(), static_cast<std::size_t>(K));
                ASSERT_EQ(got.size(), expectCount)
                    << "seed=" << seed << " " << s << "->" << t
                    << " 返回路径条数不对（暴力枚举共 " << all.size() << " 条）";

                for (std::size_t i = 0; i < got.size(); ++i) {
                    EXPECT_NEAR(g.pathWeight(got[i]), g.pathWeight(all[i]), 1e-12)
                        << "seed=" << seed << " " << s << "->" << t
                        << " 第 " << i + 1 << " 短路径的代价不符";
                }
                ++checked;
            }
        }
    }
    EXPECT_GT(checked, 500) << "覆盖的用例太少，实验没有说服力";
}

TEST(YenKShortestPaths, HandlesKLargerThanAvailablePaths) {
    Graph g;
    g.addEdge(1, 2, 1.0, 100);
    g.addEdge(2, 3, 1.0, 100);
    // 只有一条路径，要 100 条
    const auto paths = yenKShortestPaths(g, 1, 3, 100);
    EXPECT_EQ(paths.size(), 1u) << "路径不足时应返回实际条数而不是报错";
}

TEST(YenKShortestPaths, ReturnsEmptyWhenUnreachable) {
    Graph g;
    g.addEdge(1, 2, 1.0, 100);
    g.addEdge(3, 4, 1.0, 100);
    EXPECT_TRUE(yenKShortestPaths(g, 1, 4, 4).empty());
}

TEST(YenKShortestPaths, RejectsNonPositiveK) {
    Graph g;
    g.addEdge(1, 2, 1.0, 100);
    EXPECT_TRUE(yenKShortestPaths(g, 1, 2, 0).empty());
    EXPECT_TRUE(yenKShortestPaths(g, 1, 2, -1).empty());
}

TEST(YenKShortestPaths, IsDeterministic) {
    // 同样的输入必须给出完全相同的输出 —— 规划系统的硬要求
    const Graph g = makeRandomGraph(42, 8, 0.4);
    const auto first = yenKShortestPaths(g, 0, 7, 5);
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(yenKShortestPaths(g, 0, 7, 5), first)
            << "第 " << i << " 次调用结果与首次不同";
    }
}

TEST(YenKShortestPaths, UsesParallelEdgesAsDistinctPaths) {
    // 同一对节点间的两条平行链路应算作两条不同的路径 ——
    // 这正是需要边级图的原因
    Graph g;
    const EdgeId fiber = g.addEdge(1, 2, 0.001, 10e9);
    const EdgeId micro = g.addEdge(1, 2, 0.002, 50e6);

    const auto paths = yenKShortestPaths(g, 1, 2, 4);
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0], (std::vector<EdgeId>{fiber})) << "应先返回时延更小的光缆";
    EXPECT_EQ(paths[1], (std::vector<EdgeId>{micro}));
}
