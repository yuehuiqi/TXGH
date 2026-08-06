// 协议 JSON 编解码单元测试
//
// ★ 这个文件的重点是**恶意/畸形输入**，不是正常路径。
//   报文来自网络，是攻击者可以随意构造的。nlohmann 默认在类型不匹配时抛异常，
//   若异常穿过 IO 线程的回调栈，一条畸形报文就能打死整个服务端进程 ——
//   一个可远程触发的 DoS。所以每种"输入不是预期形状"的情况都要有用例。

#include "service/json_codec.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>

using namespace thgh;
using nlohmann::json;

// ── 信封解析 ────────────────────────────────────────────────────────────────

TEST(Envelope, ParsesTypeAndPayload) {
    const Envelope e = parseEnvelope(R"({"type":"start_plan","payload":{"a":1}})");
    EXPECT_TRUE(e.wellFormed);
    EXPECT_EQ(e.type, MessageType::StartPlan);
    EXPECT_FALSE(e.payload.empty());
}

TEST(Envelope, MalformedJsonDoesNotThrow) {
    // ★ 这几条如果抛异常，服务端就是可被一条报文打死的
    for (const std::string& bad : {
             std::string("{"),
             std::string("not json at all"),
             std::string(""),
             std::string("{\"type\":"),
             std::string("[1,2,3]"),          // 是合法 JSON 但不是对象
             std::string("null"),
             std::string("\"just a string\""),
             std::string("{\"type\":{\"nested\":1}}"),  // type 不是字符串
         }) {
        Envelope e;
        ASSERT_NO_THROW(e = parseEnvelope(bad)) << "输入: " << bad;
        EXPECT_EQ(e.type, MessageType::Unknown) << "输入: " << bad;
    }
}

TEST(Envelope, UnknownTypeStringIsUnknownNotCrash) {
    const Envelope e = parseEnvelope(R"({"type":"plan_results","payload":{}})");
    EXPECT_TRUE(e.wellFormed);
    // 注意是 plan_results（多了个 s），正是 message.h 注释里举的例子
    EXPECT_EQ(e.type, MessageType::Unknown);
}

TEST(Envelope, MissingPayloadIsEmptyNotError) {
    const Envelope e = parseEnvelope(R"({"type":"heartbeat"})");
    EXPECT_TRUE(e.wellFormed);
    EXPECT_EQ(e.type, MessageType::Heartbeat);
    EXPECT_TRUE(e.payload.empty());
}

// ── 规划请求解析 ────────────────────────────────────────────────────────────

namespace {

std::string validPayload() {
    return R"({
        "scene_id": 7,
        "nodes": [
            {"id":1,"node_type":"干线","longitude":116.0,"latitude":39.0,
             "altitude":10.0,"comm_methods":["fiber","microwave"]},
            {"id":2,"node_type":"干线","longitude":116.1,"latitude":39.0,
             "altitude":20.0,"comm_methods":["fiber"]}
        ],
        "flows": [
            {"fid":100,"src_node_id":1,"dst_node_id":2,
             "rate_bps":1000000.0,"qos_level":1}
        ]
    })";
}

}  // namespace

TEST(PlanRequest, ParsesValidPayload) {
    PlanRequest req;
    std::string err;
    ASSERT_TRUE(parsePlanRequest(validPayload(), req, err)) << err;

    EXPECT_EQ(req.sceneId, 7);
    ASSERT_EQ(req.nodes.size(), 2u);
    EXPECT_EQ(req.nodes[0].id, 1);
    EXPECT_EQ(req.nodes[0].nodeType, "干线");
    EXPECT_DOUBLE_EQ(req.nodes[0].longitude, 116.0);
    EXPECT_EQ(req.nodes[0].commMethods,
              (std::vector<std::string>{"fiber", "microwave"}));

    ASSERT_EQ(req.flows.size(), 1u);
    EXPECT_EQ(req.flows[0].fid, 100);
    EXPECT_EQ(req.flows[0].qosLevel, 1);
    EXPECT_DOUBLE_EQ(req.flows[0].rateBps, 1e6);
}

TEST(PlanRequest, RejectsMalformedInputWithReason) {
    struct Case {
        const char* payload;
        const char* expectSubstr;
    };
    const Case cases[] = {
        {"not json", "JSON"},
        {R"({"flows":[{"fid":1}]})", "nodes"},
        {R"({"nodes":[],"flows":[{"fid":1}]})", "nodes"},
        {R"({"nodes":[{"id":1}]})", "flows"},
        {R"({"nodes":[{"id":1}],"flows":[]})", "flows"},
        {R"({"nodes":[{"node_type":"干线"}],"flows":[{"fid":1}]})", "id"},
        {R"({"nodes":[{"id":1}],"flows":[{"src_node_id":1}]})", "fid"},
    };
    for (const Case& c : cases) {
        PlanRequest req;
        std::string err;
        EXPECT_FALSE(parsePlanRequest(c.payload, req, err)) << c.payload;
        EXPECT_NE(err.find(c.expectSubstr), std::string::npos)
            << "输入: " << c.payload << "\n实际错误: " << err;
    }
}

