# ─────────────────────────────────────────────────────────────────────────────
# server —— C++ 网络服务端
#
# 只能在 Linux 构建：核心是 epoll，Windows 上没有对应物
# （Windows 的 IOCP 属于 Proactor 模型，不是同一套东西）。
#
# 分层（P2~P4 逐步填充）：
#   net/      EventLoop(epoll ET) / Acceptor / Connection / 小根堆定时器
#   db/       MySQL 连接池 + DAO
#   planner/  规划算法 C++ 实现（由算法侧 Python 原型移植）
# ─────────────────────────────────────────────────────────────────────────────

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "server 依赖 epoll，只能在 Linux 上构建")
endif()

find_package(Threads REQUIRED)

add_library(thgh_server_core STATIC
    net/timer_queue.cpp   # 小根堆定时器（心跳超时踢除）
    net/buffer.cpp        # 连接读写缓冲（ET 模式必需）
    net/socket.cpp        # fd 的 RAII 封装 + socket 选项
    net/channel.cpp       # fd 的事件注册与分发
    net/event_loop.cpp    # epoll ET 事件循环 + eventfd 唤醒
    net/acceptor.cpp      # 主 Reactor 连接接入（含 EMFILE 兜底）
    net/tcp_connection.cpp        # 连接：ET 循环读写 + 分帧 + 优雅关闭
    net/event_loop_thread_pool.cpp # 从 Reactor 线程池
    net/tcp_server.cpp    # 主从 Reactor 装配 + 心跳巡检
    db/mysql_conn.cpp     # MySQL 连接 RAII 封装 + 预处理语句 + 事务
    db/conn_pool.cpp      # 连接池：复用、健康检查、超时获取
    db/scene_dao.cpp      # 场景数据访问：事务原子性 + 批量插入
    planner/geo.cpp       # WGS84 → ECEF 坐标转换与三维距离
    planner/graph.cpp     # 边级图 + Dijkstra + Yen K 最短路
    planner/static_planner.cpp # 拓扑构建 + 多路径流量分配
    service/compute_pool.cpp   # 计算线程池：把规划算法挪出 IO 线程
    service/json_codec.cpp     # 协议消息 JSON 编解码（不抛异常）
    service/plan_service.cpp   # 业务编排：Ack → Progress×N → Result
    service/data_service.cpp   # 数据访问编排：CRUD 收归服务端
    service/message_router.cpp # 按消息类型分发到各业务处理器
)

target_include_directories(thgh_server_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# MySQL 客户端库。优先用 pkg-config（Ubuntu 的 libmysqlclient-dev 提供），
# 找不到就退回手工指定，避免在没装 pkg-config 的环境里直接失败。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MYSQLCLIENT QUIET mysqlclient)
endif()

target_link_libraries(thgh_server_core PUBLIC thgh_protocol Threads::Threads)

if(MYSQLCLIENT_FOUND)
    target_include_directories(thgh_server_core PUBLIC ${MYSQLCLIENT_INCLUDE_DIRS})
    target_link_libraries(thgh_server_core PUBLIC ${MYSQLCLIENT_LIBRARIES})
    message(STATUS "server: 使用 pkg-config 找到 mysqlclient")
else()
    target_link_libraries(thgh_server_core PUBLIC mysqlclient)
    message(STATUS "server: 未通过 pkg-config 找到 mysqlclient，直接链接 -lmysqlclient")
endif()
target_compile_options(thgh_server_core PRIVATE -Wall -Wextra)

# 回显服务端：P2 网络核心的端到端验证程序
add_executable(echo_server apps/echo_server.cpp)
target_link_libraries(echo_server PRIVATE thgh_server_core)

# 数据库量化实验：连接池收益、事务原子性、批量插入、索引优化
add_executable(db_bench apps/db_bench.cpp)
target_link_libraries(db_bench PRIVATE thgh_server_core)

# 规划服务端：完整四层（网络 → 协议 → 业务计算 → 数据）
add_executable(plan_server apps/plan_server.cpp)
target_link_libraries(plan_server PRIVATE thgh_server_core)

# 规划算法性能实验：Dijkstra 两种实现对照、Yen 规模曲线、端到端 plan()
add_executable(planner_bench apps/planner_bench.cpp)
target_link_libraries(planner_bench PRIVATE thgh_server_core)

# InnoDB 锁与隔离级别实验：间隙锁、MVCC、死锁检测
add_executable(lock_experiment apps/lock_experiment.cpp)
target_link_libraries(lock_experiment PRIVATE thgh_server_core)

if(THGH_BUILD_TESTS)
    add_subdirectory(tests)
endif()
