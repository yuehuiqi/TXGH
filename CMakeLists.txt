# ─────────────────────────────────────────────────────────────────────────────
# THGH —— 通信网络规划管控系统
#
# 单仓库（monorepo）承载三部分，最关键的原因是 protocol/ 需要被客户端和
# 服务端共同引用：协议的消息类型、字段名、状态机语义必须严格一致，
# 拆成两个仓库就只能复制两份定义，迟早出现"改了一边忘了另一边"的隐蔽 bug。
#
#   protocol/  —— 客户端与服务端共享的协议定义与编解码（无 Qt、无网络依赖）
#   client/    —— Qt 客户端（Windows 上开发构建）
#   server/    —— C++ 网络服务端（Linux 上开发构建，epoll 仅 Linux 可用）
#   bench/     —— 压测客户端（必须与服务端同机运行，见 README）
#
# 两端构建的是不同子集，靠下面的 option 控制：
#   Windows:  cmake -S . -B build -G "MinGW Makefiles" \
#                   -DTHGH_BUILD_CLIENT=ON -DTHGH_BUILD_SERVER=OFF \
#                   -DCMAKE_PREFIX_PATH="C:/Qt/Qt5.12.12/5.12.12/mingw73_64"
#   Linux:    cmake -S . -B build -DTHGH_BUILD_CLIENT=OFF -DTHGH_BUILD_SERVER=ON
#
# ⚠️ 路径中不能含中文：MinGW 的 make 按系统 ANSI 代码页解析 Makefile 里的
#    UTF-8 路径，中文目录会导致 "No rule to make target"。
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)

project(THGH
    VERSION 0.1.0
    DESCRIPTION "通信网络规划管控系统：Qt 客户端 + C++ 服务端 + MySQL"
    LANGUAGES CXX
)

# ── 全局设置 ────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ── 构建开关 ────────────────────────────────────────────────────────────────
# 默认值按平台推断：Windows 只有 Qt 客户端能编，Linux 侧 epoll 服务端能编。
# 显式传参可以覆盖。
if(WIN32)
    set(_client_default ON)
    set(_server_default OFF)
else()
    set(_client_default OFF)
    set(_server_default ON)
endif()

option(THGH_BUILD_CLIENT "构建 Qt 客户端" ${_client_default})
option(THGH_BUILD_SERVER "构建 C++ 网络服务端（依赖 epoll，仅 Linux）" ${_server_default})
option(THGH_BUILD_TESTS  "构建 gtest 单元测试" ON)
option(THGH_BUILD_BENCH  "构建压测客户端" ${_server_default})

option(THGH_SANITIZE_ADDRESS "用 ASan 编译（越界/泄漏，仅 Linux/GCC/Clang）" OFF)
option(THGH_SANITIZE_THREAD  "用 TSan 编译（数据竞争，仅 Linux/GCC/Clang）" OFF)

if(THGH_BUILD_SERVER AND WIN32)
    message(FATAL_ERROR
        "服务端使用 epoll，只能在 Linux 上构建。Windows 上请传 -DTHGH_BUILD_SERVER=OFF")
endif()

# ── Sanitizer ───────────────────────────────────────────────────────────────
if(THGH_SANITIZE_THREAD AND THGH_SANITIZE_ADDRESS)
    message(FATAL_ERROR "TSan 与 ASan 不能同时开启：两者都要改写内存访问指令并接管分配器")
endif()
if(THGH_SANITIZE_THREAD OR THGH_SANITIZE_ADDRESS)
    if(WIN32)
        message(FATAL_ERROR "MinGW 不提供 libasan/libtsan，Sanitizer 请在 Linux 上构建")
    endif()
    if(THGH_SANITIZE_THREAD)
        set(_san thread)
    else()
        set(_san address)
    endif()
    add_compile_options(-fsanitize=${_san} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${_san})
    message(STATUS "Sanitizer 已开启: ${_san}")

    # ── TSan 与新内核 ASLR 的兼容处理 ──────────────────────────────────────
    # Ubuntu 24.04 的 6.8 内核默认 vm.mmap_rnd_bits=32，随机化范围超出了
    # ThreadSanitizer 预期的地址空间布局，TSan 二进制一启动就：
    #     FATAL: ThreadSanitizer: unexpected memory mapping 0x...
    # 而且是在 gtest_discover_tests 跑发现阶段就挂，表现为构建失败（退出码 66），
    # 很容易误判成链接错误。
    #
    # 两种修法：
    #   a) sysctl vm.mmap_rnd_bits=28 —— 全局降低 ASLR 熵，影响整台机器的安全姿态
    #   b) setarch -R 逐进程关闭 ASLR —— 作用域仅限被测二进制
    # 这里选 b。CMAKE_CROSSCOMPILING_EMULATOR 会被 CMake 用来包装测试可执行文件
    # 的调用（gtest_discover_tests 也遵守它），正好是我们要的钩子。
    if(THGH_SANITIZE_THREAD AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(THGH_SETARCH setarch)
        if(THGH_SETARCH)
            set(CMAKE_CROSSCOMPILING_EMULATOR
                "${THGH_SETARCH};${CMAKE_SYSTEM_PROCESSOR};-R")
            message(STATUS "TSan: 经 setarch -R 关闭 ASLR 运行测试（规避内核 ASLR 冲突）")
        else()
            message(WARNING
                "TSan 已开启但未找到 setarch；若测试报 'unexpected memory mapping'，"
                "请安装 util-linux 或执行 sysctl -w vm.mmap_rnd_bits=28")
        endif()
    endif()
endif()

# ── 测试 ────────────────────────────────────────────────────────────────────
if(THGH_BUILD_TESTS)
    enable_testing()
endif()

# ── 子目录 ──────────────────────────────────────────────────────────────────
# protocol 无条件构建：它是两端共享的地基，且不依赖 Qt 或平台特性
add_subdirectory(protocol)

if(THGH_BUILD_CLIENT)
    add_subdirectory(client)
endif()

if(THGH_BUILD_SERVER)
    add_subdirectory(server)
endif()

if(THGH_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# ── 配置摘要 ────────────────────────────────────────────────────────────────
message(STATUS "──────────── THGH 构建配置 ────────────")
message(STATUS "  构建类型   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Qt 客户端  : ${THGH_BUILD_CLIENT}")
message(STATUS "  C++ 服务端 : ${THGH_BUILD_SERVER}")
message(STATUS "  压测客户端 : ${THGH_BUILD_BENCH}")
message(STATUS "  单元测试   : ${THGH_BUILD_TESTS}")
message(STATUS "───────────────────────────────────────")
