#include "planner/static_planner.h"

#include "planner/geo.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <sstream>
#include <unordered_map>

namespace thgh {
namespace {

// 各通信方式的带宽。数值沿用算法侧原型的取值。
const std::map<std::string, double>& bandwidthMap() {
    static const std::map<std::string, double> kMap = {
        {"fiber", 10e9},   {"copper", 100e6},    {"microwave", 50e6},
        {"scatter", 10e6}, {"satellite", 2e6},   {"shortwave", 64e3},
        {"manet", 25e6},   {"4G5G", 100e6},
    };
    return kMap;
}

const std::map<std::string, std::string>& displayMap() {
    static const std::map<std::string, std::string> kMap = {
        {"fiber", "光缆"},      {"copper", "野战电缆"},
        {"microwave", "微波接力"}, {"scatter", "超视距微波"},
        {"satellite", "卫星通信"}, {"shortwave", "短波电台"},
        {"manet", "自组网电台"},   {"adhoc", "自组网电台"},
        {"4G5G", "移动公网"},
    };
    return kMap;
}

bool isBackbone(const std::string& t) {
    return t == "干线" || t == "backbone";
}
bool isAccess(const std::string& t) { return t == "支线" || t == "access"; }

// 格式化成固定小数位，避免不同平台 printf 的默认精度差异
std::string fixed(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return buf;
}

}  // namespace

double bandwidthForMethod(const std::string& method) {
    const auto& m = bandwidthMap();
    auto it = m.find(method);
    // 未知方式回退到 50Mbps：宁可给个保守的默认值，
    // 也不要因为配置里多了个没见过的方式就整条链路建不起来
    return it == m.end() ? 50e6 : it->second;
}

std::string displayNameForMethod(const std::string& method) {
    const auto& m = displayMap();
    auto it = m.find(method);
    return it == m.end() ? method : it->second;
}

// ── 构造 ────────────────────────────────────────────────────────────────────

StaticPlanner::StaticPlanner(std::vector<PlanNode> nodes,
                             std::vector<PlanFlow> flows, PlanConfig cfg)
    : m_nodes(std::move(nodes)), m_flows(std::move(flows)), m_cfg(cfg) {}

// ── 进度报告 ────────────────────────────────────────────────────────────────

void StaticPlanner::report(int percent, const std::string& stage) {
    if (!m_progress) {
        return;
    }
    // 百分比必须单调不减 —— 进度条往回退比没有进度条更糟，
    // 用户会以为出错了。这里在源头上钳住，而不是指望每个调用点都算对。
    if (percent < m_lastPercent) {
        percent = m_lastPercent;
    }
    if (percent > 100) {
        percent = 100;
    }
    m_lastPercent = percent;
    m_progress(percent, stage);
}

// ── 拓扑构建 ────────────────────────────────────────────────────────────────

void StaticPlanner::buildTopology() {
    m_graph.clear();
    m_edgeMeta.clear();

    std::vector<const PlanNode*> backbone;
    std::vector<const PlanNode*> access;
    for (const PlanNode& n : m_nodes) {
        if (isBackbone(n.nodeType)) {
            backbone.push_back(&n);
        } else if (isAccess(n.nodeType)) {
            access.push_back(&n);
        }
        // 其它类型不参与建链，与原型行为一致
    }

    // 干线之间两两互联
    for (std::size_t i = 0; i < backbone.size(); ++i) {
        for (std::size_t j = i + 1; j < backbone.size(); ++j) {
            connectIfPossible(*backbone[i], *backbone[j]);
        }
    }
    // 支线只接入干线，支线之间不直连（分层组网的典型结构）
    for (const PlanNode* a : access) {
        for (const PlanNode* b : backbone) {
            connectIfPossible(*a, *b);
        }
    }
}

void StaticPlanner::connectIfPossible(const PlanNode& a, const PlanNode& b) {
    const double distM = distanceMeters(a.longitude, a.latitude, a.altitude,
                                        b.longitude, b.latitude, b.altitude);
    if (distM > m_cfg.commRangeKm * 1000.0) {
        return;  // 超出通信距离
    }

    // 求两端共同支持的通信方式。
    // ★ 用有序容器求交集，而不是像原型那样遍历 Python 的 set ——
    //   set 的迭代顺序不确定，会导致**同样的输入产生不同的链路 id 分配**，
    //   进而让规划结果不可复现。可复现性对一个规划系统是硬要求：
    //   同样的场景重跑必须给出同样的方案，否则没法核对、没法追责。
    std::vector<std::string> methodsA = a.commMethods;
    std::vector<std::string> methodsB = b.commMethods;
    std::sort(methodsA.begin(), methodsA.end());
    std::sort(methodsB.begin(), methodsB.end());
    methodsA.erase(std::unique(methodsA.begin(), methodsA.end()), methodsA.end());
    methodsB.erase(std::unique(methodsB.begin(), methodsB.end()), methodsB.end());

    std::vector<std::string> common;
    std::set_intersection(methodsA.begin(), methodsA.end(), methodsB.begin(),
                          methodsB.end(), std::back_inserter(common));
    if (common.empty()) {
        return;  // 没有共同通信方式，物理上连不上
    }

    const double delay = propagationDelaySec(distM);

    // 每种共同方式都建一条独立链路 —— 这正是需要边级图的原因：
    // 同一对节点之间可能同时有光缆和微波，两条边的带宽差 200 倍
    for (const std::string& method : common) {
        const double bw = bandwidthForMethod(method);
        EdgeMeta meta;
        if (method == "fiber") {
            meta.linkType = "wired";
            meta.wirelessType.clear();
        } else {
            meta.linkType = "wireless";
            meta.wirelessType = method;
        }
        meta.method = method;

        m_graph.addEdge(a.id, b.id, delay, bw);
        m_edgeMeta.push_back(std::move(meta));
    }
}

// ── 流量分配 ────────────────────────────────────────────────────────────────

PlanResult StaticPlanner::allocateFlows() {
    PlanResult result;

    // 每条链路的剩余带宽
    std::vector<double> availableBw;
    availableBw.reserve(m_graph.edgeCount());
    for (const Edge& e : m_graph.edges()) {
        availableBw.push_back(e.capacity);
    }

    // 每条链路上承载的流
    std::vector<std::vector<LinkFlowResult>> flowsOnLink(m_graph.edgeCount());

    // 一条流被拆分到的若干路径
    struct AllocatedPath {
        std::vector<EdgeId> edges;
        std::vector<NodeId> nodes;
        int hops = 0;
        double bandwidthBps = 0.0;
    };
    struct FlowOutcome {
        bool satisfied = false;
        std::vector<AllocatedPath> paths;
        std::string reason;
    };
    std::unordered_map<int, FlowOutcome> outcomes;

    // 按 QoS 等级升序（高优先级先分配）、同级按速率降序。
    // 速率大的先分配是个启发式：大流更难找到足够带宽的路径，
    // 先安排能减少"大流被小流挤得无路可走"的情况。
    std::vector<const PlanFlow*> sorted;
    sorted.reserve(m_flows.size());
    for (const PlanFlow& f : m_flows) {
        sorted.push_back(&f);
    }
    // stable_sort：同 QoS 同速率时保持输入顺序，保证结果可复现
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const PlanFlow* a, const PlanFlow* b) {
                         if (a->qosLevel != b->qosLevel) {
                             return a->qosLevel < b->qosLevel;
                         }
                         return a->rateBps > b->rateBps;
                     });

    // 流量分配占进度的 10%~90%。剩下的 10% 留给建拓扑、最后 10% 留给汇总输出 ——
    // 让进度条在"看起来还有事要做"的时候不要提前冲到 100%。
    const std::size_t totalToAllocate = sorted.size();
    std::size_t done = 0;

    for (const PlanFlow* flow : sorted) {
        double remaining = flow->rateBps;

        const auto paths =
            yenKShortestPaths(m_graph, flow->src, flow->dst, m_cfg.pathsPerFlow);

        FlowOutcome outcome;
        if (paths.empty()) {
            outcome.satisfied = false;
            outcome.reason = "无可达路径";
            outcomes[flow->fid] = std::move(outcome);
            ++done;
            report(static_cast<int>(10 + 80 * done / totalToAllocate),
                   "分配业务流带宽");
            continue;
        }

        // 多路径分摊：按时延从小到大依次填，每条路径能给多少给多少。
        // 瓶颈是路径上剩余带宽的最小值（木桶效应）。
        for (const auto& edges : paths) {
            if (remaining <= 0.0) {
                break;
            }
            double minAvail = std::numeric_limits<double>::infinity();
            for (EdgeId e : edges) {
                minAvail = std::min(minAvail, availableBw[static_cast<std::size_t>(e)]);
            }
            const double alloc = std::min(remaining, minAvail);
            if (alloc <= 0.0) {
                continue;
            }

            for (EdgeId e : edges) {
                availableBw[static_cast<std::size_t>(e)] -= alloc;
                flowsOnLink[static_cast<std::size_t>(e)].push_back(
                    LinkFlowResult{flow->fid, alloc, "主用流"});
            }

            AllocatedPath ap;
            ap.edges = edges;
            ap.nodes = m_graph.edgesToNodes(edges, flow->src);
            ap.hops = static_cast<int>(ap.nodes.size()) - 1;
            ap.bandwidthBps = alloc;
            outcome.paths.push_back(std::move(ap));

            remaining -= alloc;
        }

        // 剩余不到原始需求的 1% 即认为已满足 ——
        // 浮点扣减会有累积误差，要求严格等于 0 不现实
        if (remaining < m_cfg.satisfiedTolerance * flow->rateBps) {
            outcome.satisfied = true;
        } else {
            outcome.satisfied = false;
            outcome.reason =
                "所有路径耗尽，仍有 " + fixed(remaining, 0) + " bps 无法满足";
        }
        outcomes[flow->fid] = std::move(outcome);

        ++done;
        report(static_cast<int>(10 + 80 * done / totalToAllocate),
               "分配业务流带宽");
    }

    report(90, "汇总规划结果");

    // ── 输出链路（含无流量的，界面要画出完整拓扑）────────────────────
    result.links.reserve(m_graph.edgeCount());
    for (std::size_t i = 0; i < m_graph.edgeCount(); ++i) {
        const Edge& e = m_graph.edge(static_cast<EdgeId>(i));
        const EdgeMeta& meta = m_edgeMeta[i];
        LinkResult lr;
        lr.linkId = static_cast<int>(i);
        lr.srcNodeId = e.u;
        lr.dstNodeId = e.v;
        lr.linkType = meta.linkType;
        lr.wirelessType = meta.wirelessType;
        lr.deviceDisplayName = displayNameForMethod(meta.method);
        lr.bandwidthBps = e.capacity;
        lr.propDelayS = e.weight;
        lr.flows = flowsOnLink[i];
        result.links.push_back(std::move(lr));
    }

    // ── 输出规划结果与告警 ──────────────────────────────────────────
    // 告警分三段顺序追加，与协议约定一致：
    //   ① summary（整体概览）② 每条成功流 ③ 每条失败流
    std::vector<WarnItem> successWarns;
    std::vector<WarnItem> errorWarns;
    int satisfiedCount = 0;

    // 按**输入顺序**而不是分配顺序输出，便于客户端对照
    for (const PlanFlow& flow : m_flows) {
        auto it = outcomes.find(flow.fid);
        const bool ok = it != outcomes.end() && it->second.satisfied;

        if (ok) {
            ++satisfiedCount;
            std::ostringstream seg;
            bool firstSeg = true;
            for (const auto& p : it->second.paths) {
                PlanPathResult pr;
                pr.fid = flow.fid;
                pr.srcNodeId = flow.src;
                pr.dstNodeId = flow.dst;
                pr.isSatisfied = true;
                pr.pathNodes = p.nodes;
                pr.hops = p.hops;
                pr.pathLinks = p.edges;
                pr.actualBandwidthBps = p.bandwidthBps;
                result.planResults.push_back(std::move(pr));

                if (!firstSeg) {
                    seg << "；";
                }
                firstSeg = false;
                for (std::size_t k = 0; k < p.nodes.size(); ++k) {
                    if (k > 0) {
                        seg << " → ";
                    }
                    seg << "节点" << p.nodes[k];
                }
                seg << "（" << p.hops << " 跳，"
                    << fixed(p.bandwidthBps / 1e6, 2) << " Mbps）";
            }
            successWarns.push_back(
                WarnItem{"info",
                         "规划流 fid=" + std::to_string(flow.fid) +
                             " 已成功路由：" +
                             (it->second.paths.empty() ? "无路径" : seg.str()),
                         "flow_success", flow.fid});
        } else {
            const std::string reason =
                it != outcomes.end() ? it->second.reason : "规划失败";
            PlanPathResult pr;
            pr.fid = flow.fid;
            pr.srcNodeId = flow.src;
            pr.dstNodeId = flow.dst;
            pr.isSatisfied = false;
            pr.hops = -1;
            pr.actualBandwidthBps = 0.0;
            pr.unsatisfiedReason = reason;
            result.planResults.push_back(std::move(pr));

            errorWarns.push_back(
                WarnItem{"error",
                         "规划流 fid=" + std::to_string(flow.fid) +
                             " 无法满足：" + reason,
                         "unsatisfied_plan", flow.fid});
        }
    }

    const int totalFlows = static_cast<int>(m_flows.size());
    int activeLinks = 0;
    for (const LinkResult& lr : result.links) {
        if (!lr.flows.empty()) {
            ++activeLinks;
        }
    }

    WarnItem summary;
    summary.level = "info";
    summary.warnType = "summary";
    summary.relatedFid = -1;
    summary.message = "规划完成：共 " + std::to_string(totalFlows) + " 条流，" +
                      std::to_string(satisfiedCount) + " 条满足、" +
                      std::to_string(totalFlows - satisfiedCount) +
                      " 条未满足；活跃链路 " + std::to_string(activeLinks) +
                      "/" + std::to_string(result.links.size()) + " 条";

    result.warns.push_back(std::move(summary));
    result.warns.insert(result.warns.end(), successWarns.begin(),
                        successWarns.end());
    result.warns.insert(result.warns.end(), errorWarns.begin(), errorWarns.end());

    return result;
}

PlanResult StaticPlanner::plan() {
    const auto t0 = std::chrono::steady_clock::now();
    m_lastPercent = -1;

    report(0, "开始规划");
    buildTopology();
    report(10, "构建网络拓扑");

    PlanResult r = allocateFlows();
    report(100, "规划完成");

    r.elapsedSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    // 耗时补进 summary（它是第一条 warn）
    if (!r.warns.empty()) {
        r.warns[0].message += "；耗时 " + fixed(r.elapsedSec, 3) + " s";
    }
    return r;
}

}  // namespace thgh
