#include "db/mysql_conn.h"

#include <cstring>
#include <mutex>

namespace thgh {
namespace {

// libmysqlclient 要求全进程初始化一次。用函数内静态变量保证只做一次，
// C++11 起这个初始化本身是线程安全的（magic static）。
void ensureLibraryInit() {
    static const bool ok = [] {
        return ::mysql_library_init(0, nullptr, nullptr) == 0;
    }();
    (void)ok;
}

// 预处理语句结果绑定用的缓冲。
// MySQL 的 C API 要求调用方预先给出缓冲区，字符串类型只能先给一个估计长度，
// 不够时用 mysql_stmt_fetch_column 单独取回 —— 这里统一按需扩容后重取。
// 注意用 bool 而不是 my_bool：
// my_bool 是 MySQL 5.7 时代的 typedef，**8.0 已将其移除**，改用标准 bool。
// 网上大量教程仍在用 my_bool，照抄会直接编译失败。
struct OutBind {
    std::vector<char> buffer;
    unsigned long length = 0;
    bool isNull = false;
    bool error = false;
};

}  // namespace

// ── MySqlConnection ─────────────────────────────────────────────────────────

MySqlConnection::MySqlConnection() { ensureLibraryInit(); }

MySqlConnection::~MySqlConnection() { close(); }

bool MySqlConnection::connect(const MySqlConfig& cfg) {
    close();

    m_mysql = ::mysql_init(nullptr);
    if (m_mysql == nullptr) {
        m_lastError = "mysql_init 失败（内存不足）";
        return false;
    }

    ::mysql_options(m_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &cfg.connectTimeoutSec);
    ::mysql_options(m_mysql, MYSQL_OPT_READ_TIMEOUT, &cfg.readTimeoutSec);
    ::mysql_options(m_mysql, MYSQL_OPT_WRITE_TIMEOUT, &cfg.writeTimeoutSec);
    ::mysql_options(m_mysql, MYSQL_SET_CHARSET_NAME, cfg.charset.c_str());

    // 这里**刻意不调用** mysql_options(MYSQL_OPT_RECONNECT)：
    //   1. MySQL 8.0 默认就是关闭自动重连，无需显式设置
    //   2. 该选项在 8.0.34 起被标记为废弃，每次调用都会往 stderr 打警告，
    //      在连接池预建连接时会刷屏
    // 废弃的理由恰好和我们关闭它的理由一致：自动重连会静默地建一条新连接，
    // 但新连接上没有原来的会话状态 —— 未提交的事务被丢弃、临时表和会话变量
    // 全没了，却不报任何错。连接健康检查由连接池显式做（见 conn_pool.cpp）。
    (void)cfg.autoReconnect;

    if (::mysql_real_connect(m_mysql, cfg.host.c_str(), cfg.user.c_str(),
                             cfg.password.c_str(), cfg.database.c_str(),
                             cfg.port, nullptr, 0) == nullptr) {
        captureError("mysql_real_connect");
        ::mysql_close(m_mysql);
        m_mysql = nullptr;
        return false;
    }

    m_inTransaction = false;
    return true;
}

void MySqlConnection::close() {
    if (m_mysql != nullptr) {
        // 还挂着未提交的事务就直接关连接，InnoDB 会自动回滚，
        // 但显式回滚更清晰，也避免依赖隐式行为
        if (m_inTransaction) {
            ::mysql_rollback(m_mysql);
            m_inTransaction = false;
        }
        ::mysql_close(m_mysql);
        m_mysql = nullptr;
    }
}

bool MySqlConnection::ping() {
    if (m_mysql == nullptr) {
        return false;
    }
    if (::mysql_ping(m_mysql) != 0) {
        captureError("mysql_ping");
        return false;
    }
    return true;
}

bool MySqlConnection::execute(const std::string& sql) {
    if (m_mysql == nullptr) {
        m_lastError = "未连接";
        return false;
    }
    // 用 mysql_real_query 而不是 mysql_query：前者接受长度参数，
    // 因此是二进制安全的（SQL 里含 \0 也不会被截断）
    if (::mysql_real_query(m_mysql, sql.c_str(),
                           static_cast<unsigned long>(sql.size())) != 0) {
        captureError("mysql_real_query");
        return false;
    }
    // 即使是 INSERT/UPDATE 也要把结果集取走并释放，
    // 否则连接会停留在"有未读结果"的状态，下一条语句直接失败
    MYSQL_RES* res = ::mysql_store_result(m_mysql);
    if (res != nullptr) {
        ::mysql_free_result(res);
    }
    return true;
}

bool MySqlConnection::query(const std::string& sql, ResultSet& out) {
    out.clear();
    if (m_mysql == nullptr) {
        m_lastError = "未连接";
        return false;
    }
    if (::mysql_real_query(m_mysql, sql.c_str(),
                           static_cast<unsigned long>(sql.size())) != 0) {
        captureError("mysql_real_query");
        return false;
    }

    MYSQL_RES* res = ::mysql_store_result(m_mysql);
    if (res == nullptr) {
        // 无结果集的语句（如 DDL）也走这里，不算错误
        if (::mysql_field_count(m_mysql) == 0) {
            return true;
        }
        captureError("mysql_store_result");
        return false;
    }

    const unsigned numFields = ::mysql_num_fields(res);
    MYSQL_FIELD* fields = ::mysql_fetch_fields(res);
    for (unsigned i = 0; i < numFields; ++i) {
        out.addColumn(fields[i].name);
    }

    MYSQL_ROW row;
    while ((row = ::mysql_fetch_row(res)) != nullptr) {
        // 必须用 mysql_fetch_lengths 而不是 strlen：
        // 字段值可能含 \0（BLOB 或二进制数据），strlen 会截断
        unsigned long* lengths = ::mysql_fetch_lengths(res);
        std::vector<std::string> values;
        std::vector<bool> nulls;
        values.reserve(numFields);
        nulls.reserve(numFields);
        for (unsigned i = 0; i < numFields; ++i) {
            if (row[i] == nullptr) {
                values.emplace_back();
                nulls.push_back(true);
            } else {
                values.emplace_back(row[i], lengths[i]);
                nulls.push_back(false);
            }
        }
        out.addRow(std::move(values), std::move(nulls));
    }

    ::mysql_free_result(res);
    return true;
}

bool MySqlConnection::executePrepared(const std::string& sql,
                                      const std::vector<Param>& params) {
    return bindAndExecute(sql, params, nullptr);
}

bool MySqlConnection::queryPrepared(const std::string& sql,
                                    const std::vector<Param>& params,
                                    ResultSet& out) {
    out.clear();
    return bindAndExecute(sql, params, &out);
}

bool MySqlConnection::bindAndExecute(const std::string& sql,
                                     const std::vector<Param>& params,
                                     ResultSet* out) {
    if (m_mysql == nullptr) {
        m_lastError = "未连接";
        return false;
    }

    MYSQL_STMT* stmt = ::mysql_stmt_init(m_mysql);
    if (stmt == nullptr) {
        captureError("mysql_stmt_init");
        return false;
    }
    // 用 unique_ptr 保证任何返回路径都会释放，
    // 漏一个 mysql_stmt_close 就是句柄泄漏
    std::unique_ptr<MYSQL_STMT, decltype(&::mysql_stmt_close)> guard(
        stmt, &::mysql_stmt_close);

    if (::mysql_stmt_prepare(stmt, sql.c_str(),
                             static_cast<unsigned long>(sql.size())) != 0) {
        m_lastError = std::string("mysql_stmt_prepare: ") +
                      ::mysql_stmt_error(stmt);
        m_lastErrno = ::mysql_stmt_errno(stmt);
        return false;
    }

    // ── 绑定输入参数 ────────────────────────────────────────────────────
    std::vector<MYSQL_BIND> binds(params.size());
    std::vector<unsigned long> lengths(params.size(), 0);
    // 用 char 数组而不是 std::vector<bool>：后者是位压缩特化，
    // 元素不是独立对象、无法取地址交给 C API
    std::vector<char> nullFlags(params.size(), 0);
    std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * binds.size());

