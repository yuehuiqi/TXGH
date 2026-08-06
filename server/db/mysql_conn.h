#ifndef THGH_SERVER_DB_MYSQL_CONN_H
#define THGH_SERVER_DB_MYSQL_CONN_H

// ─────────────────────────────────────────────────────────────────────────────
// MySqlConnection —— 单条 MySQL 连接的 RAII 封装
//
// 直接用 libmysqlclient 的 C API，不引入 ORM。理由：
//   1. 这个项目的 SQL 都很直白（5 张表的 CRUD），ORM 带来的抽象收益有限
//   2. 面试会追问"索引怎么生效""事务怎么控制"，直接写 SQL 才说得清
//   3. C API 的资源管理（MYSQL*、MYSQL_RES、MYSQL_STMT）正是 RAII 的用武之地，
//      漏一个 mysql_free_result 就是内存泄漏
//
// ── 关于 SQL 注入 ─────────────────────────────────────────────────────────
// 凡是把外部输入拼进 SQL 的地方一律用**预处理语句**（prepared statement）：
// 参数值走独立的二进制协议通道传输，不参与 SQL 文本解析，
// 因此从原理上不可能改变语句结构 —— 这比转义可靠得多，
// 转义只要漏一处、或者字符集处理出偏差就会被绕过。
//
// 线程安全：**无**。一个实例只能被一个线程使用，这也是连接池存在的意义。
// libmysqlclient 的 MYSQL 句柄本身不支持并发调用。
// ─────────────────────────────────────────────────────────────────────────────

#include <mysql/mysql.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace thgh {

// 查询结果：按行存字符串。
// 用字符串而不是 variant：本项目的读路径要么直接回填 JSON、要么做数值转换，
// 统一成字符串反而少一层类型分发。
class ResultSet {
public:
    ResultSet() = default;

    std::size_t rowCount() const { return m_rows.size(); }
    std::size_t columnCount() const { return m_columns.size(); }
    const std::vector<std::string>& columns() const { return m_columns; }

    const std::string& at(std::size_t row, std::size_t col) const {
        return m_rows[row][col];
    }
    const std::vector<std::string>& row(std::size_t r) const { return m_rows[r]; }

    // NULL 与空字符串必须区分：数据库里 NULL 表示"没有值"，
    // 空字符串表示"值就是空的"，混淆会导致回填 JSON 时语义出错
    bool isNull(std::size_t row, std::size_t col) const {
        return m_nulls[row][col];
    }

    void addColumn(std::string name) { m_columns.push_back(std::move(name)); }
    void addRow(std::vector<std::string> values, std::vector<bool> nulls) {
        m_rows.push_back(std::move(values));
        m_nulls.push_back(std::move(nulls));
    }
    void clear() {
        m_columns.clear();
        m_rows.clear();
        m_nulls.clear();
    }

private:
    std::vector<std::string> m_columns;
    std::vector<std::vector<std::string>> m_rows;
    std::vector<std::vector<bool>> m_nulls;
};

// 预处理语句的参数。只支持本项目实际用到的三种类型。
struct Param {
    enum class Type { Int64, Double, String, Null };
    Type type = Type::Null;
    std::int64_t i = 0;
    double d = 0.0;
    std::string s;

    static Param ofInt(std::int64_t v) {
        Param p;
        p.type = Type::Int64;
        p.i = v;
        return p;
    }
    static Param ofDouble(double v) {
        Param p;
        p.type = Type::Double;
        p.d = v;
        return p;
    }
    static Param ofString(std::string v) {
        Param p;
        p.type = Type::String;
        p.s = std::move(v);
        return p;
    }
    static Param ofNull() { return Param{}; }
};

struct MySqlConfig {
    std::string host = "127.0.0.1";
    unsigned port = 3306;
    std::string user = "thgh";
    std::string password;
    std::string database = "thgh";
    std::string charset = "utf8mb4";
    unsigned connectTimeoutSec = 5;
    unsigned readTimeoutSec = 10;
    unsigned writeTimeoutSec = 10;
    // 自动重连：**刻意关闭**。
    // libmysqlclient 的自动重连会静默地建一条新连接，
    // 但新连接上没有原来的会话状态 —— 未提交的事务被丢弃、
    // 临时表和会话变量全没了，却不会报错。
    // 这种"看似成功实则数据丢失"比直接失败危险得多，
    // 连接健康检查交给连接池显式做。
    bool autoReconnect = false;
};

class MySqlConnection {
public:
    MySqlConnection();
    ~MySqlConnection();

    MySqlConnection(const MySqlConnection&) = delete;
    MySqlConnection& operator=(const MySqlConnection&) = delete;

    bool connect(const MySqlConfig& cfg);
    void close();
    bool connected() const { return m_mysql != nullptr; }

    // 探活。连接池取出空闲连接前用它确认可用性 ——
    // MySQL 服务端有 wait_timeout（默认 8 小时）会主动断开长时间空闲的连接，
    // 不检查就会把一条已死的连接交给业务，表现为随机的查询失败。
    bool ping();

    // ── 执行 ────────────────────────────────────────────────────────────
    // 无参数的语句（DDL、内部固定 SQL）。**不要用它拼接外部输入。**
    bool execute(const std::string& sql);
    bool query(const std::string& sql, ResultSet& out);

    // 带参数的语句，走预处理协议，从原理上杜绝 SQL 注入
    bool executePrepared(const std::string& sql, const std::vector<Param>& params);
    bool queryPrepared(const std::string& sql, const std::vector<Param>& params,
                       ResultSet& out);

    // 最近一次 INSERT 产生的自增主键
    std::uint64_t lastInsertId() const;
    // 最近一次语句影响的行数
    std::uint64_t affectedRows() const;

    // ── 事务 ────────────────────────────────────────────────────────────
    bool begin();
    bool commit();
    bool rollback();
    bool inTransaction() const { return m_inTransaction; }

    // ── 错误 ────────────────────────────────────────────────────────────
    const std::string& lastError() const { return m_lastError; }
    unsigned lastErrno() const { return m_lastErrno; }

    MYSQL* raw() { return m_mysql; }

private:
    bool bindAndExecute(const std::string& sql, const std::vector<Param>& params,
                        ResultSet* out);
    void captureError(const char* where);

    MYSQL* m_mysql = nullptr;
    bool m_inTransaction = false;
    std::string m_lastError;
    unsigned m_lastErrno = 0;
};

// ── 事务的 RAII 守卫 ────────────────────────────────────────────────────────
// 析构时若尚未 commit 则自动 rollback。
// 意义：多语句写入中途 return 或抛异常时，不会留下一个悬着的未提交事务
// 占着行锁 —— 那会阻塞其它会话直到连接超时，是很典型的线上事故。
class Transaction {
public:
    explicit Transaction(MySqlConnection& conn);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool begun() const { return m_begun; }
    bool commit();
    void rollback();

private:
    MySqlConnection& m_conn;
    bool m_begun = false;
    bool m_finished = false;
};

}  // namespace thgh

#endif  // THGH_SERVER_DB_MYSQL_CONN_H
