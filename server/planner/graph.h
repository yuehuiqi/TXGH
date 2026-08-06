#ifndef THGH_SERVER_PLANNER_GRAPH_H
#define THGH_SERVER_PLANNER_GRAPH_H

// ─────────────────────────────────────────────────────────────────────────────
// 通信网络图与路径算法
//
// ── 为什么是"边级"而不是"节点级"图 ────────────────────────────────────────
// 两个节点之间可能存在**多条平行链路**：同一对节点既有光缆又有微波时，
// 就是两条独立的边，带宽和时延都不同。
// 节点级邻接表（node → [neighbor]）无法表达这种情况，
// 所以邻接表存的是 node → [(neighbor, edgeId)]，路径也用**边序列**表示。
// 这样带宽扣减能精确落到具体那条物理链路上。
//
// ── 权重用传播时延而不是跳数 ──────────────────────────────────────────────
// 甲方的 QoS 约束是端到端时延，所以最短路要按时延最小算。
// 按跳数最少的话，一条走卫星的单跳路径会击败三跳光缆路径 ——
// 但卫星单向传播就有 100+ms，光缆三跳可能只有几毫秒。
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace thgh {

using NodeId = int;
using EdgeId = int;

// 一条边（无向）。id 即它在 edges 数组里的下标。
struct Edge {
    NodeId u = 0;
    NodeId v = 0;
    double weight = 0.0;    // 传播时延，秒
    double capacity = 0.0;  // 带宽，bps

    // 给定一端，返回另一端
    NodeId other(NodeId from) const { return from == u ? v : u; }
};

// 邻接项。除了邻居的 NodeId，还带上它的**紧凑索引**。
//
// 为什么要多存一个索引：NodeId 是业务侧给的，可以是任意整数、不连续；
// 而最短路算法要为每个节点维护 dist/parent/settled 三份状态。
// 用 unordered_map<NodeId, …> 存这些状态的话，每条边松弛都要做好几次哈希查找 ——
// 实测这部分占了 Dijkstra 总耗时的 60%（perf 数据见 docsTXGH/05-p4-planner.md）。
// 建图时把 NodeId 映射到 [0, nodeCount) 的紧凑索引，
// 算法内部就能用 vector 直接下标寻址，把哈希查找从热路径上彻底去掉。
struct Adj {
    NodeId node = 0;   // 邻居的业务 id
    EdgeId edge = 0;   // 连接这条邻接的边
    int nodeIdx = 0;   // 邻居的紧凑索引
};

// 边级无向图
class Graph {
public:
    // 添加一条边，返回它的 id
    EdgeId addEdge(NodeId u, NodeId v, double weight, double capacity);

    std::size_t edgeCount() const { return m_edges.size(); }
    const Edge& edge(EdgeId id) const { return m_edges[static_cast<std::size_t>(id)]; }
    const std::vector<Edge>& edges() const { return m_edges; }

    // 节点 u 的邻接
    const std::vector<Adj>& adjacency(NodeId u) const;
    // 按紧凑索引取邻接（算法内部用，省掉一次哈希查找）
    const std::vector<Adj>& adjacencyByIndex(int idx) const { return m_adj[static_cast<std::size_t>(idx)]; }

    // NodeId ↔ 紧凑索引。节点不存在时 nodeIndex 返回 -1。
    int nodeIndex(NodeId u) const;
    NodeId nodeAt(int idx) const { return m_nodeList[static_cast<std::size_t>(idx)]; }
    std::size_t nodeCount() const { return m_nodeList.size(); }

    bool hasNode(NodeId u) const { return m_nodeIndex.count(u) > 0; }
    std::vector<NodeId> nodes() const;

    // 把边序列还原成节点序列（从 start 出发沿边递推）。
    // 返回空表示边序列不连续 —— 这是拼接路径时最容易出错的地方，
    // 必须显式检查而不能假设。
    std::vector<NodeId> edgesToNodes(const std::vector<EdgeId>& path,
                                     NodeId start) const;

    double pathWeight(const std::vector<EdgeId>& path) const;

    void clear();

private:
    // 首次见到某个 NodeId 时给它分配紧凑索引
    int internNode(NodeId u);

    std::vector<Edge> m_edges;
    std::vector<NodeId> m_nodeList;              // 索引 → NodeId
    std::unordered_map<NodeId, int> m_nodeIndex; // NodeId → 索引（只在入口查一次）
    std::vector<std::vector<Adj>> m_adj;         // 按索引存的邻接表
    static const std::vector<Adj> kEmptyAdj;
};

// 路径搜索时的禁用集合（Yen 算法要用）
struct Bans {
    std::unordered_set<EdgeId> edges;
    std::unordered_set<NodeId> nodes;
};

// ── Dijkstra ────────────────────────────────────────────────────────────────
// 返回 src → dst 的最小时延路径（边序列）。不可达返回空。
//
// 实现要点有两条，第二条是被实测数据纠正过来的：
//
//  1) 优先队列里只放 (cost, nodeIdx)，路径通过 parent 数组回溯重建。
//     原型把整条边序列塞进堆元素，每次 push 都要拷贝一个 vector。
//     ⚠️ 但实测下来，这一项的收益远没有想象中大（800 节点稠密图上只有 1.1x，
//        甚至一度是负收益）—— 因为真正的瓶颈在下面第 2 条，它把差异淹没了。
//        原先这里写着"复杂度退化到 O(V·E log V)"，实验不支持这个说法，已删除。
//
//  2) ★ dist/parent/settled 全部用**紧凑索引 + vector**，不用 unordered_map。
//     这是实测出来的真正大头：改之前 perf 显示 60% 的时间花在哈希查找上
//     （settled.find 32.5% + dist.find 20.3% + operator[] 8.2%），
//     而 Dijkstra 本体只占 20.9%。改完之后热点回到算法本身。
//     教训是：先量再改。凭直觉认定的瓶颈和实际瓶颈可以完全是两回事。
std::vector<EdgeId> dijkstra(const Graph& g, NodeId src, NodeId dst,
                             const Bans& bans = Bans{});

// ── Yen's K 最短路 ──────────────────────────────────────────────────────────
// 返回不超过 K 条**无环**路径，按总时延升序。
//
// 算法骨架：
//   A = [最短路]
//   循环 k = 1..K-1：
//     对 A[k-1] 上的每个偏离点 i：
//       rootPath = A[k-1] 的前 i 条边（走到 spurNode）
//       禁用：所有与 rootPath 前缀相同的已知路径的第 i 条边（避免重复）
//             + rootPath 上除 spurNode 外的全部节点（避免成环）
//       spurPath = Dijkstra(spurNode → dst)
//       候选 = rootPath + spurPath，放进候选堆 B
//     从 B 取代价最小的加入 A
//
// ⚠️ 原型实现的偏差（本实现已修正）：
//   原型取 rootPath = prevPath[:i+1] 但 spurNode = prevNodes[i]，
//   前者走完到达的是 prevNodes[i+1]，两段**不衔接**；
//   拼接后在还原节点序列时找不到对应边而返回 None，
//   于是绝大多数候选路径被静默丢弃，产出的并非真正的 K 短路。
//   正确的取法是 rootPath = prevPath[:i]，恰好走到 spurNode。
std::vector<std::vector<EdgeId>> yenKShortestPaths(const Graph& g, NodeId src,
                                                   NodeId dst, int K);

}  // namespace thgh

#endif  // THGH_SERVER_PLANNER_GRAPH_H