    // 注意：MYSQL_BIND 里存的是**指针**，指向的对象必须活到 execute 之后。
    // params 是 const 引用、生命周期覆盖本函数，所以可以直接取地址；
    // 若这里改成从临时对象取地址就会是悬垂指针。
    for (std::size_t i = 0; i < params.size(); ++i) {
        const Param& p = params[i];
        switch (p.type) {
            case Param::Type::Int64:
                binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                binds[i].buffer = const_cast<std::int64_t*>(&p.i);
                break;
            case Param::Type::Double:
                binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                binds[i].buffer = const_cast<double*>(&p.d);
                break;
            case Param::Type::String:
                lengths[i] = static_cast<unsigned long>(p.s.size());
                binds[i].buffer_type = MYSQL_TYPE_STRING;
                binds[i].buffer = const_cast<char*>(p.s.data());
                binds[i].buffer_length = lengths[i];
                binds[i].length = &lengths[i];
                break;
            case Param::Type::Null:
                nullFlags[i] = 1;
                binds[i].buffer_type = MYSQL_TYPE_NULL;
                binds[i].is_null = reinterpret_cast<bool*>(&nullFlags[i]);
                break;
        }
    }

    if (!params.empty() && ::mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        m_lastError = std::string("mysql_stmt_bind_param: ") +
                      ::mysql_stmt_error(stmt);
        return false;
    }