TEST(PlanRequest, WrongFieldTypesDoNotThrow) {
    // ★ 字段名对但类型全错 —— nlohmann 的 get<T>() 在这里会抛
    const std::string payload = R"({
        "scene_id": "seven",
        "nodes": [{"id":"abc","longitude":"east","comm_methods":123}],
        "flows": [{"fid":[1],"rate_bps":{"a":1}}]
    })";
    PlanRequest req;
    std::string err;
    ASSERT_NO_THROW(parsePlanRequest(payload, req, err));
    // id 取不到 → 判定为非法请求，但绝不能是崩溃
    EXPECT_FALSE(err.empty());
}

TEST(PlanRequest, AcceptsCommMethodsAsCommaSeparatedString) {
    // 老版本客户端发的是逗号分隔字符串。协议演进不能要求两端同时升级。
    const std::string payload = R"({
        "nodes": [{"id":1,"comm_methods":"fiber, microwave;manet"}],
        "flows": [{"fid":1,"src_node_id":1,"dst_node_id":1}]
    })";
    PlanRequest req;
    std::string err;
    ASSERT_TRUE(parsePlanRequest(payload, req, err)) << err;
    EXPECT_EQ(req.nodes[0].commMethods,
              (std::vector<std::string>{"fiber", "microwave", "manet"}));
}

TEST(PlanRequest, IntegerFieldsTolerateFloatEncoding) {
    // 有些 JSON 序列化器会把整数写成 12.0，直接 get<int>() 会抛
    const std::string payload = R"({
        "nodes": [{"id":1.0,"comm_methods":["fiber"]}],
        "flows": [{"fid":2.0,"src_node_id":1.0,"dst_node_id":1.0,"qos_level":1.0}]
    })";
    PlanRequest req;
    std::string err;
    ASSERT_TRUE(parsePlanRequest(payload, req, err)) << err;
    EXPECT_EQ(req.nodes[0].id, 1);
    EXPECT_EQ(req.flows[0].fid, 2);
    EXPECT_EQ(req.flows[0].qosLevel, 1);
}

TEST(PlanRequest, ClampsOutOfRangeConfig) {
    // ★ 这两个值直接决定计算量。客户端传负数或超大值就是远程资源耗尽面：
    //   pathsPerFlow=100000 会让每条流跑十万次 Dijkstra。
    const PlanConfig def;
    struct Case { const char* cfg; };
    for (const char* cfg : {R"("config":{"paths_per_flow":100000})",
                            R"("config":{"paths_per_flow":-5})",
                            R"("config":{"paths_per_flow":0})",
                            R"("config":{"comm_range_km":-1})",
                            R"("config":{"comm_range_km":1e9})"}) {
        const std::string payload =
            std::string(R"({"nodes":[{"id":1,"comm_methods":["fiber"]}],)") +
            R"("flows":[{"fid":1,"src_node_id":1,"dst_node_id":1}],)" + cfg + "}";
        PlanRequest req;
        std::string err;
        ASSERT_TRUE(parsePlanRequest(payload, req, err)) << payload << " / " << err;
        EXPECT_GE(req.config.pathsPerFlow, 1);
        EXPECT_LE(req.config.pathsPerFlow, 32);
        EXPECT_GT(req.config.commRangeKm, 0.0);
        EXPECT_LE(req.config.commRangeKm, 40000.0);
    }
}

TEST(PlanRequest, AcceptsValidConfigOverride) {
    const std::string payload =
        R"({"nodes":[{"id":1,"comm_methods":["fiber"]}],)"
        R"("flows":[{"fid":1,"src_node_id":1,"dst_node_id":1}],)"
        R"("config":{"comm_range_km":300.0,"paths_per_flow":8}})";
    PlanRequest req;
    std::string err;
    ASSERT_TRUE(parsePlanRequest(payload, req, err)) << err;
    EXPECT_DOUBLE_EQ(req.config.commRangeKm, 300.0);
    EXPECT_EQ(req.config.pathsPerFlow, 8);
}

// ── 编码 ────────────────────────────────────────────────────────────────────

TEST(Encode, AckCarriesTaskIdAndCounts) {
    const json j = json::parse(encodeAck(42, 10, 3));
    EXPECT_EQ(j.at("type"), "ack");
    EXPECT_EQ(j.at("task_id"), 42);
    EXPECT_EQ(j.at("payload").at("node_count"), 10);
    EXPECT_EQ(j.at("payload").at("flow_count"), 3);
}

