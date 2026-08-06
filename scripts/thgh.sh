#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# THGH 服务端运维脚本
#
# 之前这些操作散落在 /root/ 下的几个一次性脚本里（start_p6.sh、flame.sh…），
# 路径写死、别人拿不到、换台机器就得重写。收进仓库统一管理。
#
# 用法：scripts/thgh.sh <命令> [参数]
# 所有命令都从仓库根目录推断路径，不依赖当前工作目录。
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# 仓库根目录 = 本脚本所在目录的上一级
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
BIN="${BUILD}/bin"

# 数据库凭据：优先用已有环境变量，否则用开发默认值。
# 生产环境请在调用前 export 真实值，不要改这里的默认值。
export THGH_DB_HOST="${THGH_DB_HOST:-127.0.0.1}"
export THGH_DB_PORT="${THGH_DB_PORT:-3306}"
export THGH_DB_USER="${THGH_DB_USER:-thgh}"
export THGH_DB_PASSWORD="${THGH_DB_PASSWORD:-thgh_dev_2026}"
export THGH_DB_NAME="${THGH_DB_NAME:-thgh}"

PORT="${THGH_PORT:-9000}"

die() { echo "错误：$*" >&2; exit 1; }
need_bin() { [ -x "${BIN}/$1" ] || die "找不到 ${BIN}/$1，先执行：$0 build"; }

usage() {
    cat <<'EOF'
用法：scripts/thgh.sh <命令> [参数]

构建
  build [类型]        配置并构建服务端（类型：release[默认] / debug / asan / tsan）
  test                跑全量单测（自动带上数据库凭据，14 个 MySQL 用例才不会被跳过）
  valgrind            对全部测试二进制跑 memcheck（CI 里跑的就是这段）

运行
  start [端口]        前台启动服务端（Ctrl-C 停止）
  daemon [端口]       后台启动，日志写 /tmp/thgh_server.log
  stop                停止后台服务端
  status              查看服务端与容器状态

容器
  up                  docker compose 拉起服务端 + MySQL
  down                停止并移除容器
  logs                跟踪服务端容器日志

验证与压测
  verify [端口]       跑全部端到端验证（规划四类 + 数据四类）
  bench [端口]        网络层压测（QPS / P99）
  flame [秒] [端口]   生成 perf 火焰图到 /tmp/flame_plan.svg

示例
  scripts/thgh.sh build && scripts/thgh.sh test
  scripts/thgh.sh daemon && scripts/thgh.sh verify
  THGH_DB_PASSWORD=xxx scripts/thgh.sh start
EOF
}

cmd_build() {
    local kind="${1:-release}"
    local args=()
    case "$kind" in
        release) args=(-DCMAKE_BUILD_TYPE=RelWithDebInfo) ;;
        debug)   args=(-DCMAKE_BUILD_TYPE=Debug) ;;
        asan)    args=(-DCMAKE_BUILD_TYPE=Debug -DTHGH_SANITIZE_ADDRESS=ON); BUILD="${ROOT}/build-asan" ;;
        tsan)    args=(-DCMAKE_BUILD_TYPE=Debug -DTHGH_SANITIZE_THREAD=ON);  BUILD="${ROOT}/build-tsan" ;;
        *) die "未知构建类型：$kind（可选 release/debug/asan/tsan）" ;;
    esac
    cmake -S "$ROOT" -B "$BUILD" "${args[@]}" \
          -DTHGH_BUILD_SERVER=ON -DTHGH_BUILD_CLIENT=OFF -DTHGH_BUILD_TESTS=ON
    cmake --build "$BUILD" -j"$(nproc)"
    echo "✅ 构建完成：$BUILD"
}

cmd_test() {
    # ★ 把 THGH_TEST_DB_* 传进去，否则 14 个 MySQL 用例会被静默跳过，
    #   ctest 依然报"175 passed"——那是个很容易误导人的绿色。
    THGH_TEST_DB_HOST="$THGH_DB_HOST" \
    THGH_TEST_DB_USER="$THGH_DB_USER" \
    THGH_TEST_DB_PASSWORD="$THGH_DB_PASSWORD" \
    THGH_TEST_DB_NAME="$THGH_DB_NAME" \
        ctest --test-dir "$BUILD" --output-on-failure
}

cmd_valgrind() {
    shopt -s nullglob
    local tests=("$BIN"/test_*)
    [ "${#tests[@]}" -ge 5 ] || die "测试二进制只有 ${#tests[@]} 个，构建产物路径可能变了"
    echo "对 ${#tests[@]} 个二进制跑 memcheck"
    for t in "${tests[@]}"; do
        echo "=== $(basename "$t") ==="
        valgrind --error-exitcode=99 --leak-check=full \
                 --errors-for-leak-kinds=definite,indirect --track-origins=yes "$t"
    done
    echo "✅ 全部 ERROR SUMMARY: 0"
}

cmd_start() {
    need_bin plan_server
    exec "${BIN}/plan_server" "${1:-$PORT}" 3 4 60
}

