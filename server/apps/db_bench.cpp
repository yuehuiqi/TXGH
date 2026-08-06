// 数据库量化实验
//
// 三组对比，全部产出可直接写进简历的数字：
//   实验一：连接池 vs 每次新建连接 —— 获取连接的耗时
//   实验二：逐条 INSERT vs 批量多值 INSERT —— 网络往返次数的影响
//   实验三：加索引前后的查询耗时 + EXPLAIN 执行计划对比
//
// 用法：db_bench [host] [user] [password] [database]

#include "db/conn_pool.h"
#include "db/mysql_conn.h"
#include "db/scene_dao.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 连接池的获取耗时在微秒量级，用毫秒计时会全部截断成 0.000，
// 算出"提升 18 万倍"这种明显荒谬的数字。测量精度必须匹配被测量级。
double usSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(p / 100.0 * (v.size() - 1));
    return v[std::min(idx, v.size() - 1)];
}

void banner(const char* title) {
    std::printf("\n");
    std::printf("========================================================\n");
    std::printf("  %s\n", title);
    std::printf("========================================================\n");
}

// 造一批测试节点
std::vector<thgh::SceneNodeRecord> makeNodes(int count) {
    std::vector<thgh::SceneNodeRecord> nodes;
    nodes.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        thgh::SceneNodeRecord n;
        n.nodeId = i;
        n.name = "node-" + std::to_string(i);
        n.nodeType = (i % 3 == 0) ? "指挥节点" : "通信节点";
        n.longitude = 116.0 + i * 0.001;
        n.latitude = 39.0 + i * 0.001;
        n.altitude = 10.0 + i;
        n.commMethods = "fiber,microwave";
        n.deviceParams = R"({"power":30,"gain":12})";
        nodes.push_back(std::move(n));
    }
    return nodes;
}

std::vector<thgh::SceneLinkRecord> makeLinks(int nodeCount) {
    std::vector<thgh::SceneLinkRecord> links;
    for (int i = 0; i + 1 < nodeCount; ++i) {
        thgh::SceneLinkRecord l;
        l.src = i;
        l.dst = i + 1;
        l.linkType = "wireless";
        l.wirelessType = "microwave";
        l.bandwidthBps = 50e6;
        l.propDelayS = 1e-5;
        links.push_back(std::move(l));
    }
    return links;
}

