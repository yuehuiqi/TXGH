// 坐标转换与静态规划的单元测试
//
// geo 部分用**可独立查证的基准值**校验（椭球极半径、赤道点坐标、
// 一度经差随纬度的收缩比），而不是拿实现自己的输出当期望值 ——
// 那种"测试"只能证明代码没改过，证明不了它算得对。
//
// planner 部分验证的是规划的**契约**：可复现、带宽守恒、优先级生效、
// 输出结构完整。这些性质与具体数值无关，不会因为参数微调就失效。

#include "planner/geo.h"
#include "planner/static_planner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace thgh;

// ── WGS84 → ECEF ────────────────────────────────────────────────────────────

TEST(Geo, EquatorPrimeMeridianEqualsSemiMajorAxis) {
    // 赤道 + 本初子午线是 ECEF 的 X 轴与椭球面的交点，坐标应为 (a, 0, 0)
    const EcefPoint p = geodeticToEcef(0.0, 0.0, 0.0);
    EXPECT_NEAR(p.x, wgs84::kSemiMajorAxis, 1e-6);
    EXPECT_NEAR(p.y, 0.0, 1e-6);
    EXPECT_NEAR(p.z, 0.0, 1e-6);
}

TEST(Geo, NorthPoleEqualsSemiMinorAxis) {
    // 北极点的 z 应等于短半轴 b = a(1-f) = 6356752.314245...
    // 这个值可以独立查证（WGS84 定义值），不是从实现里抄来的
    const double b = wgs84::kSemiMajorAxis * (1.0 - wgs84::kFlattening);
    EXPECT_NEAR(b, 6356752.314245, 1e-6) << "椭球参数本身就不对";

    const EcefPoint p = geodeticToEcef(0.0, 90.0, 0.0);
    EXPECT_NEAR(p.z, b, 1e-6);
    EXPECT_NEAR(std::hypot(p.x, p.y), 0.0, 1e-6);
}

TEST(Geo, NinetyDegreesEastLiesOnYAxis) {
    const EcefPoint p = geodeticToEcef(90.0, 0.0, 0.0);
    EXPECT_NEAR(p.x, 0.0, 1e-6);
    EXPECT_NEAR(p.y, wgs84::kSemiMajorAxis, 1e-6);
}

TEST(Geo, AltitudeAddsAlongTheNormal) {
    // 赤道上抬高 1000m，距地心应恰好多 1000m
    const EcefPoint ground = geodeticToEcef(30.0, 0.0, 0.0);
    const EcefPoint up = geodeticToEcef(30.0, 0.0, 1000.0);
    const double r0 = std::sqrt(ground.x * ground.x + ground.y * ground.y +
                                ground.z * ground.z);
    const double r1 = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
    EXPECT_NEAR(r1 - r0, 1000.0, 1e-6);
}

TEST(Geo, OneDegreeOfLatitudeIsAboutOneEleventhOfAThousandKm) {
    // 一度纬差约 110.6~111.7 km（随纬度略变）
    const double d = distanceMeters(116.0, 39.0, 0.0, 116.0, 40.0, 0.0);
    EXPECT_GT(d, 110000.0);
    EXPECT_LT(d, 112000.0);
}

