// InnoDB 锁与隔离级别实验
//
// 方案 4.2 的要求：开两个会话故意构造并发场景，观察间隙锁阻塞现象，
// 配合 SHOW ENGINE INNODB STATUS 记录锁等待信息，
// 把"事务隔离级别/MVCC"从背诵变成真实经历。
//
// 四个实验：
//   实验一：间隙锁阻塞插入（REPEATABLE READ 下的幻读防护）
//   实验二：同样的场景在 READ COMMITTED 下不阻塞（对比）
//   实验三：MVCC 快照读 —— 可重复读到底"可重复"在哪
//   实验四：死锁检测与自动回滚
//
// 用法：lock_experiment [host] [user] [password] [database]

#include "db/mysql_conn.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
thgh::MySqlConfig g_cfg;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void banner(const char* title) {
    std::printf("\n");
    std::printf("========================================================\n");
    std::printf("  %s\n", title);
    std::printf("========================================================\n");
}

// 开一条独立连接 = 一个独立会话。
// 这是实验的前提：锁是会话级的，同一连接上的两个事务不会互相阻塞。
std::unique_ptr<thgh::MySqlConnection> newSession(const char* name) {
    auto conn = std::unique_ptr<thgh::MySqlConnection>(new thgh::MySqlConnection());
    if (!conn->connect(g_cfg)) {
        std::fprintf(stderr, "[%s] 连接失败: %s\n", name,
                     conn->lastError().c_str());
        return nullptr;
    }
    return conn;
}

void resetTable(thgh::MySqlConnection& conn) {
    conn.execute("DROP TABLE IF EXISTS lock_demo");
    conn.execute(
        "CREATE TABLE lock_demo ("
        "  id INT PRIMARY KEY,"
        "  seq INT NOT NULL,"
        "  note VARCHAR(50),"
        "  INDEX idx_seq (seq)"   // 间隙锁加在这个二级索引上
        ") ENGINE=InnoDB");
    // 刻意留出间隙：10, 20, 30 之间有空档，20~30 就是一个"间隙"
    conn.execute(
        "INSERT INTO lock_demo (id, seq, note) VALUES "
        "(1,10,'a'),(2,20,'b'),(3,30,'c')");
}

// 打印当前的锁等待信息。
// MySQL 8.0 用 performance_schema.data_lock_waits 取代了老版本的
// information_schema.innodb_lock_waits，后者在 8.0 已被移除。
void showLockWaits(thgh::MySqlConnection& conn) {
    thgh::ResultSet rs;
    const bool ok = conn.query(
        "SELECT r.trx_id AS waiting_trx, r.trx_mysql_thread_id AS waiting_thread, "
        "       SUBSTRING(r.trx_query,1,60) AS waiting_query, "
        "       b.trx_id AS blocking_trx, b.trx_mysql_thread_id AS blocking_thread "
        "FROM performance_schema.data_lock_waits w "
        "JOIN information_schema.innodb_trx r "
        "  ON r.trx_id = CONV(LEFT(w.REQUESTING_ENGINE_TRANSACTION_ID,16),16,10) "
        "JOIN information_schema.innodb_trx b "
        "  ON b.trx_id = CONV(LEFT(w.BLOCKING_ENGINE_TRANSACTION_ID,16),16,10)",
        rs);
    if (!ok || rs.rowCount() == 0) {
        // 换个更直接的视图：直接看持有和等待的锁
        thgh::ResultSet rs2;
        if (conn.query(
                "SELECT ENGINE_TRANSACTION_ID, LOCK_TYPE, LOCK_MODE, LOCK_STATUS, "
                "       INDEX_NAME, LOCK_DATA "
                "FROM performance_schema.data_locks "
                "WHERE OBJECT_NAME='lock_demo' ORDER BY LOCK_STATUS DESC",
                rs2) &&
            rs2.rowCount() > 0) {
            std::printf("  当前 lock_demo 上的锁：\n");
            for (std::size_t i = 0; i < rs2.rowCount(); ++i) {
                std::printf("    事务 %-8s %-8s %-20s %-8s 索引=%-10s 数据=%s\n",
                            rs2.at(i, 0).c_str(), rs2.at(i, 1).c_str(),
                            rs2.at(i, 2).c_str(), rs2.at(i, 3).c_str(),
                            rs2.isNull(i, 4) ? "-" : rs2.at(i, 4).c_str(),
                            rs2.isNull(i, 5) ? "-" : rs2.at(i, 5).c_str());
            }
        }
        return;
    }
    std::printf("  锁等待关系：\n");
    for (std::size_t i = 0; i < rs.rowCount(); ++i) {
        std::printf("    事务 %s (线程 %s) 正在等待 事务 %s (线程 %s)\n",
                    rs.at(i, 0).c_str(), rs.at(i, 1).c_str(),
                    rs.at(i, 3).c_str(), rs.at(i, 4).c_str());
        std::printf("    被阻塞的语句: %s\n", rs.at(i, 2).c_str());
    }
}