// ── 实验一：连接池 vs 每次新建 ──────────────────────────────────────────────
void experimentConnectionPool(const thgh::MySqlConfig& cfg) {
    banner("实验一：连接池 vs 每次新建连接");

    constexpr int kRounds = 200;

    // A. 不用池：每次建一条新连接
    std::vector<double> noPool;
    noPool.reserve(kRounds);
    for (int i = 0; i < kRounds; ++i) {
        const auto t0 = Clock::now();
        thgh::MySqlConnection conn;
        if (!conn.connect(cfg)) {
            std::fprintf(stderr, "建连失败: %s\n", conn.lastError().c_str());
            return;
        }
        noPool.push_back(usSince(t0));
        conn.close();
    }

    // B. 用池
    thgh::ConnectionPool pool;
    thgh::ConnectionPool::Options opt;
    opt.initialSize = 4;
    opt.maxSize = 16;
    if (!pool.init(cfg, opt)) {
        std::fprintf(stderr, "连接池初始化失败\n");
        return;
    }

    std::vector<double> withPool;
    withPool.reserve(kRounds);
    for (int i = 0; i < kRounds; ++i) {
        const auto t0 = Clock::now();
        auto conn = pool.acquire();
        if (!conn) {
            std::fprintf(stderr, "从池获取连接失败\n");
            return;
        }
        withPool.push_back(usSince(t0));
        // conn 析构时自动归还
    }

    double sumA = 0, sumB = 0;
    for (double v : noPool) sumA += v;
    for (double v : withPool) sumB += v;
    const double avgA = sumA / noPool.size();
    const double avgB = sumB / withPool.size();

    std::printf("%d 次获取连接（计时用微秒：池化后的耗时在微秒量级，\n", kRounds);
    std::printf("用毫秒计时会全部截断成 0.000，算出\"提升十几万倍\"这种荒谬数字）\n\n");
    std::printf("%-22s %12s %12s %12s\n", "方式", "平均/us", "P50/us", "P99/us");
    std::printf("%s\n", std::string(62, '-').c_str());
    std::printf("%-22s %12.1f %12.1f %12.1f\n", "每次新建连接", avgA,
                percentile(noPool, 50), percentile(noPool, 99));
    std::printf("%-22s %12.1f %12.1f %12.1f\n", "连接池复用", avgB,
                percentile(withPool, 50), percentile(withPool, 99));
    std::printf("\n获取连接耗时 %.1fus → %.1fus，降低 %.2f%%，约 %.0f 倍\n", avgA,
                avgB, (avgA - avgB) / avgA * 100.0,
                avgB > 0 ? avgA / avgB : 0.0);
    std::printf("\n说明：建连开销 = TCP 三次握手 + MySQL 协议握手 + 认证 + 选库，\n");
    std::printf("      这套流程的耗时远大于一次简单查询本身。\n");

    const auto s = pool.stats();
    std::printf("\n连接池统计：累计获取 %llu 次，物理连接创建 %llu 条，"
                "峰值借出 %zu 条\n",
                (unsigned long long)s.acquired, (unsigned long long)s.created,
                s.peakInUse);
}

// ── 实验二：逐条 INSERT vs 批量 INSERT ──────────────────────────────────────
void experimentBatchInsert(thgh::MySqlConnection& conn) {
    banner("实验二：逐条 INSERT vs 批量多值 INSERT");

    constexpr int kNodes = 500;
    const auto nodes = makeNodes(kNodes);

    // 准备一个宿主场景
    conn.execute("DELETE FROM scenes WHERE name LIKE 'bench-batch%'");
    conn.executePrepared(
        "INSERT INTO scenes (name, description, scene_type, create_time) "
        "VALUES (?,?,?,?)",
        {thgh::Param::ofString("bench-batch-A"), thgh::Param::ofString(""),
         thgh::Param::ofString("bench"), thgh::Param::ofString("2026")});
    const std::int64_t sceneA = static_cast<std::int64_t>(conn.lastInsertId());

    // A. 逐条插入（每条一次网络往返）
    const auto t0 = Clock::now();
    {
        thgh::Transaction tx(conn);
        for (const auto& n : nodes) {
            conn.executePrepared(
                "INSERT INTO scene_nodes (scene_id, node_id, name, node_type, "
                "longitude, latitude, altitude, comm_methods, device_params) "
                "VALUES (?,?,?,?,?,?,?,?,?)",
                {thgh::Param::ofInt(sceneA), thgh::Param::ofInt(n.nodeId),
                 thgh::Param::ofString(n.name), thgh::Param::ofString(n.nodeType),
                 thgh::Param::ofDouble(n.longitude),
                 thgh::Param::ofDouble(n.latitude),
                 thgh::Param::ofDouble(n.altitude),
                 thgh::Param::ofString(n.commMethods),
                 thgh::Param::ofString(n.deviceParams)});
        }
        tx.commit();
    }
    const double oneByOne = msSince(t0);

    // B. 批量插入（SceneDao 内部按 500 行一批合并）
    conn.executePrepared(
        "INSERT INTO scenes (name, description, scene_type, create_time) "
        "VALUES (?,?,?,?)",
        {thgh::Param::ofString("bench-batch-B"), thgh::Param::ofString(""),
         thgh::Param::ofString("bench"), thgh::Param::ofString("2026")});
    const std::int64_t sceneB = static_cast<std::int64_t>(conn.lastInsertId());

    const auto t1 = Clock::now();
    {
        thgh::Transaction tx(conn);
        thgh::SceneRecord dummy;
        // 直接调私有批量路径不方便，这里用 saveScene 的等价形式：
        // 先建场景再批量插节点。为公平起见只计时插入部分。
        std::string sql =
            "INSERT INTO scene_nodes (scene_id, node_id, name, node_type, "
            "longitude, latitude, altitude, comm_methods, device_params) VALUES ";
        std::vector<thgh::Param> params;
        params.reserve(nodes.size() * 9);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (i > 0) sql += ",";
            sql += "(?,?,?,?,?,?,?,?,?)";
            const auto& n = nodes[i];
            params.push_back(thgh::Param::ofInt(sceneB));
            params.push_back(thgh::Param::ofInt(n.nodeId));
            params.push_back(thgh::Param::ofString(n.name));
            params.push_back(thgh::Param::ofString(n.nodeType));
            params.push_back(thgh::Param::ofDouble(n.longitude));
            params.push_back(thgh::Param::ofDouble(n.latitude));
            params.push_back(thgh::Param::ofDouble(n.altitude));
            params.push_back(thgh::Param::ofString(n.commMethods));
            params.push_back(thgh::Param::ofString(n.deviceParams));
        }
        conn.executePrepared(sql, params);
        tx.commit();
    }
    const double batched = msSince(t1);

    std::printf("插入 %d 个节点：\n\n", kNodes);
    std::printf("%-28s %10s\n", "方式", "耗时/ms");
    std::printf("%s\n", std::string(40, '-').c_str());
    std::printf("%-28s %10.1f\n", "逐条 INSERT（500 次往返）", oneByOne);
    std::printf("%-28s %10.1f\n", "批量多值 INSERT（1 次往返）", batched);
    std::printf("\n耗时降低 %.1f%%（%.1fms → %.1fms，约 %.1f 倍）\n",
                (oneByOne - batched) / oneByOne * 100.0, oneByOne, batched,
                batched > 0 ? oneByOne / batched : 0.0);
    std::printf("\n说明：两者都在同一个事务内，差异纯粹来自网络往返次数。\n");

    conn.execute("DELETE FROM scenes WHERE name LIKE 'bench-batch%'");
}