TEST(Geo, OneDegreeOfLongitudeShrinksWithLatitude) {
    // ★ 这条正是"不能直接对经纬度做欧氏距离"的证据：
    //   同样 1 度经差，在北纬 60 度约为赤道的 cos(60°)=0.5 倍
    const double atEquator = distanceMeters(0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    const double at60N = distanceMeters(0.0, 60.0, 0.0, 1.0, 60.0, 0.0);
    EXPECT_NEAR(at60N / atEquator, 0.5, 0.01)
        << "赤道 " << atEquator << "m，北纬60度 " << at60N << "m";
}

TEST(Geo, PureAltitudeDifferenceIsTheHeightDifference) {
    // 同一经纬度、只有高度差时，三维直线距离就是高度差本身。
    // Haversine 这里会算出 0 —— 这就是不能用它的原因。
    const double d = distanceMeters(116.4, 39.9, 0.0, 116.4, 39.9, 500.0);
    EXPECT_NEAR(d, 500.0, 1e-6);
}

TEST(Geo, DistanceIsSymmetricAndZeroForSamePoint) {
    const double ab = distanceMeters(116.4, 39.9, 50.0, 121.5, 31.2, 10.0);
    const double ba = distanceMeters(121.5, 31.2, 10.0, 116.4, 39.9, 50.0);
    EXPECT_DOUBLE_EQ(ab, ba);
    EXPECT_DOUBLE_EQ(distanceMeters(116.4, 39.9, 50.0, 116.4, 39.9, 50.0), 0.0);
}

TEST(Geo, PropagationDelayMatchesSpeedOfLight) {
    EXPECT_NEAR(propagationDelaySec(3e8), 1.0, 1e-12);
    // 300km 光缆约 1ms
    EXPECT_NEAR(propagationDelaySec(300e3), 1e-3, 1e-12);
}

// ── 带宽表 ──────────────────────────────────────────────────────────────────

TEST(Bandwidth, KnownMethodsUseTheirRatedCapacity) {
    EXPECT_DOUBLE_EQ(bandwidthForMethod("fiber"), 10e9);
    EXPECT_DOUBLE_EQ(bandwidthForMethod("satellite"), 2e6);
    EXPECT_DOUBLE_EQ(bandwidthForMethod("shortwave"), 64e3);
}

TEST(Bandwidth, UnknownMethodFallsBackInsteadOfFailing) {
    // 配置里出现没见过的通信方式时，宁可给个保守默认值也不要整条链路建不起来
    EXPECT_DOUBLE_EQ(bandwidthForMethod("laser-2077"), 50e6);
    EXPECT_EQ(displayNameForMethod("laser-2077"), "laser-2077");
}

// ── 拓扑构建 ────────────────────────────────────────────────────────────────

namespace {

// 两个相距约 11km 的干线节点（0.1 度纬差）
std::vector<PlanNode> twoBackbones(std::vector<std::string> a,
                                   std::vector<std::string> b) {
    return {
        PlanNode{1, "干线", 116.0, 39.0, 0.0, std::move(a)},
        PlanNode{2, "干线", 116.0, 39.1, 0.0, std::move(b)},
    };
}

// 找出连接指定两个节点的全部链路
std::vector<const LinkResult*> linksBetween(const PlanResult& r, NodeId x,
                                            NodeId y) {
    std::vector<const LinkResult*> out;
    for (const LinkResult& l : r.links) {
        if ((l.srcNodeId == x && l.dstNodeId == y) ||
            (l.srcNodeId == y && l.dstNodeId == x)) {
            out.push_back(&l);
        }
    }
    return out;
}

}  // namespace

TEST(Topology, ConnectsBackbonesWithCommonMethod) {
    StaticPlanner p(twoBackbones({"fiber", "microwave"}, {"fiber"}), {});
    const PlanResult r = p.plan();
    const auto links = linksBetween(r, 1, 2);
    ASSERT_EQ(links.size(), 1u) << "只有 fiber 是共同方式";
    EXPECT_EQ(links[0]->linkType, "wired");
    EXPECT_TRUE(links[0]->wirelessType.empty());
    EXPECT_DOUBLE_EQ(links[0]->bandwidthBps, 10e9);
}

TEST(Topology, NoCommonMethodMeansNoLink) {
    StaticPlanner p(twoBackbones({"fiber"}, {"satellite"}), {});
    EXPECT_TRUE(p.plan().links.empty()) << "没有共同通信方式，物理上连不上";
}

TEST(Topology, CreatesOneLinkPerCommonMethod) {
    // ★ 需要边级图的直接证据：同一对节点之间两种共同方式 = 两条独立链路
    StaticPlanner p(twoBackbones({"fiber", "microwave", "satellite"},
                                 {"fiber", "microwave"}),
                    {});
    const PlanResult r = p.plan();
    const auto links = linksBetween(r, 1, 2);
    ASSERT_EQ(links.size(), 2u);

    std::set<double> caps;
    for (const LinkResult* l : links) {
        caps.insert(l->bandwidthBps);
    }
    EXPECT_EQ(caps, (std::set<double>{10e9, 50e6})) << "两条链路带宽应各自独立";
}

TEST(Topology, RespectsCommunicationRange) {
    // 两点约 11km。把通信距离压到 5km，链路应该建不起来
    PlanConfig cfg;
    cfg.commRangeKm = 5.0;
    StaticPlanner near(twoBackbones({"fiber"}, {"fiber"}), {}, cfg);
    EXPECT_TRUE(near.plan().links.empty());

    cfg.commRangeKm = 50.0;
    StaticPlanner far(twoBackbones({"fiber"}, {"fiber"}), {}, cfg);
    EXPECT_EQ(far.plan().links.size(), 1u);
}

TEST(Topology, AccessNodesConnectOnlyToBackbone) {
    // 分层组网：支线只接干线，支线之间不直连
    std::vector<PlanNode> nodes = {
        PlanNode{1, "干线", 116.00, 39.00, 0.0, {"fiber"}},
        PlanNode{2, "支线", 116.01, 39.00, 0.0, {"fiber"}},
        PlanNode{3, "支线", 116.02, 39.00, 0.0, {"fiber"}},
    };
    StaticPlanner p(std::move(nodes), {});
    const PlanResult r = p.plan();

    EXPECT_EQ(linksBetween(r, 1, 2).size(), 1u);
    EXPECT_EQ(linksBetween(r, 1, 3).size(), 1u);
    EXPECT_TRUE(linksBetween(r, 2, 3).empty()) << "支线之间不应直连";
}

TEST(Topology, LinkDelayComesFromDistance) {
    StaticPlanner p(twoBackbones({"fiber"}, {"fiber"}), {});
    const PlanResult r = p.plan();
    ASSERT_EQ(r.links.size(), 1u);
    // 约 11km / 3e8 ≈ 37us
    EXPECT_GT(r.links[0].propDelayS, 3e-5);
    EXPECT_LT(r.links[0].propDelayS, 4e-5);
}

TEST(Topology, UnknownNodeTypesAreIgnored) {
    std::vector<PlanNode> nodes = {
        PlanNode{1, "干线", 116.00, 39.00, 0.0, {"fiber"}},
        PlanNode{2, "观察哨", 116.01, 39.00, 0.0, {"fiber"}},
    };
    StaticPlanner p(std::move(nodes), {});
    EXPECT_TRUE(p.plan().links.empty());
}

// ── 流量分配 ────────────────────────────────────────────────────────────────

namespace {

// 三个共线的干线节点，相邻间隔约 43km、首尾约 86km。
//
// ⚠️ 注意：干线之间是**两两互联**的，所以默认配置（150km）下 1、3 也会直连，
//    拓扑是个三角形而不是一条链。写测试时踩过这个坑 ——
//    原以为流 1→3 会走两跳，实际走了直连的一跳；
//    原以为单条链路会拥塞，实际有两条路径分摊，容量翻倍。
//    需要真正的链式拓扑时用下面的 chainConfig()。
std::vector<PlanNode> threeBackbones(const std::vector<std::string>& methods) {
    return {
        PlanNode{1, "干线", 116.0, 39.00, 0.0, methods},
        PlanNode{2, "干线", 116.5, 39.00, 0.0, methods},
        PlanNode{3, "干线", 117.0, 39.00, 0.0, methods},
    };
}

// 通信距离压到 60km：相邻(43km)连得上、首尾(86km)连不上，
// 拓扑退化成真正的链 1—2—3。
PlanConfig chainConfig() {
    PlanConfig c;
    c.commRangeKm = 60.0;
    return c;
}

const PlanPathResult* findPath(const PlanResult& r, int fid) {
    for (const PlanPathResult& p : r.planResults) {
        if (p.fid == fid) {
            return &p;
        }
    }
    return nullptr;
}

int countWarns(const PlanResult& r, const std::string& type) {
    return static_cast<int>(std::count_if(
        r.warns.begin(), r.warns.end(),
        [&type](const WarnItem& w) { return w.warnType == type; }));
}

}  // namespace

TEST(Allocation, SatisfiesFlowWithinCapacity) {
    // 链式拓扑，1→3 必须经过 2
    std::vector<PlanFlow> flows = {PlanFlow{100, 1, 3, 1e6, 1}};
    StaticPlanner p(threeBackbones({"fiber"}), flows, chainConfig());
    const PlanResult r = p.plan();

    const PlanPathResult* path = findPath(r, 100);
    ASSERT_NE(path, nullptr);
    EXPECT_TRUE(path->isSatisfied);
    EXPECT_DOUBLE_EQ(path->actualBandwidthBps, 1e6);
    EXPECT_EQ(path->pathNodes, (std::vector<NodeId>{1, 2, 3}));
    EXPECT_EQ(path->hops, 2);
}

TEST(Allocation, ReportsUnreachableFlow) {
    // 4 号节点没有共同通信方式，接不进来
    std::vector<PlanNode> nodes = threeBackbones({"fiber"});
    nodes.push_back(PlanNode{4, "干线", 117.2, 39.00, 0.0, {"satellite"}});
    std::vector<PlanFlow> flows = {PlanFlow{200, 1, 4, 1e6, 1}};

    StaticPlanner p(std::move(nodes), flows);
    const PlanResult r = p.plan();

    const PlanPathResult* path = findPath(r, 200);
    ASSERT_NE(path, nullptr);
    EXPECT_FALSE(path->isSatisfied);
    EXPECT_EQ(path->hops, -1);
    EXPECT_FALSE(path->unsatisfiedReason.empty()) << "失败必须给出原因";
    EXPECT_EQ(countWarns(r, "unsatisfied_plan"), 1);
}

TEST(Allocation, BandwidthIsConserved) {
    // ★ 守恒律：每条链路上承载的流量之和不能超过它的容量。
    //   这是分配算法最基本的正确性约束。
    std::vector<PlanFlow> flows;
    for (int i = 0; i < 30; ++i) {
        flows.push_back(PlanFlow{i, 1, 3, 20e6, i % 3});
    }
    // 用微波（50Mbps）制造拥塞
    StaticPlanner p(threeBackbones({"microwave"}), flows);
    const PlanResult r = p.plan();

    for (const LinkResult& l : r.links) {
        double used = 0.0;
        for (const LinkFlowResult& f : l.flows) {
            used += f.bandwidthBps;
        }
        EXPECT_LE(used, l.bandwidthBps + 1e-6)
            << "链路 " << l.linkId << " 超卖：用了 " << used << " / "
            << l.bandwidthBps;
    }
}

TEST(Allocation, HigherPriorityFlowsWinUnderCongestion) {
    // ★ 单条微波链路 50Mbps，两条各要 40Mbps 的流，只能满足一条。
    //   qosLevel 小的优先级高，应该是它被满足。
    //   用两节点拓扑而不是三节点：三个干线两两互联会给出第二条路径，
    //   容量翻倍后两条流都能满足，就测不出优先级了。
    std::vector<PlanFlow> flows = {
        PlanFlow{1, 1, 2, 40e6, 2},  // 低优先级，但排在前面
        PlanFlow{2, 1, 2, 40e6, 0},  // 高优先级
    };
    StaticPlanner p(twoBackbones({"microwave"}, {"microwave"}), flows);
    const PlanResult r = p.plan();

    const PlanPathResult* low = findPath(r, 1);
    const PlanPathResult* high = findPath(r, 2);
    ASSERT_NE(low, nullptr);
    ASSERT_NE(high, nullptr);
    EXPECT_TRUE(high->isSatisfied) << "高优先级流应优先获得带宽";
    EXPECT_FALSE(low->isSatisfied);
}

TEST(Allocation, SplitsFlowAcrossParallelPaths) {
    // ★ 多路径分摊：1、2 之间两条微波各 50Mbps（不同方式），
    //   要 80Mbps 时单条路径不够，必须拆到两条上。
    std::vector<PlanNode> nodes = {
        PlanNode{1, "干线", 116.00, 39.00, 0.0, {"microwave", "manet"}},
        PlanNode{2, "干线", 116.05, 39.00, 0.0, {"microwave", "manet"}},
    };
    // microwave 50Mbps + manet 25Mbps = 75Mbps，要 70Mbps 需要两条都用上
    std::vector<PlanFlow> flows = {PlanFlow{1, 1, 2, 70e6, 1}};

    StaticPlanner p(std::move(nodes), flows);
    const PlanResult r = p.plan();

    double total = 0.0;
    int segments = 0;
    for (const PlanPathResult& pr : r.planResults) {
        if (pr.fid == 1 && pr.isSatisfied) {
            total += pr.actualBandwidthBps;
            ++segments;
        }
    }
    EXPECT_EQ(segments, 2) << "单条链路装不下，应拆成两段";
    EXPECT_NEAR(total, 70e6, 1.0);
}

TEST(Allocation, PartiallyServedFlowIsMarkedUnsatisfied) {
    // 要 80Mbps，唯一一条微波链路只有 50Mbps ——
    // 分到一部分也算未满足，而且原因里要写清还差多少
    std::vector<PlanFlow> flows = {PlanFlow{7, 1, 2, 80e6, 1}};
    StaticPlanner p(twoBackbones({"microwave"}, {"microwave"}), flows);
    const PlanResult r = p.plan();

    const PlanPathResult* path = findPath(r, 7);
    ASSERT_NE(path, nullptr);
    EXPECT_FALSE(path->isSatisfied);
    EXPECT_NE(path->unsatisfiedReason.find("bps"), std::string::npos)
        << "原因: " << path->unsatisfiedReason;
}

TEST(Allocation, InactiveLinksStillAppearInOutput) {
    // 界面要画完整拓扑，没跑流量的链路也得输出
    std::vector<PlanNode> nodes = threeBackbones({"fiber"});
    std::vector<PlanFlow> flows = {PlanFlow{1, 1, 2, 1e6, 1}};
    StaticPlanner p(std::move(nodes), flows);
    const PlanResult r = p.plan();

    ASSERT_EQ(r.links.size(), 3u) << "三个干线两两互联应有 3 条链路";
    const auto idle = linksBetween(r, 2, 3);
    ASSERT_EQ(idle.size(), 1u);
    EXPECT_TRUE(idle[0]->flows.empty());
}

// ── 输出契约 ────────────────────────────────────────────────────────────────

TEST(Warns, SummaryComesFirstAndCarriesElapsedTime) {
    std::vector<PlanFlow> flows = {PlanFlow{1, 1, 3, 1e6, 1}};
    StaticPlanner p(threeBackbones({"fiber"}), flows);
    const PlanResult r = p.plan();

    ASSERT_FALSE(r.warns.empty());
    EXPECT_EQ(r.warns[0].warnType, "summary") << "summary 必须是第一条";
    EXPECT_NE(r.warns[0].message.find("耗时"), std::string::npos);
    EXPECT_GE(r.elapsedSec, 0.0);
}

TEST(Warns, SuccessBeforeFailure) {
    std::vector<PlanNode> nodes = threeBackbones({"fiber"});
    nodes.push_back(PlanNode{9, "干线", 117.2, 39.00, 0.0, {"satellite"}});
    std::vector<PlanFlow> flows = {
        PlanFlow{1, 1, 9, 1e6, 1},  // 失败，排在前面
        PlanFlow{2, 1, 3, 1e6, 1},  // 成功
    };
    StaticPlanner p(std::move(nodes), flows);
    const PlanResult r = p.plan();

    std::vector<std::string> order;
    for (const WarnItem& w : r.warns) {
        order.push_back(w.warnType);
    }
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "summary");
    EXPECT_EQ(order[1], "flow_success") << "成功告警应在失败之前";
    EXPECT_EQ(order[2], "unsatisfied_plan");
    EXPECT_EQ(r.warns[2].level, "error");
}