// ── 实验一：间隙锁阻塞插入 ──────────────────────────────────────────────────
void experimentGapLock() {
    banner("实验一：间隙锁阻塞插入（REPEATABLE READ）");

    auto sA = newSession("A");
    auto sB = newSession("B");
    auto sMon = newSession("监控");
    if (!sA || !sB || !sMon) return;

    resetTable(*sA);
    sA->execute("SET SESSION transaction_isolation = 'REPEATABLE-READ'");
    sB->execute("SET SESSION transaction_isolation = 'REPEATABLE-READ'");

    std::printf("表数据：(id=1,seq=10) (id=2,seq=20) (id=3,seq=30)\n");
    std::printf("seq 上有二级索引 idx_seq，20 和 30 之间存在间隙\n\n");

    // 会话 A：对 seq 在 [20,30] 范围做条件更新，先不提交
    std::printf("[会话A] BEGIN; UPDATE lock_demo SET note='x' WHERE seq BETWEEN 20 AND 30;\n");
    sA->begin();
    if (!sA->execute("UPDATE lock_demo SET note='x' WHERE seq BETWEEN 20 AND 30")) {
        std::printf("  更新失败: %s\n", sA->lastError().c_str());
        return;
    }
    std::printf("[会话A] 更新完成，**故意不提交**，持有范围锁\n\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 会话 B：往 A 锁住的间隙里插入
    std::printf("[会话B] 尝试 INSERT (id=4, seq=25) —— 落在 20~30 的间隙里\n");
    std::atomic<bool> done{false};
    std::atomic<bool> succeeded{false};
    double blockedMs = 0;

    std::thread tB([&] {
        const auto t0 = Clock::now();
        sB->begin();
        // 这条 INSERT 会被 A 持有的间隙锁阻塞，直到 A 提交或锁等待超时
        const bool ok = sB->execute(
            "INSERT INTO lock_demo (id, seq, note) VALUES (4, 25, 'new')");
        blockedMs = msSince(t0);
        succeeded = ok;
        if (!ok) {
            std::printf("[会话B] 插入失败: %s\n", sB->lastError().c_str());
        }
        sB->rollback();
        done = true;
    });

    // 等一会儿，让 B 确实进入阻塞状态，然后观察锁
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    std::printf("\n[监控] 此时 B 是否还在阻塞：%s\n", done ? "已返回" : "**仍在阻塞**");
    showLockWaits(*sMon);

    std::printf("\n[会话A] COMMIT，释放锁\n");
    sA->commit();

    tB.join();
    std::printf("[会话B] 插入%s，被阻塞了 %.0f ms\n",
                succeeded ? "成功" : "失败", blockedMs);

    std::printf("\n结论：\n");
    std::printf("  会话 B 的插入被阻塞了约 %.0f ms —— 直到 A 提交才放行。\n", blockedMs);
    std::printf("  注意 B 插入的 seq=25 这一行**原本并不存在**，\n");
    std::printf("  A 也没有锁住任何一条具体的记录行 ——\n");
    std::printf("  被锁住的是 20 到 30 之间的**间隙**，这就是 Gap Lock。\n");
    std::printf("  它的作用是防止幻读：保证 A 在事务内重复执行同一范围查询时，\n");
    std::printf("  不会突然多出一行别人插进来的记录。\n");
}

// ── 实验二：READ COMMITTED 下不阻塞 ────────────────────────────────────────
void experimentReadCommitted() {
    banner("实验二：同样场景在 READ COMMITTED 下不阻塞（对比）");

    auto sA = newSession("A");
    auto sB = newSession("B");
    if (!sA || !sB) return;

    resetTable(*sA);
    // READ COMMITTED 下 InnoDB **不加间隙锁**（只锁命中的记录行），
    // 因此不防幻读，但并发插入的能力更强
    sA->execute("SET SESSION transaction_isolation = 'READ-COMMITTED'");
    sB->execute("SET SESSION transaction_isolation = 'READ-COMMITTED'");

    std::printf("[会话A] BEGIN; UPDATE ... WHERE seq BETWEEN 20 AND 30;（不提交）\n");
    sA->begin();
    sA->execute("UPDATE lock_demo SET note='x' WHERE seq BETWEEN 20 AND 30");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::printf("[会话B] 尝试 INSERT (id=4, seq=25)\n");
    const auto t0 = Clock::now();
    sB->begin();
    const bool ok = sB->execute(
        "INSERT INTO lock_demo (id, seq, note) VALUES (4, 25, 'new')");
    const double elapsed = msSince(t0);
    sB->rollback();
    sA->rollback();

    std::printf("[会话B] 插入%s，耗时 %.1f ms\n", ok ? "成功" : "失败", elapsed);
    std::printf("\n结论：\n");
    std::printf("  同样的操作在 READ COMMITTED 下**立即成功**（%.1f ms，无阻塞）。\n",
                elapsed);
    std::printf("  因为 RC 隔离级别下 InnoDB 不加间隙锁，只锁命中的记录行。\n");
    std::printf("  代价是不防幻读；收益是并发插入能力显著更强。\n");
    std::printf("  这就是为什么很多互联网业务把隔离级别调成 RC —— \n");
    std::printf("  业务本身能容忍幻读，但受不了间隙锁带来的写入阻塞。\n");
}

// ── 实验三：MVCC 快照读 ─────────────────────────────────────────────────────
void experimentMvcc() {
    banner("实验三：MVCC 快照读 —— 可重复读到底可重复在哪");

    auto sA = newSession("A");
    auto sB = newSession("B");
    if (!sA || !sB) return;

    resetTable(*sA);
    sA->execute("SET SESSION transaction_isolation = 'REPEATABLE-READ'");
    sB->execute("SET SESSION transaction_isolation = 'REPEATABLE-READ'");

    auto readNote = [](thgh::MySqlConnection& c) -> std::string {
        thgh::ResultSet rs;
        c.query("SELECT note FROM lock_demo WHERE id = 1", rs);
        return rs.rowCount() > 0 ? rs.at(0, 0) : "(空)";
    };

    std::printf("[会话A] BEGIN; 第一次读 id=1 的 note\n");
    sA->begin();
    // 注意：ReadView 是在**第一次快照读**时创建的，不是 BEGIN 时。
    // 所以这次 SELECT 很关键，没有它下面的对比就不成立。
    const std::string first = readNote(*sA);
    std::printf("        读到 note = '%s'（此刻创建 ReadView）\n\n", first.c_str());

    std::printf("[会话B] 把 id=1 的 note 改成 'MODIFIED' 并提交\n");
    sB->begin();
    sB->execute("UPDATE lock_demo SET note='MODIFIED' WHERE id = 1");
    sB->commit();
    std::printf("        已提交\n\n");

    std::printf("[会话A] 在同一事务内再读一次\n");
    const std::string second = readNote(*sA);
    std::printf("        读到 note = '%s'\n", second.c_str());

    std::printf("\n[会话A] 现在做一次**当前读**（SELECT ... FOR UPDATE）\n");
    thgh::ResultSet rs;
    sA->query("SELECT note FROM lock_demo WHERE id = 1 FOR UPDATE", rs);
    const std::string current = rs.rowCount() > 0 ? rs.at(0, 0) : "(空)";
    std::printf("        读到 note = '%s'\n", current.c_str());

    sA->commit();

    std::printf("\n[会话A] 提交后重新读\n");
    const std::string afterCommit = readNote(*sA);
    std::printf("        读到 note = '%s'\n", afterCommit.c_str());

    std::printf("\n结论：\n");
    std::printf("  快照读（普通 SELECT）：'%s' → '%s'，**两次读到的一样**。\n",
                first.c_str(), second.c_str());
    std::printf("  即使 B 已经提交了修改，A 在自己的事务内看到的仍是旧版本 ——\n");
    std::printf("  这就是 MVCC：每行数据维护版本链，事务在第一次快照读时生成\n");
    std::printf("  ReadView，之后一律按这个 ReadView 判断该看哪个版本。\n\n");
    std::printf("  当前读（FOR UPDATE）：'%s'，**读到了最新值**。\n", current.c_str());
    std::printf("  因为当前读要加锁，必须基于最新数据，不能用快照。\n\n");
    std::printf("  事务提交后：'%s'，新事务生成新的 ReadView。\n", afterCommit.c_str());
}

// ── 实验四：死锁检测 ────────────────────────────────────────────────────────
void experimentDeadlock() {
    banner("实验四：死锁检测与自动回滚");

    auto sA = newSession("A");
    auto sB = newSession("B");
    auto sMon = newSession("监控");
    if (!sA || !sB || !sMon) return;

    resetTable(*sA);

    std::printf("构造经典的循环等待：\n");
    std::printf("  A 锁 id=1，然后想锁 id=3\n");
    std::printf("  B 锁 id=3，然后想锁 id=1\n\n");

    sA->begin();
    sB->begin();

    sA->execute("UPDATE lock_demo SET note='A1' WHERE id = 1");
    std::printf("[会话A] 已锁住 id=1\n");
    sB->execute("UPDATE lock_demo SET note='B3' WHERE id = 3");
    std::printf("[会话B] 已锁住 id=3\n\n");

    std::atomic<int> victimCount{0};
    std::string victimError;

    std::thread tA([&] {
        if (!sA->execute("UPDATE lock_demo SET note='A3' WHERE id = 3")) {
            ++victimCount;
            victimError = "A: " + sA->lastError();
        }
    });
    std::thread tB([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!sB->execute("UPDATE lock_demo SET note='B1' WHERE id = 1")) {
            ++victimCount;
            victimError = "B: " + sB->lastError();
        }
    });

    tA.join();
    tB.join();

    std::printf("被回滚的事务数：%d\n", victimCount.load());
    if (!victimError.empty()) {
        std::printf("错误信息：%s\n", victimError.c_str());
    }

    sA->rollback();
    sB->rollback();

    // 从 InnoDB 状态里取最近一次死锁的记录
    thgh::ResultSet rs;
    if (sMon->query("SHOW ENGINE INNODB STATUS", rs) && rs.rowCount() > 0) {
        const std::string& status = rs.at(0, 2);
        const std::size_t pos = status.find("LATEST DETECTED DEADLOCK");
        if (pos != std::string::npos) {
            std::printf("\n── SHOW ENGINE INNODB STATUS 中的死锁记录（节选）──\n");
            const std::size_t end = status.find("TRANSACTIONS", pos);
            std::string section = status.substr(
                pos, (end == std::string::npos ? 1400 : std::min<std::size_t>(
                                                            end - pos, 1400)));
            std::printf("%s\n", section.c_str());
        }
    }

    std::printf("\n结论：\n");
    std::printf("  InnoDB **主动检测**到了循环等待，并选一个事务作为牺牲者回滚，\n");
    std::printf("  而不是让两个事务一直互相等到超时。\n");
    std::printf("  选择依据大致是回滚代价最小（改动行数少的那个）。\n");
    std::printf("  应用层的正确姿态：捕获死锁错误（错误码 1213）后**重试**，\n");
    std::printf("  而不是直接把错误抛给用户 —— 死锁在高并发下是正常现象。\n");
    std::printf("  预防上最有效的是：让所有事务按**相同顺序**访问资源。\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_cfg.host = argv[1];
    if (argc > 2) g_cfg.user = argv[2];
    if (argc > 3) g_cfg.password = argv[3];
    if (argc > 4) g_cfg.database = argv[4];

    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    auto probe = newSession("探测");
    if (!probe) {
        return 1;
    }
    thgh::ResultSet rs;
    probe->query("SELECT @@version, @@transaction_isolation, @@innodb_lock_wait_timeout", rs);
    if (rs.rowCount() > 0) {
        std::printf("MySQL %s，默认隔离级别 %s，锁等待超时 %s 秒\n",
                    rs.at(0, 0).c_str(), rs.at(0, 1).c_str(), rs.at(0, 2).c_str());
    }
    probe.reset();

    experimentGapLock();
    experimentReadCommitted();
    experimentMvcc();
    experimentDeadlock();

    // 清理
    auto cleanup = newSession("清理");
    if (cleanup) {
        cleanup->execute("DROP TABLE IF EXISTS lock_demo");
    }

    std::printf("\n全部实验完成\n");
    return 0;
}