// ── 实验三：索引优化 ────────────────────────────────────────────────────────
void experimentIndex(thgh::MySqlConnection& conn) {
    banner("实验三：索引优化（EXPLAIN + 耗时对比）");

    // 造数据：2000 个场景，每个场景 200 个节点 → scene_nodes 共 40 万行
    constexpr int kScenes = 2000;
    constexpr int kNodesPerScene = 200;

    std::printf("正在造数据：%d 个场景 × %d 节点 = %d 行...\n", kScenes,
                kNodesPerScene, kScenes * kNodesPerScene);

    // ★ 先强制把索引恢复到基线状态。
    //
    // 上一次运行若中途中断，DROP INDEX 不会执行，索引会留在表上，
    // 导致本次"加索引前"的基线其实已经带着索引 —— 对比完全失效。
    // 实验类程序必须自己保证初始状态干净，不能假设上次跑完整了。
    //
    // 这里有个 MySQL 的坑：**外键列上必须始终存在可用的索引**。
    // scene_nodes.scene_id 是外键，建表时 MySQL 自动建了 fk_scene_nodes_scene(scene_id)。
    // 我们新建的复合索引 (scene_id, node_type) 最左列也是 scene_id，
    // 按最左前缀原则它同样能满足外键要求，于是原索引变得可删除；
    // 而一旦原索引没了，复合索引就成了外键的唯一支撑，直接 DROP 会报：
    //     ERROR 1553: Cannot drop index ... needed in a foreign key constraint
    // 正确做法是**先补回单列索引，再删复合索引**。
    conn.execute("CREATE INDEX fk_scene_nodes_scene ON scene_nodes(scene_id)");
    conn.execute("DROP INDEX idx_scenes_name ON scenes");
    conn.execute("DROP INDEX idx_scene_nodes_scene_type ON scene_nodes");
    {
        thgh::ResultSet chk;
        conn.query("SELECT INDEX_NAME FROM information_schema.STATISTICS "
                   "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN "
                   "('scenes','scene_nodes') AND INDEX_NAME NOT IN ('PRIMARY') "
                   "GROUP BY INDEX_NAME", chk);
        std::printf("基线索引检查：");
        if (chk.rowCount() == 0) {
            std::printf("除主键外无其它索引\n");
        } else {
            for (std::size_t i = 0; i < chk.rowCount(); ++i) {
                std::printf("%s ", chk.at(i, 0).c_str());
            }
            std::printf("（外键自带索引属正常）\n");
        }
    }

    conn.execute("DELETE FROM scenes WHERE name LIKE 'idx-bench-%'");

    const auto tGen = Clock::now();
    for (int s = 0; s < kScenes; ++s) {
        thgh::SceneRecord scene;
        scene.name = "idx-bench-" + std::to_string(s);
        scene.sceneType = "bench";
        scene.createTime = "2026-08-04";
        auto nodes = makeNodes(kNodesPerScene);
        std::int64_t sceneId = 0;
        if (!thgh::SceneDao::saveScene(conn, scene, nodes, {}, &sceneId)) {
            std::fprintf(stderr, "造数据失败\n");
            return;
        }
    }
    std::printf("造数据完成，耗时 %.1f 秒\n\n", msSince(tGen) / 1000.0);

    // 取一个中间的场景 id 作为查询目标，避免命中边界优化
    thgh::ResultSet rs;
    conn.query("SELECT id FROM scenes WHERE name = 'idx-bench-1000'", rs);
    const std::int64_t targetScene =
        rs.rowCount() > 0 ? std::strtoll(rs.at(0, 0).c_str(), nullptr, 10) : 1;

    auto timeQuery = [&conn](const std::string& sql, int rounds) -> double {
        // 预热一次，避免把 buffer pool 冷启动算进去
        thgh::ResultSet tmp;
        conn.query(sql, tmp);
        const auto t0 = Clock::now();
        for (int i = 0; i < rounds; ++i) {
            thgh::ResultSet r;
            conn.query(sql, r);
        }
        return msSince(t0) / rounds;
    };

    auto showExplain = [&conn](const std::string& sql, const char* label) {
        thgh::ResultSet rs2;
        if (!conn.query("EXPLAIN " + sql, rs2) || rs2.rowCount() == 0) {
            return;
        }
        std::printf("  %s EXPLAIN：", label);
        // 只打关键三列：type（访问类型）、key（用了哪个索引）、rows（预估扫描行数）
        for (std::size_t c = 0; c < rs2.columnCount(); ++c) {
            const std::string& col = rs2.columns()[c];
            if (col == "type" || col == "key" || col == "rows") {
                std::printf("%s=%s  ", col.c_str(),
                            rs2.isNull(0, c) ? "NULL" : rs2.at(0, c).c_str());
            }
        }
        std::printf("\n");
    };

    // ── 查询 1：按场景名查找（scenes.name 上无索引 → 全表扫描）────────
    const std::string q1 =
        "SELECT id, name FROM scenes WHERE name = 'idx-bench-1000'";
    // ── 查询 2：按场景 id + 节点类型查节点 ───────────────────────────
    const std::string q2 =
        "SELECT id, name FROM scene_nodes WHERE scene_id = " +
        std::to_string(targetScene) + " AND node_type = '指挥节点'";

    std::printf("── 加索引前 ──\n");
    showExplain(q1, "查询1(按场景名)");
    const double q1Before = timeQuery(q1, 50);
    std::printf("  查询1 平均耗时 %.3f ms\n", q1Before);
    showExplain(q2, "查询2(场景+类型)");
    const double q2Before = timeQuery(q2, 50);
    std::printf("  查询2 平均耗时 %.3f ms\n", q2Before);

    // ── 建索引 ───────────────────────────────────────────────────────
    std::printf("\n正在创建索引...\n");
    conn.execute("CREATE INDEX idx_scenes_name ON scenes(name)");
    // 联合索引，顺序有讲究：等值条件列在前、区分度高的在前。
    // (scene_id, node_type) 能同时服务"只按 scene_id 查"和"按两者查"，
    // 反过来 (node_type, scene_id) 就无法单独服务前者 —— 最左前缀原则。
    conn.execute(
        "CREATE INDEX idx_scene_nodes_scene_type ON scene_nodes(scene_id, node_type)");
    conn.execute("ANALYZE TABLE scenes, scene_nodes");

    std::printf("\n── 加索引后 ──\n");
    showExplain(q1, "查询1(按场景名)");
    const double q1After = timeQuery(q1, 50);
    std::printf("  查询1 平均耗时 %.3f ms\n", q1After);
    showExplain(q2, "查询2(场景+类型)");
    const double q2After = timeQuery(q2, 50);
    std::printf("  查询2 平均耗时 %.3f ms\n", q2After);

    std::printf("\n%-24s %12s %12s %10s\n", "查询", "加索引前/ms", "加索引后/ms",
                "提升");
    std::printf("%s\n", std::string(62, '-').c_str());
    std::printf("%-24s %12.3f %12.3f %9.1fx\n", "按场景名精确查找", q1Before,
                q1After, q1After > 0 ? q1Before / q1After : 0.0);
    std::printf("%-24s %12.3f %12.3f %9.1fx\n", "场景内按节点类型筛选", q2Before,
                q2After, q2After > 0 ? q2Before / q2After : 0.0);

    // ── 索引失效场景演示 ─────────────────────────────────────────────
    std::printf("\n── 索引失效场景（同样的列，索引用不上）──\n");
    const std::string qBad1 =
        "SELECT id FROM scenes WHERE name LIKE '%bench-1000'";
    const std::string qBad2 =
        "SELECT id FROM scenes WHERE LEFT(name, 9) = 'idx-bench'";
    showExplain(qBad1, "前置通配符 LIKE '%xxx'");
    showExplain(qBad2, "对索引列做函数运算");
    std::printf("\n  注意 type=index 而不是 type=ALL —— 这两者要分清：\n");
    std::printf("    type=ref   : 走索引定位，只读命中的少数行（rows=1），O(log n)\n");
    std::printf("    type=index : **全索引扫描**，索引树从头扫到尾（rows=2000）\n");
    std::printf("    type=ALL   : 全表扫描，逐行读数据页\n");
    std::printf("  这里因为只 SELECT id、恰好被索引覆盖（覆盖索引），\n");
    std::printf("  MySQL 选择扫索引而不是扫表——索引比表小，仍比 ALL 快，\n");
    std::printf("  但**依然是 O(n) 的全量扫描，索引的定位能力完全没用上**。\n");
    std::printf("  失效原因：前置通配符无法利用 B+ 树的有序性；\n");
    std::printf("  对索引列做函数运算后，索引里存的原值与计算结果不再对应。\n");

    // 清理。顺序同上：先补回外键的单列索引，再删复合索引
    std::printf("\n清理测试数据...\n");
    conn.execute("CREATE INDEX fk_scene_nodes_scene ON scene_nodes(scene_id)");
    conn.execute("DROP INDEX idx_scenes_name ON scenes");
    conn.execute("DROP INDEX idx_scene_nodes_scene_type ON scene_nodes");
    conn.execute("DELETE FROM scenes WHERE name LIKE 'idx-bench-%'");
}