TEST(Warns, ResultsFollowInputOrderNotAllocationOrder) {
    // 分配顺序按 QoS 排过，但输出要按输入顺序，方便客户端对照
    std::vector<PlanFlow> flows = {
        PlanFlow{10, 1, 3, 1e6, 2},
        PlanFlow{20, 1, 2, 1e6, 0},  // 优先级更高，会先被分配
        PlanFlow{30, 2, 3, 1e6, 1},
    };
    StaticPlanner p(threeBackbones({"fiber"}), flows);
    const PlanResult r = p.plan();

    std::vector<int> fids;
    for (const PlanPathResult& pr : r.planResults) {
        fids.push_back(pr.fid);
    }
    EXPECT_EQ(fids, (std::vector<int>{10, 20, 30}));
}

TEST(Planner, IsReproducible) {
    // ★ 同样的输入必须给出逐字节相同的方案。
    //   原型遍历 Python set 分配链路 id，这条保证不了 ——
    //   而一个规划系统如果重跑结果就变，没法核对也没法追责。
    std::vector<PlanNode> nodes;
    for (int i = 0; i < 12; ++i) {
        nodes.push_back(PlanNode{i,
                                 i < 5 ? "干线" : "支线",
                                 116.0 + 0.01 * i,
                                 39.0 + 0.005 * (i % 4),
                                 10.0 * i,
                                 {"fiber", "microwave", "manet"}});
    }
    std::vector<PlanFlow> flows;
    for (int i = 0; i < 8; ++i) {
        flows.push_back(PlanFlow{i, i % 5, 5 + (i % 7), 5e6 * (i + 1), i % 3});
    }

    auto fingerprint = [](const PlanResult& r) {
        std::string s;
        for (const LinkResult& l : r.links) {
            s += std::to_string(l.linkId) + ":" + std::to_string(l.srcNodeId) +
                 "-" + std::to_string(l.dstNodeId) + ":" +
                 l.deviceDisplayName + ":" + std::to_string(l.flows.size()) + ";";
        }
        for (const PlanPathResult& p : r.planResults) {
            s += std::to_string(p.fid) + ":" + (p.isSatisfied ? "1" : "0") + ":";
            for (NodeId n : p.pathNodes) {
                s += std::to_string(n) + ">";
            }
            s += "|";
        }
        return s;
    };

    StaticPlanner first(nodes, flows);
    const std::string expected = fingerprint(first.plan());

    for (int i = 0; i < 10; ++i) {
        StaticPlanner again(nodes, flows);
        EXPECT_EQ(fingerprint(again.plan()), expected)
            << "第 " << i << " 次规划结果与首次不同";
    }
}