    if (::mysql_stmt_execute(stmt) != 0) {
        m_lastError =
            std::string("mysql_stmt_execute: ") + ::mysql_stmt_error(stmt);
        m_lastErrno = ::mysql_stmt_errno(stmt);
        return false;
    }

    if (out == nullptr) {
        return true;  // 写语句，不需要取结果
    }

    // ── 取结果集 ────────────────────────────────────────────────────────
    MYSQL_RES* meta = ::mysql_stmt_result_metadata(stmt);
    if (meta == nullptr) {
        return true;  // 没有结果集
    }
    std::unique_ptr<MYSQL_RES, decltype(&::mysql_free_result)> metaGuard(
        meta, &::mysql_free_result);

    const unsigned numFields = ::mysql_num_fields(meta);
    MYSQL_FIELD* fields = ::mysql_fetch_fields(meta);
    for (unsigned i = 0; i < numFields; ++i) {
        out->addColumn(fields[i].name);
    }

    std::vector<OutBind> outBufs(numFields);
    std::vector<MYSQL_BIND> resBinds(numFields);
    std::memset(resBinds.data(), 0, sizeof(MYSQL_BIND) * numFields);

    for (unsigned i = 0; i < numFields; ++i) {
        // 初始缓冲给 256 字节。不够时下面用 mysql_stmt_fetch_column 单独重取，
        // 避免为了 TEXT 字段给每列都预分配几 KB
        outBufs[i].buffer.resize(256);
        resBinds[i].buffer_type = MYSQL_TYPE_STRING;
        resBinds[i].buffer = outBufs[i].buffer.data();
        resBinds[i].buffer_length =
            static_cast<unsigned long>(outBufs[i].buffer.size());
        resBinds[i].length = &outBufs[i].length;
        resBinds[i].is_null = &outBufs[i].isNull;
        resBinds[i].error = &outBufs[i].error;
    }

    if (::mysql_stmt_bind_result(stmt, resBinds.data()) != 0) {
        m_lastError =
            std::string("mysql_stmt_bind_result: ") + ::mysql_stmt_error(stmt);
        return false;
    }
    // store_result 把整个结果集拉到客户端，之后 fetch 不再走网络。
    // 代价是内存，好处是连接能尽快脱离"有未读结果"的状态 ——
    // 对连接池场景很重要，否则连接归还后下一个使用者会直接失败。
    if (::mysql_stmt_store_result(stmt) != 0) {
        m_lastError =
            std::string("mysql_stmt_store_result: ") + ::mysql_stmt_error(stmt);
        return false;
    }

