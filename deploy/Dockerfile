# ─────────────────────────────────────────────────────────────────────────────
# THGH 服务端镜像
#
# 多阶段构建：编译阶段装全套工具链，运行阶段只留运行时依赖。
# 单阶段的话镜像里会带着 g++、cmake、各种 -dev 包，体积翻好几倍，
# 而且把编译器留在生产镜像里本身就是个攻击面。
#
# ── 为什么在构建阶段就跑单测 ──────────────────────────────────────────────
# 测试不过就构建失败，坏的镜像**根本不会被产出**。
# 如果放到运行阶段再跑，坏镜像已经在仓库里了，只是启动时才报错。
# ─────────────────────────────────────────────────────────────────────────────

# ── 构建阶段 ─────────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

# DEBIAN_FRONTEND：apt 装某些包时会弹交互式配置界面，在容器里没人能应答，
# 会一直卡到超时。
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libmysqlclient-dev \
        nlohmann-json3-dev \
        libgtest-dev \
        libgmock-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 先只拷贝构建描述文件，让依赖解析这一层能被缓存。
# 直接 COPY . . 的话，改任何一行源码都会让上面所有层失效。
COPY CMakeLists.txt ./
COPY protocol/CMakeLists.txt        protocol/
COPY server/CMakeLists.txt          server/
COPY server/tests/CMakeLists.txt    server/tests/
COPY bench/CMakeLists.txt           bench/

# 再拷源码
COPY protocol/ protocol/
COPY server/   server/
COPY bench/    bench/

# 客户端是 Qt 程序、只在 Windows 上构建，这里显式关掉。
# 不关的话 CMake 会去找 Qt5 然后报一堆看不懂的错。
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DTHGH_BUILD_CLIENT=OFF \
        -DTHGH_BUILD_SERVER=ON \
        -DTHGH_BUILD_TESTS=ON \
 && cmake --build build -j"$(nproc)"

# ★ 测试不过就构建失败，坏镜像不会被产出。
#   需要数据库的用例会自行跳过（没设 THGH_TEST_DB_HOST），这是预期的 ——
#   数据库相关的验证由 docker-compose 起完整栈之后跑 data_client 完成。
RUN ctest --test-dir build --output-on-failure

# ── 运行阶段 ─────────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# 只装运行时需要的：MySQL 客户端库（不是 -dev）。
# ca-certificates 不是必需的，但没有它以后想加 HTTPS 相关功能会莫名其妙失败。
RUN apt-get update && apt-get install -y --no-install-recommends \
        libmysqlclient21 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 不用 root 跑服务。容器里的 root 就是宿主机的 root，
# 一旦有远程代码执行漏洞，逃逸后的权限完全不同。
RUN useradd --system --create-home --shell /usr/sbin/nologin thgh

COPY --from=builder /src/build/bin/plan_server     /usr/local/bin/
COPY --from=builder /src/build/bin/data_client     /usr/local/bin/
COPY --from=builder /src/build/bin/plan_client     /usr/local/bin/
COPY --from=builder /src/build/bin/bench_client    /usr/local/bin/
COPY --from=builder /src/build/bin/planner_bench   /usr/local/bin/
COPY --from=builder /src/server/db/schema.sql      /opt/thgh/schema.sql

USER thgh
EXPOSE 9000

# 健康检查：能建立 TCP 连接就算活着。
# 不做业务级探测是有意的 —— 探测本身要占一个连接和一次计算派发，
# 频繁探测会在过载时雪上加霜。
HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/9000' || exit 1

# 参数：端口 / IO 线程数 / 计算线程数 / 心跳超时秒
# 数据库凭据一律走环境变量（见 docker-compose.yml），不进命令行 ——
# 命令行会出现在 `ps aux` 和 `docker inspect` 里。
ENTRYPOINT ["plan_server"]
CMD ["9000", "3", "4", "60"]