TEST(Planner, HandlesEmptyInput) {
    StaticPlanner p({}, {});
    const PlanResult r = p.plan();
    EXPECT_TRUE(r.links.empty());
    EXPECT_TRUE(r.planResults.empty());
    ASSERT_FALSE(r.warns.empty());
    EXPECT_EQ(r.warns[0].warnType, "summary") << "空场景也要给 summary";
}

TEST(Planner, PathNodesAreContiguousAlongReportedLinks) {
    // 输出自洽：pathNodes 必须与 pathLinks 对得上，
    // 否则客户端画出来的路径和实际占用的链路是两回事
    std::vector<PlanNode> nodes;
    for (int i = 0; i < 8; ++i) {
        nodes.push_back(PlanNode{
            i, "干线", 116.0 + 0.02 * i, 39.0, 0.0, {"fiber", "microwave"}});
    }
    std::vector<PlanFlow> flows = {PlanFlow{1, 0, 7, 50e6, 1}};

    StaticPlanner p(std::move(nodes), flows);
    const PlanResult r = p.plan();
    const Graph& g = p.graph();

    for (const PlanPathResult& pr : r.planResults) {
        if (!pr.isSatisfied) {
            continue;
        }
        ASSERT_FALSE(pr.pathNodes.empty());
        EXPECT_EQ(pr.pathNodes.front(), pr.srcNodeId);
        EXPECT_EQ(pr.pathNodes.back(), pr.dstNodeId);
        EXPECT_EQ(static_cast<std::size_t>(pr.hops) + 1, pr.pathNodes.size());
        EXPECT_EQ(g.edgesToNodes(pr.pathLinks, pr.srcNodeId), pr.pathNodes);
    }
}
