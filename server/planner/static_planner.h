#ifndef THGH_SERVER_PLANNER_STATIC_PLANNER_H
#define THGH_SERVER_PLANNER_STATIC_PLANNER_H

// ─────────────────────────────────────────────────────────────────────────────
// StaticPlanner —— 静态通信网络规划
//
// 三步：
//   1. 建拓扑：按通信距离与共同通信方式，决定哪些节点对之间能建链
//   2. 算权重：由 ECEF 距离推出每条链路的传播时延，作为最短路的边权
//   3. 分配流量：按 QoS 优先级依次为每条业务流选路并扣减带宽
//
// ── 关于本实现与算法侧 Python 原型的关系 ──────────────────────────────────
// 算法方案由算法侧以 Python 原型给出，本文件是它的 C++ 实现。
// 移植过程中修正了原型的两处缺陷（都在下面的注释里标明了）：
//   * Yen K 短路的偏离点取法有 off-by-one，导致产出的不是真正的 K 短路
//   * 遍历 Python set 导致链路 id 分配顺序不确定，同样输入可能得到不同结果
// 这些修正都用暴力枚举做了验证（见 tests/test_graph.cpp）。
//
// ── 为什么服务端要用 C++ 重写而不是转发给 Python ──────────────────────────
// 若把规划计算转发给 Python 子进程，服务端就退化成一个纯 IO 转发器 ——
// 业务逻辑层是空的，而且部署时要额外维护 Python 运行时与依赖。
// 用 C++ 实现之后服务端是完整的四层：网络 → 协议 → 业务计算 → 数据。
// ─────────────────────────────────────────────────────────────────────────────

#include "planner/graph.h"

#include <functional>
#include <string>
#include <vector>

namespace thgh {

// ── 输入 ────────────────────────────────────────────────────────────────────

struct PlanNode {
    NodeId id = 0;
    std::string nodeType;   // "干线"/"支线"（兼容旧的 "backbone"/"access"）
    double longitude = 0.0; // 度
    double latitude = 0.0;
    double altitude = 0.0;  // 米
    // 该节点支持的通信方式，如 {"fiber","microwave"}
    std::vector<std::string> commMethods;
};

struct PlanFlow {
    int fid = 0;
    NodeId src = 0;
    NodeId dst = 0;
    double rateBps = 0.0;
    // QoS 等级，数值越小优先级越高。缺省 2（最低档）。
    int qosLevel = 2;
};

struct PlanConfig {
    // 最大通信距离，超过则无法建链（公里）
    double commRangeKm = 150.0;
    // 每条流最多尝试几条路径（Yen 的 K）
    int pathsPerFlow = 4;
    // 剩余需求低于原始速率的这个比例即认为已满足
    double satisfiedTolerance = 0.01;
};

// ── 输出（字段与协议层保持一致，见 protocol/message.h）────────────────────

struct LinkFlowResult {
    int fid = 0;
    double bandwidthBps = 0.0;
    std::string description;
};

struct LinkResult {
    int linkId = 0;
    NodeId srcNodeId = 0;
    NodeId dstNodeId = 0;
    std::string linkType;      // "wired" / "wireless"
    std::string wirelessType;  // 无线方式，有线为空
    std::string deviceDisplayName;
    double bandwidthBps = 0.0;
    double propDelayS = 0.0;
    std::vector<LinkFlowResult> flows;
};

struct PlanPathResult {
    int fid = 0;
    NodeId srcNodeId = 0;
    NodeId dstNodeId = 0;
    bool isSatisfied = false;
    std::vector<NodeId> pathNodes;
    int hops = -1;
    std::vector<EdgeId> pathLinks;
    double actualBandwidthBps = 0.0;
    std::string unsatisfiedReason;
};

struct WarnItem {
    std::string level;     // "info" / "warn" / "error"
    std::string message;
    std::string warnType;  // "summary" / "flow_success" / "unsatisfied_plan"
    int relatedFid = -1;
};

struct PlanResult {
    std::vector<LinkResult> links;
    std::vector<PlanPathResult> planResults;
    std::vector<WarnItem> warns;
    double elapsedSec = 0.0;
};

// 各通信方式的默认带宽（bps）。未列出的方式回退到 50Mbps。
double bandwidthForMethod(const std::string& method);
// 通信方式的中文显示名
std::string displayNameForMethod(const std::string& method);

// 进度回调。percent 单调不减，stage 是阶段名。
//
// ── 为什么 planner 只报事实、不做节流 ────────────────────────────────────
// 200 条流就会回调 200 次。要不要每次都往网络上发，是**传输层**关心的事：
// 取决于对端读得多快、输出缓冲堆了多少。planner 不知道也不该知道这些。
// 所以这里如实回调，节流由调用方（PlanService）决定。
// 回调在调用 plan() 的那个线程内同步执行，本身很便宜。
using ProgressCallback = std::function<void(int percent, const std::string& stage)>;

class StaticPlanner {
public:
    StaticPlanner(std::vector<PlanNode> nodes, std::vector<PlanFlow> flows,
                  PlanConfig cfg = PlanConfig{});

    // 设置进度回调。不设则不报告进度（单测走这条路径，不受影响）。
    void setProgressCallback(ProgressCallback cb) { m_progress = std::move(cb); }

    PlanResult plan();

    // 供测试检查中间结果
    const Graph& graph() const { return m_graph; }

private:
    void report(int percent, const std::string& stage);

    void buildTopology();
    void connectIfPossible(const PlanNode& a, const PlanNode& b);
    PlanResult allocateFlows();

    std::vector<PlanNode> m_nodes;
    std::vector<PlanFlow> m_flows;
    PlanConfig m_cfg;
    ProgressCallback m_progress;
    int m_lastPercent = -1;  // 保证百分比单调不减

    Graph m_graph;
    // 与 m_graph 中边 id 一一对应的元信息
    struct EdgeMeta {
        std::string linkType;
        std::string wirelessType;
        std::string method;
    };
    std::vector<EdgeMeta> m_edgeMeta;
};

}  // namespace thgh

#endif  // THGH_SERVER_PLANNER_STATIC_PLANNER_H
