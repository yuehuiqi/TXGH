# TXGH · 通信网络规划管控系统

面向多设备组网管理的通信网络规划管控系统，完成
**场景建模 → 规划任务下发 → 路径规划计算 → 结果回传与态势呈现** 的完整链路。

本仓库包含三部分：**Qt 客户端**、**自研 C++ 网络服务端**、**两端共享的协议层**。

> 本仓库为通用技术框架版本，已移除具体业务参数与涉密材料。

---

## 目录

- [快速开始](#快速开始)
- [系统架构](#系统架构)
- [协议](#协议)
- [目录结构](#目录结构)
- [构建](#构建)
- [运行](#运行)
- [测试与验证](#测试与验证)
- [性能数据](#性能数据)
- [核心设计取舍](#核心设计取舍)
- [常见问题](#常见问题)
- [已知边界](#已知边界)

---

## 快速开始

三条路径，按需求选一条。

### A. Docker 一键起（最省事）

```bash
cd deploy
cp .env.example .env          # 按需改凭据
docker compose up -d --build
docker compose ps             # 两个都 healthy 才算就绪
```

首次构建约 10~20 分钟（装工具链 + 编译 + 跑 175 个测试）。
MySQL 首次启动会自动建表并灌入演示数据，起完就能直接看到一个完整场景。

### B. 源码构建服务端（Linux / WSL）

```bash
./scripts/thgh.sh build       # 构建
./scripts/thgh.sh test        # 175 个测试
./scripts/thgh.sh daemon      # 后台启动
./scripts/thgh.sh verify      # 8 类端到端验证
```

### C. 客户端（Windows）

```bash
cmake -S . -B build -G "MinGW Makefiles" \
      -DCMAKE_PREFIX_PATH="C:/Qt/Qt5.12.12/5.12.12/mingw73_64"
cmake --build build -j4
./build/bin/TXGHClient.exe
```

客户端启动时先试连服务端（默认 `127.0.0.1:9000`），
**连不上会自动回退单机模式**（直连本地数据库），状态栏显示当前模式。

---

## 系统架构

```
   Windows                                    Linux 服务器
┌──────────────────┐                    ┌──────────────────────────────┐
│  Qt 客户端        │                    │  plan_server                 │
│                  │                    │                              │
│  SimBridge ──────┼──① 规划通道────────┼→ 主 Reactor（只 accept）      │
│  （推送式）       │   ack/progress/    │      ↓ 轮询分发               │
│                  │   result           │   从 Reactor ×N（epoll ET）   │
│  RemoteDataStore ┼──② 数据通道────────┼→     ↓ 分帧                   │
│  （请求应答式）   │   data_request/    │   MessageRouter（按类型分发） │
│                  │   data_reply       │      ↓            ↓          │
└──────────────────┘                    │  PlanService   DataService   │
                                        │      ↓            ↓          │
                                        │   ComputePool（计算线程 ×M）  │
                                        │      ↓            ↓          │
                                        │   规划算法      MySQL 连接池   │
                                        └──────────────────┬───────────┘
                                                           ↓
                                                    MySQL（5 张表）
```

### 服务端四层，依赖严格单向

```
service ──→ planner
   │           ↓
   └──────→   db
   ↑
  net
```

| 层 | 职责 |
| --- | --- |
| `net/` | epoll ET 事件循环、主从 Reactor、连接管理、小根堆定时器 |
| `service/` | 消息路由、规划编排、数据访问编排、计算线程池 |
| `planner/` | 坐标转换、图算法、流量分配（纯计算，可单测） |
| `db/` | 连接池、DAO、预处理语句与事务 |

### ★ 计算不在 IO 线程上

规划计算是纯 CPU 任务，单次可达数十毫秒。若在从 Reactor 里同步执行：

1. 会冻结该 Reactor 上的**全部**连接（不只是发起请求的那条）
2. 心跳应答发不出去，对端可能反过来判定服务端已死
3. **分阶段进度推送根本发不出去** —— 发它的线程正被计算本身占着

所以规划与数据库操作都派发到 `ComputePool`。IO 线程只负责收发。

### ★ 客户端建两条连接，不是一条

| 通道 | 类 | 模式 |
| --- | --- | --- |
| 规划 | `SimBridge` | **推送式**：发一次请求，服务端陆续推 ack → progress×N → result |
| 数据 | `RemoteDataStore` | **请求应答式**：发一条问一条，同步等回复 |

混在一条连接上，同步等数据回复时会先读到规划进度，需要缓存再转交 ——
而转交依赖 Qt 信号，在阻塞等待里转交等于重入。分开就没这个问题。

---

## 协议

行分隔（`\n`）的 JSON 长连接协议。TCP 是字节流没有消息边界，
接收侧用读写双游标做流式分帧，并对单条消息设上限 ——
没有上限的话，对端持续发不含分隔符的字节就能把内存吃光。

### 规划任务（推送式）

```
客户端 → start_plan
服务端 → ack          已受理，附 task_id 与解析到的规模
服务端 → progress     ×N，阶段名 + 百分比（单调不减）
服务端 → plan_result  最终结果
```

实测 150 节点 / 150 流：**ack 在 1.4ms 到达，结果在 115ms** ——
中间 113ms 原本是完全的黑盒。

进度消息**可丢**（节流 + 输出缓冲背压），结果消息**不可丢**。

### 数据访问（请求应答式）

```
客户端 → data_request  {"op":"node.addBatch","args":{...}}
服务端 → data_reply    {"ok":true,"data":{...}}
```

18 个操作覆盖场景/节点/链路/模板的 CRUD。

**批量接口是必需品而非便利方法**：导入场景原本是循环逐条写，
走协议后每条都是一次网络往返 —— 500 个节点实测 1533.9ms → **26.6ms（57.7 倍）**。

### 消息类型与字段名收口到共享层

全部定义在 [`protocol/include/thgh/message.h`](protocol/include/thgh/message.h)。
两端引用同一份常量 —— 否则一端写 `plan_result`、另一端写 `plan_results`，
编译器不报错、两边单测各自也都能过，**只有联调时才发现**。

---

## 目录结构

```
TXGH/
├── CMakeLists.txt       顶层构建：按平台决定编客户端还是服务端
│
├── protocol/            ★ 两端共享，零第三方依赖
│   ├── include/thgh/
│   │   ├── message.h       消息类型 + 字段名 + 18 个操作名
│   │   └── line_framer.h   行分帧器（粘包/半包）
│   ├── src/                对应实现
│   └── tests/              35 个单测
│
├── client/              Qt 客户端（Windows）
│   ├── main.cpp / mainwindow.*        入口与主窗口
│   ├── simbridge.*                    ★ 规划通道（推送式）
│   ├── idatastore.h                   ★ 数据访问接口
│   ├── remotedatastore.*              ★ 数据通道实现（走协议）
│   ├── dbmanager.*                    直连数据库实现（单机模式回退）
│   ├── modelcodec.*                   复合字段 JSON 编解码（两实现共用）
│   ├── topoview.* / devicetopologyview.*   态势图与拓扑编辑
│   ├── dialog*.（cpp/h/ui）            配置对话框
│   ├── qtnodes/                       第三方节点编辑器库
│   └── resources/ + res.qrc           界面资源
│
├── server/              C++ 服务端（只能 Linux 编，依赖 epoll）
│   ├── net/                ① 网络层
│   │   ├── event_loop.*       epoll ET 事件循环 + eventfd 唤醒
│   │   ├── channel.*          fd 的事件注册与分发
│   │   ├── acceptor.*         主 Reactor 接入（含 EMFILE 兜底）
│   │   ├── tcp_connection.*   连接：读写缓冲 + 分帧 + 优雅关闭
│   │   ├── tcp_server.*       主从 Reactor 装配 + 心跳巡检
│   │   ├── event_loop_thread_pool.*  从 Reactor 线程池
│   │   ├── buffer.* / socket.*       读写缓冲、fd 的 RAII 封装
│   │   └── timer_queue.*      小根堆定时器
│   │
│   ├── service/            ② 业务编排层
│   │   ├── message_router.*   按消息类型分发
│   │   ├── plan_service.*     规划业务（ack/progress/result）
│   │   ├── data_service.*     数据访问业务（18 个操作）
│   │   ├── compute_pool.*     ★ 计算线程池
│   │   └── json_codec.*       JSON 编解码（不抛异常）
│   │
│   ├── planner/            ③ 业务计算层
│   │   ├── geo.*              WGS84→ECEF 坐标转换
│   │   ├── graph.*            边级图 + Dijkstra + Yen K 短路
│   │   └── static_planner.*   拓扑构建 + 多路径流量分配
│   │
│   ├── db/                 ④ 数据层
│   │   ├── mysql_conn.*       连接 RAII + 预处理语句 + 事务
│   │   ├── conn_pool.*        连接池
│   │   ├── scene_dao.*        场景数据访问
│   │   ├── schema.sql         建表脚本（5 张表）
│   │   └── seed.sql           演示数据
│   │
│   ├── apps/               可执行程序
│   │   ├── plan_server.cpp    ★ 正式服务端
│   │   ├── echo_server.cpp    回显服务端（只验证网络层）
│   │   ├── planner_bench.cpp  规划算法性能实验
│   │   ├── db_bench.cpp       数据库实验
│   │   └── lock_experiment.cpp  InnoDB 锁实验
│   │
│   └── tests/              140 个单测
│
├── bench/               压测与端到端验证
│   ├── bench_client.cpp    网络层压测（QPS / P99）
│   ├── plan_client.cpp     规划协议验证（4 种模式）
│   └── data_client.cpp     数据协议验证（4 种模式）
│
├── deploy/              Dockerfile + docker-compose + .env.example
└── scripts/thgh.sh      运维脚本（构建/测试/启停/验证/压测/火焰图）
```

### 关于 JSON 库的切分

`protocol/` 刻意**不包含** JSON 序列化：客户端用 `QJsonDocument`，
服务端用 `nlohmann/json`。把 JSON 库塞进共享层等于强迫另一端也引入它。
两端各用各的库，但**字段名与操作名一律取自 `protocol/` 的常量**。

---

## 构建

### 依赖

| | 服务端（Linux） | 客户端（Windows） |
| --- | --- | --- |
| 系统 | Ubuntu 22.04+ | Windows 10+ |
| 编译器 | GCC 11+（C++17） | MinGW GCC 7.3（Qt 自带） |
| CMake | 3.16+ | 3.16+ |
| 依赖库 | `libmysqlclient-dev` `nlohmann-json3-dev` `libgtest-dev` `libgmock-dev` | Qt 5.12+：`Core Gui Widgets Sql Charts Network Svg OpenGL` |
| 数据库 | MySQL 8.0+ | —（远端模式不需要） |
| 可选 | `valgrind` `perf` `docker` | — |

```bash
sudo apt install -y build-essential cmake git \
    libgtest-dev libgmock-dev libmysqlclient-dev nlohmann-json3-dev
```

### 服务端

```bash
./scripts/thgh.sh build              # 等价于下面两行
```

<details>
<summary>原始命令</summary>

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DTHGH_BUILD_SERVER=ON -DTHGH_BUILD_CLIENT=OFF -DTHGH_BUILD_TESTS=ON
cmake --build build -j$(nproc)
```
</details>

其它构建类型：`./scripts/thgh.sh build asan` / `build tsan`（产物在 `build-asan/` `build-tsan/`）。

服务端使用 `epoll`，**只能在 Linux 构建**（Windows 的 IOCP 属 Proactor 模型，不是同一套东西）。

### 客户端

```bash
cmake -S . -B build -G "MinGW Makefiles" \
      -DCMAKE_PREFIX_PATH="C:/Qt/Qt5.12.12/5.12.12/mingw73_64"
cmake --build build -j4
```

> ⚠️ **路径中不能含中文**。MinGW 的 make 按系统 ANSI 代码页解析 Makefile 里的
> UTF-8 路径，中文目录会导致 `No rule to make target`，报错信息毫无提示性。

### 构建选项

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `THGH_BUILD_CLIENT` | Windows 上 ON | 构建 Qt 客户端 |
| `THGH_BUILD_SERVER` | Linux 上 ON | 构建 C++ 服务端 |
| `THGH_BUILD_TESTS` | ON | 构建 gtest 单元测试 |
| `THGH_BUILD_BENCH` | 同服务端 | 构建压测客户端 |
| `THGH_SANITIZE_ADDRESS` | OFF | ASan（仅 Linux，MinGW 不提供 libasan） |
| `THGH_SANITIZE_THREAD` | OFF | TSan（仅 Linux） |

> TSan 在 Ubuntu 24.04（内核 6.8）上需要 `setarch -R` 关闭 ASLR，否则启动即报
> `unexpected memory mapping`。顶层 CMakeLists 已用 `CMAKE_CROSSCOMPILING_EMULATOR`
> 自动包装测试进程，**不改动全局 sysctl**。

---

## 运行

### 准备数据库

**方式一：Docker**（推荐，自动建表 + 灌演示数据）

```bash
cd deploy && cp .env.example .env && docker compose up -d
```

**方式二：已有 MySQL**

```bash
mysql -u root -p -e "CREATE DATABASE IF NOT EXISTS thgh CHARACTER SET utf8mb4;"
mysql -u root -p -e "CREATE USER IF NOT EXISTS 'thgh'@'localhost' IDENTIFIED BY '你的密码';"
mysql -u root -p -e "GRANT ALL ON thgh.* TO 'thgh'@'localhost'; FLUSH PRIVILEGES;"
mysql -u thgh -p thgh < server/db/schema.sql
mysql -u thgh -p thgh < server/db/seed.sql      # 可选：演示数据
```

### 启动服务端

```bash
export THGH_DB_HOST=127.0.0.1 THGH_DB_USER=thgh THGH_DB_PASSWORD='你的密码' THGH_DB_NAME=thgh

./scripts/thgh.sh start        # 前台，Ctrl-C 停止
./scripts/thgh.sh daemon       # 后台，日志 /tmp/thgh_server.log
./scripts/thgh.sh stop
./scripts/thgh.sh status
```

<details>
<summary>原始命令与参数含义</summary>

```bash
./build/bin/plan_server <端口> <IO线程数> <计算线程数> <心跳超时秒>
./build/bin/plan_server 9000 3 4 60
```

| 参数 | 建议值 | 说明 |
| --- | --- | --- |
| 端口 | 9000 | |
| IO 线程数 | 核数 − 1 | 从 Reactor 数量，各持独立 epoll 实例 |
| 计算线程数 | 核数 | 跑规划算法与数据库操作 |
| 心跳超时秒 | 60 | 超过此时长无数据即判定半开连接并踢除 |

</details>

> **数据库凭据只从环境变量读，不接受命令行参数** ——
> 命令行会出现在 `ps aux` 和 `docker inspect` 里。
>
> **不设 `THGH_DB_HOST` 时数据访问功能不启用**，服务端仍可跑规划任务。
> 这样压测网络层不必先起数据库；数据请求会收到明确的
> "本服务端未启用数据访问功能（未配置数据库）"。

### 环境变量

| 变量 | 用途 | 默认 |
| --- | --- | --- |
| `THGH_DB_HOST` | 数据库地址。**不设则不启用数据访问** | — |
| `THGH_DB_PORT` | 数据库端口 | `3306` |
| `THGH_DB_USER` | 数据库用户 | `thgh` |
| `THGH_DB_PASSWORD` | 数据库密码 | 空 |
| `THGH_DB_NAME` | 库名 | `thgh` |
| `THGH_TEST_DB_*` | 同上，**测试专用**（不设则跳过 14 个 MySQL 用例） | — |

---

## 测试与验证

```bash
./scripts/thgh.sh test        # 175 个单测（自动带上数据库凭据）
./scripts/thgh.sh valgrind    # 9 个二进制跑 memcheck
./scripts/thgh.sh verify      # 8 类端到端验证
./scripts/thgh.sh bench       # 网络层压测（QPS / P99）
```

> ★ **测试一定要带数据库凭据。** 不带的话 ctest 会**静默跳过 14 个 MySQL 用例**，
> 但仍然显示 `175 tests passed` —— 这是个很容易误导人的绿色。
> `scripts/thgh.sh test` 会自动把 `THGH_DB_*` 转成 `THGH_TEST_DB_*` 传进去。
>
> 核实方法：`ctest --test-dir build | grep -c Skipped` 应为 **0**。

<details>
<summary>端到端验证的 8 个模式</summary>

```bash
# 规划协议
./build/bin/plan_client 127.0.0.1 9000 150 150 normal    # 消息时序断言
./build/bin/plan_client 127.0.0.1 9000 150 150 abort     # 计算中途断连
./build/bin/plan_client 127.0.0.1 9000 150 150 flood     # 过载拒绝
./build/bin/plan_client 127.0.0.1 9000 100 100 garbage   # 13 种畸形报文

# 数据访问
./build/bin/data_client 127.0.0.1 9000 crud              # 24 项逐项校验
./build/bin/data_client 127.0.0.1 9000 batch 500         # 批量 vs 逐条
./build/bin/data_client 127.0.0.1 9000 concurrent 16     # 并发无串数据
./build/bin/data_client 127.0.0.1 9000 garbage           # 含 SQL 注入尝试
```
</details>

| 检查项 | 状态 |
| --- | --- |
| 单元测试 | **175 个**（协议 35 + 服务端 140），带凭据时 **0 跳过** |
| ThreadSanitizer | 真实负载下**零数据竞争** |
| AddressSanitizer + LeakSanitizer | **零报告** |
| valgrind memcheck | 9 个二进制全部 `ERROR SUMMARY: 0` |
| 编译告警 | 两端 `-Wall -Wextra` **零告警** |

### ⚠️ 压测须知

**压测客户端必须与服务端同机运行。** 跨地域部署时 RTT 可达数十毫秒，
测出的 P99 全是网络往返延迟而非服务端性能。跨地域链路只用于功能联调。

---

## 性能数据

> 测量环境：4 核 / 14GB，Ubuntu 24.04.4（内核 6.8），GCC 13.3.0，
> MySQL 8.0.46，`RelWithDebInfo`。**压测客户端与服务端同机 loopback**。

### 网络层

| 指标 | 100 连接 | 1000 连接 |
| --- | --- | --- |
| QPS | 175,628 | 165,655 |
| P50 延迟 | 19.6 us | 20.8 us |
| **P99 延迟** | **53.9 us** | **57.7 us** |
| P99.9 延迟 | 80.0 us | 97.7 us |
| 错误数 | 0 | 0 |

连接数增至 10 倍，**QPS 仅降 5.7%** —— 体现 epoll 相对 select/poll 的扩展性。
128 字节消息，请求-应答模式，3 个从 Reactor，持续 10 秒。

### 规划计算

| 项 | 优化前 | 优化后 | 提升 |
| --- | --- | --- | --- |
| 单次最短路（800 节点 / 16896 边，200 次查询） | 82.81 ms | **33.84 ms** | 2.45x |
| 端到端 `plan()`（200 节点 / 200 条流） | 350.41 ms | **71.41 ms** | 4.9x |

优化来自 perf 定位：改造前 **60% 时间耗在 `unordered_map` 哈希查找**上
（`settled.find` 32.5% + `dist.find` 20.3% + `operator[]` 8.2%），算法本体仅 20.9%。
节点 id 本就连续，改用紧凑索引 + `vector` 直接寻址后，
热点占比回到 **79.4%**，哈希表完全从热点消失。

### 数据访问

| 项 | 结果 |
| --- | --- |
| 连接池化（获取连接） | 15918.9 us → **0.1 us** |
| 批量插入（DAO 层，500 节点） | 62.1 ms → **12.3 ms**（5.1x） |
| **批量写入（协议层，500 节点）** | 1533.9 ms → **26.6 ms（57.7x）**，往返 500 次 → 1 次 |
| 索引优化（40 万行） | 0.420 ms → **0.066 ms**（6.4x），`EXPLAIN` 由 `type=ALL/rows=2000` 变为 `type=ref/rows=1` |
| 间隙锁阻塞（RR） | 805 ms（会话 B 插入间隙内不存在的记录被阻塞至会话 A 提交） |
| 同场景 RC 隔离级别 | 0.3 ms 无阻塞 |

### 协议分阶段推送

150 节点 / 150 流：**首次反馈 115 ms → 1.4 ms**，中间由 43 条进度填充
（另有 111 条被节流跳过）。

40 并发过载下：**受理 27 个全部完成、拒绝 13 个**（队列上限 24 + 3 个执行中，
数字精确对上），零静默丢弃、零客户端干等。

计算期间断连：22 个受理、**20 个算完后因连接已断被正确丢弃**，服务端零崩溃。

### 火焰图

`plan_server` 在 120 节点 / 120 流连续规划下采样（cpu-clock 软件事件，2943 采样）。
完整交互式火焰图见 `../docsTXGH/assets/flame_plan_server.svg`。

| 分类 | 占比 |
| --- | --- |
| 规划算法（`dijkstra` / Yen / 堆操作） | 39.3% |
| **JSON 序列化**（nlohmann + grisu2 浮点转字符串） | **18.3%** |
| **内存分配**（malloc / free 家族） | **20.6%** |
| 其它 | 21.7% |

**序列化与内存分配合计 38.9%，已与算法本身相当。** 这与实测吻合：
150 节点场景服务端算完只用 36.3 ms，结果 118 ms 才送达 ——
差值花在序列化 17145 条链路并传输上。
**下一个优化目标是结果报文，不是算法**（按需拉取链路、或改用更紧凑的编码）。

---

## 核心设计取舍

| 决策 | 理由 |
| --- | --- |
| 计算**移出 IO 线程** | 单次 71ms 的计算会冻结该 Reactor 上的全部连接，而且进度推送根本发不出去 —— 发它的线程正被计算占着 |
| 计算队列**有界，满则拒绝** | 无界队列过载时"看起来还活着但内存一直涨"，最后被 OOM killer 干掉。快速失败优于慢速崩溃 |
| **不阻塞等待**队列空位 | 调用方是 IO 线程，阻塞它等于把整个 Reactor 拖下水 |
| 跨线程连接引用用 **`weak_ptr`** | 持 `shared_ptr` 会让连接被计算线程续命、fd 迟迟不释放，高并发下就是 fd 泄漏 |
| 进度**可丢**、结果**不可丢** | 丢几条进度只是进度条跳一下；堆爆内存是事故 |
| 客户端数据访问**同步 + 硬超时** | 改造前 `QSqlQuery::exec()` 本来就阻塞 GUI 线程，换成网络不是新增阻塞；全改异步要重写 70 处调用点 |
| 同步等待用 **`waitForReadyRead`** 而非 `QEventLoop` | 后者会继续派发 GUI 事件，用户能在等待期间点按钮、关窗口，导致重入或对象被析构 |
| 数据库操作全走**预处理语句** | 参数来自网络是不可信输入；端到端测试真的发过 `'); DROP TABLE scenes; --`，它被当作普通字符串存下，表完好 |
| 凭据只从**环境变量**读 | 命令行会出现在 `ps aux` 和 `docker inspect` 里 |
| **不支持计算中途取消** | 真正的取消要在算法热循环插检查点，会污染实现也拖慢它。计算是有界时间的纯 CPU 任务，跑完丢弃结果代价可接受 |

---

## 常见问题

### 服务端起不来

```
[ConnectionPool] 建立连接失败: mysql_real_connect: [1698] Access denied for user 'root'@'localhost'
```
MySQL 的 `root` 默认用 `auth_socket` 认证，**只能本地 socket 登录、不走 TCP 密码认证**。
用专门的应用账号，不要用 root。

---

```
[plan] 未设置 THGH_DB_HOST，数据访问功能未启用
```
不是错误。没配数据库时服务端仍能跑规划，只是数据请求会被拒绝。

---

```
启动失败：端口 9000 可能已被占用
```
```bash
./scripts/thgh.sh status     # 看有没有已经在跑的
./scripts/thgh.sh stop
ss -tlnp | grep 9000         # 或者看是谁占着
```

### `ctest` 说 175 个都过了，但我改的数据库代码没被测到

大概率是 14 个 MySQL 用例被静默跳过了。确认：

```bash
ctest --test-dir build | grep -c Skipped     # 应该是 0
```
不是 0 就是没带凭据，用 `./scripts/thgh.sh test`。

### TSan 报 `unexpected memory mapping`

Ubuntu 24.04（内核 6.8）默认 `vm.mmap_rnd_bits=32` 与 TSan 的地址空间布局冲突。
仓库已用 `setarch -R` 自动处理；**手工跑二进制**时需要自己加：

```bash
setarch -R ./build-tsan/bin/plan_server 9000 3 4 60
```
不要改全局 `sysctl`，那会降低整机的 ASLR 强度。

### valgrind 下 `test_compute_pool` 失败但 `ERROR SUMMARY: 0`

已修复。根因是 valgrind 的 memcheck **把多线程串行化执行**（实测 4 个任务
全落在同 1 个线程上），"并行比串行快"这类断言在它下面不可能成立。
当前代码会检测 `LD_PRELOAD` 识别 valgrind 并跳过并行度断言。

### `docker compose up` 后服务端反复重启

```bash
docker compose logs server
```
若是连不上数据库，检查 `.env` 里的凭据与 MySQL 容器是否一致。
`depends_on` 用的是 `condition: service_healthy` —— 如果 MySQL 一直不 healthy，
服务端根本不会启动，`docker compose ps` 会显示 `Created` 而非 `Up`。

### 改了代码但容器里还是旧的

```bash
docker compose up -d --build     # 要加 --build
```

### 客户端一直是单机模式

说明连不上服务端。检查服务端是否在跑、端口是否通。
客户端的服务端地址在 `client/mainwindow.h` 的 `m_serverHost` / `m_serverPort`。

### 运行时警告 `No matching signal for on_btnDeviceDetails_clicked()`

已知问题，无害。`client/dialogdevice.{cpp,h,ui}` 是历史遗留文件，
`.ui` 里留了按钮但对应的槽没被编译进来（原 `TXGH.pro` 就未纳入构建，
CMake 迁移时保持了一致行为）。

---

## 已知边界

- **路径规划算法**由算法侧以 Python 给出原型，本仓库 `server/planner/` 是其
  **C++ 移植实现**。移植中修正了原型 Yen 算法的偏离点 off-by-one
  （原实现产出的并非真正的 K 短路），正确性用暴力枚举参考实现验证
  （750 组随机用例逐条吻合）。
- **客户端数据访问是同步的**（请求即阻塞至响应或超时）。改造前直连数据库时
  同样阻塞 GUI 线程，因此不是新引入的阻塞，风险由硬超时兜底。
  **若单次操作的数据量涨到需要秒级传输，应重新考虑异步化。**
- **规划计算不支持中途取消**，当前是让它跑完再丢弃结果。
  **若单次计算达到秒级，此取舍需重新评估。**
- `client/dialogdevice.*` 与 `client/TXGH.pro` 是历史遗留，未纳入构建。
- 演示数据（`server/db/seed.sql`）是**示意数据**，坐标不对应任何真实部署。

---

## 详细改造记录

每阶段的设计取舍、踩过的坑、实测数据与面试追问预案，
见 [`../docsTXGH/`](../docsTXGH/)（P0~P7 共 9 篇）。
