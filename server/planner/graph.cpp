#include "planner/graph.h"

#include <algorithm>
#include <queue>
#include <set>

namespace thgh {

const std::vector<Adj> Graph::kEmptyAdj;

// ── Graph ───────────────────────────────────────────────────────────────────

int Graph::internNode(NodeId u) {
    auto it = m_nodeIndex.find(u);
    if (it != m_nodeIndex.end()) {
        return it->second;
    }
    const int idx = static_cast<int>(m_nodeList.size());
    m_nodeIndex.emplace(u, idx);
    m_nodeList.push_back(u);
    m_adj.emplace_back();
    return idx;
}

int Graph::nodeIndex(NodeId u) const {
    auto it = m_nodeIndex.find(u);
    return it == m_nodeIndex.end() ? -1 : it->second;
}

EdgeId Graph::addEdge(NodeId u, NodeId v, double weight, double capacity) {
    const EdgeId id = static_cast<EdgeId>(m_edges.size());
    m_edges.push_back(Edge{u, v, weight, capacity});
    const int ui = internNode(u);
    const int vi = internNode(v);
    // 无向图：两端都要能找到这条边
    m_adj[static_cast<std::size_t>(ui)].push_back(Adj{v, id, vi});
    m_adj[static_cast<std::size_t>(vi)].push_back(Adj{u, id, ui});
    return id;
}

const std::vector<Adj>& Graph::adjacency(NodeId u) const {
    const int idx = nodeIndex(u);
    return idx < 0 ? kEmptyAdj : m_adj[static_cast<std::size_t>(idx)];
}

std::vector<NodeId> Graph::nodes() const {
    // 排序：让遍历顺序确定，结果可复现。
    // 索引顺序取决于建图时边的插入次序，虽然对同一份输入是确定的，
    // 但排序后对调用方更直观，也不依赖建图顺序。
    std::vector<NodeId> out = m_nodeList;
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<NodeId> Graph::edgesToNodes(const std::vector<EdgeId>& path,
                                        NodeId start) const {
    std::vector<NodeId> nodes;
    nodes.reserve(path.size() + 1);
    nodes.push_back(start);

    NodeId cur = start;
    for (EdgeId eid : path) {
        if (eid < 0 || static_cast<std::size_t>(eid) >= m_edges.size()) {
            return {};
        }
        const Edge& e = m_edges[static_cast<std::size_t>(eid)];
        // 当前节点必须是这条边的某一端，否则路径不连续
        if (e.u != cur && e.v != cur) {
            return {};
        }
        cur = e.other(cur);
        nodes.push_back(cur);
    }
    return nodes;
}

double Graph::pathWeight(const std::vector<EdgeId>& path) const {
    double sum = 0.0;
    for (EdgeId eid : path) {
        if (eid >= 0 && static_cast<std::size_t>(eid) < m_edges.size()) {
            sum += m_edges[static_cast<std::size_t>(eid)].weight;
        }
    }
    return sum;
}

void Graph::clear() {
    m_edges.clear();
    m_adj.clear();
    m_nodeList.clear();
    m_nodeIndex.clear();
}

// ── Dijkstra ────────────────────────────────────────────────────────────────

std::vector<EdgeId> dijkstra(const Graph& g, NodeId src, NodeId dst,
                             const Bans& bans) {
    if (src == dst) {
        return {};
    }
    if (bans.nodes.count(src) > 0 || bans.nodes.count(dst) > 0) {
        return {};
    }
    if (!g.hasNode(src) || !g.hasNode(dst)) {
        return {};
    }

    const int srcIdx = g.nodeIndex(src);
    const int dstIdx = g.nodeIndex(dst);
    const std::size_t n = g.nodeCount();

    constexpr double kInf = std::numeric_limits<double>::infinity();

    // ★ 全部按紧凑索引用 vector 存，不用 unordered_map。
    //   改造前这三份状态是哈希表，perf 显示 60% 的时间耗在 find/operator[] 上；
    //   节点索引本来就连续，直接下标寻址即可，没有任何理由付哈希的代价。
    std::vector<double> dist(n, kInf);
    // 回溯用：记录到达每个节点时"从哪个节点、经哪条边"过来的
    std::vector<int> parentNode(n, -1);
    std::vector<EdgeId> parentEdge(n, -1);
    std::vector<char> settled(n, 0);

    // 禁用集合同样转成按索引的标记数组。
    // 转换是 O(|bans|) 的一次性开销，换掉的是每条边松弛时的哈希查找 ——
    // Yen 会把 Dijkstra 调用成千上万次，这笔账很划算。
    std::vector<char> nodeBanned;
    if (!bans.nodes.empty()) {
        nodeBanned.assign(n, 0);
        for (NodeId b : bans.nodes) {
            const int bi = g.nodeIndex(b);
            if (bi >= 0) {
                nodeBanned[static_cast<std::size_t>(bi)] = 1;
            }
        }
    }
    std::vector<char> edgeBanned;
    if (!bans.edges.empty()) {
        edgeBanned.assign(g.edgeCount(), 0);
        for (EdgeId b : bans.edges) {
            if (b >= 0 && static_cast<std::size_t>(b) < g.edgeCount()) {
                edgeBanned[static_cast<std::size_t>(b)] = 1;
            }
        }
    }

    // 小根堆：(累计时延, 节点索引)。
    // 索引作为次级比较键，保证时延相同时的出堆顺序确定、结果可复现。
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;

    dist[static_cast<std::size_t>(srcIdx)] = 0.0;
    pq.emplace(0.0, srcIdx);

    while (!pq.empty()) {
        const auto [cost, u] = pq.top();
        pq.pop();
        const std::size_t ui = static_cast<std::size_t>(u);

        // 惰性删除：同一个节点可能以不同代价多次入堆，
        // 出堆时若已定型或代价已过期就跳过。
        // 这比"支持 decrease-key 的堆"实现简单得多，代价是堆里有冗余项，
        // 但冗余项总数受边数限制，复杂度仍是 O(E log E)。
        if (settled[ui] || cost > dist[ui]) {
            continue;
        }
        settled[ui] = 1;

        if (u == dstIdx) {
            break;  // 目标已定型，最短路确定
        }

        for (const Adj& a : g.adjacencyByIndex(u)) {
            const std::size_t vi = static_cast<std::size_t>(a.nodeIdx);
            if (settled[vi]) {
                continue;
            }
            if (!nodeBanned.empty() && nodeBanned[vi]) {
                continue;
            }
            if (!edgeBanned.empty() &&
                edgeBanned[static_cast<std::size_t>(a.edge)]) {
                continue;
            }
            const double nd = cost + g.edge(a.edge).weight;
            if (nd < dist[vi]) {
                dist[vi] = nd;
                parentNode[vi] = u;
                parentEdge[vi] = a.edge;
                pq.emplace(nd, a.nodeIdx);
            }
        }
    }

    if (!settled[static_cast<std::size_t>(dstIdx)]) {
        return {};  // 不可达
    }

    // 从终点回溯出边序列，再反转
    std::vector<EdgeId> path;
    int cur = dstIdx;
    while (cur != srcIdx) {
        const std::size_t ci = static_cast<std::size_t>(cur);
        if (parentNode[ci] < 0) {
            return {};  // 理论上不会发生，防御性返回
        }
        path.push_back(parentEdge[ci]);
        cur = parentNode[ci];
    }
    std::reverse(path.begin(), path.end());
    return path;
}

// ── Yen's K 最短路 ──────────────────────────────────────────────────────────

std::vector<std::vector<EdgeId>> yenKShortestPaths(const Graph& g, NodeId src,
                                                   NodeId dst, int K) {
    std::vector<std::vector<EdgeId>> A;  // 已确定的 K 短路
    if (K <= 0 || src == dst) {
        return A;
    }

    const std::vector<EdgeId> first = dijkstra(g, src, dst);
    if (first.empty()) {
        return A;  // 不可达
    }
    A.push_back(first);

    // 候选集 B。用 set 而不是 priority_queue：
    // 需要同时支持"取最小"和"判重"，set 两者都是 O(log n)，
    // 而堆没法高效判重（只能另配一个 set，两份数据要同步维护）。
    // 比较键把代价放首位，路径本身作次级键保证顺序确定。
    using Candidate = std::pair<double, std::vector<EdgeId>>;
    std::set<Candidate> B;

    for (int k = 1; k < K; ++k) {
        const std::vector<EdgeId>& prevPath = A[static_cast<std::size_t>(k - 1)];
        const std::vector<NodeId> prevNodes = g.edgesToNodes(prevPath, src);
        if (prevNodes.empty()) {
            break;  // 上一条路径本身就不连续，不应发生
        }

        for (std::size_t i = 0; i < prevPath.size(); ++i) {
            // spurNode 是第 i 条边的起点；
            // rootPath 是**前 i 条边**，恰好从 src 走到 spurNode。
            // 原型这里取了 prevPath[:i+1]，走到的是 prevNodes[i+1]，
            // 与 spurNode 不衔接，导致后续拼接失败被静默丢弃。
            const NodeId spurNode = prevNodes[i];
            const std::vector<EdgeId> rootPath(prevPath.begin(),
                                               prevPath.begin() +
                                                   static_cast<std::ptrdiff_t>(i));

            Bans bans;

            // 1) 禁用所有"root 部分相同"的已知路径的下一条边。
            //    否则 Dijkstra 会重新走出一条已经在 A 里的路径。
            for (const auto& p : A) {
                if (p.size() > i &&
                    std::equal(rootPath.begin(), rootPath.end(), p.begin())) {
                    bans.edges.insert(p[i]);
                }
            }
            // 候选集里已有的也要禁，避免产出重复候选
            for (const auto& c : B) {
                const auto& p = c.second;
                if (p.size() > i &&
                    std::equal(rootPath.begin(), rootPath.end(), p.begin())) {
                    bans.edges.insert(p[i]);
                }
            }

            // 2) 禁用 rootPath 上除 spurNode 外的全部节点，保证拼出来的路径无环。
            //    注意**不能**把 spurNode 自己禁掉 —— 它是 spur 搜索的起点。
            for (std::size_t j = 0; j < i; ++j) {
                bans.nodes.insert(prevNodes[j]);
            }

            const std::vector<EdgeId> spurPath = dijkstra(g, spurNode, dst, bans);
            if (spurPath.empty()) {
                continue;
            }

            std::vector<EdgeId> total = rootPath;
            total.insert(total.end(), spurPath.begin(), spurPath.end());

            // 去环校验。禁用节点已经排除了大部分成环情况，
            // 这里再验一次是廉价的保险 —— 路径不连续或有重复节点都不接受。
            const std::vector<NodeId> totalNodes = g.edgesToNodes(total, src);
            if (totalNodes.empty()) {
                continue;
            }
            std::unordered_set<NodeId> uniq(totalNodes.begin(), totalNodes.end());
            if (uniq.size() != totalNodes.size()) {
                continue;
            }

            B.emplace(g.pathWeight(total), std::move(total));
        }

        if (B.empty()) {
            break;  // 没有更多候选，K 短路不足 K 条
        }
        A.push_back(B.begin()->second);
        B.erase(B.begin());
    }

    return A;
}

}  // namespace thgh