    for (;;) {
        const int rc = ::mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) {
            break;
        }
        if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
            m_lastError =
                std::string("mysql_stmt_fetch: ") + ::mysql_stmt_error(stmt);
            return false;
        }

        std::vector<std::string> values;
        std::vector<bool> nulls;
        values.reserve(numFields);
        nulls.reserve(numFields);

        for (unsigned i = 0; i < numFields; ++i) {
            if (outBufs[i].isNull) {
                values.emplace_back();
                nulls.push_back(true);
                continue;
            }
            const unsigned long len = outBufs[i].length;
            if (len > outBufs[i].buffer.size()) {
                // 缓冲不够，扩容后针对这一列重新取
                outBufs[i].buffer.resize(len);
                MYSQL_BIND rebind;
                std::memset(&rebind, 0, sizeof(rebind));
                rebind.buffer_type = MYSQL_TYPE_STRING;
                rebind.buffer = outBufs[i].buffer.data();
                rebind.buffer_length = len;
                rebind.length = &outBufs[i].length;
                rebind.is_null = &outBufs[i].isNull;
                if (::mysql_stmt_fetch_column(stmt, &rebind, i, 0) != 0) {
                    m_lastError = std::string("mysql_stmt_fetch_column: ") +
                                  ::mysql_stmt_error(stmt);
                    return false;
                }
            }
            values.emplace_back(outBufs[i].buffer.data(), len);
            nulls.push_back(false);
        }
        out->addRow(std::move(values), std::move(nulls));
    }

    return true;
}

std::uint64_t MySqlConnection::lastInsertId() const {
    return m_mysql != nullptr ? ::mysql_insert_id(m_mysql) : 0;
}

std::uint64_t MySqlConnection::affectedRows() const {
    return m_mysql != nullptr ? ::mysql_affected_rows(m_mysql) : 0;
}

bool MySqlConnection::begin() {
    if (m_mysql == nullptr) {
        m_lastError = "未连接";
        return false;
    }
    if (m_inTransaction) {
        // MySQL 不支持嵌套事务，BEGIN 会隐式提交上一个 —— 那是静默的数据风险，
        // 这里直接拒绝而不是放行
        m_lastError = "已在事务中，MySQL 不支持嵌套事务";
        return false;
    }
    if (::mysql_real_query(m_mysql, "START TRANSACTION", 17) != 0) {
        captureError("START TRANSACTION");
        return false;
    }
    m_inTransaction = true;
    return true;
}

bool MySqlConnection::commit() {
    if (!m_inTransaction) {
        return true;
    }
    const bool ok = ::mysql_commit(m_mysql) == 0;
    if (!ok) {
        captureError("mysql_commit");
    }
    m_inTransaction = false;
    return ok;
}

bool MySqlConnection::rollback() {
    if (!m_inTransaction) {
        return true;
    }
    const bool ok = ::mysql_rollback(m_mysql) == 0;
    if (!ok) {
        captureError("mysql_rollback");
    }
    m_inTransaction = false;
    return ok;
}

void MySqlConnection::captureError(const char* where) {
    m_lastErrno = ::mysql_errno(m_mysql);
    m_lastError = std::string(where) + ": [" + std::to_string(m_lastErrno) +
                  "] " + ::mysql_error(m_mysql);
}

// ── Transaction ─────────────────────────────────────────────────────────────

Transaction::Transaction(MySqlConnection& conn) : m_conn(conn) {
    m_begun = m_conn.begin();
}

Transaction::~Transaction() {
    // 未显式提交就回滚。这是本类存在的全部意义：
    // 多语句写入中途 return 或抛异常时，不会留下悬着的未提交事务占着行锁。
    if (m_begun && !m_finished) {
        m_conn.rollback();
    }
}

bool Transaction::commit() {
    if (!m_begun || m_finished) {
        return false;
    }
    m_finished = true;
    return m_conn.commit();
}

void Transaction::rollback() {
    if (!m_begun || m_finished) {
        return;
    }
    m_finished = true;
    m_conn.rollback();
}

}  // namespace thgh
