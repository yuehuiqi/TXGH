# ─────────────────────────────────────────────────────────────────────────────
# protocol —— 客户端与服务端共享的协议层
#
# 这一层是整个仓库用 monorepo 的根本理由：协议的消息类型、字段名、
# 状态机语义必须在两端严格一致。拆成两个仓库就只能复制两份定义，
# 迟早出现"改了一边忘了另一边"的隐蔽 bug。
#
# ── 共享什么、不共享什么（这是个刻意的切分）──────────────────────────────
#
# 共享：
#   * 分帧器（LineFramer）—— 行缓冲切分，处理 TCP 粘包/半包。纯字节操作。
#   * 消息类型与字段名常量 —— 防止两端把 "plan_result" 拼错成 "plan_results"
#     这类只能在联调时才发现的问题。
#
# 不共享：
#   * JSON 的序列化/反序列化。客户端是 Qt 程序、已经在用 QJsonDocument；
#     服务端用 nlohmann/json。如果把 JSON 库塞进共享层，
#     就等于**强迫 Qt 客户端再引入一个 JSON 库**，得不偿失。
#     两端各用各的库，但字段名一律取自本层的常量，一致性照样有保证。
#
# 于是本层**零第三方依赖**，只用标准库 —— 这也让它能被最干净地单测：
# 分帧逻辑不碰 socket、不碰 JSON，粘包/半包可以逐字节精确构造。
# ─────────────────────────────────────────────────────────────────────────────

add_library(thgh_protocol STATIC
    src/line_framer.cpp   # 行缓冲分帧：粘包 / 半包 / 长度上限
    src/message.cpp       # 消息类型与字段名常量
)

target_include_directories(thgh_protocol PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(thgh_protocol PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(thgh_protocol PRIVATE /W4)
else()
    target_compile_options(thgh_protocol PRIVATE -Wall -Wextra)
endif()

# ── 单元测试 ────────────────────────────────────────────────────────────────
if(THGH_BUILD_TESTS)
    add_subdirectory(tests)
endif()