// ── 实验四：事务原子性 ──────────────────────────────────────────────────────
void experimentTransaction(thgh::MySqlConnection& conn) {
    banner("实验四：事务原子性（中途失败必须整体回滚）");

    conn.execute("DELETE FROM scenes WHERE name LIKE 'tx-test%'");

    thgh::ResultSet before;
    conn.query("SELECT COUNT(*) FROM scenes", before);
    const std::string cntBefore = before.at(0, 0);

    // 构造一个中途必然失败的场景：第二批节点里塞一个超长的 node_type，
    // 触发数据截断错误（MySQL 严格模式下会报错）
    {
        thgh::Transaction tx(conn);
        conn.executePrepared(
            "INSERT INTO scenes (name, scene_type) VALUES (?,?)",
            {thgh::Param::ofString("tx-test-rollback"),
             thgh::Param::ofString("bench")});
        const std::int64_t sid = static_cast<std::int64_t>(conn.lastInsertId());

        // 先插几个正常节点
        conn.executePrepared(
            "INSERT INTO scene_nodes (scene_id, node_id, longitude, latitude) "
            "VALUES (?,?,?,?)",
            {thgh::Param::ofInt(sid), thgh::Param::ofInt(1),
             thgh::Param::ofDouble(116.0), thgh::Param::ofDouble(39.0)});

        // 再插一个必定失败的：node_type 字段是 VARCHAR(100)，这里给 500 字符
        const bool bad = conn.executePrepared(
            "INSERT INTO scene_nodes (scene_id, node_id, node_type, longitude, "
            "latitude) VALUES (?,?,?,?,?)",
            {thgh::Param::ofInt(sid), thgh::Param::ofInt(2),
             thgh::Param::ofString(std::string(500, 'X')),
             thgh::Param::ofDouble(116.0), thgh::Param::ofDouble(39.0)});

        std::printf("插入超长字段：%s\n", bad ? "意外成功" : "按预期失败");
        std::printf("错误信息：%s\n", conn.lastError().c_str());
        // 不调用 commit，Transaction 析构自动回滚
    }

    thgh::ResultSet after;
    conn.query("SELECT COUNT(*) FROM scenes", after);
    const std::string cntAfter = after.at(0, 0);

    thgh::ResultSet leftover;
    conn.query("SELECT COUNT(*) FROM scenes WHERE name = 'tx-test-rollback'",
               leftover);

    std::printf("\n场景总数：事务前 %s，回滚后 %s\n", cntBefore.c_str(),
                cntAfter.c_str());
    std::printf("残留的半成品场景数：%s\n", leftover.at(0, 0).c_str());
    std::printf("\n结论：%s\n",
                leftover.at(0, 0) == "0"
                    ? "整体回滚成功，没有留下任何残缺数据 ✓"
                    : "回滚失败，存在残留数据 ✗");
    std::printf("\n对比改造前：客户端逐条裸执行、全仓库零事务，\n");
    std::printf("中途失败会留下一个只有部分节点、没有链路的残缺场景，\n");
    std::printf("而且没有任何标记能识别它 —— 用户看到的是一个打开就报错的场景。\n");
}

}  // namespace

int main(int argc, char** argv) {
    thgh::MySqlConfig cfg;
    if (argc > 1) cfg.host = argv[1];
    if (argc > 2) cfg.user = argv[2];
    if (argc > 3) cfg.password = argv[3];
    if (argc > 4) cfg.database = argv[4];

    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    std::printf("数据库 %s@%s/%s\n", cfg.user.c_str(), cfg.host.c_str(),
                cfg.database.c_str());

    thgh::MySqlConnection conn;
    if (!conn.connect(cfg)) {
        std::fprintf(stderr, "连接数据库失败: %s\n", conn.lastError().c_str());
        return 1;
    }

    experimentConnectionPool(cfg);
    experimentTransaction(conn);
    experimentBatchInsert(conn);
    experimentIndex(conn);

    std::printf("\n全部实验完成\n");
    return 0;
}