cmd_daemon() {
    need_bin plan_server
    cmd_stop >/dev/null 2>&1 || true
    nohup "${BIN}/plan_server" "${1:-$PORT}" 3 4 60 > /tmp/thgh_server.log 2>&1 &
    sleep 2
    head -3 /tmp/thgh_server.log
    echo "✅ 已后台启动，日志：/tmp/thgh_server.log"
}

cmd_stop() {
    pkill -f "${BIN}/plan_server" && echo "已停止" || echo "没有在运行的服务端"
}

cmd_status() {
    echo "── 裸机进程 ──"
    pgrep -af "plan_server" || echo "（无）"
    echo "── 容器 ──"
    (cd "${ROOT}/deploy" && docker compose ps 2>/dev/null) || echo "（compose 未运行）"
}

cmd_up()   { (cd "${ROOT}/deploy" && [ -f .env ] || cp .env.example .env; docker compose up -d --build); }
cmd_down() { (cd "${ROOT}/deploy" && docker compose down); }
cmd_logs() { (cd "${ROOT}/deploy" && docker compose logs -f server); }

cmd_verify() {
    local p="${1:-$PORT}"
    need_bin plan_client; need_bin data_client
    local fail=0
    echo "════ 规划协议 ════"
    for m in normal abort flood garbage; do
        echo "── $m ──"
        "${BIN}/plan_client" 127.0.0.1 "$p" 120 120 "$m" 2>&1 | tail -3 || fail=1
    done
    echo "════ 数据访问 ════"
    for m in crud batch concurrent garbage; do
        echo "── $m ──"
        "${BIN}/data_client" 127.0.0.1 "$p" "$m" 2>&1 | tail -3 || fail=1
    done
    [ "$fail" = 0 ] && echo "✅ 全部端到端验证通过" || die "存在失败项"
}

cmd_bench() {
    local p="${1:-$PORT}"
    need_bin bench_client
    # ⚠️ 压测客户端必须与服务端同机。跨地域跑测出来的 P99 全是 RTT。
    "${BIN}/bench_client" 127.0.0.1 "$p" 100 10
    "${BIN}/bench_client" 127.0.0.1 "$p" 1000 10
}

cmd_flame() {
    local dur="${1:-25}" p="${2:-$PORT}" out="/tmp/flame_plan.svg"
    local fg="${FLAMEGRAPH_DIR:-/root/FlameGraph}"
    [ -x "${fg}/flamegraph.pl" ] || die "找不到 FlameGraph 工具，先执行：git clone https://github.com/brendangregg/FlameGraph.git ${fg}"
    local pid
    pid="$(pgrep -f "plan_server ${p}" | head -1)" || die "端口 ${p} 上没有运行中的 plan_server"
    echo "采样 pid=${pid} 持续 ${dur}s"

    # 采样期间必须真的在加压 —— 采空闲进程的话火焰图上全是 epoll_wait
    ( for _ in $(seq 1 400); do "${BIN}/plan_client" 127.0.0.1 "$p" 120 120 normal >/dev/null 2>&1; done ) &
    local load=$!
    sleep 1
    # -e cpu-clock：云主机不暴露 PMU，硬件事件不可用
    # --call-graph dwarf：RelWithDebInfo 下帧指针可能被优化掉
    perf record -F 199 -e cpu-clock --call-graph dwarf -p "$pid" -o /tmp/perf_flame.data -- sleep "$dur"
    kill "$load" 2>/dev/null || true; wait "$load" 2>/dev/null || true

    perf script -i /tmp/perf_flame.data > /tmp/perf_flame.txt
    "${fg}/stackcollapse-perf.pl" /tmp/perf_flame.txt > /tmp/perf_flame.folded
    "${fg}/flamegraph.pl" --title "THGH plan_server" \
        --subtitle "120 节点/120 流 连续规划，cpu-clock 软件事件采样" \
        /tmp/perf_flame.folded > "$out"
    echo "✅ 生成 $out（$(wc -c < "$out") 字节，$(wc -l < /tmp/perf_flame.folded) 个唯一栈）"
}

case "${1:-}" in
    build)    shift; cmd_build "$@" ;;
    test)     shift; cmd_test "$@" ;;
    valgrind) shift; cmd_valgrind "$@" ;;
    start)    shift; cmd_start "$@" ;;
    daemon)   shift; cmd_daemon "$@" ;;
    stop)     shift; cmd_stop "$@" ;;
    status)   shift; cmd_status "$@" ;;
    up)       shift; cmd_up "$@" ;;
    down)     shift; cmd_down "$@" ;;
    logs)     shift; cmd_logs "$@" ;;
    verify)   shift; cmd_verify "$@" ;;
    bench)    shift; cmd_bench "$@" ;;
    flame)    shift; cmd_flame "$@" ;;
    ""|-h|--help|help) usage ;;
    *) echo "未知命令：$1" >&2; echo; usage; exit 1 ;;
esac