TEST(Encode, ProgressCarriesPercentAndStage) {
    const json j = json::parse(encodeProgress(7, 55, "分配业务流带宽"));
    EXPECT_EQ(j.at("type"), "progress");
    EXPECT_EQ(j.at("task_id"), 7);
    EXPECT_EQ(j.at("payload").at("percent"), 55);
    EXPECT_EQ(j.at("payload").at("stage"), "分配业务流带宽");
}

TEST(Encode, ErrorPutsMessageAtTopLevel) {
    // 与改造前协议保持一致：错误文本在顶层 message，不在 payload 里，
    // 这样客户端的解析代码不用改
    const json j = json::parse(encodeError("出错了", 9));
    EXPECT_EQ(j.at("type"), "error");
    EXPECT_EQ(j.at("message"), "出错了");
    EXPECT_EQ(j.at("task_id"), 9);
}

TEST(Encode, ErrorWithoutTaskIdOmitsTheField) {
    const json j = json::parse(encodeError("连报文都没解析出来"));
    EXPECT_FALSE(j.contains("task_id"));
}

TEST(Encode, PlanResultRoundTripsAllFields) {
    PlanResult r;
    LinkResult l;
    l.linkId = 3;
    l.srcNodeId = 1;
    l.dstNodeId = 2;
    l.linkType = "wireless";
    l.wirelessType = "microwave";
    l.bandwidthBps = 50e6;
    l.propDelayS = 3.7e-5;
    l.flows.push_back(LinkFlowResult{100, 1e6, "主用流"});
    r.links.push_back(l);

    PlanPathResult p;
    p.fid = 100;
    p.srcNodeId = 1;
    p.dstNodeId = 2;
    p.isSatisfied = true;
    p.hops = 1;
    p.pathNodes = {1, 2};
    p.pathLinks = {3};
    p.actualBandwidthBps = 1e6;
    r.planResults.push_back(p);

    r.warns.push_back(WarnItem{"info", "规划完成", "summary", -1});
    r.elapsedSec = 0.012;

    const json j = json::parse(encodePlanResult(5, r));
    EXPECT_EQ(j.at("type"), "plan_result");
    EXPECT_EQ(j.at("task_id"), 5);

    const json& pl = j.at("payload");
    ASSERT_EQ(pl.at("links").size(), 1u);
    EXPECT_EQ(pl.at("links")[0].at("link_id"), 3);
    EXPECT_EQ(pl.at("links")[0].at("wireless_type"), "microwave");
    EXPECT_DOUBLE_EQ(pl.at("links")[0].at("bandwidth_bps").get<double>(), 50e6);
    ASSERT_EQ(pl.at("links")[0].at("flows").size(), 1u);
    EXPECT_EQ(pl.at("links")[0].at("flows")[0].at("fid"), 100);

    ASSERT_EQ(pl.at("plan_results").size(), 1u);
    EXPECT_TRUE(pl.at("plan_results")[0].at("is_satisfied").get<bool>());
    EXPECT_EQ(pl.at("plan_results")[0].at("path_nodes"),
              (json{1, 2}));

    ASSERT_EQ(pl.at("warns").size(), 1u);
    EXPECT_EQ(pl.at("warns")[0].at("warn_type"), "summary");
}

TEST(Encode, EveryMessageIsSingleLine) {
    // ★ 协议靠 '\n' 分帧。编码结果里若出现裸换行，一条消息会被切成两条，
    //   两端都会解析失败 —— 而且是"偶尔失败"，最难查。
    PlanResult r;
    r.warns.push_back(WarnItem{"info", "第一行\n第二行", "summary", -1});

    for (const std::string& s : {encodeAck(1, 2, 3),
                                 encodeProgress(1, 50, "阶段\n名"),
                                 encodeError("错误\n描述"),
                                 encodeHeartbeatAck(),
                                 encodePlanResult(1, r)}) {
        EXPECT_EQ(s.find('\n'), std::string::npos)
            << "编码结果含裸换行，会破坏分帧: " << s;
    }
}

TEST(Encode, NonAsciiSurvivesRoundTrip) {
    // 告警文本全是中文，编码后必须还能原样解回来
    PlanResult r;
    r.warns.push_back(
        WarnItem{"error", "规划流 fid=1 无法满足：所有路径耗尽", "unsatisfied_plan", 1});
    const json j = json::parse(encodePlanResult(1, r));
    EXPECT_EQ(j.at("payload").at("warns")[0].at("message"),
              "规划流 fid=1 无法满足：所有路径耗尽");
}
